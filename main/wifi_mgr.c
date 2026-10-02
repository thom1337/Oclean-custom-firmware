#include "wifi_mgr.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "oem_hal.h"
#include "oem_api.h"
#include "oem_glue.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_eg;
#define BIT_CONNECTED BIT0

// STA never gives up: each failed attempt arms a one-shot timer (2 s, doubling to
// 30 s) that reconnects, so the event loop is never blocked. After AP_AFTER_FAILS
// failures in a row the setup SoftAP comes up alongside STA so the web UI stays
// reachable to fix the credentials; it is dropped again once STA has an IP.
#define RETRY_MIN_MS    2000
#define RETRY_MAX_MS    30000
#define AP_AFTER_FAILS  5       // about a minute of failed attempts
#define APPLY_DELAY_MS  1000    // lets the HTTP response to a settings save go out first
#define SETTLE_MS       500     // after dropping a link, before the next attempt
#define DHCP_WAIT_MS    20000   // associated but no IP within this -> drop and count a failure

static esp_timer_handle_t s_retry;
static SemaphoreHandle_t s_lock;       // guards s_pending / s_has_pending and timer (re)arming
static wifi_config_t s_pending;        // STA config handed over by wifi_mgr_apply_sta()
static bool s_has_pending;
static wifi_config_t s_sta;            // timer-task copy while it is being applied
static volatile bool s_have_creds;
static volatile int  s_fails;          // consecutive failed STA attempts
static volatile bool s_assoc;          // currently associated to an AP (CONNECTED..DISCONNECTED)
static volatile bool s_ap_up;          // written by the event-loop task once Wi-Fi is started; read by the brush logic
static volatile bool s_greeted;        // the stock "connected" effects have run for these credentials since boot

static void sta_config(const app_config_t *cfg, wifi_config_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy((char *)out->sta.ssid, cfg->wifi_ssid, sizeof(out->sta.ssid) - 1);
    strncpy((char *)out->sta.password, cfg->wifi_pass, sizeof(out->sta.password) - 1);
    out->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
}

// (Re)arm the reconnect timer; ms == 0 cancels it. Credentials waiting to be
// applied always win, so no caller can cancel or delay them.
static void arm_retry(uint32_t ms)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_has_pending) ms = APPLY_DELAY_MS;
    esp_timer_stop(s_retry);
    if (ms) esp_timer_start_once(s_retry, (uint64_t)ms * 1000);
    xSemaphoreGive(s_lock);
}

static uint32_t backoff_ms(int fails)
{
    uint32_t ms = RETRY_MIN_MS;
    while (--fails > 0 && ms < RETRY_MAX_MS) ms *= 2;
    return ms < RETRY_MAX_MS ? ms : RETRY_MAX_MS;
}

// Every STA connect goes through this callback (esp_timer task), so applying new
// credentials can never race a reconnect.
static void retry_cb(void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool apply = s_has_pending;
    if (apply) { s_sta = s_pending; s_has_pending = false; }
    xSemaphoreGive(s_lock);

    if (apply) {
        s_fails = 0;
        s_greeted = false;       // the first connection to the new network shows on the brush again
        esp_wifi_disconnect();   // set_config is refused while an attempt is in progress
        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_sta);
        if (err == ESP_OK) ESP_LOGI(TAG, "applied new STA creds for '%.32s'", (char *)s_sta.sta.ssid);
        else ESP_LOGE(TAG, "applying STA creds failed: %s", esp_err_to_name(err));
        // If a link was up, dropping it is reported as STA_DISCONNECTED, which
        // re-arms the timer; if not, this does.
        arm_retry(SETTLE_MS);
        return;
    }
    if (wifi_mgr_is_connected()) return;
    if (s_assoc) {
        // Associated but DHCP never produced an IP within the deadline: drop the
        // link so the STA_DISCONNECTED that follows counts a failure, backs off,
        // and brings up the setup AP like any other failure.
        ESP_LOGW(TAG, "associated but no IP; dropping link");
        esp_wifi_disconnect();
        return;
    }
    // A failed attempt is reported as STA_DISCONNECTED, which re-arms the timer.
    // If the call itself is refused nothing would, so re-arm here.
    if (esp_wifi_connect() != ESP_OK) arm_retry(RETRY_MAX_MS);
}

static void start_softap(void)
{
    static wifi_config_t ap = { .ap = {
        .ssid = "oclean-setup", .ssid_len = sizeof("oclean-setup") - 1,
        .channel = 1,
        .authmode = WIFI_AUTH_OPEN,   // open captive-setup AP
        .max_connection = 4,
    } };
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK || esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed");
        return;
    }
    s_ap_up = true;
    ESP_LOGI(TAG, "SoftAP 'oclean-setup' up (open) — connect and browse http://192.168.4.1");
}

static void stop_softap(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    s_ap_up = false;
    ESP_LOGI(TAG, "SoftAP 'oclean-setup' down");
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_have_creds) arm_retry(1);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        s_assoc = true;
        arm_retry(DHCP_WAIT_MS);   // associated: give DHCP a bounded window, then drop and retry
        // Stock's wifi_event_handler (0x4200bbac): Wi-Fi light on, screen awake, idle 60 s.
        // Stock gets here once per wake: it stops Wi-Fi at the first disconnect and
        // starts it again only at the next wake. This driver reconnects for ever, so
        // only the first connection since boot (every wake from deep sleep is a boot)
        // or since new credentials counts. A link that comes back later only updates
        // the status: it must not restart the idle timer or wake the screen.
        hal_lock();
        g_oem.wifi_status = 1;
        bool first = !s_greeted;
        s_greeted = true;
        if (first && brush_app_running()) {   // not in safe mode: no LED driver, no main loop
            if (!g_oem.session_active) oem_idle_timeout(60);
            // The wake script fades the backlight in. On the dock nothing switches it
            // off again once the 30 s after docking have passed, and that first
            // connection can come much later here (a network that was down, new
            // credentials). So only off the charger, where stock's own wake function
            // plays this script too (0x4201bd70) and the idle timer ends it.
            if (g_oem.batt_pct && g_oem.power_state == OEM_PWR_BATTERY) oem_led_set(1, 0, 0);
        }
        hal_unlock();
        if (first) hal_event_post(OEM_EV_BLE_WAKE);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_assoc = false;
        xEventGroupClearBits(s_eg, BIT_CONNECTED);
        hal_lock(); g_oem.wifi_status = 2; hal_unlock();
        if (!s_have_creds) return;
        int fails = ++s_fails;
        if (fails >= AP_AFTER_FAILS && !s_ap_up) {
            ESP_LOGW(TAG, "%d failed STA attempts in a row; bringing up the setup AP", fails);
            start_softap();
            // This takes about 50 s, by which time the screen is usually off and the
            // 30 s deep-sleep window of a brush with a network is running. With the
            // setup AP up it counts as one without (hal_wifi_has_ssid): start that
            // longer window now, or the AP would be gone again in 10..20 s.
            if (s_ap_up && brush_app_running()) oem_net_activity();
        }
        arm_retry(backoff_ms(fails));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        // Somebody joined the setup AP: the deep-sleep window starts over for them.
        if (brush_app_running()) oem_net_activity();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        // DHCP binds from the tcpip thread, so a GOT_IP from a just-dropped
        // association can land right after STA_DISCONNECTED. Ignoring it when we
        // are not associated avoids being marked "connected" with no link, no
        // retry timer and the setup AP dropped — idle until reboot.
        if (!s_assoc) return;
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_fails = 0;
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
        arm_retry(0);
        if (s_ap_up) stop_softap();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_eg, BIT_CONNECTED);
        if (s_assoc) arm_retry(DHCP_WAIT_MS);   // still associated: re-acquire window, else drop
    }
}

void wifi_mgr_start(const app_config_t *cfg)
{
    s_eg = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();   // created once up front; idle until the setup AP is enabled
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    const esp_timer_create_args_t ta = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_retry));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));

    s_have_creds = cfg->wifi_ssid[0] != '\0';
    // A brush with a Wi-Fi network is "bound" in stock terms (sys_config byte 0x0c
    // == 2): it drives the Wi-Fi light and hides the unbound icon on the mode pages.
    hal_lock(); g_oem.sys[0x0c] = s_have_creds ? 2 : 0; hal_unlock();
    if (s_have_creds) {
        wifi_config_t sta; sta_config(cfg, &sta);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
        ESP_LOGI(TAG, "connecting to '%s'", cfg->wifi_ssid);
    } else {
        start_softap();
    }
    ESP_ERROR_CHECK(esp_wifi_start());
}

bool wifi_mgr_is_connected(void)
{
    return s_eg && (xEventGroupGetBits(s_eg) & BIT_CONNECTED);
}

bool wifi_mgr_has_creds(void)   { return s_have_creds; }
bool wifi_mgr_setup_ap_up(void) { return s_ap_up; }

void wifi_mgr_apply_sta(const app_config_t *cfg)
{
    if (cfg->wifi_ssid[0] == '\0') return;
    // The mode is left alone: if the setup AP is up it stays up until STA has an
    // IP, so a mistyped password can still be corrected.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    sta_config(cfg, &s_pending);
    s_has_pending = true;
    xSemaphoreGive(s_lock);
    s_have_creds = true;
    // "Bound" from now on, as wifi_mgr_start() sets it at boot: the Wi-Fi light and
    // the mode pages follow without a restart (and hal_wifi_has_ssid() through
    // wifi_mgr_has_creds()).
    hal_lock(); g_oem.sys[0x0c] = 2; hal_unlock();
    arm_retry(APPLY_DELAY_MS);
    ESP_LOGI(TAG, "new STA creds for '%s' queued", cfg->wifi_ssid);
}

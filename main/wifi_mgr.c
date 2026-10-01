#include "wifi_mgr.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_eg;
#define BIT_CONNECTED BIT0
static int s_retries;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_eg, BIT_CONNECTED);
        if (s_retries++ < 20) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "giving up STA reconnect after %d tries", s_retries);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_eg, BIT_CONNECTED);
    }
}

static void start_softap(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, "oclean-setup", sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen("oclean-setup");
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_OPEN;   // open captive-setup AP
    ap.ap.channel = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGI(TAG, "SoftAP 'oclean-setup' up (open) — connect and browse http://192.168.4.1");
}

void wifi_mgr_start(const app_config_t *cfg)
{
    s_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));

    bool have_creds = cfg->wifi_ssid[0] != '\0';
    if (have_creds) {
        wifi_config_t sta = {0};
        strncpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid) - 1);
        strncpy((char *)sta.sta.password, cfg->wifi_pass, sizeof(sta.sta.password) - 1);
        sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
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

void wifi_mgr_apply_sta(const app_config_t *cfg)
{
    if (cfg->wifi_ssid[0] == '\0') return;
    wifi_config_t sta = {0};
    strncpy((char *)sta.sta.ssid, cfg->wifi_ssid, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, cfg->wifi_pass, sizeof(sta.sta.password) - 1);
    sta.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    s_retries = 0;
    esp_wifi_disconnect();
    esp_wifi_connect();
    ESP_LOGI(TAG, "applied new STA creds for '%s'", cfg->wifi_ssid);
}

#include "web_server.h"
#include "web_auth.h"
#include "config_store.h"
#include "metrics.h"
#include "mqtt_ha.h"
#include "wifi_mgr.h"
#include "ble_server.h"
#include "weblog.h"
#include "ui_res.h"
#include "boot_guard.h"
#include "oem_hal.h"
#include "oem_api.h"
#include "oem_glue.h"
#include "hw_power.h"
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_pm.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "web";

// ---- embedded static assets ----
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t app_js_start[]     asm("_binary_app_js_start");
extern const uint8_t app_js_end[]       asm("_binary_app_js_end");
extern const uint8_t style_css_start[]  asm("_binary_style_css_start");
extern const uint8_t style_css_end[]    asm("_binary_style_css_end");

static esp_err_t send_embedded(httpd_req_t *r, const uint8_t *s, const uint8_t *e, const char *ct)
{
    httpd_resp_set_type(r, ct);
    return httpd_resp_send(r, (const char *)s, e - s);
}

// A client is using the web UI: keep the brush up. Not in safe mode, where the brush
// logic does not run (brush_app_running() is false) and nobody would take the command.
static void net_activity(void)
{
    if (brush_app_running()) oem_net_activity();
}

// Opening the page is something a person does, so it counts as activity (on the
// setup AP it is all there is before the settings are saved), as do opening the Files
// tab and the copy it then makes. The requests an open page keeps sending by itself
// (/api/status, /api/log) must not: a forgotten browser tab would keep the brush awake
// until the battery is empty. The page load counts through the one GET /api/config the
// page sends when it opens (h_config_get), not through GET / itself: that needs no
// password (the login view is on it), so anyone on the network, or a page elsewhere
// loading it, could otherwise keep a password-protected brush awake.
static esp_err_t h_index(httpd_req_t *r){ return send_embedded(r, index_html_start, index_html_end, "text/html"); }
static esp_err_t h_js(httpd_req_t *r){ return send_embedded(r, app_js_start, app_js_end, "application/javascript"); }
static esp_err_t h_css(httpd_req_t *r){ return send_embedded(r, style_css_start, style_css_end, "text/css"); }

// ---- helpers ----
static void send_json(httpd_req_t *r, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, s);
    free(s);
    cJSON_Delete(o);
}

// A request header as a malloc'd string; NULL if it is absent (or empty), or no memory.
static char *hdr(httpd_req_t *r, const char *name)
{
    size_t n = httpd_req_get_hdr_value_len(r, name);
    if (!n) return NULL;
    char *s = malloc(n + 1);
    if (s && httpd_req_get_hdr_value_str(r, name, s, n + 1) != ESP_OK) { free(s); s = NULL; }
    return s;
}

// ---- web password ----
// Optional: none is set until the owner sets one in Settings, and until then the web UI
// is as open as before. Once set, every /api/* request needs the session cookie or the
// password; the page and its two assets stay open so the login view can load. The
// password comes as HTTP Basic (the login form, curl -u, dump_res.py) and is checked
// against a salted PBKDF2-HMAC-SHA256 hash. A match hands out the session cookie. Its
// token is random and kept in NVS next to the hash, so a login survives deep sleep
// (every wake is a boot), reboots and OTA; a new password makes a new token, which
// logs out every other browser. The 8 s button hold clears the password
// (web_auth_forget). It is all plain HTTP: whoever can sniff the network sees the
// password at login and the token on every request.
//
// Two rules hold whether or not a password is set (guard()): the Host header must be
// an IP address, which stops DNS rebinding, and a POST must be JSON or an octet-stream,
// which stops cross-site requests (CSRF). The header parsing is in web_auth.c.
#define AUTH_KEY  "web_auth"    // NVS blob in namespace "oclean"
#define AUTH_ITER 10000         // PBKDF2 iterations: an estimated 0.4..1 s here
typedef struct {
    uint32_t iter;              // 0: no password
    uint8_t  salt[16];
    uint8_t  hash[32];
    uint8_t  token[WEB_TOKEN_LEN];
} web_auth_t;
static web_auth_t s_auth;
static portMUX_TYPE s_auth_mux = portMUX_INITIALIZER_UNLOCKED;   // web_auth_forget() runs in other tasks
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_pm_cpu;   // full clock while hashing: the APB lock alone leaves the CPU at 80 MHz
#endif

static void auth_get(web_auth_t *a)       { taskENTER_CRITICAL(&s_auth_mux); *a = s_auth; taskEXIT_CRITICAL(&s_auth_mux); }
static void auth_put(const web_auth_t *a) { taskENTER_CRITICAL(&s_auth_mux); s_auth = *a; taskEXIT_CRITICAL(&s_auth_mux); }
static bool pass_set(void)                { web_auth_t a; auth_get(&a); return a.iter != 0; }

// A record that is missing or unreadable means no password.
static bool s_auth_loaded;
static void auth_load(void)
{
    s_auth_loaded = true;
    web_auth_t a = {0};
    size_t n = sizeof a;
    nvs_handle_t h;
    if (nvs_open("oclean", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, AUTH_KEY, &a, &n) != ESP_OK || n != sizeof a) memset(&a, 0, sizeof a);
        nvs_close(h);
    }
    auth_put(&a);
    ESP_LOGI(TAG, "web password %s", a.iter ? "set" : "not set");
}

// The hash runs in the server task, which then answers nothing else. Its priority
// drops below the brush tasks' (2 and 3) meanwhile, so the motor and the screen keep
// running whichever core it is on.
static bool pw_hash(const char *pass, const web_auth_t *a, uint8_t out[32])
{
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, 1);
#if CONFIG_PM_ENABLE
    if (s_pm_cpu) esp_pm_lock_acquire(s_pm_cpu);
#endif
    int64_t t0 = esp_timer_get_time();
    int e = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pass, strlen(pass),
                                          a->salt, sizeof a->salt, a->iter, 32, out);
    int64_t t1 = esp_timer_get_time();
#if CONFIG_PM_ENABLE
    if (s_pm_cpu) esp_pm_lock_release(s_pm_cpu);
#endif
    vTaskPrioritySet(NULL, prio);
    ESP_LOGI(TAG, "password hash (%lu iterations): %lu ms", (unsigned long)a->iter, (unsigned long)((t1 - t0) / 1000));
    return e == 0;
}

// New salt, hash and token. Wi-Fi is up whenever this server runs, so esp_fill_random()
// is a true random source.
static bool auth_set(const char *pass)
{
    web_auth_t a = { .iter = AUTH_ITER };
    esp_fill_random(a.salt, sizeof a.salt);
    esp_fill_random(a.token, sizeof a.token);
    bool ok = pw_hash(pass, &a, a.hash);
    nvs_handle_t h;
    if (ok && (ok = nvs_open("oclean", NVS_READWRITE, &h) == ESP_OK)) {
        ok = nvs_set_blob(h, AUTH_KEY, &a, sizeof a) == ESP_OK && nvs_commit(h) == ESP_OK;
        nvs_close(h);
    }
    if (ok) auth_put(&a);
    ESP_LOGW(TAG, "web password %s", ok ? "set (other browsers are logged out)" : "NOT saved");
    return ok;
}

void web_auth_forget(void)
{
    nvs_handle_t h;
    if (nvs_open("oclean", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, AUTH_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    const web_auth_t none = {0};
    auth_put(&none);
    ESP_LOGW(TAG, "web password cleared (button held 8 s)");
}

bool web_auth_is_set(void)
{
    if (!s_auth_loaded) auth_load();
    return pass_set();
}

// The cookie is not the token itself but HMAC-SHA256(token, the brush's own address on
// this connection), cut to the token's length. So a login at the setup AP's
// 192.168.4.1, an address many other gadgets' setup APs use too (and the browser hands
// them the cookie), is no key to the brush's address on the home network, nor the other
// way round. The address comes from the socket, not from the Host header, which the
// client chooses. (The server socket is IPv6 with IPv4-mapped addresses.)
static bool session_of(httpd_req_t *r, const uint8_t token[WEB_TOKEN_LEN], uint8_t out[WEB_TOKEN_LEN])
{
    struct sockaddr_storage sa;
    socklen_t n = sizeof sa;
    int fd = httpd_req_to_sockfd(r);
    if (fd < 0 || getsockname(fd, (struct sockaddr *)&sa, &n) != 0) return false;
    const void *ip = &((struct sockaddr_in *)&sa)->sin_addr;
    size_t len = 4;
    if (sa.ss_family == AF_INET6) { ip = &((struct sockaddr_in6 *)&sa)->sin6_addr; len = 16; }
    uint8_t mac[32];
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), token, WEB_TOKEN_LEN, ip, len, mac) != 0) return false;
    memcpy(out, mac, WEB_TOKEN_LEN);
    return true;
}

// httpd keeps the pointer until the reply is sent; one server task, so one buffer.
static void set_cookie(httpd_req_t *r, const uint8_t token[WEB_TOKEN_LEN])
{
    static char s[112];
    uint8_t v[WEB_TOKEN_LEN];
    if (!session_of(r, token, v)) { ESP_LOGE(TAG, "no session cookie: local address unknown"); return; }
    int n = snprintf(s, sizeof s, "oclean=");
    for (int i = 0; i < WEB_TOKEN_LEN; i++) n += snprintf(s + n, sizeof s - n, "%02x", v[i]);
    // No Secure attribute: a browser drops a Secure cookie that comes over plain HTTP.
    snprintf(s + n, sizeof s - n, "; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000");
    httpd_resp_set_hdr(r, "Set-Cookie", s);
}

static bool cookie_ok(httpd_req_t *r, const web_auth_t *a)
{
    uint8_t got[WEB_TOKEN_LEN], want[WEB_TOKEN_LEN];
    char *c = hdr(r, "Cookie");
    bool ok = c && web_cookie_token(c, got) && session_of(r, a->token, want) &&
              mbedtls_ct_memcmp(got, want, sizeof got) == 0;
    free(c);
    return ok;
}

// The cookie, or else the password (which then also sets the cookie).
static bool auth_ok(httpd_req_t *r)
{
    web_auth_t a;
    auth_get(&a);
    if (!a.iter) return true;                // no password set
    if (cookie_ok(r, &a)) return true;
    uint8_t h[32];
    char pass[WEB_PASS_MAX + 1];
    // A password the settings would refuse cannot be the stored one: not worth a hash.
    char *z = hdr(r, "Authorization");
    bool ok = z && web_basic_pass(z, pass) && web_pass_valid(pass) && pw_hash(pass, &a, h) &&
              mbedtls_ct_memcmp(h, a.hash, sizeof h) == 0;
    free(z);
    memset(pass, 0, sizeof pass);
    if (ok) {
        set_cookie(r, a.token);
        net_activity();                      // logging in is something a person does
        ESP_LOGI(TAG, "logged in");
    }
    return ok;
}

// ---- every route goes through guard() (see reg()) ----
typedef struct {
    esp_err_t (*fn)(httpd_req_t *);
    bool open;                               // reachable without the password
} route_t;

static bool host_ok(httpd_req_t *r) { char *s = hdr(r, "Host"); bool ok = web_host_ok(s); free(s); return ok; }
static bool ct_ok(httpd_req_t *r)   { char *s = hdr(r, "Content-Type"); bool ok = web_ct_ok(s); free(s); return ok; }

// A refused request never reaches its handler, so it never counts as activity.
static esp_err_t guard(httpd_req_t *r)
{
    const route_t *rt = r->user_ctx;
    if (!host_ok(r)) {
        httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "open the brush by its IP address");
        return ESP_OK;
    }
    if (r->method == HTTP_POST && !ct_ok(r)) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "send application/json (a firmware image as application/octet-stream)");
        return ESP_OK;
    }
    if (!rt->open && !auth_ok(r)) {
        // No WWW-Authenticate: the browser shows no password dialog of its own, and
        // app.js shows its login view.
        httpd_resp_send_err(r, HTTPD_401_UNAUTHORIZED, "password required");
        return ESP_OK;
    }
    return rt->fn(r);
}

// ---- /api/status : metrics + runtime state ----
static esp_err_t h_status(httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "metrics", metrics_build_state_json());
    cJSON_AddBoolToObject(o, "mqtt_connected", mqtt_ha_is_connected());
    cJSON_AddBoolToObject(o, "wifi_connected", wifi_mgr_is_connected());
    cJSON_AddBoolToObject(o, "ble_connected", ble_server_connected());
    cJSON_AddStringToObject(o, "project", esp_app_get_description()->project_name);
    cJSON_AddBoolToObject(o, "safe_mode", boot_guard_mode() == BOOT_SAFE);
    cJSON_AddBoolToObject(o, "oem_pictures", ui_res_available());
    // Input diagnostics (no serial port: this is how the first boot gets debugged).
    cJSON *d = cJSON_AddObjectToObject(o, "diag");
    hal_lock();
    cJSON_AddNumberToObject(d, "touch_state", oem_touch_state());
    cJSON_AddNumberToObject(d, "touch_x", g_oem.touch_x);
    cJSON_AddNumberToObject(d, "touch_y", g_oem.touch_y);
    cJSON_AddNumberToObject(d, "force_raw", g_oem.force_raw);
    cJSON_AddNumberToObject(d, "force_base", g_oem.force_base);
    cJSON_AddNumberToObject(d, "force_coef", g_oem.force_coef);
    cJSON_AddBoolToObject(d, "force_available", oem_pressure_available());
    cJSON_AddNumberToObject(d, "motor_state", g_oem.motor_state);
    cJSON_AddNumberToObject(d, "gear", g_oem.gear);
    cJSON_AddBoolToObject(d, "ota", g_oem.ota);
    cJSON_AddBoolToObject(d, "batt_fault", g_oem.batt_fault);
    // Charge diagnostics: "charging" in the metrics only says "on the dock".
    cJSON_AddNumberToObject(d, "batt_raw_mv", g_oem.batt_raw_mv);
    // Only known while the brush logic runs. In safe mode the charger pins are never
    // set up: GPIO9 is not read, and GPIO26 keeps whatever the crashed boot left
    // latched (the pad hold survives a panic reset). null = unknown.
    if (brush_app_running()) {
        cJSON_AddBoolToObject(d, "charge_blocked", oem_charge_blocked());
        cJSON_AddBoolToObject(d, "charger_present", oem_charger_present());
    } else {
        cJSON_AddNullToObject(d, "charge_blocked");
        cJSON_AddNullToObject(d, "charger_present");
    }
    cJSON_AddBoolToObject(d, "thermal_cut", oem_charge_thermal_cut());
    cJSON_AddNumberToObject(d, "alive_edges", oem_charger_alive_count());
    cJSON_AddBoolToObject(d, "pm_apb_lock", hw_power_apb_held());
    cJSON_AddNumberToObject(d, "pm_cpu_locks", hw_power_cpu_locks());
    hal_unlock();
    send_json(r, o);
    return ESP_OK;
}

// ---- /api/brush POST : {"brushing":bool, "mode":0..5, "strength":1..5} — acts like the user on the brush ----
static esp_err_t h_brush(httpd_req_t *r)
{
    char buf[128];
    int n = r->content_len;
    if (n <= 0 || n >= (int)sizeof(buf)) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad len"); return ESP_OK; }
    int got = 0;
    while (got < n) { int k = httpd_req_recv(r, buf + got, n - got); if (k <= 0) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "recv"); return ESP_OK; } got += k; }
    buf[n] = 0;
    cJSON *in = cJSON_Parse(buf);
    if (!in) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }
    if (!brush_app_running()) {
        cJSON_Delete(in);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "safe mode: the brush logic is not running");
        return ESP_OK;
    }
    oem_net_activity();
    cJSON *v;
    if ((v = cJSON_GetObjectItem(in, "mode")) && cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 5)
        oem_remote_mode((uint8_t)v->valuedouble);
    if ((v = cJSON_GetObjectItem(in, "strength")) && cJSON_IsNumber(v) && v->valuedouble >= 1 && v->valuedouble <= 5)
        oem_remote_strength((uint8_t)v->valuedouble);
    if ((v = cJSON_GetObjectItem(in, "brushing")) && cJSON_IsBool(v))
        oem_remote_brushing(cJSON_IsTrue(v));
    cJSON_Delete(in);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

// ---- flash regions: the Files tab's read-only copies (also re/tools/uisim/dump_res.py) ----
// The partitions of the running table, plus the bootloader and the partition table in
// front of them, which are not partitions and are read by address.
static const struct { const char *label; uint32_t addr, size; } EXTRA[] = {
    { "bootloader",      CONFIG_BOOTLOADER_OFFSET_IN_FLASH, CONFIG_PARTITION_TABLE_OFFSET - CONFIG_BOOTLOADER_OFFSET_IN_FLASH },
    { "partition_table", CONFIG_PARTITION_TABLE_OFFSET,     0x1000 },
};

// A region by name: one of EXTRA, a partition label, or (no name) the OEM picture partition.
static bool region_find(const char *label, uint32_t *addr, uint32_t *size)
{
    for (int i = 0; label && i < 2; i++)
        if (strcmp(label, EXTRA[i].label) == 0) { *addr = EXTRA[i].addr; *size = EXTRA[i].size; return true; }
    const esp_partition_t *p = label
        ? esp_partition_find_first(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, label)
        : esp_partition_find_first((esp_partition_type_t)0x40, (esp_partition_subtype_t)0x00, NULL);
    if (!p) return false;
    *addr = p->address; *size = p->size;
    return true;
}

// Whether [addr, addr + n) overlaps an NVS partition. NVS holds the Wi-Fi and MQTT
// passwords in clear (ours, and the Wi-Fi driver's own copy), which /api/config never
// hands out, and the web password's hash and session token. Withheld while no web
// password is set; once one is, every request here needs it, so only the owner can copy
// it (a backup of the settings and the factory and radio calibration).
static bool touches_secrets(uint32_t addr, uint32_t n)
{
    bool hit = false;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if ((p->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS || p->subtype == ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS) &&
            addr < p->address + p->size && p->address < addr + n) { hit = true; break; }
    }
    esp_partition_iterator_release(it);
    return hit;
}

// A copy someone started counts as activity, so the brush on battery does not
// deep-sleep half-way through it (on the dock it never sleeps). At most every 5 s;
// it lapses with the last chunk.
static void copy_activity(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (last && now - last < 5000000) return;
    last = now;
    net_activity();
}

// httpd_send() may take part of the buffer; <= 0 is a closed socket or 5 s without
// progress, and the client retries the chunk.
static esp_err_t send_all(httpd_req_t *r, const char *b, size_t n)
{
    while (n) {
        int k = httpd_send(r, b, n);
        if (k <= 0) return ESP_FAIL;
        b += k; n -= k;
    }
    return ESP_OK;
}

// "key" from the query as a number; false if present but not one. A missing key leaves *v.
static bool query_u32(const char *q, const char *key, uint32_t *v)
{
    char s[16], *end;
    esp_err_t e = httpd_query_key_value(q, key, s, sizeof s);
    if (e == ESP_ERR_NOT_FOUND) return true;
    if (e != ESP_OK || !s[0]) return false;
    *v = strtoul(s, &end, 0);
    return *end == '\0';
}

// ---- /api/parts GET : the regions, for the Files tab ----
static cJSON *add_region(cJSON *a, const char *label, uint32_t addr, uint32_t size, int type, int sub)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "label", label);
    cJSON_AddNumberToObject(e, "type", type);
    cJSON_AddNumberToObject(e, "sub", sub);
    cJSON_AddNumberToObject(e, "addr", addr);
    cJSON_AddNumberToObject(e, "size", size);
    cJSON_AddBoolToObject(e, "readable", !touches_secrets(addr, size) || pass_set());
    cJSON_AddItemToArray(a, e);
    return e;
}

static esp_err_t h_parts(httpd_req_t *r)
{
    net_activity();   // opening the tab is something a person does; nothing polls this
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);   // what /api/ota writes
    uint32_t flash = 0;
    esp_flash_get_size(NULL, &flash);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "flash_size", flash);
    cJSON_AddBoolToObject(o, "encrypted", esp_flash_encryption_enabled());
    cJSON *a = cJSON_AddArrayToObject(o, "regions");
    for (int i = 0; i < 2; i++) add_region(a, EXTRA[i].label, EXTRA[i].addr, EXTRA[i].size, -1, -1);
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        cJSON *e = add_region(a, p->label, p->address, p->size, p->type, p->subtype);
        if (p->type != ESP_PARTITION_TYPE_APP) continue;
        cJSON_AddBoolToObject(e, "running", run && run->address == p->address);
        cJSON_AddBoolToObject(e, "next", next && next->address == p->address);
        esp_app_desc_t d;
        uint32_t head = 0;
        if (esp_ota_get_partition_description(p, &d) == ESP_OK) {
            char s[33];
            snprintf(s, sizeof s, "%.32s", d.project_name); cJSON_AddStringToObject(e, "project", s);
            snprintf(s, sizeof s, "%.32s", d.version);      cJSON_AddStringToObject(e, "version", s);
        } else {
            cJSON_AddBoolToObject(e, "blank", esp_partition_read(p, 0, &head, 4) == ESP_OK && head == 0xFFFFFFFF);
        }
    }
    send_json(r, o);
    return ESP_OK;
}

// ---- /api/res GET ?part=&off=&len= : raw read of one region, at most 64 KB a request ----
// Without part= it reads the OEM picture partition, as before. Small requests keep the
// one server task free for /api/status and /api/log in between.
#define RES_CHUNK 65536
static esp_err_t h_res(httpd_req_t *r)
{
    char q[96] = "", v[24];
    size_t qlen = httpd_req_get_url_query_len(r);
    if (qlen >= sizeof q) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "query too long"); return ESP_OK; }
    if (qlen) httpd_req_get_url_query_str(r, q, sizeof q);
    esp_err_t e = httpd_query_key_value(q, "part", v, sizeof v);
    uint32_t base, total, off = 0, len = RES_CHUNK;
    if (e == ESP_ERR_HTTPD_RESULT_TRUNC || !region_find(e == ESP_OK ? v : NULL, &base, &total)) {
        httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, e == ESP_ERR_NOT_FOUND ? "no OEM picture partition" : "no such partition");
        return ESP_OK;
    }
    if (!query_u32(q, "off", &off) || !query_u32(q, "len", &len)) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "off / len: not a number"); return ESP_OK; }
    if (off >= total) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "offset past the end"); return ESP_OK; }
    if (touches_secrets(base, total) && !pass_set()) {
        httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "withheld until a web UI password is set: holds the Wi-Fi and MQTT passwords");
        return ESP_OK;
    }
    if (len > RES_CHUNK) len = RES_CHUNK;
    if (len > total - off) len = total - off;
    char *buf = malloc(4096);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    uint32_t k = len < 4096 ? len : 4096;
    if (k && esp_flash_read(NULL, buf, base + off, k) != ESP_OK) {
        free(buf); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "flash read failed"); return ESP_OK;
    }
    httpd_resp_set_type(r, "application/octet-stream");
    char hdr[12]; snprintf(hdr, sizeof hdr, "%lu", (unsigned long)total);
    httpd_resp_set_hdr(r, "X-Res-Size", hdr);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    // The headers with the real Content-Length and no body (IDF sends one only if buf is
    // given), then the body raw: a read cut short fails in the client instead of passing
    // for a complete one, as the chunked encoding did.
    esp_err_t err = httpd_resp_send(r, NULL, len);
    for (uint32_t done = 0; err == ESP_OK && done < len; done += k) {
        k = len - done < 4096 ? len - done : 4096;
        if (done && esp_flash_read(NULL, buf, base + off + done, k) != ESP_OK) err = ESP_FAIL;
        else err = send_all(r, buf, k);
    }
    free(buf);
    if (err == ESP_OK) copy_activity();
    return err;   // ESP_FAIL closes the connection
}

// ---- /api/config GET : current config (password masked) ----
// The page sends this once when it opens (never as a poll), after guard() has let it
// through: that is where a page load counts as activity (see h_index).
static esp_err_t h_config_get(httpd_req_t *r)
{
    net_activity();
    app_config_t c; config_load(&c);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "wifi_ssid", c.wifi_ssid);
    cJSON_AddBoolToObject(o, "mqtt_enabled", c.mqtt_enabled);
    cJSON_AddStringToObject(o, "mqtt_host", c.mqtt_host);
    cJSON_AddNumberToObject(o, "mqtt_port", c.mqtt_port);
    cJSON_AddBoolToObject(o, "mqtt_tls", c.mqtt_tls);
    cJSON_AddStringToObject(o, "mqtt_user", c.mqtt_user);
    cJSON_AddBoolToObject(o, "mqtt_pass_set", c.mqtt_pass[0] != '\0');
    cJSON_AddStringToObject(o, "mqtt_base_topic", c.mqtt_base_topic);
    cJSON_AddStringToObject(o, "mqtt_discovery_prefix", c.mqtt_discovery_prefix);
    cJSON_AddStringToObject(o, "device_name", c.device_name);
    cJSON_AddNumberToObject(o, "publish_interval_s", c.publish_interval_s);
    cJSON_AddStringToObject(o, "tz", c.tz);
    uint8_t panel = 0; nvs_handle_t h;
    if (nvs_open("oclean", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "lcd_panel", &panel); nvs_close(h); }
    cJSON_AddNumberToObject(o, "lcd_panel", panel);   // 0 auto (stock panel id), 1..5 force a table (hw_display.c)
    cJSON_AddBoolToObject(o, "web_pass_set", pass_set());
    cJSON_AddBoolToObject(o, "safe_mode", !brush_app_running());   // takes no first password (h_config_post)
    send_json(r, o);
    return ESP_OK;
}

static void cpy_str(cJSON *o, const char *k, char *dst, size_t n)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v) && v->valuestring) strlcpy(dst, v->valuestring, n);
}
// Passwords: only overwrite when a non-empty one was provided (blank = unchanged).
static void cpy_pass(cJSON *o, const char *k, char *dst, size_t n)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) strlcpy(dst, v->valuestring, n);
}

// ---- /api/config POST : update MQTT + Wi-Fi settings ----
static esp_err_t h_config_post(httpd_req_t *r)
{
    int total = r->content_len;
    if (total <= 0 || total > 4096) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad len"); return ESP_OK; }
    char *buf = malloc(total + 1);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    int got = 0;
    while (got < total) {
        int k = httpd_req_recv(r, buf + got, total - got);
        if (k <= 0) { free(buf); httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "recv"); return ESP_OK; }
        got += k;
    }
    buf[total] = 0;
    cJSON *in = cJSON_Parse(buf);
    free(buf);
    if (!in) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "json"); return ESP_OK; }

    // The web password: blank means unchanged, as for the other two. Checked before
    // anything is applied, so a refused one changes nothing.
    char web_pass[WEB_PASS_MAX + 1] = "";
    cJSON *wp = cJSON_GetObjectItem(in, "web_pass");
    if (cJSON_IsString(wp) && wp->valuestring && wp->valuestring[0]) {
        if (!web_pass_valid(wp->valuestring)) {
            cJSON_Delete(in);
            httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "web password: 12 to 64 printable ASCII characters");
            return ESP_OK;
        }
        strlcpy(web_pass, wp->valuestring, sizeof web_pass);
    }
    // A first web password is taken in normal mode only. There a brush without one is on
    // no network (wifi_mgr_start()), so the first one comes in over the setup AP, whose
    // passcode only the screen shows: whoever sets it holds the brush. Safe mode joins its
    // network anyway (it is the repair path), so there anyone on that network could.
    bool first = !pass_set();
    if (web_pass[0] && first && !brush_app_running()) {
        cJSON_Delete(in);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "safe mode takes no first web UI password: set it over the setup AP once the brush runs normally");
        return ESP_OK;
    }

    app_config_t c; config_load(&c);
    char old_ssid[33]; strlcpy(old_ssid, c.wifi_ssid, sizeof(old_ssid));
    char old_pass[65]; strlcpy(old_pass, c.wifi_pass, sizeof(old_pass));
    char old_tz[sizeof(c.tz)]; strlcpy(old_tz, c.tz, sizeof(old_tz));

    cpy_str(in, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
    cpy_pass(in, "wifi_pass", c.wifi_pass, sizeof(c.wifi_pass));
    cpy_str(in, "mqtt_host", c.mqtt_host, sizeof(c.mqtt_host));
    cpy_str(in, "mqtt_user", c.mqtt_user, sizeof(c.mqtt_user));
    cpy_pass(in, "mqtt_pass", c.mqtt_pass, sizeof(c.mqtt_pass));
    cpy_str(in, "mqtt_base_topic", c.mqtt_base_topic, sizeof(c.mqtt_base_topic));
    cpy_str(in, "mqtt_discovery_prefix", c.mqtt_discovery_prefix, sizeof(c.mqtt_discovery_prefix));
    cpy_str(in, "device_name", c.device_name, sizeof(c.device_name));
    cpy_str(in, "tz", c.tz, sizeof(c.tz));
    cJSON *v;
    if ((v = cJSON_GetObjectItem(in, "mqtt_enabled"))) c.mqtt_enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(in, "mqtt_tls")))     c.mqtt_tls = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(in, "mqtt_port")) && cJSON_IsNumber(v)) c.mqtt_port = (uint16_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "publish_interval_s")) && cJSON_IsNumber(v)) c.publish_interval_s = (uint16_t)v->valuedouble;
    // The brush joins no Wi-Fi network without a web password (one set in this same
    // save counts; see also wifi_mgr_start()): it would otherwise sit on the home network
    // with a web UI anyone there can take over. Refused before anything is stored.
    // Removing the network needs none.
    bool wifi_changed = strcmp(old_ssid, c.wifi_ssid) != 0 || strcmp(old_pass, c.wifi_pass) != 0;
    bool needs_pass = wifi_changed && c.wifi_ssid[0] && !pass_set();
    if (needs_pass && !web_pass[0]) {
        cJSON_Delete(in);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "set a web UI password first: the brush joins no Wi-Fi network without one");
        return ESP_OK;
    }
    if ((v = cJSON_GetObjectItem(in, "lcd_panel")) && cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 5) {
        nvs_handle_t h;
        if (nvs_open("oclean", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "lcd_panel", (uint8_t)v->valuedouble); nvs_commit(h); nvs_close(h); }
    }
    cJSON_Delete(in);
    net_activity();

    // The brush's clock follows a new time zone at once. What the C library would not
    // understand (an empty field, a name like "Europe/Berlin") is not stored: the old
    // zone stays, and the reply says which one is in effect.
    if (strcmp(old_tz, c.tz) != 0 && !oem_glue_set_tz(c.tz)) strlcpy(c.tz, old_tz, sizeof(c.tz));

    // Before Wi-Fi is applied: the hash takes about a second, and new Wi-Fi settings
    // must find the reply already sent. This browser gets the new token; every other
    // one has to log in again.
    bool new_pass = web_pass[0] != '\0', pass_ok = true;
    if (new_pass) {
        pass_ok = auth_set(web_pass);
        memset(web_pass, 0, sizeof web_pass);
        web_auth_t a; auth_get(&a);
        if (pass_ok) set_cookie(r, a.token);
    }
    // The first password: the brush now joins the network it has (it held back without
    // one, see wifi_mgr_start()), the reply going out first.
    if (first && new_pass && pass_ok && c.wifi_ssid[0]) wifi_changed = true;
    if (needs_pass && !pass_ok) {            // the password the network depends on was not stored
        strlcpy(c.wifi_ssid, old_ssid, sizeof(c.wifi_ssid));
        strlcpy(c.wifi_pass, old_pass, sizeof(c.wifi_pass));
        wifi_changed = false;
    }

    bool ok = config_save(&c);
    // apply live
    mqtt_ha_restart(&c);
    if (wifi_changed && c.wifi_ssid[0]) wifi_mgr_apply_sta(&c);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "saved", ok && pass_ok);
    cJSON_AddStringToObject(o, "tz", c.tz);
    cJSON_AddBoolToObject(o, "web_pass_set", pass_set());
    send_json(r, o);
    return ESP_OK;
}

static esp_err_t h_reboot(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "{\"rebooting\":true}");
    // Not in safe mode: the gauge never ran there, and saving its power-on state would
    // replace the battery record with "no record".
    if (brush_app_running()) { hal_lock(); oem_gauge_save(); hal_unlock(); }
    boot_guard_clean_exit();
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

// ---- /api/ota POST : flash a firmware image (raw body) into the idle OTA slot ----
#define OTA_BUF  4096
#define OTA_HEAD (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

// httpd_req_recv() that rides out a stalled sender for ~30 s (6 x the 5 s socket timeout).
static int ota_recv(httpd_req_t *r, char *buf, size_t n)
{
    int k = HTTPD_SOCK_ERR_TIMEOUT;
    for (int i = 0; i < 6 && k == HTTPD_SOCK_ERR_TIMEOUT; i++) k = httpd_req_recv(r, buf, n);
    return k;
}

// OEM screens for a firmware update: 88 with the percentage, then 89 (ok) / 90 (failed).
// In safe mode there is no UI task and no gauge (brush_app_running() is false): the
// update itself must still work, it is what safe mode is for.
static void ota_progress(uint8_t pct)
{
    if (!brush_app_running()) return;
    hal_lock(); oem_show(88, &pct, 1); hal_unlock();
}
static void ota_done(bool ok)
{
    hal_lock();
    if (brush_app_running()) {
        oem_show(ok ? 89 : 90, NULL, 0);
        if (ok) oem_gauge_save();
    }
    if (!ok) g_oem.ota = 0;
    hal_unlock();
}

static esp_err_t h_ota(httpd_req_t *r)
{
    // Browsers send this content type cross-origin only after a CORS preflight,
    // which this server never grants; that keeps other sites' pages from flashing
    // the device through a visitor's browser.
    char ct[40] = "";
    httpd_req_get_hdr_value_str(r, "Content-Type", ct, sizeof(ct));
    if (strcmp(ct, "application/octet-stream") != 0) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "send the image as application/octet-stream");
        return ESP_OK;
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA slot"); return ESP_OK; }
    // Stock refuses an update below 20 % battery and never updates while brushing.
    // Neither is known in safe mode (g_oem holds its initial values there: 0 %, on
    // battery), and safe mode must never refuse the update.
    bool live = brush_app_running();
    hal_lock();
    bool busy = live && g_oem.session_active;
    bool low = live && !g_oem.batt_fault && g_oem.batt_pct < 20 && g_oem.power_state == OEM_PWR_BATTERY;
    if (!busy && !low) g_oem.ota = 1;             // button locked, no idle sleep (cleared below on failure)
    hal_unlock();
    if (busy) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "brushing in progress"); return ESP_OK; }
    if (low)  { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "battery below 20 %: put the brush on the charger"); return ESP_OK; }
    net_activity();
    size_t total = r->content_len;
    if (total < OTA_HEAD || total > part->size) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "image does not fit the OTA slot");
        ota_done(false);
        return ESP_OK;
    }
    char *buf = malloc(OTA_BUF);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); ota_done(false); return ESP_OK; }
    uint8_t pct = 0; ota_progress(pct);

    // Read the head first. Any image this bootloader can boot is accepted: it
    // need not be a build of this project, nor an ESP-IDF app with a descriptor.
    // So only what every bootable image has is checked up front (magic, chip);
    // esp_ota_end() verifies the rest.
    size_t have = 0;
    while (have < OTA_HEAD) {
        int k = ota_recv(r, buf + have, OTA_BUF - have);
        if (k <= 0) { free(buf); ota_done(false); return ESP_FAIL; }   // sender gone: drop the connection
        have += k;
    }
    esp_image_header_t ih; esp_app_desc_t ad;
    memcpy(&ih, buf, sizeof(ih));
    memcpy(&ad, buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(ad));
    if (ih.magic != ESP_IMAGE_HEADER_MAGIC || ih.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        free(buf);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "not an ESP32-S3 firmware image");
        ota_done(false);
        return ESP_OK;
    }
    // Reject what the bootloader will refuse anyway: an image whose first segment
    // loads into the ESP32-S3 second-stage bootloader's DRAM (0x3FCE3700..0x3FCEB710)
    // is a bootloader or a merged full-flash image, not a bootable app. If written it
    // would verify, be marked valid and reported flashed, yet the bootloader would
    // fall back at boot and the stale otadata would break rollback for the next update.
    esp_image_segment_header_t sh;
    memcpy(&sh, buf + sizeof(esp_image_header_t), sizeof(sh));
    if (sh.load_addr >= 0x3FCE3700 && sh.load_addr < 0x3FCEB710) {
        free(buf);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "this is a bootloader or merged flash image, not an app image");
        ota_done(false);
        return ESP_OK;
    }
    bool has_desc = ad.magic_word == ESP_APP_DESC_MAGIC_WORD;
    bool ours = has_desc && strncmp(ad.project_name, esp_app_get_description()->project_name, sizeof(ad.project_name)) == 0;
    if (has_desc) ESP_LOGI(TAG, "OTA: writing %.32s %.32s (%u bytes) to %s", ad.project_name, ad.version, (unsigned)total, part->label);
    else ESP_LOGI(TAG, "OTA: writing an image without an app descriptor (%u bytes) to %s", (unsigned)total, part->label);

    // If our own running image is still on its post-OTA trial (PENDING_VERIFY),
    // confirm it now — reaching this handler proves the update path works — so this
    // esp_ota_begin() is not refused with ESP_ERR_OTA_ROLLBACK_INVALID_STATE.
    esp_ota_img_states_t rst;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &rst) == ESP_OK && rst == ESP_OTA_IMG_PENDING_VERIFY)
        esp_ota_mark_app_valid_cancel_rollback();

    esp_ota_handle_t h = 0;
    // Pass the real size so the slot is erased up front (ceil(total/sector) sectors);
    // OTA_WITH_SEQUENTIAL_WRITES would erase one sector past the end on an image that
    // exactly fills the slot and fail the last write.
    esp_err_t err = esp_ota_begin(part, total, &h);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        ota_done(false);
        return ESP_OK;
    }
    bool sender_gone = false;
    for (size_t got = 0;;) {
        err = esp_ota_write(h, buf, have);
        got += have;
        uint8_t p = (uint8_t)(got * 100 / total);
        if (p / 10 != pct / 10) { pct = p; ota_progress(pct); }   // one screen update per 10 %
        if (err != ESP_OK || got >= total) break;
        size_t want = total - got;
        int k = ota_recv(r, buf, want < OTA_BUF ? want : OTA_BUF);
        if (k <= 0) { sender_gone = true; err = ESP_FAIL; break; }
        have = k;
    }
    free(buf);
    if (err == ESP_OK) err = esp_ota_end(h);   // verifies the whole image (checksum, SHA-256)
    else esp_ota_abort(h);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", sender_gone ? "upload interrupted" : esp_err_to_name(err));
        ota_done(false);
        if (sender_gone) return ESP_FAIL;
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    // Our own builds confirm themselves once started (see app_main), so they keep
    // rollback protection. Other firmware can't be assumed to, and the bootloader
    // would revert an unconfirmed image at its second boot, so it is marked valid
    // now. (The call acts on the active otadata entry, which is now the new slot's.)
    bool rollback = true;
    if (!ours) {
        esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
        esp_ota_mark_app_valid_cancel_rollback();
        rollback = !(esp_ota_get_state_partition(part, &st) == ESP_OK && st == ESP_OTA_IMG_VALID);
    }
    ESP_LOGI(TAG, "OTA done (%s), rebooting into %s", rollback ? "rollback armed" : "kept without rollback", part->label);
    ota_done(true);
    boot_guard_clean_exit();
    // Push the whole reply out before the reboot: disable Nagle so the body isn't
    // held waiting for an ACK of the header, and close the connection so the browser
    // doesn't try to reuse a socket that is about to die with the restart.
    int sfd = httpd_req_to_sockfd(r);
    if (sfd >= 0) { int one = 1; setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)); }
    httpd_resp_set_hdr(r, "Connection", "close");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, rollback ? "{\"rebooting\":true,\"rollback\":true}" : "{\"rebooting\":true,\"rollback\":false}");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

#define LOGBUF_CHUNK 16384   // max log bytes returned per /api/log poll (== the ring size)
// ---- /api/log : device log tail (no serial on this device) ----
// GET /api/log?since=<cursor>
// Body = new log text; response header X-Log-Cursor = the cursor to pass next time.
static esp_err_t h_log(httpd_req_t *r)
{
    size_t cursor = 0;
    size_t qlen = httpd_req_get_url_query_len(r) + 1;
    if (qlen > 1 && qlen < 128) {
        char q[128]; char v[24];
        if (httpd_req_get_url_query_str(r, q, sizeof(q)) == ESP_OK &&
            httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) cursor = strtoul(v, NULL, 10);
    }
    char *buf = malloc(LOGBUF_CHUNK);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    weblog_read(buf, LOGBUF_CHUNK, &cursor);
    char hdr[24]; snprintf(hdr, sizeof(hdr), "%u", (unsigned)cursor);
    httpd_resp_set_hdr(r, "X-Log-Cursor", hdr);
    httpd_resp_set_type(r, "text/plain; charset=utf-8");
    httpd_resp_sendstr(r, buf);
    free(buf);
    return ESP_OK;
}

// ---- /api/log POST {"level":"verbose|debug|info|warn|error"} : level of the device log ----
// A POST, not a query on the GET above: it changes state, so it gets the CSRF rule.
static esp_err_t h_log_level(httpd_req_t *r)
{
    static const struct { const char *name; esp_log_level_t lvl; } L[] = {
        { "verbose", ESP_LOG_VERBOSE }, { "debug", ESP_LOG_DEBUG }, { "info", ESP_LOG_INFO },
        { "warn", ESP_LOG_WARN }, { "error", ESP_LOG_ERROR },
    };
    char buf[64];
    int n = r->content_len;
    if (n <= 0 || n >= (int)sizeof(buf)) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad len"); return ESP_OK; }
    int got = 0;
    while (got < n) { int k = httpd_req_recv(r, buf + got, n - got); if (k <= 0) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "recv"); return ESP_OK; } got += k; }
    buf[n] = 0;
    cJSON *in = cJSON_Parse(buf);
    cJSON *v = cJSON_GetObjectItem(in, "level");
    unsigned i = 0;
    while (i < sizeof L / sizeof L[0] && !(cJSON_IsString(v) && strcmp(v->valuestring, L[i].name) == 0)) i++;
    cJSON_Delete(in);
    if (i == sizeof L / sizeof L[0]) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "level: verbose, debug, info, warn or error");
        return ESP_OK;
    }
    weblog_set_level("*", L[i].lvl);
    // "*" resets every tag. The server's own two stay at info at most: at debug they log
    // every request and response header, the password (Authorization) and the session
    // token (Cookie, Set-Cookie) included.
    esp_log_level_t cap = L[i].lvl < ESP_LOG_INFO ? L[i].lvl : ESP_LOG_INFO;
    weblog_set_level("httpd_parse", cap);
    weblog_set_level("httpd_txrx", cap);
    net_activity();
    ESP_LOGW(TAG, "log level set to %s", L[i].name);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    return ESP_OK;
}

// Registers a route behind guard(). open: reachable without the password (the page and
// its two assets, so the login view can load).
static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t *), bool open)
{
    static route_t routes[16];
    static int n;
    if (n == sizeof routes / sizeof routes[0]) { ESP_LOGE(TAG, "%s not registered (routes[] full)", uri); return; }
    route_t *rt = &routes[n++];
    *rt = (route_t){ .fn = h, .open = open };
    httpd_uri_t u = { .uri = uri, .method = m, .handler = guard, .user_ctx = rt };
    if (httpd_register_uri_handler(s, &u) != ESP_OK) ESP_LOGE(TAG, "%s not registered (max_uri_handlers?)", uri);
}

bool web_server_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 8192;
    // Sessions whose peer vanished (a phone leaving the setup AP) are otherwise
    // never closed, and once all are taken the server stops accepting. Also keep
    // one of the 10 lwIP sockets (httpd uses 3 itself) free for the MQTT client.
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 6;
    if (!s_auth_loaded) auth_load();
#if CONFIG_PM_ENABLE
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "web_hash", &s_pm_cpu) != ESP_OK) s_pm_cpu = NULL;
#endif
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return false; }
    reg(s, "/",                  HTTP_GET,  h_index,       true);
    reg(s, "/app.js",            HTTP_GET,  h_js,          true);
    reg(s, "/style.css",         HTTP_GET,  h_css,         true);
    reg(s, "/api/status",        HTTP_GET,  h_status,      false);
    reg(s, "/api/config",        HTTP_GET,  h_config_get,  false);
    reg(s, "/api/config",        HTTP_POST, h_config_post, false);
    reg(s, "/api/reboot",        HTTP_POST, h_reboot,      false);
    reg(s, "/api/ota",           HTTP_POST, h_ota,         false);
    reg(s, "/api/log",           HTTP_GET,  h_log,         false);
    reg(s, "/api/log",           HTTP_POST, h_log_level,   false);
    reg(s, "/api/brush",         HTTP_POST, h_brush,       false);
    reg(s, "/api/res",           HTTP_GET,  h_res,         false);
    reg(s, "/api/parts",         HTTP_GET,  h_parts,       false);
    ESP_LOGI(TAG, "web server started on :80");
    return true;
}

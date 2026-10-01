#include "web_server.h"
#include "config_store.h"
#include "metrics.h"
#include "mqtt_ha.h"
#include "wifi_mgr.h"
#include "ble_server.h"
#include "fs_storage.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
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
static esp_err_t h_index(httpd_req_t *r){ return send_embedded(r, index_html_start, index_html_end, "text/html"); }
static esp_err_t h_js(httpd_req_t *r){ return send_embedded(r, app_js_start, app_js_end, "application/javascript"); }
static esp_err_t h_css(httpd_req_t *r){ return send_embedded(r, style_css_start, style_css_end, "text/css"); }

// ---- helpers ----
static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '%' && p[1] && p[2]) {
            char hex[3] = { p[1], p[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') { *o++ = ' '; }
        else { *o++ = *p; }
    }
    *o = 0;
}

// Read the "path" query param, URL-decode, and validate it stays inside the
// browse root. Returns false (and sends 400/403) on any violation.
static bool get_safe_path(httpd_req_t *r, char *out, size_t out_len)
{
    size_t qlen = httpd_req_get_url_query_len(r) + 1;
    if (qlen <= 1 || qlen > 1024) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no path"); return false; }
    char *q = malloc(qlen);
    if (!q) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return false; }
    httpd_req_get_url_query_str(r, q, qlen);
    char val[768];
    if (httpd_query_key_value(q, "path", val, sizeof(val)) != ESP_OK) {
        free(q); httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "no path"); return false;
    }
    free(q);
    url_decode(val);

    const char *root = fs_browse_root();
    // Empty or "/" maps to the browse root.
    char joined[800];
    if (val[0] == '\0' || strcmp(val, "/") == 0) {
        snprintf(joined, sizeof(joined), "%s", root);
    } else if (strncmp(val, root, strlen(root)) == 0) {
        snprintf(joined, sizeof(joined), "%s", val);
    } else if (val[0] == '/') {
        snprintf(joined, sizeof(joined), "%s%s", root, val);  // treat as root-relative
    } else {
        snprintf(joined, sizeof(joined), "%s/%s", root, val);
    }
    // Reject any parent-traversal.
    if (strstr(joined, "..")) { httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "denied"); return false; }
    // Must remain within the root prefix.
    if (strncmp(joined, root, strlen(root)) != 0) { httpd_resp_send_err(r, HTTPD_403_FORBIDDEN, "denied"); return false; }
    strlcpy(out, joined, out_len);
    return true;
}

static void send_json(httpd_req_t *r, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, s);
    free(s);
    cJSON_Delete(o);
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
    send_json(r, o);
    return ESP_OK;
}

// ---- /api/config GET : current config (password masked) ----
static esp_err_t h_config_get(httpd_req_t *r)
{
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

    app_config_t c; config_load(&c);
    char old_ssid[33]; strlcpy(old_ssid, c.wifi_ssid, sizeof(old_ssid));
    char old_pass[65]; strlcpy(old_pass, c.wifi_pass, sizeof(old_pass));

    cpy_str(in, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
    cpy_pass(in, "wifi_pass", c.wifi_pass, sizeof(c.wifi_pass));
    cpy_str(in, "mqtt_host", c.mqtt_host, sizeof(c.mqtt_host));
    cpy_str(in, "mqtt_user", c.mqtt_user, sizeof(c.mqtt_user));
    cpy_pass(in, "mqtt_pass", c.mqtt_pass, sizeof(c.mqtt_pass));
    cpy_str(in, "mqtt_base_topic", c.mqtt_base_topic, sizeof(c.mqtt_base_topic));
    cpy_str(in, "mqtt_discovery_prefix", c.mqtt_discovery_prefix, sizeof(c.mqtt_discovery_prefix));
    cpy_str(in, "device_name", c.device_name, sizeof(c.device_name));
    cJSON *v;
    if ((v = cJSON_GetObjectItem(in, "mqtt_enabled"))) c.mqtt_enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(in, "mqtt_tls")))     c.mqtt_tls = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(in, "mqtt_port")) && cJSON_IsNumber(v)) c.mqtt_port = (uint16_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "publish_interval_s")) && cJSON_IsNumber(v)) c.publish_interval_s = (uint16_t)v->valuedouble;
    cJSON_Delete(in);

    bool ok = config_save(&c);
    // apply live
    mqtt_ha_restart(&c);
    bool wifi_changed = strcmp(old_ssid, c.wifi_ssid) != 0 || strcmp(old_pass, c.wifi_pass) != 0;
    if (wifi_changed && c.wifi_ssid[0]) wifi_mgr_apply_sta(&c);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "saved", ok);
    send_json(r, o);
    return ESP_OK;
}

// ---- /api/fs/list : read-only directory listing ----
static esp_err_t h_fs_list(httpd_req_t *r)
{
    char path[800];
    if (!get_safe_path(r, path, sizeof(path))) return ESP_OK;
    DIR *d = opendir(path);
    if (!d) { httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "not a dir"); return ESP_OK; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "path", path);
    cJSON *arr = cJSON_AddArrayToObject(o, "entries");
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char full[1100];
        snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
        struct stat st; cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", de->d_name);
        if (stat(full, &st) == 0) {
            bool isdir = S_ISDIR(st.st_mode);
            cJSON_AddStringToObject(e, "type", isdir ? "dir" : "file");
            cJSON_AddNumberToObject(e, "size", isdir ? 0 : (double)st.st_size);
            cJSON_AddNumberToObject(e, "mtime", (double)st.st_mtime);
        } else {
            cJSON_AddStringToObject(e, "type", "file");
            cJSON_AddNumberToObject(e, "size", 0);
        }
        cJSON_AddItemToArray(arr, e);
    }
    closedir(d);
    send_json(r, o);
    return ESP_OK;
}

// Stream a file to the client. If `download` set, force attachment.
static esp_err_t stream_file(httpd_req_t *r, bool download)
{
    char path[800];
    if (!get_safe_path(r, path, sizeof(path))) return ESP_OK;
    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) { httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "not a file"); return ESP_OK; }
    FILE *f = fopen(path, "rb");
    if (!f) { httpd_resp_send_err(r, HTTPD_404_NOT_FOUND, "open"); return ESP_OK; }

    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    if (download) {
        httpd_resp_set_type(r, "application/octet-stream");
        char cd[256]; snprintf(cd, sizeof(cd), "attachment; filename=\"%.200s\"", base);
        httpd_resp_set_hdr(r, "Content-Disposition", cd);
    } else {
        httpd_resp_set_type(r, "text/plain; charset=utf-8");
    }
    char *chunk = malloc(2048);
    if (!chunk) { fclose(f); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }
    size_t n;
    while ((n = fread(chunk, 1, 2048, f)) > 0) {
        if (httpd_resp_send_chunk(r, chunk, n) != ESP_OK) { break; }
    }
    free(chunk);
    fclose(f);
    httpd_resp_send_chunk(r, NULL, 0);  // end
    return ESP_OK;
}
static esp_err_t h_fs_view(httpd_req_t *r){ return stream_file(r, false); }
static esp_err_t h_fs_download(httpd_req_t *r){ return stream_file(r, true); }

static esp_err_t h_reboot(httpd_req_t *r)
{
    httpd_resp_sendstr(r, "{\"rebooting\":true}");
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
    size_t total = r->content_len;
    if (total < OTA_HEAD || total > part->size) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "image does not fit the OTA slot");
        return ESP_OK;
    }
    char *buf = malloc(OTA_BUF);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_OK; }

    // Read the head first. Any image this bootloader can boot is accepted: it
    // need not be a build of this project, nor an ESP-IDF app with a descriptor.
    // So only what every bootable image has is checked up front (magic, chip);
    // esp_ota_end() verifies the rest.
    size_t have = 0;
    while (have < OTA_HEAD) {
        int k = ota_recv(r, buf + have, OTA_BUF - have);
        if (k <= 0) { free(buf); return ESP_FAIL; }   // sender gone: drop the connection
        have += k;
    }
    esp_image_header_t ih; esp_app_desc_t ad;
    memcpy(&ih, buf, sizeof(ih));
    memcpy(&ad, buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(ad));
    if (ih.magic != ESP_IMAGE_HEADER_MAGIC || ih.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        free(buf);
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "not an ESP32-S3 firmware image");
        return ESP_OK;
    }
    bool has_desc = ad.magic_word == ESP_APP_DESC_MAGIC_WORD;
    bool ours = has_desc && strncmp(ad.project_name, esp_app_get_description()->project_name, sizeof(ad.project_name)) == 0;
    if (has_desc) ESP_LOGI(TAG, "OTA: writing %.32s %.32s (%u bytes) to %s", ad.project_name, ad.version, (unsigned)total, part->label);
    else ESP_LOGI(TAG, "OTA: writing an image without an app descriptor (%u bytes) to %s", (unsigned)total, part->label);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        free(buf);
        httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_OK;
    }
    bool sender_gone = false;
    for (size_t got = 0;;) {
        err = esp_ota_write(h, buf, have);
        got += have;
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
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, rollback ? "{\"rebooting\":true,\"rollback\":true}" : "{\"rebooting\":true,\"rollback\":false}");
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
    return ESP_OK;
}

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*h)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = h };
    httpd_register_uri_handler(s, &u);
}

void web_server_start(void)
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
    httpd_handle_t s = NULL;
    if (httpd_start(&s, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return; }
    reg(s, "/",                  HTTP_GET,  h_index);
    reg(s, "/app.js",            HTTP_GET,  h_js);
    reg(s, "/style.css",         HTTP_GET,  h_css);
    reg(s, "/api/status",        HTTP_GET,  h_status);
    reg(s, "/api/config",        HTTP_GET,  h_config_get);
    reg(s, "/api/config",        HTTP_POST, h_config_post);
    reg(s, "/api/fs/list",       HTTP_GET,  h_fs_list);
    reg(s, "/api/fs/view",       HTTP_GET,  h_fs_view);
    reg(s, "/api/fs/download",   HTTP_GET,  h_fs_download);
    reg(s, "/api/reboot",        HTTP_POST, h_reboot);
    reg(s, "/api/ota",           HTTP_POST, h_ota);
    ESP_LOGI(TAG, "web server started on :80");
}

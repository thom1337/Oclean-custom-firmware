#include "web_server.h"
#include "config_store.h"
#include "metrics.h"
#include "mqtt_ha.h"
#include "wifi_mgr.h"
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
    cJSON_AddStringToObject(o, "brush_mac", c.brush_mac);
    cJSON_AddStringToObject(o, "brush_name_prefix", c.brush_name_prefix);
    cJSON_AddNumberToObject(o, "ble_poll_interval_s", c.ble_poll_interval_s);
    send_json(r, o);
    return ESP_OK;
}

static void cpy_str(cJSON *o, const char *k, char *dst, size_t n)
{
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsString(v) && v->valuestring) strlcpy(dst, v->valuestring, n);
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

    cpy_str(in, "wifi_ssid", c.wifi_ssid, sizeof(c.wifi_ssid));
    cpy_str(in, "wifi_pass", c.wifi_pass, sizeof(c.wifi_pass));
    cpy_str(in, "mqtt_host", c.mqtt_host, sizeof(c.mqtt_host));
    cpy_str(in, "mqtt_user", c.mqtt_user, sizeof(c.mqtt_user));
    // only overwrite password if a non-empty one was provided
    cJSON *pw = cJSON_GetObjectItem(in, "mqtt_pass");
    if (cJSON_IsString(pw) && pw->valuestring && pw->valuestring[0]) strlcpy(c.mqtt_pass, pw->valuestring, sizeof(c.mqtt_pass));
    cpy_str(in, "mqtt_base_topic", c.mqtt_base_topic, sizeof(c.mqtt_base_topic));
    cpy_str(in, "mqtt_discovery_prefix", c.mqtt_discovery_prefix, sizeof(c.mqtt_discovery_prefix));
    cpy_str(in, "device_name", c.device_name, sizeof(c.device_name));
    cpy_str(in, "brush_mac", c.brush_mac, sizeof(c.brush_mac));
    cpy_str(in, "brush_name_prefix", c.brush_name_prefix, sizeof(c.brush_name_prefix));
    cJSON *v;
    if ((v = cJSON_GetObjectItem(in, "mqtt_enabled"))) c.mqtt_enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(in, "mqtt_tls")))     c.mqtt_tls = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(in, "mqtt_port")) && cJSON_IsNumber(v)) c.mqtt_port = (uint16_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "publish_interval_s")) && cJSON_IsNumber(v)) c.publish_interval_s = (uint16_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(in, "ble_poll_interval_s")) && cJSON_IsNumber(v)) c.ble_poll_interval_s = (uint16_t)v->valuedouble;
    cJSON_Delete(in);

    bool ok = config_save(&c);
    // apply live
    mqtt_ha_restart(&c);
    if (strcmp(old_ssid, c.wifi_ssid) != 0 && c.wifi_ssid[0]) wifi_mgr_apply_sta(&c);

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
    ESP_LOGI(TAG, "web server started on :80");
}

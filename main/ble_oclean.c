#include "ble_oclean.h"
#include "metrics.h"
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "ble_oclean";

// Oclean Standard Service + characteristics (128-bit, little-endian byte order).
static const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(
    0x18,0xcc,0x54,0xb9,0xf9,0x56,0xc6,0x91,0x21,0x40,0xa6,0x41,0xa8,0xca,0x82,0x80); // 8082caa8-...
static const ble_uuid128_t UUID_BB85 = BLE_UUID128_INIT(
    0x85,0xbb,0x3f,0x67,0x5b,0x85,0x83,0x91,0xd8,0x49,0x0c,0x00,0xa3,0xb9,0x84,0x9d); // write (cmd)
static const ble_uuid128_t UUID_BB86 = BLE_UUID128_INIT(
    0x86,0xbb,0x3f,0x67,0x5b,0x85,0x0a,0x99,0xf5,0x46,0x8c,0x79,0x94,0xdf,0x78,0x5f); // notify (status)
static const ble_uuid128_t UUID_BB89 = BLE_UUID128_INIT(
    0x89,0xbb,0x3f,0x67,0x5b,0x85,0x0a,0x99,0xf5,0x46,0x8c,0x79,0x94,0xdf,0x78,0x5f); // write (brush)
static const ble_uuid128_t UUID_BB90 = BLE_UUID128_INIT(
    0x90,0xbb,0x3f,0x67,0x5b,0x85,0x0a,0x99,0xf5,0x46,0x8c,0x79,0x94,0xdf,0x78,0x5f); // notify (session)

static struct {
    app_config_t cfg;
    uint8_t  own_addr_type;
    uint16_t conn;
    uint16_t svc_start, svc_end;
    uint16_t h_bb85, h_bb86, h_bb89, h_bb90;
    bool     connected, ready;
    esp_timer_handle_t poll;
} S = { .conn = BLE_HS_CONN_HANDLE_NONE };

static int gap_event(struct ble_gap_event *ev, void *arg);

// ---------- scanning ----------
static void start_scan(void)
{
    struct ble_gap_disc_params dp = {0};
    dp.filter_duplicates = 1;
    dp.passive = 0;   // active scan to receive the name in scan responses
    ble_gap_disc(S.own_addr_type, BLE_HS_FOREVER, &dp, gap_event, NULL);
    ESP_LOGI(TAG, "scanning for brush…");
}

static bool adv_matches(const struct ble_gap_disc_desc *d)
{
    // Match by MAC if configured.
    if (S.cfg.brush_mac[0]) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                 d->addr.val[5], d->addr.val[4], d->addr.val[3],
                 d->addr.val[2], d->addr.val[1], d->addr.val[0]);
        return strcasecmp(mac, S.cfg.brush_mac) == 0;
    }
    // Otherwise match advertised name prefix.
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return false;
    if (!f.name || f.name_len == 0) return false;
    size_t pl = strlen(S.cfg.brush_name_prefix);
    return f.name_len >= pl && strncmp((const char *)f.name, S.cfg.brush_name_prefix, pl) == 0;
}

// ---------- command writes ----------
static esp_err_t write_to(uint16_t handle, const uint8_t *data, size_t len)
{
    if (!S.connected || handle == 0) return ESP_ERR_INVALID_STATE;
    int rc = ble_gattc_write_flat(S.conn, handle, data, len, NULL, NULL);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t ble_oclean_send_cmd(const uint8_t *data, size_t len) { return write_to(S.h_bb85, data, len); }
esp_err_t ble_oclean_request_status(void)   { const uint8_t c[] = {0x03,0x03}; return write_to(S.h_bb85, c, 2); }
esp_err_t ble_oclean_request_sessions(void) { const uint8_t c[] = {0x03,0x07}; return write_to(S.h_bb89, c, 2); }
esp_err_t ble_oclean_reset_head(void)       { const uint8_t c[] = {0x02,0x0F}; return write_to(S.h_bb85, c, 2); }
esp_err_t ble_oclean_set_brightness(uint8_t pct){ const uint8_t c[] = {0x02,0x0C,pct}; return write_to(S.h_bb85, c, 3); }
esp_err_t ble_oclean_wake_gesture(bool on)  { const uint8_t c[] = {0x02,0x09, on?0x01:0xEC}; return write_to(S.h_bb85, c, 3); }

esp_err_t ble_oclean_sync_time(void)
{
    time_t now = time(NULL);
    struct tm tmv; localtime_r(&now, &tmv);
    uint8_t c[10] = {
        0x02, 0x01,
        (uint8_t)(tmv.tm_year % 100), (uint8_t)(tmv.tm_mon + 1), (uint8_t)tmv.tm_mday,
        (uint8_t)tmv.tm_hour, (uint8_t)tmv.tm_min, (uint8_t)tmv.tm_sec,
        (uint8_t)(tmv.tm_wday == 0 ? 7 : tmv.tm_wday), 0x00
    };
    return write_to(S.h_bb89, c, sizeof(c));
}

// ---------- notification parsing ----------
static void parse_status(const uint8_t *b, int n)   // 0303 response on bb86
{
    if (n < 6) return;
    brush_state_t s; metrics_get_brush_state(&s);
    s.battery_pct = b[5];               // battery percent
    s.brushing = (b[2] & 0x01) != 0;    // status flags byte
    s.charging = (b[2] & 0x02) != 0;
    metrics_set_brush_state(&s);
    ESP_LOGI(TAG, "status: battery=%d%% brushing=%d charging=%d", s.battery_pct, s.brushing, s.charging);
}

static void parse_fw(const uint8_t *b, int n)        // 0307 INFO (version string) on bb86
{
    // payload after 0307 prefix is ASCII like "0.0.1.1" then "OK"
    brush_state_t s; metrics_get_brush_state(&s);
    int j = 0;
    for (int i = 2; i < n && j < (int)sizeof(s.fw_version) - 1; i++) {
        char c = (char)b[i];
        if (c == 'O' && i + 1 < n && b[i+1] == 'K') break;   // trailing OK
        if (c >= '0' && c <= '9') { s.fw_version[j++] = c; }
        else if (c == '.' && j > 0) { s.fw_version[j++] = c; }
    }
    s.fw_version[j] = 0;
    if (j) { metrics_set_brush_state(&s); ESP_LOGI(TAG, "fw version: %s", s.fw_version); }
}

static void parse_session(const uint8_t *b, int n)   // 0307 *B# record on bb90
{
    // Best-effort: a session record arrived -> count it and timestamp it.
    // The "*B#" (2a 42 23) header marks an Oclean session push.
    brush_state_t s; metrics_get_brush_state(&s);
    s.total_sessions += 1;
    s.last_session_epoch = (uint32_t)time(NULL);
    // Extended (K3/newer) record: bytes 0-1 after prefix are a BE length header,
    // score is carried later in the record. Guard heavily; leave unknown if not sure.
    if (n >= 20 && b[2] == 0x2a && b[3] == 0x42 && b[4] == 0x23) {
        // b[17] observed to carry a small count in captures; expose conservatively.
        int dur = b[16];                 // best-effort session duration seconds
        if (dur > 0 && dur < 600) s.last_session_secs = dur;
    }
    metrics_set_brush_state(&s);
    ESP_LOGI(TAG, "session record (#%lu)", (unsigned long)s.total_sessions);
}

// ---------- GATT discovery ----------
static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &UUID_BB85.u) == 0) S.h_bb85 = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &UUID_BB86.u) == 0) S.h_bb86 = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &UUID_BB89.u) == 0) S.h_bb89 = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &UUID_BB90.u) == 0) S.h_bb90 = chr->val_handle;
    } else if (err->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "chars: bb85=%u bb86=%u bb89=%u bb90=%u", S.h_bb85, S.h_bb86, S.h_bb89, S.h_bb90);
        // Subscribe to notify characteristics (CCCD = value handle + 1).
        uint16_t en = 1;
        if (S.h_bb86) ble_gattc_write_flat(conn, S.h_bb86 + 1, &en, sizeof(en), NULL, NULL);
        if (S.h_bb90) ble_gattc_write_flat(conn, S.h_bb90 + 1, &en, sizeof(en), NULL, NULL);
        S.ready = true;
        // Type-1 init + first poll.
        ble_oclean_sync_time();
        ble_oclean_request_status();
        ble_oclean_request_sessions();
        const uint8_t fw[] = {0x03,0x07}; write_to(S.h_bb85, fw, 2);  // version on bb86
        if (S.poll) {
            uint32_t iv = S.cfg.ble_poll_interval_s ? S.cfg.ble_poll_interval_s : 20;
            esp_timer_start_periodic(S.poll, (uint64_t)iv * 1000000ULL);
        }
    }
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc) {
        S.svc_start = svc->start_handle; S.svc_end = svc->end_handle;
    } else if (err->status == BLE_HS_EDONE) {
        if (S.svc_start) ble_gattc_disc_all_chrs(conn, S.svc_start, S.svc_end, on_chr, NULL);
        else { ESP_LOGW(TAG, "Oclean service not found"); }
    }
    return 0;
}

// ---------- GAP event ----------
static void poll_cb(void *arg)
{
    if (!S.connected || !S.ready) return;
    ble_oclean_request_status();
    ble_oclean_request_sessions();
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        if (adv_matches(&ev->disc)) {
            ble_gap_disc_cancel();
            struct ble_gap_conn_params cp = {0};  // defaults
            ble_gap_connect(S.own_addr_type, &ev->disc.addr, 30000, NULL, gap_event, NULL);
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            S.conn = ev->connect.conn_handle; S.connected = true; S.ready = false;
            S.svc_start = S.svc_end = 0; S.h_bb85 = S.h_bb86 = S.h_bb89 = S.h_bb90 = 0;
            ESP_LOGI(TAG, "connected; discovering services");
            ble_gattc_disc_svc_by_uuid(S.conn, &SVC_UUID.u, on_svc, NULL);
        } else {
            ESP_LOGW(TAG, "connect failed (%d); rescanning", ev->connect.status);
            start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason %d); rescanning", ev->disconnect.reason);
        S.connected = false; S.ready = false; S.conn = BLE_HS_CONN_HANDLE_NONE;
        if (S.poll) esp_timer_stop(S.poll);
        start_scan();
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        int n = OS_MBUF_PKTLEN(ev->notify_rx.om);
        if (n <= 0 || n > 256) return 0;
        uint8_t buf[256];
        os_mbuf_copydata(ev->notify_rx.om, 0, n, buf);
        uint16_t h = ev->notify_rx.attr_handle;
        if (h == S.h_bb86) {
            if (n >= 2 && buf[0] == 0x03 && buf[1] == 0x03) parse_status(buf, n);
            else if (n >= 3 && buf[0] == 0x03 && buf[1] == 0x07) parse_fw(buf, n);
        } else if (h == S.h_bb90) {
            parse_session(buf, n);
        }
        return 0;
    }
    default:
        return 0;
    }
}

// ---------- host task / init ----------
static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &S.own_addr_type);
    start_scan();
}
static void on_reset(int reason) { ESP_LOGW(TAG, "nimble reset, reason=%d", reason); }

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

bool ble_oclean_is_connected(void) { return S.connected && S.ready; }

void ble_oclean_start(const app_config_t *cfg)
{
    S.cfg = *cfg;
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble_port_init failed"); return; }
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("oclean-bridge");
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    const esp_timer_create_args_t ta = { .callback = poll_cb, .name = "ble_poll" };
    esp_timer_create(&ta, &S.poll);
    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "BLE central started (target: %s)",
             cfg->brush_mac[0] ? cfg->brush_mac : cfg->brush_name_prefix);
}

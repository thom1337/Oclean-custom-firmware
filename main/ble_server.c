#include "ble_server.h"
#include "metrics.h"
#include "oem_api.h"
#include "oem_hal.h"
#include "oem_glue.h"
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "esp_system.h"
#include "boot_guard.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_srv";
#define DEV_NAME "Oclean X Ultra 20"

// Oclean Standard Service + chars (128-bit, little-endian init order).
static const ble_uuid128_t SVC   = BLE_UUID128_INIT(0x18,0xcc,0x54,0xb9,0xf9,0x56,0xc6,0x91,0x21,0x40,0xa6,0x41,0xa8,0xca,0x82,0x80);
static const ble_uuid128_t BB85  = BLE_UUID128_INIT(0x85,0xbb,0x3f,0x67,0x5b,0x85,0x83,0x91,0xd8,0x49,0x0c,0x00,0xa3,0xb9,0x84,0x9d);
static const ble_uuid128_t BB86  = BLE_UUID128_INIT(0x86,0xbb,0x3f,0x67,0x5b,0x85,0x0a,0x99,0xf5,0x46,0x8c,0x79,0x94,0xdf,0x78,0x5f);
static const ble_uuid128_t BB89  = BLE_UUID128_INIT(0x89,0xbb,0x3f,0x67,0x5b,0x85,0x0a,0x99,0xf5,0x46,0x8c,0x79,0x94,0xdf,0x78,0x5f);
static const ble_uuid128_t BB90  = BLE_UUID128_INIT(0x90,0xbb,0x3f,0x67,0x5b,0x85,0x0a,0x99,0xf5,0x46,0x8c,0x79,0x94,0xdf,0x78,0x5f);

static uint8_t  s_own_addr_type;
static volatile bool s_started;            // the NimBLE host is up (ble_server_start succeeded)
static volatile bool s_adv_off;            // deep sleep is coming: do not advertise again
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_h_bb86, s_h_bb90;        // value handles of notify chars
static uint8_t  s_last86[32]; static int s_n86;   // last response for READ
static uint8_t  s_last90[32]; static int s_n90;

static int gap_event(struct ble_gap_event *ev, void *arg);

bool ble_server_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

static void notify(uint16_t handle, const uint8_t *data, int n, uint8_t *cache, int *ncache)
{
    memcpy(cache, data, n); *ncache = n;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
    if (om) ble_gatts_notify_custom(s_conn, handle, om);
}

// Handle a command written to bb85/bb89 (category/opcode recovered from stock FW).
static void handle_write(const uint8_t *b, int n, bool brush_chan)
{
    if (n < 2) return;
    uint8_t cat = b[0], op = b[1];
    bool live = brush_app_running();                  // false in safe mode (BLE is not started there today)
    if (live) oem_net_activity();                     // a GATT write keeps the brush awake, as stock
    brush_state_t s; metrics_get_brush_state(&s);

    if (cat == 0x03 && op == 0x03) {                 // status -> bb86
        uint8_t r[6] = { 0x03, 0x03,
            (uint8_t)((s.brushing ? 1 : 0) | (s.charging ? 2 : 0)),
            0, 0, (uint8_t)(s.battery_pct < 0 ? 0 : s.battery_pct) };
        notify(s_h_bb86, r, 6, s_last86, &s_n86);
    } else if (cat == 0x03 && op == 0x07) {          // version / sessions
        if (brush_chan) {                            // sessions -> bb90 (*B# framed)
            uint8_t r[8] = { 0x03, 0x07, 0x2a, 0x42, 0x23, 0x00, 0x4f, 0x4b };
            notify(s_h_bb90, r, 8, s_last90, &s_n90);
        } else {                                     // version -> bb86
            const char *v = s.fw_version[0] ? s.fw_version : "1.3.3.7";
            uint8_t r[24] = { 0x03, 0x07 }; int k = 2;
            for (const char *p = v; *p && k < 20; p++) r[k++] = *p;
            r[k++] = 'O'; r[k++] = 'K';
            notify(s_h_bb86, r, k, s_last86, &s_n86);
        }
    } else if (cat == 0x02 && op == 0x06) {          // set brush scheme: only the strength field is honoured
        if (live && n >= 3 && (b[2] & 0x1F) >= 1 && (b[2] & 0x1F) <= 5) oem_remote_strength(b[2] & 0x1F);
        uint8_t r[4] = { 0x02, 0x06, 0x4f, 0x4b }; notify(s_h_bb86, r, 4, s_last86, &s_n86);
    } else if (cat == 0x02 && op == 0x0F) {          // reset brush head (no counter in this firmware)
        uint8_t r[4] = { 0x02, 0x0f, 0x4f, 0x4b }; notify(s_h_bb86, r, 4, s_last86, &s_n86);
    } else if (cat == 0x09 && op == 0xED) {          // 09 ED EF factory reset
        // Only our own namespace: the stock one holds the panel id and UI language.
        ESP_LOGW(TAG, "factory reset requested");
        nvs_handle_t h;
        if (nvs_open("oclean", NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
        boot_guard_clean_exit();
        esp_restart();
    } else {                                         // generic ACK
        uint8_t r[4] = { cat, op, 0x4f, 0x4b }; notify(s_h_bb86, r, 4, s_last86, &s_n86);
    }
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *u = ctxt->chr->uuid;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[64];
        int n = OS_MBUF_PKTLEN(ctxt->om);
        if (n > (int)sizeof(buf)) n = sizeof(buf);
        os_mbuf_copydata(ctxt->om, 0, n, buf);
        bool brush_chan = (ble_uuid_cmp(u, &BB89.u) == 0);
        handle_write(buf, n, brush_chan);
        return 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        if (ble_uuid_cmp(u, &BB86.u) == 0) return os_mbuf_append(ctxt->om, s_last86, s_n86) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
        if (ble_uuid_cmp(u, &BB90.u) == 0) return os_mbuf_append(ctxt->om, s_last90, s_n90) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    return 0;
}

static const struct ble_gatt_svc_def GATT[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &SVC.u,
      .characteristics = (struct ble_gatt_chr_def[]){
        { .uuid = &BB85.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
        { .uuid = &BB86.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_h_bb86 },
        { .uuid = &BB89.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
        { .uuid = &BB90.u, .access_cb = chr_access, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &s_h_bb90 },
        {0},
      }},
    {0},
};

static void advertise(void)
{
    if (s_adv_off) return;
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)DEV_NAME; f.name_len = strlen(DEV_NAME); f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    // service UUID in scan response (128-bit is too big for the adv packet w/ name)
    struct ble_hs_adv_fields rsp = {0};
    rsp.uuids128 = (ble_uuid128_t *)&SVC; rsp.num_uuids128 = 1; rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params ap = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap, gap_event, NULL);
    ESP_LOGI(TAG, "advertising as '%s'", DEV_NAME);
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle; ESP_LOGI(TAG, "phone connected");
            hal_event_post(OEM_EV_BLE_WAKE);          // stock: a BLE connection wakes the screen
            if (brush_app_running()) { hal_lock(); oem_gauge_report_reset(); hal_unlock(); }
        } else advertise();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (%d)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE; advertise();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void on_sync(void) { ble_hs_util_ensure_addr(0); ble_hs_id_infer_auto(0, &s_own_addr_type); advertise(); }
static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

void ble_server_start(void)
{
    if (nimble_port_init() != ESP_OK) { ESP_LOGE(TAG, "nimble init"); return; }
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(GATT);
    ble_gatts_add_svcs(GATT);
    ble_svc_gap_device_name_set(DEV_NAME);
    ble_hs_cfg.sync_cb = on_sync;
    nimble_port_freertos_init(host_task);
    s_started = true;
    ESP_LOGI(TAG, "BLE GATT server starting (Oclean service)");
}

// Right before deep sleep (stock: esp_ble_gap_stop_advertising at the end of the BLE
// window). Called by the pre-sleep hook: main task, core lock held. It must not wait
// for the host task, which may be blocked on that lock in a GATT / GAP callback;
// ble_gap_adv_stop() only waits for the controller's answer (at most 2 s).
void ble_server_stop_adv(void)
{
    s_adv_off = true;
    if (!s_started || !ble_hs_synced()) return;
    int rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv stop: %d", rc);
}

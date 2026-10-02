#include "mqtt_ha.h"
#include "metrics.h"
#include "oem_api.h"
#include "oem_glue.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "cJSON.h"

static const char *TAG = "mqtt_ha";
static esp_mqtt_client_handle_t s_client;
static app_config_t s_cfg;
static esp_timer_handle_t s_timer;
static volatile bool s_connected;
static char s_node[32];      // unique node id, e.g. oclean_a1b2c3
static char s_avail[96];     // availability topic
static char s_state[96];     // state topic
static char s_cmd_base[96];  // command topic base: <base>/<node>/cmd
static char s_cmd_wild[100]; // subscription wildcard: <base>/<node>/cmd/#

static void make_ids(void)
{
    uint8_t mac[6]; esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_node, sizeof(s_node), "oclean_%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(s_avail, sizeof(s_avail), "%s/%s/availability", s_cfg.mqtt_base_topic, s_node);
    snprintf(s_state, sizeof(s_state), "%s/%s/state", s_cfg.mqtt_base_topic, s_node);
    snprintf(s_cmd_base, sizeof(s_cmd_base), "%s/%s/cmd", s_cfg.mqtt_base_topic, s_node);
    snprintf(s_cmd_wild, sizeof(s_cmd_wild), "%s/#", s_cmd_base);
}

// Controllable entities exposed to Home Assistant. Each maps an HA control to a
// oem_remote_* call on the brush logic via handle_cmd() below.
typedef struct { const char *component, *id, *name, *icon; } cmd_ent_t;
static const cmd_ent_t CMD_ENTS[] = {
    {"switch", "brushing",   "Brushing",           "mdi:toothbrush"},
    {"number", "mode",       "Cleaning Mode",      "mdi:tune"},
    {"number", "strength",   "Cleaning Intensity", "mdi:speedometer"},
};

// Remote commands behave like the user doing it on the brush (oem_remote_*).
static void handle_cmd(const char *id, const char *val)
{
    if (!id) return;
    ESP_LOGI(TAG, "cmd '%s' = '%s'", id, val ? val : "");
    if (!brush_app_running()) {               // safe mode: nobody runs the brush logic
        ESP_LOGW(TAG, "cmd ignored: the brush logic is not running");
        return;
    }
    oem_net_activity();
    if      (!strcmp(id, "brushing"))   oem_remote_brushing(val && (!strcmp(val, "ON") || !strcmp(val, "on")));
    else if (!strcmp(id, "mode"))       oem_remote_mode((uint8_t)atoi(val ? val : "5"));
    else if (!strcmp(id, "strength"))   oem_remote_strength((uint8_t)atoi(val ? val : "3"));
    else ESP_LOGW(TAG, "unknown cmd id '%s'", id);
}

// Build the HA "device" object (shared by every entity so they group together).
static cJSON *device_obj(void)
{
    cJSON *d = cJSON_CreateObject();
    cJSON *ids = cJSON_CreateArray();
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_node));
    cJSON_AddItemToObject(d, "identifiers", ids);
    cJSON_AddStringToObject(d, "name", s_cfg.device_name);
    cJSON_AddStringToObject(d, "manufacturer", "Oclean");
    cJSON_AddStringToObject(d, "model", "X Ultra 20 (OCLEANV20)");
    // Only the version, not a snapshot of the brush state: this runs once per entity
    // (37 times per discovery) and a snapshot takes the core lock of the brush logic.
    const char *fw = metrics_fw_version();
    cJSON_AddStringToObject(d, "sw_version", fw[0] ? fw : "custom");
    return d;
}

static void publish_discovery(void)
{
    size_t n; const metric_def_t *defs = metrics_defs(&n);
    for (size_t i = 0; i < n; i++) {
        const metric_def_t *m = defs[i].id ? &defs[i] : NULL;
        if (!m) continue;
        const char *component = (defs[i].kind == M_BOOL) ? "binary_sensor" : "sensor";
        char topic[160];
        snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
                 s_cfg.mqtt_discovery_prefix, component, s_node, defs[i].id);

        cJSON *c = cJSON_CreateObject();
        char uniq[64], name[96];
        snprintf(uniq, sizeof(uniq), "%s_%s", s_node, defs[i].id);
        snprintf(name, sizeof(name), "%s", defs[i].name);
        cJSON_AddStringToObject(c, "name", name);
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "state_topic", s_state);
        cJSON_AddStringToObject(c, "availability_topic", s_avail);
        char tmpl[64];
        snprintf(tmpl, sizeof(tmpl), "{{ value_json.%s }}", defs[i].id);
        cJSON_AddStringToObject(c, "value_template", tmpl);
        if (defs[i].unit)         cJSON_AddStringToObject(c, "unit_of_measurement", defs[i].unit);
        if (defs[i].device_class) cJSON_AddStringToObject(c, "device_class", defs[i].device_class);
        if (defs[i].state_class)  cJSON_AddStringToObject(c, "state_class", defs[i].state_class);
        if (defs[i].icon)         cJSON_AddStringToObject(c, "icon", defs[i].icon);
        if (defs[i].kind == M_BOOL) {
            cJSON_AddStringToObject(c, "payload_on", "ON");
            cJSON_AddStringToObject(c, "payload_off", "OFF");
        }
        cJSON_AddItemToObject(c, "device", device_obj());

        char *payload = cJSON_PrintUnformatted(c);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true /*retain*/);
        free(payload);
        cJSON_Delete(c);
    }
    ESP_LOGI(TAG, "published HA discovery for %u metrics", (unsigned)n);

    // Controllable entities (HA -> local hardware commands).
    size_t cn = sizeof(CMD_ENTS) / sizeof(CMD_ENTS[0]);
    for (size_t i = 0; i < cn; i++) {
        const cmd_ent_t *e = &CMD_ENTS[i];
        char topic[160], cmd_topic[128], uniq[64];
        snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
                 s_cfg.mqtt_discovery_prefix, e->component, s_node, e->id);
        snprintf(cmd_topic, sizeof(cmd_topic), "%s/%s", s_cmd_base, e->id);
        snprintf(uniq, sizeof(uniq), "%s_%s", s_node, e->id);

        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "name", e->name);
        cJSON_AddStringToObject(c, "unique_id", uniq);
        cJSON_AddStringToObject(c, "command_topic", cmd_topic);
        cJSON_AddStringToObject(c, "availability_topic", s_avail);
        cJSON_AddStringToObject(c, "icon", e->icon);
        if (!strcmp(e->component, "button")) {
            cJSON_AddStringToObject(c, "payload_press", "PRESS");
        } else if (!strcmp(e->component, "number")) {
            bool strength = !strcmp(e->id, "strength");
            cJSON_AddNumberToObject(c, "min", strength ? 1 : 0);
            cJSON_AddNumberToObject(c, "max", 5);
            cJSON_AddStringToObject(c, "mode", "box");
        } else if (!strcmp(e->component, "switch")) {
            cJSON_AddStringToObject(c, "payload_on", "ON");
            cJSON_AddStringToObject(c, "payload_off", "OFF");
            cJSON_AddBoolToObject(c, "optimistic", true);
        }
        cJSON_AddItemToObject(c, "device", device_obj());
        char *payload = cJSON_PrintUnformatted(c);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 1, true);
        free(payload);
        cJSON_Delete(c);
    }
    ESP_LOGI(TAG, "published HA discovery for %u commands", (unsigned)cn);
}

void mqtt_ha_publish_state(void)
{
    if (!s_client || !s_connected) return;
    cJSON *o = metrics_build_state_json();
    char *payload = cJSON_PrintUnformatted(o);
    esp_mqtt_client_publish(s_client, s_state, payload, 0, 0, false);
    free(payload);
    cJSON_Delete(o);
}

// The periodic publish takes the core lock (metrics snapshot) and writes to the
// socket. Neither belongs in the esp_timer task: its callbacks also drive the brush
// logic (10 ms tick, button timers), and it would sit there for as long as the main
// task keeps the lock (up to about half a second during a wake). So the timer only
// wakes this task. s_pub_lock keeps a client restart from destroying the client
// under a publish.
static TaskHandle_t s_pub_task;
static SemaphoreHandle_t s_pub_lock;

static void pub_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_pub_lock, portMAX_DELAY);
        mqtt_ha_publish_state();
        xSemaphoreGive(s_pub_lock);
    }
}

static void on_timer(void *arg) { (void)arg; if (s_pub_task) xTaskNotifyGive(s_pub_task); }

static void on_mqtt(void *h, esp_event_base_t base, int32_t id, void *data)
{
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            ESP_LOGI(TAG, "connected");
            esp_mqtt_client_publish(s_client, s_avail, "online", 0, 1, true);
            esp_mqtt_client_subscribe(s_client, s_cmd_wild, 1);
            publish_discovery();
            mqtt_ha_publish_state();
            break;
        case MQTT_EVENT_DISCONNECTED:
            s_connected = false;
            ESP_LOGW(TAG, "disconnected");
            break;
        case MQTT_EVENT_DATA: {
            esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
            char topic[160], val[64];
            int tl = ev->topic_len < (int)sizeof(topic) - 1 ? ev->topic_len : (int)sizeof(topic) - 1;
            int dl = ev->data_len  < (int)sizeof(val) - 1   ? ev->data_len  : (int)sizeof(val) - 1;
            memcpy(topic, ev->topic, tl); topic[tl] = 0;
            memcpy(val, ev->data, dl);   val[dl] = 0;
            const char *id = strstr(topic, "/cmd/");
            if (id) handle_cmd(id + 5, val);
            break;
        }
        default: break;
    }
}

static void stop_client(void)
{
    if (s_timer) { esp_timer_stop(s_timer); esp_timer_delete(s_timer); s_timer = NULL; }
    if (s_pub_lock) xSemaphoreTake(s_pub_lock, portMAX_DELAY);   // a periodic publish in progress finishes first
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    s_connected = false;
    if (s_pub_lock) xSemaphoreGive(s_pub_lock);
}

void mqtt_ha_start(const app_config_t *cfg)
{
    stop_client();
    s_cfg = *cfg;
    if (!s_cfg.mqtt_enabled || s_cfg.mqtt_host[0] == '\0') {
        ESP_LOGI(TAG, "MQTT disabled or no host; not starting");
        return;
    }
    make_ids();

    esp_mqtt_client_config_t mc = {0};
    mc.broker.address.hostname = s_cfg.mqtt_host;
    mc.broker.address.port = s_cfg.mqtt_port;
    mc.broker.address.transport = s_cfg.mqtt_tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
    if (s_cfg.mqtt_user[0]) mc.credentials.username = s_cfg.mqtt_user;
    if (s_cfg.mqtt_pass[0]) mc.credentials.authentication.password = s_cfg.mqtt_pass;
    mc.session.last_will.topic = s_avail;
    mc.session.last_will.msg = "offline";
    mc.session.last_will.qos = 1;
    mc.session.last_will.retain = true;
    if (s_cfg.mqtt_tls) mc.broker.verification.skip_cert_common_name_check = true; // broker often self-signed on LAN

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) { ESP_LOGE(TAG, "client init failed"); return; }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    esp_mqtt_client_start(s_client);

    uint32_t iv = s_cfg.publish_interval_s ? s_cfg.publish_interval_s : 30;
    if (!s_pub_lock) s_pub_lock = xSemaphoreCreateMutex();
    if (s_pub_lock && !s_pub_task && xTaskCreate(pub_task, "mqtt_state", 6144, NULL, 2, &s_pub_task) != pdPASS)
        s_pub_task = NULL;
    if (!s_pub_task) ESP_LOGE(TAG, "no publish task: state is only sent on connect");
    const esp_timer_create_args_t ta = { .callback = on_timer, .name = "mqtt_state" };
    esp_timer_create(&ta, &s_timer);
    esp_timer_start_periodic(s_timer, (uint64_t)iv * 1000000ULL);
    ESP_LOGI(TAG, "MQTT started -> %s:%u (publish every %us)", s_cfg.mqtt_host, (unsigned)s_cfg.mqtt_port, (unsigned)iv);
}

void mqtt_ha_restart(const app_config_t *cfg) { mqtt_ha_start(cfg); }
bool mqtt_ha_is_connected(void) { return s_connected; }

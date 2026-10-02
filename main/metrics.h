#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

// A single published metric. `id` is the stable MQTT/HA object id (snake_case),
// `name` is the human label, `unit` optional (NULL = none), `device_class` /
// `state_class` are Home-Assistant hints (NULL = omit). `kind` selects how the
// value is rendered when building the state JSON.
typedef enum { M_FLOAT, M_INT, M_STR, M_BOOL } metric_kind_t;

typedef struct {
    const char *id;
    const char *name;
    const char *unit;
    const char *device_class;
    const char *state_class;
    const char *icon;
    metric_kind_t kind;
} metric_def_t;

// Snapshot of the brush state kept by the oem core (see oem_state.h), for the web
// UI, MQTT and BLE. battery_pct / brush_score are -1 when unknown.
typedef struct {
    int      battery_pct;
    int      battery_mv;
    int      power_state;        // OEM_PWR_* (1 charging, 2 battery, 3 full)
    bool     charging;
    bool     brushing;           // session running
    bool     paused;             // session paused
    int      mode;               // brushing mode 0..5
    int      strength;           // intensity 1..5 (mode 5)
    int      session_secs;       // elapsed seconds of the current / last session
    int      session_total;      // planned seconds
    int      brush_score;        // 0..100, -1 none yet
    int      sessions_today;
    int      seconds_today;
    int      screen;             // current OEM screen id
    bool     asleep;             // screen-off stage
    bool     locked;             // touch lock
    int      pressure;           // brushing force (AW8686X units)
    int      touch_state;        // touch controller state (5 = running)
    int      lang;
    float    imu_temp_c;         // QMI8658 die temperature, NAN unknown
    char     fw_version[16];
} brush_state_t;

void metrics_set_fw_version(const char *v);
const char *metrics_fw_version(void);      // "" until set; without a snapshot and without the core lock
void metrics_get_brush_state(brush_state_t *out);

// The full static list of metric definitions and its length.
const metric_def_t *metrics_defs(size_t *count);

// Build a JSON object with the current value of every metric (keys = metric id).
// Caller owns the returned cJSON* (cJSON_Delete).
cJSON *metrics_build_state_json(void);

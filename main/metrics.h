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

// Brush-domain values fed from the application/BLE layer (updated elsewhere).
// All "possible metrics" the stock app exposes over BLE map onto these.
typedef struct {
    int      battery_pct;        // 0..100, -1 unknown
    bool     charging;
    bool     brushing;
    int      mode;               // current cleaning mode/scheme index, -1 unknown
    int      last_session_secs;  // duration of last brushing session
    uint32_t last_session_epoch; // unix time of last session, 0 unknown
    int      brush_score;        // 0..100, -1 unknown
    uint32_t total_sessions;     // lifetime brushing sessions
    int      brush_head_days;    // brush-head age in days, -1 unknown
    char     fw_version[16];     // firmware version string reported over BLE
    int      pressure;           // raw force-sensor reading (AW8686X), -1 unknown
    float    imu_temp_c;         // QMI8658 die temperature, NAN unknown
} brush_state_t;

// Thread-safe setter the BLE/app layer calls as new data arrives.
void metrics_set_brush_state(const brush_state_t *s);
void metrics_get_brush_state(brush_state_t *out);

// The full static list of metric definitions and its length.
const metric_def_t *metrics_defs(size_t *count);

// Build a JSON object with the current value of every metric (keys = metric id).
// Caller owns the returned cJSON* (cJSON_Delete).
cJSON *metrics_build_state_json(void);

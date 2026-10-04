#pragma once
#include <stdbool.h>
// Start the HTTP server: dashboard, MQTT/Wi-Fi settings API, read-only copies of the
// flash partitions, and firmware update (upload an app image, flashed to the idle OTA
// slot), behind the optional web password. Returns true if the server started.
bool web_server_start(void);

// Clear the web password (the 8 s button hold, in normal and in safe mode): the web
// UI is open again until a new one is set. Safe from any task, also before
// web_server_start().
void web_auth_forget(void);

// Whether a web password is set. Reads it from NVS on the first call, so it also works
// before web_server_start() (wifi_mgr_start() asks: no password, no home network).
bool web_auth_is_set(void);

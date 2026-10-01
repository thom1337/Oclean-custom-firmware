#pragma once
// Start the HTTP server: dashboard, MQTT/Wi-Fi settings API, a read-only
// filesystem browser (list / view / download), and firmware update (upload an
// app image, flashed to the idle OTA slot).
void web_server_start(void);

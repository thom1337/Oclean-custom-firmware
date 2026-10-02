#pragma once
#include <stdbool.h>
// NimBLE peripheral that serves the Oclean GATT service (8082caa8…) so the
// official phone app can connect to the custom firmware. Best-effort protocol
// parity (status / sessions / control opcodes recovered from the stock FW).
void ble_server_start(void);
bool ble_server_connected(void);
// Stop advertising for good (deep sleep follows). Safe when BLE was never started.
void ble_server_stop_adv(void);

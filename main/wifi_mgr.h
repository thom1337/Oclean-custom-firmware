#pragma once
#include "config_store.h"
// Bring up networking: STA from config if credentials exist, otherwise the
// setup SoftAP ("oclean-setup") so the web UI is reachable for provisioning.
// STA reconnects forever with backoff; while it keeps failing the setup AP
// comes up as a fallback and is dropped again once STA has an IP.
void wifi_mgr_start(const app_config_t *cfg);
bool wifi_mgr_is_connected(void);
// Apply new STA credentials live (after a settings change) and reconnect.
// Returns at once; the switch happens about a second later.
void wifi_mgr_apply_sta(const app_config_t *cfg);
// For hal_wifi_has_ssid() (oem_glue.c); plain flags, callable from any task and
// before wifi_mgr_start() (both false then).
bool wifi_mgr_has_creds(void);     // STA credentials are in use, also ones applied live
bool wifi_mgr_setup_ap_up(void);   // the setup AP is up: no network yet, or the stored one cannot be joined
// The setup AP's WPA3 passcode (9 digits, new each time the AP comes up) while it is
// up; NULL when it is down or open (safe mode, which has no screen to show it on).
const char *wifi_mgr_setup_ap_code(void);

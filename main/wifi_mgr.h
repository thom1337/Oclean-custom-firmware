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

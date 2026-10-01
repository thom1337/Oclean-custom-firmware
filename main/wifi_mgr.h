#pragma once
#include "config_store.h"
// Bring up networking: STA from config if credentials exist, otherwise a
// fallback SoftAP ("oclean-setup") so the web UI is reachable for provisioning.
void wifi_mgr_start(const app_config_t *cfg);
bool wifi_mgr_is_connected(void);
// Apply new STA credentials live (after a settings change) and reconnect.
void wifi_mgr_apply_sta(const app_config_t *cfg);

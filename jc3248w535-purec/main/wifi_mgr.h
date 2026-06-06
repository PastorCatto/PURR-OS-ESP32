// Minimal WiFi station bring-up (esp_wifi) — pure C.
#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

void wifi_mgr_init(void);   // brings up the STA interface (no auto-connect)
bool wifi_mgr_ready(void);

#ifdef __cplusplus
}
#endif

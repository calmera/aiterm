// Phone/browser provisioning: SoftAP + captive DNS + HTTP server serving the embedded page
// and the REST API (§5.2). The HTTP server also runs on the station interface so the page is
// reachable at http://<device-ip>/ once connected.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Start the HTTP server (idempotent). Works on whatever interfaces are up.
esp_err_t provisioning_http_start(void);
// Start the SoftAP "aiterm-XXXX" (open) + captive DNS + HTTP server.
esp_err_t provisioning_ap_start(void);
void      provisioning_ap_stop(void);
bool      provisioning_ap_active(void);
const char* provisioning_ap_ssid(void);
const char* provisioning_ap_password(void);  // "" when open

// Hooks implemented by the application (main.c): called from HTTP handler tasks.
// Return a malloc'd JSON string {"ok":true,...} or {"ok":false,"error":"..."}; caller frees.
char* app_test_inference(void);
char* app_test_mcp(const char* url, const char* auth_header, const char* auth_value);
// Called after a config change was saved (apply theme/font/etc. on the UI task).
void  app_config_changed(void);

#ifdef __cplusplus
}
#endif

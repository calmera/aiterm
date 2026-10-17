// The sign-in flow driven from the setup page: GitHub's device flow, then the identity plane for
// the login and its accounts. A small task polls GitHub; the page polls /api/chronicle/login/status.
#pragma once
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fetches the install profile (when missing or asked), asks GitHub for a device code and starts
// polling. The page shows user_code + verification_uri from the status.
esp_err_t chron_login_start(bool refresh_profile, char* err, size_t err_len);
void      chron_login_cancel(void);
// {"state":"idle|pending|authorized|error","user_code":..,"verification_uri":..,"expires_in":N,
//  "login":..,"accounts":[{"account":..,"role":..}],"account":..,"store":..,"error":..}  (free())
char*     chron_login_status_json(void);
esp_err_t chron_login_set_account(const char* account, const char* store);
void      chron_login_logout(void);

#ifdef __cplusplus
}
#endif

// The GitHub sign-in flow, shared by the badge screen and the setup page: the device flow, then
// the identity plane for the login and its accounts. Runs on its own task; progress is readable
// at any time and announced as CHRON_EV_LOGIN events.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "chron_proto.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHRON_LOGIN_IDLE = 0,
    CHRON_LOGIN_STARTING,    // fetching the install profile and the device code
    CHRON_LOGIN_PENDING,     // code shown, waiting for the approval on GitHub
    CHRON_LOGIN_FINISHING,   // approved; asking chronicle which accounts the identity holds
    CHRON_LOGIN_AUTHORIZED,
    CHRON_LOGIN_ERROR,
} chron_login_state_t;

typedef struct {
    chron_login_state_t state;
    char user_code[16];
    char verification_uri[128];
    char qr_url[176];          // verification_uri?user_code=… — GitHub pre-fills the code
    int  seconds_left;
    char error[160];
    char login[CHRON_NAME_MAX];
    char account[CHRON_NAME_MAX];
    int  accounts;             // memberships found by the last sign-in
} chron_login_status_t;

// Starts a sign-in on its own task. Fails at once when the badge has no Wi-Fi or a sign-in
// is already running. refresh_profile re-fetches the install profile first.
esp_err_t chron_login_start_async(bool refresh_profile, char* err, size_t err_len);
void      chron_login_get(chron_login_status_t* out);
void      chron_login_cancel(void);
const char* chron_login_state_name(chron_login_state_t s);
// For the setup page: the status plus login/account/store/accounts as JSON (free()).
char*     chron_login_status_json(void);
esp_err_t chron_login_set_account(const char* account, const char* store);
void      chron_login_logout(void);

#ifdef __cplusplus
}
#endif

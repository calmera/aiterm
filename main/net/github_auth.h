// GitHub OAuth device flow for the chronicle sign-in (the flow `chronicle login` uses): the badge
// asks GitHub for a code, the person approves it on their phone, the badge polls for the token.
// Device-flow tokens refresh with the client id alone (no client secret). Target only (HTTP).
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "chron_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t github_device_start(const char* client_id, gh_device_t* out, char* err, size_t err_len);
// One poll of the token endpoint: 1 = tokens filled, 0 = still pending, -1 = failed (err filled).
int github_device_poll(const char* client_id, const char* device_code, char* access, size_t an, char* refresh, size_t rn, int* expires_in, char* err, size_t err_len);
// 1 = refreshed (access + new refresh token), -1 = refused / failed.
int github_refresh(const char* client_id, const char* refresh_token, char* access, size_t an, char* refresh, size_t rn, int* expires_in, char* err, size_t err_len);

#ifdef __cplusplus
}
#endif

// One NATS connection (espidf-nats, C++) behind a C API. A dedicated task owns the client: it runs
// the protocol loop and executes every call below, so callers from any task block until their
// command has run. Message callbacks run on that task; from inside a callback only the
// non-blocking calls (publish, subscribe, unsubscribe) may be used.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* host;
    int         port;
    bool        tls;          // TLS with the IDF certificate bundle; sni = host
    const char* jwt;          // user JWT (sentinel) — may be NULL
    const char* nkey_seed;    // "S..." seed matching the JWT — may be NULL
    const char* token;        // connect token ("<selector>:<github token>") — may be NULL
    const char* name;         // client name
} nats_glue_cfg_t;

// subject/reply/headers/data are only valid during the callback.
typedef void (*nats_glue_msg_cb)(const char* subject, const char* reply, const char* headers, int header_len,
                                 const char* data, int len, void* user);
typedef void (*nats_glue_state_cb)(bool connected, void* user);

typedef struct {
    char* headers;    // plat_malloc'd, may be NULL
    int   header_len;
    char* data;       // plat_malloc'd, NUL-terminated
    int   len;
    int   status;     // 0, or the NATS/1.0 status (503 no responders, 408 timeout, …)
} nats_glue_reply_t;

esp_err_t nats_glue_start(void);  // creates the owner task (idempotent)
// Connects (blocking, timeout_ms). Any previous connection is dropped first.
esp_err_t nats_glue_connect(const nats_glue_cfg_t* cfg, int timeout_ms, char* err, size_t err_len);
void      nats_glue_disconnect(void);
bool      nats_glue_connected(void);
void      nats_glue_set_state_cb(nats_glue_state_cb cb, void* user);  // reconnects / drops, on the owner task

int       nats_glue_subscribe(const char* subject, nats_glue_msg_cb cb, void* user);  // >= 0 on success
void      nats_glue_unsubscribe(int sid);
// headers: full "NATS/1.0\r\nK: v\r\n\r\n" block or NULL. data: text payload (NUL-terminated), may be NULL.
esp_err_t nats_glue_publish(const char* subject, const char* headers, const char* data, const char* reply);
// Request/reply with headers (blocking). ESP_ERR_TIMEOUT when nothing answered in time.
esp_err_t nats_glue_request(const char* subject, const char* headers, const char* data, int timeout_ms, nats_glue_reply_t* out);
void      nats_glue_reply_free(nats_glue_reply_t* r);
void      nats_glue_new_inbox(char* out, size_t n);  // a unique _INBOX.aiterm.<id> subject
bool      nats_glue_on_owner_task(void);
bool      nats_glue_auth_error(void);        // the server refused our credentials (connect or later)
const char* nats_glue_server_error(void);    // last -ERR text from the server

#ifdef __cplusplus
}
#endif

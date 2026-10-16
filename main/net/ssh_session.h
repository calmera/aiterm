// SSH client session (libssh2 over lwIP, password auth) with a shell channel on a PTY.
// Runs its own pump task; incoming bytes are delivered through a callback (any task).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SSH_ST_IDLE = 0, SSH_ST_CONNECTING, SSH_ST_AUTH, SSH_ST_OPEN, SSH_ST_CLOSED, SSH_ST_ERROR } ssh_state_t;

typedef struct {
    void (*on_data)(const uint8_t* data, size_t len, void* user);   // remote -> terminal
    void (*on_state)(ssh_state_t state, const char* message, void* user);
    void* user;
} ssh_callbacks_t;

// Start connecting asynchronously. cols/rows are the initial PTY size.
esp_err_t ssh_session_start(const char* host, uint16_t port, const char* user, const char* password,
                            int cols, int rows, const ssh_callbacks_t* cb);
void      ssh_session_send(const uint8_t* data, size_t len);  // terminal -> remote
void      ssh_session_resize(int cols, int rows);
void      ssh_session_close(void);
ssh_state_t ssh_session_state(void);
bool      ssh_session_available(void);  // false when built without CONFIG_AITERM_SSH

#ifdef __cplusplus
}
#endif

// Streaming HTTP(S) client on top of esp_http_client: response headers and body chunks are
// delivered through callbacks as they arrive, with cooperative abort. Target only.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const char* name; const char* value; } http_header_t;

typedef struct {
    const char*          url;
    const char*          method;        // "GET" or "POST"
    const http_header_t* headers;
    int                  n_headers;
    const char*          body;          // may be NULL
    size_t               body_len;
    int                  connect_timeout_ms;  // default 15000
    int                  read_timeout_ms;     // per-read timeout, default 30000
    int                  total_timeout_ms;    // 0 = unlimited
    void (*on_header)(const char* key, const char* value, void* user);  // may be NULL
    void (*on_data)(const char* data, size_t len, void* user);          // may be NULL
    void*                user;
    volatile bool*       abort_flag;    // may be NULL; polled between reads
} http_stream_req_t;

typedef struct {
    int    status;             // HTTP status (0 if the connection failed)
    char   content_type[96];
    char   error[160];         // human readable failure reason
    size_t bytes;              // body bytes delivered
    bool   aborted;
} http_stream_res_t;

// Runs the request to completion (blocking). Returns ESP_OK when a response was received
// (status may still be >= 400), ESP_ERR_* otherwise with res->error filled in.
esp_err_t http_stream_run(const http_stream_req_t* req, http_stream_res_t* res);

// Convenience: collect the whole body into a malloc'd (plat_malloc) NUL-terminated string.
// Caller frees *out_body. max_len caps the body size (0 = 1 MiB).
esp_err_t http_fetch(const http_stream_req_t* req, http_stream_res_t* res, char** out_body, size_t max_len);

#ifdef __cplusplus
}
#endif

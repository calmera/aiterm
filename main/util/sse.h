// Incremental Server-Sent-Events parser. Feed arbitrary byte chunks; get one callback per
// event with the concatenated multi-line `data:` payload. Host-tested (host_tests/test_sse.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "jsonbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

// event: value of the `event:` field ("" if none). data: joined data lines (no trailing \n).
typedef void (*sse_event_cb_t)(const char* event, const char* data, void* user);

typedef struct {
    jsonbuf_t      line;      // partial line being assembled
    jsonbuf_t      data;      // accumulated data: lines of the current event
    char           event[64];
    bool           have_data;
    bool           saw_cr;
    sse_event_cb_t cb;
    void*          user;
    size_t         max_event_bytes;  // guard against runaway events (0 = unlimited)
    bool           overflow;
} sse_parser_t;

void sse_init(sse_parser_t* p, sse_event_cb_t cb, void* user);
void sse_free(sse_parser_t* p);
void sse_reset(sse_parser_t* p);
// Feed n bytes. Callbacks fire synchronously for every complete event.
void sse_feed(sse_parser_t* p, const char* buf, size_t n);
// Flush a trailing event that was not terminated by a blank line (end of stream).
void sse_finish(sse_parser_t* p);

#ifdef __cplusplus
}
#endif

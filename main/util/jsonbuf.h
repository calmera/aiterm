// Growable byte buffer (PSRAM on target). Used for HTTP bodies, JSON documents, SSE
// accumulation and scrollback text. Always NUL-terminated.
#pragma once
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} jsonbuf_t;

void  jb_init(jsonbuf_t* b);
void  jb_free(jsonbuf_t* b);
void  jb_clear(jsonbuf_t* b);
bool  jb_reserve(jsonbuf_t* b, size_t extra);
bool  jb_append(jsonbuf_t* b, const char* s, size_t n);
bool  jb_append_str(jsonbuf_t* b, const char* s);
bool  jb_append_char(jsonbuf_t* b, char c);
bool  jb_appendf(jsonbuf_t* b, const char* fmt, ...);
// Append s as a JSON string literal including the surrounding quotes, escaping as needed.
bool  jb_append_json_string(jsonbuf_t* b, const char* s);
// Detach the buffer (caller owns the returned pointer; buffer becomes empty).
char* jb_detach(jsonbuf_t* b);
// Remove the first n bytes.
void  jb_consume(jsonbuf_t* b, size_t n);
static inline const char* jb_cstr(const jsonbuf_t* b) { return b->data ? b->data : ""; }

#ifdef __cplusplus
}
#endif

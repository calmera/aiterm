#include "jsonbuf.h"
#include <stdio.h>
#include <string.h>
#include "platform.h"

void jb_init(jsonbuf_t* b) { b->data = NULL; b->len = 0; b->cap = 0; }
void jb_free(jsonbuf_t* b) { plat_free(b->data); jb_init(b); }
void jb_clear(jsonbuf_t* b) { b->len = 0; if (b->data) b->data[0] = 0; }

bool jb_reserve(jsonbuf_t* b, size_t extra) {
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return true;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < need) ncap = ncap < (1u << 20) ? ncap * 2 : ncap + (1u << 20);
    char* nd = (char*)plat_realloc(b->data, ncap);
    if (!nd) return false;
    b->data = nd; b->cap = ncap;
    if (b->len == 0) b->data[0] = 0;
    return true;
}
bool jb_append(jsonbuf_t* b, const char* s, size_t n) {
    if (n == 0) return jb_reserve(b, 0);
    if (!jb_reserve(b, n)) return false;
    memcpy(b->data + b->len, s, n);
    b->len += n; b->data[b->len] = 0;
    return true;
}
bool jb_append_str(jsonbuf_t* b, const char* s) { return s ? jb_append(b, s, strlen(s)) : true; }
bool jb_append_char(jsonbuf_t* b, char c) { return jb_append(b, &c, 1); }
bool jb_appendf(jsonbuf_t* b, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return false; }
    if (!jb_reserve(b, (size_t)n)) { va_end(ap2); return false; }
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return true;
}
bool jb_append_json_string(jsonbuf_t* b, const char* s) {
    if (!jb_append_char(b, '"')) return false;
    if (s) {
        for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
            switch (*p) {
                case '"':  if (!jb_append_str(b, "\\\"")) return false; break;
                case '\\': if (!jb_append_str(b, "\\\\")) return false; break;
                case '\n': if (!jb_append_str(b, "\\n")) return false; break;
                case '\r': if (!jb_append_str(b, "\\r")) return false; break;
                case '\t': if (!jb_append_str(b, "\\t")) return false; break;
                default:
                    if (*p < 0x20) { if (!jb_appendf(b, "\\u%04x", *p)) return false; }
                    else if (!jb_append_char(b, (char)*p)) return false;
            }
        }
    }
    return jb_append_char(b, '"');
}
char* jb_detach(jsonbuf_t* b) {
    char* d = b->data;
    if (!d) { d = (char*)plat_malloc(1); if (d) d[0] = 0; }
    jb_init(b);
    return d;
}
void jb_consume(jsonbuf_t* b, size_t n) {
    if (n >= b->len) { jb_clear(b); return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n; b->data[b->len] = 0;
}

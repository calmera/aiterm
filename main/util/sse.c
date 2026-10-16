#include "sse.h"
#include <string.h>
#include "platform.h"

void sse_init(sse_parser_t* p, sse_event_cb_t cb, void* user) {
    memset(p, 0, sizeof(*p));
    jb_init(&p->line);
    jb_init(&p->data);
    p->cb = cb;
    p->user = user;
    p->max_event_bytes = 2 * 1024 * 1024;
}
void sse_free(sse_parser_t* p) { jb_free(&p->line); jb_free(&p->data); }
void sse_reset(sse_parser_t* p) {
    jb_clear(&p->line); jb_clear(&p->data);
    p->event[0] = 0; p->have_data = false; p->saw_cr = false; p->overflow = false;
}

static void dispatch(sse_parser_t* p) {
    if (p->have_data && !p->overflow) {
        // strip one trailing newline added by the joiner
        if (p->data.len > 0 && p->data.data[p->data.len - 1] == '\n') { p->data.len--; p->data.data[p->data.len] = 0; }
        if (p->cb) p->cb(p->event, jb_cstr(&p->data), p->user);
    }
    jb_clear(&p->data);
    p->event[0] = 0;
    p->have_data = false;
    p->overflow = false;
}

static void handle_line(sse_parser_t* p, const char* l, size_t n) {
    if (n == 0) { dispatch(p); return; }
    if (l[0] == ':') return;  // comment
    const char* colon = memchr(l, ':', n);
    const char* field = l; size_t flen = colon ? (size_t)(colon - l) : n;
    const char* val = colon ? colon + 1 : l + n;
    size_t vlen = colon ? n - flen - 1 : 0;
    if (vlen > 0 && val[0] == ' ') { val++; vlen--; }
    if (flen == 4 && memcmp(field, "data", 4) == 0) {
        if (p->max_event_bytes && p->data.len + vlen > p->max_event_bytes) { p->overflow = true; return; }
        jb_append(&p->data, val, vlen);
        jb_append_char(&p->data, '\n');
        p->have_data = true;
    } else if (flen == 5 && memcmp(field, "event", 5) == 0) {
        size_t c = vlen < sizeof(p->event) - 1 ? vlen : sizeof(p->event) - 1;
        memcpy(p->event, val, c); p->event[c] = 0;
    }
    // id: and retry: are ignored
}

void sse_feed(sse_parser_t* p, const char* buf, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        if (p->saw_cr) {
            p->saw_cr = false;
            if (c == '\n') continue;  // CRLF
        }
        if (c == '\r' || c == '\n') {
            if (c == '\r') p->saw_cr = true;
            handle_line(p, jb_cstr(&p->line), p->line.len);
            jb_clear(&p->line);
        } else {
            if (p->max_event_bytes && p->line.len > p->max_event_bytes) { p->overflow = true; continue; }
            jb_append_char(&p->line, c);
        }
    }
}

void sse_finish(sse_parser_t* p) {
    if (p->line.len > 0) { handle_line(p, jb_cstr(&p->line), p->line.len); jb_clear(&p->line); }
    if (p->have_data) dispatch(p);
}

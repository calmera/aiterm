#include "test_util.h"
#include "util/sse.h"

typedef struct { char events[16][64]; char data[16][512]; int n; } collector_t;
static void on_event(const char* ev, const char* data, void* user) {
    collector_t* c = (collector_t*)user;
    if (c->n < 16) { snprintf(c->events[c->n], 64, "%s", ev); snprintf(c->data[c->n], 512, "%s", data); c->n++; }
}

int main(void) {
    collector_t c = {0};
    sse_parser_t p;
    sse_init(&p, on_event, &c);
    // basic event
    const char* s1 = "data: {\"a\":1}\n\n";
    sse_feed(&p, s1, strlen(s1));
    CHECK_INT(c.n, 1);
    CHECK_STR(c.data[0], "{\"a\":1}");
    CHECK_STR(c.events[0], "");
    // multi-line data joined by \n, event name, comment, id ignored, CRLF
    const char* s2 = ": keep-alive\r\nevent: message_delta\r\nid: 7\r\ndata: line1\r\ndata: line2\r\n\r\n";
    sse_feed(&p, s2, strlen(s2));
    CHECK_INT(c.n, 2);
    CHECK_STR(c.events[1], "message_delta");
    CHECK_STR(c.data[1], "line1\nline2");
    // split across arbitrary read boundaries
    const char* s3 = "data: {\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}\n\ndata: [DONE]\n\n";
    size_t L = strlen(s3);
    for (size_t i = 0; i < L; i += 3) sse_feed(&p, s3 + i, (i + 3 <= L) ? 3 : L - i);
    CHECK_INT(c.n, 4);
    CHECK_STR(c.data[2], "{\"choices\":[{\"delta\":{\"content\":\"Hel\"}}]}");
    CHECK_STR(c.data[3], "[DONE]");
    // data without space after colon, and 'data' with empty value
    const char* s4 = "data:x\ndata\n\n";
    sse_feed(&p, s4, strlen(s4));
    CHECK_INT(c.n, 5);
    CHECK_STR(c.data[4], "x\n");
    // event without terminating blank line is flushed by finish
    const char* s5 = "data: tail";
    sse_feed(&p, s5, strlen(s5));
    CHECK_INT(c.n, 5);
    sse_finish(&p);
    CHECK_INT(c.n, 6);
    CHECK_STR(c.data[5], "tail");
    // blank lines without data produce no event
    sse_feed(&p, "\n\n\n", 3);
    CHECK_INT(c.n, 6);
    // event name resets after dispatch
    const char* s6 = "data: a\n\n";
    sse_feed(&p, s6, strlen(s6));
    CHECK_STR(c.events[6], "");
    sse_free(&p);
    TEST_MAIN_END("test_sse");
}

#include "test_util.h"
#include "ui/vt_screen.h"

static void feed(vt_screen_t* t, const char* s) { vt_feed(t, s, strlen(s)); }
static const char* row(vt_screen_t* t, int r) { static char buf[256]; vt_row_text(t, r, buf, sizeof(buf)); return buf; }

int main(void) {
    vt_screen_t t;
    CHECK(vt_init(&t, 10, 5, 0xE6E6E6, 0x111111));
    feed(&t, "hello");
    CHECK_STR(row(&t, 0), "hello"); CHECK_INT(t.cx, 5); CHECK_INT(t.cy, 0);
    // wrap at column 10 (deferred wrap)
    feed(&t, "worldXY");
    CHECK_STR(row(&t, 0), "helloworld"); CHECK_STR(row(&t, 1), "XY"); CHECK_INT(t.cy, 1);
    // CR LF, cursor moves
    feed(&t, "\r\nabc\x1b[2A\x1b[3C!");  // up 2 -> row 0, right 3 -> col 3 (cx was 3 after "abc")
    CHECK_INT(t.cy, 0); CHECK_INT(t.cx, 7);
    CHECK_STR(row(&t, 0), "hellow!rld");
    // CUP + EL
    feed(&t, "\x1b[2;1Hzz\x1b[K");
    CHECK_STR(row(&t, 1), "zz"); CHECK_INT(t.cy, 1); CHECK_INT(t.cx, 2);
    // ED 2 clears everything, cursor stays
    feed(&t, "\x1b[2J");
    CHECK_STR(row(&t, 0), ""); CHECK_INT(t.cy, 1);
    // SGR colours and attributes
    feed(&t, "\x1b[H\x1b[1;31mR\x1b[0m\x1b[38;5;196mG\x1b[48;2;1;2;3mB\x1b[0m");
    const vt_cell_t* c = vt_cell(&t, 0, 0);
    CHECK_INT(c->cp, 'R'); CHECK(c->attr & VT_ATTR_BOLD); CHECK_INT(c->fg, 0xCD3131);
    c = vt_cell(&t, 0, 1); CHECK_INT(c->fg, vt_palette(196)); CHECK_INT(c->attr, 0);
    c = vt_cell(&t, 0, 2); CHECK_INT(c->bg, 0x010203);
    c = vt_cell(&t, 0, 3); CHECK_INT(c->cp, 0);
    // scroll region: rows 2-4 (1-based), fill and scroll
    feed(&t, "\x1b[2J\x1b[2;4r\x1b[H");
    CHECK_INT(t.cy, 0);
    feed(&t, "\x1b[2;1Ha\r\nb\r\nc\r\nd");  // d causes scroll inside region
    CHECK_STR(row(&t, 0), "");
    CHECK_STR(row(&t, 1), "b"); CHECK_STR(row(&t, 2), "c"); CHECK_STR(row(&t, 3), "d"); CHECK_STR(row(&t, 4), "");
    CHECK_INT(t.cy, 3);
    // reverse index at top of region scrolls down
    feed(&t, "\x1b[2;1H\x1bM");
    CHECK_STR(row(&t, 1), ""); CHECK_STR(row(&t, 2), "b"); CHECK_STR(row(&t, 4), "");
    feed(&t, "\x1b[r");  // reset region
    CHECK_INT(t.scroll_top, 0); CHECK_INT(t.scroll_bot, 4);
    // insert/delete line and chars
    feed(&t, "\x1b[2J\x1b[H" "12345\r\n" "abcde\x1b[1;1H\x1b[L");
    CHECK_STR(row(&t, 0), ""); CHECK_STR(row(&t, 1), "12345"); CHECK_STR(row(&t, 2), "abcde");
    feed(&t, "\x1b[2;2H\x1b[2P");
    CHECK_STR(row(&t, 1), "145");
    feed(&t, "\x1b[2;2H\x1b[2@");
    CHECK_STR(row(&t, 1), "1  45");
    feed(&t, "\x1b[3;1H\x1b[M");
    CHECK_STR(row(&t, 2), "");
    // tabs
    feed(&t, "\x1b[2J\x1b[H\tx");
    CHECK_INT(vt_cell(&t, 0, 8)->cp, 'x');
    // DSR cursor position report
    feed(&t, "\x1b[3;4H\x1b[6n");
    char resp[64]; size_t rl = vt_take_response(&t, resp, sizeof(resp) - 1); resp[rl] = 0;
    CHECK_STR(resp, "\x1b[3;4R");
    // DA
    feed(&t, "\x1b[c");
    rl = vt_take_response(&t, resp, sizeof(resp) - 1); resp[rl] = 0;
    CHECK(strncmp(resp, "\x1b[?", 3) == 0);
    // UTF-8 split across feeds, OSC title
    vt_feed(&t, "\x1b[2J\x1b[H\xC3", 8); vt_feed(&t, "\xA9" "e", 2);
    CHECK_STR(row(&t, 0), "ée");
    feed(&t, "\x1b]0;my title\x07");
    CHECK_STR(t.title, "my title");
    // cursor hide/show, autowrap off
    feed(&t, "\x1b[?25l"); CHECK(!t.cursor_visible); feed(&t, "\x1b[?25h"); CHECK(t.cursor_visible);
    feed(&t, "\x1b[?7l\x1b[1;1H0123456789ABC");
    CHECK_STR(row(&t, 0), "012345678C"); CHECK_INT(t.cy, 0);
    feed(&t, "\x1b[?7h");
    // save/restore cursor
    feed(&t, "\x1b[4;5H\x1b" "7\x1b[H\x1b" "8");
    CHECK_INT(t.cy, 3); CHECK_INT(t.cx, 4);
    // dirty tracking
    vt_clear_dirty(&t);
    feed(&t, "\x1b[5;1Hq");
    CHECK(vt_row_dirty(&t, 4)); CHECK(!vt_row_dirty(&t, 0));
    // resize keeps content
    CHECK(vt_resize(&t, 20, 6));
    CHECK_STR(row(&t, 4), "q"); CHECK_INT(t.cols, 20);
    vt_free(&t);
    TEST_MAIN_END("test_vt");
}

// VT100/ANSI terminal emulation into a cell grid (SSH mode). Host-tested (host_tests/test_vt.c).
// Supports cursor movement, erase, insert/delete, scrolling regions, autowrap, tab stops,
// SGR (bold/dim/italic/underline/inverse, 16 + 256 + truecolor), DSR/DA responses.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VT_ATTR_BOLD      0x01
#define VT_ATTR_DIM       0x02
#define VT_ATTR_ITALIC    0x04
#define VT_ATTR_UNDERLINE 0x08
#define VT_ATTR_INVERSE   0x10

typedef struct {
    uint32_t cp;    // Unicode code point (0 = empty)
    uint32_t fg;    // 0x00RRGGBB
    uint32_t bg;
    uint8_t  attr;
} vt_cell_t;

typedef struct {
    vt_cell_t* cells;
    int        cols, rows;
    int        cx, cy;              // cursor (0-based)
    int        scroll_top, scroll_bot;
    uint32_t   def_fg, def_bg;
    uint32_t   cur_fg, cur_bg;
    uint8_t    cur_attr;
    bool       wrap_pending;
    bool       autowrap;
    bool       origin_mode;
    bool       cursor_visible;
    bool       app_cursor_keys;
    bool       insert_mode;
    int        saved_cx, saved_cy;
    uint32_t   saved_fg, saved_bg;
    uint8_t    saved_attr;
    uint8_t*   tabstops;
    bool*      dirty;               // per row
    // parser
    int        state;
    int        params[16];
    int        nparams;
    bool       priv;                // '?' prefix
    char       inter;
    char       osc[128];
    int        osclen;
    uint32_t   utf8_cp;
    int        utf8_need;
    char       response[64];
    int        resp_len;
    char       title[64];
    bool       bell;
} vt_screen_t;

bool   vt_init(vt_screen_t* t, int cols, int rows, uint32_t def_fg, uint32_t def_bg);
void   vt_free(vt_screen_t* t);
bool   vt_resize(vt_screen_t* t, int cols, int rows);
void   vt_reset(vt_screen_t* t);
void   vt_feed(vt_screen_t* t, const char* data, size_t n);
const vt_cell_t* vt_cell(const vt_screen_t* t, int row, int col);
bool   vt_row_dirty(const vt_screen_t* t, int row);
void   vt_clear_dirty(vt_screen_t* t);
void   vt_mark_all_dirty(vt_screen_t* t);
// Bytes the terminal wants sent back to the host (cursor position reports etc.).
size_t vt_take_response(vt_screen_t* t, char* out, size_t n);
// Row as UTF-8 text with trailing spaces trimmed (for tests/logging).
void   vt_row_text(const vt_screen_t* t, int row, char* buf, size_t n);
// xterm 256-colour palette lookup.
uint32_t vt_palette(int index);

#ifdef __cplusplus
}
#endif

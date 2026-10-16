// Generic scrollable monospace text view (used by code, diff and table views). Lines may carry
// LVGL recolor commands ("#RRGGBB text#"); '#' in content must be escaped as "##".
#pragma once
#include "ui.h"

typedef struct {
    ui_view_t*  view;
    lv_obj_t*   parent;
    lv_obj_t**  rows;
    lv_obj_t**  row_bg;      // optional per-row background rectangles
    int         nrows;       // visible rows
    char**      lines;       // owned copies
    uint32_t*   line_bg;     // 0 = none, else 0x01RRGGBB
    int         nlines;
    int         top;         // first visible line
    int         hoff;        // horizontal offset in columns
    int         max_cols;    // longest line (code points, approx, ignoring recolor cmds)
    int         y0;          // top of first row
} textview_t;

textview_t* textview_create(ui_view_t* v, lv_obj_t* parent, int y0);
void        textview_destroy(textview_t* t);
// Takes ownership of lines[] strings (plat_malloc'd). bg may be NULL.
void        textview_set_lines(textview_t* t, char** lines, uint32_t* bg, int n);
void        textview_refresh(textview_t* t);
// Handles ↑↓ PgUp PgDn Home End ←→ ; returns true if consumed.
bool        textview_key(textview_t* t, const ui_key_t* k);
int         textview_visible_rows(const textview_t* t);
// Escape '#' for recolor labels (returns plat_malloc'd string).
char*       textview_escape(const char* s);
// Hex colour string "RRGGBB" for a theme colour.
void        textview_hex(lv_color_t c, char out[7]);

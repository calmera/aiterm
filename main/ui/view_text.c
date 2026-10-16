#include "view_text.h"
#include <stdio.h>
#include <string.h>
#include "util/jsonbuf.h"
#include "util/platform.h"

void textview_hex(lv_color_t c, char out[7]) { snprintf(out, 7, "%02X%02X%02X", c.red, c.green, c.blue); }

char* textview_escape(const char* s) {
    jsonbuf_t j; jb_init(&j);
    for (const char* p = s ? s : ""; *p; p++) { if (*p == '#') jb_append_str(&j, "##"); else jb_append_char(&j, *p); }
    return jb_detach(&j);
}

// visible length in columns, skipping recolor commands
static int visible_cols(const char* s) {
    int n = 0; bool in_cmd = false;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '#') {
            if (p[1] == '#') { n++; p++; continue; }
            if (!in_cmd) { in_cmd = true; p += 6; if (p[1] == ' ') p++; continue; }
            in_cmd = false; continue;
        }
        if ((*p & 0xC0) != 0x80) n++;
    }
    return n;
}

// Return a plat_malloc'd substring starting at visible column `from`, preserving an active
// recolor command across the cut.
static char* cut_cols(const char* s, int from) {
    jsonbuf_t j; jb_init(&j);
    int col = 0; bool in_cmd = false; char cmd[8] = "";
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p == '#') {
            if (p[1] == '#') { if (col >= from) jb_append_str(&j, "##"); col++; p += 2; continue; }
            if (!in_cmd) { in_cmd = true; snprintf(cmd, sizeof(cmd), "%.6s", (const char*)p + 1); if (col >= from) { jb_append_char(&j, '#'); jb_append_str(&j, cmd); jb_append_char(&j, ' '); } p += 7; if (*p == ' ') p++; continue; }
            in_cmd = false; if (col >= from) jb_append_char(&j, '#'); p++; continue;
        }
        int len = 1; if ((*p & 0xE0) == 0xC0) len = 2; else if ((*p & 0xF0) == 0xE0) len = 3; else if ((*p & 0xF8) == 0xF0) len = 4;
        if (col == from && in_cmd && j.len == 0) { jb_append_char(&j, '#'); jb_append_str(&j, cmd); jb_append_char(&j, ' '); }
        if (col >= from) jb_append(&j, (const char*)p, len);
        col++; p += len;
    }
    if (in_cmd && j.len) jb_append_char(&j, '#');
    return jb_detach(&j);
}

textview_t* textview_create(ui_view_t* v, lv_obj_t* parent, int y0) {
    textview_t* t = (textview_t*)plat_calloc(1, sizeof(textview_t));
    t->view = v; t->parent = parent; t->y0 = y0;
    int lh = ui_line_h();
    int avail = lv_obj_get_height(parent) - y0;
    t->nrows = avail / lh;
    if (t->nrows < 1) t->nrows = 1;
    t->rows = (lv_obj_t**)plat_calloc(t->nrows, sizeof(lv_obj_t*));
    t->row_bg = (lv_obj_t**)plat_calloc(t->nrows, sizeof(lv_obj_t*));
    const ui_theme_t* th = ui_theme();
    for (int i = 0; i < t->nrows; i++) {
        t->row_bg[i] = lv_obj_create(parent);
        ui_style_plain(t->row_bg[i], th->bg);
        lv_obj_set_size(t->row_bg[i], ui_screen_w(), lh);
        lv_obj_set_pos(t->row_bg[i], 0, y0 + i * lh);
        lv_obj_add_flag(t->row_bg[i], LV_OBJ_FLAG_HIDDEN);
        t->rows[i] = ui_mono_label(parent, ui_font(), th->fg);
        lv_label_set_recolor(t->rows[i], true);
        lv_obj_set_pos(t->rows[i], 4, y0 + i * lh);
        lv_obj_set_width(t->rows[i], ui_screen_w() - 8);
    }
    return t;
}
void textview_destroy(textview_t* t) {
    if (!t) return;
    for (int i = 0; i < t->nlines; i++) plat_free(t->lines[i]);
    plat_free(t->lines); plat_free(t->line_bg); plat_free(t->rows); plat_free(t->row_bg);
    plat_free(t);
}
void textview_set_lines(textview_t* t, char** lines, uint32_t* bg, int n) {
    for (int i = 0; i < t->nlines; i++) plat_free(t->lines[i]);
    plat_free(t->lines); plat_free(t->line_bg);
    t->lines = lines; t->nlines = n; t->line_bg = bg;
    t->top = 0; t->hoff = 0; t->max_cols = 0;
    for (int i = 0; i < n; i++) { int c = visible_cols(lines[i]); if (c > t->max_cols) t->max_cols = c; }
    textview_refresh(t);
}
int textview_visible_rows(const textview_t* t) { return t->nrows; }
void textview_refresh(textview_t* t) {
    int maxtop = t->nlines - t->nrows; if (maxtop < 0) maxtop = 0;
    if (t->top > maxtop) t->top = maxtop;
    if (t->top < 0) t->top = 0;
    if (t->hoff < 0) t->hoff = 0;
    for (int i = 0; i < t->nrows; i++) {
        int li = t->top + i;
        if (li >= t->nlines) { lv_label_set_text(t->rows[i], ""); lv_obj_add_flag(t->row_bg[i], LV_OBJ_FLAG_HIDDEN); continue; }
        if (t->hoff > 0) { char* c = cut_cols(t->lines[li], t->hoff); lv_label_set_text(t->rows[i], c); plat_free(c); }
        else lv_label_set_text(t->rows[i], t->lines[li]);
        uint32_t bg = t->line_bg ? t->line_bg[li] : 0;
        if (bg) { lv_obj_set_style_bg_color(t->row_bg[i], lv_color_hex(bg & 0xFFFFFF), 0); lv_obj_clear_flag(t->row_bg[i], LV_OBJ_FLAG_HIDDEN); }
        else lv_obj_add_flag(t->row_bg[i], LV_OBJ_FLAG_HIDDEN);
    }
}
bool textview_key(textview_t* t, const ui_key_t* k) {
    if (!k->is_nav) {
        if (k->ascii == ' ') { t->top += t->nrows - 1; textview_refresh(t); return true; }
        return false;
    }
    int page = t->nrows - 1; if (page < 1) page = 1;
    switch (k->nav) {
        case UI_NAV_UP: t->top--; break;
        case UI_NAV_DOWN: t->top++; break;
        case UI_NAV_PGUP: t->top -= page; break;
        case UI_NAV_PGDN: t->top += page; break;
        case UI_NAV_HOME: if (k->mods & UI_MOD_SHIFT) t->hoff = 0; else t->top = 0; break;
        case UI_NAV_END: t->top = t->nlines; break;
        case UI_NAV_LEFT: t->hoff -= 8; break;
        case UI_NAV_RIGHT: if (t->hoff + 8 < t->max_cols) t->hoff += 8; break;
        default: return false;
    }
    textview_refresh(t);
    return true;
}

#include "view_list.h"
#include <stdio.h>
#include <string.h>
#include "util/platform.h"

#define MAX_ROWS 64
#define MAX_VISIBLE 48

static ui_view_t     s_view;
static char*         s_rows[MAX_ROWS];
static int           s_n = 0, s_cur = 0, s_top = 0, s_visible = 0;
static lv_obj_t*     s_lbl[MAX_VISIBLE];
static lv_obj_t*     s_bg[MAX_VISIBLE];
static view_list_cb_t s_cb = NULL;
static void*         s_user = NULL;
static bool          s_done = false;

static void render(void) {
    const ui_theme_t* t = ui_theme();
    if (s_cur < s_top) s_top = s_cur;
    if (s_cur >= s_top + s_visible) s_top = s_cur - s_visible + 1;
    for (int r = 0; r < s_visible; r++) {
        int i = s_top + r;
        if (i >= s_n) { lv_label_set_text(s_lbl[r], ""); lv_obj_add_flag(s_bg[r], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_label_set_text(s_lbl[r], s_rows[i]);
        bool cur = i == s_cur;
        lv_obj_set_style_text_color(s_lbl[r], cur ? t->sel_fg : t->fg, 0);
        if (cur) lv_obj_clear_flag(s_bg[r], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_bg[r], LV_OBJ_FLAG_HIDDEN);
    }
}
static void finish(int index) {
    if (s_done) return;
    s_done = true;
    view_list_cb_t cb = s_cb; void* user = s_user;
    ui_pop();
    if (cb) cb(index, user);
}
static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (!k->is_nav) {
        if (k->ascii == ' ') finish(s_cur);
        else if (k->ascii >= '1' && k->ascii <= '9' && (k->ascii - '1') < s_n) finish(k->ascii - '1');
        return;
    }
    switch (k->nav) {
        case UI_NAV_ESC: finish(-1); return;
        case UI_NAV_UP: s_cur = s_cur > 0 ? s_cur - 1 : s_n - 1; break;
        case UI_NAV_DOWN: s_cur = (s_cur + 1) % s_n; break;
        case UI_NAV_PGUP: s_cur -= s_visible; if (s_cur < 0) s_cur = 0; break;
        case UI_NAV_PGDN: s_cur += s_visible; if (s_cur >= s_n) s_cur = s_n - 1; break;
        case UI_NAV_HOME: s_cur = 0; break;
        case UI_NAV_END: s_cur = s_n - 1; break;
        case UI_NAV_ENTER: case UI_NAV_RIGHT: finish(s_cur); return;
        default: return;
    }
    render();
}
static void on_close(ui_view_t* v) {
    (void)v;
    for (int i = 0; i < s_n; i++) plat_free(s_rows[i]);
    s_n = 0;
    if (!s_done) { s_done = true; if (s_cb) s_cb(-1, s_user); }
}

void view_list_show(const char* title, const char* status, const char* const* rows, int n, int initial, view_list_cb_t cb, void* user) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "list";
    s_view.on_key = on_key;
    s_view.on_close = on_close;
    s_cb = cb; s_user = user; s_done = false;
    s_n = 0;
    for (int i = 0; i < n && i < MAX_ROWS; i++) s_rows[s_n++] = plat_strdup(rows[i] ? rows[i] : "");
    if (s_n == 0) s_rows[s_n++] = plat_strdup("(nothing here yet)");
    s_cur = initial >= 0 && initial < s_n ? initial : 0; s_top = 0;
    lv_obj_t* c = ui_view_create_root(&s_view, status ? status : "↑↓ move · Enter pick · Esc back");
    const ui_theme_t* t = ui_theme();
    int lh = ui_line_h();
    lv_obj_t* p = ui_mono_label(c, ui_font_bold(), t->fg);
    lv_obj_set_pos(p, 12, 10);
    lv_label_set_text(p, title ? title : "");
    int y = 10 + lh + lh / 2;
    int avail = lv_obj_get_height(c) - y - 4;
    s_visible = avail / (lh + 2);
    if (s_visible > MAX_VISIBLE) s_visible = MAX_VISIBLE;
    if (s_visible < 1) s_visible = 1;
    for (int r = 0; r < s_visible; r++) {
        s_bg[r] = lv_obj_create(c);
        ui_style_plain(s_bg[r], t->sel_bg);
        lv_obj_set_size(s_bg[r], ui_screen_w() - 16, lh + 2);
        lv_obj_set_pos(s_bg[r], 8, y + r * (lh + 2));
        lv_obj_add_flag(s_bg[r], LV_OBJ_FLAG_HIDDEN);
        s_lbl[r] = ui_mono_label(c, ui_font(), t->fg);
        lv_obj_set_pos(s_lbl[r], 12, y + r * (lh + 2) + 1);
        lv_obj_set_width(s_lbl[r], ui_screen_w() - 24);
        lv_label_set_long_mode(s_lbl[r], LV_LABEL_LONG_CLIP);
    }
    render();
    ui_push(&s_view);
}

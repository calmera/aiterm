// render_choice: prompt + selectable list. ↑↓ move, Space toggles (multiple), Enter confirms,
// Esc cancels.
#include <stdio.h>
#include <string.h>
#include "render_tools.h"
#include "util/jsonbuf.h"
#include "util/platform.h"
#include "views.h"

#define MAX_OPTS 64

static ui_view_t s_view;
static char*     s_opts[MAX_OPTS];
static bool      s_sel[MAX_OPTS];
static int       s_n = 0, s_cur = 0, s_top = 0;
static bool      s_multi = false;
static lv_obj_t* s_rows[48];
static lv_obj_t* s_row_bg[48];
static int       s_visible = 0;
static bool      s_finished = false;

static void render(void) {
    const ui_theme_t* t = ui_theme();
    if (s_cur < s_top) s_top = s_cur;
    if (s_cur >= s_top + s_visible) s_top = s_cur - s_visible + 1;
    char buf[300];
    for (int r = 0; r < s_visible; r++) {
        int i = s_top + r;
        if (i >= s_n) { lv_label_set_text(s_rows[r], ""); lv_obj_add_flag(s_row_bg[r], LV_OBJ_FLAG_HIDDEN); continue; }
        if (s_multi) snprintf(buf, sizeof(buf), "[%c] %s", s_sel[i] ? 'x' : ' ', s_opts[i]);
        else snprintf(buf, sizeof(buf), "%2d. %s", i + 1, s_opts[i]);
        lv_label_set_text(s_rows[r], buf);
        bool cur = i == s_cur;
        lv_obj_set_style_text_color(s_rows[r], cur ? t->sel_fg : t->fg, 0);
        if (cur) lv_obj_clear_flag(s_row_bg[r], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_row_bg[r], LV_OBJ_FLAG_HIDDEN);
    }
}
static void finish(const char* json) {
    if (s_finished) return;
    s_finished = true;
    render_tool_finish(json);
    ui_pop();
}
static void confirm(void) {
    jsonbuf_t j; jb_init(&j);
    jb_append_str(&j, "{\"selected\":[");
    bool first = true;
    if (s_multi) { for (int i = 0; i < s_n; i++) if (s_sel[i]) { jb_appendf(&j, "%s%d", first ? "" : ",", i); first = false; } }
    else jb_appendf(&j, "%d", s_cur);
    jb_append_str(&j, "],\"labels\":[");
    first = true;
    if (s_multi) { for (int i = 0; i < s_n; i++) if (s_sel[i]) { if (!first) jb_append_char(&j, ','); jb_append_json_string(&j, s_opts[i]); first = false; } }
    else jb_append_json_string(&j, s_opts[s_cur]);
    jb_append_str(&j, "]}");
    char* out = jb_detach(&j);
    finish(out);
    plat_free(out);
}
static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (!k->is_nav) {
        if (k->ascii == ' ') { if (s_multi) { s_sel[s_cur] = !s_sel[s_cur]; render(); } else confirm(); }
        else if (k->ascii >= '1' && k->ascii <= '9' && !s_multi && (k->ascii - '1') < s_n) { s_cur = k->ascii - '1'; confirm(); }
        return;
    }
    switch (k->nav) {
        case UI_NAV_ESC: finish("{\"cancelled\":true}"); return;
        case UI_NAV_UP: s_cur = s_cur > 0 ? s_cur - 1 : s_n - 1; break;
        case UI_NAV_DOWN: s_cur = (s_cur + 1) % s_n; break;
        case UI_NAV_HOME: case UI_NAV_PGUP: s_cur = 0; break;
        case UI_NAV_END: case UI_NAV_PGDN: s_cur = s_n - 1; break;
        case UI_NAV_ENTER: confirm(); return;
        default: return;
    }
    render();
}
static void on_close(ui_view_t* v) {
    (void)v;
    if (!s_finished) { s_finished = true; render_tool_finish("{\"cancelled\":true}"); }
    for (int i = 0; i < s_n; i++) plat_free(s_opts[i]);
    s_n = 0;
}

void view_choice_show(const char* prompt, const cJSON* options, bool allow_multiple) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "choice";
    s_view.on_key = on_key;
    s_view.on_close = on_close;
    s_finished = false;
    s_multi = allow_multiple;
    s_n = 0; s_cur = 0; s_top = 0;
    const cJSON* o;
    cJSON_ArrayForEach(o, options) {
        if (s_n >= MAX_OPTS) break;
        char num[32];
        const char* txt = cJSON_IsString(o) ? o->valuestring : (cJSON_IsNumber(o) ? (snprintf(num, sizeof(num), "%g", o->valuedouble), num) : "(option)");
        s_opts[s_n] = plat_strdup(txt);
        s_sel[s_n] = false;
        s_n++;
    }
    lv_obj_t* c = ui_view_create_root(&s_view, allow_multiple ? "Choice · ↑↓ move · Space toggle · Enter confirm · Esc cancel" : "Choice · ↑↓ move · Enter select · Esc cancel");
    const ui_theme_t* t = ui_theme();
    int lh = ui_line_h();
    lv_obj_t* p = ui_mono_label(c, ui_font_bold(), t->fg);
    lv_label_set_long_mode(p, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(p, ui_screen_w() - 24);
    lv_obj_set_pos(p, 12, 10);
    lv_label_set_text(p, prompt);
    lv_obj_update_layout(p);
    int y = 10 + lv_obj_get_height(p) + lh;
    int avail = lv_obj_get_height(c) - y - 4;
    s_visible = avail / (lh + 2);
    if (s_visible > 48) s_visible = 48;
    if (s_visible < 1) s_visible = 1;
    for (int r = 0; r < s_visible; r++) {
        s_row_bg[r] = lv_obj_create(c);
        ui_style_plain(s_row_bg[r], t->sel_bg);
        lv_obj_set_size(s_row_bg[r], ui_screen_w() - 16, lh + 2);
        lv_obj_set_pos(s_row_bg[r], 8, y + r * (lh + 2));
        lv_obj_add_flag(s_row_bg[r], LV_OBJ_FLAG_HIDDEN);
        s_rows[r] = ui_mono_label(c, ui_font(), t->fg);
        lv_obj_set_pos(s_rows[r], 12, y + r * (lh + 2) + 1);
        lv_obj_set_width(s_rows[r], ui_screen_w() - 24);
    }
    render();
    ui_push(&s_view);
}

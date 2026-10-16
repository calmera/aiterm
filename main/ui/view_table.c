// render_table: grid with bold header, column widths from content (cap 40, ellipsized), scrolls both ways.
#include <stdio.h>
#include <string.h>
#include "render_tools.h"
#include "util/jsonbuf.h"
#include "util/platform.h"
#include "view_text.h"
#include "views.h"

static ui_view_t   s_view;
static textview_t* s_tv = NULL;

static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (k->is_nav && k->nav == UI_NAV_ESC) { render_tool_finish("{\"status\":\"shown\"}"); ui_pop(); return; }
    if (s_tv) textview_key(s_tv, k);
}
static void on_close(ui_view_t* v) { (void)v; render_tool_finish("{\"status\":\"shown\"}"); textview_destroy(s_tv); s_tv = NULL; }

static const char* cell_str(const cJSON* c, char* tmp, size_t n) {
    if (cJSON_IsString(c)) return c->valuestring;
    if (cJSON_IsNumber(c)) { snprintf(tmp, n, "%g", c->valuedouble); return tmp; }
    if (cJSON_IsBool(c)) return cJSON_IsTrue(c) ? "true" : "false";
    if (cJSON_IsNull(c) || !c) return "";
    return "…";
}
// append s padded/ellipsized to width w (in code points)
static void append_cell(jsonbuf_t* j, const char* s, int w) {
    int len = ui_utf8_len(s);
    if (len > w) {
        int off = ui_utf8_offset(s, w - 1);
        char* e = plat_strndup(s, off); char* esc = textview_escape(e); plat_free(e);
        jb_append_str(j, esc); jb_append_str(j, "…"); plat_free(esc);
    } else {
        char* esc = textview_escape(s);
        jb_append_str(j, esc); plat_free(esc);
        for (int i = len; i < w; i++) jb_append_char(j, ' ');
    }
}

void view_table_show(const cJSON* columns, const cJSON* rows, const char* title) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "table"; s_view.on_key = on_key; s_view.on_close = on_close;
    int ncols = cJSON_GetArraySize(columns);
    if (ncols > 32) ncols = 32;
    int widths[32] = {0};
    char tmp[32];
    for (int i = 0; i < ncols; i++) { int l = ui_utf8_len(cell_str(cJSON_GetArrayItem(columns, i), tmp, sizeof(tmp))); widths[i] = l; }
    const cJSON* row;
    int nrows = 0;
    cJSON_ArrayForEach(row, rows) {
        nrows++;
        for (int i = 0; i < ncols; i++) { int l = ui_utf8_len(cell_str(cJSON_IsArray(row) ? cJSON_GetArrayItem(row, i) : NULL, tmp, sizeof(tmp))); if (l > widths[i]) widths[i] = l; }
    }
    for (int i = 0; i < ncols; i++) { if (widths[i] > 40) widths[i] = 40; if (widths[i] < 1) widths[i] = 1; }
    const ui_theme_t* th = ui_theme();
    char mu[7], fg[7]; textview_hex(th->muted, mu); textview_hex(th->fg, fg);
    int n = nrows + 2;
    char** lines = (char**)plat_calloc(n, sizeof(char*));
    uint32_t* bg = (uint32_t*)plat_calloc(n, sizeof(uint32_t));
    jsonbuf_t j; jb_init(&j);
    jb_appendf(&j, "#%s ", fg);
    for (int i = 0; i < ncols; i++) { if (i) jb_append_str(&j, " │ "); append_cell(&j, cell_str(cJSON_GetArrayItem(columns, i), tmp, sizeof(tmp)), widths[i]); }
    jb_append_char(&j, '#');
    lines[0] = jb_detach(&j);
    bg[0] = 0x01000000 | (th->dark ? 0x222222 : 0xDDDDDD);
    jb_init(&j);
    jb_appendf(&j, "#%s ", mu);
    for (int i = 0; i < ncols; i++) { if (i) jb_append_str(&j, "─┼─"); for (int k = 0; k < widths[i]; k++) jb_append_str(&j, "─"); }
    jb_append_char(&j, '#');
    lines[1] = jb_detach(&j);
    int li = 2;
    cJSON_ArrayForEach(row, rows) {
        jb_init(&j);
        for (int i = 0; i < ncols; i++) { if (i) jb_appendf(&j, " #%s │# ", mu); append_cell(&j, cell_str(cJSON_IsArray(row) ? cJSON_GetArrayItem(row, i) : NULL, tmp, sizeof(tmp)), widths[i]); }
        lines[li++] = jb_detach(&j);
    }
    char status[160];
    snprintf(status, sizeof(status), "%s · %d rows · ↑↓ scroll · ←→ pan · Esc close", title ? title : "Table", nrows);
    lv_obj_t* c = ui_view_create_root(&s_view, status);
    s_tv = textview_create(&s_view, c, 4);
    textview_set_lines(s_tv, lines, bg, li);
    ui_push(&s_view);
}

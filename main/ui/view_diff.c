// render_diff: per-file sections, + green, - red, hunk headers muted. A accept, R reject, Esc = reject.
#include <stdio.h>
#include <string.h>
#include "render_tools.h"
#include "util/jsonbuf.h"
#include "util/linediff.h"
#include "util/platform.h"
#include "view_text.h"
#include "views.h"

static ui_view_t   s_view;
static textview_t* s_tv = NULL;
static bool        s_finished = false;

static void finish(const char* json) {
    if (s_finished) return;
    s_finished = true;
    render_tool_finish(json);
    ui_pop();
}
static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (k->is_nav && k->nav == UI_NAV_ESC) { finish("{\"decision\":\"rejected\"}"); return; }
    if (!k->is_nav) {
        char c = k->ascii | 0x20;
        if (c == 'a' || c == 'y') { finish("{\"decision\":\"accepted\"}"); return; }
        if (c == 'r' || c == 'n') { finish("{\"decision\":\"rejected\"}"); return; }
    }
    if (s_tv) textview_key(s_tv, k);
}
static void on_close(ui_view_t* v) {
    (void)v;
    if (!s_finished) { s_finished = true; render_tool_finish("{\"decision\":\"rejected\"}"); }
    textview_destroy(s_tv); s_tv = NULL;
}

void view_diff_show(const char* diff, const char* old_text, const char* new_text, const char* path) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "diff"; s_view.on_key = on_key; s_view.on_close = on_close;
    s_finished = false;
    diff_result_t r;
    bool ok = diff ? diff_parse_unified(diff, &r) : diff_compute(old_text ? old_text : "", new_text ? new_text : "", path, &r);
    const ui_theme_t* th = ui_theme();
    char add[7], del[7], mu[7], fg[7];
    textview_hex(th->diff_add, add); textview_hex(th->diff_del, del); textview_hex(th->muted, mu); textview_hex(th->fg, fg);
    int n = ok ? r.n : 1;
    char** lines = (char**)plat_calloc(n, sizeof(char*));
    uint32_t* bg = (uint32_t*)plat_calloc(n, sizeof(uint32_t));
    if (!ok) {
        lines[0] = plat_strdup("(could not parse diff)");
    } else {
        uint32_t add_bg = th->dark ? 0x01000000 | 0x12261A : 0x01000000 | 0xDDF4E4;
        uint32_t del_bg = th->dark ? 0x01000000 | 0x2A1215 : 0x01000000 | 0xFADCDC;
        for (int i = 0; i < r.n; i++) {
            char* e = textview_escape(r.lines[i].text);
            jsonbuf_t j; jb_init(&j);
            switch (r.lines[i].kind) {
                case DL_ADD: jb_appendf(&j, "#%s +%s#", add, e); bg[i] = add_bg; break;
                case DL_DEL: jb_appendf(&j, "#%s -%s#", del, e); bg[i] = del_bg; break;
                case DL_HUNK: jb_appendf(&j, "#%s %s#", mu, e); break;
                case DL_FILE: jb_appendf(&j, "#%s %s#", fg, e); break;
                case DL_META: jb_appendf(&j, "#%s %s#", mu, e); break;
                default: jb_appendf(&j, "  %s", e); break;
            }
            plat_free(e);
            lines[i] = jb_detach(&j);
        }
    }
    char status[160];
    snprintf(status, sizeof(status), "Diff%s%s · +%d −%d · A accept · R reject · Esc reject", path ? " " : "", path ? path : "", ok ? r.adds : 0, ok ? r.dels : 0);
    lv_obj_t* c = ui_view_create_root(&s_view, status);
    s_tv = textview_create(&s_view, c, 4);
    textview_set_lines(s_tv, lines, bg, n);
    if (ok) diff_free(&r);
    ui_push(&s_view);
}

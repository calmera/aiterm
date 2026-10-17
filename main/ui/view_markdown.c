// render_markdown: scrollable column of blocks. Paragraphs/headings/lists use lv_spangroup
// (bold runs in the bold font, inline code in the code colour); fenced code is monospace on a
// code background; tables are aligned monospace text; hr is a 1 px line.
#include <stdio.h>
#include <string.h>
#include "render_tools.h"
#include "util/jsonbuf.h"
#include "util/markdown.h"
#include "util/platform.h"
#include "view_text.h"
#include "views.h"

static ui_view_t s_view;
static lv_obj_t* s_col = NULL;   // scrollable container
static int       s_content_h = 0;

#define MARGIN 12

static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (!s_col) return;
    int lh = ui_line_h();
    int page = lv_obj_get_height(s_col) - lh;
    int32_t dy = 0;
    if (k->is_nav) {
        switch (k->nav) {
            case UI_NAV_ESC: render_tool_finish("{\"status\":\"shown\"}"); ui_pop(); return;
            case UI_NAV_UP: dy = -lh; break;
            case UI_NAV_DOWN: dy = lh; break;
            case UI_NAV_PGUP: dy = -page; break;
            case UI_NAV_PGDN: dy = page; break;
            case UI_NAV_HOME: lv_obj_scroll_to_y(s_col, 0, LV_ANIM_OFF); return;
            case UI_NAV_END: lv_obj_scroll_to_y(s_col, s_content_h, LV_ANIM_OFF); return;
            default: return;
        }
    } else if (k->ascii == ' ') dy = page;
    else return;
    lv_obj_scroll_by_bounded(s_col, 0, -dy, LV_ANIM_OFF);
}
static void on_close(ui_view_t* v) { (void)v; render_tool_finish("{\"status\":\"shown\"}"); s_col = NULL; }

static int s_last_h = 0;  // height of the block created by the last add_spangroup() call
static lv_obj_t* add_spangroup(lv_obj_t* parent, const md_block_t* b, const lv_font_t* base, const lv_font_t* bold, lv_color_t color, int x, int width, const char* prefix) {
    const ui_theme_t* th = ui_theme();
    lv_obj_t* sg = lv_spangroup_create(parent);
    lv_obj_remove_style_all(sg);
    lv_obj_set_style_text_font(sg, base, 0);
    lv_obj_set_style_text_color(sg, color, 0);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
    lv_spangroup_set_overflow(sg, LV_SPAN_OVERFLOW_CLIP);
    lv_obj_set_width(sg, width);
    if (prefix && *prefix) {
        lv_span_t* sp = lv_spangroup_new_span(sg);
        lv_span_set_text(sp, prefix);
        lv_style_set_text_color(lv_span_get_style(sp), th->muted);
        lv_style_set_text_font(lv_span_get_style(sp), base);
    }
    for (int i = 0; i < b->nruns; i++) {
        lv_span_t* sp = lv_spangroup_new_span(sg);
        lv_span_set_text(sp, b->runs[i].text);
        lv_style_t* st = lv_span_get_style(sp);
        if (b->runs[i].flags & MD_RUN_BOLD) lv_style_set_text_font(st, bold);
        else lv_style_set_text_font(st, base);
        if (b->runs[i].flags & MD_RUN_CODE) lv_style_set_text_color(st, th->str);
        else if (b->runs[i].flags & MD_RUN_ITALIC) lv_style_set_text_color(st, th->muted);
        else lv_style_set_text_color(st, color);
    }
    lv_obj_set_pos(sg, x, 0);
    // LVGL 9: spans added with lv_spangroup_new_span()/lv_span_set_text() are only measured
    // after an explicit refresh; without it get_expand_height() returned 0 on the badge and
    // every block was stacked on top of the previous one.
    lv_spangroup_refresh(sg);
    int h = lv_spangroup_get_expand_height(sg, width);
    if (h <= 0) h = lv_font_get_line_height(base);
    lv_obj_set_height(sg, h + 2);
    s_last_h = h + 2;  // lv_obj_get_height() would still return the pre-layout (0) size here
    return sg;
}

void view_markdown_show(const char* markdown, const char* title) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "markdown"; s_view.on_key = on_key; s_view.on_close = on_close;
    char status[160];
    snprintf(status, sizeof(status), "%s · ↑↓ PgUp PgDn scroll · Esc close", title ? title : "Markdown");
    lv_obj_t* c = ui_view_create_root(&s_view, status);
    const ui_theme_t* th = ui_theme();
    s_col = lv_obj_create(c);
    ui_style_plain(s_col, th->bg);
    lv_obj_set_size(s_col, ui_screen_w(), lv_obj_get_height(c));
    lv_obj_set_pos(s_col, 0, 0);
    lv_obj_set_scroll_dir(s_col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_col, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_MOMENTUM);

    int width = ui_screen_w() - 2 * MARGIN;
    int y = 8;
    int lh = ui_line_h();
    int cw = ui_char_w();
    md_block_t* blocks = md_parse(markdown);
    for (const md_block_t* b = blocks; b; b = b->next) {
        lv_obj_t* o = NULL;
        switch (b->type) {
            case MD_HEADING: {
                const lv_font_t* f = ui_font_heading(b->level);
                if (b != blocks) y += lh / 2;
                o = add_spangroup(s_col, b, f, f, th->heading, MARGIN, width, NULL);
                break;
            }
            case MD_PARAGRAPH:
                o = add_spangroup(s_col, b, ui_font(), ui_font_bold(), th->fg, MARGIN, width, NULL);
                break;
            case MD_QUOTE: {
                lv_obj_t* bar = lv_obj_create(s_col);
                ui_style_plain(bar, th->muted);
                lv_obj_set_size(bar, 2, lh);
                lv_obj_set_pos(bar, MARGIN, y);
                o = add_spangroup(s_col, b, ui_font(), ui_font_bold(), th->muted, MARGIN + cw, width - cw, NULL);
                lv_obj_set_height(bar, s_last_h);
                break;
            }
            case MD_BULLET: case MD_NUMBERED: {
                char prefix[16];
                int indent = MARGIN + b->level * 2 * cw;
                if (b->type == MD_BULLET) snprintf(prefix, sizeof(prefix), "%s ", b->level % 2 == 0 ? "•" : "◦");
                else snprintf(prefix, sizeof(prefix), "%d. ", b->number);
                o = add_spangroup(s_col, b, ui_font(), ui_font_bold(), th->fg, indent, width - (indent - MARGIN), prefix);
                break;
            }
            case MD_CODE: {
                // count lines
                int n = 1; for (const char* p = b->code; *p; p++) if (*p == '\n' && p[1]) n++;
                if (b->code[0] == 0) n = 0;
                lv_obj_t* bg = lv_obj_create(s_col);
                ui_style_plain(bg, th->code_bg);
                lv_obj_set_size(bg, width, n * lh + 8);
                lv_obj_set_pos(bg, MARGIN, y);
                lv_obj_t* l = ui_mono_label(s_col, ui_font(), th->fg);
                lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
                lv_obj_set_width(l, width - 8);
                // expand tabs, strip trailing newline
                jsonbuf_t j; jb_init(&j);
                for (const char* p = b->code; *p; p++) { if (*p == '\t') jb_append_str(&j, "    "); else if (*p != '\r') jb_append_char(&j, *p); }
                if (j.len && j.data[j.len - 1] == '\n') { j.len--; j.data[j.len] = 0; }
                lv_label_set_text(l, jb_cstr(&j));
                jb_free(&j);
                lv_obj_set_pos(l, MARGIN + 4, y + 4);
                y += n * lh + 8 + lh / 2;
                continue;
            }
            case MD_HR: {
                lv_obj_t* hr = lv_obj_create(s_col);
                ui_style_plain(hr, th->muted);
                lv_obj_set_size(hr, width, 1);
                lv_obj_set_pos(hr, MARGIN, y + lh / 2);
                y += lh;
                continue;
            }
            case MD_TABLE: {
                int widths[32] = {0};
                int nc = b->ncols > 32 ? 32 : b->ncols;
                // cells are shown as plain text: strip `code`, **bold**, [links](url)
                int ncell = b->nrows * b->ncols;
                char** plain = (char**)plat_calloc((size_t)(ncell > 0 ? ncell : 1), sizeof(char*));
                for (int ci = 0; ci < ncell; ci++) plain[ci] = md_inline_plain(b->cells[ci]);
                for (int r = 0; r < b->nrows; r++) for (int ci = 0; ci < nc; ci++) { int l = ui_utf8_len(plain[r * b->ncols + ci]); if (l > widths[ci]) widths[ci] = l > 40 ? 40 : l; }
                jsonbuf_t j; jb_init(&j);
                for (int r = 0; r < b->nrows; r++) {
                    for (int ci = 0; ci < nc; ci++) {
                        const char* s = plain[r * b->ncols + ci];
                        int len = ui_utf8_len(s);
                        if (ci) jb_append_str(&j, " │ ");
                        if (len > widths[ci]) { jb_append(&j, s, ui_utf8_offset(s, widths[ci] - 1)); jb_append_str(&j, "…"); }
                        else { jb_append_str(&j, s); for (int k = len; k < widths[ci]; k++) jb_append_char(&j, ' '); }
                    }
                    jb_append_char(&j, '\n');
                    if (r == 0) { for (int ci = 0; ci < nc; ci++) { if (ci) jb_append_str(&j, "─┼─"); for (int k = 0; k < widths[ci]; k++) jb_append_str(&j, "─"); } jb_append_char(&j, '\n'); }
                }
                if (j.len && j.data[j.len - 1] == '\n') { j.len--; j.data[j.len] = 0; }
                for (int ci = 0; ci < ncell; ci++) plat_free(plain[ci]);
                plat_free(plain);
                lv_obj_t* l = ui_mono_label(s_col, ui_font(), th->fg);
                lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
                lv_obj_set_width(l, width);
                lv_label_set_text(l, jb_cstr(&j));
                jb_free(&j);
                lv_obj_set_pos(l, MARGIN, y);
                y += (b->nrows + 1) * lh + lh / 2;
                continue;
            }
        }
        if (o) {
            lv_obj_set_y(o, y);
            y += s_last_h + (b->type == MD_BULLET || b->type == MD_NUMBERED ? 2 : lh / 2);
        }
    }
    md_free(blocks);
    s_content_h = y + 8;
    // spacer so the last line can scroll fully into view
    lv_obj_t* sp = lv_obj_create(s_col);
    ui_style_plain(sp, th->bg);
    lv_obj_set_size(sp, 1, 1);
    lv_obj_set_pos(sp, 0, s_content_h);
    ui_push(&s_view);
}

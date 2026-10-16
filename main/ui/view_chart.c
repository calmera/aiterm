// render_chart: line/bar via lv_chart, pie via lv_arc segments. Legend as plain text.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "render_tools.h"
#include "util/jsonbuf.h"
#include "util/platform.h"
#include "views.h"

static ui_view_t s_view;

static const uint32_t k_palette[] = {0x79C0FF, 0x2EA043, 0xF85149, 0xE3B341, 0xBC8CFF, 0x56D4DD, 0xFFA657, 0x8A8A8A};

static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (k->is_nav && k->nav == UI_NAV_ESC) { render_tool_finish("{\"status\":\"shown\"}"); ui_pop(); }
}
static void on_close(ui_view_t* v) { (void)v; render_tool_finish("{\"status\":\"shown\"}"); }

void view_chart_show(const char* type, const cJSON* series, const cJSON* labels, const char* title, const char* y_label) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "chart"; s_view.on_key = on_key; s_view.on_close = on_close;
    char status[160];
    snprintf(status, sizeof(status), "%s · Esc close", title ? title : "Chart");
    lv_obj_t* c = ui_view_create_root(&s_view, status);
    const ui_theme_t* th = ui_theme();
    int lh = ui_line_h();
    int ns = cJSON_GetArraySize(series);
    if (ns > 8) ns = 8;
    // find max point count and value range
    int npts = 0; double vmin = 0, vmax = 0; bool first = true;
    for (int s = 0; s < ns; s++) {
        const cJSON* vals = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(series, s), "values");
        int n = cJSON_IsArray(vals) ? cJSON_GetArraySize(vals) : 0;
        if (n > npts) npts = n;
        const cJSON* v; cJSON_ArrayForEach(v, vals) { if (!cJSON_IsNumber(v)) continue; if (first) { vmin = vmax = v->valuedouble; first = false; } if (v->valuedouble < vmin) vmin = v->valuedouble; if (v->valuedouble > vmax) vmax = v->valuedouble; }
    }
    if (npts > 256) npts = 256;
    if (first) { vmin = 0; vmax = 1; }
    if (vmin > 0) vmin = 0;
    if (vmax <= vmin) vmax = vmin + 1;
    bool pie = type && strcmp(type, "pie") == 0;
    bool bar = type && strcmp(type, "bar") == 0;
    int legend_h = (ns + 1) * lh + 8;
    int chart_w = ui_screen_w() - 40;
    int chart_h = lv_obj_get_height(c) - legend_h - 2 * lh - 20;
    if (pie) {
        // one pie per the first series; segments from its values
        const cJSON* vals = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(series, 0), "values");
        double total = 0; const cJSON* v;
        cJSON_ArrayForEach(v, vals) if (cJSON_IsNumber(v) && v->valuedouble > 0) total += v->valuedouble;
        int size = chart_h < chart_w ? chart_h : chart_w;
        if (size > 480) size = 480;
        double start = 0; int i = 0;
        jsonbuf_t leg; jb_init(&leg);
        cJSON_ArrayForEach(v, vals) {
            if (!cJSON_IsNumber(v) || v->valuedouble <= 0 || total <= 0) { i++; continue; }
            double span = v->valuedouble / total * 360.0;
            lv_obj_t* arc = lv_arc_create(c);
            lv_obj_remove_style_all(arc);
            lv_obj_set_size(arc, size, size);
            lv_obj_set_pos(arc, (ui_screen_w() - size) / 2, 10 + lh);
            lv_arc_set_rotation(arc, 270);
            lv_arc_set_bg_angles(arc, 0, 360);
            lv_arc_set_angles(arc, (lv_value_precise_t)start, (lv_value_precise_t)(start + span));
            lv_obj_set_style_arc_width(arc, size / 4, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(arc, lv_color_hex(k_palette[i % 8]), LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
            lv_obj_set_style_arc_width(arc, 0, LV_PART_MAIN);
            lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
            lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
            const cJSON* lab = labels ? cJSON_GetArrayItem(labels, i) : NULL;
            char hex[8]; snprintf(hex, sizeof(hex), "%06X", (unsigned)k_palette[i % 8]);
            jb_appendf(&leg, "#%s ■# %s  %.1f%%\n", hex, cJSON_IsString(lab) ? lab->valuestring : "", v->valuedouble / total * 100.0);
            start += span; i++;
        }
        lv_obj_t* l = ui_mono_label(c, ui_font(), th->fg);
        lv_label_set_recolor(l, true);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, ui_screen_w() - 40);
        lv_label_set_text(l, jb_cstr(&leg));
        lv_obj_set_pos(l, 20, 10 + lh + size + lh);
        jb_free(&leg);
    } else {
        lv_obj_t* chart = lv_chart_create(c);
        lv_obj_remove_style_all(chart);
        lv_obj_set_style_bg_color(chart, th->bg, 0);
        lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chart, 1, 0);
        lv_obj_set_style_border_color(chart, th->muted, 0);
        lv_obj_set_style_border_side(chart, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_line_color(chart, th->code_bg, 0);
        lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
        lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(chart, 4, 0);
        lv_obj_set_size(chart, chart_w - 60, chart_h);
        lv_obj_set_pos(chart, 60 + 10, 10 + lh);
        lv_chart_set_type(chart, bar ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
        lv_chart_set_point_count(chart, (uint32_t)npts);
        lv_chart_set_div_line_count(chart, 5, 0);
        // integer scaling: pick a factor so the range maps into int32 with 2 decimals
        double scale = 100.0;
        if (fabs(vmax) * scale > 1e9 || fabs(vmin) * scale > 1e9) scale = 1.0;
        lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)floor(vmin * scale), (int32_t)ceil(vmax * scale));
        jsonbuf_t leg; jb_init(&leg);
        for (int s = 0; s < ns; s++) {
            const cJSON* so = cJSON_GetArrayItem(series, s);
            const cJSON* vals = cJSON_GetObjectItemCaseSensitive(so, "values");
            const cJSON* name = cJSON_GetObjectItemCaseSensitive(so, "name");
            lv_chart_series_t* ser = lv_chart_add_series(chart, lv_color_hex(k_palette[s % 8]), LV_CHART_AXIS_PRIMARY_Y);
            int i = 0; const cJSON* v;
            cJSON_ArrayForEach(v, vals) { if (i++ >= npts) break; lv_chart_set_next_value(chart, ser, cJSON_IsNumber(v) ? (int32_t)lround(v->valuedouble * scale) : LV_CHART_POINT_NONE); }
            char hex[8]; snprintf(hex, sizeof(hex), "%06X", (unsigned)k_palette[s % 8]);
            jb_appendf(&leg, "#%s ━# %s\n", hex, cJSON_IsString(name) ? name->valuestring : "series");
        }
        // y axis min/max labels
        lv_obj_t* ymax = ui_mono_label(c, ui_font(), th->muted);
        lv_label_set_text_fmt(ymax, "%.6g", vmax);
        lv_obj_set_pos(ymax, 8, 10 + lh);
        lv_obj_t* ymin = ui_mono_label(c, ui_font(), th->muted);
        lv_label_set_text_fmt(ymin, "%.6g", vmin);
        lv_obj_set_pos(ymin, 8, 10 + lh + chart_h - lh);
        if (y_label) { lv_obj_t* yl = ui_mono_label(c, ui_font(), th->muted); lv_label_set_text(yl, y_label); lv_obj_set_pos(yl, 8, 10); }
        // x labels: first, middle, last
        if (labels && cJSON_IsArray(labels) && npts > 0) {
            int idx[3] = {0, npts / 2, npts - 1};
            for (int k = 0; k < 3; k++) {
                const cJSON* lab = cJSON_GetArrayItem(labels, idx[k]);
                if (!cJSON_IsString(lab)) continue;
                lv_obj_t* xl = ui_mono_label(c, ui_font(), th->muted);
                lv_label_set_text(xl, lab->valuestring);
                int x = 70 + (npts > 1 ? (chart_w - 60 - 8) * idx[k] / (npts - 1) : 0);
                if (k == 2) x -= ui_utf8_len(lab->valuestring) * ui_char_w();
                if (k == 1) x -= ui_utf8_len(lab->valuestring) * ui_char_w() / 2;
                lv_obj_set_pos(xl, x, 10 + lh + chart_h + 4);
            }
        }
        lv_obj_t* l = ui_mono_label(c, ui_font(), th->fg);
        lv_label_set_recolor(l, true);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, ui_screen_w() - 40);
        lv_label_set_text(l, jb_cstr(&leg));
        lv_obj_set_pos(l, 20, 10 + lh + chart_h + lh + 8);
        jb_free(&leg);
    }
    if (title) { lv_obj_t* t = ui_mono_label(c, ui_font_bold(), th->fg); lv_label_set_text(t, title); lv_obj_set_pos(t, 20, 6); }
    ui_push(&s_view);
}

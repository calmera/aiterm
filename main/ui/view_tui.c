#include "view_tui.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "sdkconfig.h"
#include "util/platform.h"

#define SB_LINES   CONFIG_AITERM_SCROLLBACK_LINES
#define INPUT_MAX  4096
#define HISTORY_N  32
#define MAX_ROWS   64

typedef struct { char* text; uint8_t style; } sb_line_t;

static ui_view_t*  s_view = NULL;
static lv_obj_t*   s_rows[MAX_ROWS];
static int         s_nrows = 0;
static lv_obj_t*   s_sep1 = NULL;
static lv_obj_t*   s_input = NULL;
static lv_obj_t*   s_cursor = NULL;
static lv_obj_t*   s_cursor_ch = NULL;
static lv_timer_t* s_blink = NULL;
static lv_timer_t* s_refresh = NULL;

static sb_line_t*  s_sb = NULL;    // ring buffer
static int         s_sb_head = 0;  // index of oldest
static int         s_sb_count = 0;
static bool        s_streaming = false;
static int         s_scroll = 0;   // visual rows scrolled up from the bottom
static bool        s_dirty = true;

static char        s_prompt[16] = "> ";
static char        s_in[INPUT_MAX];
static int         s_in_len = 0;    // bytes
static int         s_in_cur = 0;    // byte offset of cursor
static bool        s_in_enabled = true;
static char*       s_hist[HISTORY_N];
static int         s_hist_n = 0;
static int         s_hist_pos = -1;  // -1 = not browsing
static char        s_hist_saved[INPUT_MAX];
// System bar (top) and segmented status line (bottom)
static lv_obj_t*   s_top       = NULL;
static lv_obj_t*   s_clock     = NULL;
static lv_obj_t*   s_net_lbl   = NULL;
static lv_obj_t*   s_wifi_bar[4];
static lv_obj_t*   s_batt_out  = NULL;
static lv_obj_t*   s_batt_fill = NULL;
static lv_obj_t*   s_batt_lbl  = NULL;
static lv_obj_t*   s_status_bg = NULL;
#define STATUS_SEGS 5
static lv_obj_t*   s_seg[STATUS_SEGS];
static lv_timer_t* s_clock_timer = NULL;
static struct { char mode[8]; int mode_kind; char model[64]; int gauge_pct; bool gauge_est; char amount[48]; int tools; char extra[24]; } s_st = {.mode = "AI", .gauge_pct = -1, .tools = -1};
static struct { bool wifi_connected; int rssi; char ssid[33]; char net_text[48]; int battery_pct; bool charging; } s_sys = {.battery_pct = -1};
static tui_submit_cb_t s_submit = NULL;
static void*       s_submit_user = NULL;

// ---------------------------------------------------------------------------------------
// Scrollback storage
// ---------------------------------------------------------------------------------------
static sb_line_t* sb_at(int i) { return &s_sb[(s_sb_head + i) % SB_LINES]; }
static sb_line_t* sb_last(void) { return s_sb_count ? sb_at(s_sb_count - 1) : NULL; }
static void sb_push(uint8_t style, const char* text, size_t n) {
    sb_line_t* l;
    if (s_sb_count == SB_LINES) {
        l = &s_sb[s_sb_head];
        plat_free(l->text);
        s_sb_head = (s_sb_head + 1) % SB_LINES;
    } else {
        l = sb_at(s_sb_count);
        s_sb_count++;
    }
    l->style = style;
    l->text  = plat_strndup(text ? text : "", n);
}
static void sb_append_to_last(const char* text, size_t n) {
    sb_line_t* l = sb_last();
    if (!l) { sb_push(TUI_STYLE_NORMAL, text, n); return; }
    size_t cur = l->text ? strlen(l->text) : 0;
    char* nt = (char*)plat_realloc(l->text, cur + n + 1);
    if (!nt) return;
    memcpy(nt + cur, text, n);
    nt[cur + n] = 0;
    l->text = nt;
}

static lv_color_t style_color(uint8_t st) {
    const ui_theme_t* t = ui_theme();
    switch (st) {
        case TUI_STYLE_USER: return t->user;
        case TUI_STYLE_MUTED: return t->muted;
        case TUI_STYLE_ERROR: return t->error;
        case TUI_STYLE_SYSTEM: return t->muted;
        case TUI_STYLE_DIFF_ADD: return t->diff_add;
        case TUI_STYLE_DIFF_DEL: return t->diff_del;
        default: return t->fg;
    }
}

// ---------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------
static int line_h(void) { return ui_line_h(); }
static int topbar_h(void) { return line_h() + 6; }
int tui_visible_rows(void) {
    int lh = line_h();
    int avail = ui_screen_h() - topbar_h() - 2 * lh - 1;  // system bar + status + input + separator
    int r = avail / lh;
    if (r > MAX_ROWS) r = MAX_ROWS;
    return r < 1 ? 1 : r;
}
int tui_cols(void) { int c = ui_cols(); return c < 10 ? 10 : c; }

static void render_status(void);
static void render_sysbar(void);

static void build_widgets(void) {
    lv_obj_t* root = s_view->root;
    const ui_theme_t* t = ui_theme();
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, t->bg, 0);
    int lh = line_h();
    int th = topbar_h();
    int rows = tui_visible_rows();
    int top = ui_screen_h() - 2 * lh - 1 - rows * lh;  // leftover space goes between the bar and the text
    s_nrows = rows;
    // --- system bar (top)
    s_top = lv_obj_create(root);
    ui_style_plain(s_top, t->bar_bg);
    lv_obj_set_size(s_top, ui_screen_w(), th);
    lv_obj_set_pos(s_top, 0, 0);
    s_clock = ui_mono_label(s_top, ui_font_bold(), t->fg);
    lv_obj_set_pos(s_clock, 6, 3);
    s_net_lbl = ui_mono_label(s_top, ui_font(), t->fg);
    for (int i = 0; i < 4; i++) { s_wifi_bar[i] = lv_obj_create(s_top); ui_style_plain(s_wifi_bar[i], t->seg_bg); }
    s_batt_out = lv_obj_create(s_top);
    ui_style_plain(s_batt_out, t->bar_bg);
    lv_obj_set_style_border_width(s_batt_out, 1, 0);
    lv_obj_set_style_border_color(s_batt_out, t->fg, 0);
    lv_obj_set_style_radius(s_batt_out, 2, 0);
    s_batt_fill = lv_obj_create(s_batt_out);
    ui_style_plain(s_batt_fill, t->ok);
    s_batt_lbl = ui_mono_label(s_top, ui_font(), t->fg);
    // --- scrollback
    for (int i = 0; i < rows; i++) {
        s_rows[i] = ui_mono_label(root, ui_font(), t->fg);
        lv_obj_set_pos(s_rows[i], 2, top + i * lh);
        lv_obj_set_width(s_rows[i], ui_screen_w() - 4);
    }
    // --- status line (bottom): coloured segments on a bar background
    int y_status = ui_screen_h() - 2 * lh - 1;
    s_sep1 = lv_obj_create(root);
    ui_style_plain(s_sep1, t->seg_bg);
    lv_obj_set_size(s_sep1, ui_screen_w(), 1);
    lv_obj_set_pos(s_sep1, 0, y_status);
    s_status_bg = lv_obj_create(root);
    ui_style_plain(s_status_bg, t->bar_bg);
    lv_obj_set_size(s_status_bg, ui_screen_w(), lh);
    lv_obj_set_pos(s_status_bg, 0, y_status + 1);
    for (int i = 0; i < STATUS_SEGS; i++) {
        s_seg[i] = ui_mono_label(s_status_bg, ui_font(), t->fg);
        lv_obj_set_style_bg_opa(s_seg[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(s_seg[i], LV_OBJ_FLAG_HIDDEN);
    }
    // --- input line
    int y_input = ui_screen_h() - lh;
    s_cursor = lv_obj_create(root);
    ui_style_plain(s_cursor, t->sel_bg);
    lv_obj_set_size(s_cursor, ui_char_w(), lh);
    lv_obj_set_pos(s_cursor, 2, y_input);
    s_input = ui_mono_label(root, ui_font(), t->fg);
    lv_obj_set_pos(s_input, 2, y_input);
    lv_obj_set_width(s_input, ui_screen_w() - 4);
    s_cursor_ch = ui_mono_label(root, ui_font(), t->sel_fg);
    lv_obj_set_pos(s_cursor_ch, 2, y_input);
    lv_obj_set_width(s_cursor_ch, ui_char_w() + 2);
    render_status();
    render_sysbar();
    s_dirty = true;
}

// ---------------------------------------------------------------------------------------
// Status line + system bar
// ---------------------------------------------------------------------------------------
static void seg_place(int i, int* x, const char* text, lv_color_t fg, lv_color_t bg, const lv_font_t* font) {
    lv_obj_t* l = s_seg[i];
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_set_style_bg_color(l, bg, 0);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, *x, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
    *x += ui_utf8_len(text) * ui_char_w();
}

static void render_status(void) {
    if (!s_status_bg) return;
    const ui_theme_t* t = ui_theme();
    int cols = tui_cols();
    for (int i = 0; i < STATUS_SEGS; i++) lv_obj_add_flag(s_seg[i], LV_OBJ_FLAG_HIDDEN);
    char mode[12], gauge[48] = "", amount[56] = "", tools[32] = "";
    snprintf(mode, sizeof(mode), " %s ", s_st.mode[0] ? s_st.mode : "AI");
    int pct = s_st.gauge_pct;
    if (pct >= 0) {
        if (pct > 100) pct = 100;
        int filled = (pct * 8 + 50) / 100;
        size_t n = (size_t)snprintf(gauge, sizeof(gauge), " ");
        for (int i = 0; i < 8; i++) n += (size_t)snprintf(gauge + n, sizeof(gauge) - n, "%s", i < filled ? "█" : "░");
        snprintf(gauge + n, sizeof(gauge) - n, " %s%d%% ", s_st.gauge_est ? "~" : "", pct);
    }
    if (s_st.amount[0]) snprintf(amount, sizeof(amount), " %s ", s_st.amount);
    if (s_st.tools >= 0) snprintf(tools, sizeof(tools), " %d tools ", s_st.tools);
    else if (s_st.extra[0]) snprintf(tools, sizeof(tools), " %s ", s_st.extra);
    // the model segment takes what is left of the line
    int fixed = ui_utf8_len(mode) + ui_utf8_len(gauge) + ui_utf8_len(amount) + ui_utf8_len(tools);
    int avail = cols - fixed - 2;
    if (avail < 6) avail = 6;
    char model[72];
    if (ui_utf8_len(s_st.model) > avail) {
        int cut = ui_utf8_offset(s_st.model, avail - 1);
        snprintf(model, sizeof(model), " %.*s… ", cut, s_st.model);
    } else {
        snprintf(model, sizeof(model), " %s ", s_st.model[0] ? s_st.model : "no model");
    }
    int x = 0;
    seg_place(0, &x, mode, lv_color_hex(0xFFFFFF), s_st.mode_kind == 1 ? t->accent2 : s_st.mode_kind == 2 ? t->accent3 : t->accent, ui_font_bold());
    seg_place(1, &x, model, t->fg, t->seg_bg, ui_font());
    if (gauge[0]) seg_place(2, &x, gauge, pct >= 85 ? t->error : pct >= 60 ? t->warn : t->ok, t->bar_bg, ui_font());
    if (amount[0]) seg_place(3, &x, amount, t->fg, t->bar_bg, ui_font());
    if (tools[0]) seg_place(4, &x, tools, t->muted, t->bar_bg, ui_font());
}

static void render_sysbar(void) {
    if (!s_top) return;
    const ui_theme_t* t = ui_theme();
    int th = topbar_h(), lh = line_h(), cw = ui_char_w();
    // left: clock once the network time is in, the app name before that
    char left[24];
    time_t now = time(NULL);
    if (now > 1700000000) { struct tm tm; localtime_r(&now, &tm); strftime(left, sizeof(left), "%H:%M", &tm); }
    else snprintf(left, sizeof(left), "ai-terminal");
    lv_label_set_text(s_clock, left);
    // right side, laid out from the right edge inwards
    int x = ui_screen_w() - 6;
    if (s_sys.battery_pct >= 0) {
        char b[8]; snprintf(b, sizeof(b), "%d%%", s_sys.battery_pct);
        x -= ui_utf8_len(b) * cw;
        lv_label_set_text(s_batt_lbl, b);
        lv_obj_set_pos(s_batt_lbl, x, 3);
        int ow = 22, oh = lh - 6;
        x -= ow + 5;
        lv_obj_set_size(s_batt_out, ow, oh);
        lv_obj_set_pos(s_batt_out, x, (th - oh) / 2);
        int fw = (ow - 4) * s_sys.battery_pct / 100;
        lv_obj_set_size(s_batt_fill, fw < 1 ? 1 : fw, oh - 4);
        lv_obj_set_pos(s_batt_fill, 1, 1);
        lv_obj_set_style_bg_color(s_batt_fill, s_sys.charging ? t->accent : s_sys.battery_pct <= 15 ? t->error : s_sys.battery_pct <= 30 ? t->warn : t->ok, 0);
        lv_obj_clear_flag(s_batt_out, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_batt_lbl, LV_OBJ_FLAG_HIDDEN);
        x -= 10;
    } else {
        lv_obj_add_flag(s_batt_out, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_batt_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    const char* txt = s_sys.wifi_connected ? s_sys.ssid : s_sys.net_text;
    x -= ui_utf8_len(txt) * cw;
    lv_label_set_text(s_net_lbl, txt);
    lv_obj_set_pos(s_net_lbl, x, 3);
    lv_obj_set_style_text_color(s_net_lbl, s_sys.wifi_connected ? t->fg : t->warn, 0);
    x -= 6;
    int level = !s_sys.wifi_connected ? 0 : s_sys.rssi >= -60 ? 4 : s_sys.rssi >= -67 ? 3 : s_sys.rssi >= -75 ? 2 : 1;
    lv_color_t on = level >= 3 ? t->ok : level == 2 ? t->warn : t->error;
    int bw = 4, gap = 2, maxh = th - 8;
    for (int i = 3; i >= 0; i--) {
        int bh = maxh * (i + 1) / 4;
        x -= bw;
        lv_obj_set_size(s_wifi_bar[i], bw, bh);
        lv_obj_set_pos(s_wifi_bar[i], x, th - 4 - bh);
        lv_obj_set_style_bg_color(s_wifi_bar[i], i < level ? on : t->seg_bg, 0);
        x -= gap;
    }
}

static void clock_cb(lv_timer_t* tm) {
    (void)tm;
    static int last_min = -1;
    time_t now = time(NULL);
    struct tm lt; localtime_r(&now, &lt);
    if (lt.tm_min != last_min) { last_min = lt.tm_min; render_sysbar(); }
}

// ---------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------
static int line_rows(const char* text, int cols) {
    int n = ui_utf8_len(text);
    if (n == 0) return 1;
    return (n + cols - 1) / cols;
}

static void render_scrollback(void) {
    int cols = tui_cols();
    int rows = s_nrows;
    // Walk backwards from the newest line, skipping s_scroll visual rows, filling `rows`.
    int   fill   = rows;
    int   skip   = s_scroll;
    int   total_rows_above = 0;
    // First pass: find (line, row) for the bottom of the window.
    struct { int line; int row; } slots[MAX_ROWS];
    int nslots = 0;
    for (int li = s_sb_count - 1; li >= 0 && (fill > 0); li--) {
        sb_line_t* l = sb_at(li);
        int lr = line_rows(l->text, cols);
        for (int r = lr - 1; r >= 0 && fill > 0; r--) {
            if (skip > 0) { skip--; continue; }
            slots[nslots].line = li; slots[nslots].row = r; nslots++; fill--;
        }
    }
    if (skip > 0) {  // scrolled past the top: clamp
        s_scroll -= skip;
        if (s_scroll < 0) s_scroll = 0;
    }
    (void)total_rows_above;
    // slots[0] is the bottom row. Fill labels top-down; empty rows at the top.
    for (int i = 0; i < rows; i++) {
        int si = rows - 1 - i;  // slot index for label i (top label = oldest)
        lv_obj_t* lab = s_rows[i];
        if (si >= nslots) { lv_label_set_text(lab, ""); continue; }
        sb_line_t* l = sb_at(slots[si].line);
        const char* txt = l->text ? l->text : "";
        int start = ui_utf8_offset(txt, slots[si].row * cols);
        int end   = ui_utf8_offset(txt, (slots[si].row + 1) * cols);
        char tmp[cols * 4 + 1];
        int n = end - start;
        if (n > (int)sizeof(tmp) - 1) n = (int)sizeof(tmp) - 1;
        memcpy(tmp, txt + start, n);
        tmp[n] = 0;
        lv_label_set_text(lab, tmp);
        lv_obj_set_style_text_color(lab, style_color(l->style), 0);
    }
}

static void render_input(void) {
    int cols = tui_cols();
    int plen = ui_utf8_len(s_prompt);
    int avail = cols - plen - 1;
    if (avail < 4) avail = 4;
    int cur_cp = ui_utf8_len_n(s_in, s_in_cur);
    int total_cp = ui_utf8_len(s_in);
    // horizontal window so the cursor is visible
    int win = 0;
    if (cur_cp >= avail) win = cur_cp - avail + 1;
    int start = ui_utf8_offset(s_in, win);
    int end   = ui_utf8_offset(s_in, win + avail);
    char buf[64 + (avail + 1) * 4];
    int n = snprintf(buf, sizeof(buf), "%s", s_prompt);
    int m = end - start;
    if (m > (int)sizeof(buf) - n - 1) m = (int)sizeof(buf) - n - 1;
    memcpy(buf + n, s_in + start, m);
    buf[n + m] = 0;
    lv_label_set_text(s_input, buf);
    // cursor
    int cur_col = plen + (cur_cp - win);
    int cw = ui_char_w();
    lv_obj_set_pos(s_cursor, 2 + cur_col * cw, ui_screen_h() - line_h());
    lv_obj_set_size(s_cursor, cw, line_h());
    lv_obj_set_pos(s_cursor_ch, 2 + cur_col * cw, ui_screen_h() - line_h());
    // character under the cursor (inverted)
    char ch[8] = "";
    if (s_in_cur < s_in_len) {
        int e = ui_utf8_offset(s_in, cur_cp + 1);
        int k = e - s_in_cur;
        if (k > 6) k = 6;
        memcpy(ch, s_in + s_in_cur, k);
        ch[k] = 0;
    }
    lv_label_set_text(s_cursor_ch, ch);
    (void)total_cp;
    const ui_theme_t* t = ui_theme();
    lv_obj_set_style_text_color(s_input, s_in_enabled ? t->fg : t->muted, 0);
}

static void refresh_timer_cb(lv_timer_t* tmr) {
    (void)tmr;
    if (!s_dirty) return;
    s_dirty = false;
    render_scrollback();
    render_input();
}
static void blink_cb(lv_timer_t* tmr) {
    (void)tmr;
    if (!s_cursor) return;
    bool hidden = lv_obj_has_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    if (hidden || !s_in_enabled) {
        lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_cursor_ch, LV_OBJ_FLAG_HIDDEN);
        if (!s_in_enabled) { lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(s_cursor_ch, LV_OBJ_FLAG_HIDDEN); }
    } else {
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cursor_ch, LV_OBJ_FLAG_HIDDEN);
    }
}

// ---------------------------------------------------------------------------------------
// Input editing
// ---------------------------------------------------------------------------------------
static int cp_prev(int off) {  // byte offset of previous code point start
    if (off <= 0) return 0;
    off--;
    while (off > 0 && (((unsigned char)s_in[off]) & 0xC0) == 0x80) off--;
    return off;
}
static int cp_next(int off) {
    if (off >= s_in_len) return s_in_len;
    off++;
    while (off < s_in_len && (((unsigned char)s_in[off]) & 0xC0) == 0x80) off++;
    return off;
}
static void in_insert(const char* utf8) {
    int n = (int)strlen(utf8);
    if (s_in_len + n >= INPUT_MAX - 1) return;
    memmove(s_in + s_in_cur + n, s_in + s_in_cur, s_in_len - s_in_cur + 1);
    memcpy(s_in + s_in_cur, utf8, n);
    s_in_len += n; s_in_cur += n;
}
static void in_backspace(void) {
    if (s_in_cur == 0) return;
    int p = cp_prev(s_in_cur);
    memmove(s_in + p, s_in + s_in_cur, s_in_len - s_in_cur + 1);
    s_in_len -= (s_in_cur - p); s_in_cur = p;
}
static void in_delete(void) {
    if (s_in_cur >= s_in_len) return;
    int nx = cp_next(s_in_cur);
    memmove(s_in + s_in_cur, s_in + nx, s_in_len - nx + 1);
    s_in_len -= (nx - s_in_cur);
}
static void in_set(const char* t) {
    strlcpy(s_in, t ? t : "", INPUT_MAX);
    s_in_len = (int)strlen(s_in); s_in_cur = s_in_len;
}
static void hist_add(const char* t) {
    if (!t || !*t) return;
    if (s_hist_n > 0 && strcmp(s_hist[s_hist_n - 1], t) == 0) return;
    if (s_hist_n == HISTORY_N) { plat_free(s_hist[0]); memmove(s_hist, s_hist + 1, sizeof(char*) * (HISTORY_N - 1)); s_hist_n--; }
    s_hist[s_hist_n++] = plat_strdup(t);
}
static void hist_browse(int dir) {  // dir -1 = older, +1 = newer
    if (s_hist_n == 0) return;
    if (s_hist_pos == -1) {
        if (dir > 0) return;
        strlcpy(s_hist_saved, s_in, INPUT_MAX);
        s_hist_pos = s_hist_n - 1;
    } else {
        s_hist_pos += dir;
        if (s_hist_pos >= s_hist_n) { s_hist_pos = -1; in_set(s_hist_saved); return; }
        if (s_hist_pos < 0) s_hist_pos = 0;
    }
    in_set(s_hist[s_hist_pos]);
}

static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (k->is_nav) {
        switch (k->nav) {
            case UI_NAV_PGUP: tui_scroll_rows(s_nrows - 1); return;
            case UI_NAV_PGDN: tui_scroll_rows(-(s_nrows - 1)); return;
            case UI_NAV_UP:
                if (k->mods & UI_MOD_SHIFT) { tui_scroll_rows(1); return; }
                if (s_in_enabled) { hist_browse(-1); s_dirty = true; }
                return;
            case UI_NAV_DOWN:
                if (k->mods & UI_MOD_SHIFT) { tui_scroll_rows(-1); return; }
                if (s_in_enabled) { hist_browse(+1); s_dirty = true; }
                return;
            default: break;
        }
        if (!s_in_enabled) return;
        switch (k->nav) {
            case UI_NAV_LEFT: s_in_cur = cp_prev(s_in_cur); break;
            case UI_NAV_RIGHT: s_in_cur = cp_next(s_in_cur); break;
            case UI_NAV_HOME: s_in_cur = 0; break;
            case UI_NAV_END: s_in_cur = s_in_len; break;
            case UI_NAV_BACKSPACE: in_backspace(); break;
            case UI_NAV_DELETE: in_delete(); break;
            case UI_NAV_ENTER: {
                if (s_in_len == 0) break;
                char* line = plat_strdup(s_in);
                hist_add(line);
                s_hist_pos = -1;
                in_set("");
                s_scroll = 0;
                s_dirty = true;
                if (s_submit) s_submit(line, s_submit_user);
                plat_free(line);
                return;
            }
            default: break;
        }
        s_scroll = 0;
        s_dirty = true;
        return;
    }
    // text key
    if (!s_in_enabled) return;
    if (k->mods & UI_MOD_CTRL) {
        char c = k->ascii | 0x20;
        if (c == 'u') { in_set(""); }
        else if (c == 'a') { s_in_cur = 0; }
        else if (c == 'e') { s_in_cur = s_in_len; }
        else if (c == 'k') { s_in[s_in_cur] = 0; s_in_len = s_in_cur; }
        else if (c == 'w') { while (s_in_cur > 0 && s_in[s_in_cur - 1] == ' ') in_backspace(); while (s_in_cur > 0 && s_in[s_in_cur - 1] != ' ') in_backspace(); }
        else if (c == 'l') { tui_clear(); }
        s_scroll = 0; s_dirty = true;
        return;
    }
    if (k->mods & UI_MOD_META) return;  // Meta combos are handled globally
    in_insert(k->utf8);
    s_hist_pos = -1;
    s_scroll = 0;
    s_dirty = true;
}

static void on_resize(ui_view_t* v) { (void)v; build_widgets(); }

// ---------------------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------------------
void tui_create(ui_view_t* v) {
    s_view = v;
    v->name = "tui";
    v->on_key = on_key;
    v->on_resize = on_resize;
    v->on_close = NULL;
    if (!s_sb) s_sb = (sb_line_t*)plat_calloc(SB_LINES, sizeof(sb_line_t));
    v->root = lv_obj_create(lv_screen_active());
    ui_style_plain(v->root, ui_theme()->bg);
    lv_obj_set_size(v->root, ui_screen_w(), ui_screen_h());
    lv_obj_set_pos(v->root, 0, 0);
    lv_obj_clear_flag(v->root, LV_OBJ_FLAG_SCROLLABLE);
    build_widgets();
    s_refresh = lv_timer_create(refresh_timer_cb, 30, NULL);
    s_clock_timer = lv_timer_create(clock_cb, 1000, NULL);
    s_blink   = lv_timer_create(blink_cb, 500, NULL);
    ui_set_root(v);
}
void tui_set_status_info(const tui_status_t* st) {
    if (!st) return;
    strlcpy(s_st.mode, st->mode ? st->mode : "AI", sizeof(s_st.mode));
    s_st.mode_kind = st->mode_kind;
    strlcpy(s_st.extra, st->extra ? st->extra : "", sizeof(s_st.extra));
    strlcpy(s_st.model, st->model ? st->model : "", sizeof(s_st.model));
    s_st.gauge_pct = st->gauge_pct;
    s_st.gauge_est = st->gauge_est;
    strlcpy(s_st.amount, st->amount ? st->amount : "", sizeof(s_st.amount));
    s_st.tools = st->tools;
    render_status();
}
void tui_set_sysbar(const tui_sysbar_t* sb) {
    if (!sb) return;
    s_sys.wifi_connected = sb->wifi_connected;
    s_sys.rssi = sb->rssi;
    strlcpy(s_sys.ssid, sb->ssid ? sb->ssid : "", sizeof(s_sys.ssid));
    strlcpy(s_sys.net_text, sb->net_text ? sb->net_text : "", sizeof(s_sys.net_text));
    s_sys.battery_pct = sb->battery_pct;
    s_sys.charging = sb->charging;
    render_sysbar();
}
void tui_set_prompt(const char* prompt) { strlcpy(s_prompt, prompt ? prompt : "", sizeof(s_prompt)); s_dirty = true; }
void tui_set_submit_cb(tui_submit_cb_t cb, void* user) { s_submit = cb; s_submit_user = user; }
void tui_set_input_enabled(bool enabled) { s_in_enabled = enabled; s_dirty = true; }
bool tui_input_enabled(void) { return s_in_enabled; }

static void append_split(tui_style_t style, const char* text, bool continue_last) {
    const char* p = text;
    bool first = true;
    while (1) {
        const char* nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (first && continue_last) sb_append_to_last(p, n);
        else sb_push((uint8_t)style, p, n);
        first = false;
        if (!nl) break;
        p = nl + 1;
        if (*p == 0) { sb_push((uint8_t)style, "", 0); break; }  // trailing newline -> empty line start
    }
}
void tui_append_line(tui_style_t style, const char* text) {
    if (s_streaming) tui_stream_end();
    append_split(style, text ? text : "", false);
    s_dirty = true;
}
void tui_appendf(tui_style_t style, const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tui_append_line(style, buf);
}
void tui_stream_begin(tui_style_t style) {
    if (s_streaming) tui_stream_end();
    sb_push((uint8_t)style, "", 0);
    s_streaming = true;
    s_dirty = true;
}
void tui_stream_text(const char* utf8) {
    if (!utf8 || !*utf8) return;
    if (!s_streaming) tui_stream_begin(TUI_STYLE_NORMAL);
    sb_line_t* last = sb_last();
    uint8_t style = last ? last->style : TUI_STYLE_NORMAL;
    append_split((tui_style_t)style, utf8, true);
    s_dirty = true;
}
void tui_stream_end(void) {
    if (!s_streaming) return;
    s_streaming = false;
    sb_line_t* last = sb_last();
    if (last && last->text && last->text[0] == 0) {  // drop a dangling empty line
        plat_free(last->text); last->text = NULL; s_sb_count--;
    }
    s_dirty = true;
}
bool tui_streaming(void) { return s_streaming; }
void tui_clear(void) {
    for (int i = 0; i < s_sb_count; i++) { sb_line_t* l = sb_at(i); plat_free(l->text); l->text = NULL; }
    s_sb_count = 0; s_sb_head = 0; s_scroll = 0; s_streaming = false;
    s_dirty = true;
}
void tui_scroll_rows(int rows) {
    s_scroll += rows;
    if (s_scroll < 0) s_scroll = 0;
    s_dirty = true;
}
void tui_scroll_to_bottom(void) { s_scroll = 0; s_dirty = true; }
const char* tui_input_text(void) { return s_in; }
void tui_input_set_text(const char* text) { in_set(text); s_dirty = true; }

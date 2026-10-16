#include "view_ssh.h"
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "net/ssh_session.h"
#include "ui.h"
#include "util/jsonbuf.h"
#include "util/platform.h"
#include "vt_screen.h"

static const char* TAG = "sshview";
static ui_view_t    s_view;
static vt_screen_t  s_vt;
static bool         s_vt_ok = false;
static bool         s_open = false;
static lv_obj_t**   s_rows = NULL;
static lv_obj_t*    s_cursor = NULL;
static lv_timer_t*  s_timer = NULL;
static RingbufHandle_t s_rx = NULL;   // bytes from the session task
static char         s_status[160];
static volatile ssh_state_t s_last_state = SSH_ST_IDLE;
static char         s_state_msg[96];
static volatile bool s_state_dirty = false;
static int          s_cols = 80, s_rows_n = 24;

// ---- session callbacks (session task) -------------------------------------------------
static void on_data(const uint8_t* d, size_t n, void* u) { (void)u; if (s_rx) xRingbufferSend(s_rx, d, n, pdMS_TO_TICKS(200)); }
static void on_state(ssh_state_t st, const char* msg, void* u) { (void)u; s_last_state = st; strlcpy(s_state_msg, msg, sizeof(s_state_msg)); s_state_dirty = true; }

// ---- rendering (UI task) --------------------------------------------------------------
static void render_row(int r) {
    const ui_theme_t* th = ui_theme();
    jsonbuf_t j; jb_init(&j);
    uint32_t cur = 0xFFFFFFFF; bool open = false;
    char fgs[7];
    for (int c = 0; c < s_vt.cols; c++) {
        const vt_cell_t* cell = vt_cell(&s_vt, r, c);
        uint32_t fg = cell->fg, bg = cell->bg;
        if (cell->attr & VT_ATTR_INVERSE) { uint32_t t = fg; fg = bg; bg = t; }
        if (cell->attr & VT_ATTR_DIM) fg = ((fg >> 1) & 0x7F7F7F);
        if (cell->attr & VT_ATTR_BOLD) { uint32_t rr = (fg >> 16) & 255, gg = (fg >> 8) & 255, bb = fg & 255; rr = rr + (255 - rr) / 3; gg = gg + (255 - gg) / 3; bb = bb + (255 - bb) / 3; fg = (rr << 16) | (gg << 8) | bb; }
        (void)bg;
        uint32_t cp = cell->cp ? cell->cp : ' ';
        if (fg != cur) {
            if (open) jb_append_char(&j, '#');
            snprintf(fgs, sizeof(fgs), "%06X", (unsigned)(fg & 0xFFFFFF));
            jb_appendf(&j, "#%s ", fgs);
            open = true; cur = fg;
        }
        if (cp == '#') jb_append_str(&j, "##");
        else if (cp < 0x80) jb_append_char(&j, (char)cp);
        else if (cp < 0x800) { jb_append_char(&j, (char)(0xC0 | (cp >> 6))); jb_append_char(&j, (char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { jb_append_char(&j, (char)(0xE0 | (cp >> 12))); jb_append_char(&j, (char)(0x80 | ((cp >> 6) & 0x3F))); jb_append_char(&j, (char)(0x80 | (cp & 0x3F))); }
        else { jb_append_char(&j, (char)(0xF0 | (cp >> 18))); jb_append_char(&j, (char)(0x80 | ((cp >> 12) & 0x3F))); jb_append_char(&j, (char)(0x80 | ((cp >> 6) & 0x3F))); jb_append_char(&j, (char)(0x80 | (cp & 0x3F))); }
    }
    if (open) jb_append_char(&j, '#');
    lv_label_set_text(s_rows[r], jb_cstr(&j));
    jb_free(&j);
    (void)th;
}
static void update_status(void) {
    app_config_t* c = app_config();
    int i = c->ui.ssh_host >= 0 ? c->ui.ssh_host : 0;
    const char* st = "";
    switch (s_last_state) {
        case SSH_ST_CONNECTING: st = "connecting…"; break;
        case SSH_ST_AUTH: st = "authenticating…"; break;
        case SSH_ST_OPEN: st = "connected"; break;
        case SSH_ST_CLOSED: st = "closed — Esc to leave"; break;
        case SSH_ST_ERROR: st = s_state_msg; break;
        default: st = "idle"; break;
    }
    if (c->ssh_count) snprintf(s_status, sizeof(s_status), "SSH · %s@%s · %s · Meta+Q quit", c->ssh[i].user, c->ssh[i].host, st);
    else snprintf(s_status, sizeof(s_status), "SSH · no host configured · Esc to leave");
    ui_view_set_status(&s_view, s_status);
}
static void tick(lv_timer_t* t) {
    (void)t;
    if (!s_open) return;
    // drain incoming bytes
    size_t len = 0;
    int budget = 16;
    while (budget-- > 0) {
        uint8_t* item = (uint8_t*)xRingbufferReceiveUpTo(s_rx, &len, 0, 2048);
        if (!item) break;
        vt_feed(&s_vt, (const char*)item, len);
        vRingbufferReturnItem(s_rx, item);
        char resp[64]; size_t rl = vt_take_response(&s_vt, resp, sizeof(resp));
        if (rl) ssh_session_send((const uint8_t*)resp, rl);
    }
    for (int r = 0; r < s_vt.rows; r++) if (vt_row_dirty(&s_vt, r)) render_row(r);
    vt_clear_dirty(&s_vt);
    // cursor
    int cw = ui_char_w(), lh = ui_line_h();
    lv_obj_set_pos(s_cursor, 2 + s_vt.cx * cw, 2 + s_vt.cy * lh);
    if (s_vt.cursor_visible && s_last_state == SSH_ST_OPEN) lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    if (s_state_dirty) {
        s_state_dirty = false;
        update_status();
        if (s_last_state == SSH_ST_ERROR) {
            char line[160]; snprintf(line, sizeof(line), "\r\n[error] %s — check host, user and password in the setup page\r\n", s_state_msg);
            vt_feed(&s_vt, line, strlen(line));
        } else if (s_last_state == SSH_ST_CLOSED) {
            const char* line = "\r\n[connection closed]\r\n";
            vt_feed(&s_vt, line, strlen(line));
        }
    }
}

// ---- keys --------------------------------------------------------------------------------
static void send_str(const char* s) { ssh_session_send((const uint8_t*)s, strlen(s)); }
static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    bool app = s_vt.app_cursor_keys;
    if (k->mods & UI_MOD_META) {
        if (!k->is_nav && (k->ascii == 'q' || k->ascii == 'Q')) { ssh_session_close(); ui_pop(); return; }
        if (k->is_nav && k->nav == UI_NAV_ESC) { ssh_session_close(); ui_pop(); return; }
        return;  // other Meta combos are handled globally
    }
    if (k->is_nav) {
        switch (k->nav) {
            case UI_NAV_ESC:
                if (s_last_state == SSH_ST_CLOSED || s_last_state == SSH_ST_ERROR || s_last_state == SSH_ST_IDLE) { ssh_session_close(); ui_pop(); return; }
                send_str("\x1b"); return;
            case UI_NAV_ENTER: send_str("\r"); return;
            case UI_NAV_BACKSPACE: send_str("\x7f"); return;
            case UI_NAV_DELETE: send_str("\x1b[3~"); return;
            case UI_NAV_TAB: send_str(k->mods & UI_MOD_SHIFT ? "\x1b[Z" : "\t"); return;
            case UI_NAV_UP: send_str(app ? "\x1bOA" : "\x1b[A"); return;
            case UI_NAV_DOWN: send_str(app ? "\x1bOB" : "\x1b[B"); return;
            case UI_NAV_RIGHT: send_str(app ? "\x1bOC" : "\x1b[C"); return;
            case UI_NAV_LEFT: send_str(app ? "\x1bOD" : "\x1b[D"); return;
            case UI_NAV_HOME: send_str("\x1b[H"); return;
            case UI_NAV_END: send_str("\x1b[F"); return;
            case UI_NAV_PGUP: send_str("\x1b[5~"); return;
            case UI_NAV_PGDN: send_str("\x1b[6~"); return;
            case UI_NAV_INSERT: send_str("\x1b[2~"); return;
            case UI_NAV_F1: send_str("\x1bOP"); return;
            case UI_NAV_F2: send_str("\x1bOQ"); return;
            case UI_NAV_F3: send_str("\x1bOR"); return;
            case UI_NAV_F4: send_str("\x1bOS"); return;
            case UI_NAV_F5: send_str("\x1b[15~"); return;
            case UI_NAV_F6: send_str("\x1b[17~"); return;
            case UI_NAV_F7: send_str("\x1b[18~"); return;
            case UI_NAV_F8: send_str("\x1b[19~"); return;
            case UI_NAV_F9: send_str("\x1b[20~"); return;
            case UI_NAV_F10: send_str("\x1b[21~"); return;
            case UI_NAV_F11: send_str("\x1b[23~"); return;
            case UI_NAV_F12: send_str("\x1b[24~"); return;
            default: return;
        }
    }
    if (k->mods & UI_MOD_CTRL) {
        char c = k->ascii;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 1);
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 1);
        else if (c == '[' || c == '3') c = 0x1b; else if (c == '\\' || c == '4') c = 0x1c; else if (c == ']' || c == '5') c = 0x1d;
        else if (c == ' ' || c == '2' || c == '@') c = 0; else if (c == '/' || c == '7' || c == '_') c = 0x1f; else if (c == '?' || c == '8') c = 0x7f;
        else return;
        ssh_session_send((const uint8_t*)&c, 1);
        return;
    }
    if (k->mods & UI_MOD_ALT) { send_str("\x1b"); }
    send_str(k->utf8);
}
static void on_close(ui_view_t* v) {
    (void)v;
    s_open = false;
    ssh_session_close();
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    if (s_vt_ok) { vt_free(&s_vt); s_vt_ok = false; }
    plat_free(s_rows); s_rows = NULL;
}
static void on_resize(ui_view_t* v) { (void)v; }

bool view_ssh_is_open(void) { return s_open; }

void view_ssh_open(void) {
    if (s_open) return;
    app_config_t* c = app_config();
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "ssh"; s_view.on_key = on_key; s_view.on_close = on_close; s_view.on_resize = on_resize;
    lv_obj_t* content = ui_view_create_root(&s_view, "SSH");
    const ui_theme_t* th = ui_theme();
    int cw = ui_char_w(), lh = ui_line_h();
    s_cols = (ui_screen_w() - 4) / cw;
    s_rows_n = (lv_obj_get_height(content) - 4) / lh;
    if (s_cols < 20) s_cols = 20;
    if (s_rows_n < 5) s_rows_n = 5;
    uint32_t fg = ((uint32_t)th->fg.red << 16) | ((uint32_t)th->fg.green << 8) | th->fg.blue;
    uint32_t bg = ((uint32_t)th->bg.red << 16) | ((uint32_t)th->bg.green << 8) | th->bg.blue;
    s_vt_ok = vt_init(&s_vt, s_cols, s_rows_n, fg, bg);
    s_rows = (lv_obj_t**)plat_calloc(s_rows_n, sizeof(lv_obj_t*));
    for (int r = 0; r < s_rows_n; r++) {
        s_rows[r] = ui_mono_label(content, ui_font(), th->fg);
        lv_label_set_recolor(s_rows[r], true);
        lv_obj_set_pos(s_rows[r], 2, 2 + r * lh);
        lv_obj_set_width(s_rows[r], ui_screen_w() - 4);
    }
    s_cursor = lv_obj_create(content);
    ui_style_plain(s_cursor, th->fg);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_60, 0);
    lv_obj_set_size(s_cursor, cw, lh);
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    if (!s_rx) s_rx = xRingbufferCreate(16384, RINGBUF_TYPE_BYTEBUF);
    s_open = true;
    s_last_state = SSH_ST_IDLE; s_state_msg[0] = 0; s_state_dirty = true;
    s_timer = lv_timer_create(tick, 30, NULL);
    ui_push(&s_view);
    if (c->ssh_count == 0) {
        const char* m = "No SSH host configured.\r\nAdd one on the setup page (F1 → Set up from phone), then pick it under Settings → SSH host.\r\n";
        vt_feed(&s_vt, m, strlen(m));
        update_status();
        return;
    }
    int i = c->ui.ssh_host >= 0 && c->ui.ssh_host < c->ssh_count ? c->ui.ssh_host : 0;
    char banner[160];
    snprintf(banner, sizeof(banner), "Connecting to %s@%s:%u …\r\n", c->ssh[i].user, c->ssh[i].host, c->ssh[i].port);
    vt_feed(&s_vt, banner, strlen(banner));
    ssh_callbacks_t cb = {.on_data = on_data, .on_state = on_state, .user = NULL};
    esp_err_t r = ssh_session_start(c->ssh[i].host, c->ssh[i].port, c->ssh[i].user, c->ssh[i].password, s_cols, s_rows_n, &cb);
    if (r != ESP_OK) ESP_LOGW(TAG, "ssh start: %s", esp_err_to_name(r));
    update_status();
}

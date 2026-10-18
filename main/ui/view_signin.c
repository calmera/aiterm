#include "view_signin.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "net/chron_login.h"
#include "ui.h"
#include "view_tui.h"

static const char* TAG = "signin";

static ui_view_t   s_view;
static bool        s_visible = false;
static lv_obj_t*   s_qr = NULL;
static lv_obj_t*   s_code = NULL;
static lv_obj_t*   s_code_hint = NULL;
static lv_obj_t*   s_steps = NULL;
static lv_obj_t*   s_status = NULL;
static lv_timer_t* s_timer = NULL;
static char        s_qr_shown[176] = "";
static char        s_code_shown[16] = "";
static chron_login_state_t s_state_shown = CHRON_LOGIN_IDLE;

static void render(void) {
    if (!s_visible) return;
    const ui_theme_t* t = ui_theme();
    chron_login_status_t st; chron_login_get(&st);
    char buf[256];
    switch (st.state) {
        case CHRON_LOGIN_STARTING: snprintf(buf, sizeof(buf), "asking GitHub for a code…"); break;
        case CHRON_LOGIN_PENDING: snprintf(buf, sizeof(buf), "waiting for your approval on GitHub · %d:%02d left · Esc cancels", st.seconds_left / 60, st.seconds_left % 60); break;
        case CHRON_LOGIN_FINISHING: snprintf(buf, sizeof(buf), "approved — asking chronicle which accounts you hold…"); break;
        case CHRON_LOGIN_AUTHORIZED:
            if (st.accounts == 0) snprintf(buf, sizeof(buf), "signed in as %s, but you hold no chronicle account yet — create one in the console. Esc closes.", st.login);
            else snprintf(buf, sizeof(buf), "signed in as %s · account %s%s. Esc closes, Meta+M switches to Rooms.", st.login, st.account, st.accounts > 1 ? " (others on the setup page)" : "");
            break;
        case CHRON_LOGIN_ERROR: snprintf(buf, sizeof(buf), "failed: %s — Esc closes", st.error); break;
        default: snprintf(buf, sizeof(buf), "cancelled — Esc closes"); break;
    }
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_text_color(s_status, st.state == CHRON_LOGIN_ERROR ? t->error : st.state == CHRON_LOGIN_AUTHORIZED ? t->ok : t->muted, 0);
    bool have_code = (st.state == CHRON_LOGIN_PENDING || st.state == CHRON_LOGIN_FINISHING) && st.user_code[0];
    if (have_code) {
        lv_label_set_text(s_code, st.user_code);
        lv_label_set_text(s_code_hint, "your one-time code — type it on GitHub if the field is empty");
        char sb[96]; snprintf(sb, sizeof(sb), "Sign in to chronicle · code %s · Esc cancel", st.user_code);
        ui_view_set_status(&s_view, sb);
    } else {
        lv_label_set_text(s_code, st.state == CHRON_LOGIN_STARTING ? "· · · ·" : "");
        lv_label_set_text(s_code_hint, st.state == CHRON_LOGIN_STARTING ? "fetching a code from GitHub" : "");
        ui_view_set_status(&s_view, "Sign in to chronicle · Esc cancel/close");
    }
    if (have_code && strcmp(s_qr_shown, st.qr_url) != 0) {
        strlcpy(s_qr_shown, st.qr_url, sizeof(s_qr_shown));
        lv_result_t r = lv_qrcode_update(s_qr, st.qr_url, (uint32_t)strlen(st.qr_url));
        if (r == LV_RESULT_OK) lv_obj_clear_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGI(TAG, "qr %s: %s", r == LV_RESULT_OK ? "ok" : "FAILED", st.qr_url);
    } else if (!have_code) {
        s_qr_shown[0] = 0;
        lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    }
    if (st.state != s_state_shown || strcmp(s_code_shown, have_code ? st.user_code : "") != 0) {
        s_state_shown = st.state;
        strlcpy(s_code_shown, have_code ? st.user_code : "", sizeof(s_code_shown));
        ESP_LOGI(TAG, "view: state=%s code=%s", chron_login_state_name(st.state), s_code_shown[0] ? s_code_shown : "-");
    }
}
static void timer_cb(lv_timer_t* tm) { (void)tm; render(); }
static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (k->is_nav && k->nav == UI_NAV_ESC) {
        chron_login_status_t st; chron_login_get(&st);
        if (st.state == CHRON_LOGIN_STARTING || st.state == CHRON_LOGIN_PENDING) chron_login_cancel();
        ui_pop();
    }
}
static void on_close(ui_view_t* v) {
    (void)v;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    s_visible = false; s_qr = s_code = s_code_hint = s_steps = s_status = NULL; s_qr_shown[0] = 0; s_code_shown[0] = 0; s_state_shown = CHRON_LOGIN_IDLE;
}
bool view_signin_visible(void) { return s_visible; }
void view_signin_refresh(void) { render(); }

void view_signin_show(void) {
    if (s_visible) { render(); return; }
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "signin";
    s_view.on_key = on_key;
    s_view.on_close = on_close;
    lv_obj_t* c = ui_view_create_root(&s_view, "Sign in to chronicle · Esc cancel/close");
    const ui_theme_t* t = ui_theme();
    int lh = ui_line_h();
    int w = ui_screen_w();
    int y = 8;
    lv_obj_t* title = ui_mono_label(c, ui_font_bold(), t->fg);
    lv_label_set_text(title, "Sign in to chronicle with GitHub");
    lv_obj_set_pos(title, 12, y);
    y += lh + 10;
    // the code, on its own row: the one thing that must be readable from across the desk
    const lv_font_t* big = ui_font_heading(1);
    int big_h = lv_font_get_line_height(big);
    lv_obj_t* code_bg = lv_obj_create(c);
    ui_style_plain(code_bg, t->seg_bg);
    lv_obj_set_size(code_bg, w - 24, big_h + 12);
    lv_obj_set_pos(code_bg, 12, y);
    s_code = ui_mono_label(code_bg, big, t->fg);
    lv_obj_set_width(s_code, w - 24 - 16);
    lv_label_set_long_mode(s_code, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_code, 8, 6);
    lv_label_set_text(s_code, "");
    y += big_h + 12 + 2;
    s_code_hint = ui_mono_label(c, ui_font(), t->muted);
    lv_obj_set_width(s_code_hint, w - 24);
    lv_obj_set_style_text_align(s_code_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_code_hint, 12, y);
    lv_label_set_text(s_code_hint, "");
    y += lh + 10;
    // the QR on the left, the steps on the right
    int qsize = 280;
    s_qr = lv_qrcode_create(c);
    lv_qrcode_set_size(s_qr, qsize);
    lv_qrcode_set_dark_color(s_qr, t->fg);
    lv_qrcode_set_light_color(s_qr, t->bg);
    lv_obj_set_pos(s_qr, 24, y);
    lv_obj_set_style_border_width(s_qr, 0, 0);
    lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
    int rx = 24 + qsize + 24;
    s_steps = ui_mono_label(c, ui_font(), t->fg);
    lv_obj_set_pos(s_steps, rx, y);
    lv_obj_set_width(s_steps, w - rx - 16);
    lv_label_set_long_mode(s_steps, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_steps,
        "1. Scan the QR with your phone.\n"
        "   Use a network with internet;\n"
        "   the badge's hotspot has none.\n\n"
        "2. Sign in to GitHub if asked.\n\n"
        "3. Enter the code above if the\n"
        "   field is empty, then Continue\n"
        "   and Authorize.\n\n"
        "No camera? Open\n"
        "github.com/login/device");
    y += qsize + 14;
    s_status = ui_mono_label(c, ui_font(), t->muted);
    lv_obj_set_pos(s_status, 24, y);
    lv_obj_set_width(s_status, w - 48);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    s_visible = true;
    render();
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    ui_push(&s_view);
}
void view_signin_start(void) {
    char err[160];
    if (chron_login_start_async(true, err, sizeof(err)) != ESP_OK) {
        chron_login_status_t st; chron_login_get(&st);
        if (st.state == CHRON_LOGIN_STARTING || st.state == CHRON_LOGIN_PENDING) { view_signin_show(); return; }  // already running: just show it
        tui_appendf(TUI_STYLE_ERROR, "[error] sign-in: %s", err);
        return;
    }
    view_signin_show();
}

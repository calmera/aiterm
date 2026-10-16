#include "view_provision.h"
#include <stdio.h>
#include <string.h>
#include "net/wifi.h"
#include "provisioning/provisioning.h"

static ui_view_t s_view;
static lv_obj_t* s_info = NULL;
static lv_timer_t* s_timer = NULL;
static char s_sta_url[64] = "";

static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (k->is_nav && k->nav == UI_NAV_ESC) ui_pop();
}
static void on_close(ui_view_t* v) {
    (void)v;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    s_info = NULL;
    // Leaving the screen keeps the hotspot running only while nothing else is configured;
    // when Wi-Fi works, stop the AP so the C6 can concentrate on the station link.
    if (wifi_sta_connected()) provisioning_ap_stop();
}
static lv_obj_t* make_qr(lv_obj_t* parent, const char* text, int size, int x, int y) {
    const ui_theme_t* t = ui_theme();
    lv_obj_t* qr = lv_qrcode_create(parent);
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, t->fg);
    lv_qrcode_set_light_color(qr, t->bg);
    lv_qrcode_update(qr, text, (uint32_t)strlen(text));
    lv_obj_set_pos(qr, x, y);
    lv_obj_set_style_border_width(qr, 0, 0);
    return qr;
}
void view_provision_update(void) {
    if (!s_info) return;
    char ip[16] = "";
    bool sta = wifi_sta_ip(ip, sizeof(ip));
    char buf[256];
    snprintf(buf, sizeof(buf), "Hotspot: %s (%d device%s connected)\nStation: %s%s",
             provisioning_ap_ssid(), wifi_ap_clients(), wifi_ap_clients() == 1 ? "" : "s",
             sta ? "connected, http://" : (wifi_sta_state() == WIFI_ST_CONNECTING ? "connecting…" : wifi_sta_state() == WIFI_ST_FAILED ? wifi_sta_last_error() : "not connected"),
             sta ? ip : "");
    if (sta) strlcat(buf, "/", sizeof(buf));
    lv_label_set_text(s_info, buf);
}
static void timer_cb(lv_timer_t* t) { (void)t; view_provision_update(); }

void view_provision_show(const char* ap_ssid, const char* ap_password, const char* sta_url) {
    memset(&s_view, 0, sizeof(s_view));
    s_view.name = "setup";
    s_view.on_key = on_key;
    s_view.on_close = on_close;
    lv_obj_t* c = ui_view_create_root(&s_view, "Set up from phone · Esc to close");
    const ui_theme_t* t = ui_theme();
    int lh = ui_line_h();
    int y = 8;
    lv_obj_t* title = ui_mono_label(c, ui_font_bold(), t->fg);
    lv_label_set_text(title, "Set up from a phone or laptop");
    lv_obj_set_pos(title, 12, y);
    y += lh + 8;
    char wifi_str[96];
    if (ap_password && ap_password[0]) snprintf(wifi_str, sizeof(wifi_str), "WIFI:T:WPA;S:%s;P:%s;;", ap_ssid, ap_password);
    else snprintf(wifi_str, sizeof(wifi_str), "WIFI:T:nopass;S:%s;;", ap_ssid);
    const char* url = "http://192.168.4.1/";
    int qsize = 260;
    make_qr(c, wifi_str, qsize, 40, y + lh + 6);
    make_qr(c, url, qsize, 720 - 40 - qsize, y + lh + 6);
    lv_obj_t* l1 = ui_mono_label(c, ui_font(), t->muted);
    lv_label_set_text(l1, "1. Join the hotspot");
    lv_obj_set_pos(l1, 40, y);
    lv_obj_t* l2 = ui_mono_label(c, ui_font(), t->muted);
    lv_label_set_text(l2, "2. Open the setup page");
    lv_obj_set_pos(l2, 720 - 40 - qsize, y);
    y += lh + 6 + qsize + 8;
    lv_obj_t* txt = ui_mono_label(c, ui_font(), t->fg);
    char buf[256];
    if (ap_password && ap_password[0]) snprintf(buf, sizeof(buf), "Wi-Fi: %s   password: %s\nURL:   %s", ap_ssid, ap_password, url);
    else snprintf(buf, sizeof(buf), "Wi-Fi: %s   (open network)\nURL:   %s", ap_ssid, url);
    lv_label_set_text(txt, buf);
    lv_obj_set_pos(txt, 40, y);
    y += 2 * lh + 10;
    if (sta_url && sta_url[0]) {
        strlcpy(s_sta_url, sta_url, sizeof(s_sta_url));
        lv_obj_t* l3 = ui_mono_label(c, ui_font(), t->muted);
        lv_label_set_text_fmt(l3, "Already on Wi-Fi? Same page at %s", sta_url);
        lv_obj_set_pos(l3, 40, y);
        make_qr(c, sta_url, 120, 720 - 40 - 120, y - 4);
        y += lh + 4;
    }
    s_info = ui_mono_label(c, ui_font(), t->muted);
    lv_obj_set_pos(s_info, 40, y + 6);
    lv_obj_set_width(s_info, 720 - 80 - 130);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    view_provision_update();
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    ui_push(&s_view);
}

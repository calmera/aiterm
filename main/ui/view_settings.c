#include "view_settings.h"
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_heap_caps.h"
#include "net/wifi.h"
#include "sdkconfig.h"

#define MAX_ITEMS 40
cfg_mode_t app_next_mode(cfg_mode_t m);  // main.c: AI -> SSH -> Rooms (when signed in) -> AI

typedef enum { IT_MODE, IT_INFERENCE, IT_MCP, IT_SSH_HOST, IT_ROOMS, IT_THEME, IT_FONT, IT_SETUP, IT_WIFI, IT_FORGET, IT_ABOUT, IT_HEADER } item_kind_t;
typedef struct { item_kind_t kind; int index; } item_t;

static ui_view_t s_view;
static settings_callbacks_t s_cb;
static item_t s_items[MAX_ITEMS];
static int s_n = 0;
static int s_sel = 0;
static int s_scroll = 0;
static lv_obj_t* s_rows[MAX_ITEMS];
static lv_obj_t* s_rows_bg[MAX_ITEMS];
static int s_visible = 0;

static void build_items(void) {
    app_config_t* c = app_config();
    s_n = 0;
    s_items[s_n++] = (item_t){IT_MODE, 0};
    s_items[s_n++] = (item_t){IT_INFERENCE, 0};
    s_items[s_n++] = (item_t){IT_HEADER, 0};  // "MCP servers"
    for (int i = 0; i < c->mcp_count && s_n < MAX_ITEMS - 10; i++) s_items[s_n++] = (item_t){IT_MCP, i};
    s_items[s_n++] = (item_t){IT_SSH_HOST, 0};
    s_items[s_n++] = (item_t){IT_ROOMS, 0};
    s_items[s_n++] = (item_t){IT_THEME, 0};
    s_items[s_n++] = (item_t){IT_FONT, 0};
    s_items[s_n++] = (item_t){IT_SETUP, 0};
    s_items[s_n++] = (item_t){IT_WIFI, 0};
    s_items[s_n++] = (item_t){IT_FORGET, 0};
    s_items[s_n++] = (item_t){IT_ABOUT, 0};
}

static void item_text(const item_t* it, char* buf, size_t n) {
    app_config_t* c = app_config();
    switch (it->kind) {
        case IT_MODE: snprintf(buf, n, "Mode                 %s", c->ui.default_mode == CFG_MODE_SSH ? "SSH" : c->ui.default_mode == CFG_MODE_ROOMS ? "Rooms" : "AI"); break;
        case IT_ROOMS:
            if (app_config_has_chronicle()) snprintf(buf, n, "Chronicle rooms      %s · account %s · store %s  (change on phone)", c->chronicle.login, c->chronicle.account, c->chronicle.store);
            else snprintf(buf, n, "Chronicle rooms      (not signed in — sign in with GitHub on the phone page)");
            break;
        case IT_INFERENCE: {
            char key[16]; app_config_mask_secret(c->inference.api_key, key, sizeof(key));
            snprintf(buf, n, "Inference            %s · %s · key %s  (change on phone)",
                     c->inference.provider == CFG_PROVIDER_ANTHROPIC ? "anthropic" : "openai", c->inference.model[0] ? c->inference.model : "-", key[0] ? key : "none");
            break;
        }
        case IT_HEADER: snprintf(buf, n, "MCP servers          %s", c->mcp_count ? "" : "(none — add on phone)"); break;
        case IT_MCP: snprintf(buf, n, "  [%c] %-16s %s", c->mcp[it->index].enabled ? 'x' : ' ', c->mcp[it->index].name[0] ? c->mcp[it->index].name : "server", c->mcp[it->index].url); break;
        case IT_SSH_HOST:
            if (c->ssh_count == 0) snprintf(buf, n, "SSH host             (none — add on phone)");
            else { int i = c->ui.ssh_host < 0 ? 0 : c->ui.ssh_host; snprintf(buf, n, "SSH host             %s (%s@%s:%u)", c->ssh[i].name[0] ? c->ssh[i].name : "host", c->ssh[i].user, c->ssh[i].host, c->ssh[i].port); }
            break;
        case IT_THEME: snprintf(buf, n, "Theme                %s", c->ui.dark ? "dark" : "light"); break;
        case IT_FONT: snprintf(buf, n, "Font size            %s", c->ui.font_size == 0 ? "small" : c->ui.font_size == 2 ? "large" : "medium"); break;
        case IT_SETUP: snprintf(buf, n, "Set up from phone    →"); break;
        case IT_WIFI: {
            char ip[16] = "";
            bool con = wifi_sta_ip(ip, sizeof(ip));
            snprintf(buf, n, "Wi-Fi                %s%s%s", c->wifi.ssid[0] ? c->wifi.ssid : "(not configured)", con ? " · " : "", con ? ip : (wifi_sta_state() == WIFI_ST_CONNECTING ? " · connecting" : ""));
            break;
        }
        case IT_FORGET: snprintf(buf, n, "Forget Wi-Fi         (reboots into setup)"); break;
        case IT_ABOUT: snprintf(buf, n, "About                v%s · heap %u KB · psram %u KB", CONFIG_AITERM_FW_VERSION,
                                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024)); break;
    }
}

static void render(void) {
    const ui_theme_t* t = ui_theme();
    if (s_sel < s_scroll) s_scroll = s_sel;
    if (s_sel >= s_scroll + s_visible) s_scroll = s_sel - s_visible + 1;
    char buf[320];
    for (int r = 0; r < s_visible; r++) {
        int i = s_scroll + r;
        if (i >= s_n) { lv_label_set_text(s_rows[r], ""); lv_obj_add_flag(s_rows_bg[r], LV_OBJ_FLAG_HIDDEN); continue; }
        item_text(&s_items[i], buf, sizeof(buf));
        lv_label_set_text(s_rows[r], buf);
        bool sel = (i == s_sel);
        bool header = s_items[i].kind == IT_HEADER;
        lv_obj_set_style_text_color(s_rows[r], sel ? t->sel_fg : (header ? t->muted : t->fg), 0);
        if (sel) lv_obj_clear_flag(s_rows_bg[r], LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(s_rows_bg[r], LV_OBJ_FLAG_HIDDEN);
    }
}

static void activate(void) {
    app_config_t* c = app_config();
    item_t* it = &s_items[s_sel];
    switch (it->kind) {
        case IT_MODE: c->ui.default_mode = app_next_mode(c->ui.default_mode); app_config_save(); if (s_cb.on_mode_changed) s_cb.on_mode_changed(); break;
        case IT_MCP: c->mcp[it->index].enabled = !c->mcp[it->index].enabled; app_config_save(); if (s_cb.on_mcp_toggled) s_cb.on_mcp_toggled(it->index); break;
        case IT_SSH_HOST: if (c->ssh_count) { c->ui.ssh_host = (int8_t)((c->ui.ssh_host + 1) % c->ssh_count); app_config_save(); if (s_cb.on_ssh_host_changed) s_cb.on_ssh_host_changed(); } break;
        case IT_THEME: c->ui.dark = !c->ui.dark; app_config_save(); if (s_cb.on_theme_changed) s_cb.on_theme_changed(); break;
        case IT_FONT: c->ui.font_size = (uint8_t)((c->ui.font_size + 1) % 3); app_config_save(); if (s_cb.on_font_changed) s_cb.on_font_changed(); break;
        case IT_SETUP: if (s_cb.on_setup_from_phone) s_cb.on_setup_from_phone(); return;
        case IT_FORGET: if (s_cb.on_forget_wifi) s_cb.on_forget_wifi(); return;
        case IT_ABOUT: if (s_cb.on_about) s_cb.on_about(); return;
        default: break;
    }
    render();
}

static void on_key(ui_view_t* v, const ui_key_t* k) {
    (void)v;
    if (!k->is_nav) {
        if (k->ascii == ' ') activate();
        return;
    }
    switch (k->nav) {
        case UI_NAV_ESC: ui_pop(); return;
        case UI_NAV_UP: do { s_sel = s_sel > 0 ? s_sel - 1 : s_n - 1; } while (s_items[s_sel].kind == IT_HEADER); break;
        case UI_NAV_DOWN: do { s_sel = (s_sel + 1) % s_n; } while (s_items[s_sel].kind == IT_HEADER); break;
        case UI_NAV_HOME: case UI_NAV_PGUP: s_sel = 0; break;
        case UI_NAV_END: case UI_NAV_PGDN: s_sel = s_n - 1; break;
        case UI_NAV_ENTER: case UI_NAV_RIGHT: activate(); return;
        default: return;
    }
    render();
}
static void layout(void) {
    lv_obj_t* c = ui_view_content(&s_view);
    lv_obj_clean(c);
    const ui_theme_t* t = ui_theme();
    int lh = ui_line_h();
    int y = 8;
    lv_obj_t* title = ui_mono_label(c, ui_font_bold(), t->fg);
    lv_label_set_text(title, "Settings");
    lv_obj_set_pos(title, 12, y);
    y += lh + 8;
    int avail = lv_obj_get_height(c) - y - 4;
    s_visible = avail / (lh + 2);
    if (s_visible > MAX_ITEMS) s_visible = MAX_ITEMS;
    for (int r = 0; r < s_visible; r++) {
        s_rows_bg[r] = lv_obj_create(c);
        ui_style_plain(s_rows_bg[r], t->sel_bg);
        lv_obj_set_size(s_rows_bg[r], 720 - 16, lh + 2);
        lv_obj_set_pos(s_rows_bg[r], 8, y + r * (lh + 2));
        lv_obj_add_flag(s_rows_bg[r], LV_OBJ_FLAG_HIDDEN);
        s_rows[r] = ui_mono_label(c, ui_font(), t->fg);
        lv_obj_set_pos(s_rows[r], 12, y + r * (lh + 2) + 1);
        lv_obj_set_width(s_rows[r], 720 - 24);
    }
    render();
}
static void on_resize(ui_view_t* v) { (void)v; ui_view_set_status(v, "Settings · ↑↓ move · Enter change · Esc back"); layout(); }
static void on_close(ui_view_t* v) { (void)v; }

void view_settings_show(const settings_callbacks_t* cb) {
    memset(&s_view, 0, sizeof(s_view));
    s_cb = *cb;
    s_view.name = "settings";
    s_view.on_key = on_key;
    s_view.on_close = on_close;
    s_view.on_resize = on_resize;
    ui_view_create_root(&s_view, "Settings · ↑↓ move · Enter change · Esc back");
    build_items();
    s_sel = 0; s_scroll = 0;
    layout();
    ui_push(&s_view);
}

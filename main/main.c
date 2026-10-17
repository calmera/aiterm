// ai-terminal — boot, BSP init, UI task, Wi-Fi bring-up, provisioning, mode dispatch.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "agent/agent_loop.h"
#include "agent/tools.h"
#include "app_config.h"
#include "bsp/device.h"
#include "bsp/display.h"
#include "bsp/input.h"
#include "bsp/power.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/mcp_client.h"
#include "net/wifi.h"
#include "nvs_flash.h"
#include "provisioning/provisioning.h"
#include "rooms/rooms_mode.h"
#include "sdkconfig.h"
#include "ui/ui.h"
#include "ui/view_provision.h"
#include "ui/view_settings.h"
#include "ui/view_signin.h"
#include "ui/view_ssh.h"
#include "ui/view_tui.h"

static const char* TAG = "main";
static ui_view_t   s_tui_view;
static bool        s_wifi_ok = false;
static bool        s_first_boot = false;

const char* aiterm_version(void) { return CONFIG_AITERM_FW_VERSION; }
static void mcp_refresh_async(void);
static void update_status_line(void);
void update_status_line_public(void) { update_status_line(); }

// AI -> SSH -> Rooms (only once signed in) -> AI
cfg_mode_t app_next_mode(cfg_mode_t m) {
    if (m == CFG_MODE_AI) return CFG_MODE_SSH;
    if (m == CFG_MODE_SSH) return app_config_has_chronicle() ? CFG_MODE_ROOMS : CFG_MODE_AI;
    return CFG_MODE_AI;
}
static const char* prompt_for_mode(cfg_mode_t m) { return m == CFG_MODE_SSH ? "ssh> " : m == CFG_MODE_ROOMS ? "rooms> " : "> "; }

// ---------------------------------------------------------------------------------------
// Status line
// ---------------------------------------------------------------------------------------
// "12.3k" style token counts for the status bar.
static void fmt_tokens(uint32_t n, char* out, size_t len) {
    if (n < 1000) snprintf(out, len, "%u", (unsigned)n);
    else if (n < 1000000) snprintf(out, len, "%.1fk", n / 1000.0);
    else snprintf(out, len, "%.2fM", n / 1000000.0);
}

static void update_status_line(void) {
    app_config_t* c = app_config();
    // ---- system bar: wifi + battery
    tui_sysbar_t sb = {0};
    char ip[16] = "", nettext[48] = "";
    sb.wifi_connected = wifi_sta_ip(ip, sizeof(ip));
    sb.rssi = sb.wifi_connected ? wifi_sta_rssi() : 0;
    sb.ssid = wifi_sta_ssid();
    if (!sb.wifi_connected) {
        if (wifi_sta_state() == WIFI_ST_CONNECTING) snprintf(nettext, sizeof(nettext), "connecting…");
        else if (provisioning_ap_active()) snprintf(nettext, sizeof(nettext), "setup %s", provisioning_ap_ssid());
        else snprintf(nettext, sizeof(nettext), "no wifi");
    }
    sb.net_text = nettext;
    bsp_power_battery_information_t bi;
    if (bsp_power_get_battery_information(&bi) == ESP_OK && bi.battery_available) {
        sb.battery_pct = (int)bi.remaining_percentage;
        sb.charging = bi.battery_charging;
    } else {
        sb.battery_pct = -1;  // the WHY2025 BSP has no fuel gauge / charger driver: hidden
    }
    tui_set_sysbar(&sb);
    // ---- status line
    tui_status_t st = {.gauge_pct = -1, .tools = -1};
    char model[80], amount[48] = "", extra[24] = "";
    if (c->ui.default_mode == CFG_MODE_ROOMS) {
        rooms_mode_status(&st, model, sizeof(model), amount, sizeof(amount), extra, sizeof(extra));
        tui_set_status_info(&st);
        return;
    }
    if (c->ui.default_mode == CFG_MODE_SSH) {
        int i = c->ui.ssh_host >= 0 ? c->ui.ssh_host : 0;
        if (c->ssh_count) snprintf(model, sizeof(model), "%s@%s", c->ssh[i].user, c->ssh[i].host);
        else snprintf(model, sizeof(model), "no host configured");
        st.mode = "SSH"; st.mode_kind = 1;
    } else {
        agent_stats_t as; agent_stats_get(&as);
        int win = c->inference.context_window > 0 ? c->inference.context_window : 128000;
        int pct = (int)(((uint64_t)as.ctx_tokens * 100 + (uint64_t)win / 2) / (uint64_t)win);
        st.gauge_pct = pct > 100 ? 100 : pct;
        st.gauge_est = as.ctx_estimated;
        if (c->inference.price_in > 0 || c->inference.price_out > 0) {
            double usd = (as.in_tokens * (double)c->inference.price_in + as.out_tokens * (double)c->inference.price_out) / 1e6;
            snprintf(amount, sizeof(amount), usd < 0.1 ? "$%.4f" : "$%.2f", usd);
        } else {
            char tk[16]; fmt_tokens(as.in_tokens + as.out_tokens, tk, sizeof(tk));
            snprintf(amount, sizeof(amount), "%s tok", tk);
        }
        snprintf(model, sizeof(model), "%s", c->inference.model[0] ? c->inference.model : "no model");
        st.mode = "AI"; st.tools = tools_count();
    }
    st.model = model; st.amount = amount;
    tui_set_status_info(&st);
}
static void ui_status_cb(void* arg) { (void)arg; update_status_line(); view_provision_update(); }
static void wifi_status_changed(void* user) { (void)user; ui_post(ui_status_cb, NULL); }

// ---------------------------------------------------------------------------------------
// Settings / provisioning actions (UI task)
// ---------------------------------------------------------------------------------------
static void apply_ui_config(void) {
    app_config_t* c = app_config();
    ui_set_dark(c->ui.dark);
    ui_set_font_size((ui_font_size_t)c->ui.font_size);
    update_status_line();
}
static void start_provisioning_ui(void) {
    if (!s_wifi_ok) { tui_append_line(TUI_STYLE_ERROR, "[error] radio not available — cannot start the setup hotspot"); return; }
    if (!provisioning_ap_active()) {
        if (provisioning_ap_start() != ESP_OK) { tui_append_line(TUI_STYLE_ERROR, "[error] could not start the setup hotspot"); return; }
    }
    char ip[16], url[48] = "";
    if (wifi_sta_ip(ip, sizeof(ip))) snprintf(url, sizeof(url), "http://%s/", ip);
    view_provision_show(provisioning_ap_ssid(), provisioning_ap_password(), url[0] ? url : NULL);
    update_status_line();
}
static cfg_mode_t s_shown_mode = CFG_MODE_AI;
static void cb_mode_changed(void) {
    cfg_mode_t m = app_config()->ui.default_mode;
    if (s_shown_mode == CFG_MODE_ROOMS && m != CFG_MODE_ROOMS) rooms_mode_leave();
    s_shown_mode = m;
    update_status_line();
    if (m == CFG_MODE_ROOMS) { rooms_mode_enter(); return; }
    tui_appendf(TUI_STYLE_SYSTEM, "mode: %s", m == CFG_MODE_SSH ? "SSH — Enter opens the terminal, Meta+Q leaves it" : "AI");
    tui_set_prompt(prompt_for_mode(m));
}
static void cb_theme_changed(void) { apply_ui_config(); }
static void cb_font_changed(void) { apply_ui_config(); }
static void cb_mcp_toggled(int index) { (void)index; mcp_refresh_async(); }
static void cb_setup(void) { start_provisioning_ui(); }
static void cb_forget_wifi(void) {
    app_config_t* c = app_config();
    c->wifi.ssid[0] = 0; c->wifi.password[0] = 0;
    app_config_save();
    tui_append_line(TUI_STYLE_SYSTEM, "Wi-Fi forgotten — rebooting into setup");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}
static void cb_about(void) {
    ui_pop();
    tui_appendf(TUI_STYLE_SYSTEM, "ai-terminal %s · IDF %s", aiterm_version(), esp_get_idf_version());
    tui_appendf(TUI_STYLE_SYSTEM, "heap: internal %u KB free, PSRAM %u KB free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    char ip[16] = "";
    if (wifi_sta_ip(ip, sizeof(ip))) tui_appendf(TUI_STYLE_SYSTEM, "wifi: %s · %s · %d dBm · setup page http://%s/", wifi_sta_ssid(), ip, wifi_sta_rssi(), ip);
    else tui_appendf(TUI_STYLE_SYSTEM, "wifi: not connected (%s)", wifi_sta_last_error()[0] ? wifi_sta_last_error() : "idle");
}
static void cb_ssh_host_changed(void) { update_status_line(); }
static void cb_chronicle_signin(void) { ui_pop(); view_signin_start(); }
static const settings_callbacks_t s_settings_cb = {
    .on_mode_changed = cb_mode_changed, .on_theme_changed = cb_theme_changed, .on_font_changed = cb_font_changed,
    .on_mcp_toggled = cb_mcp_toggled, .on_setup_from_phone = cb_setup, .on_forget_wifi = cb_forget_wifi,
    .on_about = cb_about, .on_ssh_host_changed = cb_ssh_host_changed, .on_chronicle_signin = cb_chronicle_signin,
};

static void mcp_done_cb(void* arg) { (void)arg; update_status_line(); tui_appendf(TUI_STYLE_MUTED, "MCP: %d tools from %d server(s)", tools_mcp_count(), app_config()->mcp_count); }
static void mcp_refresh_task(void* arg) {
    (void)arg;
    mcp_client_refresh_all();
    ui_post(mcp_done_cb, NULL);
    vTaskDelete(NULL);
}
static void mcp_refresh_async(void) {
    if (!wifi_sta_connected() || app_config()->mcp_count == 0) return;
    xTaskCreatePinnedToCore(mcp_refresh_task, "mcp", 12288, NULL, 4, NULL, 1);
}
static void config_changed_cb(void* arg) { (void)arg; apply_ui_config(); mcp_refresh_async(); rooms_mode_on_network_up(); }
void app_config_changed(void) { ui_post(config_changed_cb, NULL); }

// Hooks used by the provisioning API (replaced with real implementations in later phases).
char* app_test_inference(void) { return agent_test_inference(); }
char* app_test_mcp(const char* url, const char* auth_header, const char* auth_value) { return mcp_client_test(url, auth_header, auth_value); }

// ---------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------
static void on_submit(const char* text, void* user) {
    (void)user;
    if (app_config()->ui.default_mode == CFG_MODE_ROOMS) { rooms_mode_submit(text); return; }
    tui_appendf(TUI_STYLE_USER, "> %s", text);
    if (app_config()->ui.default_mode == CFG_MODE_SSH) {
        view_ssh_open();
        return;
    }
    agent_submit(text);
}

static bool global_keys(const ui_key_t* k) {
    bool meta = (k->mods & UI_MOD_META) != 0;
    if (app_config()->ui.default_mode == CFG_MODE_ROOMS && ui_top() == ui_root() && rooms_mode_key(k)) return true;
    if (k->is_nav) {
        if (k->nav == UI_NAV_ESC && ui_stack_depth() > 1) { ui_pop(); update_status_line(); return true; }
        if (k->nav == UI_NAV_ESC && agent_busy()) { agent_abort(); return true; }
        if (k->nav == UI_NAV_F1) { if (ui_top() == ui_root()) view_settings_show(&s_settings_cb); return true; }
        return false;
    }
    if (meta && (k->ascii == ',' || k->ascii == 'm' || k->ascii == 'M' || k->ascii == 'n' || k->ascii == 'N')) {
        while (ui_stack_depth() > 1) ui_pop();
        if (k->ascii == ',') view_settings_show(&s_settings_cb);
        else if (k->ascii == 'm' || k->ascii == 'M') { app_config()->ui.default_mode = app_next_mode(app_config()->ui.default_mode); app_config_save(); cb_mode_changed(); }
        else if (app_config()->ui.default_mode == CFG_MODE_ROOMS) { rooms_mode_picker(); }
        else { agent_new_session(); tui_clear(); tui_append_line(TUI_STYLE_SYSTEM, "new session"); }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Tasks
// ---------------------------------------------------------------------------------------
static void first_boot_cb(void* arg) { (void)arg; tui_append_line(TUI_STYLE_SYSTEM, "No Wi-Fi configured. Starting setup hotspot…"); start_provisioning_ui(); }
static void wifi_ready_cb(void* arg) {
    bool ok = (bool)(uintptr_t)arg;
    if (!ok) tui_appendf(TUI_STYLE_ERROR, "[error] Wi-Fi radio not responding — %s", wifi_sta_last_error());
    update_status_line();
}

static void net_boot_task(void* arg) {
    (void)arg;
    esp_err_t r = wifi_init();
    s_wifi_ok = (r == ESP_OK);
    ui_post(wifi_ready_cb, (void*)(uintptr_t)s_wifi_ok);
    if (!s_wifi_ok) { vTaskDelete(NULL); return; }
    wifi_set_status_cb(wifi_status_changed, NULL);
    provisioning_http_start();  // always reachable at http://<ip>/ when on Wi-Fi
    app_config_t* c = app_config();
    if (c->wifi.ssid[0]) {
        wifi_sta_connect(c->wifi.ssid, c->wifi.password);
        if (!wifi_sta_wait(20000)) {
            ESP_LOGW(TAG, "wifi connect failed: %s", wifi_sta_last_error());
        } else {
            rooms_mode_on_network_up();
            if (c->mcp_count > 0) {
                mcp_client_refresh_all();
                ui_post(mcp_done_cb, NULL);
            }
        }
    } else {
        s_first_boot = true;
        ui_post(first_boot_cb, NULL);
    }
    vTaskDelete(NULL);
}

static void ui_task(void* arg) {
    (void)arg;
    ui_init();
    tui_create(&s_tui_view);
    ui_set_global_key_handler(global_keys);
    tui_set_submit_cb(on_submit, NULL);
    agent_init();
    agent_set_stats_cb(ui_status_cb, NULL);  // refresh the context gauge / cost after every request
    rooms_mode_init();
    if (app_config()->ui.default_mode == CFG_MODE_ROOMS && !app_config_has_chronicle()) app_config()->ui.default_mode = CFG_MODE_AI;
    s_shown_mode = app_config()->ui.default_mode;
    apply_ui_config();
    tui_set_prompt(prompt_for_mode(app_config()->ui.default_mode));
    tui_appendf(TUI_STYLE_SYSTEM, "ai-terminal %s — F1 settings · Meta+M mode · Meta+N new session · Esc cancel/close", aiterm_version());
    if (app_config()->ui.default_mode == CFG_MODE_ROOMS) rooms_mode_enter();
    xTaskCreatePinnedToCore(net_boot_task, "netboot", 8192, NULL, 5, NULL, 1);
    ui_task_loop();
}

// Logs when PSRAM runs low (< 2 MB free) so leaks show up in the serial log.
static void heap_watchdog_task(void* arg) {
    (void)arg;
    bool warned = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (psram < 2 * 1024 * 1024) {
            if (!warned) ESP_LOGW(TAG, "low PSRAM: %u KB free (internal %u KB)", (unsigned)(psram / 1024), (unsigned)(internal / 1024));
            warned = true;
        } else {
            warned = false;
        }
        if (internal < 48 * 1024) ESP_LOGW(TAG, "low internal heap: %u KB free", (unsigned)(internal / 1024));
    }
}

static void mount_storage(void) {
    esp_vfs_spiffs_conf_t conf = {.base_path = "/storage", .partition_label = "storage", .max_files = 8, .format_if_mount_failed = true};
    esp_err_t r = esp_vfs_spiffs_register(&conf);
    if (r != ESP_OK) ESP_LOGW(TAG, "storage mount failed: %s", esp_err_to_name(r));
}

void app_main(void) {
    esp_err_t res = nvs_flash_init();
    if (res == ESP_ERR_NVS_NO_FREE_PAGES || res == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        res = nvs_flash_init();
    }
    ESP_ERROR_CHECK(res);
    app_config_load();
    {   // clock timezone (SNTP starts once Wi-Fi is up)
        const char* tz = app_config()->ui.timezone;
        setenv("TZ", tz[0] ? tz : "UTC0", 1);
        tzset();
    }
    mount_storage();

    const bsp_configuration_t bsp_configuration = {
        .display = {.requested_color_format = BSP_DISPLAY_COLOR_FORMAT_16_565RGB, .num_fbs = 1},
    };
    esp_err_t bsp_res = bsp_device_initialize(&bsp_configuration);
    if (bsp_res != ESP_OK) ESP_LOGE(TAG, "BSP init failed: %s", esp_err_to_name(bsp_res));
    if (bsp_device_get_initialized_without_coprocessor()) ESP_LOGW(TAG, "started without coprocessor");
    bsp_display_set_backlight_brightness(100);

    ESP_LOGI(TAG, "free heap: internal %u, psram %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    xTaskCreatePinnedToCore(ui_task, "ui", 16384, NULL, 5, NULL, 0);
    xTaskCreate(heap_watchdog_task, "heapwd", 3072, NULL, 1, NULL);
}

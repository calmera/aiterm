#include "ui.h"
#include <string.h>
#include "bsp/display.h"
#include "bsp/input.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char* TAG = "ui";

// Fonts generated with lv_font_conv from DejaVu Sans Mono (see main/fonts/).
LV_FONT_DECLARE(mono_13);
LV_FONT_DECLARE(mono_15);
LV_FONT_DECLARE(mono_18);
LV_FONT_DECLARE(mono_bold_13);
LV_FONT_DECLARE(mono_bold_15);
LV_FONT_DECLARE(mono_bold_18);
LV_FONT_DECLARE(mono_bold_22);
LV_FONT_DECLARE(mono_bold_26);

#define UI_MAX_STACK 8
#define SCREEN_W     720
#define SCREEN_H     720

static lv_display_t*   s_disp        = NULL;
#define FB_BPP 2  // DPI panel frame buffer is RGB565 (little-endian uint16 per pixel)
static uint8_t*        s_fb          = NULL;  // DPI panel frame buffer (RGB565)
static size_t          s_fb_stride   = SCREEN_W * FB_BPP;
static QueueHandle_t   s_input_queue = NULL;
static QueueHandle_t   s_post_queue  = NULL;
static TaskHandle_t    s_ui_task     = NULL;
static ui_theme_t      s_theme;
static ui_font_size_t  s_font_size   = UI_FONT_MEDIUM;
static ui_view_t*      s_stack[UI_MAX_STACK];
static int             s_depth       = 0;
static ui_global_key_fn s_global_key = NULL;

typedef struct { ui_cb_t fn; void* arg; SemaphoreHandle_t done; } ui_post_t;

// ---------------------------------------------------------------------------------------
// Theme / fonts
// ---------------------------------------------------------------------------------------
static void theme_fill(bool dark) {
    s_theme.dark     = dark;
    s_theme.bg       = dark ? lv_color_hex(0x111111) : lv_color_hex(0xF4F4F4);
    s_theme.fg       = dark ? lv_color_hex(0xE6E6E6) : lv_color_hex(0x111111);
    s_theme.muted    = lv_color_hex(0x8A8A8A);
    s_theme.sel_bg   = s_theme.fg;
    s_theme.sel_fg   = s_theme.bg;
    s_theme.diff_add = lv_color_hex(0x2EA043);
    s_theme.diff_del = lv_color_hex(0xF85149);
    s_theme.kw       = dark ? lv_color_hex(0x79C0FF) : lv_color_hex(0x0550AE);
    s_theme.str      = dark ? lv_color_hex(0xA5D6FF) : lv_color_hex(0x0A3069);
    s_theme.comment  = lv_color_hex(0x8B949E);
    s_theme.code_bg  = dark ? lv_color_hex(0x1E1E1E) : lv_color_hex(0xE4E4E4);
    s_theme.user     = dark ? lv_color_hex(0x58A6FF) : lv_color_hex(0x0969DA);  // prompts stand out
    s_theme.error    = lv_color_hex(0xF85149);
    s_theme.heading  = dark ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x000000);
    s_theme.accent   = lv_color_hex(0x1F6FEB);                                   // AI mode block
    s_theme.accent2  = lv_color_hex(0x8957E5);                                   // SSH mode block
    s_theme.seg_bg   = dark ? lv_color_hex(0x30363D) : lv_color_hex(0xD0D7DE);
    s_theme.bar_bg   = dark ? lv_color_hex(0x1C2128) : lv_color_hex(0xE6EBF0);
    s_theme.ok       = lv_color_hex(0x3FB950);
    s_theme.warn     = lv_color_hex(0xD29922);
}
const ui_theme_t* ui_theme(void) { return &s_theme; }

static void notify_resize(void) {
    lv_obj_set_style_bg_color(lv_screen_active(), s_theme.bg, 0);
    for (int i = 0; i < s_depth; i++) {
        if (s_stack[i]->on_resize) s_stack[i]->on_resize(s_stack[i]);
    }
}
void ui_set_dark(bool dark) { theme_fill(dark); notify_resize(); }
void ui_set_font_size(ui_font_size_t size) { s_font_size = size; notify_resize(); }
ui_font_size_t ui_get_font_size(void) { return s_font_size; }

const lv_font_t* ui_font(void) {
    switch (s_font_size) {
        case UI_FONT_SMALL: return &mono_13;
        case UI_FONT_LARGE: return &mono_18;
        default: return &mono_15;
    }
}
const lv_font_t* ui_font_bold(void) {
    switch (s_font_size) {
        case UI_FONT_SMALL: return &mono_bold_13;
        case UI_FONT_LARGE: return &mono_bold_18;
        default: return &mono_bold_15;
    }
}
const lv_font_t* ui_font_heading(int level) {
    // level 1 is the largest; scale with the base size
    if (level <= 1) return s_font_size == UI_FONT_SMALL ? &mono_bold_22 : &mono_bold_26;
    if (level == 2) return s_font_size == UI_FONT_SMALL ? &mono_bold_18 : &mono_bold_22;
    return s_font_size == UI_FONT_LARGE ? &mono_bold_22 : &mono_bold_18;
}
int ui_char_w(void) {
    // Monospace: every glyph has the same advance. Measure a plain ASCII glyph.
    return lv_font_get_glyph_width(ui_font(), 'M', 'M');
}
int ui_line_h(void) { return lv_font_get_line_height(ui_font()); }
int ui_cols(void) { int w = ui_char_w(); return w > 0 ? SCREEN_W / w : 80; }
int ui_screen_w(void) { return SCREEN_W; }
int ui_screen_h(void) { return SCREEN_H; }

// ---------------------------------------------------------------------------------------
// Display driver: LVGL renders RGB565 into PSRAM partial buffers; flush rotates the area
// straight into the MIPI-DSI DPI frame buffer and writes back the cache.
// ---------------------------------------------------------------------------------------
static lv_display_rotation_t configured_rotation(void) {
#if CONFIG_AITERM_DISPLAY_ROTATION_0
    return LV_DISPLAY_ROTATION_0;
#elif CONFIG_AITERM_DISPLAY_ROTATION_180
    return LV_DISPLAY_ROTATION_180;
#elif CONFIG_AITERM_DISPLAY_ROTATION_270
    return LV_DISPLAY_ROTATION_270;
#else
    return LV_DISPLAY_ROTATION_90;
#endif
}

static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    if (!s_fb) { lv_display_flush_ready(disp); return; }
    lv_display_rotation_t rot = lv_display_get_rotation(disp);
    static bool s_logged_first_flush = false;
    if (!s_logged_first_flush) {
        s_logged_first_flush = true;
        lv_draw_buf_t* db = lv_display_get_buf_active(disp);
        ESP_LOGI(TAG, "first flush: area %dx%d at (%d,%d) rot=%d draw-buf stride=%u (flush assumes %d) cf=%d",
                 (int)lv_area_get_width(area), (int)lv_area_get_height(area), (int)area->x1, (int)area->y1, (int)rot,
                 db ? (unsigned)db->header.stride : 0u, (int)(lv_area_get_width(area) * FB_BPP), db ? (int)db->header.cf : -1);
    }
    lv_area_t dst = *area;
    int32_t src_w = lv_area_get_width(area);
    int32_t src_h = lv_area_get_height(area);
    int32_t src_stride = src_w * FB_BPP;
    if (rot != LV_DISPLAY_ROTATION_0) lv_display_rotate_area(disp, &dst);
    // Clamp to the panel (should already be inside).
    if (dst.x1 < 0 || dst.y1 < 0 || dst.x2 >= SCREEN_W || dst.y2 >= SCREEN_H) { lv_display_flush_ready(disp); return; }
    uint8_t* dest = s_fb + (size_t)dst.y1 * s_fb_stride + (size_t)dst.x1 * FB_BPP;
    if (rot == LV_DISPLAY_ROTATION_0) {
        for (int32_t y = 0; y < src_h; y++) {
            memcpy(dest + (size_t)y * s_fb_stride, px_map + (size_t)y * src_stride, (size_t)src_stride);
        }
    } else {
        lv_draw_sw_rotate(px_map, dest, src_w, src_h, src_stride, (int32_t)s_fb_stride, rot, LV_COLOR_FORMAT_RGB565);
    }
    // Write the touched rows back from cache to PSRAM so the DSI DMA sees them.
    size_t rows  = (size_t)lv_area_get_height(&dst);
    uint8_t* row0 = s_fb + (size_t)dst.y1 * s_fb_stride;
    esp_cache_msync(row0, rows * s_fb_stride, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    lv_display_flush_ready(disp);
}

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
static void fb_fill565(uint16_t v) {
    uint16_t* fb = (uint16_t*)s_fb;
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) fb[i] = v;
    esp_cache_msync(s_fb, (size_t)SCREEN_W * SCREEN_H * FB_BPP, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

#if CONFIG_AITERM_DISPLAY_SELFTEST
// Hardware bring-up: write known patterns straight into the DPI frame buffer (bypassing LVGL)
// so a photo of the panel tells DSI/panel problems apart from LVGL/stride problems.
static void display_selftest(void) {
    if (!s_fb) return;
    static const struct { const char* name; uint8_t r, g, b; } solid[] = {
        {"red", 255, 0, 0}, {"green", 0, 255, 0}, {"blue", 0, 0, 255}, {"white", 255, 255, 255}, {"dark grey 0x11", 0x11, 0x11, 0x11}};
    for (size_t i = 0; i < sizeof(solid) / sizeof(solid[0]); i++) {
        ESP_LOGI(TAG, "selftest %u/5: solid %s", (unsigned)i + 1, solid[i].name);
        fb_fill565(rgb565(solid[i].r, solid[i].g, solid[i].b));
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
    ESP_LOGI(TAG, "selftest 6: 8 vertical colour bars (black,R,G,Y,B,M,C,W), bottom quarter 1-px checkerboard");
    uint16_t* fb = (uint16_t*)s_fb;
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            int bar = x * 8 / SCREEN_W;
            uint8_t r = (bar & 1) ? 255 : 0, g = (bar & 2) ? 255 : 0, b = (bar & 4) ? 255 : 0;
            if (y >= SCREEN_H * 3 / 4) { uint8_t v = ((x + y) & 1) ? 255 : 0; r = g = b = v; }
            fb[(size_t)y * SCREEN_W + x] = rgb565(r, g, b);
        }
    }
    esp_cache_msync(s_fb, (size_t)SCREEN_W * SCREEN_H * FB_BPP, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    vTaskDelay(pdMS_TO_TICKS(5000));
    ESP_LOGI(TAG, "selftest done, starting LVGL");
}
#endif

static void display_init(void) {
    esp_lcd_panel_handle_t panel = NULL;
    if (bsp_display_get_panel(&panel) == ESP_OK && panel) {
        void* fb0 = NULL;
        if (esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb0) == ESP_OK && fb0) {
            s_fb = (uint8_t*)fb0;
        } else {
            ESP_LOGE(TAG, "could not get DPI frame buffer");
        }
    } else {
        ESP_LOGE(TAG, "no display panel (running headless)");
    }
#if CONFIG_AITERM_DISPLAY_SELFTEST
    display_selftest();
#endif
    s_disp = lv_display_create(SCREEN_W, SCREEN_H);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    size_t buf_lines = CONFIG_AITERM_DRAW_BUF_LINES;
    size_t buf_sz    = (size_t)SCREEN_W * buf_lines * FB_BPP;
    uint8_t* b1 = heap_caps_aligned_alloc(128, buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t* b2 = heap_caps_aligned_alloc(128, buf_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b1 || !b2) {
        ESP_LOGE(TAG, "draw buffer allocation failed, falling back to internal RAM");
        if (!b1) b1 = heap_caps_aligned_alloc(128, buf_sz, MALLOC_CAP_8BIT);
        if (!b2) b2 = NULL;
    }
    lv_display_set_buffers(s_disp, b1, b2, (uint32_t)buf_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_set_rotation(s_disp, configured_rotation());
    lv_display_set_default(s_disp);
    if (s_fb) {
        fb_fill565(rgb565(0x11, 0x11, 0x11));
    }
}

// ---------------------------------------------------------------------------------------
// Keyboard: BSP events -> ui_key_t with auto-repeat for navigation keys
// ---------------------------------------------------------------------------------------
static uint32_t map_mods(uint32_t m) {
    uint32_t r = 0;
    if (m & BSP_INPUT_MODIFIER_SHIFT) r |= UI_MOD_SHIFT;
    if (m & BSP_INPUT_MODIFIER_CTRL) r |= UI_MOD_CTRL;
    if (m & BSP_INPUT_MODIFIER_ALT) r |= UI_MOD_ALT;
    if (m & BSP_INPUT_MODIFIER_SUPER) r |= UI_MOD_META;
    if (m & BSP_INPUT_MODIFIER_FUNCTION) r |= UI_MOD_FN;
    return r;
}

static ui_nav_key_t map_nav(bsp_input_navigation_key_t k, uint32_t mods) {
    bool fn = mods & UI_MOD_FN;
    switch (k) {
        case BSP_INPUT_NAVIGATION_KEY_ESC: return UI_NAV_ESC;
        case BSP_INPUT_NAVIGATION_KEY_UP: return fn ? UI_NAV_PGUP : UI_NAV_UP;
        case BSP_INPUT_NAVIGATION_KEY_DOWN: return fn ? UI_NAV_PGDN : UI_NAV_DOWN;
        case BSP_INPUT_NAVIGATION_KEY_LEFT: return fn ? UI_NAV_HOME : UI_NAV_LEFT;
        case BSP_INPUT_NAVIGATION_KEY_RIGHT: return fn ? UI_NAV_END : UI_NAV_RIGHT;
        case BSP_INPUT_NAVIGATION_KEY_HOME: return UI_NAV_HOME;
        case BSP_INPUT_NAVIGATION_KEY_END: return UI_NAV_END;
        case BSP_INPUT_NAVIGATION_KEY_PGUP: return UI_NAV_PGUP;
        case BSP_INPUT_NAVIGATION_KEY_PGDN: return UI_NAV_PGDN;
        case BSP_INPUT_NAVIGATION_KEY_RETURN: return UI_NAV_ENTER;
        case BSP_INPUT_NAVIGATION_KEY_SELECT: return UI_NAV_ENTER;
        case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: return fn ? UI_NAV_DELETE : UI_NAV_BACKSPACE;
        case BSP_INPUT_NAVIGATION_KEY_TAB: return UI_NAV_TAB;
        case BSP_INPUT_NAVIGATION_KEY_SUPER: return UI_NAV_META;
        case BSP_INPUT_NAVIGATION_KEY_MENU: return UI_NAV_F1;
        case BSP_INPUT_NAVIGATION_KEY_F1: return UI_NAV_F1;
        case BSP_INPUT_NAVIGATION_KEY_F2: return UI_NAV_F2;
        case BSP_INPUT_NAVIGATION_KEY_F3: return UI_NAV_F3;
        case BSP_INPUT_NAVIGATION_KEY_F4: return UI_NAV_F4;
        case BSP_INPUT_NAVIGATION_KEY_F5: return UI_NAV_F5;
        case BSP_INPUT_NAVIGATION_KEY_F6: return UI_NAV_F6;
        case BSP_INPUT_NAVIGATION_KEY_F7: return UI_NAV_F7;
        case BSP_INPUT_NAVIGATION_KEY_F8: return UI_NAV_F8;
        case BSP_INPUT_NAVIGATION_KEY_F9: return UI_NAV_F9;
        case BSP_INPUT_NAVIGATION_KEY_F10: return UI_NAV_F10;
        case BSP_INPUT_NAVIGATION_KEY_F11: return UI_NAV_F11;
        case BSP_INPUT_NAVIGATION_KEY_F12: return UI_NAV_F12;
        default: return UI_NAV_NONE;
    }
}

static void dispatch_key(const ui_key_t* k) {
    if (s_global_key && s_global_key(k)) return;
    ui_view_t* top = ui_top();
    if (top && top->on_key) top->on_key(top, k);
}

// auto-repeat state for the held navigation key
static ui_key_t s_held;
static bool     s_held_active = false;
static int64_t  s_held_next_ms = 0;
#define REPEAT_DELAY_MS    400
#define REPEAT_INTERVAL_MS 45

static void handle_bsp_event(const bsp_input_event_t* ev) {
#if CONFIG_AITERM_DISPLAY_SELFTEST
    ESP_LOGI(TAG, "input event type=%d", (int)ev->type);  // bring-up: prove the keyboard path
#endif
    ui_key_t k = {0};
    switch (ev->type) {
        case INPUT_EVENT_TYPE_NAVIGATION: {
            uint32_t mods = map_mods(ev->args_navigation.modifiers);
            bsp_input_navigation_key_t nk = ev->args_navigation.key;
            if (nk == BSP_INPUT_NAVIGATION_KEY_SPACE_L || nk == BSP_INPUT_NAVIGATION_KEY_SPACE_M ||
                nk == BSP_INPUT_NAVIGATION_KEY_SPACE_R) {
                if (!ev->args_navigation.state) { s_held_active = false; return; }
                k.is_nav = false; k.ascii = ' '; k.utf8[0] = ' '; k.mods = mods;
                dispatch_key(&k);
                s_held = k; s_held_active = true; s_held_next_ms = esp_timer_get_time() / 1000 + REPEAT_DELAY_MS;
                return;
            }
            ui_nav_key_t n = map_nav(nk, mods);
            if (n == UI_NAV_NONE) return;
            if (!ev->args_navigation.state) {  // release
                if (s_held_active && s_held.is_nav && s_held.nav == n) s_held_active = false;
                return;
            }
            k.is_nav = true; k.nav = n; k.mods = mods;
            dispatch_key(&k);
            if (n != UI_NAV_ESC && n != UI_NAV_ENTER && n != UI_NAV_META && n < UI_NAV_F1) {
                s_held = k; s_held_active = true; s_held_next_ms = esp_timer_get_time() / 1000 + REPEAT_DELAY_MS;
            } else {
                s_held_active = false;
            }
            return;
        }
        case INPUT_EVENT_TYPE_KEYBOARD: {
            k.is_nav = false;
            k.mods   = map_mods(ev->args_keyboard.modifiers);
            k.ascii  = ev->args_keyboard.ascii;
            strlcpy(k.utf8, ev->args_keyboard.utf8, sizeof(k.utf8));
            if (k.utf8[0] == 0 && k.ascii) { k.utf8[0] = k.ascii; k.utf8[1] = 0; }
            if (k.utf8[0] == 0) return;
            s_held_active = false;  // text keys are not repeated by us
            dispatch_key(&k);
            return;
        }
        default: return;
    }
}

static void poll_input(void) {
    if (!s_input_queue) return;
    bsp_input_event_t ev;
    int budget = 32;
    while (budget-- > 0 && xQueueReceive(s_input_queue, &ev, 0) == pdTRUE) handle_bsp_event(&ev);
    if (s_held_active) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now >= s_held_next_ms) {
            dispatch_key(&s_held);
            s_held_next_ms = now + REPEAT_INTERVAL_MS;
        }
    }
}

// ---------------------------------------------------------------------------------------
// Screen stack
// ---------------------------------------------------------------------------------------
lv_obj_t* ui_view_create_root(ui_view_t* v, const char* status_text) {
    lv_obj_t* root = lv_obj_create(lv_screen_active());
    ui_style_plain(root, s_theme.bg);
    lv_obj_set_size(root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    int lh = ui_line_h();
    lv_obj_t* content = lv_obj_create(root);
    ui_style_plain(content, s_theme.bg);
    lv_obj_set_size(content, SCREEN_W, SCREEN_H - lh - 2);
    lv_obj_set_pos(content, 0, 0);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    // 1 px separator
    lv_obj_t* sep = lv_obj_create(root);
    ui_style_plain(sep, s_theme.muted);
    lv_obj_set_size(sep, SCREEN_W, 1);
    lv_obj_set_pos(sep, 0, SCREEN_H - lh - 1);
    lv_obj_t* status = ui_mono_label(root, ui_font(), s_theme.muted);
    lv_obj_set_pos(status, 4, SCREEN_H - lh);
    lv_obj_set_width(status, SCREEN_W - 8);
    lv_label_set_long_mode(status, LV_LABEL_LONG_CLIP);
    lv_label_set_text(status, status_text ? status_text : "");
    v->root = root;
    // Make sizes valid immediately: views read lv_obj_get_height(content) while laying out.
    lv_obj_update_layout(root);
    return content;
}
lv_obj_t* ui_view_content(ui_view_t* v) { return v && v->root ? lv_obj_get_child(v->root, 0) : NULL; }
void ui_view_set_status(ui_view_t* v, const char* status_text) {
    if (!v || !v->root) return;
    lv_obj_t* status = lv_obj_get_child(v->root, 2);
    if (status) lv_label_set_text(status, status_text ? status_text : "");
}
void ui_set_root(ui_view_t* v) { s_stack[0] = v; if (s_depth == 0) s_depth = 1; }
ui_view_t* ui_root(void) { return s_depth > 0 ? s_stack[0] : NULL; }
ui_view_t* ui_top(void) { return s_depth > 0 ? s_stack[s_depth - 1] : NULL; }
int ui_stack_depth(void) { return s_depth; }
void ui_push(ui_view_t* v) {
    if (s_depth >= UI_MAX_STACK) { ESP_LOGW(TAG, "view stack full"); return; }
    ui_view_t* below = ui_top();
    if (below && below->root) lv_obj_add_flag(below->root, LV_OBJ_FLAG_HIDDEN);
    s_stack[s_depth++] = v;
    if (v->root) { lv_obj_clear_flag(v->root, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(v->root); }
    s_held_active = false;
}
void ui_pop(void) {
    if (s_depth <= 1) return;
    ui_view_t* v = s_stack[--s_depth];
    if (v->root) { lv_obj_delete(v->root); v->root = NULL; }
    if (v->on_close) v->on_close(v);
    ui_view_t* top = ui_top();
    if (top && top->root) { lv_obj_clear_flag(top->root, LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(top->root); }
    s_held_active = false;
}
void ui_set_global_key_handler(ui_global_key_fn fn) { s_global_key = fn; }

// ---------------------------------------------------------------------------------------
// Cross-task posting
// ---------------------------------------------------------------------------------------
bool ui_is_ui_task(void) { return xTaskGetCurrentTaskHandle() == s_ui_task; }
bool ui_post(ui_cb_t fn, void* arg) {
    ui_post_t p = {.fn = fn, .arg = arg, .done = NULL};
    return xQueueSend(s_post_queue, &p, pdMS_TO_TICKS(1000)) == pdTRUE;
}
void ui_call_sync(ui_cb_t fn, void* arg) {
    if (ui_is_ui_task()) { fn(arg); return; }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    ui_post_t p = {.fn = fn, .arg = arg, .done = done};
    if (xQueueSend(s_post_queue, &p, portMAX_DELAY) == pdTRUE) xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
}
static void drain_posts(void) {
    ui_post_t p;
    int budget = 16;
    while (budget-- > 0 && xQueueReceive(s_post_queue, &p, 0) == pdTRUE) {
        p.fn(p.arg);
        if (p.done) xSemaphoreGive(p.done);
    }
}

// ---------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------
void ui_style_plain(lv_obj_t* obj, lv_color_t bg) {
    lv_obj_remove_style_all(obj);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}
lv_obj_t* ui_mono_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_remove_style_all(l);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, "");
    return l;
}
int ui_utf8_len(const char* s) {
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) if ((*p & 0xC0) != 0x80) n++;
    return n;
}
int ui_utf8_len_n(const char* s, int n) {
    int c = 0;
    for (int i = 0; i < n && s[i]; i++) if ((((unsigned char)s[i]) & 0xC0) != 0x80) c++;
    return c;
}
int ui_utf8_offset(const char* s, int n) {
    int i = 0, cp = 0;
    while (s[i]) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) { if (cp == n) return i; cp++; }
        i++;
    }
    return i;
}

// ---------------------------------------------------------------------------------------
// Init / loop
// ---------------------------------------------------------------------------------------
void ui_init(void) {
    s_ui_task    = xTaskGetCurrentTaskHandle();
    s_post_queue = xQueueCreate(32, sizeof(ui_post_t));
    theme_fill(true);
    lv_init();
    lv_tick_set_cb(tick_cb);
    display_init();
    if (bsp_input_get_queue(&s_input_queue) != ESP_OK) {
        ESP_LOGW(TAG, "no input queue");
        s_input_queue = NULL;
    }
    lv_obj_t* scr = lv_screen_active();
    ui_style_plain(scr, s_theme.bg);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

void ui_task_loop(void) {
#if CONFIG_AITERM_DISPLAY_SELFTEST
    int64_t next_beat = 0;
#endif
    while (1) {
#if CONFIG_AITERM_DISPLAY_SELFTEST
        if (esp_timer_get_time() >= next_beat) {  // bring-up heartbeat: proves the UI task is alive
            next_beat = esp_timer_get_time() + 10 * 1000000LL;
            ESP_LOGI(TAG, "ui alive: depth=%d internal=%u psram=%u", s_depth,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
#endif
        uint32_t wait = lv_timer_handler();
        if (wait > 10) wait = 10;
        if (wait < 1) wait = 1;
        drain_posts();
        poll_input();
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

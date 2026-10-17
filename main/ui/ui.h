// LVGL glue for the WHY2025 badge: display + keyboard drivers, theme, fonts, screen stack.
// Everything in here runs on the UI task. Other tasks talk to the UI with ui_post().
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- keys -----------------------------------------------------------------------------
typedef enum {
    UI_NAV_NONE = 0,
    UI_NAV_ESC, UI_NAV_UP, UI_NAV_DOWN, UI_NAV_LEFT, UI_NAV_RIGHT,
    UI_NAV_HOME, UI_NAV_END, UI_NAV_PGUP, UI_NAV_PGDN,
    UI_NAV_ENTER, UI_NAV_BACKSPACE, UI_NAV_DELETE, UI_NAV_TAB, UI_NAV_INSERT,
    UI_NAV_F1, UI_NAV_F2, UI_NAV_F3, UI_NAV_F4, UI_NAV_F5, UI_NAV_F6,
    UI_NAV_F7, UI_NAV_F8, UI_NAV_F9, UI_NAV_F10, UI_NAV_F11, UI_NAV_F12,
    UI_NAV_META,  // the Meta/Super key pressed alone
} ui_nav_key_t;

#define UI_MOD_SHIFT 0x01
#define UI_MOD_CTRL  0x02
#define UI_MOD_ALT   0x04
#define UI_MOD_META  0x08
#define UI_MOD_FN    0x10

typedef struct {
    bool         is_nav;   // true: nav key (no text), false: text key
    ui_nav_key_t nav;
    char         utf8[7];  // text key: UTF-8 bytes (NUL terminated)
    char         ascii;    // text key: plain ASCII (lowercase letter even if Ctrl held), 0 if none
    uint32_t     mods;
} ui_key_t;

// ---- views ----------------------------------------------------------------------------
typedef struct ui_view ui_view_t;
struct ui_view {
    const char* name;
    lv_obj_t*   root;  // full-screen container; created by ui_view_create_root()
    void (*on_key)(ui_view_t* v, const ui_key_t* k);
    void (*on_close)(ui_view_t* v);  // called after the view was popped (free resources here)
    void (*on_resize)(ui_view_t* v); // theme/font changed: re-layout
    void* user;
};

// ---- theme / fonts --------------------------------------------------------------------
typedef struct {
    lv_color_t bg, fg, muted, sel_bg, sel_fg, diff_add, diff_del, kw, str, comment, code_bg, user, error, heading;
    lv_color_t accent, accent2, seg_bg, bar_bg, ok, warn;  // status / system bars
    bool       dark;
} ui_theme_t;

typedef enum { UI_FONT_SMALL = 0, UI_FONT_MEDIUM = 1, UI_FONT_LARGE = 2 } ui_font_size_t;

void              ui_init(void);
// Runs forever on the calling task: LVGL timers, keyboard, posted callbacks.
void              ui_task_loop(void) __attribute__((noreturn));

const ui_theme_t* ui_theme(void);
void              ui_set_dark(bool dark);
void              ui_set_font_size(ui_font_size_t size);
ui_font_size_t    ui_get_font_size(void);
const lv_font_t*  ui_font(void);                 // regular monospace at current size
const lv_font_t*  ui_font_bold(void);            // bold monospace at current size
const lv_font_t*  ui_font_heading(int level);    // level 1..3 -> large bold fonts
int               ui_char_w(void);               // advance width of the monospace font (px)
int               ui_line_h(void);               // line height (px)
int               ui_cols(void);                 // columns that fit on screen
int               ui_screen_w(void);
int               ui_screen_h(void);

// ---- screen stack ---------------------------------------------------------------------
// Create the view's full-screen root object with a one-line status label at the bottom.
// Returns the content area (everything above the status line).
lv_obj_t* ui_view_create_root(ui_view_t* v, const char* status_text);
void      ui_view_set_status(ui_view_t* v, const char* status_text);
lv_obj_t* ui_view_content(ui_view_t* v);
void      ui_push(ui_view_t* v);
void      ui_pop(void);                    // pops the top view (never the root TUI)
ui_view_t* ui_top(void);
ui_view_t* ui_root(void);
void      ui_set_root(ui_view_t* v);
int       ui_stack_depth(void);

// Global key handler, tried before the top view's on_key. Return true if consumed.
typedef bool (*ui_global_key_fn)(const ui_key_t* k);
void ui_set_global_key_handler(ui_global_key_fn fn);

// ---- cross-task ----------------------------------------------------------------------
typedef void (*ui_cb_t)(void* arg);
// Queue fn(arg) to run on the UI task. Safe from any task. Returns false if the queue is full.
bool ui_post(ui_cb_t fn, void* arg);
// Blocking variant: waits until fn has run (must NOT be called from the UI task).
void ui_call_sync(ui_cb_t fn, void* arg);
bool ui_is_ui_task(void);

// ---- helpers ------------------------------------------------------------------------
// A label styled as monospace text with the given colour, no padding.
lv_obj_t* ui_mono_label(lv_obj_t* parent, const lv_font_t* font, lv_color_t color);
// Apply the plain theme to an object: bg, no border/radius/padding.
void ui_style_plain(lv_obj_t* obj, lv_color_t bg);
// Count UTF-8 code points.
int ui_utf8_len(const char* s);
// Count code points in the first n bytes.
int ui_utf8_len_n(const char* s, int n);
// Byte offset of the n-th code point (clamped).
int ui_utf8_offset(const char* s, int n);

#ifdef __cplusplus
}
#endif

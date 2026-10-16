// The root chat/terminal view: scrollback + status line + single-line input editor.
// All functions must be called from the UI task (use ui_post() from other tasks).
#pragma once
#include <stdbool.h>
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TUI_STYLE_NORMAL = 0,  // assistant / plain text
    TUI_STYLE_USER,        // echoed user input
    TUI_STYLE_MUTED,       // tool call lines, hints
    TUI_STYLE_ERROR,       // [error] lines
    TUI_STYLE_SYSTEM,      // banners
    TUI_STYLE_DIFF_ADD,
    TUI_STYLE_DIFF_DEL,
} tui_style_t;

typedef void (*tui_submit_cb_t)(const char* text, void* user);

void tui_create(ui_view_t* v);  // builds the view and registers it as the UI root
void tui_set_status(const char* text);
void tui_set_prompt(const char* prompt);
void tui_set_submit_cb(tui_submit_cb_t cb, void* user);
void tui_set_input_enabled(bool enabled);
bool tui_input_enabled(void);

// Append a full line (embedded '\n' splits into several lines).
void tui_append_line(tui_style_t style, const char* text);
void tui_appendf(tui_style_t style, const char* fmt, ...);
// Streaming: start a line, append fragments as they arrive, end it.
void tui_stream_begin(tui_style_t style);
void tui_stream_text(const char* utf8);
void tui_stream_end(void);
bool tui_streaming(void);

void tui_clear(void);
void tui_scroll_rows(int rows);  // >0 scrolls up (older), <0 down
void tui_scroll_to_bottom(void);
int  tui_visible_rows(void);
int  tui_cols(void);

// Access to the current input text (for history, etc.)
const char* tui_input_text(void);
void        tui_input_set_text(const char* text);

#ifdef __cplusplus
}
#endif

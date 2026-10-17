// Rooms mode: the chat view as a chronicle-agents client. Everything here runs on the UI task;
// chronicle events are marshalled in with ui_post().
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "ui/ui.h"
#include "ui/view_tui.h"

#ifdef __cplusplus
extern "C" {
#endif

void rooms_mode_init(void);                 // once, after tui_create(): registers for chronicle events
void rooms_mode_enter(void);                // the mode became ROOMS
void rooms_mode_leave(void);                // another mode took over (the connection stays)
void rooms_mode_submit(const char* text);   // Enter: a message or a /command
bool rooms_mode_key(const ui_key_t* k);     // Meta+L picker, Tab completes @names
void rooms_mode_picker(void);
void rooms_mode_on_network_up(void);        // Wi-Fi is up: connect when this is the active mode
// Fills the rooms segments of the status line (buffers owned by the caller).
void rooms_mode_status(tui_status_t* st, char* model, size_t mn, char* amount, size_t an, char* extra, size_t en);

#ifdef __cplusplus
}
#endif

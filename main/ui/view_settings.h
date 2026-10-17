// On-device settings (§5.3): a keyboard-driven list. Short choices only; long strings are
// configured from the phone page.
#pragma once
#include "ui.h"

typedef struct {
    void (*on_mode_changed)(void);        // AI/SSH toggled
    void (*on_theme_changed)(void);
    void (*on_font_changed)(void);
    void (*on_mcp_toggled)(int index);
    void (*on_setup_from_phone)(void);
    void (*on_forget_wifi)(void);
    void (*on_about)(void);
    void (*on_ssh_host_changed)(void);
    void (*on_chronicle_signin)(void);    // "Sign in to chronicle" chosen
} settings_callbacks_t;

void view_settings_show(const settings_callbacks_t* cb);  // UI task

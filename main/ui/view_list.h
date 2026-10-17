// A keyboard-driven list picker: title, rows, ↑↓ move, Enter picks, Esc cancels. Rows are copied.
#pragma once
#include "ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*view_list_cb_t)(int index, void* user);  // index -1 = cancelled; runs on the UI task after the view closed
void view_list_show(const char* title, const char* status, const char* const* rows, int n, int initial, view_list_cb_t cb, void* user);

#ifdef __cplusplus
}
#endif

// SSH terminal view: renders a vt_screen and forwards keys to the session.
#pragma once
#include <stdbool.h>
// Open (push) the terminal view and connect to the SSH host selected in the config. UI task.
void view_ssh_open(void);
bool view_ssh_is_open(void);

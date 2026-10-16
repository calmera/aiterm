// Dispatch of the local render_* tools. Called from the agent (network) task; each tool pushes a
// full-screen view on the UI task and blocks until the user dismisses/answers it.
#pragma once
#include <stdbool.h>
// Returns a malloc'd (plat_malloc) JSON result string. Never returns NULL.
char* render_tool_run(const char* name, const char* arguments_json, volatile bool* abort_flag);
// Called by views on the UI task when they finish; result is copied.
void  render_tool_finish(const char* result_json);

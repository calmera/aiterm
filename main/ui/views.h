// Full-screen render views (Phase 3). Each `*_show` must run on the UI task; the view calls
// render_tool_finish(json) when the user dismisses or answers it.
#pragma once
#include "cJSON.h"
#include "ui.h"

void view_markdown_show(const char* markdown, const char* title);
void view_code_show(const char* code, const char* language, const char* title);
void view_table_show(const cJSON* columns, const cJSON* rows, const char* title);
void view_choice_show(const char* prompt, const cJSON* options, bool allow_multiple);
// diff: either unified diff text, or old/new (+path)
void view_diff_show(const char* diff, const char* old_text, const char* new_text, const char* path);
void view_image_show(const char* url, const char* base64, const char* format, const char* title);
void view_chart_show(const char* type, const cJSON* series, const cJSON* labels, const char* title, const char* y_label);

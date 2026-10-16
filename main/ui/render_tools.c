// Phase 2 stub: the real views arrive in Phase 3. Keeps the agent loop buildable.
#include "render_tools.h"
#include <stdio.h>
#include <string.h>
#include "util/platform.h"

char* render_tool_run(const char* name, const char* arguments_json, volatile bool* abort_flag) {
    (void)arguments_json; (void)abort_flag;
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s is not available on this firmware build\"}", name ? name : "tool");
    return plat_strdup(buf);
}
void render_tool_finish(const char* result_json) { (void)result_json; }

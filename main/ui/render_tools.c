// Dispatch of render_* tools: runs on the agent task, pushes the view on the UI task and blocks
// until the view reports a result (or the request is aborted).
#include "render_tools.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "ui.h"
#include "util/platform.h"
#include "views.h"

static SemaphoreHandle_t s_done = NULL;
static char*             s_result = NULL;
static bool              s_pending = false;

typedef struct { const char* name; cJSON* args; bool ok; char err[96]; } show_req_t;

static const char* jstr(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void show_on_ui(void* arg) {
    show_req_t* r = (show_req_t*)arg;
    cJSON* a = r->args;
    r->ok = true;
    if (strcmp(r->name, "render_markdown") == 0) {
        const char* md = jstr(a, "markdown");
        if (!md) { r->ok = false; snprintf(r->err, sizeof(r->err), "markdown is required"); return; }
        view_markdown_show(md, jstr(a, "title"));
    } else if (strcmp(r->name, "render_code") == 0) {
        const char* code = jstr(a, "code");
        if (!code) { r->ok = false; snprintf(r->err, sizeof(r->err), "code is required"); return; }
        view_code_show(code, jstr(a, "language"), jstr(a, "title"));
    } else if (strcmp(r->name, "render_table") == 0) {
        const cJSON* cols = cJSON_GetObjectItemCaseSensitive(a, "columns");
        const cJSON* rows = cJSON_GetObjectItemCaseSensitive(a, "rows");
        if (!cJSON_IsArray(cols) || !cJSON_IsArray(rows)) { r->ok = false; snprintf(r->err, sizeof(r->err), "columns and rows arrays are required"); return; }
        view_table_show(cols, rows, jstr(a, "title"));
    } else if (strcmp(r->name, "render_choice") == 0) {
        const cJSON* opts = cJSON_GetObjectItemCaseSensitive(a, "options");
        const char* prompt = jstr(a, "prompt");
        if (!cJSON_IsArray(opts) || cJSON_GetArraySize(opts) == 0) { r->ok = false; snprintf(r->err, sizeof(r->err), "options array is required"); return; }
        const cJSON* mult = cJSON_GetObjectItemCaseSensitive(a, "allow_multiple");
        view_choice_show(prompt ? prompt : "Choose:", opts, cJSON_IsTrue(mult));
    } else if (strcmp(r->name, "render_diff") == 0) {
        const char* diff = jstr(a, "diff");
        const char* o = jstr(a, "old");
        const char* n = jstr(a, "new");
        if (!diff && !(o && n)) { r->ok = false; snprintf(r->err, sizeof(r->err), "diff, or old and new, is required"); return; }
        view_diff_show(diff, o, n, jstr(a, "path"));
    } else if (strcmp(r->name, "render_image") == 0) {
        const char* url = jstr(a, "url");
        const char* b64 = jstr(a, "base64");
        if (!url && !b64) { r->ok = false; snprintf(r->err, sizeof(r->err), "url or base64 is required"); return; }
        view_image_show(url, b64, jstr(a, "format"), jstr(a, "title"));
    } else if (strcmp(r->name, "render_chart") == 0) {
        const cJSON* series = cJSON_GetObjectItemCaseSensitive(a, "series");
        const char* type = jstr(a, "type");
        if (!cJSON_IsArray(series) || cJSON_GetArraySize(series) == 0) { r->ok = false; snprintf(r->err, sizeof(r->err), "series array is required"); return; }
        view_chart_show(type ? type : "line", series, cJSON_GetObjectItemCaseSensitive(a, "labels"), jstr(a, "title"), jstr(a, "y_label"));
    } else {
        r->ok = false; snprintf(r->err, sizeof(r->err), "unknown render tool");
    }
}

static void pop_on_ui(void* arg) { (void)arg; if (ui_stack_depth() > 1) ui_pop(); }

char* render_tool_run(const char* name, const char* arguments_json, volatile bool* abort_flag) {
    if (!s_done) s_done = xSemaphoreCreateBinary();
    cJSON* args = arguments_json && *arguments_json ? cJSON_Parse(arguments_json) : cJSON_CreateObject();
    if (!args) return plat_strdup("{\"error\":\"tool arguments are not valid JSON\"}");
    show_req_t req = {.name = name, .args = args};
    plat_free(s_result); s_result = NULL;
    xSemaphoreTake(s_done, 0);  // clear stale signal
    s_pending = true;
    ui_call_sync(show_on_ui, &req);
    cJSON_Delete(args);
    if (!req.ok) {
        s_pending = false;
        char buf[160];
        snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", req.err);
        return plat_strdup(buf);
    }
    // Wait for the view; poll the abort flag.
    while (xSemaphoreTake(s_done, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (abort_flag && *abort_flag) {
            s_pending = false;
            ui_call_sync(pop_on_ui, NULL);
            return plat_strdup("{\"error\":\"cancelled by user\"}");
        }
    }
    s_pending = false;
    char* out = s_result ? s_result : plat_strdup("{\"status\":\"shown\"}");
    s_result = NULL;
    return out;
}

void render_tool_finish(const char* result_json) {
    if (!s_pending) return;
    plat_free(s_result);
    s_result = plat_strdup(result_json ? result_json : "{\"status\":\"shown\"}");
    if (s_done) xSemaphoreGive(s_done);
}

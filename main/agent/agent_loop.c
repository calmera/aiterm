#include "agent_loop.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "history.h"
#include "net/anthropic.h"
#include "net/http_stream.h"
#include "net/mcp_client.h"
#include "net/openai.h"
#include "net/wifi.h"
#include "tools.h"
#include "ui/render_tools.h"
#include "ui/ui.h"
#include "ui/view_tui.h"
#include "util/jsonbuf.h"
#include "util/platform.h"

static const char* TAG = "agent";

typedef enum { CMD_SUBMIT, CMD_NEW_SESSION } cmd_kind_t;
typedef struct { cmd_kind_t kind; char* text; } cmd_t;

static QueueHandle_t  s_cmds = NULL;
static history_t      s_history;
static volatile bool  s_abort = false;
static volatile bool  s_busy = false;
static agent_stats_t  s_stats;
static void (*s_stats_cb)(void*) = NULL;
static void*          s_stats_arg = NULL;
static char           s_sysprompt[CFG_PROMPT + 512];

#define MAX_ROUNDS 16

// ---------------------------------------------------------------------------------------
// UI bridge (everything that touches LVGL/TUI is posted to the UI task)
// ---------------------------------------------------------------------------------------
typedef struct { tui_style_t style; char* text; } ui_line_t;
static void ui_line_cb(void* arg) { ui_line_t* l = (ui_line_t*)arg; tui_append_line(l->style, l->text); plat_free(l->text); plat_free(l); }
static void ui_line(tui_style_t style, const char* text) {
    ui_line_t* l = (ui_line_t*)plat_malloc(sizeof(ui_line_t));
    if (!l) return;
    l->style = style; l->text = plat_strdup(text);
    if (!ui_post(ui_line_cb, l)) { plat_free(l->text); plat_free(l); }
}
static void ui_linef(tui_style_t style, const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    ui_line(style, buf);
}
static void ui_stream_begin_cb(void* arg) { (void)arg; tui_stream_begin(TUI_STYLE_NORMAL); }
static void ui_stream_text_cb(void* arg) { char* t = (char*)arg; tui_stream_text(t); plat_free(t); }
static void ui_stream_end_cb(void* arg) { (void)arg; tui_stream_end(); }
static void ui_input_cb(void* arg) { tui_set_input_enabled((bool)(uintptr_t)arg); }
static void set_input(bool en) { ui_post(ui_input_cb, (void*)(uintptr_t)en); }

// Text deltas are coalesced a little so the UI queue is not flooded by single tokens.
static jsonbuf_t s_pending;
static int64_t   s_pending_since = 0;
static void flush_pending(void) {
    if (s_pending.len == 0) return;
    char* t = jb_detach(&s_pending);
    if (!ui_post(ui_stream_text_cb, t)) plat_free(t);
    jb_init(&s_pending);
}
static void on_text(const char* delta, void* user) {
    (void)user;
    if (s_pending.len == 0) s_pending_since = plat_now_ms();
    jb_append_str(&s_pending, delta);
    if (s_pending.len > 48 || plat_now_ms() - s_pending_since > 60) flush_pending();
}
static void on_tool_begin(const char* id, const char* name, void* user) { (void)id; (void)user; (void)name; }
static void on_error(const char* msg, void* user) { (void)msg; (void)user; }

// ---------------------------------------------------------------------------------------
// Request plumbing
// ---------------------------------------------------------------------------------------
typedef struct { llm_stream_t* st; bool anthropic; } feed_ctx_t;
static void on_data(const char* data, size_t len, void* user) {
    feed_ctx_t* c = (feed_ctx_t*)user;
    if (c->anthropic) anthropic_stream_feed(c->st, data, len); else openai_stream_feed(c->st, data, len);
}

static void build_system_prompt(void) {
    const char* user = app_config()->inference.system_prompt;
    snprintf(s_sysprompt, sizeof(s_sysprompt), "%s%s%s", tools_system_prompt_prefix(), user[0] ? "\n\n" : "", user);
}

static const char* summarize_args(const char* json, char* out, size_t n) {
    // "key=value, key=value" for the first few scalar fields; long strings truncated.
    out[0] = 0;
    cJSON* o = json ? cJSON_Parse(json) : NULL;
    if (!o) { snprintf(out, n, "%.60s", json ? json : ""); return out; }
    size_t used = 0; int cnt = 0;
    cJSON* it;
    cJSON_ArrayForEach(it, o) {
        if (cnt++ >= 4 || used + 24 >= n) break;
        char val[48];
        if (cJSON_IsString(it)) snprintf(val, sizeof(val), "\"%.28s%s\"", it->valuestring, strlen(it->valuestring) > 28 ? "…" : "");
        else if (cJSON_IsNumber(it)) snprintf(val, sizeof(val), "%g", it->valuedouble);
        else if (cJSON_IsBool(it)) snprintf(val, sizeof(val), "%s", cJSON_IsTrue(it) ? "true" : "false");
        else if (cJSON_IsArray(it)) snprintf(val, sizeof(val), "[%d items]", cJSON_GetArraySize(it));
        else snprintf(val, sizeof(val), "{…}");
        used += (size_t)snprintf(out + used, n - used, "%s%s=%s", used ? ", " : "", it->string ? it->string : "?", val);
    }
    cJSON_Delete(o);
    return out;
}

// Returns true when the turn should continue with another request (tool calls executed).
// Fills *stop_reason for diagnostics.
static bool do_request(int round) {
    app_config_t* c = app_config();
    bool anthropic = c->inference.provider == CFG_PROVIDER_ANTHROPIC;
    int n_tools = 0;
    const tool_def_t* tools = tools_defs(&n_tools);
    build_system_prompt();
    char* body = anthropic ? anthropic_build_request(c->inference.model, s_sysprompt, &s_history, tools, n_tools, true, 4096)
                           : openai_build_request(c->inference.model, s_sysprompt, &s_history, tools, n_tools, true, 0);
    if (!body) { ui_line(TUI_STYLE_ERROR, "[error] out of memory building the request"); return false; }
    char url[CFG_URL + 32];
    snprintf(url, sizeof(url), "%s%s", c->inference.base_url, anthropic ? "/messages" : "/chat/completions");
    char auth[CFG_SECRET + 16];
    http_header_t hdrs[6];
    int nh = 0;
    hdrs[nh++] = (http_header_t){"Content-Type", "application/json"};
    hdrs[nh++] = (http_header_t){"Accept", "text/event-stream"};
    if (anthropic) {
        hdrs[nh++] = (http_header_t){"x-api-key", c->inference.api_key};
        hdrs[nh++] = (http_header_t){"anthropic-version", ANTHROPIC_VERSION};
    } else if (c->inference.api_key[0]) {
        snprintf(auth, sizeof(auth), "Bearer %s", c->inference.api_key);
        hdrs[nh++] = (http_header_t){"Authorization", auth};
    }
    llm_stream_t st;
    llm_stream_cb_t cb = {.on_text = on_text, .on_tool_call_begin = on_tool_begin, .on_error = on_error};
    llm_stream_init(&st, &cb, NULL);
    feed_ctx_t fc = {.st = &st, .anthropic = anthropic};
    jb_init(&s_pending);
    http_stream_req_t req = {
        .url = url, .method = "POST", .headers = hdrs, .n_headers = nh, .body = body, .body_len = strlen(body),
        .connect_timeout_ms = 15000, .read_timeout_ms = 60000, .total_timeout_ms = 0,
        .on_data = on_data, .user = &fc, .abort_flag = &s_abort,
    };
    http_stream_res_t res;
    ui_post(ui_stream_begin_cb, NULL);
    esp_err_t err = http_stream_run(&req, &res);
    if (anthropic) anthropic_stream_finish(&st); else openai_stream_finish(&st);
    flush_pending();
    ui_post(ui_stream_end_cb, NULL);
    // Session statistics (status bar). Fall back to a size estimate when usage is not reported.
    s_stats.requests++;
    if (st.usage_in > 0 || st.usage_out > 0) {
        s_stats.in_tokens  += (uint32_t)(st.usage_in > 0 ? st.usage_in : 0);
        s_stats.out_tokens += (uint32_t)(st.usage_out > 0 ? st.usage_out : 0);
        s_stats.ctx_tokens    = (uint32_t)((st.usage_in > 0 ? st.usage_in : 0) + (st.usage_out > 0 ? st.usage_out : 0));
        s_stats.ctx_estimated = false;
    } else {
        uint32_t est_in = (uint32_t)((strlen(body) + 3) / 4);
        uint32_t est_out = (uint32_t)((st.text.len + 3) / 4);
        s_stats.in_tokens += est_in; s_stats.out_tokens += est_out;
        s_stats.ctx_tokens = est_in + est_out; s_stats.ctx_estimated = true;
    }
    if (s_stats_cb) ui_post(s_stats_cb, s_stats_arg);
    plat_free(body);

    bool continue_turn = false;
    if (res.aborted) {
        ui_line(TUI_STYLE_MUTED, "[cancelled]");
        if (st.text.len) history_add_assistant(&s_history, llm_stream_text(&st), NULL, 0);
    } else if (err != ESP_OK && res.bytes == 0) {
        const char* hint = wifi_sta_connected() ? "check the base URL and that the server is reachable" : "Wi-Fi is not connected";
        ui_linef(TUI_STYLE_ERROR, "[error] %s — %s", res.error[0] ? res.error : esp_err_to_name(err), hint);
    } else if (res.status >= 400) {
        const char* hint = res.status == 401 || res.status == 403 ? "check the API key" : res.status == 404 ? "check the base URL and model name" : res.status == 429 ? "rate limited, try again" : "server error";
        ui_linef(TUI_STYLE_ERROR, "[error] HTTP %d%s%s — %s", res.status, st.got_error ? ": " : "", st.got_error ? st.error : "", hint);
    } else if (st.got_error) {
        ui_linef(TUI_STYLE_ERROR, "[error] %s", st.error);
    } else {
        tool_call_t* calls = NULL;
        int n = llm_stream_take_tool_calls(&st, &calls);
        history_add_assistant(&s_history, st.text.len ? llm_stream_text(&st) : NULL, calls, n);
        if (n > 0 && round < MAX_ROUNDS) {
            // Execute in order. `calls` is now owned by the history; read from the message.
            const msg_t* am = s_history.tail;
            for (int i = 0; i < am->n_tool_calls && !s_abort; i++) {
                const tool_call_t* tc = &am->tool_calls[i];
                char summary[160];
                ui_linef(TUI_STYLE_MUTED, "▸ %s(%s)", tc->name, summarize_args(tc->arguments, summary, sizeof(summary)));
                char* result = NULL;
                bool is_err = false;
                const tool_entry_t* te = tools_find(tc->name);
                if (te && te->server == -1) {
                    result = render_tool_run(tc->name, tc->arguments, &s_abort);
                } else if (te) {
                    result = mcp_client_call(te->server, te->remote_name, tc->arguments, &is_err);
                } else {
                    char tmp[200]; snprintf(tmp, sizeof(tmp), "{\"error\":\"unknown tool %s\"}", tc->name);
                    result = plat_strdup(tmp); is_err = true;
                }
                if (!result) { result = plat_strdup("{\"error\":\"tool failed\"}"); is_err = true; }
                if (strncmp(result, "{\"error\"", 8) == 0) is_err = true;
                ui_linef(TUI_STYLE_MUTED, "◂ %s (%u bytes)", is_err ? "error" : "result", (unsigned)strlen(result));
                history_add_tool_result(&s_history, tc->id, tc->name, result, is_err);
                plat_free(result);
            }
            continue_turn = !s_abort;
            if (round + 1 >= MAX_ROUNDS) { ui_line(TUI_STYLE_MUTED, "[tool loop limit reached]"); continue_turn = false; }
        } else if (n > 0) {
            ui_line(TUI_STYLE_MUTED, "[tool loop limit reached]");
        } else if (strcmp(st.finish_reason, "length") == 0) {
            ui_line(TUI_STYLE_MUTED, "[response truncated by max_tokens]");
        }
        tool_calls_free(NULL, 0);
    }
    llm_stream_free(&st);
    jb_free(&s_pending);
    return continue_turn;
}

static void handle_submit(char* text) {
    app_config_t* c = app_config();
    if (!app_config_has_inference()) { ui_line(TUI_STYLE_ERROR, "[error] no inference endpoint configured — F1 → Set up from phone"); plat_free(text); return; }
    if (!wifi_sta_connected()) { ui_line(TUI_STYLE_ERROR, "[error] Wi-Fi not connected — F1 → Wi-Fi"); plat_free(text); return; }
    (void)c;
    s_busy = true; s_abort = false;
    set_input(false);
    history_add_user(&s_history, text);
    plat_free(text);
    int round = 0;
    while (do_request(round) && !s_abort) round++;
    s_busy = false;
    set_input(true);
}

static void agent_task(void* arg) {
    (void)arg;
    cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_cmds, &cmd, portMAX_DELAY) != pdTRUE) continue;
        switch (cmd.kind) {
            case CMD_SUBMIT: handle_submit(cmd.text); break;
            case CMD_NEW_SESSION:
                history_clear(&s_history);
                memset(&s_stats, 0, sizeof(s_stats));
                if (s_stats_cb) ui_post(s_stats_cb, s_stats_arg);
                break;
        }
    }
}

void agent_init(void) {
    history_init(&s_history, 96 * 1024);
    tools_init();
    s_cmds = xQueueCreate(8, sizeof(cmd_t));
    // 24 KB stack in PSRAM: TLS + JSON building is stack hungry.
    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(agent_task, "agent", 24576, NULL, 5, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r != pdPASS) xTaskCreatePinnedToCore(agent_task, "agent", 16384, NULL, 5, NULL, 1);
}
void agent_submit(const char* text) {
    if (s_busy) { tui_append_line(TUI_STYLE_MUTED, "(busy — press Esc to cancel the current request)"); return; }
    cmd_t c = {.kind = CMD_SUBMIT, .text = plat_strdup(text)};
    if (xQueueSend(s_cmds, &c, 0) != pdTRUE) plat_free(c.text);
}
void agent_abort(void) { if (s_busy) s_abort = true; }
void agent_new_session(void) {
    s_abort = true;
    cmd_t c = {.kind = CMD_NEW_SESSION, .text = NULL};
    xQueueSend(s_cmds, &c, 0);
}
bool agent_busy(void) { return s_busy; }
void agent_stats_get(agent_stats_t* out) { if (out) *out = s_stats; }
void agent_set_stats_cb(void (*cb)(void* arg), void* arg) { s_stats_cb = cb; s_stats_arg = arg; }

char* agent_test_inference(void) {
    app_config_t* c = app_config();
    cJSON* out = cJSON_CreateObject();
    if (!app_config_has_inference()) { cJSON_AddBoolToObject(out, "ok", false); cJSON_AddStringToObject(out, "error", "base URL and model are required"); }
    else if (!wifi_sta_connected()) { cJSON_AddBoolToObject(out, "ok", false); cJSON_AddStringToObject(out, "error", "device is not connected to Wi-Fi yet"); }
    else {
        bool anthropic = c->inference.provider == CFG_PROVIDER_ANTHROPIC;
        history_t h; history_init(&h, 4096);
        history_add_user(&h, "Reply with the single word: pong");
        char* body = anthropic ? anthropic_build_request(c->inference.model, NULL, &h, NULL, 0, false, 8)
                               : openai_build_request(c->inference.model, NULL, &h, NULL, 0, false, 8);
        history_clear(&h);
        char url[CFG_URL + 32];
        snprintf(url, sizeof(url), "%s%s", c->inference.base_url, anthropic ? "/messages" : "/chat/completions");
        char auth[CFG_SECRET + 16];
        http_header_t hdrs[5]; int nh = 0;
        hdrs[nh++] = (http_header_t){"Content-Type", "application/json"};
        if (anthropic) { hdrs[nh++] = (http_header_t){"x-api-key", c->inference.api_key}; hdrs[nh++] = (http_header_t){"anthropic-version", ANTHROPIC_VERSION}; }
        else if (c->inference.api_key[0]) { snprintf(auth, sizeof(auth), "Bearer %s", c->inference.api_key); hdrs[nh++] = (http_header_t){"Authorization", auth}; }
        http_stream_req_t req = {.url = url, .method = "POST", .headers = hdrs, .n_headers = nh, .body = body, .body_len = strlen(body), .connect_timeout_ms = 15000, .read_timeout_ms = 30000, .total_timeout_ms = 45000};
        http_stream_res_t res; char* resp = NULL;
        esp_err_t err = http_fetch(&req, &res, &resp, 64 * 1024);
        plat_free(body);
        jsonbuf_t text; jb_init(&text); char perr[160] = "";
        if (err != ESP_OK) { cJSON_AddBoolToObject(out, "ok", false); cJSON_AddStringToObject(out, "error", res.error[0] ? res.error : esp_err_to_name(err)); }
        else {
            bool ok = anthropic ? anthropic_parse_response(resp ? resp : "", &text, perr, sizeof(perr)) : openai_parse_response(resp ? resp : "", &text, perr, sizeof(perr));
            if (ok) { cJSON_AddBoolToObject(out, "ok", true); cJSON_AddStringToObject(out, "reply", jb_cstr(&text)); }
            else { char e[220]; snprintf(e, sizeof(e), "HTTP %d: %s", res.status, perr); cJSON_AddBoolToObject(out, "ok", false); cJSON_AddStringToObject(out, "error", e); }
        }
        jb_free(&text);
        plat_free(resp);
    }
    char* s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s;
}

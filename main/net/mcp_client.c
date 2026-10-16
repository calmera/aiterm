// MCP Streamable HTTP client (§7): JSON-RPC over POST, application/json or text/event-stream
// responses, Mcp-Session-Id, MCP-Protocol-Version, tools/list pagination, per-server cache in
// /storage so tools are available right after boot while a refresh runs.
#include "mcp_client.h"
#include <stdio.h>
#include <string.h>
#include "agent/tools.h"
#include "app_config.h"
#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "http_stream.h"
#include "mcp_proto.h"
#include "sdkconfig.h"
#include "util/jsonbuf.h"
#include "util/platform.h"

static const char* TAG = "mcp";

typedef struct {
    char session[128];
    char protocol[32];
    bool initialized;
    int  next_id;
    int  tool_count;
    char last_error[96];
} server_state_t;

static server_state_t    s_state[CFG_MAX_MCP];
static SemaphoreHandle_t s_lock = NULL;

static void lock(void) { if (!s_lock) s_lock = xSemaphoreCreateMutex(); xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }

typedef struct { char session[128]; } hdr_ctx_t;
static void on_header(const char* k, const char* v, void* user) {
    hdr_ctx_t* c = (hdr_ctx_t*)user;
    if (strcasecmp(k, "Mcp-Session-Id") == 0) strlcpy(c->session, v, sizeof(c->session));
}

// One JSON-RPC round trip. Returns the matching response message (caller deletes) or NULL with err.
static cJSON* rpc(const char* url, const char* auth_header, const char* auth_value, server_state_t* st,
                  const char* body, int id, int timeout_ms, char* err, size_t err_len, int* http_status) {
    http_header_t hdrs[6]; int nh = 0;
    hdrs[nh++] = (http_header_t){"Content-Type", "application/json"};
    hdrs[nh++] = (http_header_t){"Accept", "application/json, text/event-stream"};
    if (auth_header && *auth_header && auth_value && *auth_value) hdrs[nh++] = (http_header_t){auth_header, auth_value};
    if (st->session[0]) hdrs[nh++] = (http_header_t){"Mcp-Session-Id", st->session};
    if (st->protocol[0]) hdrs[nh++] = (http_header_t){"MCP-Protocol-Version", st->protocol};
    hdr_ctx_t hc = {0};
    http_stream_req_t req = {.url = url, .method = "POST", .headers = hdrs, .n_headers = nh, .body = body, .body_len = strlen(body),
                             .connect_timeout_ms = 15000, .read_timeout_ms = timeout_ms, .total_timeout_ms = timeout_ms,
                             .on_header = on_header, .user = &hc};
    http_stream_res_t res; char* resp = NULL;
    esp_err_t e = http_fetch(&req, &res, &resp, 1u << 20);
    if (http_status) *http_status = res.status;
    if (hc.session[0]) strlcpy(st->session, hc.session, sizeof(st->session));
    if (e != ESP_OK) { snprintf(err, err_len, "%s", res.error[0] ? res.error : esp_err_to_name(e)); plat_free(resp); return NULL; }
    if (res.status == 202 || res.status == 204) { plat_free(resp); return NULL; }  // notification accepted
    if (res.status >= 400) {
        snprintf(err, err_len, "HTTP %d%s%.60s", res.status, resp && *resp ? ": " : "", resp && *resp ? resp : "");
        plat_free(resp);
        return NULL;
    }
    if (id < 0) { plat_free(resp); return NULL; }
    cJSON* msg = mcp_extract_response(resp ? resp : "", resp ? strlen(resp) : 0, res.content_type, id);
    plat_free(resp);
    if (!msg) { snprintf(err, err_len, "no JSON-RPC response for id %d", id); return NULL; }
    if (mcp_response_is_error(msg, err, err_len)) { cJSON_Delete(msg); return NULL; }
    return msg;
}

static bool do_initialize(const char* url, const char* ah, const char* av, server_state_t* st, char* err, size_t err_len) {
    st->session[0] = 0; st->protocol[0] = 0; st->initialized = false;
    int id = ++st->next_id;
    char* body = mcp_req_initialize(id, "ai-terminal", CONFIG_AITERM_FW_VERSION);
    int status = 0;
    cJSON* msg = rpc(url, ah, av, st, body, id, 20000, err, err_len, &status);
    plat_free(body);
    if (!msg) return false;
    char ver[32] = "";
    if (mcp_initialize_protocol_version(mcp_response_result(msg), ver, sizeof(ver))) strlcpy(st->protocol, ver, sizeof(st->protocol));
    else strlcpy(st->protocol, MCP_PROTOCOL_VERSION, sizeof(st->protocol));
    cJSON_Delete(msg);
    body = mcp_req_initialized_notification();
    char ignored[64];
    rpc(url, ah, av, st, body, -1, 10000, ignored, sizeof(ignored), NULL);
    plat_free(body);
    st->initialized = true;
    return true;
}

typedef struct { int server; const char* server_name; cJSON* cache; int count; } list_ctx_t;
static void add_tool_cb(const char* name, const char* desc, const char* schema, void* user) {
    list_ctx_t* c = (list_ctx_t*)user;
    if (c->server >= 0) tools_add_mcp(c->server, c->server_name, name, desc, schema);
    if (c->cache) {
        cJSON* t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "name", name);
        cJSON_AddStringToObject(t, "description", desc);
        cJSON* s = cJSON_Parse(schema);
        if (s) cJSON_AddItemToObject(t, "inputSchema", s);
        cJSON_AddItemToArray(c->cache, t);
    }
    c->count++;
}

// Fetch all tool pages. If ctx->server >= 0 registers tools; always fills ctx->cache when non-NULL.
static bool do_tools_list(const char* url, const char* ah, const char* av, server_state_t* st, list_ctx_t* ctx, char* err, size_t err_len) {
    char cursor[128] = "";
    int pages = 0;
    do {
        int id = ++st->next_id;
        char* body = mcp_req_tools_list(id, cursor[0] ? cursor : NULL);
        int status = 0;
        cJSON* msg = rpc(url, ah, av, st, body, id, 20000, err, err_len, &status);
        plat_free(body);
        if (!msg) return false;
        char next[128] = "";
        mcp_parse_tools_list(mcp_response_result(msg), add_tool_cb, ctx, next, sizeof(next));
        cJSON_Delete(msg);
        strlcpy(cursor, next, sizeof(cursor));
    } while (cursor[0] && ++pages < 20);
    return true;
}

// ---------------------------------------------------------------------------------------
// Cache: /storage/mcp_<index>.json holds {"url":..., "tools":[...]}
// ---------------------------------------------------------------------------------------
static void cache_path(int i, char* out, size_t n) { snprintf(out, n, "/storage/mcp_%d.json", i); }
static void cache_write(int i, const char* url, cJSON* tools) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "url", url);
    cJSON_AddItemReferenceToObject(root, "tools", tools);
    char* txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return;
    char p[48]; cache_path(i, p, sizeof(p));
    FILE* f = fopen(p, "w");
    if (f) { fwrite(txt, 1, strlen(txt), f); fclose(f); }
    free(txt);
}
static int cache_load(int i, const char* url, const char* server_name) {
    char p[48]; cache_path(i, p, sizeof(p));
    FILE* f = fopen(p, "r");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 512 * 1024) { fclose(f); return 0; }
    char* buf = (char*)plat_malloc((size_t)n + 1);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)n, f); buf[rd] = 0; fclose(f);
    cJSON* root = cJSON_Parse(buf);
    plat_free(buf);
    if (!root) return 0;
    cJSON* u = cJSON_GetObjectItemCaseSensitive(root, "url");
    int count = 0;
    if (cJSON_IsString(u) && strcmp(u->valuestring, url) == 0) {
        cJSON* result = cJSON_CreateObject();
        cJSON_AddItemReferenceToObject(result, "tools", cJSON_GetObjectItemCaseSensitive(root, "tools"));
        list_ctx_t ctx = {.server = i, .server_name = server_name, .cache = NULL};
        count = mcp_parse_tools_list(result, add_tool_cb, &ctx, NULL, 0);
        cJSON_Delete(result);
    }
    cJSON_Delete(root);
    return count;
}

// ---------------------------------------------------------------------------------------
void mcp_client_refresh_all(void) {
    lock();
    app_config_t* c = app_config();
    tools_clear_mcp();
    // 1) cached lists first so tools exist immediately
    for (int i = 0; i < c->mcp_count; i++) {
        memset(&s_state[i], 0, sizeof(s_state[i]));
        if (!c->mcp[i].enabled || !c->mcp[i].url[0]) continue;
        s_state[i].tool_count = cache_load(i, c->mcp[i].url, c->mcp[i].name[0] ? c->mcp[i].name : "mcp");
        if (s_state[i].tool_count) ESP_LOGI(TAG, "[%d] %s: %d cached tools", i, c->mcp[i].name, s_state[i].tool_count);
    }
    // 2) live refresh
    for (int i = 0; i < c->mcp_count; i++) {
        if (!c->mcp[i].enabled || !c->mcp[i].url[0]) continue;
        server_state_t* st = &s_state[i];
        char err[96] = "";
        if (!do_initialize(c->mcp[i].url, c->mcp[i].auth_header, c->mcp[i].auth_value, st, err, sizeof(err))) {
            ESP_LOGW(TAG, "[%d] %s: initialize failed: %s", i, c->mcp[i].name, err);
            strlcpy(st->last_error, err, sizeof(st->last_error));
            continue;
        }
        // fetch into a temporary cache; on success replace the registered tools for this server
        list_ctx_t ctx = {.server = -1, .server_name = c->mcp[i].name, .cache = cJSON_CreateArray()};
        if (!do_tools_list(c->mcp[i].url, c->mcp[i].auth_header, c->mcp[i].auth_value, st, &ctx, err, sizeof(err))) {
            ESP_LOGW(TAG, "[%d] %s: tools/list failed: %s", i, c->mcp[i].name, err);
            strlcpy(st->last_error, err, sizeof(st->last_error));
            cJSON_Delete(ctx.cache);
            continue;
        }
        cache_write(i, c->mcp[i].url, ctx.cache);
        cJSON_Delete(ctx.cache);
        // re-register: drop everything and reload all servers from cache (simple and consistent)
        tools_clear_mcp();
        for (int k = 0; k < c->mcp_count; k++) {
            if (!c->mcp[k].enabled || !c->mcp[k].url[0]) continue;
            s_state[k].tool_count = cache_load(k, c->mcp[k].url, c->mcp[k].name[0] ? c->mcp[k].name : "mcp");
        }
        st->last_error[0] = 0;
        ESP_LOGI(TAG, "[%d] %s: %d tools", i, c->mcp[i].name, st->tool_count);
    }
    unlock();
}

char* mcp_client_call(int server, const char* tool, const char* arguments_json, bool* is_error) {
    if (is_error) *is_error = true;
    app_config_t* c = app_config();
    if (server < 0 || server >= c->mcp_count) return plat_strdup("{\"error\":\"unknown MCP server\"}");
    lock();
    cfg_mcp_server_t* s = &c->mcp[server];
    server_state_t* st = &s_state[server];
    char err[96] = "";
    char* out = NULL;
    for (int attempt = 0; attempt < 2 && !out; attempt++) {
        if (!st->initialized && !do_initialize(s->url, s->auth_header, s->auth_value, st, err, sizeof(err))) break;
        int id = ++st->next_id;
        char* body = mcp_req_tools_call(id, tool, arguments_json);
        int status = 0;
        cJSON* msg = rpc(s->url, s->auth_header, s->auth_value, st, body, id, 120000, err, sizeof(err), &status);
        plat_free(body);
        if (!msg) {
            if (status == 404 || status == 400) { st->initialized = false; continue; }  // session expired: re-init once
            break;
        }
        bool ie = false;
        out = mcp_flatten_call_result(mcp_response_result(msg), &ie);
        cJSON_Delete(msg);
        if (is_error) *is_error = ie;
        if (ie) {  // wrap as an error object for the model
            jsonbuf_t j; jb_init(&j);
            jb_append_str(&j, "{\"error\":"); jb_append_json_string(&j, out); jb_append_char(&j, '}');
            plat_free(out); out = jb_detach(&j);
        }
    }
    unlock();
    if (!out) {
        jsonbuf_t j; jb_init(&j);
        jb_append_str(&j, "{\"error\":"); jb_append_json_string(&j, err[0] ? err : "MCP call failed"); jb_append_char(&j, '}');
        out = jb_detach(&j);
    }
    return out;
}

char* mcp_client_test(const char* url, const char* auth_header, const char* auth_value) {
    server_state_t st; memset(&st, 0, sizeof(st));
    cJSON* out = cJSON_CreateObject();
    char err[96] = "";
    if (!do_initialize(url, auth_header, auth_value, &st, err, sizeof(err))) {
        cJSON_AddBoolToObject(out, "ok", false); cJSON_AddStringToObject(out, "error", err);
    } else {
        list_ctx_t ctx = {.server = -1, .server_name = "test", .cache = cJSON_CreateArray()};
        if (!do_tools_list(url, auth_header, auth_value, &st, &ctx, err, sizeof(err))) {
            cJSON_AddBoolToObject(out, "ok", false); cJSON_AddStringToObject(out, "error", err);
            cJSON_Delete(ctx.cache);
        } else {
            cJSON_AddBoolToObject(out, "ok", true);
            cJSON_AddStringToObject(out, "protocol", st.protocol);
            cJSON* names = cJSON_AddArrayToObject(out, "tools");
            cJSON* t; cJSON_ArrayForEach(t, ctx.cache) { cJSON* n = cJSON_GetObjectItemCaseSensitive(t, "name"); if (cJSON_IsString(n)) cJSON_AddItemToArray(names, cJSON_CreateString(n->valuestring)); }
            cJSON_Delete(ctx.cache);
        }
    }
    char* s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s;
}

#include "mcp_proto.h"
#include <stdio.h>
#include <string.h>
#include "util/jsonbuf.h"
#include "util/platform.h"
#include "util/sse.h"

static char* dup_free(char* cjson_text) { char* out = plat_strdup(cjson_text ? cjson_text : ""); free(cjson_text); return out; }

static cJSON* rpc_base(int id, const char* method) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "jsonrpc", "2.0");
    if (id >= 0) cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddStringToObject(o, "method", method);
    return o;
}

char* mcp_req_initialize(int id, const char* client_name, const char* client_version) {
    cJSON* o = rpc_base(id, "initialize");
    cJSON* p = cJSON_AddObjectToObject(o, "params");
    cJSON_AddStringToObject(p, "protocolVersion", MCP_PROTOCOL_VERSION);
    cJSON_AddObjectToObject(p, "capabilities");
    cJSON* ci = cJSON_AddObjectToObject(p, "clientInfo");
    cJSON_AddStringToObject(ci, "name", client_name ? client_name : "ai-terminal");
    cJSON_AddStringToObject(ci, "version", client_version ? client_version : "0");
    char* t = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return dup_free(t);
}
char* mcp_req_initialized_notification(void) {
    cJSON* o = rpc_base(-1, "notifications/initialized");
    char* t = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return dup_free(t);
}
char* mcp_req_tools_list(int id, const char* cursor) {
    cJSON* o = rpc_base(id, "tools/list");
    cJSON* p = cJSON_AddObjectToObject(o, "params");
    if (cursor && *cursor) cJSON_AddStringToObject(p, "cursor", cursor);
    char* t = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return dup_free(t);
}
char* mcp_req_tools_call(int id, const char* name, const char* arguments_json) {
    cJSON* o = rpc_base(id, "tools/call");
    cJSON* p = cJSON_AddObjectToObject(o, "params");
    cJSON_AddStringToObject(p, "name", name);
    cJSON* args = arguments_json && *arguments_json ? cJSON_Parse(arguments_json) : NULL;
    if (!args || !cJSON_IsObject(args)) { cJSON_Delete(args); args = cJSON_CreateObject(); }
    cJSON_AddItemToObject(p, "arguments", args);
    char* t = cJSON_PrintUnformatted(o); cJSON_Delete(o);
    return dup_free(t);
}

// ---------------------------------------------------------------------------------------
typedef struct { int id; cJSON* found; } find_ctx_t;
static bool msg_has_id(const cJSON* msg, int id) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(msg, "id");
    if (cJSON_IsNumber(v)) return v->valueint == id;
    if (cJSON_IsString(v)) { char b[16]; snprintf(b, sizeof(b), "%d", id); return strcmp(v->valuestring, b) == 0; }
    return false;
}
static void consider(find_ctx_t* c, cJSON* msg) {
    if (c->found || !msg) { cJSON_Delete(msg); return; }
    if (cJSON_IsArray(msg)) {  // batch
        cJSON* it;
        cJSON_ArrayForEach(it, msg) {
            if (msg_has_id(it, c->id) && (cJSON_GetObjectItemCaseSensitive(it, "result") || cJSON_GetObjectItemCaseSensitive(it, "error"))) { c->found = cJSON_Duplicate(it, true); break; }
        }
        cJSON_Delete(msg);
        return;
    }
    if (cJSON_IsObject(msg) && msg_has_id(msg, c->id) && (cJSON_GetObjectItemCaseSensitive(msg, "result") || cJSON_GetObjectItemCaseSensitive(msg, "error"))) { c->found = msg; return; }
    cJSON_Delete(msg);  // notification / request from server / other id
}
static void sse_cb(const char* event, const char* data, void* user) {
    (void)event;
    find_ctx_t* c = (find_ctx_t*)user;
    if (c->found || !data || !*data) return;
    consider(c, cJSON_Parse(data));
}

cJSON* mcp_extract_response(const char* body, size_t len, const char* content_type, int id) {
    find_ctx_t ctx = {.id = id, .found = NULL};
    bool sse = content_type && strstr(content_type, "text/event-stream") != NULL;
    if (!sse && content_type && !strstr(content_type, "json")) {
        // unknown type: sniff
        sse = len > 5 && (strncmp(body, "data:", 5) == 0 || strncmp(body, "event:", 6) == 0 || strncmp(body, ":", 1) == 0);
    }
    if (!content_type) sse = len > 5 && (strncmp(body, "data:", 5) == 0 || strncmp(body, "event:", 6) == 0);
    if (sse) {
        sse_parser_t p;
        sse_init(&p, sse_cb, &ctx);
        sse_feed(&p, body, len);
        sse_finish(&p);
        sse_free(&p);
    } else {
        consider(&ctx, cJSON_ParseWithLength(body, len));
    }
    return ctx.found;
}

bool mcp_response_is_error(const cJSON* msg, char* err, size_t err_len) {
    const cJSON* e = cJSON_GetObjectItemCaseSensitive(msg, "error");
    if (!e) return false;
    const cJSON* m = cJSON_GetObjectItemCaseSensitive(e, "message");
    const cJSON* c = cJSON_GetObjectItemCaseSensitive(e, "code");
    if (cJSON_IsString(m)) snprintf(err, err_len, "%s%s%d%s", m->valuestring, cJSON_IsNumber(c) ? " (" : "", cJSON_IsNumber(c) ? c->valueint : 0, cJSON_IsNumber(c) ? ")" : "");
    else snprintf(err, err_len, "JSON-RPC error");
    return true;
}
cJSON* mcp_response_result(cJSON* msg) { return msg ? cJSON_GetObjectItemCaseSensitive(msg, "result") : NULL; }

int mcp_parse_tools_list(cJSON* result, mcp_tool_cb_t cb, void* user, char* next_cursor, size_t cursor_len) {
    if (next_cursor && cursor_len) next_cursor[0] = 0;
    if (!result) return 0;
    cJSON* tools = cJSON_GetObjectItemCaseSensitive(result, "tools");
    int n = 0;
    cJSON* t;
    cJSON_ArrayForEach(t, tools) {
        cJSON* name = cJSON_GetObjectItemCaseSensitive(t, "name");
        if (!cJSON_IsString(name)) continue;
        cJSON* desc = cJSON_GetObjectItemCaseSensitive(t, "description");
        cJSON* schema = cJSON_GetObjectItemCaseSensitive(t, "inputSchema");
        char* sj = schema ? cJSON_PrintUnformatted(schema) : NULL;
        if (cb) cb(name->valuestring, cJSON_IsString(desc) ? desc->valuestring : "", sj ? sj : "{\"type\":\"object\",\"properties\":{}}", user);
        free(sj);
        n++;
    }
    cJSON* nc = cJSON_GetObjectItemCaseSensitive(result, "nextCursor");
    if (cJSON_IsString(nc) && next_cursor && cursor_len) snprintf(next_cursor, cursor_len, "%s", nc->valuestring);
    return n;
}

char* mcp_flatten_call_result(cJSON* result, bool* is_error) {
    jsonbuf_t j; jb_init(&j);
    if (is_error) *is_error = false;
    if (!result) { jb_append_str(&j, "(empty result)"); return jb_detach(&j); }
    cJSON* ie = cJSON_GetObjectItemCaseSensitive(result, "isError");
    if (cJSON_IsTrue(ie) && is_error) *is_error = true;
    cJSON* content = cJSON_GetObjectItemCaseSensitive(result, "content");
    cJSON* item;
    cJSON_ArrayForEach(item, content) {
        cJSON* type = cJSON_GetObjectItemCaseSensitive(item, "type");
        const char* ty = cJSON_IsString(type) ? type->valuestring : "";
        if (strcmp(ty, "text") == 0) {
            cJSON* txt = cJSON_GetObjectItemCaseSensitive(item, "text");
            if (cJSON_IsString(txt)) { if (j.len) jb_append_char(&j, '\n'); jb_append_str(&j, txt->valuestring); }
        } else if (strcmp(ty, "image") == 0 || strcmp(ty, "audio") == 0) {
            cJSON* mime = cJSON_GetObjectItemCaseSensitive(item, "mimeType");
            cJSON* data = cJSON_GetObjectItemCaseSensitive(item, "data");
            size_t bytes = cJSON_IsString(data) ? strlen(data->valuestring) * 3 / 4 : 0;
            if (j.len) jb_append_char(&j, '\n');
            jb_appendf(&j, "[%s %s, %u bytes]", ty, cJSON_IsString(mime) ? mime->valuestring : "?", (unsigned)bytes);
        } else if (strcmp(ty, "resource") == 0 || strcmp(ty, "resource_link") == 0) {
            cJSON* res = cJSON_GetObjectItemCaseSensitive(item, "resource");
            cJSON* txt = res ? cJSON_GetObjectItemCaseSensitive(res, "text") : NULL;
            cJSON* uri = res ? cJSON_GetObjectItemCaseSensitive(res, "uri") : cJSON_GetObjectItemCaseSensitive(item, "uri");
            if (j.len) jb_append_char(&j, '\n');
            if (cJSON_IsString(txt)) jb_append_str(&j, txt->valuestring);
            else jb_appendf(&j, "[resource %s]", cJSON_IsString(uri) ? uri->valuestring : "?");
        }
    }
    if (j.len == 0) {
        cJSON* sc = cJSON_GetObjectItemCaseSensitive(result, "structuredContent");
        if (sc) { char* t = cJSON_PrintUnformatted(sc); if (t) { jb_append_str(&j, t); free(t); } }
    }
    if (j.len == 0) jb_append_str(&j, cJSON_IsTrue(ie) ? "(tool error)" : "(empty result)");
    return jb_detach(&j);
}

bool mcp_initialize_protocol_version(cJSON* result, char* out, size_t out_len) {
    cJSON* v = result ? cJSON_GetObjectItemCaseSensitive(result, "protocolVersion") : NULL;
    if (!cJSON_IsString(v)) return false;
    snprintf(out, out_len, "%s", v->valuestring);
    return true;
}

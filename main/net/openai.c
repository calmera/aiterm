#include "openai.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "util/platform.h"

// ---------------------------------------------------------------------------------------
// Request
// ---------------------------------------------------------------------------------------
static void add_raw_json(cJSON* parent, const char* key, const char* json_text) {
    cJSON* v = json_text ? cJSON_Parse(json_text) : NULL;
    if (!v) v = cJSON_CreateObject();
    cJSON_AddItemToObject(parent, key, v);
}

char* openai_build_request(const char* model, const char* system_prompt, const history_t* h,
                           const tool_def_t* tools, int n_tools, bool stream, int max_tokens) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model ? model : "");
    cJSON* msgs = cJSON_AddArrayToObject(root, "messages");
    if (system_prompt && *system_prompt) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", "system");
        cJSON_AddStringToObject(m, "content", system_prompt);
        cJSON_AddItemToArray(msgs, m);
    }
    for (const msg_t* m = h->head; m; m = m->next) {
        cJSON* o = cJSON_CreateObject();
        switch (m->role) {
            case MSG_USER:
                cJSON_AddStringToObject(o, "role", "user");
                cJSON_AddStringToObject(o, "content", m->content ? m->content : "");
                break;
            case MSG_ASSISTANT:
                cJSON_AddStringToObject(o, "role", "assistant");
                if (m->content && *m->content) cJSON_AddStringToObject(o, "content", m->content);
                else if (m->n_tool_calls == 0) cJSON_AddStringToObject(o, "content", "");
                else cJSON_AddNullToObject(o, "content");
                if (m->n_tool_calls > 0) {
                    cJSON* tcs = cJSON_AddArrayToObject(o, "tool_calls");
                    for (int i = 0; i < m->n_tool_calls; i++) {
                        cJSON* tc = cJSON_CreateObject();
                        cJSON_AddStringToObject(tc, "id", m->tool_calls[i].id ? m->tool_calls[i].id : "");
                        cJSON_AddStringToObject(tc, "type", "function");
                        cJSON* fn = cJSON_AddObjectToObject(tc, "function");
                        cJSON_AddStringToObject(fn, "name", m->tool_calls[i].name ? m->tool_calls[i].name : "");
                        cJSON_AddStringToObject(fn, "arguments", m->tool_calls[i].arguments && *m->tool_calls[i].arguments ? m->tool_calls[i].arguments : "{}");
                        cJSON_AddItemToArray(tcs, tc);
                    }
                }
                break;
            case MSG_TOOL:
                cJSON_AddStringToObject(o, "role", "tool");
                cJSON_AddStringToObject(o, "tool_call_id", m->tool_call_id ? m->tool_call_id : "");
                cJSON_AddStringToObject(o, "content", m->content ? m->content : "");
                break;
        }
        cJSON_AddItemToArray(msgs, o);
    }
    if (n_tools > 0) {
        cJSON* ts = cJSON_AddArrayToObject(root, "tools");
        for (int i = 0; i < n_tools; i++) {
            cJSON* t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "type", "function");
            cJSON* fn = cJSON_AddObjectToObject(t, "function");
            cJSON_AddStringToObject(fn, "name", tools[i].name);
            cJSON_AddStringToObject(fn, "description", tools[i].description ? tools[i].description : "");
            add_raw_json(fn, "parameters", tools[i].schema_json);
            cJSON_AddItemToArray(ts, t);
        }
        cJSON_AddStringToObject(root, "tool_choice", "auto");
    }
    if (max_tokens > 0) cJSON_AddNumberToObject(root, "max_tokens", max_tokens);
    if (stream) {
        cJSON_AddBoolToObject(root, "stream", true);
        cJSON* so = cJSON_AddObjectToObject(root, "stream_options");
        cJSON_AddBoolToObject(so, "include_usage", true);
    }
    char* txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    char* out = plat_strdup(txt);
    free(txt);
    return out;
}

// ---------------------------------------------------------------------------------------
// Streaming
// ---------------------------------------------------------------------------------------
static void sse_cb(const char* event, const char* data, void* user) {
    (void)event;
    openai_handle_event((llm_stream_t*)user, data);
}

void llm_stream_init(llm_stream_t* s, const llm_stream_cb_t* cb, void* user) {
    memset(s, 0, sizeof(*s));
    if (cb) s->cb = *cb;
    s->user = user;
    jb_init(&s->text);
    sse_init(&s->sse, sse_cb, s);
}
void llm_stream_free(llm_stream_t* s) {
    jb_free(&s->text);
    for (int i = 0; i < s->n_calls; i++) { plat_free(s->calls[i].id); plat_free(s->calls[i].name); jb_free(&s->calls[i].arguments); }
    s->n_calls = 0;
    sse_free(&s->sse);
}
int llm_stream_take_tool_calls(llm_stream_t* s, tool_call_t** out) {
    *out = NULL;
    if (s->n_calls == 0) return 0;
    tool_call_t* arr = (tool_call_t*)plat_calloc(s->n_calls, sizeof(tool_call_t));
    if (!arr) return 0;
    int n = s->n_calls;
    for (int i = 0; i < n; i++) {
        arr[i].id = s->calls[i].id ? s->calls[i].id : plat_strdup("");
        arr[i].name = s->calls[i].name ? s->calls[i].name : plat_strdup("");
        arr[i].arguments = s->calls[i].arguments.len ? jb_detach(&s->calls[i].arguments) : plat_strdup("{}");
        jb_free(&s->calls[i].arguments);
        s->calls[i].id = NULL; s->calls[i].name = NULL;
    }
    s->n_calls = 0;
    *out = arr;
    return n;
}

static llm_tool_acc_t* acc_for_index(llm_stream_t* s, int index) {
    for (int i = 0; i < s->n_calls; i++) if (s->calls[i].index == index) return &s->calls[i];
    if (s->n_calls >= LLM_MAX_TOOL_CALLS) return NULL;
    llm_tool_acc_t* a = &s->calls[s->n_calls++];
    memset(a, 0, sizeof(*a));
    a->index = index;
    jb_init(&a->arguments);
    return a;
}

static void set_error(llm_stream_t* s, const char* msg) {
    s->got_error = true;
    snprintf(s->error, sizeof(s->error), "%s", msg ? msg : "unknown error");
    if (s->cb.on_error) s->cb.on_error(s->error, s->user);
}

void openai_handle_event(llm_stream_t* s, const char* data) {
    if (!data) return;
    while (*data == ' ') data++;
    if (strcmp(data, "[DONE]") == 0) { s->done = true; return; }
    cJSON* root = cJSON_Parse(data);
    if (!root) return;  // ignore garbage / keep-alives
    cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (err) {
        cJSON* m = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "message") : err;
        set_error(s, cJSON_IsString(m) ? m->valuestring : "server error");
        cJSON_Delete(root);
        return;
    }
    cJSON* model = cJSON_GetObjectItemCaseSensitive(root, "model");
    if (cJSON_IsString(model) && !s->model[0]) snprintf(s->model, sizeof(s->model), "%s", model->valuestring);
    cJSON* usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
    if (cJSON_IsObject(usage)) {
        cJSON* pi = cJSON_GetObjectItemCaseSensitive(usage, "prompt_tokens");
        cJSON* co = cJSON_GetObjectItemCaseSensitive(usage, "completion_tokens");
        if (cJSON_IsNumber(pi)) s->usage_in = pi->valueint;
        if (cJSON_IsNumber(co)) s->usage_out = co->valueint;
    }
    cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON* ch = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    if (ch) {
        cJSON* fr = cJSON_GetObjectItemCaseSensitive(ch, "finish_reason");
        if (cJSON_IsString(fr)) snprintf(s->finish_reason, sizeof(s->finish_reason), "%s", fr->valuestring);
        cJSON* delta = cJSON_GetObjectItemCaseSensitive(ch, "delta");
        if (!delta) delta = cJSON_GetObjectItemCaseSensitive(ch, "message");  // non-streaming shape
        if (delta) {
            cJSON* content = cJSON_GetObjectItemCaseSensitive(delta, "content");
            if (cJSON_IsString(content) && content->valuestring[0]) {
                jb_append_str(&s->text, content->valuestring);
                if (s->cb.on_text) s->cb.on_text(content->valuestring, s->user);
            }
            cJSON* tcs = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls");
            if (cJSON_IsArray(tcs)) {
                int pos = 0;
                cJSON* tc;
                cJSON_ArrayForEach(tc, tcs) {
                    cJSON* idx = cJSON_GetObjectItemCaseSensitive(tc, "index");
                    int index = cJSON_IsNumber(idx) ? idx->valueint : pos;
                    pos++;
                    llm_tool_acc_t* a = acc_for_index(s, index);
                    if (!a) continue;
                    cJSON* id = cJSON_GetObjectItemCaseSensitive(tc, "id");
                    if (cJSON_IsString(id) && id->valuestring[0] && !a->id) a->id = plat_strdup(id->valuestring);
                    cJSON* fn = cJSON_GetObjectItemCaseSensitive(tc, "function");
                    if (fn) {
                        cJSON* name = cJSON_GetObjectItemCaseSensitive(fn, "name");
                        if (cJSON_IsString(name) && name->valuestring[0] && !a->name) {
                            a->name = plat_strdup(name->valuestring);
                            if (s->cb.on_tool_call_begin) s->cb.on_tool_call_begin(a->id ? a->id : "", a->name, s->user);
                        }
                        cJSON* args = cJSON_GetObjectItemCaseSensitive(fn, "arguments");
                        if (cJSON_IsString(args)) jb_append_str(&a->arguments, args->valuestring);
                        else if (cJSON_IsObject(args)) {  // some servers send parsed objects
                            char* t = cJSON_PrintUnformatted(args);
                            if (t) { jb_clear(&a->arguments); jb_append_str(&a->arguments, t); free(t); }
                        }
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}

void openai_stream_feed(llm_stream_t* s, const char* buf, size_t n) { sse_feed(&s->sse, buf, n); }
void openai_stream_finish(llm_stream_t* s) {
    sse_finish(&s->sse);
    // Some servers (and non-SSE error bodies) send a bare JSON object without "data:".
    if (!s->done && !s->got_error && s->text.len == 0 && s->n_calls == 0 && s->sse.line.len == 0) { /* nothing */ }
    // Tool calls without ids (Ollama) get synthetic ids.
    for (int i = 0; i < s->n_calls; i++) {
        if (!s->calls[i].id || !s->calls[i].id[0]) {
            char tmp[32]; snprintf(tmp, sizeof(tmp), "call_%d", i);
            plat_free(s->calls[i].id); s->calls[i].id = plat_strdup(tmp);
        }
    }
}

bool openai_parse_response(const char* body, jsonbuf_t* text, char* err, size_t err_len) {
    cJSON* root = cJSON_Parse(body);
    if (!root) { snprintf(err, err_len, "response is not JSON"); return false; }
    cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (e) {
        cJSON* m = cJSON_IsObject(e) ? cJSON_GetObjectItemCaseSensitive(e, "message") : e;
        snprintf(err, err_len, "%s", cJSON_IsString(m) ? m->valuestring : "server error");
        cJSON_Delete(root);
        return false;
    }
    cJSON* choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    cJSON* ch = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON* msg = ch ? cJSON_GetObjectItemCaseSensitive(ch, "message") : NULL;
    cJSON* content = msg ? cJSON_GetObjectItemCaseSensitive(msg, "content") : NULL;
    if (cJSON_IsString(content)) jb_append_str(text, content->valuestring);
    bool ok = ch != NULL;
    if (!ok) snprintf(err, err_len, "no choices in response");
    cJSON_Delete(root);
    return ok;
}

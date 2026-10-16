#include "anthropic.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "util/platform.h"

static void add_raw_json(cJSON* parent, const char* key, const char* json_text) {
    cJSON* v = json_text ? cJSON_Parse(json_text) : NULL;
    if (!v) { v = cJSON_CreateObject(); cJSON_AddStringToObject(v, "type", "object"); }
    cJSON_AddItemToObject(parent, key, v);
}

char* anthropic_build_request(const char* model, const char* system_prompt, const history_t* h,
                              const tool_def_t* tools, int n_tools, bool stream, int max_tokens) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", model ? model : "");
    cJSON_AddNumberToObject(root, "max_tokens", max_tokens > 0 ? max_tokens : 4096);
    if (system_prompt && *system_prompt) cJSON_AddStringToObject(root, "system", system_prompt);
    cJSON* msgs = cJSON_AddArrayToObject(root, "messages");
    // Consecutive tool results must be merged into one user message; consecutive same-role
    // messages are merged too (Anthropic requires alternation).
    cJSON* pending_user_blocks = NULL;  // array of content blocks for a user message being built
    for (const msg_t* m = h->head; m; m = m->next) {
        if (m->role == MSG_USER || m->role == MSG_TOOL) {
            if (!pending_user_blocks) pending_user_blocks = cJSON_CreateArray();
            if (m->role == MSG_TOOL) {
                cJSON* b = cJSON_CreateObject();
                cJSON_AddStringToObject(b, "type", "tool_result");
                cJSON_AddStringToObject(b, "tool_use_id", m->tool_call_id ? m->tool_call_id : "");
                cJSON_AddStringToObject(b, "content", m->content && *m->content ? m->content : "(empty)");
                if (m->is_error) cJSON_AddBoolToObject(b, "is_error", true);
                cJSON_AddItemToArray(pending_user_blocks, b);
            } else if (m->content && *m->content) {
                cJSON* b = cJSON_CreateObject();
                cJSON_AddStringToObject(b, "type", "text");
                cJSON_AddStringToObject(b, "text", m->content);
                cJSON_AddItemToArray(pending_user_blocks, b);
            }
            continue;
        }
        // assistant: flush pending user content first
        if (pending_user_blocks) {
            if (cJSON_GetArraySize(pending_user_blocks) == 0) { cJSON_Delete(pending_user_blocks); }
            else { cJSON* u = cJSON_CreateObject(); cJSON_AddStringToObject(u, "role", "user"); cJSON_AddItemToObject(u, "content", pending_user_blocks); cJSON_AddItemToArray(msgs, u); }
            pending_user_blocks = NULL;
        }
        cJSON* a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "role", "assistant");
        cJSON* blocks = cJSON_AddArrayToObject(a, "content");
        if (m->content && *m->content) {
            cJSON* b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "type", "text");
            cJSON_AddStringToObject(b, "text", m->content);
            cJSON_AddItemToArray(blocks, b);
        }
        for (int i = 0; i < m->n_tool_calls; i++) {
            cJSON* b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "type", "tool_use");
            cJSON_AddStringToObject(b, "id", m->tool_calls[i].id ? m->tool_calls[i].id : "");
            cJSON_AddStringToObject(b, "name", m->tool_calls[i].name ? m->tool_calls[i].name : "");
            add_raw_json(b, "input", m->tool_calls[i].arguments);
            cJSON_AddItemToArray(blocks, b);
        }
        if (cJSON_GetArraySize(blocks) == 0) {  // never send an empty assistant message
            cJSON* b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "type", "text");
            cJSON_AddStringToObject(b, "text", "(no output)");
            cJSON_AddItemToArray(blocks, b);
        }
        cJSON_AddItemToArray(msgs, a);
    }
    if (pending_user_blocks) {
        if (cJSON_GetArraySize(pending_user_blocks) == 0) { cJSON_Delete(pending_user_blocks); }
        else { cJSON* u = cJSON_CreateObject(); cJSON_AddStringToObject(u, "role", "user"); cJSON_AddItemToObject(u, "content", pending_user_blocks); cJSON_AddItemToArray(msgs, u); }
    }
    if (n_tools > 0) {
        cJSON* ts = cJSON_AddArrayToObject(root, "tools");
        for (int i = 0; i < n_tools; i++) {
            cJSON* t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "name", tools[i].name);
            cJSON_AddStringToObject(t, "description", tools[i].description ? tools[i].description : "");
            add_raw_json(t, "input_schema", tools[i].schema_json);
            cJSON_AddItemToArray(ts, t);
        }
    }
    if (stream) cJSON_AddBoolToObject(root, "stream", true);
    char* txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    char* out = plat_strdup(txt);
    free(txt);
    return out;
}

// ---------------------------------------------------------------------------------------
// Streaming. We track the content block index -> tool accumulator mapping via calls[].index.
// ---------------------------------------------------------------------------------------
static llm_tool_acc_t* acc_for_block(llm_stream_t* s, int index, bool create) {
    for (int i = 0; i < s->n_calls; i++) if (s->calls[i].index == index) return &s->calls[i];
    if (!create || s->n_calls >= LLM_MAX_TOOL_CALLS) return NULL;
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

void anthropic_handle_event(llm_stream_t* s, const char* event, const char* data) {
    cJSON* root = cJSON_Parse(data);
    if (!root) return;
    cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char* t = cJSON_IsString(type) ? type->valuestring : (event ? event : "");
    if (strcmp(t, "error") == 0) {
        cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON* m = e ? cJSON_GetObjectItemCaseSensitive(e, "message") : NULL;
        set_error(s, cJSON_IsString(m) ? m->valuestring : "server error");
    } else if (strcmp(t, "message_start") == 0) {
        cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "message");
        cJSON* model = msg ? cJSON_GetObjectItemCaseSensitive(msg, "model") : NULL;
        if (cJSON_IsString(model)) snprintf(s->model, sizeof(s->model), "%s", model->valuestring);
        cJSON* usage = msg ? cJSON_GetObjectItemCaseSensitive(msg, "usage") : NULL;
        cJSON* in = usage ? cJSON_GetObjectItemCaseSensitive(usage, "input_tokens") : NULL;
        if (cJSON_IsNumber(in)) s->usage_in = in->valueint;
    } else if (strcmp(t, "content_block_start") == 0) {
        cJSON* idx = cJSON_GetObjectItemCaseSensitive(root, "index");
        cJSON* cb = cJSON_GetObjectItemCaseSensitive(root, "content_block");
        cJSON* cbt = cb ? cJSON_GetObjectItemCaseSensitive(cb, "type") : NULL;
        if (cJSON_IsString(cbt) && strcmp(cbt->valuestring, "tool_use") == 0) {
            llm_tool_acc_t* a = acc_for_block(s, cJSON_IsNumber(idx) ? idx->valueint : s->n_calls, true);
            if (a) {
                cJSON* id = cJSON_GetObjectItemCaseSensitive(cb, "id");
                cJSON* name = cJSON_GetObjectItemCaseSensitive(cb, "name");
                a->id = plat_strdup(cJSON_IsString(id) ? id->valuestring : "");
                a->name = plat_strdup(cJSON_IsString(name) ? name->valuestring : "");
                if (s->cb.on_tool_call_begin) s->cb.on_tool_call_begin(a->id, a->name, s->user);
            }
        } else if (cJSON_IsString(cbt) && strcmp(cbt->valuestring, "text") == 0) {
            cJSON* text = cJSON_GetObjectItemCaseSensitive(cb, "text");
            if (cJSON_IsString(text) && text->valuestring[0]) {
                jb_append_str(&s->text, text->valuestring);
                if (s->cb.on_text) s->cb.on_text(text->valuestring, s->user);
            }
        }
    } else if (strcmp(t, "content_block_delta") == 0) {
        cJSON* idx = cJSON_GetObjectItemCaseSensitive(root, "index");
        cJSON* d = cJSON_GetObjectItemCaseSensitive(root, "delta");
        cJSON* dt = d ? cJSON_GetObjectItemCaseSensitive(d, "type") : NULL;
        if (cJSON_IsString(dt) && strcmp(dt->valuestring, "text_delta") == 0) {
            cJSON* text = cJSON_GetObjectItemCaseSensitive(d, "text");
            if (cJSON_IsString(text) && text->valuestring[0]) {
                jb_append_str(&s->text, text->valuestring);
                if (s->cb.on_text) s->cb.on_text(text->valuestring, s->user);
            }
        } else if (cJSON_IsString(dt) && strcmp(dt->valuestring, "input_json_delta") == 0) {
            llm_tool_acc_t* a = acc_for_block(s, cJSON_IsNumber(idx) ? idx->valueint : -1, false);
            cJSON* pj = cJSON_GetObjectItemCaseSensitive(d, "partial_json");
            if (a && cJSON_IsString(pj)) jb_append_str(&a->arguments, pj->valuestring);
        }
    } else if (strcmp(t, "message_delta") == 0) {
        cJSON* d = cJSON_GetObjectItemCaseSensitive(root, "delta");
        cJSON* sr = d ? cJSON_GetObjectItemCaseSensitive(d, "stop_reason") : NULL;
        if (cJSON_IsString(sr)) snprintf(s->finish_reason, sizeof(s->finish_reason), "%s", sr->valuestring);
        cJSON* usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
        cJSON* out = usage ? cJSON_GetObjectItemCaseSensitive(usage, "output_tokens") : NULL;
        if (cJSON_IsNumber(out)) s->usage_out = out->valueint;
    } else if (strcmp(t, "message_stop") == 0) {
        s->done = true;
    }
    cJSON_Delete(root);
}

static void sse_cb(const char* event, const char* data, void* user) { anthropic_handle_event((llm_stream_t*)user, event, data); }

void anthropic_stream_feed(llm_stream_t* s, const char* buf, size_t n) {
    s->sse.cb = sse_cb;  // llm_stream_init wires the OpenAI handler; swap it here
    sse_feed(&s->sse, buf, n);
}
void anthropic_stream_finish(llm_stream_t* s) {
    s->sse.cb = sse_cb;
    sse_finish(&s->sse);
    // Map Anthropic stop reasons onto the OpenAI vocabulary used by the agent loop.
    if (strcmp(s->finish_reason, "tool_use") == 0) snprintf(s->finish_reason, sizeof(s->finish_reason), "tool_calls");
    else if (strcmp(s->finish_reason, "end_turn") == 0 || strcmp(s->finish_reason, "stop_sequence") == 0) snprintf(s->finish_reason, sizeof(s->finish_reason), "stop");
    else if (strcmp(s->finish_reason, "max_tokens") == 0) snprintf(s->finish_reason, sizeof(s->finish_reason), "length");
}

bool anthropic_parse_response(const char* body, jsonbuf_t* text, char* err, size_t err_len) {
    cJSON* root = cJSON_Parse(body);
    if (!root) { snprintf(err, err_len, "response is not JSON"); return false; }
    cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (e) {
        cJSON* m = cJSON_GetObjectItemCaseSensitive(e, "message");
        snprintf(err, err_len, "%s", cJSON_IsString(m) ? m->valuestring : "server error");
        cJSON_Delete(root);
        return false;
    }
    cJSON* content = cJSON_GetObjectItemCaseSensitive(root, "content");
    bool ok = cJSON_IsArray(content);
    if (ok) {
        cJSON* b;
        cJSON_ArrayForEach(b, content) {
            cJSON* t = cJSON_GetObjectItemCaseSensitive(b, "text");
            if (cJSON_IsString(t)) jb_append_str(text, t->valuestring);
        }
    } else snprintf(err, err_len, "no content in response");
    cJSON_Delete(root);
    return ok;
}

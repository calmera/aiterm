#include "history.h"
#include <string.h>
#include "util/platform.h"

static size_t sz(const char* s) { return s ? strlen(s) : 0; }

void history_init(history_t* h, size_t budget) { memset(h, 0, sizeof(*h)); h->budget = budget ? budget : 96 * 1024; }

void tool_calls_free(tool_call_t* calls, int n) {
    if (!calls) return;
    for (int i = 0; i < n; i++) { plat_free(calls[i].id); plat_free(calls[i].name); plat_free(calls[i].arguments); }
    plat_free(calls);
}
static void msg_free(msg_t* m) {
    plat_free(m->content);
    tool_calls_free(m->tool_calls, m->n_tool_calls);
    plat_free(m->tool_call_id);
    plat_free(m->tool_name);
    plat_free(m);
}
void history_clear(history_t* h) {
    msg_t* m = h->head;
    while (m) { msg_t* n = m->next; msg_free(m); m = n; }
    size_t b = h->budget;
    memset(h, 0, sizeof(*h));
    h->budget = b;
}
static msg_t* append(history_t* h, msg_t* m) {
    m->bytes = 32 + sz(m->content) + sz(m->tool_call_id) + sz(m->tool_name);
    for (int i = 0; i < m->n_tool_calls; i++) m->bytes += 32 + sz(m->tool_calls[i].id) + sz(m->tool_calls[i].name) + sz(m->tool_calls[i].arguments);
    if (h->tail) h->tail->next = m; else h->head = m;
    h->tail = m;
    h->count++;
    h->bytes += m->bytes;
    history_trim(h);
    return m;
}
msg_t* history_add_user(history_t* h, const char* text) {
    msg_t* m = (msg_t*)plat_calloc(1, sizeof(msg_t));
    if (!m) return NULL;
    m->role = MSG_USER; m->content = plat_strdup(text ? text : "");
    return append(h, m);
}
msg_t* history_add_assistant(history_t* h, const char* text, tool_call_t* calls, int n_calls) {
    msg_t* m = (msg_t*)plat_calloc(1, sizeof(msg_t));
    if (!m) { tool_calls_free(calls, n_calls); return NULL; }
    m->role = MSG_ASSISTANT; m->content = text ? plat_strdup(text) : NULL;
    m->tool_calls = calls; m->n_tool_calls = calls ? n_calls : 0;
    return append(h, m);
}
msg_t* history_add_tool_result(history_t* h, const char* call_id, const char* name, const char* content, bool is_error) {
    msg_t* m = (msg_t*)plat_calloc(1, sizeof(msg_t));
    if (!m) return NULL;
    m->role = MSG_TOOL; m->tool_call_id = plat_strdup(call_id ? call_id : ""); m->tool_name = plat_strdup(name ? name : "");
    m->content = plat_strdup(content ? content : ""); m->is_error = is_error;
    return append(h, m);
}
static void drop_head(history_t* h) {
    msg_t* m = h->head;
    if (!m) return;
    h->head = m->next;
    if (!h->head) h->tail = NULL;
    h->count--; h->bytes -= m->bytes;
    msg_free(m);
}
void history_trim(history_t* h) {
    // Keep at least the last user message; drop from the front in groups:
    // an assistant message with tool calls is dropped together with the tool results after it.
    while (h->bytes > h->budget && h->count > 1 && h->head != h->tail) {
        msg_t* m = h->head;
        bool group = (m->role == MSG_ASSISTANT && m->n_tool_calls > 0);
        drop_head(h);
        if (group) while (h->head && h->head->role == MSG_TOOL && h->head != h->tail) drop_head(h);
        // never start the remaining history with orphan tool results
        while (h->head && h->head->role == MSG_TOOL && h->head != h->tail) drop_head(h);
    }
}

// Conversation data model shared by the agent loop and the provider wire formats.
// Host-testable (no ESP dependencies).
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { MSG_USER = 0, MSG_ASSISTANT, MSG_TOOL } msg_role_t;

typedef struct {
    char* id;         // provider call id
    char* name;       // tool name as sent to the model (may carry the MCP server prefix)
    char* arguments;  // JSON object text
} tool_call_t;

typedef struct msg {
    msg_role_t   role;
    char*        content;       // text; may be NULL
    tool_call_t* tool_calls;    // assistant only
    int          n_tool_calls;
    char*        tool_call_id;  // tool result only
    char*        tool_name;     // tool result only
    bool         is_error;      // tool result only
    size_t       bytes;         // approximate size, for the budget
    struct msg*  next;
} msg_t;

typedef struct {
    msg_t* head;
    msg_t* tail;
    int    count;
    size_t bytes;
    size_t budget;  // bytes; oldest turns are dropped when exceeded
} history_t;

typedef struct {
    const char* name;
    const char* description;
    const char* schema_json;  // JSON Schema object text
} tool_def_t;

void   history_init(history_t* h, size_t budget);
void   history_clear(history_t* h);
msg_t* history_add_user(history_t* h, const char* text);
// Takes ownership of `calls` (array and strings).
msg_t* history_add_assistant(history_t* h, const char* text, tool_call_t* calls, int n_calls);
msg_t* history_add_tool_result(history_t* h, const char* call_id, const char* name, const char* content, bool is_error);
// Drop the oldest messages until bytes <= budget. Never splits an assistant tool-call message
// from its tool results.
void   history_trim(history_t* h);
void   tool_calls_free(tool_call_t* calls, int n);

#ifdef __cplusplus
}
#endif

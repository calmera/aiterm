// Tool registry: the seven local render tools (always present) + tools discovered from MCP
// servers (prefixed "<server>__<tool>"). Produces the unified tool_def_t list for requests.
#pragma once
#include <stdbool.h>
#include "history.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TOOLS_MAX 96

typedef struct {
    char* name;         // name presented to the model
    char* description;
    char* schema;       // JSON schema text
    int   server;       // -1 for local render tools, else MCP server index
    char* remote_name;  // MCP: tool name without prefix
} tool_entry_t;

void tools_init(void);
void tools_clear_mcp(void);
bool tools_add_mcp(int server, const char* server_name, const char* tool, const char* description, const char* schema_json);
int  tools_count(void);
int  tools_mcp_count(void);
const tool_def_t* tools_defs(int* n);   // valid until the registry changes
const tool_entry_t* tools_find(const char* name);
bool tools_is_render(const char* name);
// The device paragraph prepended to the user's system prompt.
const char* tools_system_prompt_prefix(void);

#ifdef __cplusplus
}
#endif

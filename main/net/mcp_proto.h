// MCP Streamable HTTP protocol pieces that do not touch the network: JSON-RPC request
// builders, response body decoding (application/json or text/event-stream), tools/list and
// tools/call result parsing. Host-tested (host_tests/test_mcp.c).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_PROTOCOL_VERSION      "2025-06-18"
#define MCP_PROTOCOL_VERSION_ALT  "2025-03-26"

// ---- requests (return plat_malloc'd JSON text) ----------------------------------------
char* mcp_req_initialize(int id, const char* client_name, const char* client_version);
char* mcp_req_initialized_notification(void);
char* mcp_req_tools_list(int id, const char* cursor);
char* mcp_req_tools_call(int id, const char* name, const char* arguments_json);

// ---- responses ------------------------------------------------------------------------
// Extract the JSON-RPC response with the given id from a response body. content_type decides
// how the body is parsed: text/event-stream (SSE: each data: is a JSON-RPC message) or JSON
// (single message or an array). Returns the parsed message object (caller cJSON_Delete) or
// NULL when not found.
cJSON* mcp_extract_response(const char* body, size_t len, const char* content_type, int id);
// If the message carries "error", copies its message into err and returns true.
bool   mcp_response_is_error(const cJSON* msg, char* err, size_t err_len);
// Returns the "result" object of a message (borrowed) or NULL.
cJSON* mcp_response_result(cJSON* msg);

// tools/list: iterate tools; returns the number of tools, fills next_cursor (may be empty).
typedef void (*mcp_tool_cb_t)(const char* name, const char* description, const char* input_schema_json, void* user);
int    mcp_parse_tools_list(cJSON* result, mcp_tool_cb_t cb, void* user, char* next_cursor, size_t cursor_len);

// tools/call: flatten content[] into a text for the model. *is_error reflects isError.
// Returns plat_malloc'd text.
char*  mcp_flatten_call_result(cJSON* result, bool* is_error);

// Negotiated protocol version from an initialize result (copies into out). Returns false if missing.
bool   mcp_initialize_protocol_version(cJSON* result, char* out, size_t out_len);

#ifdef __cplusplus
}
#endif

// MCP Streamable HTTP client (§7). Phase 2 ships the interface with stubs; Phase 4 fills it.
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// (Re)connect all enabled servers from the config: initialize + tools/list, register tools.
// Runs on the calling task; may take a while. Uses the on-flash cache first.
void  mcp_client_refresh_all(void);
// Call a tool on server `server`. Returns a malloc'd (plat_malloc) result string for the model;
// *is_error set when the server reported isError or the call failed.
char* mcp_client_call(int server, const char* tool, const char* arguments_json, bool* is_error);
// Provisioning test: initialize + tools/list against an ad-hoc server. Returns malloc'd JSON
// {"ok":true,"tools":[...]} or {"ok":false,"error":"..."} (free with free()).
char* mcp_client_test(const char* url, const char* auth_header, const char* auth_value);

#ifdef __cplusplus
}
#endif

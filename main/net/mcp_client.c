// Phase 2 stub — replaced by the real Streamable HTTP client in Phase 4.
#include "mcp_client.h"
#include <stdio.h>
#include <string.h>
#include "util/platform.h"

void mcp_client_refresh_all(void) {}
char* mcp_client_call(int server, const char* tool, const char* arguments_json, bool* is_error) {
    (void)server; (void)arguments_json;
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"error\":\"MCP tool %s is not available\"}", tool ? tool : "?");
    if (is_error) *is_error = true;
    return plat_strdup(buf);
}
char* mcp_client_test(const char* url, const char* auth_header, const char* auth_value) {
    (void)url; (void)auth_header; (void)auth_value;
    return strdup("{\"ok\":false,\"error\":\"MCP client not built in this phase\"}");
}

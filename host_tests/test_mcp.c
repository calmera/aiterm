#include "test_util.h"
#include "net/mcp_proto.h"
#include "util/platform.h"

static int g_tools = 0; static char g_names[8][32]; static char g_schema0[128];
static void tool_cb(const char* name, const char* desc, const char* schema, void* u) { if (g_tools < 8) snprintf(g_names[g_tools], 32, "%s", name); if (g_tools == 0) snprintf(g_schema0, sizeof(g_schema0), "%s", schema); g_tools++; }

int main(void) {
    // requests
    char* r = mcp_req_initialize(1, "ai-terminal", "0.1.0");
    CHECK(strstr(r, "\"jsonrpc\":\"2.0\"") && strstr(r, "\"id\":1") && strstr(r, "\"method\":\"initialize\"") && strstr(r, "\"protocolVersion\":\"2025-06-18\"") && strstr(r, "\"clientInfo\":{\"name\":\"ai-terminal\",\"version\":\"0.1.0\"}") && strstr(r, "\"capabilities\":{}"));
    plat_free(r);
    r = mcp_req_initialized_notification();
    CHECK_STR(r, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    plat_free(r);
    r = mcp_req_tools_list(2, NULL);
    CHECK_STR(r, "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}");
    plat_free(r);
    r = mcp_req_tools_list(3, "abc");
    CHECK(strstr(r, "\"cursor\":\"abc\"") != NULL);
    plat_free(r);
    r = mcp_req_tools_call(4, "get_weather", "{\"city\":\"Oslo\"}");
    CHECK_STR(r, "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get_weather\",\"arguments\":{\"city\":\"Oslo\"}}}");
    plat_free(r);
    r = mcp_req_tools_call(5, "t", "not json");
    CHECK(strstr(r, "\"arguments\":{}") != NULL);
    plat_free(r);

    // JSON response
    const char* j1 = "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"a\",\"description\":\"A\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}}}},{\"name\":\"b\"}],\"nextCursor\":\"page2\"}}";
    cJSON* m = mcp_extract_response(j1, strlen(j1), "application/json", 2);
    CHECK(m != NULL);
    char err[128];
    CHECK(!mcp_response_is_error(m, err, sizeof(err)));
    char cursor[64];
    int n = mcp_parse_tools_list(mcp_response_result(m), tool_cb, NULL, cursor, sizeof(cursor));
    CHECK_INT(n, 2); CHECK_STR(g_names[0], "a"); CHECK_STR(g_names[1], "b"); CHECK_STR(cursor, "page2");
    CHECK_STR(g_schema0, "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"string\"}}}");
    cJSON_Delete(m);
    // wrong id -> NULL
    m = mcp_extract_response(j1, strlen(j1), "application/json", 7);
    CHECK(m == NULL);
    // SSE response with notifications before the matching response, split events
    const char* s1 = "event: message\ndata: {\"jsonrpc\":\"2.0\",\"method\":\"notifications/progress\",\"params\":{}}\n\n"
                     "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":9,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"hello\"},{\"type\":\"text\",\"text\":\"world\"},{\"type\":\"image\",\"mimeType\":\"image/png\",\"data\":\"AAAA\"}],\"isError\":false}}\n\n"
                     "event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":10,\"result\":{}}\n\n";
    m = mcp_extract_response(s1, strlen(s1), "text/event-stream; charset=utf-8", 9);
    CHECK(m != NULL);
    bool is_err = true;
    char* flat = mcp_flatten_call_result(mcp_response_result(m), &is_err);
    CHECK(!is_err);
    CHECK_STR(flat, "hello\nworld\n[image image/png, 3 bytes]");
    plat_free(flat);
    cJSON_Delete(m);
    // error response, string id
    const char* e1 = "{\"jsonrpc\":\"2.0\",\"id\":\"3\",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}";
    m = mcp_extract_response(e1, strlen(e1), "application/json", 3);
    CHECK(m != NULL);
    CHECK(mcp_response_is_error(m, err, sizeof(err)));
    CHECK_STR(err, "Method not found (-32601)");
    cJSON_Delete(m);
    // isError result
    const char* e2 = "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"boom\"}],\"isError\":true}}";
    m = mcp_extract_response(e2, strlen(e2), NULL, 4);
    CHECK(m != NULL);
    flat = mcp_flatten_call_result(mcp_response_result(m), &is_err);
    CHECK(is_err); CHECK_STR(flat, "boom");
    plat_free(flat); cJSON_Delete(m);
    // initialize version
    const char* i1 = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"protocolVersion\":\"2025-03-26\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"x\",\"version\":\"1\"}}}";
    m = mcp_extract_response(i1, strlen(i1), "application/json", 1);
    char ver[32];
    CHECK(mcp_initialize_protocol_version(mcp_response_result(m), ver, sizeof(ver)));
    CHECK_STR(ver, "2025-03-26");
    cJSON_Delete(m);
    // batch array body
    const char* b1 = "[{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"a\":1}},{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"b\":2}}]";
    m = mcp_extract_response(b1, strlen(b1), "application/json", 2);
    CHECK(m != NULL && cJSON_GetObjectItemCaseSensitive(mcp_response_result(m), "b") != NULL);
    cJSON_Delete(m);
    TEST_MAIN_END("test_mcp");
}

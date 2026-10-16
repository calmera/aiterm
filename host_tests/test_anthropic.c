#include "test_util.h"
#include "net/anthropic.h"
#include "util/platform.h"

static char g_text[1024]; static int g_begins = 0;
static void on_text(const char* d, void* u) { strncat(g_text, d, sizeof(g_text) - strlen(g_text) - 1); }
static void on_begin(const char* id, const char* name, void* u) { g_begins++; }

int main(void) {
    history_t h; history_init(&h, 96 * 1024);
    history_add_user(&h, "hi");
    tool_call_t* calls = plat_calloc(1, sizeof(tool_call_t));
    calls[0].id = plat_strdup("toolu_1"); calls[0].name = plat_strdup("render_table"); calls[0].arguments = plat_strdup("{\"columns\":[\"a\"],\"rows\":[[\"1\"]]}");
    history_add_assistant(&h, "Let me show that.", calls, 1);
    history_add_tool_result(&h, "toolu_1", "render_table", "{\"status\":\"shown\"}", false);
    history_add_user(&h, "thanks");
    tool_def_t tools[1] = {{"render_table", "show a table", "{\"type\":\"object\"}"}};
    char* body = anthropic_build_request("claude-sonnet-4-5", "SYS", &h, tools, 1, true, 4096);
    CHECK(strstr(body, "\"system\":\"SYS\"") != NULL);
    CHECK(strstr(body, "\"max_tokens\":4096") != NULL);
    CHECK(strstr(body, "\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"render_table\",\"input\":{\"columns\":[\"a\"]") != NULL);
    // tool result + following user text are merged into one user message
    CHECK(strstr(body, "{\"role\":\"user\",\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"toolu_1\",\"content\":\"{\\\"status\\\":\\\"shown\\\"}\"},{\"type\":\"text\",\"text\":\"thanks\"}]}") != NULL);
    CHECK(strstr(body, "\"tools\":[{\"name\":\"render_table\",\"description\":\"show a table\",\"input_schema\":{\"type\":\"object\"}}]") != NULL);
    CHECK(strstr(body, "\"role\":\"tool\"") == NULL);
    CHECK(strstr(body, "\"stream\":true") != NULL);
    plat_free(body);
    history_clear(&h);

    llm_stream_t s;
    llm_stream_cb_t cb = {.on_text = on_text, .on_tool_call_begin = on_begin};
    llm_stream_init(&s, &cb, NULL);
    const char* stream =
        "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"id\":\"msg_1\",\"model\":\"claude-sonnet-4-5\",\"usage\":{\"input_tokens\":25,\"output_tokens\":1}}}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
        "event: ping\ndata: {\"type\":\"ping\"}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Sure, \"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"here:\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
        "event: content_block_start\ndata: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01\",\"name\":\"render_diff\",\"input\":{}}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"old\\\": \\\"a\"}}\n\n"
        "event: content_block_delta\ndata: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\", \\\"new\\\": \\\"b\\\"}\"}}\n\n"
        "event: content_block_stop\ndata: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
        "event: message_delta\ndata: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\",\"stop_sequence\":null},\"usage\":{\"output_tokens\":42}}\n\n"
        "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";
    size_t L = strlen(stream);
    for (size_t i = 0; i < L; i += 11) anthropic_stream_feed(&s, stream + i, (i + 11 <= L) ? 11 : L - i);
    anthropic_stream_finish(&s);
    CHECK_STR(g_text, "Sure, here:");
    CHECK(s.done);
    CHECK_STR(s.finish_reason, "tool_calls");
    CHECK_STR(s.model, "claude-sonnet-4-5");
    CHECK_INT(s.usage_in, 25);
    CHECK_INT(s.usage_out, 42);
    CHECK_INT(g_begins, 1);
    tool_call_t* out = NULL;
    int n = llm_stream_take_tool_calls(&s, &out);
    CHECK_INT(n, 1);
    CHECK_STR(out[0].id, "toolu_01");
    CHECK_STR(out[0].name, "render_diff");
    CHECK_STR(out[0].arguments, "{\"old\": \"a\", \"new\": \"b\"}");
    tool_calls_free(out, n);
    llm_stream_free(&s);

    // error event
    llm_stream_init(&s, &cb, NULL);
    const char* e = "event: error\ndata: {\"type\":\"error\",\"error\":{\"type\":\"authentication_error\",\"message\":\"invalid x-api-key\"}}\n\n";
    anthropic_stream_feed(&s, e, strlen(e)); anthropic_stream_finish(&s);
    CHECK(s.got_error);
    CHECK_STR(s.error, "invalid x-api-key");
    llm_stream_free(&s);

    jsonbuf_t t; jb_init(&t); char err[128] = "";
    CHECK(anthropic_parse_response("{\"content\":[{\"type\":\"text\",\"text\":\"pong\"}]}", &t, err, sizeof(err)));
    CHECK_STR(jb_cstr(&t), "pong");
    jb_clear(&t);
    CHECK(!anthropic_parse_response("{\"type\":\"error\",\"error\":{\"message\":\"bad\"}}", &t, err, sizeof(err)));
    CHECK_STR(err, "bad");
    jb_free(&t);
    TEST_MAIN_END("test_anthropic");
}

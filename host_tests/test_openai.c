#include "test_util.h"
#include "net/openai.h"
#include "util/platform.h"

static char g_text[1024]; static int g_begins = 0; static char g_err[256];
static void on_text(const char* d, void* u) { strncat(g_text, d, sizeof(g_text) - strlen(g_text) - 1); }
static void on_begin(const char* id, const char* name, void* u) { g_begins++; }
static void on_error(const char* m, void* u) { snprintf(g_err, sizeof(g_err), "%s", m); }

int main(void) {
    // ---- request body
    history_t h; history_init(&h, 96 * 1024);
    history_add_user(&h, "hi");
    tool_call_t* calls = plat_calloc(1, sizeof(tool_call_t));
    calls[0].id = plat_strdup("call_1"); calls[0].name = plat_strdup("render_choice"); calls[0].arguments = plat_strdup("{\"prompt\":\"p\",\"options\":[\"a\"]}");
    history_add_assistant(&h, NULL, calls, 1);
    history_add_tool_result(&h, "call_1", "render_choice", "{\"selected\":[0]}", false);
    tool_def_t tools[1] = {{"render_choice", "pick", "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"}}}"}};
    char* body = openai_build_request("gpt-4.1-mini", "SYS", &h, tools, 1, true, 0);
    CHECK(body != NULL);
    CHECK(strstr(body, "\"role\":\"system\",\"content\":\"SYS\"") != NULL);
    CHECK(strstr(body, "\"role\":\"tool\",\"tool_call_id\":\"call_1\"") != NULL);
    CHECK(strstr(body, "\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"render_choice\",\"arguments\":\"{\\\"prompt\\\"") != NULL);
    CHECK(strstr(body, "\"tools\":[{\"type\":\"function\",\"function\":{\"name\":\"render_choice\",\"description\":\"pick\",\"parameters\":{\"type\":\"object\"") != NULL);
    CHECK(strstr(body, "\"stream\":true") != NULL);
    CHECK(strstr(body, "\"tool_choice\":\"auto\"") != NULL);
    plat_free(body);
    // no tools -> no tools key
    body = openai_build_request("m", NULL, &h, NULL, 0, false, 1);
    CHECK(strstr(body, "\"tools\"") == NULL);
    CHECK(strstr(body, "\"max_tokens\":1") != NULL);
    CHECK(strstr(body, "\"stream\"") == NULL);
    plat_free(body);
    history_clear(&h);

    // ---- streaming text + fragmented tool call arguments
    llm_stream_t s;
    llm_stream_cb_t cb = {.on_text = on_text, .on_tool_call_begin = on_begin, .on_error = on_error};
    llm_stream_init(&s, &cb, NULL);
    const char* stream =
        "data: {\"id\":\"x\",\"model\":\"gpt-4.1-mini\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"Hello\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\" world\"},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_abc\",\"type\":\"function\",\"function\":{\"name\":\"render_markdown\",\"arguments\":\"\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"mark\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"down\\\":\\\"# T\\\"}\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":1,\"id\":\"call_def\",\"type\":\"function\",\"function\":{\"name\":\"render_choice\",\"arguments\":\"{}\"}}]},\"finish_reason\":null}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}\n\n"
        "data: [DONE]\n\n";
    size_t L = strlen(stream);
    for (size_t i = 0; i < L; i += 7) openai_stream_feed(&s, stream + i, (i + 7 <= L) ? 7 : L - i);
    openai_stream_finish(&s);
    CHECK_STR(g_text, "Hello world");
    CHECK_STR(llm_stream_text(&s), "Hello world");
    CHECK(s.done);
    CHECK_STR(s.finish_reason, "tool_calls");
    CHECK_STR(s.model, "gpt-4.1-mini");
    CHECK_INT(s.usage_in, 10);
    CHECK_INT(s.usage_out, 5);
    CHECK_INT(g_begins, 2);
    tool_call_t* out = NULL;
    int n = llm_stream_take_tool_calls(&s, &out);
    CHECK_INT(n, 2);
    CHECK_STR(out[0].id, "call_abc");
    CHECK_STR(out[0].name, "render_markdown");
    CHECK_STR(out[0].arguments, "{\"markdown\":\"# T\"}");
    CHECK_STR(out[1].id, "call_def");
    CHECK_STR(out[1].arguments, "{}");
    tool_calls_free(out, n);
    llm_stream_free(&s);

    // ---- error object + Ollama-style tool call without id and object arguments
    g_text[0] = 0; g_err[0] = 0;
    llm_stream_init(&s, &cb, NULL);
    const char* s2 = "data: {\"error\":{\"message\":\"Incorrect API key\",\"type\":\"invalid_request_error\"}}\n\n";
    openai_stream_feed(&s, s2, strlen(s2)); openai_stream_finish(&s);
    CHECK(s.got_error);
    CHECK_STR(g_err, "Incorrect API key");
    llm_stream_free(&s);
    llm_stream_init(&s, &cb, NULL);
    const char* s3 = "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"name\":\"get_weather\",\"arguments\":{\"city\":\"Oslo\"}}}]},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n";
    openai_stream_feed(&s, s3, strlen(s3)); openai_stream_finish(&s);
    n = llm_stream_take_tool_calls(&s, &out);
    CHECK_INT(n, 1);
    CHECK_STR(out[0].id, "call_0");
    CHECK_STR(out[0].arguments, "{\"city\":\"Oslo\"}");
    tool_calls_free(out, n);
    llm_stream_free(&s);

    // ---- non-streaming parse
    jsonbuf_t t; jb_init(&t); char err[128] = "";
    CHECK(openai_parse_response("{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"}}]}", &t, err, sizeof(err)));
    CHECK_STR(jb_cstr(&t), "ok");
    jb_clear(&t);
    CHECK(!openai_parse_response("{\"error\":{\"message\":\"nope\"}}", &t, err, sizeof(err)));
    CHECK_STR(err, "nope");
    jb_free(&t);
    TEST_MAIN_END("test_openai");
}

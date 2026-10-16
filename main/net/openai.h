// OpenAI chat/completions wire format: request body builder + streaming SSE delta parser.
// Host-testable (host_tests/test_openai.c). Works against OpenAI, Ollama /v1, vLLM, LiteLLM.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "agent/history.h"
#include "util/jsonbuf.h"
#include "util/sse.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LLM_MAX_TOOL_CALLS 16

typedef struct {
    void (*on_text)(const char* delta, void* user);
    void (*on_tool_call_begin)(const char* id, const char* name, void* user);  // may be NULL
    void (*on_error)(const char* message, void* user);                          // server error object
} llm_stream_cb_t;

typedef struct {
    char*    id;
    char*    name;
    jsonbuf_t arguments;
    int      index;  // provider index
} llm_tool_acc_t;

typedef struct {
    sse_parser_t    sse;
    llm_stream_cb_t cb;
    void*           user;
    jsonbuf_t       text;
    llm_tool_acc_t  calls[LLM_MAX_TOOL_CALLS];
    int             n_calls;
    char            finish_reason[32];
    bool            done;        // saw [DONE] / message_stop
    bool            got_error;
    char            error[256];
    char            model[64];
    int             usage_in, usage_out;
} llm_stream_t;

// ---- request --------------------------------------------------------------------------
// Returns a malloc'd JSON body (free with plat_free). system_prompt may be NULL/empty.
char* openai_build_request(const char* model, const char* system_prompt, const history_t* h,
                           const tool_def_t* tools, int n_tools, bool stream, int max_tokens);

// ---- streaming response ---------------------------------------------------------------
void llm_stream_init(llm_stream_t* s, const llm_stream_cb_t* cb, void* user);
void llm_stream_free(llm_stream_t* s);
// Move accumulated tool calls out (caller owns; array allocated with plat_malloc). Returns count.
int  llm_stream_take_tool_calls(llm_stream_t* s, tool_call_t** out);
static inline const char* llm_stream_text(const llm_stream_t* s) { return jb_cstr(&s->text); }

void openai_stream_feed(llm_stream_t* s, const char* buf, size_t n);
void openai_stream_finish(llm_stream_t* s);
// Parse one SSE data payload (exposed for tests).
void openai_handle_event(llm_stream_t* s, const char* data);

// Non-streaming response body (used by the provisioning "Test" button). Returns true on success,
// fills text (first choice content) or err.
bool openai_parse_response(const char* body, jsonbuf_t* text, char* err, size_t err_len);

#ifdef __cplusplus
}
#endif

// Anthropic Messages API wire format: request body builder + streaming SSE event parser.
// Host-testable (host_tests/test_anthropic.c). Reuses llm_stream_t from openai.h.
#pragma once
#include "openai.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ANTHROPIC_VERSION "2023-06-01"

char* anthropic_build_request(const char* model, const char* system_prompt, const history_t* h,
                              const tool_def_t* tools, int n_tools, bool stream, int max_tokens);

void anthropic_stream_feed(llm_stream_t* s, const char* buf, size_t n);
void anthropic_stream_finish(llm_stream_t* s);
// Handle one SSE event (exposed for tests).
void anthropic_handle_event(llm_stream_t* s, const char* event, const char* data);
// Non-streaming response body.
bool anthropic_parse_response(const char* body, jsonbuf_t* text, char* err, size_t err_len);

#ifdef __cplusplus
}
#endif

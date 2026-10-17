// The agent loop (§8): a dedicated network task that streams model output to the TUI,
// executes tool calls (render tools locally, MCP tools remotely) and continues the turn.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void agent_init(void);                 // creates the network task (PSRAM stack)
void agent_submit(const char* text);   // UI task: user pressed Enter
void agent_abort(void);                // UI task: Esc during a request
void agent_new_session(void);          // UI task: Meta+N
bool agent_busy(void);

// Session statistics for the status bar. Token counts come from the provider's usage report;
// when a server does not report usage, ctx_tokens is estimated from the request size (bytes/4).
typedef struct {
    uint32_t in_tokens, out_tokens;  // totals for this session (reset by agent_new_session)
    uint32_t requests;
    uint32_t ctx_tokens;             // prompt+completion tokens of the last request = context in use
    bool     ctx_estimated;
} agent_stats_t;
void agent_stats_get(agent_stats_t* out);
void agent_set_stats_cb(void (*cb)(void* arg), void* arg);  // posted to the UI task after each request
// Build a test request on the calling task (used by provisioning). Returns malloc'd JSON
// {"ok":true,"reply":"..."} or {"ok":false,"error":"..."}; caller frees with free().
char* agent_test_inference(void);

#ifdef __cplusplus
}
#endif

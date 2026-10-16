// The agent loop (§8): a dedicated network task that streams model output to the TUI,
// executes tool calls (render tools locally, MCP tools remotely) and continues the turn.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void agent_init(void);                 // creates the network task (PSRAM stack)
void agent_submit(const char* text);   // UI task: user pressed Enter
void agent_abort(void);                // UI task: Esc during a request
void agent_new_session(void);          // UI task: Meta+N
bool agent_busy(void);
// Build a test request on the calling task (used by provisioning). Returns malloc'd JSON
// {"ok":true,"reply":"..."} or {"ok":false,"error":"..."}; caller frees with free().
char* agent_test_inference(void);

#ifdef __cplusplus
}
#endif

// chronicle-agents client over the hosted chronicle: GitHub identity through the bridge, one pull
// consumer over the rooms store's stream folded into rooms/messages/agents, verified writes through
// the node, live signals. A worker task runs the blocking sequences; a callback reports events.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "chron_proto.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHRON_ST_IDLE = 0,      // not connected, nothing in progress
    CHRON_ST_CONNECTING,
    CHRON_ST_SYNCING,       // connected, replaying the store
    CHRON_ST_SYNCED,        // live
    CHRON_ST_RECONNECTING,  // the connection dropped; the client retries by itself
    CHRON_ST_FAILED,        // connect failed; see chronicle_last_error()
} chron_state_t;

typedef enum {
    CHRON_EV_STATE,        // state changed (text = description)
    CHRON_EV_ROOMS,        // the room list / a room's state changed (room_id may be set)
    CHRON_EV_MESSAGE,      // a message landed in room_id (only once synced)
    CHRON_EV_LIVE,         // live signal: room_id (or "" for presence), text = "wilson is thinking…"
    CHRON_EV_REFUSED,      // an agent refused/failed in room_id: text = reason
    CHRON_EV_ROOM_OPENED,  // a room this device asked for exists now: room_id
    CHRON_EV_ERROR,        // text
    CHRON_EV_INFO,         // text (sign-in progress and the like)
} chron_event_t;
// Called on the network tasks, never on the UI task; the receiver marshals with ui_post().
typedef void (*chron_event_cb_t)(chron_event_t ev, const char* room_id, const char* text, void* user);

void          chronicle_init(void);  // worker task + NATS glue
void          chronicle_set_event_cb(chron_event_cb_t cb, void* user);
chron_state_t chronicle_state(void);
const char*   chronicle_state_text(void);
const char*   chronicle_last_error(void);
const char*   chronicle_principal(void);  // the author name once connected

// Async commands (run on the worker; results arrive as events)
void chronicle_connect_async(void);
void chronicle_disconnect_async(void);
void chronicle_send_async(const char* room_id, const char* text);
void chronicle_create_room_async(const char* title, const char* purpose);
void chronicle_open_direct_async(const char* agent);   // creates the direct conversation if absent
void chronicle_close_room_async(const char* room_id);
void chronicle_set_open_room(const char* room_id);     // live signals follow the open room (NULL = none)

// The workspace model: read under the lock, copy out, unlock before drawing.
void       chronicle_lock(void);
void       chronicle_unlock(void);
chron_ws_t* chronicle_ws(void);
int        chronicle_agents_online(void);  // takes the lock itself
const char* chronicle_live_text(void);     // last live line for the open room ("" when idle)

// Blocking helpers for the setup flow (never on the UI task). They use the one NATS connection,
// so a live session is dropped first; the caller reconnects afterwards.
esp_err_t chronicle_fetch_profile(char* err, size_t err_len);  // site -> nats_url, client id, sentinel (saved)
esp_err_t chronicle_identity(const char* token, chron_user_info_t* who, chron_membership_t* out, int max, int* n, char* err, size_t err_len);
char*     chronicle_test_account_json(void);  // {"ok":true,"principal":..,"account":..,"ops":N} or {"ok":false,"error":..}; free()
esp_err_t chronicle_fresh_token(char* err, size_t err_len);  // refreshes the GitHub token when it is about to lapse (saved)
void      chronicle_emit_info(const char* text);  // sign-in progress lines for the TUI

#ifdef __cplusplus
}
#endif

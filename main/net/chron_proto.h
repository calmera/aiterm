// chronicle / chronicle-agents wire shapes, as plain C with no I/O so the host tests cover them:
// paths and subjects, op headers, message JSON, @mentions, the GitHub device-flow and identity
// plane JSON, and the fold of a store's operations into rooms, messages and agents.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CHRON_NAME_MAX   48
#define CHRON_ID_MAX     80
#define CHRON_MAX_ROOMS  32
#define CHRON_MAX_AGENTS 16
#define CHRON_ROOM_MSG_CAP 400   // messages kept per room (oldest dropped)
#define CHRON_MAX_MENTIONS 8

// ---- subjects, paths, ids --------------------------------------------------------------
// room/x/messages/y -> room.x.messages.y
bool chron_path_to_tail(const char* path, char* out, size_t n);
// CHRON.<store>.OPS.<tail> -> room/x/messages/y ; false when not an ops subject of the store
bool chron_subject_to_path(const char* store, const char* subject, char* out, size_t n);
void chron_ops_subject(const char* store, const char* path, char* out, size_t n);
void chron_apply_subject(const char* principal, const char* store, const char* path, char* out, size_t n);
void chron_room_id_from_title(const char* title, char* out, size_t n, uint32_t (*rnd)(void));
void chron_direct_id(const char* a, const char* b, char* out, size_t n);  // direct--<a>--<b>, sorted
void chron_mint_id(const char* prefix, char* out, size_t n, uint32_t (*rnd)(void));  // prefix + 12 hex
void chron_nuid(char* out, size_t n, uint32_t (*rnd)(void));  // 22 chars [A-Za-z0-9]
// "tls://host:4222" / "nats://host" -> host, port, tls
bool chron_parse_nats_url(const char* url, char* host, size_t hn, int* port, bool* tls);
// stream sequence from a $JS.ACK reply subject (9- and 12-token forms); 0 when absent
uint64_t chron_ack_stream_seq(const char* reply);
int64_t  chron_ack_pending(const char* reply);  // messages pending after this one; -1 when absent

// ---- headers ---------------------------------------------------------------------------
// "NATS/1.0\r\nNats-Msg-Id: ..\r\nOp-Type: ..\r\nOp-Author: ..\r\n[Op-Ts: ..\r\n]Op-Version: 1\r\n
//  [Nats-Expected-Last-Subject-Sequence: 0\r\n]\r\n" (plat_malloc'd)
char* chron_op_headers(const char* op_id, const char* op_type, const char* author, const char* ts_rfc3339, bool expect_absent);
bool  chron_header_get(const char* headers, int len, const char* key, char* out, size_t n);
int   chron_header_status(const char* headers, int len);  // 0 when no status (plain "NATS/1.0")
// decorated creds -> jwt + seed
bool  chron_parse_creds(const char* creds, char* jwt, size_t jn, char* seed, size_t sn);

// ---- payloads --------------------------------------------------------------------------
int   chron_mentions(const char* text, char out[][CHRON_NAME_MAX], int max);  // unique @names in order
char* chron_message_json(const char* text, const char* kind, char mentions[][CHRON_NAME_MAX], int n, const char* reply_to);
char* chron_room_json(const char* title, const char* purpose, const char* kind, const char* p1, const char* p2);

// ---- responses -------------------------------------------------------------------------
typedef struct { char user[CHRON_NAME_MAX]; char account[CHRON_NAME_MAX]; int64_t github_id; char login[CHRON_NAME_MAX]; } chron_user_info_t;
bool chron_parse_user_info(const char* json, chron_user_info_t* out);  // $SYS.REQ.USER.INFO
typedef struct { char account[CHRON_NAME_MAX]; char principal[CHRON_NAME_MAX]; char role[16]; } chron_membership_t;
int  chron_parse_memberships(const char* json, chron_membership_t* out, int max);  // -1 on error
bool chron_parse_consumer_create(const char* json, char* name, size_t n, int64_t* num_pending, char* err, size_t en);
bool chron_parse_apply_response(const char* json, uint64_t* seq);
typedef struct { char agent[CHRON_NAME_MAX]; char state[16]; char trigger[160]; char reason[160]; char* text; } chron_live_t;  // text plat_malloc'd or NULL
bool chron_parse_live(const char* json, chron_live_t* out);
typedef struct { char url[160]; char github_client_id[64]; char github_token_url[160]; char* sentinel; } chron_profile_t;  // sentinel plat_malloc'd
bool chron_parse_profile(const char* json, chron_profile_t* out);
// GitHub device flow
typedef struct { char device_code[128]; char user_code[16]; char verification_uri[128]; int expires_in; int interval; } gh_device_t;
bool gh_parse_device_code(const char* json, gh_device_t* out);
// 1 = tokens, 0 = still pending (authorization_pending / slow_down), -1 = error (err filled)
int  gh_parse_token(const char* json, char* access, size_t an, char* refresh, size_t rn, int* expires_in, char* err, size_t en);

// ---- the workspace fold ----------------------------------------------------------------
typedef struct {
    uint64_t seq;
    char     id[CHRON_ID_MAX];       // message id (last path token)
    char     author[CHRON_NAME_MAX];
    char     kind[12];               // message / turn / system / question
    char     ts[32];                 // Op-Ts as sent
    char*    text;                   // plat_malloc'd
    char     reply_to[CHRON_ID_MAX];
} chron_msg_t;

typedef struct {
    char     id[CHRON_ID_MAX];
    char     title[96];
    char     purpose[128];
    bool     closed;
    bool     direct;
    char     participants[2][CHRON_NAME_MAX];
    uint64_t created_seq, last_seq;
    char     last_author[CHRON_NAME_MAX];
    chron_msg_t* msgs;               // plat_malloc'd ring of up to CHRON_ROOM_MSG_CAP, oldest first
    int      nmsgs;
    int      total;                  // messages seen (incl. dropped)
} chron_room_t;

typedef struct {
    char    name[CHRON_NAME_MAX];
    char    description[128];
    char    harness[24];
    int64_t last_seen_ms;            // presence heartbeat (plat_now_ms), 0 = never
    char    live_state[16];          // last live signal state in the open room
    int64_t live_at_ms;
} chron_agent_t;

typedef struct {
    chron_room_t  rooms[CHRON_MAX_ROOMS];
    int           nrooms;
    chron_agent_t agents[CHRON_MAX_AGENTS];
    int           nagents;
    uint64_t      head_seq;          // highest sequence folded
} chron_ws_t;

void          chron_ws_init(chron_ws_t* ws);
void          chron_ws_clear(chron_ws_t* ws);
chron_room_t* chron_ws_room(chron_ws_t* ws, const char* id);
chron_agent_t* chron_ws_agent(chron_ws_t* ws, const char* name);
// Folds one operation (subject, headers, payload) read from the store's stream. Returns the room it
// touched (or NULL); *new_message is set when a message was appended to that room.
chron_room_t* chron_fold_op(chron_ws_t* ws, const char* store, const char* subject, uint64_t seq,
                            const char* headers, int hlen, const char* payload, int plen, bool* new_message);

#ifdef __cplusplus
}
#endif

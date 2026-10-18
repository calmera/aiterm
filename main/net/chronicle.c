#include "chronicle.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "github_auth.h"
#include "http_stream.h"
#include "nats_glue.h"
#include "util/jsonbuf.h"
#include "util/platform.h"

static const char* TAG = "chron";

#define BATCH          32
#define FETCH_EXPIRES_NS   30000000000LL
#define FETCH_HEARTBEAT_NS 10000000000LL
#define FETCH_STALL_MS     45000
#define PRESENCE_TTL_MS    30000

typedef enum { CMD_CONNECT, CMD_DISCONNECT, CMD_RESYNC, CMD_SEND, CMD_CREATE_ROOM, CMD_DIRECT, CMD_CLOSE_ROOM } cmd_kind_t;
typedef struct { cmd_kind_t kind; char a[CHRON_ID_MAX]; char* b; char c[128]; } cmd_t;

static QueueHandle_t      s_q = NULL;
static SemaphoreHandle_t  s_lock = NULL;      // the model
static SemaphoreHandle_t  s_conn_lock = NULL; // one connection sequence at a time
static chron_event_cb_t   s_ev = NULL;
static void*              s_ev_user = NULL;
static chron_ws_t*        s_ws = NULL;
static volatile chron_state_t s_state = CHRON_ST_IDLE;
static char               s_err[160] = "";
static char               s_principal[CHRON_NAME_MAX] = "";
static char               s_store[CFG_STR] = "rooms";
static char               s_stream[64] = "";
static char               s_consumer[80] = "";
static char               s_inbox[64] = "";
static int                s_inbox_sid = -1, s_live_agents_sid = -1, s_live_room_sid = -1;
static char               s_open_room[CHRON_ID_MAX] = "";
static char               s_live_text[160] = "";
static volatile int       s_batch_left = 0;
static volatile bool      s_fetch_outstanding = false;
static volatile int64_t   s_last_rx_ms = 0;
static volatile bool      s_live_session = false;  // a rooms session is wanted (vs. a setup-flow connection)
static volatile bool      s_want_session = false;  // rooms mode asked for a connection (retry while it fails)
static int64_t            s_retry_at_ms = 0;
static uint32_t           s_batches = 0, s_delivered = 0, s_statuses = 0;
static int                s_last_status = 0;
static int64_t            s_last_pending = -1;
static bool               s_ack_each = false;  // only when the server insisted on an ack policy
// stable strings for the glue
static char s_jwt[1400], s_seed[80], s_token[256], s_host[128];

static void emit(chron_event_t ev, const char* room, const char* text) { if (s_ev) s_ev(ev, room ? room : "", text ? text : "", s_ev_user); }
static void set_state(chron_state_t st, const char* why) {
    s_state = st;
    if (why) snprintf(s_err, sizeof(s_err), "%s", why);
    emit(CHRON_EV_STATE, NULL, chronicle_state_text());
}
void chronicle_lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void chronicle_unlock(void) { xSemaphoreGiveRecursive(s_lock); }
chron_ws_t* chronicle_ws(void) { return s_ws; }
chron_state_t chronicle_state(void) { return s_state; }
const char* chronicle_last_error(void) { return s_err; }
const char* chronicle_principal(void) { return s_principal; }
const char* chronicle_live_text(void) { return s_live_text; }
void chronicle_set_event_cb(chron_event_cb_t cb, void* user) { s_ev = cb; s_ev_user = user; }
void chronicle_emit_info(const char* text) { emit(CHRON_EV_INFO, NULL, text); }
void chronicle_emit(chron_event_t ev, const char* room_id, const char* text) { emit(ev, room_id, text); }
const char* chronicle_state_text(void) {
    switch (s_state) {
        case CHRON_ST_IDLE: return "offline";
        case CHRON_ST_CONNECTING: return "connecting";
        case CHRON_ST_SYNCING: return "reading the record";
        case CHRON_ST_SYNCED: return "connected";
        case CHRON_ST_RECONNECTING: return "reconnecting";
        case CHRON_ST_FAILED: return s_err[0] ? s_err : "failed";
    }
    return "?";
}
static uint32_t rnd(void) { return esp_random(); }
static void now_rfc3339(char* out, size_t n) {
    time_t t = time(NULL);
    if (t < 1700000000) { out[0] = 0; return; }  // clock not set: Op-Ts is optional
    struct tm tm; gmtime_r(&t, &tm);
    strftime(out, n, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

// ---------------------------------------------------------------------------------------
// connection
// ---------------------------------------------------------------------------------------
static esp_err_t glue_connect_with(const char* selector, const char* github_token, char* err, size_t en) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    int port; bool tls;
    if (!chron_parse_nats_url(ch->nats_url, s_host, sizeof(s_host), &port, &tls)) { snprintf(err, en, "bad NATS URL in the profile"); return ESP_ERR_INVALID_ARG; }
    if (!chron_parse_creds(ch->sentinel, s_jwt, sizeof(s_jwt), s_seed, sizeof(s_seed))) { snprintf(err, en, "bad sentinel in the profile"); return ESP_ERR_INVALID_ARG; }
    snprintf(s_token, sizeof(s_token), "%s:%s", selector, github_token);
    nats_glue_cfg_t cfg = {.host = s_host, .port = port, .tls = tls, .jwt = s_jwt, .nkey_seed = s_seed, .token = s_token, .name = "ai-terminal"};
    return nats_glue_connect(&cfg, 20000, err, en);
}
static esp_err_t whoami(chron_user_info_t* who, char* err, size_t en) {
    nats_glue_reply_t r;
    esp_err_t e = nats_glue_request("$SYS.REQ.USER.INFO", NULL, "", 5000, &r);
    if (e != ESP_OK) { snprintf(err, en, "whoami: %s", e == ESP_ERR_TIMEOUT ? "timeout" : e == ESP_ERR_INVALID_STATE ? "the connection dropped" : esp_err_to_name(e)); return e; }
    bool ok = chron_parse_user_info(r.data ? r.data : "", who);
    nats_glue_reply_free(&r);
    if (!ok) { snprintf(err, en, "whoami: the server reported no principal"); return ESP_FAIL; }
    return ESP_OK;
}
// A verified write through the node: returns ESP_OK (seq), ESP_ERR_INVALID_STATE with err = the
// service error code when refused, other errors otherwise.
static esp_err_t apply(const char* path, const char* op_type, const char* payload, bool expect_absent, uint64_t* seq, char* code, size_t cn, char* err, size_t en) {
    char subject[256]; chron_apply_subject(s_principal, s_store, path, subject, sizeof(subject));
    char opid[32]; chron_nuid(opid, sizeof(opid), rnd);
    char ts[32]; now_rfc3339(ts, sizeof(ts));
    char* hdr = chron_op_headers(opid, op_type, s_principal, ts, expect_absent);
    nats_glue_reply_t r;
    esp_err_t e = nats_glue_request(subject, hdr, payload, 15000, &r);
    plat_free(hdr);
    if (code) code[0] = 0;
    if (e == ESP_ERR_NOT_FOUND) { snprintf(err, en, "no chronicle node answers for this account"); nats_glue_reply_free(&r); return ESP_FAIL; }
    if (e != ESP_OK) { snprintf(err, en, "write: %s", e == ESP_ERR_TIMEOUT ? "timeout" : esp_err_to_name(e)); return e; }
    char c[48] = "";
    if (chron_header_get(r.headers, r.header_len, "Nats-Service-Error-Code", c, sizeof(c)) && c[0]) {
        char d[120] = ""; chron_header_get(r.headers, r.header_len, "Nats-Service-Error", d, sizeof(d));
        if (code) snprintf(code, cn, "%s", c);
        snprintf(err, en, "%s%s%s", c, d[0] ? ": " : "", d);
        nats_glue_reply_free(&r);
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t s = 0;
    bool ok = chron_parse_apply_response(r.data ? r.data : "", &s);
    nats_glue_reply_free(&r);
    if (!ok) { snprintf(err, en, "write: unexpected reply"); return ESP_FAIL; }
    if (seq) *seq = s;
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// the pull loop (callbacks run on the NATS task)
// ---------------------------------------------------------------------------------------
static void request_batch(void) {
    if (!s_consumer[0] || s_inbox_sid < 0) return;
    char subject[160]; snprintf(subject, sizeof(subject), "$JS.API.CONSUMER.MSG.NEXT.%s.%s", s_stream, s_consumer);
    char body[128]; snprintf(body, sizeof(body), "{\"batch\":%d,\"expires\":%lld,\"idle_heartbeat\":%lld}", BATCH, (long long)FETCH_EXPIRES_NS, (long long)FETCH_HEARTBEAT_NS);
    s_batch_left = BATCH;
    s_fetch_outstanding = true;
    s_last_rx_ms = plat_now_ms();
    s_batches++;
    esp_err_t e = nats_glue_publish(subject, NULL, body, s_inbox);
    if (s_batches <= 3 || (s_batches % 50) == 0 || e != ESP_OK) ESP_LOGI(TAG, "pull #%lu -> %s (%s)", (unsigned long)s_batches, subject, esp_err_to_name(e));
}
static void post_cmd(cmd_kind_t kind, const char* a, const char* b, const char* c);
static void inbox_cb(const char* subject, const char* reply, const char* headers, int hlen, const char* data, int len, void* user) {
    (void)user;
    s_last_rx_ms = plat_now_ms();
    int st = chron_header_status(headers, hlen);
    if (st == 100) { if (s_statuses++ < 3) ESP_LOGI(TAG, "pull heartbeat"); return; }  // idle heartbeat
    if (st >= 400 || (st > 0 && len == 0 && (!reply || !reply[0]))) {
        s_fetch_outstanding = false;
        s_last_status = st; s_statuses++;
        if (st == 409 && headers && strstr(headers, "Consumer Deleted")) { ESP_LOGW(TAG, "consumer gone — resync"); post_cmd(CMD_RESYNC, NULL, NULL, NULL); return; }
        if (st != 408) ESP_LOGW(TAG, "pull status %d: %.*s", st, hlen > 60 ? 60 : hlen, headers ? headers : "");
        else if (s_statuses <= 3) ESP_LOGI(TAG, "pull window expired with nothing new");
        // a pull window that closed with nothing new means the replay is complete
        if (st == 408 && s_state == CHRON_ST_SYNCING) set_state(CHRON_ST_SYNCED, NULL);
        request_batch();
        return;
    }
    uint64_t seq = chron_ack_stream_seq(reply);
    int64_t pending = chron_ack_pending(reply);
    s_delivered++; s_last_pending = pending;
    if (s_delivered <= 3 || (s_delivered % 100) == 0) ESP_LOGI(TAG, "op #%lu seq=%llu pending=%lld %s", (unsigned long)s_delivered, (unsigned long long)seq, (long long)pending, subject);
    bool new_msg = false; char room_id[CHRON_ID_MAX] = "";
    chronicle_lock();
    chron_room_t* room = NULL;
    if (seq == 0 || seq > s_ws->head_seq) room = chron_fold_op(s_ws, s_store, subject, seq, headers, hlen, data, len, &new_msg);  // a redelivery is not news
    if (room) snprintf(room_id, sizeof(room_id), "%s", room->id);
    chronicle_unlock();
    if (s_ack_each && reply && reply[0]) nats_glue_publish(reply, NULL, "+ACK", NULL);
    if (--s_batch_left <= 0) request_batch();
    if (room_id[0]) emit(new_msg ? CHRON_EV_MESSAGE : CHRON_EV_ROOMS, room_id, NULL);  // the UI coalesces these during a replay
    if (pending == 0 && s_state == CHRON_ST_SYNCING) set_state(CHRON_ST_SYNCED, NULL);
}
static void live_cb(const char* subject, const char* reply, const char* headers, int hlen, const char* data, int len, void* user) {
    (void)reply; (void)headers; (void)hlen; (void)user;
    chron_live_t sig;
    char json[1024]; int n = len < (int)sizeof(json) - 1 ? len : (int)sizeof(json) - 1;
    memcpy(json, data, (size_t)n); json[n] = 0;
    if (!chron_parse_live(json, &sig)) return;
    const char* room_tok = strstr(subject, ".room.");
    char room_id[CHRON_ID_MAX] = "";
    if (room_tok) snprintf(room_id, sizeof(room_id), "%s", room_tok + 6);
    chronicle_lock();
    chron_agent_t* a = chron_ws_agent(s_ws, sig.agent);
    if (!a && s_ws->nagents < CHRON_MAX_AGENTS) { a = &s_ws->agents[s_ws->nagents++]; memset(a, 0, sizeof(*a)); snprintf(a->name, sizeof(a->name), "%s", sig.agent); }
    if (a) {
        if (strcmp(sig.state, "online") == 0) a->last_seen_ms = plat_now_ms();
        else { snprintf(a->live_state, sizeof(a->live_state), "%s", sig.state); a->live_at_ms = plat_now_ms(); a->last_seen_ms = plat_now_ms(); }
    }
    chronicle_unlock();
    if (!room_tok) { plat_free(sig.text); return; }  // presence: the status line polls agents_online()
    char text[200];
    if (strcmp(sig.state, "refused") == 0 || strcmp(sig.state, "failed") == 0) {
        snprintf(text, sizeof(text), "%s %s: %s", sig.agent, sig.state, sig.reason[0] ? sig.reason : "no reason given");
        s_live_text[0] = 0;
        emit(CHRON_EV_REFUSED, room_id, text);
    } else {
        if (strcmp(sig.state, "done") == 0) text[0] = 0;
        else if (strcmp(sig.state, "picked-up") == 0) snprintf(text, sizeof(text), "%s picked it up", sig.agent);
        else if (strcmp(sig.state, "waiting") == 0) snprintf(text, sizeof(text), "%s is waiting for an answer", sig.agent);
        else if (strcmp(sig.state, "step") == 0) snprintf(text, sizeof(text), "%s is running a tool", sig.agent);
        else snprintf(text, sizeof(text), "%s is %s…", sig.agent, sig.state);
        snprintf(s_live_text, sizeof(s_live_text), "%s", text);
        emit(CHRON_EV_LIVE, room_id, text);
    }
    plat_free(sig.text);
}
static void live_room_follow(void) {
    if (s_live_room_sid >= 0) { nats_glue_unsubscribe(s_live_room_sid); s_live_room_sid = -1; }
    s_live_text[0] = 0;
    if (!s_open_room[0] || !nats_glue_connected()) return;
    char subj[160]; snprintf(subj, sizeof(subj), "CHRON.LIVE.%s.room.%s", s_store, s_open_room);
    s_live_room_sid = nats_glue_subscribe(subj, live_cb, NULL);
}
static void teardown_session(void) {
    if (s_inbox_sid >= 0) { nats_glue_unsubscribe(s_inbox_sid); s_inbox_sid = -1; }
    if (s_live_agents_sid >= 0) { nats_glue_unsubscribe(s_live_agents_sid); s_live_agents_sid = -1; }
    if (s_live_room_sid >= 0) { nats_glue_unsubscribe(s_live_room_sid); s_live_room_sid = -1; }
    s_consumer[0] = 0; s_fetch_outstanding = false; s_live_text[0] = 0;
}
// Creates the consumer (from head_seq+1 when resuming) and starts pulling. Worker task.
static esp_err_t start_pull(bool resume, char* err, size_t en) {
    uint64_t from = 0;
    chronicle_lock(); from = s_ws->head_seq; chronicle_unlock();
    char subject[128]; snprintf(subject, sizeof(subject), "$JS.API.CONSUMER.CREATE.%s", s_stream);
    // No acknowledgements: the account's member baseline does not allow publishing to $JS.ACK.>
    // (chronicle's own readers are ordered consumers that never ack), and the server answers an
    // ack with a permissions violation. Should a server insist on an ack policy, fall back to
    // explicit acks once — the violation then only costs a reconnect, and the seq guard below
    // keeps redeliveries out of the model.
    int64_t pending = -1; char cerr[128] = "";
    bool ok = false;
    for (int variant = 0; variant < 2 && !ok; variant++) {
        s_ack_each = variant == 1;
        jsonbuf_t b; jb_init(&b);
        jb_appendf(&b, "{\"stream_name\":\"%s\",\"config\":{\"filter_subject\":\"CHRON.%s.OPS.>\",", s_stream, s_store);
        if (resume && from > 0) jb_appendf(&b, "\"deliver_policy\":\"by_start_sequence\",\"opt_start_seq\":%llu,", (unsigned long long)(from + 1));
        else jb_append_str(&b, "\"deliver_policy\":\"all\",");
        if (s_ack_each) jb_append_str(&b, "\"ack_policy\":\"explicit\",\"ack_wait\":3600000000000,\"max_ack_pending\":10000,");
        else jb_append_str(&b, "\"ack_policy\":\"none\",");
        jb_append_str(&b, "\"max_deliver\":-1,\"replay_policy\":\"instant\",\"inactive_threshold\":600000000000}}");
        nats_glue_reply_t r;
        esp_err_t e = nats_glue_request(subject, NULL, jb_cstr(&b), 10000, &r);
        jb_free(&b);
        if (e != ESP_OK) { snprintf(err, en, "consumer: %s", e == ESP_ERR_TIMEOUT ? "timeout" : e == ESP_ERR_NOT_FOUND ? "JetStream not answering" : esp_err_to_name(e)); nats_glue_reply_free(&r); return ESP_FAIL; }
        ok = chron_parse_consumer_create(r.data ? r.data : "", s_consumer, sizeof(s_consumer), &pending, cerr, sizeof(cerr));
        nats_glue_reply_free(&r);
        if (!ok && variant == 0 && strstr(cerr, "ack")) { ESP_LOGW(TAG, "server wants an ack policy (%s) — acknowledging each op", cerr); continue; }
        break;
    }
    if (!ok) { snprintf(err, en, "consumer: %s", cerr); return ESP_FAIL; }
    s_batches = 0; s_delivered = 0; s_statuses = 0; s_last_status = 0; s_last_pending = pending;
    nats_glue_new_inbox(s_inbox, sizeof(s_inbox));
    s_inbox_sid = nats_glue_subscribe(s_inbox, inbox_cb, NULL);
    if (s_inbox_sid < 0) { snprintf(err, en, "inbox subscribe failed"); return ESP_FAIL; }
    char live[128]; snprintf(live, sizeof(live), "CHRON.LIVE.%s.agent.*", s_store);
    s_live_agents_sid = nats_glue_subscribe(live, live_cb, NULL);
    live_room_follow();
    ESP_LOGI(TAG, "consumer %s on %s, %lld pending", s_consumer, s_stream, (long long)pending);
    if (pending == 0) set_state(CHRON_ST_SYNCED, NULL); else set_state(CHRON_ST_SYNCING, NULL);
    request_batch();
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------
// worker
// ---------------------------------------------------------------------------------------
static void glue_state_cb(bool connected, void* user) {
    (void)user;
    if (!s_live_session) return;
    if (connected) post_cmd(CMD_RESYNC, NULL, NULL, NULL);
    else if (nats_glue_auth_error()) { s_fetch_outstanding = false; set_state(CHRON_ST_RECONNECTING, "token refused — signing in again"); post_cmd(CMD_CONNECT, NULL, NULL, NULL); }
    else { s_fetch_outstanding = false; set_state(CHRON_ST_RECONNECTING, NULL); }
}
static void do_connect(void) {
    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    cfg_chronicle_t* ch = &app_config()->chronicle;
    char err[160] = "";
    if (!app_config_has_chronicle()) { set_state(CHRON_ST_FAILED, "not signed in — use the setup page"); xSemaphoreGive(s_conn_lock); return; }
    s_live_session = false;
    teardown_session();
    set_state(CHRON_ST_CONNECTING, NULL);
    snprintf(s_store, sizeof(s_store), "%s", ch->store[0] ? ch->store : "rooms");
    size_t o = (size_t)snprintf(s_stream, sizeof(s_stream), "LOG_");
    for (const char* p = s_store; *p && o + 1 < sizeof(s_stream); p++) s_stream[o++] = *p == '-' ? '_' : (char)toupper((unsigned char)*p);
    s_stream[o] = 0;
    chronicle_lock(); chron_ws_clear(s_ws); chronicle_unlock();
    if (chronicle_fresh_token(err, sizeof(err)) != ESP_OK) ESP_LOGW(TAG, "token refresh: %s", err);
    esp_err_t e = ESP_FAIL;
    for (int attempt = 0; attempt < 4; attempt++) {
        e = glue_connect_with(ch->account, ch->access_token, err, sizeof(err));
        if (e == ESP_OK) break;
        if (e != ESP_ERR_INVALID_STATE && attempt < 3) {  // DNS / TCP / TLS trouble right after Wi-Fi came up: try again shortly
            ESP_LOGW(TAG, "connect attempt %d: %s — retrying", attempt + 1, err);
            char why[200]; snprintf(why, sizeof(why), "%s — retrying", err);
            set_state(CHRON_ST_CONNECTING, why);
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        if (e == ESP_ERR_INVALID_STATE && attempt == 0 && ch->refresh_token[0]) {  // authorization refused: try a fresh token once
            char rerr[120]; int exp = 0;
            char acc[CFG_TOKEN], ref[CFG_TOKEN];
            if (github_refresh(ch->github_client_id, ch->refresh_token, acc, sizeof(acc), ref, sizeof(ref), &exp, rerr, sizeof(rerr)) == 1) {
                strlcpy(ch->access_token, acc, sizeof(ch->access_token));
                if (ref[0]) strlcpy(ch->refresh_token, ref, sizeof(ch->refresh_token));
                ch->token_expiry = exp > 0 ? (int64_t)time(NULL) + exp : 0;
                app_config_save();
                continue;
            }
            snprintf(err, sizeof(err), "GitHub sign-in expired — sign in again on the setup page (%s)", rerr);
        }
        break;
    }
    if (e != ESP_OK) { set_state(CHRON_ST_FAILED, err); s_retry_at_ms = (e == ESP_ERR_INVALID_STATE) ? 0 : plat_now_ms() + 30000; xSemaphoreGive(s_conn_lock); return; }
    chron_user_info_t who;
    if (whoami(&who, err, sizeof(err)) != ESP_OK) { nats_glue_disconnect(); set_state(CHRON_ST_FAILED, err); xSemaphoreGive(s_conn_lock); return; }
    snprintf(s_principal, sizeof(s_principal), "%s", who.user);
    s_err[0] = 0;  // a retry made it: forget the transient error
    if (strcmp(who.account, ch->account) != 0) ESP_LOGW(TAG, "placed in account %s, expected %s", who.account, ch->account);
    s_live_session = true;
    if (start_pull(false, err, sizeof(err)) != ESP_OK) { s_live_session = false; nats_glue_disconnect(); set_state(CHRON_ST_FAILED, err); }
    xSemaphoreGive(s_conn_lock);
}
static void do_resync(void) {
    if (!s_live_session || !nats_glue_connected()) return;
    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    char err[160];
    teardown_session();
    if (start_pull(true, err, sizeof(err)) != ESP_OK) { ESP_LOGW(TAG, "resync: %s", err); set_state(CHRON_ST_RECONNECTING, err); }
    xSemaphoreGive(s_conn_lock);
}
static void do_disconnect(void) {
    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    s_live_session = false;
    teardown_session();
    nats_glue_disconnect();
    set_state(CHRON_ST_IDLE, NULL);
    xSemaphoreGive(s_conn_lock);
}
static bool need_live(void) {
    if (s_state != CHRON_ST_SYNCED && s_state != CHRON_ST_SYNCING) { emit(CHRON_EV_ERROR, NULL, "not connected to chronicle"); return false; }
    return true;
}
static void do_send(const char* room_id, const char* text) {
    if (!need_live()) return;
    char names[CHRON_MAX_MENTIONS][CHRON_NAME_MAX];
    int n = chron_mentions(text, names, CHRON_MAX_MENTIONS);
    chronicle_lock();
    chron_room_t* r = chron_ws_room(s_ws, room_id);
    bool direct = r && r->direct; bool closed = r && r->closed;
    char other[CHRON_NAME_MAX] = "";
    if (direct) { for (int i = 0; i < 2; i++) if (strcmp(r->participants[i], s_principal) != 0) snprintf(other, sizeof(other), "%s", r->participants[i]); }
    chronicle_unlock();
    if (closed) { emit(CHRON_EV_ERROR, room_id, "this room is closed"); return; }
    if (direct && other[0] && n == 0) { snprintf(names[0], CHRON_NAME_MAX, "%s", other); n = 1; }  // a conversation addresses its other party
    char* payload = chron_message_json(text, "message", names, n, NULL);
    char id[32]; chron_mint_id("m-", id, sizeof(id), rnd);
    char path[CHRON_ID_MAX * 2 + 32]; snprintf(path, sizeof(path), "room/%s/messages/%s", room_id, id);
    char err[160], code[48];
    esp_err_t e = apply(path, "create", payload, true, NULL, code, sizeof(code), err, sizeof(err));
    plat_free(payload);
    if (e != ESP_OK) emit(CHRON_EV_ERROR, room_id, err);
}
static void do_create_room(const char* title, const char* purpose) {
    if (!need_live()) return;
    char id[CHRON_ID_MAX]; chron_room_id_from_title(title, id, sizeof(id), rnd);
    char* payload = chron_room_json(title, purpose, "room", NULL, NULL);
    char path[CHRON_ID_MAX + 8]; snprintf(path, sizeof(path), "room/%s", id);
    char err[160], code[48];
    esp_err_t e = apply(path, "create", payload, true, NULL, code, sizeof(code), err, sizeof(err));
    plat_free(payload);
    if (e == ESP_OK || (e == ESP_ERR_INVALID_STATE && strcmp(code, "instance-exists") == 0)) emit(CHRON_EV_ROOM_OPENED, id, NULL);
    else emit(CHRON_EV_ERROR, NULL, err);
}
static void do_direct(const char* agent) {
    if (!need_live()) return;
    char id[CHRON_ID_MAX]; chron_direct_id(s_principal, agent, id, sizeof(id));
    chronicle_lock(); bool exists = chron_ws_room(s_ws, id) != NULL; chronicle_unlock();
    if (!exists) {
        char title[120]; snprintf(title, sizeof(title), "%s & %s", s_principal, agent);
        char* payload = chron_room_json(title, "a direct conversation", "direct", s_principal, agent);
        char path[CHRON_ID_MAX + 8]; snprintf(path, sizeof(path), "room/%s", id);
        char err[160], code[48];
        esp_err_t e = apply(path, "create", payload, true, NULL, code, sizeof(code), err, sizeof(err));
        plat_free(payload);
        if (e != ESP_OK && !(e == ESP_ERR_INVALID_STATE && strcmp(code, "instance-exists") == 0)) { emit(CHRON_EV_ERROR, NULL, err); return; }
    }
    emit(CHRON_EV_ROOM_OPENED, id, NULL);
}
static void do_close_room(const char* room_id) {
    if (!need_live()) return;
    char path[CHRON_ID_MAX + 8]; snprintf(path, sizeof(path), "room/%s", room_id);
    char err[160], code[48];
    if (apply(path, "close", "{}", false, NULL, code, sizeof(code), err, sizeof(err)) != ESP_OK) emit(CHRON_EV_ERROR, room_id, err);
}
static void watchdog(void) {
    if (s_want_session && s_state == CHRON_ST_FAILED && s_retry_at_ms && plat_now_ms() >= s_retry_at_ms) { s_retry_at_ms = 0; ESP_LOGI(TAG, "retrying the connection"); do_connect(); return; }
    if (!s_live_session || !nats_glue_connected() || !s_consumer[0]) return;
    if (s_fetch_outstanding && plat_now_ms() - s_last_rx_ms > FETCH_STALL_MS) {
        ESP_LOGW(TAG, "pull stalled (%lu batches, %lu ops, last status %d) — asking the server about the consumer", (unsigned long)s_batches, (unsigned long)s_delivered, s_last_status);
        char subj[160]; snprintf(subj, sizeof(subj), "$JS.API.CONSUMER.INFO.%s.%s", s_stream, s_consumer);
        nats_glue_reply_t r;
        esp_err_t e = nats_glue_request(subj, NULL, "", 5000, &r);
        if (e == ESP_OK) { const char* tail = r.data && r.len > 400 ? r.data + r.len - 400 : (r.data ? r.data : ""); ESP_LOGW(TAG, "consumer info (tail): %s", tail); nats_glue_reply_free(&r); }
        else ESP_LOGW(TAG, "consumer info: %s", esp_err_to_name(e));
        request_batch();
    }
}
static void worker(void* arg) {
    (void)arg;
    cmd_t* c;
    for (;;) {
        if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(5000)) != pdTRUE) { watchdog(); continue; }
        switch (c->kind) {
            case CMD_CONNECT: s_want_session = true; do_connect(); break;
            case CMD_DISCONNECT: s_want_session = false; s_retry_at_ms = 0; do_disconnect(); break;
            case CMD_RESYNC: do_resync(); break;
            case CMD_SEND: do_send(c->a, c->b ? c->b : ""); break;
            case CMD_CREATE_ROOM: do_create_room(c->b ? c->b : "", c->c); break;
            case CMD_DIRECT: do_direct(c->a); break;
            case CMD_CLOSE_ROOM: do_close_room(c->a); break;
        }
        plat_free(c->b);
        plat_free(c);
    }
}
static void post_cmd(cmd_kind_t kind, const char* a, const char* b, const char* c) {
    cmd_t* cmd = (cmd_t*)plat_calloc(1, sizeof(cmd_t));
    if (!cmd) return;
    cmd->kind = kind;
    if (a) snprintf(cmd->a, sizeof(cmd->a), "%s", a);
    if (b) cmd->b = plat_strdup(b);
    if (c) snprintf(cmd->c, sizeof(cmd->c), "%s", c);
    if (xQueueSend(s_q, &cmd, pdMS_TO_TICKS(500)) != pdTRUE) { plat_free(cmd->b); plat_free(cmd); }
}

void chronicle_init(void) {
    if (s_q) return;
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_conn_lock = xSemaphoreCreateMutex();
    s_q = xQueueCreate(8, sizeof(cmd_t*));
    s_ws = (chron_ws_t*)plat_calloc(1, sizeof(chron_ws_t));
    chron_ws_init(s_ws);
    nats_glue_start();
    nats_glue_set_state_cb(glue_state_cb, NULL);
    xTaskCreatePinnedToCore(worker, "chronw", 12288, NULL, 4, NULL, 1);  // HTTPS token refresh runs here
}
void chronicle_connect_async(void) { post_cmd(CMD_CONNECT, NULL, NULL, NULL); }
void chronicle_disconnect_async(void) { post_cmd(CMD_DISCONNECT, NULL, NULL, NULL); }
void chronicle_send_async(const char* room_id, const char* text) { post_cmd(CMD_SEND, room_id, text, NULL); }
void chronicle_create_room_async(const char* title, const char* purpose) { post_cmd(CMD_CREATE_ROOM, NULL, title, purpose); }
void chronicle_open_direct_async(const char* agent) { post_cmd(CMD_DIRECT, agent, NULL, NULL); }
void chronicle_close_room_async(const char* room_id) { post_cmd(CMD_CLOSE_ROOM, room_id, NULL, NULL); }
void chronicle_set_open_room(const char* room_id) {
    snprintf(s_open_room, sizeof(s_open_room), "%s", room_id ? room_id : "");
    if (s_live_session && nats_glue_connected() && !nats_glue_on_owner_task()) live_room_follow();
}
int chronicle_agents_online(void) {
    int n = 0; int64_t now = plat_now_ms();
    chronicle_lock();
    for (int i = 0; i < s_ws->nagents; i++) if (s_ws->agents[i].last_seen_ms && now - s_ws->agents[i].last_seen_ms < PRESENCE_TTL_MS) n++;
    chronicle_unlock();
    return n;
}

char* chronicle_debug_json(void) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", chronicle_state_text());
    cJSON_AddStringToObject(o, "error", s_err);
    cJSON_AddStringToObject(o, "principal", s_principal);
    cJSON_AddStringToObject(o, "store", s_store);
    cJSON_AddStringToObject(o, "stream", s_stream);
    cJSON_AddStringToObject(o, "consumer", s_consumer);
    cJSON_AddBoolToObject(o, "nats_connected", nats_glue_connected());
    cJSON_AddBoolToObject(o, "live_session", s_live_session);
    cJSON_AddNumberToObject(o, "batches", s_batches);
    cJSON_AddNumberToObject(o, "delivered", s_delivered);
    cJSON_AddNumberToObject(o, "statuses", s_statuses);
    cJSON_AddNumberToObject(o, "last_status", s_last_status);
    cJSON_AddNumberToObject(o, "last_pending", (double)s_last_pending);
    cJSON_AddBoolToObject(o, "fetch_outstanding", s_fetch_outstanding);
    cJSON_AddNumberToObject(o, "since_rx_ms", (double)(plat_now_ms() - s_last_rx_ms));
    cJSON_AddStringToObject(o, "open_room", s_open_room);
    chronicle_lock();
    cJSON_AddNumberToObject(o, "head_seq", (double)s_ws->head_seq);
    cJSON_AddNumberToObject(o, "rooms", s_ws->nrooms);
    cJSON_AddNumberToObject(o, "agents", s_ws->nagents);
    cJSON* rooms = cJSON_AddArrayToObject(o, "room_list");
    for (int i = 0; i < s_ws->nrooms; i++) {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "id", s_ws->rooms[i].id);
        cJSON_AddNumberToObject(r, "msgs", s_ws->rooms[i].total);
        cJSON_AddBoolToObject(r, "closed", s_ws->rooms[i].closed);
        cJSON_AddItemToArray(rooms, r);
    }
    chronicle_unlock();
    char* s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

// ---------------------------------------------------------------------------------------
// setup-flow helpers (blocking; caller's task)
// ---------------------------------------------------------------------------------------
esp_err_t chronicle_fetch_profile(char* err, size_t en) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    char url[CFG_URL + 48]; snprintf(url, sizeof(url), "%s/.well-known/chronicle/profile.json", ch->site[0] ? ch->site : "https://chronicle.impire.dev");
    http_header_t hdrs[] = {{"Accept", "application/json"}, {"User-Agent", "ai-terminal"}};
    http_stream_req_t req = {.url = url, .method = "GET", .headers = hdrs, .n_headers = 2, .connect_timeout_ms = 15000, .read_timeout_ms = 15000, .total_timeout_ms = 20000};
    http_stream_res_t res; char* body = NULL;
    esp_err_t e = http_fetch(&req, &res, &body, 64 * 1024);
    if (e != ESP_OK || res.status != 200 || !body) { snprintf(err, en, "profile: %s", e != ESP_OK ? (res.error[0] ? res.error : esp_err_to_name(e)) : "unexpected HTTP status"); plat_free(body); return ESP_FAIL; }
    chron_profile_t p;
    bool ok = chron_parse_profile(body, &p);
    plat_free(body);
    if (!ok) { snprintf(err, en, "profile: missing url, github_client_id or sentinel"); plat_free(p.sentinel); return ESP_FAIL; }
    if (strlen(p.sentinel) >= sizeof(ch->sentinel)) { snprintf(err, en, "profile: sentinel too long"); plat_free(p.sentinel); return ESP_FAIL; }
    strlcpy(ch->nats_url, p.url, sizeof(ch->nats_url));
    strlcpy(ch->github_client_id, p.github_client_id, sizeof(ch->github_client_id));
    strlcpy(ch->sentinel, p.sentinel, sizeof(ch->sentinel));
    plat_free(p.sentinel);
    app_config_save();
    return ESP_OK;
}
esp_err_t chronicle_fresh_token(char* err, size_t en) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    err[0] = 0;
    if (!ch->refresh_token[0] || ch->token_expiry == 0) return ESP_OK;
    time_t now = time(NULL);
    if (now < 1700000000) return ESP_OK;  // no clock yet: try the token as it is
    if ((int64_t)now < ch->token_expiry - 60) return ESP_OK;
    char acc[CFG_TOKEN], ref[CFG_TOKEN]; int exp = 0;
    if (github_refresh(ch->github_client_id, ch->refresh_token, acc, sizeof(acc), ref, sizeof(ref), &exp, err, en) != 1) return ESP_FAIL;
    strlcpy(ch->access_token, acc, sizeof(ch->access_token));
    if (ref[0]) strlcpy(ch->refresh_token, ref, sizeof(ch->refresh_token));
    ch->token_expiry = exp > 0 ? (int64_t)now + exp : 0;
    app_config_save();
    ESP_LOGI(TAG, "GitHub token refreshed");
    return ESP_OK;
}
esp_err_t chronicle_identity(const char* token, chron_user_info_t* who, chron_membership_t* out, int max, int* n, char* err, size_t en) {
    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    s_live_session = false;
    teardown_session();
    if (s_state != CHRON_ST_IDLE) set_state(CHRON_ST_IDLE, NULL);
    esp_err_t e = glue_connect_with("+", token, err, en);
    if (e != ESP_OK) { xSemaphoreGive(s_conn_lock); return e; }
    e = whoami(who, err, en);
    if (e == ESP_OK && (who->github_id == 0 || !who->login[0])) { snprintf(err, en, "the placement names no GitHub identity"); e = ESP_FAIL; }
    if (e == ESP_OK) {
        char subject[160]; snprintf(subject, sizeof(subject), "CHRON.CTRL.IDENTITY.MEMBERSHIPS.%lld.%s", (long long)who->github_id, who->login);
        nats_glue_reply_t r;
        e = nats_glue_request(subject, NULL, "{}", 8000, &r);
        if (e != ESP_OK) snprintf(err, en, "memberships: %s", e == ESP_ERR_TIMEOUT ? "timeout" : e == ESP_ERR_NOT_FOUND ? "no responder" : esp_err_to_name(e));
        else {
            char code[48] = "";
            if (chron_header_get(r.headers, r.header_len, "Nats-Service-Error-Code", code, sizeof(code)) && code[0]) { snprintf(err, en, "memberships: %s", code); e = ESP_FAIL; }
            else { *n = chron_parse_memberships(r.data ? r.data : "", out, max); if (*n < 0) { snprintf(err, en, "memberships: bad reply"); e = ESP_FAIL; } }
        }
        nats_glue_reply_free(&r);
    }
    nats_glue_disconnect();
    xSemaphoreGive(s_conn_lock);
    return e;
}
char* chronicle_test_account_json(void) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    cJSON* out = cJSON_CreateObject();
    char err[160] = "";
    xSemaphoreTake(s_conn_lock, portMAX_DELAY);
    s_live_session = false;
    teardown_session();
    if (s_state != CHRON_ST_IDLE) set_state(CHRON_ST_IDLE, NULL);
    esp_err_t e = ESP_FAIL;
    if (!app_config_has_chronicle()) snprintf(err, sizeof(err), "not signed in");
    else if (chronicle_fresh_token(err, sizeof(err)) != ESP_OK) {}
    else e = glue_connect_with(ch->account, ch->access_token, err, sizeof(err));
    if (e == ESP_OK) {
        chron_user_info_t who;
        e = whoami(&who, err, sizeof(err));
        if (e == ESP_OK) {
            cJSON_AddBoolToObject(out, "ok", true);
            cJSON_AddStringToObject(out, "principal", who.user);
            cJSON_AddStringToObject(out, "account", who.account);
            char stream[64]; size_t o = (size_t)snprintf(stream, sizeof(stream), "LOG_");
            for (const char* p = ch->store; *p && o + 1 < sizeof(stream); p++) stream[o++] = *p == '-' ? '_' : (char)toupper((unsigned char)*p);
            stream[o] = 0;
            char subject[128]; snprintf(subject, sizeof(subject), "$JS.API.STREAM.INFO.%s", stream);
            nats_glue_reply_t r;
            if (nats_glue_request(subject, NULL, "", 8000, &r) == ESP_OK) {
                cJSON* info = cJSON_Parse(r.data ? r.data : "");
                cJSON* st = info ? cJSON_GetObjectItemCaseSensitive(info, "state") : NULL;
                cJSON* msgs = st ? cJSON_GetObjectItemCaseSensitive(st, "messages") : NULL;
                cJSON* er = info ? cJSON_GetObjectItemCaseSensitive(info, "error") : NULL;
                if (cJSON_IsNumber(msgs)) cJSON_AddNumberToObject(out, "ops", msgs->valuedouble);
                else if (er) cJSON_AddStringToObject(out, "store", "not found — enable chronicle-agents in the console first");
                cJSON_Delete(info);
                nats_glue_reply_free(&r);
            }
        }
        nats_glue_disconnect();
    }
    xSemaphoreGive(s_conn_lock);
    if (e != ESP_OK) { cJSON_AddBoolToObject(out, "ok", false); cJSON_AddStringToObject(out, "error", err); }
    char* s = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    return s;
}

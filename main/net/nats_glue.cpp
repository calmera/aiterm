// C API over espidf-nats; see nats_glue.h. Everything that touches the NATS object runs on the
// owner task: other tasks queue a command and wait on its semaphore.
#include "nats_glue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nkeys.h"
#include "util/platform.h"

#define NATS_CONF_VERBOSE false
#include "espidf_nats.h"

static const char* TAG = "nats";

#define MAX_SUBS 16
#define MAX_REQS 6

typedef struct { int sid; nats_glue_msg_cb cb; void* user; } sub_slot_t;
typedef struct { bool used; int sid; SemaphoreHandle_t sem; nats_glue_reply_t* out; bool done; } req_slot_t;

enum cmd_kind { CMD_CONNECT, CMD_DISCONNECT, CMD_SUB, CMD_UNSUB, CMD_PUB, CMD_REQ_START, CMD_REQ_CANCEL };
typedef struct {
    cmd_kind kind;
    const nats_glue_cfg_t* cfg; int timeout_ms; char* err; size_t err_len;   // connect
    const char* subject; const char* headers; const char* data; const char* reply;  // pub / sub / req
    nats_glue_msg_cb cb; void* user; int sid;                                   // sub / unsub
    int req_slot;                                                               // req
    esp_err_t result;
    SemaphoreHandle_t done;
} cmd_t;

static TaskHandle_t       s_task = NULL;
static QueueHandle_t      s_q = NULL;
static NATS*              s_nats = NULL;
static sub_slot_t         s_subs[MAX_SUBS];
static req_slot_t         s_reqs[MAX_REQS];
static SemaphoreHandle_t  s_req_lock = NULL;
static nats_glue_state_cb s_state_cb = NULL;
static void*              s_state_user = NULL;
static volatile bool      s_auth_error = false;
static bool               s_was_connected = false;
// stable storage for strings the NATS object keeps pointers to
static char s_host[128], s_jwt[2048], s_seed[80], s_pub[64], s_token[256], s_name[32];
static nats_tls_config_t  s_tls;
static nats_auth_config_t s_auth;

// ---------------------------------------------------------------------------------------
// callbacks from the library (owner task)
// ---------------------------------------------------------------------------------------
static bool sign_cb(const uint8_t* nonce, size_t nonce_len, uint8_t* signature, size_t* sig_len, const char* seed) {
    if (!nkeys_sign(seed, nonce, nonce_len, signature)) return false;
    *sig_len = 64;
    return true;
}
static char s_server_err[160] = "";
static void on_error_cb(void) {
    if (s_nats) snprintf(s_server_err, sizeof(s_server_err), "%s", s_nats->last_server_error);
    bool auth = strstr(s_server_err, "Authorization") != NULL || strstr(s_server_err, "authorization") != NULL;
    if (auth) {
        s_auth_error = true;
        if (s_nats) s_nats->max_reconnect_attempts = 0;  // retrying with the same token is pointless; the owner reconnects with a fresh one
    }
    ESP_LOGW(TAG, "server error: %s", s_server_err);
}
static void on_connect_cb(void) {
    ESP_LOGI(TAG, "connected");
    if (s_was_connected && s_state_cb) s_state_cb(true, s_state_user);  // a reconnect; the first connect is reported by the connect command
    s_was_connected = true;
}
static void on_disconnect_cb(void) {
    ESP_LOGW(TAG, "disconnected");
    if (s_state_cb) s_state_cb(false, s_state_user);
}
static void sub_tramp(nats_msg_t e) {
    for (int i = 0; i < MAX_SUBS; i++) {
        if (s_subs[i].cb && s_subs[i].sid == e.sid) {
            s_subs[i].cb(e.subject ? e.subject : "", e.reply ? e.reply : "", e.headers, e.header_size, e.data ? e.data : "", e.size, s_subs[i].user);
            return;
        }
    }
}
static void req_tramp(nats_msg_t e) {
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_REQS; i++) {
        req_slot_t* r = &s_reqs[i];
        if (!r->used || r->sid != e.sid || r->done) continue;
        nats_glue_reply_t* out = r->out;
        memset(out, 0, sizeof(*out));
        if (e.headers && e.header_size > 0) {
            out->headers = (char*)plat_malloc((size_t)e.header_size + 1);
            if (out->headers) { memcpy(out->headers, e.headers, (size_t)e.header_size); out->headers[e.header_size] = 0; out->header_len = e.header_size; }
            // "NATS/1.0 503" etc.
            if (e.header_size >= 12 && strncmp(e.headers, "NATS/1.0 ", 9) == 0) out->status = atoi(e.headers + 9);
        }
        out->data = (char*)plat_malloc((size_t)(e.size > 0 ? e.size : 0) + 1);
        if (out->data) { if (e.size > 0) memcpy(out->data, e.data, (size_t)e.size); out->data[e.size > 0 ? e.size : 0] = 0; out->len = e.size > 0 ? e.size : 0; }
        r->done = true;
        xSemaphoreGive(r->sem);
        break;
    }
    xSemaphoreGive(s_req_lock);
}

// ---------------------------------------------------------------------------------------
// command execution (owner task)
// ---------------------------------------------------------------------------------------
static void drop_connection(void) {
    if (!s_nats) return;
    s_nats->on_disconnect = NULL;  // no state callback for a deliberate drop
    s_nats->disconnect();
    delete s_nats;
    s_nats = NULL;
    memset(s_subs, 0, sizeof(s_subs));
    s_was_connected = false;
}
static esp_err_t do_connect(const nats_glue_cfg_t* cfg, int timeout_ms, char* err, size_t err_len) {
    drop_connection();
    s_auth_error = false;
    snprintf(s_host, sizeof(s_host), "%s", cfg->host);
    snprintf(s_name, sizeof(s_name), "%s", cfg->name ? cfg->name : "aiterm");
    memset(&s_tls, 0, sizeof(s_tls));
    s_tls.enabled = cfg->tls;
    s_tls.server_name = s_host;
    s_tls.crt_bundle_attach = (void*)esp_crt_bundle_attach;
    s_nats = new NATS(s_host, cfg->port, NULL, NULL, cfg->tls ? &s_tls : NULL);
    if (!s_nats) { snprintf(err, err_len, "out of memory"); return ESP_ERR_NO_MEM; }
    memset(&s_auth, 0, sizeof(s_auth));
    if (cfg->jwt && cfg->jwt[0]) {
        snprintf(s_jwt, sizeof(s_jwt), "%s", cfg->jwt);
        snprintf(s_seed, sizeof(s_seed), "%s", cfg->nkey_seed ? cfg->nkey_seed : "");
        if (!s_seed[0] || !nkeys_public_from_seed(s_seed, s_pub, sizeof(s_pub))) {
            snprintf(err, err_len, "bad sentinel seed");
            drop_connection();
            return ESP_ERR_INVALID_ARG;
        }
        ESP_LOGI(TAG, "sentinel nkey %s", s_pub);  // must equal the JWT's sub claim
        s_auth.method = NATS_AUTH_CREDENTIALS;
        s_auth.jwt = s_jwt;
        s_auth.nkey_seed = s_seed;
        s_auth.nkey_public = s_pub;
        s_auth.sign_fn = sign_cb;
        if (cfg->token && cfg->token[0]) { snprintf(s_token, sizeof(s_token), "%s", cfg->token); s_auth.token = s_token; }
    } else if (cfg->token && cfg->token[0]) {
        snprintf(s_token, sizeof(s_token), "%s", cfg->token);
        s_auth.method = NATS_AUTH_TOKEN;
        s_auth.token = s_token;
    } else {
        s_auth.method = NATS_AUTH_NONE;
    }
    s_nats->set_auth(&s_auth);
    s_nats->on_error = on_error_cb;
    s_nats->on_connect = on_connect_cb;
    s_nats->on_disconnect = on_disconnect_cb;
    s_nats->max_reconnect_attempts = -1;
    s_nats->message_buffering_enabled = false;
    s_server_err[0] = 0;
    bool ok = s_nats->connect((unsigned long)(timeout_ms > 0 ? timeout_ms : 15000));
    // connect() returns once CONNECT was sent; the server's verdict (an auth callout takes a few
    // hundred ms) arrives after it. A PING answered by PONG means we are in; -ERR means we are not.
    if (ok) ok = s_nats->flush(6000) && s_nats->connected && !s_auth_error;
    if (!ok) {
        if (s_auth_error) snprintf(err, err_len, "authorization refused by the server (%s)", s_server_err);
        else if (s_server_err[0]) snprintf(err, err_len, "server error: %s", s_server_err);
        else snprintf(err, err_len, "%s", s_nats->connected ? "no answer to PING" : s_nats->last_error_string());
        drop_connection();
        return s_auth_error ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    return ESP_OK;
}
static int do_subscribe(const char* subject, nats_glue_msg_cb cb, void* user) {
    if (!s_nats || !s_nats->connected) return -1;
    int slot = -1;
    for (int i = 0; i < MAX_SUBS; i++) if (!s_subs[i].cb) { slot = i; break; }
    if (slot < 0) return -1;
    s_subs[slot].cb = cb; s_subs[slot].user = user; s_subs[slot].sid = -1;
    int sid = s_nats->subscribe(subject, sub_tramp, NULL, 0);
    if (sid < 0) { s_subs[slot].cb = NULL; return -1; }
    s_subs[slot].sid = sid;
    return sid;
}
static void do_unsubscribe(int sid) {
    if (sid < 0) return;
    for (int i = 0; i < MAX_SUBS; i++) if (s_subs[i].cb && s_subs[i].sid == sid) { s_subs[i].cb = NULL; break; }
    if (s_nats) s_nats->unsubscribe(sid);
}
static esp_err_t do_publish(const char* subject, const char* headers, const char* data, const char* reply) {
    if (!s_nats || !s_nats->connected) return ESP_ERR_INVALID_STATE;
    if (headers && headers[0]) s_nats->publish_with_headers(subject, headers, data ? data : "", reply && reply[0] ? reply : NULL);
    else s_nats->publish(subject, data ? data : "", reply && reply[0] ? reply : NULL);
    return ESP_OK;
}
static esp_err_t do_request_start(int slot, const char* subject, const char* headers, const char* data) {
    if (!s_nats || !s_nats->connected) return ESP_ERR_INVALID_STATE;
    char inbox[64]; nats_glue_new_inbox(inbox, sizeof(inbox));
    int sid = s_nats->subscribe(inbox, req_tramp, NULL, 1);
    if (sid < 0) return ESP_FAIL;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    s_reqs[slot].sid = sid;
    xSemaphoreGive(s_req_lock);
    return do_publish(subject, headers, data, inbox);
}
static void do_request_cancel(int slot) {
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    int sid = s_reqs[slot].sid;
    bool done = s_reqs[slot].done;
    xSemaphoreGive(s_req_lock);
    if (!done && s_nats && sid >= 0) s_nats->unsubscribe(sid);
}
static void exec_cmd(cmd_t* c) {
    switch (c->kind) {
        case CMD_CONNECT: c->result = do_connect(c->cfg, c->timeout_ms, c->err, c->err_len); if (c->result == ESP_OK && s_state_cb) s_state_cb(true, s_state_user); break;
        case CMD_DISCONNECT: drop_connection(); c->result = ESP_OK; break;
        case CMD_SUB: c->sid = do_subscribe(c->subject, c->cb, c->user); c->result = c->sid >= 0 ? ESP_OK : ESP_FAIL; break;
        case CMD_UNSUB: do_unsubscribe(c->sid); c->result = ESP_OK; break;
        case CMD_PUB: c->result = do_publish(c->subject, c->headers, c->data, c->reply); break;
        case CMD_REQ_START: c->result = do_request_start(c->req_slot, c->subject, c->headers, c->data); break;
        case CMD_REQ_CANCEL: do_request_cancel(c->req_slot); c->result = ESP_OK; break;
    }
}
static void run_cmd(cmd_t* c) {
    if (nats_glue_on_owner_task()) { exec_cmd(c); return; }
    c->done = xSemaphoreCreateBinary();
    xQueueSend(s_q, &c, portMAX_DELAY);
    xSemaphoreTake(c->done, portMAX_DELAY);
    vSemaphoreDelete(c->done);
}
static void owner_task(void* arg) {
    (void)arg;
    cmd_t* c;
    for (;;) {
        if (xQueueReceive(s_q, &c, pdMS_TO_TICKS(5)) == pdTRUE) {
            exec_cmd(c);
            xSemaphoreGive(c->done);
        }
        if (s_nats) s_nats->process();
    }
}

// ---------------------------------------------------------------------------------------
// public API
// ---------------------------------------------------------------------------------------
extern "C" {

esp_err_t nats_glue_start(void) {
    if (s_task) return ESP_OK;
    s_q = xQueueCreate(8, sizeof(cmd_t*));
    s_req_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_REQS; i++) { s_reqs[i].sem = xSemaphoreCreateBinary(); }
    if (xTaskCreatePinnedToCore(owner_task, "nats", 16384, NULL, 5, &s_task, 1) != pdPASS) return ESP_FAIL;  // the TLS handshake runs here
    return ESP_OK;
}
bool nats_glue_on_owner_task(void) { return s_task && xTaskGetCurrentTaskHandle() == s_task; }
bool nats_glue_auth_error(void) { return s_auth_error; }
const char* nats_glue_server_error(void) { return s_server_err; }

esp_err_t nats_glue_connect(const nats_glue_cfg_t* cfg, int timeout_ms, char* err, size_t err_len) {
    if (err && err_len) err[0] = 0;
    if (nats_glue_on_owner_task()) return ESP_ERR_INVALID_STATE;
    cmd_t c = {}; c.kind = CMD_CONNECT; c.cfg = cfg; c.timeout_ms = timeout_ms; c.err = err; c.err_len = err_len;
    run_cmd(&c);
    return c.result;
}
void nats_glue_disconnect(void) { cmd_t c = {}; c.kind = CMD_DISCONNECT; run_cmd(&c); }
bool nats_glue_connected(void) { return s_nats && s_nats->connected; }
void nats_glue_set_state_cb(nats_glue_state_cb cb, void* user) { s_state_cb = cb; s_state_user = user; }

int nats_glue_subscribe(const char* subject, nats_glue_msg_cb cb, void* user) {
    cmd_t c = {}; c.kind = CMD_SUB; c.subject = subject; c.cb = cb; c.user = user;
    run_cmd(&c);
    return c.result == ESP_OK ? c.sid : -1;
}
void nats_glue_unsubscribe(int sid) { cmd_t c = {}; c.kind = CMD_UNSUB; c.sid = sid; run_cmd(&c); }
esp_err_t nats_glue_publish(const char* subject, const char* headers, const char* data, const char* reply) {
    cmd_t c = {}; c.kind = CMD_PUB; c.subject = subject; c.headers = headers; c.data = data; c.reply = reply;
    run_cmd(&c);
    return c.result;
}
void nats_glue_new_inbox(char* out, size_t n) {
    uint32_t a = esp_random(), b = esp_random();
    snprintf(out, n, "_INBOX.aiterm.%08lx%08lx", (unsigned long)a, (unsigned long)b);
}
esp_err_t nats_glue_request(const char* subject, const char* headers, const char* data, int timeout_ms, nats_glue_reply_t* out) {
    memset(out, 0, sizeof(*out));
    if (nats_glue_on_owner_task()) return ESP_ERR_INVALID_STATE;  // would block the protocol loop
    if (!nats_glue_connected()) return ESP_ERR_INVALID_STATE;
    int slot = -1;
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_REQS; i++) if (!s_reqs[i].used) { slot = i; s_reqs[i].used = true; s_reqs[i].done = false; s_reqs[i].sid = -1; s_reqs[i].out = out; break; }
    xSemaphoreGive(s_req_lock);
    if (slot < 0) return ESP_ERR_NO_MEM;
    xSemaphoreTake(s_reqs[slot].sem, 0);  // clear a stale give
    cmd_t c = {}; c.kind = CMD_REQ_START; c.req_slot = slot; c.subject = subject; c.headers = headers; c.data = data;
    run_cmd(&c);
    esp_err_t r = c.result;
    if (r == ESP_OK) {
        if (xSemaphoreTake(s_reqs[slot].sem, pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 5000)) != pdTRUE) {
            cmd_t k = {}; k.kind = CMD_REQ_CANCEL; k.req_slot = slot; run_cmd(&k);
            // the reply may have landed between the timeout and the cancel: then it counts
            xSemaphoreTake(s_req_lock, portMAX_DELAY);
            bool done = s_reqs[slot].done;
            xSemaphoreGive(s_req_lock);
            r = done ? ESP_OK : ESP_ERR_TIMEOUT;
        }
        if (r == ESP_OK && out->status == 503) r = ESP_ERR_NOT_FOUND;  // no responders
    }
    xSemaphoreTake(s_req_lock, portMAX_DELAY);
    s_reqs[slot].used = false; s_reqs[slot].out = NULL;
    xSemaphoreGive(s_req_lock);
    if (r != ESP_OK) { nats_glue_reply_free(out); memset(out, 0, sizeof(*out)); }  // callers own the reply only on success
    return r;
}
void nats_glue_reply_free(nats_glue_reply_t* r) {
    if (!r) return;
    plat_free(r->headers); plat_free(r->data);
    r->headers = NULL; r->data = NULL; r->header_len = 0; r->len = 0;
}

}  // extern "C"

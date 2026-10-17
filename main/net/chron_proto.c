#include "chron_proto.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "util/jsonbuf.h"
#include "util/platform.h"

// ---------------------------------------------------------------------------------------
// subjects, paths, ids
// ---------------------------------------------------------------------------------------
bool chron_path_to_tail(const char* path, char* out, size_t n) {
    size_t l = strlen(path);
    if (l == 0 || l + 1 > n) return false;
    for (size_t i = 0; i <= l; i++) out[i] = path[i] == '/' ? '.' : path[i];
    return true;
}
bool chron_subject_to_path(const char* store, const char* subject, char* out, size_t n) {
    char prefix[96];
    snprintf(prefix, sizeof(prefix), "CHRON.%s.OPS.", store);
    size_t pl = strlen(prefix);
    if (strncmp(subject, prefix, pl) != 0) return false;
    const char* tail = subject + pl;
    size_t l = strlen(tail);
    if (l == 0 || l + 1 > n) return false;
    for (size_t i = 0; i <= l; i++) out[i] = tail[i] == '.' ? '/' : tail[i];
    return true;
}
void chron_ops_subject(const char* store, const char* path, char* out, size_t n) {
    char tail[192]; chron_path_to_tail(path, tail, sizeof(tail));
    snprintf(out, n, "CHRON.%s.OPS.%s", store, tail);
}
void chron_apply_subject(const char* principal, const char* store, const char* path, char* out, size_t n) {
    char tail[192]; chron_path_to_tail(path, tail, sizeof(tail));
    snprintf(out, n, "CHRON.API.%s.APPLY.%s.%s", principal, store, tail);
}
static uint32_t default_rnd(void) { return (uint32_t)rand(); }
void chron_mint_id(const char* prefix, char* out, size_t n, uint32_t (*rnd)(void)) {
    if (!rnd) rnd = default_rnd;
    size_t o = (size_t)snprintf(out, n, "%s", prefix ? prefix : "");
    for (int i = 0; i < 12 && o + 1 < n; i++) out[o++] = "0123456789abcdef"[rnd() & 15];
    out[o] = 0;
}
void chron_nuid(char* out, size_t n, uint32_t (*rnd)(void)) {
    static const char AL[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    if (!rnd) rnd = default_rnd;
    size_t o = 0;
    for (int i = 0; i < 22 && o + 1 < n; i++) out[o++] = AL[rnd() % 62];
    out[o] = 0;
}
void chron_room_id_from_title(const char* title, char* out, size_t n, uint32_t (*rnd)(void)) {
    size_t o = 0; bool dash = false;
    for (const char* p = title; *p && o + 1 < n && o < 40; p++) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c)) { out[o++] = (char)tolower(c); dash = false; }
        else if (!dash && o > 0) { out[o++] = '-'; dash = true; }
    }
    while (o > 0 && out[o - 1] == '-') o--;
    out[o] = 0;
    if (o == 0) chron_mint_id("r-", out, n, rnd);
}
void chron_direct_id(const char* a, const char* b, char* out, size_t n) {
    if (strcmp(a, b) > 0) { const char* t = a; a = b; b = t; }
    snprintf(out, n, "direct--%s--%s", a, b);
}
bool chron_parse_nats_url(const char* url, char* host, size_t hn, int* port, bool* tls) {
    const char* p = url;
    *tls = false; *port = 4222;
    if (strncmp(p, "tls://", 6) == 0) { *tls = true; p += 6; }
    else if (strncmp(p, "nats://", 7) == 0) p += 7;
    else if (strstr(p, "://")) return false;
    const char* colon = strrchr(p, ':');
    const char* end = strchr(p, '/');
    if (!end) end = p + strlen(p);
    if (colon && colon < end) {
        *port = atoi(colon + 1);
        if (*port <= 0 || *port > 65535) return false;
        end = colon;
    }
    size_t l = (size_t)(end - p);
    if (l == 0 || l + 1 > hn) return false;
    memcpy(host, p, l); host[l] = 0;
    return true;
}
static int ack_tokens(const char* reply, const char* tok[16]) {
    if (!reply || strncmp(reply, "$JS.ACK.", 8) != 0) return 0;
    int n = 0; const char* p = reply;
    while (n < 16) { tok[n++] = p; const char* d = strchr(p, '.'); if (!d) break; p = d + 1; }
    return n;
}
uint64_t chron_ack_stream_seq(const char* reply) {
    const char* tok[16]; int n = ack_tokens(reply, tok);
    int idx = n >= 12 ? 7 : (n >= 9 ? 5 : -1);
    return idx < 0 ? 0 : strtoull(tok[idx], NULL, 10);
}
int64_t chron_ack_pending(const char* reply) {
    const char* tok[16]; int n = ack_tokens(reply, tok);
    int idx = n >= 12 ? 10 : (n >= 9 ? 8 : -1);
    return idx < 0 ? -1 : (int64_t)strtoll(tok[idx], NULL, 10);
}

// ---------------------------------------------------------------------------------------
// headers
// ---------------------------------------------------------------------------------------
char* chron_op_headers(const char* op_id, const char* op_type, const char* author, const char* ts, bool expect_absent) {
    jsonbuf_t b; jb_init(&b);
    jb_append_str(&b, "NATS/1.0\r\n");
    jb_appendf(&b, "Nats-Msg-Id: %s\r\n", op_id);
    jb_appendf(&b, "Op-Type: %s\r\n", op_type);
    jb_appendf(&b, "Op-Author: %s\r\n", author);
    if (ts && ts[0]) jb_appendf(&b, "Op-Ts: %s\r\n", ts);
    jb_append_str(&b, "Op-Version: 1\r\n");
    if (expect_absent) jb_append_str(&b, "Nats-Expected-Last-Subject-Sequence: 0\r\n");
    jb_append_str(&b, "\r\n");
    return jb_detach(&b);
}
bool chron_header_get(const char* headers, int len, const char* key, char* out, size_t n) {
    if (!headers || len <= 0) return false;
    size_t kl = strlen(key);
    const char* p = headers; const char* end = headers + len;
    // skip the status line
    const char* nl = memchr(p, '\n', (size_t)(end - p));
    if (!nl) return false;
    p = nl + 1;
    while (p < end) {
        nl = memchr(p, '\n', (size_t)(end - p));
        const char* le = nl ? nl : end;
        const char* ve = le; if (ve > p && ve[-1] == '\r') ve--;
        if (ve == p) break;  // empty line: end of headers
        if ((size_t)(ve - p) > kl && strncasecmp(p, key, kl) == 0 && p[kl] == ':') {
            const char* v = p + kl + 1;
            while (v < ve && *v == ' ') v++;
            size_t vl = (size_t)(ve - v);
            if (vl + 1 > n) vl = n - 1;
            memcpy(out, v, vl); out[vl] = 0;
            return true;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}
int chron_header_status(const char* headers, int len) {
    if (!headers || len < 12 || strncmp(headers, "NATS/1.0", 8) != 0) return 0;
    const char* p = headers + 8;
    while (p < headers + len && *p == ' ') p++;
    if (p >= headers + len || !isdigit((unsigned char)*p)) return 0;
    return atoi(p);
}
static bool between(const char* s, const char* begin_mark, const char* end_mark, char* out, size_t n) {
    const char* b = strstr(s, begin_mark);
    if (!b) return false;
    b = strchr(b, '\n'); if (!b) return false; b++;
    const char* em = strstr(b, end_mark);
    if (!em) return false;
    const char* e = em;
    while (e > b && e[-1] != '\n') e--;  // start of the end-marker line
    while (e > b && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) e--;
    while (b < e && (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')) b++;
    size_t l = (size_t)(e - b);
    if (l == 0 || l + 1 > n) return false;
    memcpy(out, b, l); out[l] = 0;
    return true;
}
bool chron_parse_creds(const char* creds, char* jwt, size_t jn, char* seed, size_t sn) {
    if (!between(creds, "BEGIN NATS USER JWT", "END NATS USER JWT", jwt, jn)) return false;
    if (!between(creds, "BEGIN USER NKEY SEED", "END USER NKEY SEED", seed, sn)) return false;
    // tolerate a line wrap in either value
    for (char* p = jwt; *p; p++) if (*p == '\n' || *p == '\r') { memmove(p, p + 1, strlen(p)); p--; }
    for (char* p = seed; *p; p++) if (*p == '\n' || *p == '\r') { memmove(p, p + 1, strlen(p)); p--; }
    return seed[0] == 'S';
}

// ---------------------------------------------------------------------------------------
// payloads
// ---------------------------------------------------------------------------------------
static bool name_ch(char c) { return isalnum((unsigned char)c) || c == '_' || c == '-'; }
int chron_mentions(const char* text, char out[][CHRON_NAME_MAX], int max) {
    int n = 0;
    for (const char* p = text; *p; p++) {
        if (*p != '@') continue;
        if (p > text && name_ch(p[-1])) continue;
        const char* s = p + 1; const char* e = s;
        while (name_ch(*e)) e++;
        if (e == s) continue;
        size_t l = (size_t)(e - s);
        if (l >= CHRON_NAME_MAX) l = CHRON_NAME_MAX - 1;
        char name[CHRON_NAME_MAX]; memcpy(name, s, l); name[l] = 0;
        bool dup = false;
        for (int i = 0; i < n; i++) if (strcmp(out[i], name) == 0) { dup = true; break; }
        if (!dup && n < max) { strcpy(out[n++], name); }
        p = e - 1;
    }
    return n;
}
char* chron_message_json(const char* text, const char* kind, char mentions[][CHRON_NAME_MAX], int n, const char* reply_to) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "text", text);
    cJSON_AddStringToObject(o, "kind", kind && kind[0] ? kind : "message");
    cJSON* arr = cJSON_AddArrayToObject(o, "mentions");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(mentions[i]));
    if (reply_to && reply_to[0]) cJSON_AddStringToObject(o, "reply_to", reply_to);
    char* s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char* out = plat_strdup(s ? s : "{}");
    free(s);
    return out;
}
char* chron_room_json(const char* title, const char* purpose, const char* kind, const char* p1, const char* p2) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "title", title);
    if (purpose && purpose[0]) cJSON_AddStringToObject(o, "purpose", purpose);
    cJSON_AddStringToObject(o, "status", "open");
    if (kind && strcmp(kind, "direct") == 0) {
        cJSON_AddStringToObject(o, "kind", "direct");
        const char* a = p1; const char* b = p2;
        if (strcmp(a, b) > 0) { const char* t = a; a = b; b = t; }
        cJSON* arr = cJSON_AddArrayToObject(o, "participants");
        cJSON_AddItemToArray(arr, cJSON_CreateString(a));
        cJSON_AddItemToArray(arr, cJSON_CreateString(b));
    }
    char* s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char* out = plat_strdup(s ? s : "{}");
    free(s);
    return out;
}

// ---------------------------------------------------------------------------------------
// responses
// ---------------------------------------------------------------------------------------
static void jstr(cJSON* o, const char* k, char* dst, size_t n) {
    cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(dst, n, "%s", v->valuestring); else dst[0] = 0;
}
bool chron_parse_user_info(const char* json, chron_user_info_t* out) {
    memset(out, 0, sizeof(*out));
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsObject(data)) { cJSON_Delete(root); return false; }
    jstr(data, "user", out->user, sizeof(out->user));
    jstr(data, "account_name", out->account, sizeof(out->account));
    cJSON* perms = cJSON_GetObjectItemCaseSensitive(data, "permissions");
    cJSON* pub = perms ? cJSON_GetObjectItemCaseSensitive(perms, "publish") : NULL;
    cJSON* allow = pub ? cJSON_GetObjectItemCaseSensitive(pub, "allow") : NULL;
    cJSON* s;
    cJSON_ArrayForEach(s, allow) {
        if (!cJSON_IsString(s)) continue;
        const char* v = s->valuestring;
        if (strncmp(v, "CHRON.CTRL.IDENTITY.", 20) != 0) continue;
        const char* last = strrchr(v, '.');
        if (!last || last == v) continue;
        const char* prev = last - 1;
        while (prev > v && *prev != '.') prev--;
        if (*prev != '.') continue;
        int64_t id = strtoll(prev + 1, NULL, 10);
        if (id <= 0) continue;
        out->github_id = id;
        snprintf(out->login, sizeof(out->login), "%s", last + 1);
        break;
    }
    cJSON_Delete(root);
    return out->user[0] != 0;
}
int chron_parse_memberships(const char* json, chron_membership_t* out, int max) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return -1;
    cJSON* arr = cJSON_GetObjectItemCaseSensitive(root, "memberships");
    int n = 0; cJSON* m;
    cJSON_ArrayForEach(m, arr) {
        if (!cJSON_IsObject(m) || n >= max) continue;
        jstr(m, "account", out[n].account, sizeof(out[n].account));
        jstr(m, "principal", out[n].principal, sizeof(out[n].principal));
        jstr(m, "role", out[n].role, sizeof(out[n].role));
        if (out[n].account[0]) n++;
    }
    cJSON_Delete(root);
    return n;
}
static bool js_error(cJSON* root, char* err, size_t en) {
    cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (!cJSON_IsObject(e)) return false;
    cJSON* d = cJSON_GetObjectItemCaseSensitive(e, "description");
    cJSON* c = cJSON_GetObjectItemCaseSensitive(e, "code");
    snprintf(err, en, "%s (%d)", cJSON_IsString(d) ? d->valuestring : "jetstream error", cJSON_IsNumber(c) ? c->valueint : 0);
    return true;
}
bool chron_parse_consumer_create(const char* json, char* name, size_t n, int64_t* num_pending, char* err, size_t en) {
    name[0] = 0; err[0] = 0; if (num_pending) *num_pending = -1;
    cJSON* root = cJSON_Parse(json);
    if (!root) { snprintf(err, en, "bad JSON"); return false; }
    if (js_error(root, err, en)) { cJSON_Delete(root); return false; }
    jstr(root, "name", name, n);
    cJSON* np = cJSON_GetObjectItemCaseSensitive(root, "num_pending");
    if (num_pending && cJSON_IsNumber(np)) *num_pending = (int64_t)np->valuedouble;
    cJSON_Delete(root);
    if (!name[0]) snprintf(err, en, "no consumer name in response");
    return name[0] != 0;
}
bool chron_parse_apply_response(const char* json, uint64_t* seq) {
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    cJSON* s = cJSON_GetObjectItemCaseSensitive(root, "seq");
    bool ok = cJSON_IsNumber(s);
    if (ok && seq) *seq = (uint64_t)s->valuedouble;
    cJSON_Delete(root);
    return ok;
}
bool chron_parse_live(const char* json, chron_live_t* out) {
    memset(out, 0, sizeof(*out));
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    jstr(root, "agent", out->agent, sizeof(out->agent));
    jstr(root, "state", out->state, sizeof(out->state));
    jstr(root, "trigger", out->trigger, sizeof(out->trigger));
    jstr(root, "reason", out->reason, sizeof(out->reason));
    cJSON* t = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(t) && t->valuestring[0]) out->text = plat_strdup(t->valuestring);
    cJSON_Delete(root);
    return out->agent[0] && out->state[0];
}
bool chron_parse_profile(const char* json, chron_profile_t* out) {
    memset(out, 0, sizeof(*out));
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    jstr(root, "url", out->url, sizeof(out->url));
    jstr(root, "github_client_id", out->github_client_id, sizeof(out->github_client_id));
    jstr(root, "github_token_url", out->github_token_url, sizeof(out->github_token_url));
    cJSON* s = cJSON_GetObjectItemCaseSensitive(root, "sentinel");
    if (cJSON_IsString(s) && s->valuestring[0]) out->sentinel = plat_strdup(s->valuestring);
    cJSON_Delete(root);
    return out->url[0] && out->github_client_id[0] && out->sentinel;
}
bool gh_parse_device_code(const char* json, gh_device_t* out) {
    memset(out, 0, sizeof(*out));
    cJSON* root = cJSON_Parse(json);
    if (!root) return false;
    jstr(root, "device_code", out->device_code, sizeof(out->device_code));
    jstr(root, "user_code", out->user_code, sizeof(out->user_code));
    jstr(root, "verification_uri", out->verification_uri, sizeof(out->verification_uri));
    cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    cJSON* i = cJSON_GetObjectItemCaseSensitive(root, "interval");
    out->expires_in = cJSON_IsNumber(e) ? e->valueint : 900;
    out->interval = cJSON_IsNumber(i) ? i->valueint : 5;
    if (out->interval < 5) out->interval = 5;
    cJSON_Delete(root);
    return out->device_code[0] && out->user_code[0];
}
int gh_parse_token(const char* json, char* access, size_t an, char* refresh, size_t rn, int* expires_in, char* err, size_t en) {
    access[0] = 0; refresh[0] = 0; err[0] = 0; if (expires_in) *expires_in = 0;
    cJSON* root = cJSON_Parse(json);
    if (!root) { snprintf(err, en, "bad JSON from GitHub"); return -1; }
    cJSON* e = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(e)) {
        int r = -1;
        if (strcmp(e->valuestring, "authorization_pending") == 0 || strcmp(e->valuestring, "slow_down") == 0) r = 0;
        else {
            cJSON* d = cJSON_GetObjectItemCaseSensitive(root, "error_description");
            snprintf(err, en, "%s", cJSON_IsString(d) ? d->valuestring : e->valuestring);
        }
        cJSON_Delete(root);
        return r;
    }
    jstr(root, "access_token", access, an);
    jstr(root, "refresh_token", refresh, rn);
    cJSON* x = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    if (expires_in && cJSON_IsNumber(x)) *expires_in = x->valueint;
    cJSON_Delete(root);
    if (!access[0]) { snprintf(err, en, "no access token in reply"); return -1; }
    return 1;
}

// ---------------------------------------------------------------------------------------
// the fold
// ---------------------------------------------------------------------------------------
void chron_ws_init(chron_ws_t* ws) { memset(ws, 0, sizeof(*ws)); }
static void room_free(chron_room_t* r) {
    for (int i = 0; i < r->nmsgs; i++) plat_free(r->msgs[i].text);
    plat_free(r->msgs);
    r->msgs = NULL; r->nmsgs = 0;
}
void chron_ws_clear(chron_ws_t* ws) {
    for (int i = 0; i < ws->nrooms; i++) room_free(&ws->rooms[i]);
    memset(ws, 0, sizeof(*ws));
}
chron_room_t* chron_ws_room(chron_ws_t* ws, const char* id) {
    for (int i = 0; i < ws->nrooms; i++) if (strcmp(ws->rooms[i].id, id) == 0) return &ws->rooms[i];
    return NULL;
}
chron_agent_t* chron_ws_agent(chron_ws_t* ws, const char* name) {
    for (int i = 0; i < ws->nagents; i++) if (strcmp(ws->agents[i].name, name) == 0) return &ws->agents[i];
    return NULL;
}
static chron_room_t* room_get_or_add(chron_ws_t* ws, const char* id, uint64_t seq) {
    chron_room_t* r = chron_ws_room(ws, id);
    if (r) return r;
    if (ws->nrooms >= CHRON_MAX_ROOMS) {
        // evict the closed room with the oldest activity, else the oldest closed-or-not room
        int victim = -1;
        for (int i = 0; i < ws->nrooms; i++) if (ws->rooms[i].closed && (victim < 0 || ws->rooms[i].last_seq < ws->rooms[victim].last_seq)) victim = i;
        if (victim < 0) for (int i = 0; i < ws->nrooms; i++) if (victim < 0 || ws->rooms[i].last_seq < ws->rooms[victim].last_seq) victim = i;
        room_free(&ws->rooms[victim]);
        memset(&ws->rooms[victim], 0, sizeof(chron_room_t));
        r = &ws->rooms[victim];
    } else {
        r = &ws->rooms[ws->nrooms++];
    }
    snprintf(r->id, sizeof(r->id), "%s", id);
    snprintf(r->title, sizeof(r->title), "%s", id);
    r->created_seq = r->last_seq = seq;
    return r;
}
static chron_agent_t* agent_get_or_add(chron_ws_t* ws, const char* name) {
    chron_agent_t* a = chron_ws_agent(ws, name);
    if (a) return a;
    if (ws->nagents >= CHRON_MAX_AGENTS) return NULL;
    a = &ws->agents[ws->nagents++];
    memset(a, 0, sizeof(*a));
    snprintf(a->name, sizeof(a->name), "%s", name);
    return a;
}
static void apply_room_state(chron_room_t* r, cJSON* s) {
    if (!cJSON_IsObject(s)) return;
    cJSON* v;
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(s, "title")) && v->valuestring[0]) snprintf(r->title, sizeof(r->title), "%s", v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(s, "purpose"))) snprintf(r->purpose, sizeof(r->purpose), "%s", v->valuestring);
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(s, "status"))) r->closed = strcmp(v->valuestring, "closed") == 0;
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(s, "kind"))) r->direct = strcmp(v->valuestring, "direct") == 0;
    cJSON* parts = cJSON_GetObjectItemCaseSensitive(s, "participants");
    if (cJSON_IsArray(parts)) {
        int i = 0; cJSON* p;
        cJSON_ArrayForEach(p, parts) { if (cJSON_IsString(p) && i < 2) snprintf(r->participants[i++], CHRON_NAME_MAX, "%s", p->valuestring); }
    }
}
static void room_push_msg(chron_room_t* r, const chron_msg_t* m) {
    if (!r->msgs) {
        r->msgs = (chron_msg_t*)plat_calloc(CHRON_ROOM_MSG_CAP, sizeof(chron_msg_t));
        if (!r->msgs) return;
    }
    if (r->nmsgs >= CHRON_ROOM_MSG_CAP) {
        plat_free(r->msgs[0].text);
        memmove(&r->msgs[0], &r->msgs[1], sizeof(chron_msg_t) * (size_t)(CHRON_ROOM_MSG_CAP - 1));
        r->nmsgs = CHRON_ROOM_MSG_CAP - 1;
    }
    r->msgs[r->nmsgs++] = *m;
    r->total++;
}

chron_room_t* chron_fold_op(chron_ws_t* ws, const char* store, const char* subject, uint64_t seq,
                            const char* headers, int hlen, const char* payload, int plen, bool* new_message) {
    if (new_message) *new_message = false;
    char path[256];
    if (!chron_subject_to_path(store, subject, path, sizeof(path))) return NULL;
    if (seq > ws->head_seq) ws->head_seq = seq;
    char op_type[24] = "", author[CHRON_NAME_MAX] = "", ts[32] = "", op_id[CHRON_ID_MAX] = "";
    chron_header_get(headers, hlen, "Op-Type", op_type, sizeof(op_type));
    chron_header_get(headers, hlen, "Op-Author", author, sizeof(author));
    chron_header_get(headers, hlen, "Op-Ts", ts, sizeof(ts));
    chron_header_get(headers, hlen, "Nats-Msg-Id", op_id, sizeof(op_id));
    // tokens
    char* tok[6]; int nt = 0;
    char tmp[256]; snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = strtok(tmp, "/"); p && nt < 6; p = strtok(NULL, "/")) tok[nt++] = p;
    if (nt < 2) return NULL;
    cJSON* data = (plen > 0) ? cJSON_ParseWithLength(payload, (size_t)plen) : NULL;
    chron_room_t* room = NULL;
    if (strcmp(tok[0], "agent") == 0 && nt == 2) {
        chron_agent_t* a = agent_get_or_add(ws, tok[1]);
        if (a && cJSON_IsObject(data)) {
            cJSON* s = strcmp(op_type, "snapshot") == 0 ? cJSON_GetObjectItemCaseSensitive(data, "state") : data;
            cJSON* v;
            if (cJSON_IsObject(s)) {
                if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(s, "description"))) snprintf(a->description, sizeof(a->description), "%s", v->valuestring);
                if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(s, "harness"))) snprintf(a->harness, sizeof(a->harness), "%s", v->valuestring);
            }
        }
    } else if (strcmp(tok[0], "room") == 0 && nt == 2) {
        room = room_get_or_add(ws, tok[1], seq);
        if (strcmp(op_type, "close") == 0) room->closed = true;
        else if (strcmp(op_type, "snapshot") == 0) apply_room_state(room, cJSON_GetObjectItemCaseSensitive(data, "state"));
        else apply_room_state(room, data);  // create / rename
        if (seq > room->last_seq) room->last_seq = seq;
    } else if (strcmp(tok[0], "room") == 0 && nt == 4 && strcmp(tok[2], "messages") == 0 && strcmp(op_type, "create") == 0) {
        room = room_get_or_add(ws, tok[1], seq);
        chron_msg_t m; memset(&m, 0, sizeof(m));
        m.seq = seq;
        snprintf(m.id, sizeof(m.id), "%s", tok[3]);
        snprintf(m.author, sizeof(m.author), "%s", author);
        snprintf(m.ts, sizeof(m.ts), "%s", ts);
        snprintf(m.kind, sizeof(m.kind), "message");
        cJSON* v;
        if (cJSON_IsObject(data)) {
            if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(data, "kind")) && v->valuestring[0]) snprintf(m.kind, sizeof(m.kind), "%s", v->valuestring);
            if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(data, "reply_to"))) snprintf(m.reply_to, sizeof(m.reply_to), "%s", v->valuestring);
            v = cJSON_GetObjectItemCaseSensitive(data, "text");
            m.text = plat_strdup(cJSON_IsString(v) ? v->valuestring : "");
        } else {
            m.text = plat_strdup("");
        }
        room_push_msg(room, &m);
        if (seq > room->last_seq) room->last_seq = seq;
        snprintf(room->last_author, sizeof(room->last_author), "%s", author);
        if (new_message) *new_message = true;
    }
    cJSON_Delete(data);
    return room;
}

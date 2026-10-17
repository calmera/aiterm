#include "rooms_mode.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "net/chronicle.h"
#include "ui/view_list.h"
#include "util/platform.h"

static bool     s_active = false;
static char     s_room[CHRON_ID_MAX] = "";
static uint64_t s_rendered_seq = 0;
void update_status_line_public(void);  // main.c

typedef struct { chron_event_t ev; char room[CHRON_ID_MAX]; char* text; } ev_t;

// ---------------------------------------------------------------------------------------
// rendering
// ---------------------------------------------------------------------------------------
static void ts_local(const char* iso, char* out, size_t n) {
    int Y, M, D, h, m, s;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) { out[0] = 0; return; }
    struct tm tm = {.tm_year = Y - 1900, .tm_mon = M - 1, .tm_mday = D, .tm_hour = h, .tm_min = m, .tm_sec = s};
    time_t t = timegm(&tm);
    struct tm lt; localtime_r(&t, &lt);
    time_t now = time(NULL); struct tm ln; localtime_r(&now, &ln);
    if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year) strftime(out, n, "%H:%M", &lt);
    else strftime(out, n, "%d %b %H:%M", &lt);
}
static void render_message(const chron_msg_t* m) {
    bool mine = strcmp(m->author, chronicle_principal()) == 0;
    char when[24]; ts_local(m->ts, when, sizeof(when));
    if (strcmp(m->kind, "system") == 0) { tui_appendf(TUI_STYLE_SYSTEM, "— %s", m->text ? m->text : ""); return; }
    const char* who = mine ? "you" : m->author;
    if (strcmp(m->kind, "question") == 0) tui_appendf(TUI_STYLE_MUTED, "%s asks%s%s", who, when[0] ? " · " : "", when);
    else if (strcmp(m->kind, "turn") == 0) tui_appendf(TUI_STYLE_MUTED, "%s%s%s", who, when[0] ? " · " : "", when);
    else tui_appendf(mine ? TUI_STYLE_USER : TUI_STYLE_MUTED, "%s%s%s", who, when[0] ? " · " : "", when);
    tui_append_line(mine ? TUI_STYLE_USER : TUI_STYLE_NORMAL, m->text && m->text[0] ? m->text : "(empty)");
}
static void set_prompt_for_room(void) {
    char p[16];
    if (!s_room[0]) snprintf(p, sizeof(p), "rooms> ");
    else snprintf(p, sizeof(p), "#%.10s> ", s_room);
    tui_set_prompt(p);
}
static void render_room(void) {
    tui_clear();
    chronicle_lock();
    chron_room_t* r = chron_ws_room(chronicle_ws(), s_room);
    if (!r) {
        chronicle_unlock();
        tui_appendf(TUI_STYLE_SYSTEM, "#%s — new room, nothing in the record yet", s_room);
        s_rendered_seq = 0;
        set_prompt_for_room();
        return;
    }
    char head[200];
    if (r->direct) snprintf(head, sizeof(head), "%s%s%s", r->title, r->closed ? " · closed" : "", "");
    else snprintf(head, sizeof(head), "#%s — %s%s%s%s", r->id, r->title, r->purpose[0] ? " · " : "", r->purpose, r->closed ? " · closed" : "");
    tui_append_line(TUI_STYLE_SYSTEM, head);
    if (r->total > r->nmsgs) tui_appendf(TUI_STYLE_MUTED, "(%d older messages not kept on the badge)", r->total - r->nmsgs);
    for (int i = 0; i < r->nmsgs; i++) render_message(&r->msgs[i]);
    s_rendered_seq = r->nmsgs ? r->msgs[r->nmsgs - 1].seq : r->last_seq;
    chronicle_unlock();
    set_prompt_for_room();
    tui_scroll_to_bottom();
}
static void append_new_messages(void) {
    chronicle_lock();
    chron_room_t* r = chron_ws_room(chronicle_ws(), s_room);
    if (r) {
        for (int i = 0; i < r->nmsgs; i++) if (r->msgs[i].seq > s_rendered_seq) { render_message(&r->msgs[i]); s_rendered_seq = r->msgs[i].seq; }
    }
    chronicle_unlock();
}
static void open_room(const char* id) {
    snprintf(s_room, sizeof(s_room), "%s", id);
    chronicle_set_open_room(s_room);
    if (s_active) render_room();
    update_status_line_public();
}
static void pick_initial_room(void) {
    // the open room that last had a message, else general, else the first open room
    char best[CHRON_ID_MAX] = ""; uint64_t best_seq = 0; bool have_general = false; char first[CHRON_ID_MAX] = "";
    chronicle_lock();
    chron_ws_t* ws = chronicle_ws();
    for (int i = 0; i < ws->nrooms; i++) {
        chron_room_t* r = &ws->rooms[i];
        if (r->closed) continue;
        if (!first[0]) snprintf(first, sizeof(first), "%s", r->id);
        if (strcmp(r->id, "general") == 0) have_general = true;
        if (r->nmsgs && r->last_seq > best_seq) { best_seq = r->last_seq; snprintf(best, sizeof(best), "%s", r->id); }
    }
    chronicle_unlock();
    if (best[0]) open_room(best);
    else if (have_general) open_room("general");
    else if (first[0]) open_room(first);
    else { tui_append_line(TUI_STYLE_SYSTEM, "no rooms in this workspace yet — /new <title> makes one, /dm <agent> starts a conversation"); set_prompt_for_room(); }
}

// ---------------------------------------------------------------------------------------
// events (marshalled to the UI task)
// ---------------------------------------------------------------------------------------
static void handle_event(void* arg) {
    ev_t* e = (ev_t*)arg;
    switch (e->ev) {
        case CHRON_EV_STATE: {
            chron_state_t st = chronicle_state();
            if (s_active) {
                if (st == CHRON_ST_SYNCED) {
                    if (!s_room[0]) { tui_appendf(TUI_STYLE_SYSTEM, "connected as %s · Meta+L picks a room · /help for commands", chronicle_principal()); pick_initial_room(); }
                    else render_room();
                } else if (st == CHRON_ST_FAILED) tui_appendf(TUI_STYLE_ERROR, "[error] chronicle: %s", chronicle_last_error());
                else if (st == CHRON_ST_RECONNECTING) tui_append_line(TUI_STYLE_MUTED, "connection lost — reconnecting");
                else if (st == CHRON_ST_CONNECTING) tui_append_line(TUI_STYLE_MUTED, "connecting to chronicle…");
            }
            break;
        }
        case CHRON_EV_MESSAGE:
            if (s_active && strcmp(e->room, s_room) == 0) append_new_messages();
            break;
        case CHRON_EV_ROOMS: break;
        case CHRON_EV_LIVE: break;
        case CHRON_EV_REFUSED: if (s_active && strcmp(e->room, s_room) == 0) tui_appendf(TUI_STYLE_ERROR, "%s", e->text); break;
        case CHRON_EV_ROOM_OPENED: open_room(e->room); break;
        case CHRON_EV_ERROR: if (s_active) tui_appendf(TUI_STYLE_ERROR, "[error] %s", e->text); break;
        case CHRON_EV_INFO: tui_appendf(TUI_STYLE_SYSTEM, "%s", e->text); break;
    }
    update_status_line_public();
    plat_free(e->text);
    plat_free(e);
}
static void event_cb(chron_event_t ev, const char* room_id, const char* text, void* user) {
    (void)user;
    ev_t* e = (ev_t*)plat_calloc(1, sizeof(ev_t));
    if (!e) return;
    e->ev = ev;
    snprintf(e->room, sizeof(e->room), "%s", room_id ? room_id : "");
    e->text = plat_strdup(text ? text : "");
    if (!ui_post(handle_event, e)) { plat_free(e->text); plat_free(e); }
}

// ---------------------------------------------------------------------------------------
// picker
// ---------------------------------------------------------------------------------------
typedef struct { char id[CHRON_ID_MAX]; bool agent; bool new_room; } pick_t;
static pick_t s_picks[CHRON_MAX_ROOMS + CHRON_MAX_AGENTS + 1];
static int    s_npicks = 0;
static void picked(int index, void* user) {
    (void)user;
    if (index < 0 || index >= s_npicks) return;
    pick_t* p = &s_picks[index];
    if (p->new_room) { tui_input_set_text("/new "); return; }
    if (p->agent) chronicle_open_direct_async(p->id);
    else open_room(p->id);
}
void rooms_mode_picker(void) {
    const char* rows[CHRON_MAX_ROOMS + CHRON_MAX_AGENTS + 1];
    char bufs[CHRON_MAX_ROOMS + CHRON_MAX_AGENTS + 1][120];
    int n = 0, initial = 0;
    int64_t now = plat_now_ms();
    chronicle_lock();
    chron_ws_t* ws = chronicle_ws();
    for (int i = 0; i < ws->nagents && n < (int)(sizeof(rows) / sizeof(rows[0])) - 1; i++) {
        chron_agent_t* a = &ws->agents[i];
        bool online = a->last_seen_ms && now - a->last_seen_ms < 30000;
        snprintf(bufs[n], sizeof(bufs[n]), "@%-14s %s%s%s", a->name, online ? "● " : "○ ", a->description[0] ? a->description : "(agent)", online ? "" : " · offline");
        snprintf(s_picks[n].id, sizeof(s_picks[n].id), "%s", a->name); s_picks[n].agent = true; s_picks[n].new_room = false;
        rows[n] = bufs[n]; n++;
    }
    for (int pass = 0; pass < 2; pass++) {  // open rooms first, then closed
        for (int i = 0; i < ws->nrooms && n < (int)(sizeof(rows) / sizeof(rows[0])) - 1; i++) {
            chron_room_t* r = &ws->rooms[i];
            if ((pass == 0) == r->closed) continue;
            if (r->direct) snprintf(bufs[n], sizeof(bufs[n]), "%-15s %s%s", "conversation", r->title, r->closed ? " · closed" : "");
            else snprintf(bufs[n], sizeof(bufs[n]), "#%-14s %s%s%s%s", r->id, r->title, r->total ? " · " : "", r->total ? "" : "", r->closed ? " · closed" : "");
            if (r->total) { size_t l = strlen(bufs[n]); snprintf(bufs[n] + l, sizeof(bufs[n]) - l, "%d msg%s", r->total, r->total == 1 ? "" : "s"); }
            snprintf(s_picks[n].id, sizeof(s_picks[n].id), "%s", r->id); s_picks[n].agent = false; s_picks[n].new_room = false;
            if (strcmp(r->id, s_room) == 0) initial = n;
            rows[n] = bufs[n]; n++;
        }
    }
    chronicle_unlock();
    snprintf(bufs[n], sizeof(bufs[n]), "+ new room…");
    s_picks[n].new_room = true; s_picks[n].agent = false; s_picks[n].id[0] = 0;
    rows[n] = bufs[n]; n++;
    s_npicks = n;
    view_list_show("Rooms and agents", "↑↓ move · Enter open · Esc back", rows, n, initial, picked, NULL);
}

// ---------------------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------------------
static void help(void) {
    tui_append_line(TUI_STYLE_SYSTEM, "rooms: type to post in the room (@name mentions an agent; Tab completes names)");
    tui_append_line(TUI_STYLE_SYSTEM, "  /rooms (Meta+L) pick a room or agent · /dm <agent> conversation · /new <title> room");
    tui_append_line(TUI_STYLE_SYSTEM, "  /agents who exists · /close close this room · /reconnect · /help");
}
void rooms_mode_submit(const char* text) {
    while (*text == ' ') text++;
    if (!*text) return;
    if (text[0] == '/') {
        if (strncmp(text, "/rooms", 6) == 0 || strcmp(text, "/r") == 0) { rooms_mode_picker(); return; }
        if (strcmp(text, "/help") == 0 || strcmp(text, "/?") == 0) { help(); return; }
        if (strncmp(text, "/new ", 5) == 0) { const char* t = text + 5; while (*t == ' ') t++; if (*t) chronicle_create_room_async(t, ""); else tui_append_line(TUI_STYLE_ERROR, "usage: /new <title>"); return; }
        if (strncmp(text, "/dm ", 4) == 0) { const char* t = text + 4; while (*t == ' ' || *t == '@') t++; if (*t) chronicle_open_direct_async(t); else tui_append_line(TUI_STYLE_ERROR, "usage: /dm <agent>"); return; }
        if (strcmp(text, "/close") == 0) { if (s_room[0]) chronicle_close_room_async(s_room); return; }
        if (strcmp(text, "/reconnect") == 0) { s_room[0] = 0; chronicle_connect_async(); return; }
        if (strcmp(text, "/agents") == 0) {
            int64_t now = plat_now_ms();
            chronicle_lock();
            chron_ws_t* ws = chronicle_ws();
            if (ws->nagents == 0) tui_append_line(TUI_STYLE_MUTED, "no agent has announced itself in this workspace");
            for (int i = 0; i < ws->nagents; i++) {
                chron_agent_t* a = &ws->agents[i];
                bool online = a->last_seen_ms && now - a->last_seen_ms < 30000;
                tui_appendf(TUI_STYLE_MUTED, "@%s · %s · %s%s%s", a->name, online ? "online" : "offline", a->description[0] ? a->description : "(no description)", a->harness[0] ? " · " : "", a->harness);
            }
            chronicle_unlock();
            return;
        }
        tui_appendf(TUI_STYLE_ERROR, "unknown command %s — /help", text);
        return;
    }
    if (chronicle_state() != CHRON_ST_SYNCED && chronicle_state() != CHRON_ST_SYNCING) { tui_appendf(TUI_STYLE_ERROR, "[error] not connected (%s)", chronicle_state_text()); return; }
    if (!s_room[0]) { tui_append_line(TUI_STYLE_ERROR, "no room open — Meta+L picks one"); return; }
    chronicle_send_async(s_room, text);
}
bool rooms_mode_key(const ui_key_t* k) {
    bool meta = (k->mods & UI_MOD_META) != 0;
    if (meta && (k->ascii == 'l' || k->ascii == 'L')) { rooms_mode_picker(); return true; }
    if (k->is_nav && k->nav == UI_NAV_TAB) {
        const char* in = tui_input_text();
        const char* at = strrchr(in, '@');
        if (!at) return true;
        const char* prefix = at + 1;
        for (const char* p = prefix; *p; p++) if (*p == ' ') return true;
        char best[CHRON_NAME_MAX] = "";
        chronicle_lock();
        chron_ws_t* ws = chronicle_ws();
        for (int i = 0; i < ws->nagents; i++) if (strncmp(ws->agents[i].name, prefix, strlen(prefix)) == 0) { snprintf(best, sizeof(best), "%s", ws->agents[i].name); break; }
        chronicle_unlock();
        if (best[0]) {
            char out[512]; snprintf(out, sizeof(out), "%.*s@%s ", (int)(at - in), in, best);
            tui_input_set_text(out);
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// mode lifecycle / status
// ---------------------------------------------------------------------------------------
void rooms_mode_init(void) { chronicle_init(); chronicle_set_event_cb(event_cb, NULL); }
void rooms_mode_enter(void) {
    s_active = true;
    tui_clear();
    if (!app_config_has_chronicle()) {
        tui_append_line(TUI_STYLE_SYSTEM, "Rooms: not signed in. Open the setup page on your phone (F1 → Set up from phone) and press");
        tui_append_line(TUI_STYLE_SYSTEM, "\"Sign in with GitHub\" under Chronicle rooms.");
        set_prompt_for_room();
        return;
    }
    chron_state_t st = chronicle_state();
    if (st == CHRON_ST_SYNCED) { if (s_room[0]) render_room(); else pick_initial_room(); }
    else {
        tui_appendf(TUI_STYLE_SYSTEM, "Rooms · %s", chronicle_state_text());
        set_prompt_for_room();
        if (st == CHRON_ST_IDLE || st == CHRON_ST_FAILED) chronicle_connect_async();
    }
}
void rooms_mode_leave(void) { s_active = false; }
void rooms_mode_on_network_up(void) {
    chron_state_t st = chronicle_state();
    if (app_config()->ui.default_mode == CFG_MODE_ROOMS && app_config_has_chronicle() && (st == CHRON_ST_IDLE || st == CHRON_ST_FAILED)) chronicle_connect_async();
}
void rooms_mode_status(tui_status_t* st, char* model, size_t mn, char* amount, size_t an, char* extra, size_t en) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    st->mode = "ROOMS"; st->mode_kind = 2; st->gauge_pct = -1; st->tools = -1;
    if (!app_config_has_chronicle()) { snprintf(model, mn, "not signed in"); amount[0] = 0; extra[0] = 0; }
    else {
        if (s_room[0]) snprintf(model, mn, "%s · #%s", ch->account, s_room);
        else snprintf(model, mn, "%s", ch->account);
        const char* live = chronicle_live_text();
        chron_state_t cs = chronicle_state();
        if (cs == CHRON_ST_SYNCED && live[0]) snprintf(amount, an, "%s", live);
        else if (cs == CHRON_ST_SYNCED) amount[0] = 0;
        else snprintf(amount, an, "%s", chronicle_state_text());
        int online = chronicle_agents_online();
        snprintf(extra, en, "%d online", online);
    }
    st->model = model; st->amount = amount; st->extra = extra;
}

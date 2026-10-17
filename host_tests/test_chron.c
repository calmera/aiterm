#include "test_util.h"
#include "net/chron_proto.h"
#include "net/nkeys.h"
#include "util/platform.h"

static uint32_t fixed_rnd(void) { return 7; }

int main(void) {
    // ---- base32 / crc / seeds
    uint8_t raw[36]; char b32[80]; uint8_t dec[64];
    for (int i = 0; i < 32; i++) raw[2 + i] = (uint8_t)(i * 7 + 1);
    raw[0] = (uint8_t)(NKEYS_PREFIX_SEED | (NKEYS_PREFIX_USER >> 5)); raw[1] = (uint8_t)((NKEYS_PREFIX_USER & 31) << 3);
    uint16_t crc = nkeys_crc16(raw, 34); raw[34] = (uint8_t)(crc & 0xFF); raw[35] = (uint8_t)(crc >> 8);
    CHECK_INT(nkeys_crc16((const uint8_t*)"123456789", 9), 0x31C3);  // CRC-16/XMODEM check value
    CHECK_INT(nkeys_base32_encode(raw, 36, b32, sizeof(b32)), 58);
    CHECK(b32[0] == 'S' && b32[1] == 'U');
    CHECK_INT(nkeys_base32_decode(b32, dec, sizeof(dec)), 36);
    CHECK(memcmp(dec, raw, 36) == 0);
    uint8_t seed32[32], prefix = 0;
    CHECK(nkeys_seed_decode(b32, seed32, &prefix));
    CHECK_INT(prefix, NKEYS_PREFIX_USER);
    CHECK(memcmp(seed32, raw + 2, 32) == 0);
    b32[10] = b32[10] == 'A' ? 'B' : 'A';  // corrupt -> crc fails
    CHECK(!nkeys_seed_decode(b32, seed32, &prefix));
    char pub[64];
    CHECK(nkeys_public_encode(NKEYS_PREFIX_USER, raw + 2, pub, sizeof(pub)));
    CHECK_INT((int)strlen(pub), 56); CHECK(pub[0] == 'U');
    CHECK(nkeys_base32_decode("abc!", dec, sizeof(dec)) < 0);

    // ---- paths / subjects / ids
    char buf[256];
    CHECK(chron_path_to_tail("room/general/messages/m-1", buf, sizeof(buf))); CHECK_STR(buf, "room.general.messages.m-1");
    CHECK(chron_subject_to_path("rooms", "CHRON.rooms.OPS.room.general.messages.m-1", buf, sizeof(buf))); CHECK_STR(buf, "room/general/messages/m-1");
    CHECK(!chron_subject_to_path("rooms", "CHRON.other.OPS.room.x", buf, sizeof(buf)));
    chron_ops_subject("rooms", "room/general", buf, sizeof(buf)); CHECK_STR(buf, "CHRON.rooms.OPS.room.general");
    chron_apply_subject("daan", "rooms", "room/general/messages/m-1", buf, sizeof(buf)); CHECK_STR(buf, "CHRON.API.daan.APPLY.rooms.room.general.messages.m-1");
    chron_room_id_from_title("  Week 41 Close! ", buf, sizeof(buf), fixed_rnd); CHECK_STR(buf, "week-41-close");
    chron_room_id_from_title("???", buf, sizeof(buf), fixed_rnd); CHECK_STR(buf, "r-777777777777");
    chron_direct_id("wilson", "daan", buf, sizeof(buf)); CHECK_STR(buf, "direct--daan--wilson");
    chron_mint_id("m-", buf, sizeof(buf), fixed_rnd); CHECK_STR(buf, "m-777777777777");
    chron_nuid(buf, sizeof(buf), fixed_rnd); CHECK_INT((int)strlen(buf), 22);
    char host[64]; int port; bool tls;
    CHECK(chron_parse_nats_url("tls://connect.chronicle.impire.dev:4222", host, sizeof(host), &port, &tls)); CHECK_STR(host, "connect.chronicle.impire.dev"); CHECK_INT(port, 4222); CHECK(tls);
    CHECK(chron_parse_nats_url("nats://localhost", host, sizeof(host), &port, &tls)); CHECK_STR(host, "localhost"); CHECK_INT(port, 4222); CHECK(!tls);
    CHECK(!chron_parse_nats_url("wss://x:1", host, sizeof(host), &port, &tls));
    CHECK_INT((long)chron_ack_stream_seq("$JS.ACK.LOG_ROOMS.c1.1.2039.12.1700000000.0"), 2039);
    CHECK_INT((long)chron_ack_stream_seq("$JS.ACK._.ACCHASH.LOG_ROOMS.c1.1.77.12.1700000000.0.tok"), 77);
    CHECK_INT((long)chron_ack_stream_seq("_INBOX.x"), 0);
    CHECK_INT((long)chron_ack_pending("$JS.ACK.LOG_ROOMS.c1.1.2039.12.1700000000.3"), 3);
    CHECK_INT((long)chron_ack_pending("$JS.ACK._.ACCHASH.LOG_ROOMS.c1.1.77.12.1700000000.0.tok"), 0);
    CHECK_INT((long)chron_ack_pending("nope"), -1);

    // ---- headers
    char* h = chron_op_headers("op1", "create", "daan", "2026-10-17T10:00:00Z", true);
    CHECK_STR(h, "NATS/1.0\r\nNats-Msg-Id: op1\r\nOp-Type: create\r\nOp-Author: daan\r\nOp-Ts: 2026-10-17T10:00:00Z\r\nOp-Version: 1\r\nNats-Expected-Last-Subject-Sequence: 0\r\n\r\n");
    CHECK(chron_header_get(h, (int)strlen(h), "op-author", buf, sizeof(buf))); CHECK_STR(buf, "daan");
    CHECK(chron_header_get(h, (int)strlen(h), "Nats-Msg-Id", buf, sizeof(buf))); CHECK_STR(buf, "op1");
    CHECK(!chron_header_get(h, (int)strlen(h), "Op-Parents", buf, sizeof(buf)));
    CHECK_INT(chron_header_status(h, (int)strlen(h)), 0);
    plat_free(h);
    h = chron_op_headers("op2", "create", "daan", NULL, false);
    CHECK(strstr(h, "Op-Ts") == NULL && strstr(h, "Expected") == NULL);
    plat_free(h);
    const char* st = "NATS/1.0 408 Request Timeout\r\nNats-Pending-Messages: 1\r\n\r\n";
    CHECK_INT(chron_header_status(st, (int)strlen(st)), 408);
    CHECK(chron_header_get(st, (int)strlen(st), "Nats-Pending-Messages", buf, sizeof(buf))); CHECK_STR(buf, "1");
    const char* se = "NATS/1.0\r\nNats-Service-Error-Code: instance-exists\r\nNats-Service-Error: room/x in rooms\r\n\r\n";
    CHECK(chron_header_get(se, (int)strlen(se), "Nats-Service-Error-Code", buf, sizeof(buf))); CHECK_STR(buf, "instance-exists");
    const char* creds = "-----BEGIN NATS USER JWT-----\neyJ0eXAi.abc.def\n------END NATS USER JWT------\n\n************************* IMPORTANT *************************\nNKEY Seed printed below can be used to sign and prove identity.\n\n-----BEGIN USER NKEY SEED-----\nSUAEXAMPLESEED\n------END USER NKEY SEED------\n\n*************************************************************\n";
    char jwt[128], seed[64];
    CHECK(chron_parse_creds(creds, jwt, sizeof(jwt), seed, sizeof(seed))); CHECK_STR(jwt, "eyJ0eXAi.abc.def"); CHECK_STR(seed, "SUAEXAMPLESEED");
    CHECK(!chron_parse_creds("nothing here", jwt, sizeof(jwt), seed, sizeof(seed)));

    // ---- payloads
    char names[CHRON_MAX_MENTIONS][CHRON_NAME_MAX];
    int n = chron_mentions("@wilson has invoice 345 been paid? ask @bob-2 or me@example.com, @wilson again", names, CHRON_MAX_MENTIONS);
    CHECK_INT(n, 2); CHECK_STR(names[0], "wilson"); CHECK_STR(names[1], "bob-2");
    CHECK_INT(chron_mentions("no mentions @ here", names, CHRON_MAX_MENTIONS), 0);
    char* j = chron_message_json("hi @wilson", "message", names, 1, NULL);
    CHECK_STR(j, "{\"text\":\"hi @wilson\",\"kind\":\"message\",\"mentions\":[\"wilson\"]}");
    plat_free(j);
    j = chron_message_json("yes", NULL, names, 0, "room/x/messages/q1");
    CHECK(strstr(j, "\"reply_to\":\"room/x/messages/q1\"") && strstr(j, "\"mentions\":[]"));
    plat_free(j);
    j = chron_room_json("daan & wilson", "a direct conversation", "direct", "wilson", "daan");
    CHECK_STR(j, "{\"title\":\"daan & wilson\",\"purpose\":\"a direct conversation\",\"status\":\"open\",\"kind\":\"direct\",\"participants\":[\"daan\",\"wilson\"]}");
    plat_free(j);
    j = chron_room_json("Week 41", "", "room", NULL, NULL);
    CHECK_STR(j, "{\"title\":\"Week 41\",\"status\":\"open\"}");
    plat_free(j);

    // ---- responses
    chron_user_info_t ui;
    CHECK(chron_parse_user_info("{\"server\":{},\"data\":{\"user\":\"daan\",\"account\":\"ACC\",\"account_name\":\"CONTROL\",\"permissions\":{\"publish\":{\"allow\":[\"_INBOX.>\",\"CHRON.CTRL.IDENTITY.MEMBERSHIPS.12345.daan\",\"CHRON.CTRL.IDENTITY.ACCOUNT.CREATE.12345.daan\"]}}}}", &ui));
    CHECK_STR(ui.user, "daan"); CHECK_STR(ui.account, "CONTROL"); CHECK_INT((long)ui.github_id, 12345); CHECK_STR(ui.login, "daan");
    CHECK(chron_parse_user_info("{\"data\":{\"user\":\"daan\",\"account_name\":\"daan\"}}", &ui)); CHECK_INT((long)ui.github_id, 0);
    CHECK(!chron_parse_user_info("{\"data\":{}}", &ui));
    chron_membership_t mem[4];
    n = chron_parse_memberships("{\"memberships\":[{\"account\":\"daan\",\"principal\":\"daan\",\"role\":\"admin\",\"created\":true},{\"account\":\"synthetic\",\"principal\":\"daan\",\"role\":\"writer\"}],\"accounts_created\":1,\"plan\":\"beta\"}", mem, 4);
    CHECK_INT(n, 2); CHECK_STR(mem[0].account, "daan"); CHECK_STR(mem[1].role, "writer");
    CHECK_INT(chron_parse_memberships("nope", mem, 4), -1);
    char cname[64], err[128];
    int64_t np = 0;
    CHECK(chron_parse_consumer_create("{\"type\":\"io.nats.jetstream.api.v1.consumer_create_response\",\"stream_name\":\"LOG_ROOMS\",\"name\":\"abc123\",\"config\":{},\"num_pending\":42}", cname, sizeof(cname), &np, err, sizeof(err))); CHECK_STR(cname, "abc123"); CHECK_INT((long)np, 42);
    CHECK(!chron_parse_consumer_create("{\"type\":\"io.nats.jetstream.api.v1.consumer_create_response\",\"error\":{\"code\":404,\"err_code\":10059,\"description\":\"stream not found\"}}", cname, sizeof(cname), &np, err, sizeof(err))); CHECK_STR(err, "stream not found (404)");
    uint64_t seq = 0;
    CHECK(chron_parse_apply_response("{\"seq\":2041}", &seq)); CHECK_INT((long)seq, 2041);
    CHECK(!chron_parse_apply_response("{}", &seq));
    chron_live_t live;
    CHECK(chron_parse_live("{\"agent\":\"wilson\",\"state\":\"writing\",\"trigger\":\"room/general/messages/m-1\",\"text\":\"Checking inv\",\"at\":\"2026-10-17T10:00:00Z\"}", &live));
    CHECK_STR(live.agent, "wilson"); CHECK_STR(live.state, "writing"); CHECK_STR(live.text, "Checking inv"); plat_free(live.text);
    CHECK(chron_parse_live("{\"agent\":\"wilson\",\"state\":\"refused\",\"reason\":\"3 turns in a minute\",\"at\":\"x\"}", &live)); CHECK_STR(live.reason, "3 turns in a minute"); CHECK(live.text == NULL);
    chron_profile_t prof;
    CHECK(chron_parse_profile("{\"url\":\"tls://connect.chronicle.impire.dev:4222\",\"github_client_id\":\"Iv23li\",\"sentinel\":\"-----BEGIN NATS USER JWT-----\\nx\\n\",\"websocket_url\":\"wss://x\",\"github_token_url\":\"https://chronicle.impire.dev/auth/github/token\"}", &prof));
    CHECK_STR(prof.url, "tls://connect.chronicle.impire.dev:4222"); CHECK_STR(prof.github_client_id, "Iv23li"); CHECK(prof.sentinel && prof.sentinel[0] == '-'); plat_free(prof.sentinel);
    CHECK(!chron_parse_profile("{\"url\":\"tls://x\"}", &prof));
    gh_device_t dev;
    CHECK(gh_parse_device_code("{\"device_code\":\"3584d83530557fdd1f46af8289938c8ef79f9dc5\",\"user_code\":\"WDJB-MJHT\",\"verification_uri\":\"https://github.com/login/device\",\"expires_in\":900,\"interval\":5}", &dev));
    CHECK_STR(dev.user_code, "WDJB-MJHT"); CHECK_INT(dev.interval, 5); CHECK_INT(dev.expires_in, 900);
    char acc[160], ref[160]; int exp = 0;
    CHECK_INT(gh_parse_token("{\"error\":\"authorization_pending\",\"error_description\":\"The authorization request is still pending.\"}", acc, sizeof(acc), ref, sizeof(ref), &exp, err, sizeof(err)), 0);
    CHECK_INT(gh_parse_token("{\"error\":\"slow_down\",\"interval\":10}", acc, sizeof(acc), ref, sizeof(ref), &exp, err, sizeof(err)), 0);
    CHECK_INT(gh_parse_token("{\"error\":\"access_denied\",\"error_description\":\"The user denied the request.\"}", acc, sizeof(acc), ref, sizeof(ref), &exp, err, sizeof(err)), -1); CHECK_STR(err, "The user denied the request.");
    CHECK_INT(gh_parse_token("{\"access_token\":\"ghu_abc\",\"expires_in\":28800,\"refresh_token\":\"ghr_def\",\"refresh_token_expires_in\":15811200,\"token_type\":\"bearer\",\"scope\":\"\"}", acc, sizeof(acc), ref, sizeof(ref), &exp, err, sizeof(err)), 1);
    CHECK_STR(acc, "ghu_abc"); CHECK_STR(ref, "ghr_def"); CHECK_INT(exp, 28800);

    // ---- the fold
    chron_ws_t* ws = (chron_ws_t*)calloc(1, sizeof(chron_ws_t));
    chron_ws_init(ws);
    bool nm = false;
    const char* h1 = "NATS/1.0\r\nNats-Msg-Id: a\r\nOp-Type: create\r\nOp-Author: daan\r\nOp-Ts: 2026-10-17T09:00:00Z\r\nOp-Version: 1\r\n\r\n";
    const char* p1 = "{\"title\":\"General\",\"purpose\":\"where a workspace starts\",\"status\":\"open\"}";
    chron_room_t* r = chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.room.general", 10, h1, (int)strlen(h1), p1, (int)strlen(p1), &nm);
    CHECK(r != NULL && !nm); CHECK_STR(r->title, "General"); CHECK(!r->closed); CHECK_INT(ws->nrooms, 1);
    const char* p2 = "{\"text\":\"@wilson has invoice 345 been paid?\",\"kind\":\"message\",\"mentions\":[\"wilson\"]}";
    r = chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.room.general.messages.m-1", 11, h1, (int)strlen(h1), p2, (int)strlen(p2), &nm);
    CHECK(r != NULL && nm); CHECK_INT(r->nmsgs, 1); CHECK_STR(r->msgs[0].author, "daan"); CHECK_STR(r->msgs[0].text, "@wilson has invoice 345 been paid?"); CHECK_STR(r->msgs[0].kind, "message"); CHECK_INT((long)r->msgs[0].seq, 11);
    const char* h2 = "NATS/1.0\r\nNats-Msg-Id: b\r\nOp-Type: create\r\nOp-Author: wilson\r\nOp-Version: 1\r\n\r\n";
    const char* p3 = "{\"text\":\"Yes, on the 12th.\",\"kind\":\"turn\",\"reply_to\":\"room/general/messages/m-1\",\"mentions\":[\"daan\"]}";
    r = chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.room.general.messages.m-wil", 12, h2, (int)strlen(h2), p3, (int)strlen(p3), &nm);
    CHECK(nm); CHECK_INT(r->nmsgs, 2); CHECK_STR(r->msgs[1].kind, "turn"); CHECK_STR(r->msgs[1].reply_to, "room/general/messages/m-1"); CHECK_STR(r->last_author, "wilson"); CHECK_INT((long)r->last_seq, 12);
    // a message for a room whose create we have not seen yet still makes the room
    r = chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.room.close-w41.messages.m-2", 13, h1, (int)strlen(h1), p2, (int)strlen(p2), &nm);
    CHECK(r && nm); CHECK_STR(r->id, "close-w41"); CHECK_STR(r->title, "close-w41"); CHECK_INT(ws->nrooms, 2);
    const char* h3 = "NATS/1.0\r\nNats-Msg-Id: c\r\nOp-Type: close\r\nOp-Author: daan\r\nOp-Version: 1\r\n\r\n";
    r = chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.room.close-w41", 14, h3, (int)strlen(h3), "{}", 2, &nm);
    CHECK(r && r->closed && !nm);
    const char* h4 = "NATS/1.0\r\nNats-Msg-Id: d\r\nOp-Type: snapshot\r\nOp-Author: node\r\nOp-Version: 1\r\n\r\n";
    const char* p4 = "{\"state\":{\"title\":\"Week 41 close\",\"status\":\"closed\"},\"frontier\":[]}";
    r = chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.room.close-w41", 15, h4, (int)strlen(h4), p4, (int)strlen(p4), &nm);
    CHECK_STR(r->title, "Week 41 close");
    const char* p5 = "{\"name\":\"wilson\",\"description\":\"answers invoice questions\",\"harness\":\"claude-code\"}";
    r = chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.agent.wilson", 16, h2, (int)strlen(h2), p5, (int)strlen(p5), &nm);
    CHECK(r == NULL); CHECK_INT(ws->nagents, 1); CHECK_STR(ws->agents[0].description, "answers invoice questions");
    const char* p6 = "{\"title\":\"daan & wilson\",\"status\":\"open\",\"kind\":\"direct\",\"participants\":[\"daan\",\"wilson\"]}";
    r = chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.room.direct--daan--wilson", 17, h1, (int)strlen(h1), p6, (int)strlen(p6), &nm);
    CHECK(r && r->direct); CHECK_STR(r->participants[1], "wilson");
    CHECK(chron_fold_op(ws, "rooms", "CHRON.rooms.OPS.solution.chronicle-agents", 18, h1, (int)strlen(h1), "{}", 2, &nm) == NULL);
    CHECK(chron_fold_op(ws, "other", "CHRON.rooms.OPS.room.general", 19, h1, (int)strlen(h1), p1, (int)strlen(p1), &nm) == NULL);
    CHECK_INT((long)ws->head_seq, 18);
    // the per-room cap keeps the newest messages
    for (int i = 0; i < CHRON_ROOM_MSG_CAP + 5; i++) {
        char subj[96]; snprintf(subj, sizeof(subj), "CHRON.rooms.OPS.room.general.messages.m-%d", i);
        chron_fold_op(ws, "rooms", subj, 100 + (uint64_t)i, h1, (int)strlen(h1), p2, (int)strlen(p2), &nm);
    }
    r = chron_ws_room(ws, "general");
    CHECK_INT(r->nmsgs, CHRON_ROOM_MSG_CAP); CHECK_INT(r->total, CHRON_ROOM_MSG_CAP + 7); CHECK_INT((long)r->msgs[r->nmsgs - 1].seq, 100 + CHRON_ROOM_MSG_CAP + 4);
    chron_ws_clear(ws);
    CHECK_INT(ws->nrooms, 0);
    free(ws);
    TEST_MAIN_END("test_chron");
}

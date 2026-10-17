#include "chron_login.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "cJSON.h"
#include "chronicle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "github_auth.h"
#include "net/wifi.h"
#include "util/platform.h"

static const char* TAG = "login";

static chron_login_status_t s_st;
static SemaphoreHandle_t    s_lock = NULL;
static gh_device_t          s_dev;
static int64_t              s_expires_ms = 0;
static volatile bool        s_cancel = false;
static volatile bool        s_running = false;
static bool                 s_refresh_profile = false;
static chron_membership_t   s_mem[8];
static int                  s_nmem = 0;

void app_config_changed(void);

static void lock(void) { if (!s_lock) s_lock = xSemaphoreCreateMutex(); xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static void set_state(chron_login_state_t st, const char* err) {
    lock();
    s_st.state = st;
    if (err) snprintf(s_st.error, sizeof(s_st.error), "%s", err); else if (st != CHRON_LOGIN_ERROR) s_st.error[0] = 0;
    unlock();
    chronicle_emit(CHRON_EV_LOGIN, NULL, chron_login_state_name(st));
}
const char* chron_login_state_name(chron_login_state_t s) {
    switch (s) {
        case CHRON_LOGIN_IDLE: return "idle";
        case CHRON_LOGIN_STARTING: return "starting";
        case CHRON_LOGIN_PENDING: return "pending";
        case CHRON_LOGIN_FINISHING: return "finishing";
        case CHRON_LOGIN_AUTHORIZED: return "authorized";
        case CHRON_LOGIN_ERROR: return "error";
    }
    return "?";
}
static void fail(const char* err) {
    ESP_LOGW(TAG, "sign-in failed: %s", err);
    set_state(CHRON_LOGIN_ERROR, err);
    char line[200]; snprintf(line, sizeof(line), "GitHub sign-in failed: %s", err);
    chronicle_emit_info(line);
}

static void login_task(void* arg) {
    (void)arg;
    cfg_chronicle_t* ch = &app_config()->chronicle;
    char err[160] = "";
    // 1. the install profile and a device code
    if (s_refresh_profile || !ch->nats_url[0] || !ch->github_client_id[0] || !ch->sentinel[0]) {
        if (chronicle_fetch_profile(err, sizeof(err)) != ESP_OK) { fail(err); s_running = false; vTaskDelete(NULL); return; }
    }
    if (s_cancel) { set_state(CHRON_LOGIN_IDLE, NULL); s_running = false; vTaskDelete(NULL); return; }
    if (github_device_start(ch->github_client_id, &s_dev, err, sizeof(err)) != ESP_OK) { fail(err); s_running = false; vTaskDelete(NULL); return; }
    s_expires_ms = plat_now_ms() + (int64_t)s_dev.expires_in * 1000;
    lock();
    snprintf(s_st.user_code, sizeof(s_st.user_code), "%s", s_dev.user_code);
    snprintf(s_st.verification_uri, sizeof(s_st.verification_uri), "%s", s_dev.verification_uri);
    snprintf(s_st.qr_url, sizeof(s_st.qr_url), "%s?user_code=%s", s_dev.verification_uri, s_dev.user_code);
    unlock();
    set_state(CHRON_LOGIN_PENDING, NULL);
    char line[200]; snprintf(line, sizeof(line), "GitHub sign-in: scan the QR on the badge, or open %s and enter %s", s_dev.verification_uri, s_dev.user_code);
    chronicle_emit_info(line);
    // 2. poll until approved
    char acc[CFG_TOKEN] = "", ref[CFG_TOKEN] = ""; int exp = 0; int r = 0;
    int interval = s_dev.interval > 0 ? s_dev.interval : 5;
    while (!s_cancel && plat_now_ms() < s_expires_ms) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)interval * 1000));
        if (s_cancel) break;
        r = github_device_poll(ch->github_client_id, s_dev.device_code, acc, sizeof(acc), ref, sizeof(ref), &exp, err, sizeof(err));
        if (r == 1) break;
        if (r < 0) {
            if (strstr(err, "HTTP") || strstr(err, "timeout") || strstr(err, "connect") || strstr(err, "failed")) { interval += 5; continue; }  // transport trouble: keep polling
            break;
        }
    }
    if (s_cancel) { set_state(CHRON_LOGIN_IDLE, NULL); s_running = false; vTaskDelete(NULL); return; }
    if (r != 1) { fail(r == 0 ? "the code expired before it was approved" : err); s_running = false; vTaskDelete(NULL); return; }
    // 3. keep the tokens, ask the identity plane
    strlcpy(ch->access_token, acc, sizeof(ch->access_token));
    strlcpy(ch->refresh_token, ref, sizeof(ch->refresh_token));
    time_t now = time(NULL);
    ch->token_expiry = (exp > 0 && now > 1700000000) ? (int64_t)now + exp : 0;
    app_config_save();
    set_state(CHRON_LOGIN_FINISHING, NULL);
    chronicle_emit_info("GitHub approved the badge — asking chronicle which accounts you hold…");
    chron_user_info_t who;
    s_nmem = 0;
    if (chronicle_identity(ch->access_token, &who, s_mem, 8, &s_nmem, err, sizeof(err)) != ESP_OK) { fail(err); s_running = false; vTaskDelete(NULL); return; }
    strlcpy(ch->login, who.login, sizeof(ch->login));
    bool keep = false;
    for (int i = 0; i < s_nmem; i++) if (strcmp(s_mem[i].account, ch->account) == 0) keep = true;
    if (!keep) strlcpy(ch->account, s_nmem >= 1 ? s_mem[0].account : "", sizeof(ch->account));
    ch->enabled = app_config_has_chronicle();
    app_config_save();
    lock();
    snprintf(s_st.login, sizeof(s_st.login), "%s", ch->login);
    snprintf(s_st.account, sizeof(s_st.account), "%s", ch->account);
    s_st.accounts = s_nmem;
    unlock();
    set_state(CHRON_LOGIN_AUTHORIZED, NULL);
    if (s_nmem == 0) snprintf(line, sizeof(line), "signed in as %s, but you hold no chronicle account yet — create one in the console", who.login);
    else if (s_nmem == 1) snprintf(line, sizeof(line), "signed in as %s · account %s · Meta+M switches to Rooms", who.login, ch->account);
    else snprintf(line, sizeof(line), "signed in as %s · %d accounts, using %s — pick another on the setup page", who.login, s_nmem, ch->account);
    chronicle_emit_info(line);
    ESP_LOGI(TAG, "%s", line);
    app_config_changed();
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t chron_login_start_async(bool refresh_profile, char* err, size_t err_len) {
    err[0] = 0;
    if (s_running) { snprintf(err, err_len, "a sign-in is already in progress"); return ESP_ERR_INVALID_STATE; }
    if (!wifi_sta_connected()) { snprintf(err, err_len, "connect the badge to Wi-Fi first — the sign-in talks to GitHub from the badge"); return ESP_ERR_INVALID_STATE; }
    lock();
    memset(&s_st, 0, sizeof(s_st));
    unlock();
    s_refresh_profile = refresh_profile;
    s_cancel = false; s_nmem = 0;
    s_running = true;
    set_state(CHRON_LOGIN_STARTING, NULL);
    if (xTaskCreatePinnedToCore(login_task, "ghlogin", 12288, NULL, 4, NULL, 1) != pdPASS) {
        s_running = false;
        set_state(CHRON_LOGIN_ERROR, "could not start the sign-in task");
        snprintf(err, err_len, "could not start the sign-in task");
        return ESP_FAIL;
    }
    return ESP_OK;
}
void chron_login_cancel(void) {
    s_cancel = true;
    if (!s_running) set_state(CHRON_LOGIN_IDLE, NULL);
}
void chron_login_get(chron_login_status_t* out) {
    lock();
    *out = s_st;
    unlock();
    if ((out->state == CHRON_LOGIN_PENDING || out->state == CHRON_LOGIN_STARTING) && s_cancel) out->state = CHRON_LOGIN_IDLE;
    int64_t left = (s_expires_ms - plat_now_ms()) / 1000;
    out->seconds_left = out->state == CHRON_LOGIN_PENDING && left > 0 ? (int)left : 0;
    cfg_chronicle_t* ch = &app_config()->chronicle;
    if (!out->login[0]) snprintf(out->login, sizeof(out->login), "%s", ch->login);
    if (!out->account[0]) snprintf(out->account, sizeof(out->account), "%s", ch->account);
}
char* chron_login_status_json(void) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    chron_login_status_t st; chron_login_get(&st);
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "state", chron_login_state_name(st.state));
    if (st.state == CHRON_LOGIN_PENDING) {
        cJSON_AddStringToObject(o, "user_code", st.user_code);
        cJSON_AddStringToObject(o, "verification_uri", st.verification_uri);
        cJSON_AddStringToObject(o, "qr_url", st.qr_url);
        cJSON_AddNumberToObject(o, "expires_in", st.seconds_left);
    }
    if (st.state == CHRON_LOGIN_ERROR) cJSON_AddStringToObject(o, "error", st.error);
    cJSON_AddBoolToObject(o, "signed_in", app_config_has_chronicle());
    cJSON_AddBoolToObject(o, "wifi", wifi_sta_connected());
    cJSON_AddStringToObject(o, "login", ch->login);
    cJSON_AddStringToObject(o, "account", ch->account);
    cJSON_AddStringToObject(o, "store", ch->store);
    cJSON_AddStringToObject(o, "nats_url", ch->nats_url);
    cJSON* arr = cJSON_AddArrayToObject(o, "accounts");
    for (int i = 0; i < s_nmem; i++) {
        cJSON* m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "account", s_mem[i].account);
        cJSON_AddStringToObject(m, "role", s_mem[i].role);
        cJSON_AddItemToArray(arr, m);
    }
    cJSON_AddStringToObject(o, "connection", chronicle_state_text());
    char* s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}
esp_err_t chron_login_set_account(const char* account, const char* store) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    if (account) strlcpy(ch->account, account, sizeof(ch->account));
    if (store && store[0]) strlcpy(ch->store, store, sizeof(ch->store));
    ch->enabled = app_config_has_chronicle();
    app_config_save();
    app_config_changed();
    return ESP_OK;
}
void chron_login_logout(void) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    chronicle_disconnect_async();
    ch->access_token[0] = 0; ch->refresh_token[0] = 0; ch->token_expiry = 0; ch->login[0] = 0; ch->account[0] = 0; ch->enabled = false;
    s_nmem = 0;
    lock(); memset(&s_st, 0, sizeof(s_st)); unlock();
    app_config_save();
    app_config_changed();
    chronicle_emit(CHRON_EV_LOGIN, NULL, "idle");
}

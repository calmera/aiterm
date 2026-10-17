#include "chron_login.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app_config.h"
#include "cJSON.h"
#include "chronicle.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "github_auth.h"
#include "util/platform.h"

static const char* TAG = "login";

typedef enum { L_IDLE, L_PENDING, L_AUTHORIZED, L_ERROR } login_state_t;
static volatile login_state_t s_state = L_IDLE;
static gh_device_t    s_dev;
static int64_t        s_expires_ms = 0;
static char           s_err[160] = "";
static volatile bool  s_cancel = false;
static volatile bool  s_running = false;
static chron_membership_t s_mem[8];
static int            s_nmem = 0;

void app_config_changed(void);

static void login_task(void* arg) {
    (void)arg;
    cfg_chronicle_t* ch = &app_config()->chronicle;
    char acc[CFG_TOKEN] = "", ref[CFG_TOKEN] = ""; int exp = 0; char err[160] = "";
    int r = 0;
    int interval = s_dev.interval > 0 ? s_dev.interval : 5;
    while (!s_cancel && plat_now_ms() < s_expires_ms) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)interval * 1000));
        if (s_cancel) break;
        r = github_device_poll(ch->github_client_id, s_dev.device_code, acc, sizeof(acc), ref, sizeof(ref), &exp, err, sizeof(err));
        if (r == 1) break;
        if (r < 0) {
            // GitHub answers slow_down as an error string only sometimes; a transport error is retried
            if (strstr(err, "HTTP") || strstr(err, "timeout") || strstr(err, "connect")) { interval += 5; continue; }
            break;
        }
    }
    if (s_cancel) { s_state = L_IDLE; s_running = false; vTaskDelete(NULL); return; }
    if (r != 1) {
        snprintf(s_err, sizeof(s_err), "%s", r == 0 ? "the code expired before it was approved" : err);
        s_state = L_ERROR;
        chronicle_emit_info(r == 0 ? "GitHub sign-in: the code expired" : s_err);
        s_running = false; vTaskDelete(NULL); return;
    }
    strlcpy(ch->access_token, acc, sizeof(ch->access_token));
    strlcpy(ch->refresh_token, ref, sizeof(ch->refresh_token));
    time_t now = time(NULL);
    ch->token_expiry = (exp > 0 && now > 1700000000) ? (int64_t)now + exp : 0;
    app_config_save();
    chronicle_emit_info("GitHub sign-in approved — asking chronicle which accounts you hold…");
    chron_user_info_t who;
    s_nmem = 0;
    esp_err_t e = chronicle_identity(ch->access_token, &who, s_mem, 8, &s_nmem, err, sizeof(err));
    if (e != ESP_OK) {
        snprintf(s_err, sizeof(s_err), "%s", err);
        s_state = L_ERROR;
        chronicle_emit_info(s_err);
        s_running = false; vTaskDelete(NULL); return;
    }
    strlcpy(ch->login, who.login, sizeof(ch->login));
    bool keep = false;
    for (int i = 0; i < s_nmem; i++) if (strcmp(s_mem[i].account, ch->account) == 0) keep = true;
    if (!keep) strlcpy(ch->account, s_nmem >= 1 ? s_mem[0].account : "", sizeof(ch->account));
    ch->enabled = true;
    app_config_save();
    app_config_changed();
    s_state = L_AUTHORIZED;
    char line[200];
    if (s_nmem == 0) snprintf(line, sizeof(line), "signed in as %s, but you hold no chronicle account yet — create one in the console", who.login);
    else if (s_nmem == 1) snprintf(line, sizeof(line), "signed in as %s · account %s", who.login, ch->account);
    else snprintf(line, sizeof(line), "signed in as %s · %d accounts — pick one on the setup page", who.login, s_nmem);
    chronicle_emit_info(line);
    ESP_LOGI(TAG, "%s", line);
    s_running = false;
    vTaskDelete(NULL);
}

esp_err_t chron_login_start(bool refresh_profile, char* err, size_t err_len) {
    err[0] = 0;
    if (s_running) { snprintf(err, err_len, "a sign-in is already in progress"); return ESP_ERR_INVALID_STATE; }
    cfg_chronicle_t* ch = &app_config()->chronicle;
    if (refresh_profile || !ch->nats_url[0] || !ch->github_client_id[0] || !ch->sentinel[0]) {
        if (chronicle_fetch_profile(err, err_len) != ESP_OK) return ESP_FAIL;
    }
    if (github_device_start(ch->github_client_id, &s_dev, err, err_len) != ESP_OK) return ESP_FAIL;
    s_expires_ms = plat_now_ms() + (int64_t)s_dev.expires_in * 1000;
    s_cancel = false; s_err[0] = 0; s_nmem = 0;
    s_state = L_PENDING;
    s_running = true;
    if (xTaskCreatePinnedToCore(login_task, "ghlogin", 12288, NULL, 4, NULL, 1) != pdPASS) { s_running = false; s_state = L_IDLE; snprintf(err, err_len, "could not start the sign-in task"); return ESP_FAIL; }
    char line[200]; snprintf(line, sizeof(line), "GitHub sign-in: open %s and enter the code %s", s_dev.verification_uri, s_dev.user_code);
    chronicle_emit_info(line);
    return ESP_OK;
}
void chron_login_cancel(void) { s_cancel = true; if (!s_running) s_state = L_IDLE; }
char* chron_login_status_json(void) {
    cfg_chronicle_t* ch = &app_config()->chronicle;
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    const char* st = (s_state == L_PENDING && !s_cancel) ? "pending" : s_state == L_AUTHORIZED ? "authorized" : s_state == L_ERROR ? "error" : "idle";
    cJSON_AddStringToObject(o, "state", st);
    if (s_state == L_PENDING && !s_cancel) {
        cJSON_AddStringToObject(o, "user_code", s_dev.user_code);
        cJSON_AddStringToObject(o, "verification_uri", s_dev.verification_uri);
        int64_t left = (s_expires_ms - plat_now_ms()) / 1000;
        cJSON_AddNumberToObject(o, "expires_in", left > 0 ? (double)left : 0);
    }
    if (s_state == L_ERROR) cJSON_AddStringToObject(o, "error", s_err);
    cJSON_AddBoolToObject(o, "signed_in", app_config_has_chronicle());
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
    s_nmem = 0; s_state = L_IDLE;
    app_config_save();
    app_config_changed();
}

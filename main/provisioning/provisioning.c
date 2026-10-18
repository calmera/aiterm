#include "provisioning.h"
#include <string.h>
#include "app_config.h"
#include "captive_dns.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/chron_login.h"
#include "net/chronicle.h"
#include "net/wifi.h"
#include "util/platform.h"

static const char* TAG = "prov";
static httpd_handle_t s_httpd = NULL;
static char s_ap_ssid[32] = "";
static char s_ap_pass[16] = "";
static bool s_ap_active = false;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

// ---------------------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------------------
static esp_err_t send_json(httpd_req_t* req, const char* json) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, json);
}
static esp_err_t send_ok(httpd_req_t* req) { return send_json(req, "{\"ok\":true}"); }
static esp_err_t send_err(httpd_req_t* req, const char* msg) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", msg);
    char* s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    esp_err_t r = send_json(req, s);
    free(s);
    return r;
}
// Read the whole request body (max 32 KB) into a malloc'd string.
static char* read_body(httpd_req_t* req) {
    size_t len = req->content_len;
    if (len > 32 * 1024) return NULL;
    char* buf = (char*)plat_malloc(len + 1);
    if (!buf) return NULL;
    size_t off = 0;
    while (off < len) {
        int r = httpd_req_recv(req, buf + off, len - off);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; plat_free(buf); return NULL; }
        off += (size_t)r;
    }
    buf[len] = 0;
    return buf;
}
static bool is_captive_probe(httpd_req_t* req) {
    char host[128] = "";
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    // Anything that is not our IP/hostname while the AP is up gets redirected.
    if (strncmp(host, "192.168.4.1", 11) == 0) return false;
    if (strncmp(host, "aiterm", 6) == 0) return false;
    char ip[16];
    if (wifi_sta_ip(ip, sizeof(ip)) && strncmp(host, ip, strlen(ip)) == 0) return false;
    return s_ap_active;
}
static esp_err_t redirect_root(httpd_req_t* req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

// ---------------------------------------------------------------------------------------
// handlers
// ---------------------------------------------------------------------------------------
static esp_err_t h_index(httpd_req_t* req) {
    if (is_captive_probe(req)) return redirect_root(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char*)index_html_start, index_html_end - index_html_start - 1);
}
static esp_err_t h_any(httpd_req_t* req) {
    if (s_ap_active) return redirect_root(req);
    httpd_resp_send_404(req);
    return ESP_OK;
}
static esp_err_t h_config_get(httpd_req_t* req) {
    char* json = app_config_export_json(app_config(), true);
    if (!json) return send_err(req, "out of memory");
    esp_err_t r = send_json(req, json);
    free(json);
    return r;
}
static esp_err_t h_config_post(httpd_req_t* req) {
    char* body = read_body(req);
    if (!body) return send_err(req, "bad body");
    esp_err_t r = app_config_import_json(app_config(), body, true);
    plat_free(body);
    if (r != ESP_OK) return send_err(req, "invalid JSON");
    if (app_config_save() != ESP_OK) return send_err(req, "save failed");
    app_config_changed();
    return send_ok(req);
}
static esp_err_t h_wifi_scan(httpd_req_t* req) {
    wifi_scan_entry_t* list = (wifi_scan_entry_t*)plat_calloc(24, sizeof(wifi_scan_entry_t));
    if (!list) return send_err(req, "out of memory");
    int n = wifi_scan(list, 24);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON* arr = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; i < n; i++) {
        cJSON* e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(e, "rssi", list[i].rssi);
        cJSON_AddBoolToObject(e, "secure", list[i].authmode != 0);
        cJSON_AddItemToArray(arr, e);
    }
    plat_free(list);
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t r = send_json(req, s);
    free(s);
    return r;
}
static esp_err_t h_wifi_connect(httpd_req_t* req) {
    char* body = read_body(req);
    if (!body) return send_err(req, "bad body");
    cJSON* o = cJSON_Parse(body);
    plat_free(body);
    if (!o) return send_err(req, "invalid JSON");
    cJSON* ssid = cJSON_GetObjectItemCaseSensitive(o, "ssid");
    cJSON* pass = cJSON_GetObjectItemCaseSensitive(o, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring[0]) { cJSON_Delete(o); return send_err(req, "ssid required"); }
    app_config_t* c = app_config();
    strlcpy(c->wifi.ssid, ssid->valuestring, sizeof(c->wifi.ssid));
    if (cJSON_IsString(pass) && strncmp(pass->valuestring, "••••", strlen("••••")) != 0)
        strlcpy(c->wifi.password, pass->valuestring, sizeof(c->wifi.password));
    cJSON_Delete(o);
    app_config_save();
    wifi_sta_connect(c->wifi.ssid, c->wifi.password);
    app_config_changed();
    return send_ok(req);
}
static esp_err_t h_wifi_status(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    const char* st = "idle";
    switch (wifi_sta_state()) {
        case WIFI_ST_CONNECTING: st = "connecting"; break;
        case WIFI_ST_CONNECTED: st = "connected"; break;
        case WIFI_ST_FAILED: st = "failed"; break;
        case WIFI_ST_OFF: st = "off"; break;
        default: break;
    }
    cJSON_AddStringToObject(root, "state", st);
    cJSON_AddStringToObject(root, "ssid", wifi_sta_ssid());
    char ip[16] = "";
    wifi_sta_ip(ip, sizeof(ip));
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddNumberToObject(root, "rssi", wifi_sta_rssi());
    cJSON_AddStringToObject(root, "error", wifi_sta_last_error());
    cJSON_AddBoolToObject(root, "ap", s_ap_active);
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    esp_err_t r = send_json(req, s);
    free(s);
    return r;
}
static esp_err_t h_inference_test(httpd_req_t* req) {
    // Optional body: apply inference settings first (unsaved) so the user can test before saving.
    if (req->content_len > 0) {
        char* body = read_body(req);
        if (body) {
            cJSON* o = cJSON_Parse(body);
            if (o && cJSON_GetObjectItemCaseSensitive(o, "inference")) app_config_import_json(app_config(), body, true);
            cJSON_Delete(o);
            plat_free(body);
        }
    }
    char* res = app_test_inference();
    esp_err_t r = send_json(req, res ? res : "{\"ok\":false,\"error\":\"internal\"}");
    free(res);
    return r;
}
static esp_err_t h_mcp_test(httpd_req_t* req) {
    char* body = read_body(req);
    if (!body) return send_err(req, "bad body");
    cJSON* o = cJSON_Parse(body);
    plat_free(body);
    if (!o) return send_err(req, "invalid JSON");
    cJSON* url = cJSON_GetObjectItemCaseSensitive(o, "url");
    cJSON* ah = cJSON_GetObjectItemCaseSensitive(o, "auth_header");
    cJSON* av = cJSON_GetObjectItemCaseSensitive(o, "auth_value");
    if (!cJSON_IsString(url) || !url->valuestring[0]) { cJSON_Delete(o); return send_err(req, "url required"); }
    const char* avs = cJSON_IsString(av) ? av->valuestring : "";
    // masked value -> look up the stored one by url
    if (strncmp(avs, "••••", strlen("••••")) == 0) {
        app_config_t* c = app_config();
        for (int i = 0; i < c->mcp_count; i++) if (strcmp(c->mcp[i].url, url->valuestring) == 0) { avs = c->mcp[i].auth_value; break; }
    }
    char* res = app_test_mcp(url->valuestring, cJSON_IsString(ah) ? ah->valuestring : "Authorization", avs);
    cJSON_Delete(o);
    esp_err_t r = send_json(req, res ? res : "{\"ok\":false,\"error\":\"internal\"}");
    free(res);
    return r;
}
// ---- chronicle rooms: GitHub device flow + account choice ----
static esp_err_t h_chron_login_start(httpd_req_t* req) {
    bool refresh = false;
    if (req->content_len > 0) {
        char* body = read_body(req);
        if (body) { cJSON* o = cJSON_Parse(body); cJSON* r = o ? cJSON_GetObjectItemCaseSensitive(o, "refresh_profile") : NULL; refresh = cJSON_IsTrue(r); cJSON* site = o ? cJSON_GetObjectItemCaseSensitive(o, "site") : NULL;
            if (cJSON_IsString(site) && site->valuestring[0]) { strlcpy(app_config()->chronicle.site, site->valuestring, sizeof(app_config()->chronicle.site)); refresh = true; }
            cJSON_Delete(o); plat_free(body); }
    }
    char err[160];
    if (chron_login_start_async(refresh, err, sizeof(err)) != ESP_OK) return send_err(req, err);
    char* s = chron_login_status_json();
    esp_err_t r = send_json(req, s);
    free(s);
    return r;
}
static esp_err_t h_chron_login_status(httpd_req_t* req) { char* s = chron_login_status_json(); esp_err_t r = send_json(req, s); free(s); return r; }
static esp_err_t h_chron_login_cancel(httpd_req_t* req) { chron_login_cancel(); return send_ok(req); }
static esp_err_t h_chron_logout(httpd_req_t* req) { chron_login_logout(); return send_ok(req); }
static esp_err_t h_chron_account(httpd_req_t* req) {
    char* body = read_body(req);
    if (!body) return send_err(req, "bad body");
    cJSON* o = cJSON_Parse(body);
    plat_free(body);
    if (!o) return send_err(req, "invalid JSON");
    cJSON* a = cJSON_GetObjectItemCaseSensitive(o, "account");
    cJSON* st = cJSON_GetObjectItemCaseSensitive(o, "store");
    chron_login_set_account(cJSON_IsString(a) ? a->valuestring : NULL, cJSON_IsString(st) ? st->valuestring : NULL);
    cJSON_Delete(o);
    return send_ok(req);
}
static esp_err_t h_chron_debug(httpd_req_t* req) { char* s = chronicle_debug_json(); esp_err_t r = send_json(req, s ? s : "{}"); free(s); return r; }
static esp_err_t h_chron_test(httpd_req_t* req) {
    char* s = chronicle_test_account_json();
    esp_err_t r = send_json(req, s ? s : "{\"ok\":false,\"error\":\"internal\"}");
    free(s);
    if (app_config()->ui.default_mode == CFG_MODE_ROOMS) chronicle_connect_async();  // the test used the one connection
    return r;
}

static void reboot_task(void* arg) { (void)arg; vTaskDelay(pdMS_TO_TICKS(600)); esp_restart(); }
static esp_err_t h_reboot(httpd_req_t* req) {
    send_ok(req);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}
static esp_err_t h_reset(httpd_req_t* req) {
    app_config_reset();
    return h_reboot(req);
}

// ---------------------------------------------------------------------------------------
esp_err_t provisioning_http_start(void) {
    if (s_httpd) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24;
    cfg.stack_size = 12288;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;
    esp_err_t r = httpd_start(&s_httpd, &cfg);
    if (r != ESP_OK) { ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(r)); return r; }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/config", .method = HTTP_GET, .handler = h_config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = h_config_post},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = h_wifi_scan},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = h_wifi_connect},
        {.uri = "/api/wifi/status", .method = HTTP_GET, .handler = h_wifi_status},
        {.uri = "/api/inference/test", .method = HTTP_POST, .handler = h_inference_test},
        {.uri = "/api/mcp/test", .method = HTTP_POST, .handler = h_mcp_test},
        {.uri = "/api/chronicle/login/start", .method = HTTP_POST, .handler = h_chron_login_start},
        {.uri = "/api/chronicle/login/status", .method = HTTP_GET, .handler = h_chron_login_status},
        {.uri = "/api/chronicle/login/cancel", .method = HTTP_POST, .handler = h_chron_login_cancel},
        {.uri = "/api/chronicle/logout", .method = HTTP_POST, .handler = h_chron_logout},
        {.uri = "/api/chronicle/account", .method = HTTP_POST, .handler = h_chron_account},
        {.uri = "/api/chronicle/test", .method = HTTP_POST, .handler = h_chron_test},
        {.uri = "/api/chronicle/debug", .method = HTTP_GET, .handler = h_chron_debug},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/reset", .method = HTTP_POST, .handler = h_reset},
        {.uri = "/*", .method = HTTP_GET, .handler = h_any},  // captive portal probes
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    ESP_LOGI(TAG, "http server started");
    return ESP_OK;
}

esp_err_t provisioning_ap_start(void) {
    if (!wifi_available()) return ESP_ERR_INVALID_STATE;
    char suffix[5];
    wifi_mac_suffix(suffix);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "aiterm-%s", suffix);
    s_ap_pass[0] = 0;  // open network: scanning the Wi-Fi QR joins without typing anything
    esp_err_t r = wifi_ap_start(s_ap_ssid, NULL);
    if (r != ESP_OK) return r;
    s_ap_active = true;
    captive_dns_start();
    provisioning_http_start();
    return ESP_OK;
}
void provisioning_ap_stop(void) {
    if (!s_ap_active) return;
    s_ap_active = false;
    captive_dns_stop();
    wifi_ap_stop();
}
bool provisioning_ap_active(void) { return s_ap_active; }
const char* provisioning_ap_ssid(void) { return s_ap_ssid; }
const char* provisioning_ap_password(void) { return s_ap_pass; }

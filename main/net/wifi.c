#include "wifi.h"
#include <string.h>
#include "bsp/power.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "wifi_remote.h"

static const char* TAG = "wifi";

static bool               s_available   = false;
static esp_netif_t*       s_sta_netif   = NULL;
static esp_netif_t*       s_ap_netif    = NULL;
static EventGroupHandle_t s_events      = NULL;
static wifi_sta_state_t   s_state       = WIFI_ST_OFF;
static char               s_ssid[33]    = "";
static char               s_pass[65]    = "";
static char               s_ip[16]      = "";
static char               s_err[64]     = "";
static bool               s_ap_on       = false;
static int                s_ap_clients  = 0;
static int                s_retries     = 0;
static bool               s_started     = false;
static bool               s_scanning    = false;
static wifi_status_cb_t   s_cb          = NULL;
static void*              s_cb_user     = NULL;

#define EV_CONNECTED BIT0
#define EV_FAILED    BIT1
#define MAX_RETRIES  4

static void notify(void) { if (s_cb) s_cb(s_cb_user); }

static const char* reason_str(int r) {
    switch (r) {
        case WIFI_REASON_NO_AP_FOUND: return "network not found";
        case WIFI_REASON_AUTH_FAIL: case WIFI_REASON_AUTH_EXPIRE: case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "wrong password";
        case WIFI_REASON_ASSOC_FAIL: return "association failed";
        case WIFI_REASON_BEACON_TIMEOUT: return "connection lost";
        default: return "disconnected";
    }
}

static void apply_mode(void) {
    wifi_mode_t mode = WIFI_MODE_NULL;
    bool want_sta = true;  // keep STA up so scanning works
    if (want_sta && s_ap_on) mode = WIFI_MODE_APSTA;
    else if (s_ap_on) mode = WIFI_MODE_AP;
    else if (want_sta) mode = WIFI_MODE_STA;
    esp_wifi_set_mode(mode);
}

static void on_wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                if (s_ssid[0] && s_state == WIFI_ST_CONNECTING && !s_scanning) esp_wifi_connect();
                break;
            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "associated with %s", s_ssid);
                break;
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)data;
                s_ip[0] = 0;
                if (s_state == WIFI_ST_CONNECTING || s_state == WIFI_ST_CONNECTED) {
                    if (s_retries < MAX_RETRIES && s_ssid[0]) {
                        s_retries++;
                        ESP_LOGW(TAG, "disconnected (%d), retry %d", d ? d->reason : -1, s_retries);
                        s_state = WIFI_ST_CONNECTING;
                        esp_wifi_connect();
                    } else {
                        strlcpy(s_err, d ? reason_str(d->reason) : "disconnected", sizeof(s_err));
                        s_state = WIFI_ST_FAILED;
                        xEventGroupSetBits(s_events, EV_FAILED);
                    }
                    notify();
                }
                break;
            }
            case WIFI_EVENT_AP_STACONNECTED: s_ap_clients++; notify(); break;
            case WIFI_EVENT_AP_STADISCONNECTED: if (s_ap_clients > 0) s_ap_clients--; notify(); break;
            default: break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t* e = (ip_event_got_ip_t*)data;
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
            s_state = WIFI_ST_CONNECTED;
            s_retries = 0;
            s_err[0] = 0;
            ESP_LOGI(TAG, "got ip %s", s_ip);
            xEventGroupSetBits(s_events, EV_CONNECTED);
            notify();
        } else if (id == IP_EVENT_STA_LOST_IP) {
            s_ip[0] = 0;
            notify();
        }
    }
}

esp_err_t wifi_init(void) {
    if (s_events) return s_available ? ESP_OK : ESP_FAIL;
    s_events = xEventGroupCreate();
    // Power-cycle the C6 into application mode (same sequence as the launcher).
    bsp_radio_state_t prev = BSP_POWER_RADIO_STATE_OFF;
    bsp_power_get_radio_state(&prev);
    if (prev != BSP_POWER_RADIO_STATE_OFF) { bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_OFF); vTaskDelay(pdMS_TO_TICKS(100)); }
    bsp_power_set_radio_state(BSP_POWER_RADIO_STATE_APPLICATION);
    if (wifi_remote_initialize() != ESP_OK) {
        ESP_LOGE(TAG, "radio not responding (ESP-Hosted)");
        strlcpy(s_err, "radio not responding", sizeof(s_err));
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t r = esp_event_loop_create_default();
    if (r != ESP_OK && r != ESP_ERR_INVALID_STATE) return r;
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta_netif, "aiterm");
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;  // never persist credentials in the C6's NVS
    r = esp_wifi_init(&cfg);
    if (r != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(r)); strlcpy(s_err, "wifi init failed", sizeof(s_err)); return r; }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    apply_mode();
    r = esp_wifi_start();
    if (r != ESP_OK) { ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(r)); strlcpy(s_err, "wifi start failed", sizeof(s_err)); return r; }
    s_started = true;
    s_available = true;
    s_state = WIFI_ST_IDLE;
    ESP_LOGI(TAG, "wifi stack ready");
    return ESP_OK;
}
bool wifi_available(void) { return s_available; }

esp_err_t wifi_sta_connect(const char* ssid, const char* password) {
    if (!s_available) return ESP_ERR_INVALID_STATE;
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password ? password : "", sizeof(s_pass));
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, s_ssid, strnlen(s_ssid, 32));
    memcpy(wc.sta.password, s_pass, strnlen(s_pass, 64));
    wc.sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, EV_CONNECTED | EV_FAILED);
    s_retries = 0;
    s_err[0] = 0;
    s_ip[0] = 0;
    s_state = WIFI_ST_CONNECTING;
    apply_mode();
    esp_err_t r = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (r != ESP_OK) { s_state = WIFI_ST_FAILED; strlcpy(s_err, "bad configuration", sizeof(s_err)); return r; }
    r = esp_wifi_connect();
    if (r != ESP_OK && r != ESP_ERR_WIFI_CONN) { ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(r)); }
    notify();
    return ESP_OK;
}
void wifi_sta_disconnect(void) {
    s_ssid[0] = 0;
    s_state = WIFI_ST_IDLE;
    s_ip[0] = 0;
    esp_wifi_disconnect();
    notify();
}
wifi_sta_state_t wifi_sta_state(void) { return s_state; }
bool wifi_sta_connected(void) { return s_state == WIFI_ST_CONNECTED && s_ip[0]; }
bool wifi_sta_ip(char* out, size_t n) { if (!s_ip[0]) { if (n) out[0] = 0; return false; } strlcpy(out, s_ip, n); return true; }
int wifi_sta_rssi(void) {
    if (!wifi_sta_connected()) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}
const char* wifi_sta_ssid(void) { return s_ssid; }
const char* wifi_sta_last_error(void) { return s_err; }
bool wifi_sta_wait(int timeout_ms) {
    if (!s_events) return false;
    EventBits_t b = xEventGroupWaitBits(s_events, EV_CONNECTED | EV_FAILED, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (b & EV_CONNECTED) != 0;
}

esp_err_t wifi_ap_start(const char* ssid, const char* password) {
    if (!s_available) return ESP_ERR_INVALID_STATE;
    wifi_config_t ac = {0};
    strlcpy((char*)ac.ap.ssid, ssid, sizeof(ac.ap.ssid));
    ac.ap.ssid_len = strlen((char*)ac.ap.ssid);
    ac.ap.channel = 6;
    ac.ap.max_connection = 4;
    if (password && strlen(password) >= 8) {
        strlcpy((char*)ac.ap.password, password, sizeof(ac.ap.password));
        ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ac.ap.authmode = WIFI_AUTH_OPEN;
    }
    ac.ap.pmf_cfg.required = false;
    s_ap_on = true;
    s_ap_clients = 0;
    apply_mode();
    esp_err_t r = esp_wifi_set_config(WIFI_IF_AP, &ac);
    if (r != ESP_OK) { ESP_LOGE(TAG, "ap config: %s", esp_err_to_name(r)); s_ap_on = false; apply_mode(); return r; }
    ESP_LOGI(TAG, "softAP %s started", ssid);
    notify();
    return ESP_OK;
}
void wifi_ap_stop(void) {
    if (!s_ap_on) return;
    s_ap_on = false;
    apply_mode();
    notify();
}
bool wifi_ap_active(void) { return s_ap_on; }
const char* wifi_ap_ip(void) { return "192.168.4.1"; }
int wifi_ap_clients(void) { return s_ap_clients; }

int wifi_scan(wifi_scan_entry_t* out, int max) {
    if (!s_available || max <= 0) return 0;
    s_scanning = true;
    wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    sc.scan_time.active.min = 100;
    sc.scan_time.active.max = 300;
    esp_err_t r = esp_wifi_scan_start(&sc, true);
    s_scanning = false;
    if (r != ESP_OK) { ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(r)); return 0; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    wifi_ap_record_t* recs = (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_clear_ap_list(); return 0; }
    uint16_t got = n;
    esp_wifi_scan_get_ap_records(&got, recs);
    int cnt = 0;
    for (int i = 0; i < got && cnt < max; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < cnt; j++) if (strcmp(out[j].ssid, (char*)recs[i].ssid) == 0) { dup = true; break; }
        if (dup) continue;
        strlcpy(out[cnt].ssid, (char*)recs[i].ssid, sizeof(out[cnt].ssid));
        out[cnt].rssi = recs[i].rssi;
        out[cnt].authmode = (uint8_t)recs[i].authmode;
        cnt++;
    }
    free(recs);
    // After a scan the C6 may have dropped the pending connect; kick it again.
    if (s_state == WIFI_ST_CONNECTING && s_ssid[0]) esp_wifi_connect();
    return cnt;
}

void wifi_mac_suffix(char out[5]) {
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) esp_efuse_mac_get_default(mac);
    snprintf(out, 5, "%02X%02X", mac[4], mac[5]);
}
void wifi_set_status_cb(wifi_status_cb_t cb, void* user) { s_cb = cb; s_cb_user = user; }

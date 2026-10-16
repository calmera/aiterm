// Wi-Fi via the ESP32-C6 (ESP-Hosted + esp_wifi_remote). Station + optional SoftAP for
// provisioning. The esp_wifi_* API is transparently remoted, so this is plain esp_wifi code.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WIFI_ST_OFF = 0, WIFI_ST_IDLE, WIFI_ST_CONNECTING, WIFI_ST_CONNECTED, WIFI_ST_FAILED } wifi_sta_state_t;

typedef struct {
    char    ssid[33];
    int8_t  rssi;
    uint8_t authmode;  // wifi_auth_mode_t
} wifi_scan_entry_t;

typedef void (*wifi_status_cb_t)(void* user);

// Power-cycles the radio, starts ESP-Hosted and the TCP/IP stack. Returns ESP_OK when the C6
// answered. Safe to call once.
esp_err_t wifi_init(void);
bool      wifi_available(void);

esp_err_t wifi_sta_connect(const char* ssid, const char* password);  // asynchronous
void      wifi_sta_disconnect(void);
wifi_sta_state_t wifi_sta_state(void);
bool      wifi_sta_connected(void);
bool      wifi_sta_ip(char* out, size_t n);   // dotted quad, false if none
int       wifi_sta_rssi(void);
const char* wifi_sta_ssid(void);
const char* wifi_sta_last_error(void);
// Wait up to timeout_ms for connected or failed. Returns true when connected.
bool      wifi_sta_wait(int timeout_ms);

esp_err_t wifi_ap_start(const char* ssid, const char* password);  // password NULL/"" = open
void      wifi_ap_stop(void);
bool      wifi_ap_active(void);
const char* wifi_ap_ip(void);  // "192.168.4.1"
int       wifi_ap_clients(void);

// Blocking scan (a few seconds). Returns number of entries written.
int       wifi_scan(wifi_scan_entry_t* out, int max);

// "XXXX": last 4 hex digits of the station MAC (uppercase).
void      wifi_mac_suffix(char out[5]);
void      wifi_set_status_cb(wifi_status_cb_t cb, void* user);  // called from the event task

#ifdef __cplusplus
}
#endif

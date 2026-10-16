#include "captive_dns.h"
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char* TAG = "dns";
static TaskHandle_t s_task = NULL;
static volatile bool s_run = false;
static int s_sock = -1;

static void dns_task(void* arg) {
    (void)arg;
    uint8_t buf[512];
    struct sockaddr_in from;
    while (s_run) {
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
        if (n < 12) { if (!s_run) break; continue; }
        // Parse the question name to find its end.
        int i = 12;
        while (i < n && buf[i] != 0) { i += buf[i] + 1; if (i >= n) break; }
        i += 1 + 4;  // null label + qtype + qclass
        if (i > n) continue;
        uint16_t qtype = (uint16_t)((buf[i - 4] << 8) | buf[i - 3]);
        // Build response: header flags = standard response, no error; 1 answer for A queries.
        buf[2] = 0x81; buf[3] = 0x80;
        buf[4] = 0; buf[5] = 1;      // QDCOUNT 1
        buf[8] = 0; buf[9] = 0; buf[10] = 0; buf[11] = 0;
        int len = i;
        if (qtype == 1 && len + 16 <= (int)sizeof(buf)) {
            buf[6] = 0; buf[7] = 1;  // ANCOUNT 1
            uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1};
            memcpy(buf + len, ans, sizeof(ans));
            len += sizeof(ans);
        } else {
            buf[6] = 0; buf[7] = 0;
        }
        sendto(s_sock, buf, len, 0, (struct sockaddr*)&from, fl);
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t captive_dns_start(void) {
    if (s_task) return ESP_OK;
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_sock < 0) return ESP_FAIL;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { close(s_sock); s_sock = -1; return ESP_FAIL; }
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    s_run = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_task);
    ESP_LOGI(TAG, "captive DNS started");
    return ESP_OK;
}
void captive_dns_stop(void) {
    s_run = false;
    if (s_sock >= 0) { shutdown(s_sock, SHUT_RDWR); close(s_sock); s_sock = -1; }
}

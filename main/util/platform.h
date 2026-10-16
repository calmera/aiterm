// Platform abstraction so protocol/parser code compiles both on the ESP32-P4 target and
// in the host unit tests (host_tests/). Only tiny shims live here: memory, logging, time.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
// Large/long-lived buffers go to PSRAM; fall back to any memory if PSRAM is exhausted.
static inline void* plat_malloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}
static inline void* plat_calloc(size_t n, size_t sz) {
    void* p = heap_caps_calloc(n, sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(n, sz);
}
static inline void* plat_realloc(void* p, size_t n) {
    void* q = heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : realloc(p, n);
}
static inline void plat_free(void* p) { free(p); }
static inline int64_t plat_now_ms(void) { return esp_timer_get_time() / 1000; }
#define PLAT_LOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define PLAT_LOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define PLAT_LOGE(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define PLAT_LOGD(tag, ...) ESP_LOGD(tag, __VA_ARGS__)
#else
#include <stdio.h>
#include <time.h>
static inline void* plat_malloc(size_t n) { return malloc(n); }
static inline void* plat_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static inline void* plat_realloc(void* p, size_t n) { return realloc(p, n); }
static inline void plat_free(void* p) { free(p); }
static inline int64_t plat_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#define PLAT_LOGI(tag, ...) do { fprintf(stderr, "I %s: ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define PLAT_LOGW(tag, ...) do { fprintf(stderr, "W %s: ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define PLAT_LOGE(tag, ...) do { fprintf(stderr, "E %s: ", tag); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define PLAT_LOGD(tag, ...) do { } while (0)
#endif

static inline char* plat_strdup(const char* s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char* d = (char*)plat_malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}
static inline char* plat_strndup(const char* s, size_t n) {
    if (!s) return NULL;
    size_t l = strnlen(s, n);
    char* d = (char*)plat_malloc(l + 1);
    if (d) { memcpy(d, s, l); d[l] = 0; }
    return d;
}

#ifdef __cplusplus
}
#endif

// Voice I/O seam (Phase 7 — NOT implemented in this build). STT/TTS are meant to be backend
// services (OpenAI-compatible /audio/transcriptions and /audio/speech); the ESP32-C6 radio is
// BLE-only (no A2DP headsets), so audio goes through the badge speaker/microphone via the BSP.
// Enable with CONFIG_AITERM_VOICE once implemented.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*voice_record_cb_t)(const int16_t* pcm, size_t samples, void* user);

// Start capturing 16 kHz mono PCM from the microphone (if the badge revision has one).
esp_err_t voice_record_start(voice_record_cb_t cb, void* user);
esp_err_t voice_record_stop(void);
// Play 16-bit PCM through the speaker amplifier.
esp_err_t voice_play_pcm(const int16_t* pcm, size_t samples, uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

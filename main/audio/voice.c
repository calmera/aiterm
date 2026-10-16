#include "voice.h"
#include "sdkconfig.h"
#if CONFIG_AITERM_VOICE
#error "CONFIG_AITERM_VOICE is enabled but voice is not implemented yet (see audio/voice.h)"
#endif
esp_err_t voice_record_start(voice_record_cb_t cb, void* user) { (void)cb; (void)user; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t voice_record_stop(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t voice_play_pcm(const int16_t* pcm, size_t samples, uint32_t sample_rate) { (void)pcm; (void)samples; (void)sample_rate; return ESP_ERR_NOT_SUPPORTED; }

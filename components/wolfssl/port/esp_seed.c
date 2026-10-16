// RNG seed for wolfCrypt from the ESP32 hardware RNG.
#include "esp_random.h"
int aiterm_wolf_generate_seed(unsigned char* output, unsigned int sz) {
    if (!output) return 1;
    esp_fill_random(output, sz);
    return 0;
}

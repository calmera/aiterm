// Board support package API: WHY2025 power functions
// SPDX-License-Identifier: MIT
//
// The WHY2025 badge is powered by an IP5306 power-bank IC on the carrier board. Its KEY pin is
// wired to the power button only (R15 -> PWR_BUTTON1 in the carrier schematic); nothing on the
// P4 reaches it, so the firmware cannot switch the badge's 5 V rail off. What the IP5306 does do
// is shut its output down by itself once the load stays under its light-load threshold (about
// 45 mA for 32 s, the standard IP5306 behaviour) while it runs from the cells.
//
// bsp_power_off() therefore brings the badge under that threshold and parks the P4 in deep sleep:
// the panel is switched off and held in reset, the ESP32-C6 is held in reset -- its EN line also
// resets the TCA8418 keypad controller, and with the C6 down its backlight PWM pins float, so the
// 100 kOhm pull-downs on both AP3032 CTRL pins switch the display and keyboard backlights off --
// and the P4 sleeps with no wake source. On the cells the IP5306 then cuts the power and the
// power button starts the badge again. On USB power the IP5306 keeps its output up; the badge
// stays dark in deep sleep until it is reset (USB unplugged and plugged again, or the CH340's
// DTR/RTS lines, which is what esptool does). The badge has no reset button.

#include <stdbool.h>
#include <stdint.h>
#include "bsp/display.h"
#include "bsp/power.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "why2025_hardware.h"

static char const* TAG = "BSP power";

// Drive a pin to a level and hold it there. GPIO 0..15 are LP pads on the ESP32-P4: their hold
// is kept through deep sleep. Digital pads that are outputs are held by the sleep code as well
// (CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND).
static void hold_output(gpio_num_t pin, int level) {
    gpio_hold_dis(pin);
    gpio_config_t cfg = {
        .pin_bit_mask = BIT64(pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(pin, level);
    gpio_hold_en(pin);
}

esp_err_t bsp_power_initialize(void) {
    // A hold left over from bsp_power_off() would keep the radio and the keypad controller in
    // reset. A power-on reset clears holds, a deep-sleep wake would not; release them regardless.
    gpio_hold_dis(BSP_RADIO_ENABLE_PIN);
    gpio_hold_dis(BSP_LCD_RESET_PIN);
    return ESP_OK;
}

esp_err_t bsp_power_off(bool enable_alarm_wakeup) {
    (void)enable_alarm_wakeup;  // no RTC alarm hardware on this badge
    ESP_LOGI(TAG, "power off: panel off");

    // 1. Panel: display off, sleep in, then hold it in reset (GPIO 17 high asserts the reset).
    esp_lcd_panel_handle_t    panel = NULL;
    esp_lcd_panel_io_handle_t io    = NULL;
    if (bsp_display_get_panel(&panel) == ESP_OK && panel != NULL) {
        esp_lcd_panel_disp_on_off(panel, false);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (bsp_display_get_panel_io(&io) == ESP_OK && io != NULL) {
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPIN, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    hold_output(BSP_LCD_RESET_PIN, 1);

    // 2. Radio off: hold the ESP32-C6 in reset (takes the keypad controller and both backlights
    //    down with it, see the header comment). Done last and followed immediately by the sleep,
    //    so the ESP-Hosted transport has no time to react to its slave disappearing.
    ESP_LOGI(TAG, "power off: radio off, deep sleep");
    hold_output(BSP_RADIO_ENABLE_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    // 3. Deep sleep with no wake source: only a reset brings the P4 back.
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
    return ESP_OK;  // not reached
}

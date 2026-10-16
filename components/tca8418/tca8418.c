// SPDX-FileCopyrightText: 2026 Nicolai Electronics
// SPDX-License-Identifier: MIT

#include "tca8418.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "tca8418_regs.h"

static char const TAG[] = "TCA8418";

static esp_err_t tca8418_read_reg(tca8418_handle_t* handle, uint8_t reg, uint8_t* value) {
    return i2c_master_transmit_receive(handle->dev_handle, &reg, 1, value, 1, -1);
}

static esp_err_t tca8418_write_reg(tca8418_handle_t* handle, uint8_t reg, uint8_t value) {
    uint8_t buffer[2] = {reg, value};
    return i2c_master_transmit(handle->dev_handle, buffer, sizeof(buffer), -1);
}

IRAM_ATTR static void tca8418_interrupt_handler(void* pvParameters) {
    tca8418_handle_t* handle = (tca8418_handle_t*)pvParameters;
    xSemaphoreGiveFromISR(handle->interrupt_semaphore, NULL);
}

static void tca8418_interrupt_thread_entry(void* pvParameters) {
    tca8418_handle_t* handle = (tca8418_handle_t*)pvParameters;

    bool wait = false;

    while (true) {
        // Wait for interrupt
        if (wait) {
            xSemaphoreTake(handle->interrupt_semaphore, pdMS_TO_TICKS(1000));
        }

        bool      cad_int      = false;
        bool      ovr_flow_int = false;
        bool      k_lck_int    = false;
        bool      gpi_int      = false;
        bool      k_int        = false;
        esp_err_t res          = tca8418_get_int_stat(handle, &cad_int, &ovr_flow_int, &k_lck_int, &gpi_int, &k_int);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read interrupt status register: %s", esp_err_to_name(res));
            continue;
        }

        if (!cad_int && !ovr_flow_int && !k_lck_int && !gpi_int && !k_int) {
            // No interrupt
            wait = true;
            continue;
        }

        wait = false;

        if (cad_int && handle->interrupt_callback_cad) {
            handle->interrupt_callback_cad(handle);
        }
        if (ovr_flow_int && handle->interrupt_callback_overflow) {
            handle->interrupt_callback_overflow(handle);
        }
        if (k_lck_int && handle->interrupt_callback_lock) {
            handle->interrupt_callback_lock(handle);
        }
        if (gpi_int && handle->interrupt_callback_gpi) {
            handle->interrupt_callback_gpi(handle);
        }
        if (k_int && handle->interrupt_callback_key) {
            handle->interrupt_callback_key(handle);
        }

        // Writing a 1 to a status bit clears it, so only clear the flags that were set
        res = tca8418_set_int_stat(handle, cad_int, ovr_flow_int, k_lck_int, gpi_int, k_int);
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "Failed to clear interrupt status register: %s", esp_err_to_name(res));
        }
    }
}

esp_err_t tca8418_initialize(tca8418_handle_t* handle, i2c_master_bus_handle_t i2c_bus, gpio_num_t reset_pin,
                             gpio_num_t interrupt_pin) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    handle->reset_pin     = reset_pin;
    handle->interrupt_pin = interrupt_pin;

    uint8_t i2c_address = 0x34;

    esp_err_t res = i2c_master_probe(i2c_bus, i2c_address, 200);

    if (res != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = i2c_address,
        .scl_speed_hz    = 400000,
    };

    if (reset_pin >= 0) {
        gpio_config_t int_pin_cfg = {
            .pin_bit_mask = BIT64(reset_pin),
            .mode         = GPIO_MODE_OUTPUT,
        };
        res = gpio_config(&int_pin_cfg);
        if (res != ESP_OK) {
            return res;
        }

        gpio_set_level(reset_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(reset_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    res = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &handle->dev_handle);
    if (res != ESP_OK) {
        return res;
    }

    if (interrupt_pin >= 0) {
        gpio_config_t int_pin_cfg = {
            .pin_bit_mask = BIT64(interrupt_pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = false,
            .pull_down_en = false,
            .intr_type    = GPIO_INTR_NEGEDGE,
        };
        res = gpio_config(&int_pin_cfg);
        if (res != ESP_OK) {
            i2c_master_bus_rm_device(handle->dev_handle);
            return res;
        }

        handle->interrupt_semaphore = xSemaphoreCreateBinary();
        if (handle->interrupt_semaphore == NULL) {
            i2c_master_bus_rm_device(handle->dev_handle);
            return ESP_ERR_NO_MEM;
        }

        res = gpio_isr_handler_add(interrupt_pin, tca8418_interrupt_handler, (void*)handle);
        if (res != ESP_OK) {
            i2c_master_bus_rm_device(handle->dev_handle);
            vSemaphoreDelete(handle->interrupt_semaphore);
            return res;
        }

        if (xTaskCreate(tca8418_interrupt_thread_entry, "tca8418", 4096, (void*)handle, 0,
                        &handle->interrupt_handler_thread) != pdTRUE) {
            i2c_master_bus_rm_device(handle->dev_handle);
            vSemaphoreDelete(handle->interrupt_semaphore);
            return ESP_ERR_NO_MEM;
        }
    }

    return res;
}

esp_err_t tca8418_set_cad_callback(tca8418_handle_t* handle, tca8418_cb_t callback) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->interrupt_callback_cad = callback;
    return ESP_OK;
}

esp_err_t tca8418_set_overflow_callback(tca8418_handle_t* handle, tca8418_cb_t callback) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->interrupt_callback_overflow = callback;
    return ESP_OK;
}

esp_err_t tca8418_set_lock_callback(tca8418_handle_t* handle, tca8418_cb_t callback) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->interrupt_callback_lock = callback;
    return ESP_OK;
}

esp_err_t tca8418_set_gpi_callback(tca8418_handle_t* handle, tca8418_cb_t callback) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->interrupt_callback_gpi = callback;
    return ESP_OK;
}

esp_err_t tca8418_set_key_callback(tca8418_handle_t* handle, tca8418_cb_t callback) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->interrupt_callback_key = callback;
    return ESP_OK;
}

esp_err_t tca8418_get_cfg(tca8418_handle_t* handle, bool* ai, bool* gpi_e_cfg, bool* ovr_flow_m, bool* int_cfg,
                          bool* ovr_flow_ien, bool* k_lck_ien, bool* gpi_ien, bool* ke_ien) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_CFG, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (ai != NULL) {
        *ai = (value & TCA8418_REG_CFG_AI_MSK) != 0;
    }
    if (gpi_e_cfg != NULL) {
        *gpi_e_cfg = (value & TCA8418_REG_CFG_GPI_E_CFG_MSK) != 0;
    }
    if (ovr_flow_m != NULL) {
        *ovr_flow_m = (value & TCA8418_REG_CFG_OVR_FLOW_M_MSK) != 0;
    }
    if (int_cfg != NULL) {
        *int_cfg = (value & TCA8418_REG_CFG_INT_CFG_MSK) != 0;
    }
    if (ovr_flow_ien != NULL) {
        *ovr_flow_ien = (value & TCA8418_REG_CFG_OVR_FLOW_IEN_MSK) != 0;
    }
    if (k_lck_ien != NULL) {
        *k_lck_ien = (value & TCA8418_REG_CFG_K_LCK_IEN_MSK) != 0;
    }
    if (gpi_ien != NULL) {
        *gpi_ien = (value & TCA8418_REG_CFG_GPI_IEN_MSK) != 0;
    }
    if (ke_ien != NULL) {
        *ke_ien = (value & TCA8418_REG_CFG_KE_IEN_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_cfg(tca8418_handle_t* handle, bool ai, bool gpi_e_cfg, bool ovr_flow_m, bool int_cfg,
                          bool ovr_flow_ien, bool k_lck_ien, bool gpi_ien, bool ke_ien) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= ai ? TCA8418_REG_CFG_AI_MSK : 0;
    value         |= gpi_e_cfg ? TCA8418_REG_CFG_GPI_E_CFG_MSK : 0;
    value         |= ovr_flow_m ? TCA8418_REG_CFG_OVR_FLOW_M_MSK : 0;
    value         |= int_cfg ? TCA8418_REG_CFG_INT_CFG_MSK : 0;
    value         |= ovr_flow_ien ? TCA8418_REG_CFG_OVR_FLOW_IEN_MSK : 0;
    value         |= k_lck_ien ? TCA8418_REG_CFG_K_LCK_IEN_MSK : 0;
    value         |= gpi_ien ? TCA8418_REG_CFG_GPI_IEN_MSK : 0;
    value         |= ke_ien ? TCA8418_REG_CFG_KE_IEN_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_CFG, value);
}

esp_err_t tca8418_get_int_stat(tca8418_handle_t* handle, bool* cad_int, bool* ovr_flow_int, bool* k_lck_int,
                               bool* gpi_int, bool* k_int) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_INT_STAT, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (cad_int != NULL) {
        *cad_int = (value & TCA8418_REG_INT_STAT_CAD_INT_MSK) != 0;
    }
    if (ovr_flow_int != NULL) {
        *ovr_flow_int = (value & TCA8418_REG_INT_STAT_OVR_FLOW_INT_MSK) != 0;
    }
    if (k_lck_int != NULL) {
        *k_lck_int = (value & TCA8418_REG_INT_STAT_K_LCK_INT_MSK) != 0;
    }
    if (gpi_int != NULL) {
        *gpi_int = (value & TCA8418_REG_INT_STAT_GPI_INT_MSK) != 0;
    }
    if (k_int != NULL) {
        *k_int = (value & TCA8418_REG_INT_STAT_K_INT_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_int_stat(tca8418_handle_t* handle, bool cad_int, bool ovr_flow_int, bool k_lck_int, bool gpi_int,
                               bool k_int) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= cad_int ? TCA8418_REG_INT_STAT_CAD_INT_MSK : 0;
    value         |= ovr_flow_int ? TCA8418_REG_INT_STAT_OVR_FLOW_INT_MSK : 0;
    value         |= k_lck_int ? TCA8418_REG_INT_STAT_K_LCK_INT_MSK : 0;
    value         |= gpi_int ? TCA8418_REG_INT_STAT_GPI_INT_MSK : 0;
    value         |= k_int ? TCA8418_REG_INT_STAT_K_INT_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_INT_STAT, value);
}

esp_err_t tca8418_get_key_lck_ec(tca8418_handle_t* handle, bool* k_lck_en, bool* lck2, bool* lck1, uint8_t* kec) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_LCK_EC, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (k_lck_en != NULL) {
        *k_lck_en = (value & TCA8418_REG_KEY_LCK_EC_K_LCK_EN_MSK) != 0;
    }
    if (lck2 != NULL) {
        *lck2 = (value & TCA8418_REG_KEY_LCK_EC_LCK2_MSK) != 0;
    }
    if (lck1 != NULL) {
        *lck1 = (value & TCA8418_REG_KEY_LCK_EC_LCK1_MSK) != 0;
    }
    if (kec != NULL) {
        *kec = (uint8_t)((value & TCA8418_REG_KEY_LCK_EC_KEC_MSK) >> TCA8418_REG_KEY_LCK_EC_KEC_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_lck_ec(tca8418_handle_t* handle, bool k_lck_en, bool lck2, bool lck1, uint8_t kec) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= k_lck_en ? TCA8418_REG_KEY_LCK_EC_K_LCK_EN_MSK : 0;
    value         |= lck2 ? TCA8418_REG_KEY_LCK_EC_LCK2_MSK : 0;
    value         |= lck1 ? TCA8418_REG_KEY_LCK_EC_LCK1_MSK : 0;
    value         |= (kec << TCA8418_REG_KEY_LCK_EC_KEC_POS) & TCA8418_REG_KEY_LCK_EC_KEC_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_LCK_EC, value);
}

esp_err_t tca8418_get_key_event_a(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_A, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_a(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_A, value);
}

esp_err_t tca8418_get_key_event_b(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_B, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_b(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_B, value);
}

esp_err_t tca8418_get_key_event_c(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_C, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_c(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_C, value);
}

esp_err_t tca8418_get_key_event_d(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_D, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_d(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_D, value);
}

esp_err_t tca8418_get_key_event_e(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_E, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_e(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_E, value);
}

esp_err_t tca8418_get_key_event_f(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_F, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_f(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_F, value);
}

esp_err_t tca8418_get_key_event_g(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_G, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_g(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_G, value);
}

esp_err_t tca8418_get_key_event_h(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_H, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_h(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_H, value);
}

esp_err_t tca8418_get_key_event_i(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_I, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_i(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_I, value);
}

esp_err_t tca8418_get_key_event_j(tca8418_handle_t* handle, bool* pressed, uint8_t* code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KEY_EVENT_J, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (pressed != NULL) {
        *pressed = (value & TCA8418_REG_KEY_EVENT_STATE_MSK) != 0;
    }
    if (code != NULL) {
        *code = (uint8_t)((value & TCA8418_REG_KEY_EVENT_CODE_MSK) >> TCA8418_REG_KEY_EVENT_CODE_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_key_event_j(tca8418_handle_t* handle, bool pressed, uint8_t code) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= pressed ? TCA8418_REG_KEY_EVENT_STATE_MSK : 0;
    value         |= (code << TCA8418_REG_KEY_EVENT_CODE_POS) & TCA8418_REG_KEY_EVENT_CODE_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KEY_EVENT_J, value);
}

esp_err_t tca8418_get_kp_lck_timer(tca8418_handle_t* handle, uint8_t* int_mask_timer, uint8_t* lock_timer) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KP_LCK_TIMER, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (int_mask_timer != NULL) {
        *int_mask_timer = (uint8_t)((value & TCA8418_REG_KP_LCK_TIMER_INT_MASK_TIMER_MSK) >>
                                    TCA8418_REG_KP_LCK_TIMER_INT_MASK_TIMER_POS);
    }
    if (lock_timer != NULL) {
        *lock_timer =
            (uint8_t)((value & TCA8418_REG_KP_LCK_TIMER_LOCK_TIMER_MSK) >> TCA8418_REG_KP_LCK_TIMER_LOCK_TIMER_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_kp_lck_timer(tca8418_handle_t* handle, uint8_t int_mask_timer, uint8_t lock_timer) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value = 0;
    value |=
        (int_mask_timer << TCA8418_REG_KP_LCK_TIMER_INT_MASK_TIMER_POS) & TCA8418_REG_KP_LCK_TIMER_INT_MASK_TIMER_MSK;
    value |= (lock_timer << TCA8418_REG_KP_LCK_TIMER_LOCK_TIMER_POS) & TCA8418_REG_KP_LCK_TIMER_LOCK_TIMER_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_KP_LCK_TIMER, value);
}

esp_err_t tca8418_get_unlock1(tca8418_handle_t* handle, uint8_t* uk1) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_UNLOCK1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (uk1 != NULL) {
        *uk1 = (uint8_t)((value & TCA8418_REG_UNLOCK1_UK1_MSK) >> TCA8418_REG_UNLOCK1_UK1_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_unlock1(tca8418_handle_t* handle, uint8_t uk1) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= (uk1 << TCA8418_REG_UNLOCK1_UK1_POS) & TCA8418_REG_UNLOCK1_UK1_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_UNLOCK1, value);
}

esp_err_t tca8418_get_unlock2(tca8418_handle_t* handle, uint8_t* uk2) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_UNLOCK2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (uk2 != NULL) {
        *uk2 = (uint8_t)((value & TCA8418_REG_UNLOCK2_UK2_MSK) >> TCA8418_REG_UNLOCK2_UK2_POS);
    }
    return ESP_OK;
}

esp_err_t tca8418_set_unlock2(tca8418_handle_t* handle, uint8_t uk2) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= (uk2 << TCA8418_REG_UNLOCK2_UK2_POS) & TCA8418_REG_UNLOCK2_UK2_MSK;
    return tca8418_write_reg(handle, TCA8418_REG_UNLOCK2, value);
}

esp_err_t tca8418_get_gpio_int_stat1(tca8418_handle_t* handle, bool* r7is, bool* r6is, bool* r5is, bool* r4is,
                                     bool* r3is, bool* r2is, bool* r1is, bool* r0is) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_STAT1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (r7is != NULL) {
        *r7is = (value & TCA8418_REG_GPIO_INT_STAT1_R7IS_MSK) != 0;
    }
    if (r6is != NULL) {
        *r6is = (value & TCA8418_REG_GPIO_INT_STAT1_R6IS_MSK) != 0;
    }
    if (r5is != NULL) {
        *r5is = (value & TCA8418_REG_GPIO_INT_STAT1_R5IS_MSK) != 0;
    }
    if (r4is != NULL) {
        *r4is = (value & TCA8418_REG_GPIO_INT_STAT1_R4IS_MSK) != 0;
    }
    if (r3is != NULL) {
        *r3is = (value & TCA8418_REG_GPIO_INT_STAT1_R3IS_MSK) != 0;
    }
    if (r2is != NULL) {
        *r2is = (value & TCA8418_REG_GPIO_INT_STAT1_R2IS_MSK) != 0;
    }
    if (r1is != NULL) {
        *r1is = (value & TCA8418_REG_GPIO_INT_STAT1_R1IS_MSK) != 0;
    }
    if (r0is != NULL) {
        *r0is = (value & TCA8418_REG_GPIO_INT_STAT1_R0IS_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_stat1(tca8418_handle_t* handle, bool r7is, bool r6is, bool r5is, bool r4is, bool r3is,
                                     bool r2is, bool r1is, bool r0is) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= r7is ? TCA8418_REG_GPIO_INT_STAT1_R7IS_MSK : 0;
    value         |= r6is ? TCA8418_REG_GPIO_INT_STAT1_R6IS_MSK : 0;
    value         |= r5is ? TCA8418_REG_GPIO_INT_STAT1_R5IS_MSK : 0;
    value         |= r4is ? TCA8418_REG_GPIO_INT_STAT1_R4IS_MSK : 0;
    value         |= r3is ? TCA8418_REG_GPIO_INT_STAT1_R3IS_MSK : 0;
    value         |= r2is ? TCA8418_REG_GPIO_INT_STAT1_R2IS_MSK : 0;
    value         |= r1is ? TCA8418_REG_GPIO_INT_STAT1_R1IS_MSK : 0;
    value         |= r0is ? TCA8418_REG_GPIO_INT_STAT1_R0IS_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_STAT1, value);
}

esp_err_t tca8418_get_gpio_int_stat2(tca8418_handle_t* handle, bool* c7is, bool* c6is, bool* c5is, bool* c4is,
                                     bool* c3is, bool* c2is, bool* c1is, bool* c0is) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_STAT2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c7is != NULL) {
        *c7is = (value & TCA8418_REG_GPIO_INT_STAT2_C7IS_MSK) != 0;
    }
    if (c6is != NULL) {
        *c6is = (value & TCA8418_REG_GPIO_INT_STAT2_C6IS_MSK) != 0;
    }
    if (c5is != NULL) {
        *c5is = (value & TCA8418_REG_GPIO_INT_STAT2_C5IS_MSK) != 0;
    }
    if (c4is != NULL) {
        *c4is = (value & TCA8418_REG_GPIO_INT_STAT2_C4IS_MSK) != 0;
    }
    if (c3is != NULL) {
        *c3is = (value & TCA8418_REG_GPIO_INT_STAT2_C3IS_MSK) != 0;
    }
    if (c2is != NULL) {
        *c2is = (value & TCA8418_REG_GPIO_INT_STAT2_C2IS_MSK) != 0;
    }
    if (c1is != NULL) {
        *c1is = (value & TCA8418_REG_GPIO_INT_STAT2_C1IS_MSK) != 0;
    }
    if (c0is != NULL) {
        *c0is = (value & TCA8418_REG_GPIO_INT_STAT2_C0IS_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_stat2(tca8418_handle_t* handle, bool c7is, bool c6is, bool c5is, bool c4is, bool c3is,
                                     bool c2is, bool c1is, bool c0is) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c7is ? TCA8418_REG_GPIO_INT_STAT2_C7IS_MSK : 0;
    value         |= c6is ? TCA8418_REG_GPIO_INT_STAT2_C6IS_MSK : 0;
    value         |= c5is ? TCA8418_REG_GPIO_INT_STAT2_C5IS_MSK : 0;
    value         |= c4is ? TCA8418_REG_GPIO_INT_STAT2_C4IS_MSK : 0;
    value         |= c3is ? TCA8418_REG_GPIO_INT_STAT2_C3IS_MSK : 0;
    value         |= c2is ? TCA8418_REG_GPIO_INT_STAT2_C2IS_MSK : 0;
    value         |= c1is ? TCA8418_REG_GPIO_INT_STAT2_C1IS_MSK : 0;
    value         |= c0is ? TCA8418_REG_GPIO_INT_STAT2_C0IS_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_STAT2, value);
}

esp_err_t tca8418_get_gpio_int_stat3(tca8418_handle_t* handle, bool* c9is, bool* c8is) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_STAT3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c9is != NULL) {
        *c9is = (value & TCA8418_REG_GPIO_INT_STAT3_C9IS_MSK) != 0;
    }
    if (c8is != NULL) {
        *c8is = (value & TCA8418_REG_GPIO_INT_STAT3_C8IS_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_stat3(tca8418_handle_t* handle, bool c9is, bool c8is) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c9is ? TCA8418_REG_GPIO_INT_STAT3_C9IS_MSK : 0;
    value         |= c8is ? TCA8418_REG_GPIO_INT_STAT3_C8IS_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_STAT3, value);
}

esp_err_t tca8418_get_gpio_dat_stat1(tca8418_handle_t* handle, bool* r7ds, bool* r6ds, bool* r5ds, bool* r4ds,
                                     bool* r3ds, bool* r2ds, bool* r1ds, bool* r0ds) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DAT_STAT1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (r7ds != NULL) {
        *r7ds = (value & TCA8418_REG_GPIO_DAT_STAT1_R7DS_MSK) != 0;
    }
    if (r6ds != NULL) {
        *r6ds = (value & TCA8418_REG_GPIO_DAT_STAT1_R6DS_MSK) != 0;
    }
    if (r5ds != NULL) {
        *r5ds = (value & TCA8418_REG_GPIO_DAT_STAT1_R5DS_MSK) != 0;
    }
    if (r4ds != NULL) {
        *r4ds = (value & TCA8418_REG_GPIO_DAT_STAT1_R4DS_MSK) != 0;
    }
    if (r3ds != NULL) {
        *r3ds = (value & TCA8418_REG_GPIO_DAT_STAT1_R3DS_MSK) != 0;
    }
    if (r2ds != NULL) {
        *r2ds = (value & TCA8418_REG_GPIO_DAT_STAT1_R2DS_MSK) != 0;
    }
    if (r1ds != NULL) {
        *r1ds = (value & TCA8418_REG_GPIO_DAT_STAT1_R1DS_MSK) != 0;
    }
    if (r0ds != NULL) {
        *r0ds = (value & TCA8418_REG_GPIO_DAT_STAT1_R0DS_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dat_stat1(tca8418_handle_t* handle, bool r7ds, bool r6ds, bool r5ds, bool r4ds, bool r3ds,
                                     bool r2ds, bool r1ds, bool r0ds) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= r7ds ? TCA8418_REG_GPIO_DAT_STAT1_R7DS_MSK : 0;
    value         |= r6ds ? TCA8418_REG_GPIO_DAT_STAT1_R6DS_MSK : 0;
    value         |= r5ds ? TCA8418_REG_GPIO_DAT_STAT1_R5DS_MSK : 0;
    value         |= r4ds ? TCA8418_REG_GPIO_DAT_STAT1_R4DS_MSK : 0;
    value         |= r3ds ? TCA8418_REG_GPIO_DAT_STAT1_R3DS_MSK : 0;
    value         |= r2ds ? TCA8418_REG_GPIO_DAT_STAT1_R2DS_MSK : 0;
    value         |= r1ds ? TCA8418_REG_GPIO_DAT_STAT1_R1DS_MSK : 0;
    value         |= r0ds ? TCA8418_REG_GPIO_DAT_STAT1_R0DS_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DAT_STAT1, value);
}

esp_err_t tca8418_get_gpio_dat_stat2(tca8418_handle_t* handle, bool* c7ds, bool* c6ds, bool* c5ds, bool* c4ds,
                                     bool* c3ds, bool* c2ds, bool* c1ds, bool* c0ds) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DAT_STAT2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c7ds != NULL) {
        *c7ds = (value & TCA8418_REG_GPIO_DAT_STAT2_C7DS_MSK) != 0;
    }
    if (c6ds != NULL) {
        *c6ds = (value & TCA8418_REG_GPIO_DAT_STAT2_C6DS_MSK) != 0;
    }
    if (c5ds != NULL) {
        *c5ds = (value & TCA8418_REG_GPIO_DAT_STAT2_C5DS_MSK) != 0;
    }
    if (c4ds != NULL) {
        *c4ds = (value & TCA8418_REG_GPIO_DAT_STAT2_C4DS_MSK) != 0;
    }
    if (c3ds != NULL) {
        *c3ds = (value & TCA8418_REG_GPIO_DAT_STAT2_C3DS_MSK) != 0;
    }
    if (c2ds != NULL) {
        *c2ds = (value & TCA8418_REG_GPIO_DAT_STAT2_C2DS_MSK) != 0;
    }
    if (c1ds != NULL) {
        *c1ds = (value & TCA8418_REG_GPIO_DAT_STAT2_C1DS_MSK) != 0;
    }
    if (c0ds != NULL) {
        *c0ds = (value & TCA8418_REG_GPIO_DAT_STAT2_C0DS_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dat_stat2(tca8418_handle_t* handle, bool c7ds, bool c6ds, bool c5ds, bool c4ds, bool c3ds,
                                     bool c2ds, bool c1ds, bool c0ds) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c7ds ? TCA8418_REG_GPIO_DAT_STAT2_C7DS_MSK : 0;
    value         |= c6ds ? TCA8418_REG_GPIO_DAT_STAT2_C6DS_MSK : 0;
    value         |= c5ds ? TCA8418_REG_GPIO_DAT_STAT2_C5DS_MSK : 0;
    value         |= c4ds ? TCA8418_REG_GPIO_DAT_STAT2_C4DS_MSK : 0;
    value         |= c3ds ? TCA8418_REG_GPIO_DAT_STAT2_C3DS_MSK : 0;
    value         |= c2ds ? TCA8418_REG_GPIO_DAT_STAT2_C2DS_MSK : 0;
    value         |= c1ds ? TCA8418_REG_GPIO_DAT_STAT2_C1DS_MSK : 0;
    value         |= c0ds ? TCA8418_REG_GPIO_DAT_STAT2_C0DS_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DAT_STAT2, value);
}

esp_err_t tca8418_get_gpio_dat_stat3(tca8418_handle_t* handle, bool* c9ds, bool* c8ds) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DAT_STAT3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c9ds != NULL) {
        *c9ds = (value & TCA8418_REG_GPIO_DAT_STAT3_C9DS_MSK) != 0;
    }
    if (c8ds != NULL) {
        *c8ds = (value & TCA8418_REG_GPIO_DAT_STAT3_C8DS_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dat_stat3(tca8418_handle_t* handle, bool c9ds, bool c8ds) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c9ds ? TCA8418_REG_GPIO_DAT_STAT3_C9DS_MSK : 0;
    value         |= c8ds ? TCA8418_REG_GPIO_DAT_STAT3_C8DS_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DAT_STAT3, value);
}

esp_err_t tca8418_get_gpio_dat_out1(tca8418_handle_t* handle, bool* r7do, bool* r6do, bool* r5do, bool* r4do,
                                    bool* r3do, bool* r2do, bool* r1do, bool* r0do) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DAT_OUT1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (r7do != NULL) {
        *r7do = (value & TCA8418_REG_GPIO_DAT_OUT1_R7DO_MSK) != 0;
    }
    if (r6do != NULL) {
        *r6do = (value & TCA8418_REG_GPIO_DAT_OUT1_R6DO_MSK) != 0;
    }
    if (r5do != NULL) {
        *r5do = (value & TCA8418_REG_GPIO_DAT_OUT1_R5DO_MSK) != 0;
    }
    if (r4do != NULL) {
        *r4do = (value & TCA8418_REG_GPIO_DAT_OUT1_R4DO_MSK) != 0;
    }
    if (r3do != NULL) {
        *r3do = (value & TCA8418_REG_GPIO_DAT_OUT1_R3DO_MSK) != 0;
    }
    if (r2do != NULL) {
        *r2do = (value & TCA8418_REG_GPIO_DAT_OUT1_R2DO_MSK) != 0;
    }
    if (r1do != NULL) {
        *r1do = (value & TCA8418_REG_GPIO_DAT_OUT1_R1DO_MSK) != 0;
    }
    if (r0do != NULL) {
        *r0do = (value & TCA8418_REG_GPIO_DAT_OUT1_R0DO_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dat_out1(tca8418_handle_t* handle, bool r7do, bool r6do, bool r5do, bool r4do, bool r3do,
                                    bool r2do, bool r1do, bool r0do) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= r7do ? TCA8418_REG_GPIO_DAT_OUT1_R7DO_MSK : 0;
    value         |= r6do ? TCA8418_REG_GPIO_DAT_OUT1_R6DO_MSK : 0;
    value         |= r5do ? TCA8418_REG_GPIO_DAT_OUT1_R5DO_MSK : 0;
    value         |= r4do ? TCA8418_REG_GPIO_DAT_OUT1_R4DO_MSK : 0;
    value         |= r3do ? TCA8418_REG_GPIO_DAT_OUT1_R3DO_MSK : 0;
    value         |= r2do ? TCA8418_REG_GPIO_DAT_OUT1_R2DO_MSK : 0;
    value         |= r1do ? TCA8418_REG_GPIO_DAT_OUT1_R1DO_MSK : 0;
    value         |= r0do ? TCA8418_REG_GPIO_DAT_OUT1_R0DO_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DAT_OUT1, value);
}

esp_err_t tca8418_get_gpio_dat_out2(tca8418_handle_t* handle, bool* c7do, bool* c6do, bool* c5do, bool* c4do,
                                    bool* c3do, bool* c2do, bool* c1do, bool* c0do) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DAT_OUT2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c7do != NULL) {
        *c7do = (value & TCA8418_REG_GPIO_DAT_OUT2_C7DO_MSK) != 0;
    }
    if (c6do != NULL) {
        *c6do = (value & TCA8418_REG_GPIO_DAT_OUT2_C6DO_MSK) != 0;
    }
    if (c5do != NULL) {
        *c5do = (value & TCA8418_REG_GPIO_DAT_OUT2_C5DO_MSK) != 0;
    }
    if (c4do != NULL) {
        *c4do = (value & TCA8418_REG_GPIO_DAT_OUT2_C4DO_MSK) != 0;
    }
    if (c3do != NULL) {
        *c3do = (value & TCA8418_REG_GPIO_DAT_OUT2_C3DO_MSK) != 0;
    }
    if (c2do != NULL) {
        *c2do = (value & TCA8418_REG_GPIO_DAT_OUT2_C2DO_MSK) != 0;
    }
    if (c1do != NULL) {
        *c1do = (value & TCA8418_REG_GPIO_DAT_OUT2_C1DO_MSK) != 0;
    }
    if (c0do != NULL) {
        *c0do = (value & TCA8418_REG_GPIO_DAT_OUT2_C0DO_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dat_out2(tca8418_handle_t* handle, bool c7do, bool c6do, bool c5do, bool c4do, bool c3do,
                                    bool c2do, bool c1do, bool c0do) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c7do ? TCA8418_REG_GPIO_DAT_OUT2_C7DO_MSK : 0;
    value         |= c6do ? TCA8418_REG_GPIO_DAT_OUT2_C6DO_MSK : 0;
    value         |= c5do ? TCA8418_REG_GPIO_DAT_OUT2_C5DO_MSK : 0;
    value         |= c4do ? TCA8418_REG_GPIO_DAT_OUT2_C4DO_MSK : 0;
    value         |= c3do ? TCA8418_REG_GPIO_DAT_OUT2_C3DO_MSK : 0;
    value         |= c2do ? TCA8418_REG_GPIO_DAT_OUT2_C2DO_MSK : 0;
    value         |= c1do ? TCA8418_REG_GPIO_DAT_OUT2_C1DO_MSK : 0;
    value         |= c0do ? TCA8418_REG_GPIO_DAT_OUT2_C0DO_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DAT_OUT2, value);
}

esp_err_t tca8418_get_gpio_dat_out3(tca8418_handle_t* handle, bool* c9do, bool* c8do) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DAT_OUT3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c9do != NULL) {
        *c9do = (value & TCA8418_REG_GPIO_DAT_OUT3_C9DO_MSK) != 0;
    }
    if (c8do != NULL) {
        *c8do = (value & TCA8418_REG_GPIO_DAT_OUT3_C8DO_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dat_out3(tca8418_handle_t* handle, bool c9do, bool c8do) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c9do ? TCA8418_REG_GPIO_DAT_OUT3_C9DO_MSK : 0;
    value         |= c8do ? TCA8418_REG_GPIO_DAT_OUT3_C8DO_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DAT_OUT3, value);
}

esp_err_t tca8418_get_gpio_int_en1(tca8418_handle_t* handle, bool* r7ie, bool* r6ie, bool* r5ie, bool* r4ie, bool* r3ie,
                                   bool* r2ie, bool* r1ie, bool* r0ie) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_EN1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (r7ie != NULL) {
        *r7ie = (value & TCA8418_REG_GPIO_INT_EN1_R7IE_MSK) != 0;
    }
    if (r6ie != NULL) {
        *r6ie = (value & TCA8418_REG_GPIO_INT_EN1_R6IE_MSK) != 0;
    }
    if (r5ie != NULL) {
        *r5ie = (value & TCA8418_REG_GPIO_INT_EN1_R5IE_MSK) != 0;
    }
    if (r4ie != NULL) {
        *r4ie = (value & TCA8418_REG_GPIO_INT_EN1_R4IE_MSK) != 0;
    }
    if (r3ie != NULL) {
        *r3ie = (value & TCA8418_REG_GPIO_INT_EN1_R3IE_MSK) != 0;
    }
    if (r2ie != NULL) {
        *r2ie = (value & TCA8418_REG_GPIO_INT_EN1_R2IE_MSK) != 0;
    }
    if (r1ie != NULL) {
        *r1ie = (value & TCA8418_REG_GPIO_INT_EN1_R1IE_MSK) != 0;
    }
    if (r0ie != NULL) {
        *r0ie = (value & TCA8418_REG_GPIO_INT_EN1_R0IE_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_en1(tca8418_handle_t* handle, bool r7ie, bool r6ie, bool r5ie, bool r4ie, bool r3ie,
                                   bool r2ie, bool r1ie, bool r0ie) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= r7ie ? TCA8418_REG_GPIO_INT_EN1_R7IE_MSK : 0;
    value         |= r6ie ? TCA8418_REG_GPIO_INT_EN1_R6IE_MSK : 0;
    value         |= r5ie ? TCA8418_REG_GPIO_INT_EN1_R5IE_MSK : 0;
    value         |= r4ie ? TCA8418_REG_GPIO_INT_EN1_R4IE_MSK : 0;
    value         |= r3ie ? TCA8418_REG_GPIO_INT_EN1_R3IE_MSK : 0;
    value         |= r2ie ? TCA8418_REG_GPIO_INT_EN1_R2IE_MSK : 0;
    value         |= r1ie ? TCA8418_REG_GPIO_INT_EN1_R1IE_MSK : 0;
    value         |= r0ie ? TCA8418_REG_GPIO_INT_EN1_R0IE_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_EN1, value);
}

esp_err_t tca8418_get_gpio_int_en2(tca8418_handle_t* handle, bool* c7ie, bool* c6ie, bool* c5ie, bool* c4ie, bool* c3ie,
                                   bool* c2ie, bool* c1ie, bool* c0ie) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_EN2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c7ie != NULL) {
        *c7ie = (value & TCA8418_REG_GPIO_INT_EN2_C7IE_MSK) != 0;
    }
    if (c6ie != NULL) {
        *c6ie = (value & TCA8418_REG_GPIO_INT_EN2_C6IE_MSK) != 0;
    }
    if (c5ie != NULL) {
        *c5ie = (value & TCA8418_REG_GPIO_INT_EN2_C5IE_MSK) != 0;
    }
    if (c4ie != NULL) {
        *c4ie = (value & TCA8418_REG_GPIO_INT_EN2_C4IE_MSK) != 0;
    }
    if (c3ie != NULL) {
        *c3ie = (value & TCA8418_REG_GPIO_INT_EN2_C3IE_MSK) != 0;
    }
    if (c2ie != NULL) {
        *c2ie = (value & TCA8418_REG_GPIO_INT_EN2_C2IE_MSK) != 0;
    }
    if (c1ie != NULL) {
        *c1ie = (value & TCA8418_REG_GPIO_INT_EN2_C1IE_MSK) != 0;
    }
    if (c0ie != NULL) {
        *c0ie = (value & TCA8418_REG_GPIO_INT_EN2_C0IE_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_en2(tca8418_handle_t* handle, bool c7ie, bool c6ie, bool c5ie, bool c4ie, bool c3ie,
                                   bool c2ie, bool c1ie, bool c0ie) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c7ie ? TCA8418_REG_GPIO_INT_EN2_C7IE_MSK : 0;
    value         |= c6ie ? TCA8418_REG_GPIO_INT_EN2_C6IE_MSK : 0;
    value         |= c5ie ? TCA8418_REG_GPIO_INT_EN2_C5IE_MSK : 0;
    value         |= c4ie ? TCA8418_REG_GPIO_INT_EN2_C4IE_MSK : 0;
    value         |= c3ie ? TCA8418_REG_GPIO_INT_EN2_C3IE_MSK : 0;
    value         |= c2ie ? TCA8418_REG_GPIO_INT_EN2_C2IE_MSK : 0;
    value         |= c1ie ? TCA8418_REG_GPIO_INT_EN2_C1IE_MSK : 0;
    value         |= c0ie ? TCA8418_REG_GPIO_INT_EN2_C0IE_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_EN2, value);
}

esp_err_t tca8418_get_gpio_int_en3(tca8418_handle_t* handle, bool* c9ie, bool* c8ie) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_EN3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c9ie != NULL) {
        *c9ie = (value & TCA8418_REG_GPIO_INT_EN3_C9IE_MSK) != 0;
    }
    if (c8ie != NULL) {
        *c8ie = (value & TCA8418_REG_GPIO_INT_EN3_C8IE_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_en3(tca8418_handle_t* handle, bool c9ie, bool c8ie) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c9ie ? TCA8418_REG_GPIO_INT_EN3_C9IE_MSK : 0;
    value         |= c8ie ? TCA8418_REG_GPIO_INT_EN3_C8IE_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_EN3, value);
}

esp_err_t tca8418_get_kp_gpio1(tca8418_handle_t* handle, bool* row7, bool* row6, bool* row5, bool* row4, bool* row3,
                               bool* row2, bool* row1, bool* row0) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KP_GPIO1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (row7 != NULL) {
        *row7 = (value & TCA8418_REG_KP_GPIO1_ROW7_MSK) != 0;
    }
    if (row6 != NULL) {
        *row6 = (value & TCA8418_REG_KP_GPIO1_ROW6_MSK) != 0;
    }
    if (row5 != NULL) {
        *row5 = (value & TCA8418_REG_KP_GPIO1_ROW5_MSK) != 0;
    }
    if (row4 != NULL) {
        *row4 = (value & TCA8418_REG_KP_GPIO1_ROW4_MSK) != 0;
    }
    if (row3 != NULL) {
        *row3 = (value & TCA8418_REG_KP_GPIO1_ROW3_MSK) != 0;
    }
    if (row2 != NULL) {
        *row2 = (value & TCA8418_REG_KP_GPIO1_ROW2_MSK) != 0;
    }
    if (row1 != NULL) {
        *row1 = (value & TCA8418_REG_KP_GPIO1_ROW1_MSK) != 0;
    }
    if (row0 != NULL) {
        *row0 = (value & TCA8418_REG_KP_GPIO1_ROW0_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_kp_gpio1(tca8418_handle_t* handle, bool row7, bool row6, bool row5, bool row4, bool row3,
                               bool row2, bool row1, bool row0) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= row7 ? TCA8418_REG_KP_GPIO1_ROW7_MSK : 0;
    value         |= row6 ? TCA8418_REG_KP_GPIO1_ROW6_MSK : 0;
    value         |= row5 ? TCA8418_REG_KP_GPIO1_ROW5_MSK : 0;
    value         |= row4 ? TCA8418_REG_KP_GPIO1_ROW4_MSK : 0;
    value         |= row3 ? TCA8418_REG_KP_GPIO1_ROW3_MSK : 0;
    value         |= row2 ? TCA8418_REG_KP_GPIO1_ROW2_MSK : 0;
    value         |= row1 ? TCA8418_REG_KP_GPIO1_ROW1_MSK : 0;
    value         |= row0 ? TCA8418_REG_KP_GPIO1_ROW0_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_KP_GPIO1, value);
}

esp_err_t tca8418_get_kp_gpio2(tca8418_handle_t* handle, bool* col7, bool* col6, bool* col5, bool* col4, bool* col3,
                               bool* col2, bool* col1, bool* col0) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KP_GPIO2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (col7 != NULL) {
        *col7 = (value & TCA8418_REG_KP_GPIO2_COL7_MSK) != 0;
    }
    if (col6 != NULL) {
        *col6 = (value & TCA8418_REG_KP_GPIO2_COL6_MSK) != 0;
    }
    if (col5 != NULL) {
        *col5 = (value & TCA8418_REG_KP_GPIO2_COL5_MSK) != 0;
    }
    if (col4 != NULL) {
        *col4 = (value & TCA8418_REG_KP_GPIO2_COL4_MSK) != 0;
    }
    if (col3 != NULL) {
        *col3 = (value & TCA8418_REG_KP_GPIO2_COL3_MSK) != 0;
    }
    if (col2 != NULL) {
        *col2 = (value & TCA8418_REG_KP_GPIO2_COL2_MSK) != 0;
    }
    if (col1 != NULL) {
        *col1 = (value & TCA8418_REG_KP_GPIO2_COL1_MSK) != 0;
    }
    if (col0 != NULL) {
        *col0 = (value & TCA8418_REG_KP_GPIO2_COL0_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_kp_gpio2(tca8418_handle_t* handle, bool col7, bool col6, bool col5, bool col4, bool col3,
                               bool col2, bool col1, bool col0) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= col7 ? TCA8418_REG_KP_GPIO2_COL7_MSK : 0;
    value         |= col6 ? TCA8418_REG_KP_GPIO2_COL6_MSK : 0;
    value         |= col5 ? TCA8418_REG_KP_GPIO2_COL5_MSK : 0;
    value         |= col4 ? TCA8418_REG_KP_GPIO2_COL4_MSK : 0;
    value         |= col3 ? TCA8418_REG_KP_GPIO2_COL3_MSK : 0;
    value         |= col2 ? TCA8418_REG_KP_GPIO2_COL2_MSK : 0;
    value         |= col1 ? TCA8418_REG_KP_GPIO2_COL1_MSK : 0;
    value         |= col0 ? TCA8418_REG_KP_GPIO2_COL0_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_KP_GPIO2, value);
}

esp_err_t tca8418_get_kp_gpio3(tca8418_handle_t* handle, bool* col9, bool* col8) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_KP_GPIO3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (col9 != NULL) {
        *col9 = (value & TCA8418_REG_KP_GPIO3_COL9_MSK) != 0;
    }
    if (col8 != NULL) {
        *col8 = (value & TCA8418_REG_KP_GPIO3_COL8_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_kp_gpio3(tca8418_handle_t* handle, bool col9, bool col8) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= col9 ? TCA8418_REG_KP_GPIO3_COL9_MSK : 0;
    value         |= col8 ? TCA8418_REG_KP_GPIO3_COL8_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_KP_GPIO3, value);
}

esp_err_t tca8418_get_gpi_em1(tca8418_handle_t* handle, bool* row7, bool* row6, bool* row5, bool* row4, bool* row3,
                              bool* row2, bool* row1, bool* row0) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPI_EM1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (row7 != NULL) {
        *row7 = (value & TCA8418_REG_GPI_EM1_ROW7_MSK) != 0;
    }
    if (row6 != NULL) {
        *row6 = (value & TCA8418_REG_GPI_EM1_ROW6_MSK) != 0;
    }
    if (row5 != NULL) {
        *row5 = (value & TCA8418_REG_GPI_EM1_ROW5_MSK) != 0;
    }
    if (row4 != NULL) {
        *row4 = (value & TCA8418_REG_GPI_EM1_ROW4_MSK) != 0;
    }
    if (row3 != NULL) {
        *row3 = (value & TCA8418_REG_GPI_EM1_ROW3_MSK) != 0;
    }
    if (row2 != NULL) {
        *row2 = (value & TCA8418_REG_GPI_EM1_ROW2_MSK) != 0;
    }
    if (row1 != NULL) {
        *row1 = (value & TCA8418_REG_GPI_EM1_ROW1_MSK) != 0;
    }
    if (row0 != NULL) {
        *row0 = (value & TCA8418_REG_GPI_EM1_ROW0_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpi_em1(tca8418_handle_t* handle, bool row7, bool row6, bool row5, bool row4, bool row3,
                              bool row2, bool row1, bool row0) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= row7 ? TCA8418_REG_GPI_EM1_ROW7_MSK : 0;
    value         |= row6 ? TCA8418_REG_GPI_EM1_ROW6_MSK : 0;
    value         |= row5 ? TCA8418_REG_GPI_EM1_ROW5_MSK : 0;
    value         |= row4 ? TCA8418_REG_GPI_EM1_ROW4_MSK : 0;
    value         |= row3 ? TCA8418_REG_GPI_EM1_ROW3_MSK : 0;
    value         |= row2 ? TCA8418_REG_GPI_EM1_ROW2_MSK : 0;
    value         |= row1 ? TCA8418_REG_GPI_EM1_ROW1_MSK : 0;
    value         |= row0 ? TCA8418_REG_GPI_EM1_ROW0_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPI_EM1, value);
}

esp_err_t tca8418_get_gpi_em2(tca8418_handle_t* handle, bool* col7, bool* col6, bool* col5, bool* col4, bool* col3,
                              bool* col2, bool* col1, bool* col0) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPI_EM2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (col7 != NULL) {
        *col7 = (value & TCA8418_REG_GPI_EM2_COL7_MSK) != 0;
    }
    if (col6 != NULL) {
        *col6 = (value & TCA8418_REG_GPI_EM2_COL6_MSK) != 0;
    }
    if (col5 != NULL) {
        *col5 = (value & TCA8418_REG_GPI_EM2_COL5_MSK) != 0;
    }
    if (col4 != NULL) {
        *col4 = (value & TCA8418_REG_GPI_EM2_COL4_MSK) != 0;
    }
    if (col3 != NULL) {
        *col3 = (value & TCA8418_REG_GPI_EM2_COL3_MSK) != 0;
    }
    if (col2 != NULL) {
        *col2 = (value & TCA8418_REG_GPI_EM2_COL2_MSK) != 0;
    }
    if (col1 != NULL) {
        *col1 = (value & TCA8418_REG_GPI_EM2_COL1_MSK) != 0;
    }
    if (col0 != NULL) {
        *col0 = (value & TCA8418_REG_GPI_EM2_COL0_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpi_em2(tca8418_handle_t* handle, bool col7, bool col6, bool col5, bool col4, bool col3,
                              bool col2, bool col1, bool col0) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= col7 ? TCA8418_REG_GPI_EM2_COL7_MSK : 0;
    value         |= col6 ? TCA8418_REG_GPI_EM2_COL6_MSK : 0;
    value         |= col5 ? TCA8418_REG_GPI_EM2_COL5_MSK : 0;
    value         |= col4 ? TCA8418_REG_GPI_EM2_COL4_MSK : 0;
    value         |= col3 ? TCA8418_REG_GPI_EM2_COL3_MSK : 0;
    value         |= col2 ? TCA8418_REG_GPI_EM2_COL2_MSK : 0;
    value         |= col1 ? TCA8418_REG_GPI_EM2_COL1_MSK : 0;
    value         |= col0 ? TCA8418_REG_GPI_EM2_COL0_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPI_EM2, value);
}

esp_err_t tca8418_get_gpi_em3(tca8418_handle_t* handle, bool* col9, bool* col8) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPI_EM3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (col9 != NULL) {
        *col9 = (value & TCA8418_REG_GPI_EM3_COL9_MSK) != 0;
    }
    if (col8 != NULL) {
        *col8 = (value & TCA8418_REG_GPI_EM3_COL8_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpi_em3(tca8418_handle_t* handle, bool col9, bool col8) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= col9 ? TCA8418_REG_GPI_EM3_COL9_MSK : 0;
    value         |= col8 ? TCA8418_REG_GPI_EM3_COL8_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPI_EM3, value);
}

esp_err_t tca8418_get_gpio_dir1(tca8418_handle_t* handle, bool* r7dd, bool* r6dd, bool* r5dd, bool* r4dd, bool* r3dd,
                                bool* r2dd, bool* r1dd, bool* r0dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DIR1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (r7dd != NULL) {
        *r7dd = (value & TCA8418_REG_GPIO_DIR1_R7DD_MSK) != 0;
    }
    if (r6dd != NULL) {
        *r6dd = (value & TCA8418_REG_GPIO_DIR1_R6DD_MSK) != 0;
    }
    if (r5dd != NULL) {
        *r5dd = (value & TCA8418_REG_GPIO_DIR1_R5DD_MSK) != 0;
    }
    if (r4dd != NULL) {
        *r4dd = (value & TCA8418_REG_GPIO_DIR1_R4DD_MSK) != 0;
    }
    if (r3dd != NULL) {
        *r3dd = (value & TCA8418_REG_GPIO_DIR1_R3DD_MSK) != 0;
    }
    if (r2dd != NULL) {
        *r2dd = (value & TCA8418_REG_GPIO_DIR1_R2DD_MSK) != 0;
    }
    if (r1dd != NULL) {
        *r1dd = (value & TCA8418_REG_GPIO_DIR1_R1DD_MSK) != 0;
    }
    if (r0dd != NULL) {
        *r0dd = (value & TCA8418_REG_GPIO_DIR1_R0DD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dir1(tca8418_handle_t* handle, bool r7dd, bool r6dd, bool r5dd, bool r4dd, bool r3dd,
                                bool r2dd, bool r1dd, bool r0dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= r7dd ? TCA8418_REG_GPIO_DIR1_R7DD_MSK : 0;
    value         |= r6dd ? TCA8418_REG_GPIO_DIR1_R6DD_MSK : 0;
    value         |= r5dd ? TCA8418_REG_GPIO_DIR1_R5DD_MSK : 0;
    value         |= r4dd ? TCA8418_REG_GPIO_DIR1_R4DD_MSK : 0;
    value         |= r3dd ? TCA8418_REG_GPIO_DIR1_R3DD_MSK : 0;
    value         |= r2dd ? TCA8418_REG_GPIO_DIR1_R2DD_MSK : 0;
    value         |= r1dd ? TCA8418_REG_GPIO_DIR1_R1DD_MSK : 0;
    value         |= r0dd ? TCA8418_REG_GPIO_DIR1_R0DD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DIR1, value);
}

esp_err_t tca8418_get_gpio_dir2(tca8418_handle_t* handle, bool* c7dd, bool* c6dd, bool* c5dd, bool* c4dd, bool* c3dd,
                                bool* c2dd, bool* c1dd, bool* c0dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DIR2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c7dd != NULL) {
        *c7dd = (value & TCA8418_REG_GPIO_DIR2_C7DD_MSK) != 0;
    }
    if (c6dd != NULL) {
        *c6dd = (value & TCA8418_REG_GPIO_DIR2_C6DD_MSK) != 0;
    }
    if (c5dd != NULL) {
        *c5dd = (value & TCA8418_REG_GPIO_DIR2_C5DD_MSK) != 0;
    }
    if (c4dd != NULL) {
        *c4dd = (value & TCA8418_REG_GPIO_DIR2_C4DD_MSK) != 0;
    }
    if (c3dd != NULL) {
        *c3dd = (value & TCA8418_REG_GPIO_DIR2_C3DD_MSK) != 0;
    }
    if (c2dd != NULL) {
        *c2dd = (value & TCA8418_REG_GPIO_DIR2_C2DD_MSK) != 0;
    }
    if (c1dd != NULL) {
        *c1dd = (value & TCA8418_REG_GPIO_DIR2_C1DD_MSK) != 0;
    }
    if (c0dd != NULL) {
        *c0dd = (value & TCA8418_REG_GPIO_DIR2_C0DD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dir2(tca8418_handle_t* handle, bool c7dd, bool c6dd, bool c5dd, bool c4dd, bool c3dd,
                                bool c2dd, bool c1dd, bool c0dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c7dd ? TCA8418_REG_GPIO_DIR2_C7DD_MSK : 0;
    value         |= c6dd ? TCA8418_REG_GPIO_DIR2_C6DD_MSK : 0;
    value         |= c5dd ? TCA8418_REG_GPIO_DIR2_C5DD_MSK : 0;
    value         |= c4dd ? TCA8418_REG_GPIO_DIR2_C4DD_MSK : 0;
    value         |= c3dd ? TCA8418_REG_GPIO_DIR2_C3DD_MSK : 0;
    value         |= c2dd ? TCA8418_REG_GPIO_DIR2_C2DD_MSK : 0;
    value         |= c1dd ? TCA8418_REG_GPIO_DIR2_C1DD_MSK : 0;
    value         |= c0dd ? TCA8418_REG_GPIO_DIR2_C0DD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DIR2, value);
}

esp_err_t tca8418_get_gpio_dir3(tca8418_handle_t* handle, bool* c9dd, bool* c8dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_DIR3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c9dd != NULL) {
        *c9dd = (value & TCA8418_REG_GPIO_DIR3_C9DD_MSK) != 0;
    }
    if (c8dd != NULL) {
        *c8dd = (value & TCA8418_REG_GPIO_DIR3_C8DD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_dir3(tca8418_handle_t* handle, bool c9dd, bool c8dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c9dd ? TCA8418_REG_GPIO_DIR3_C9DD_MSK : 0;
    value         |= c8dd ? TCA8418_REG_GPIO_DIR3_C8DD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_DIR3, value);
}

esp_err_t tca8418_get_gpio_int_lvl1(tca8418_handle_t* handle, bool* r7il, bool* r6il, bool* r5il, bool* r4il,
                                    bool* r3il, bool* r2il, bool* r1il, bool* r0il) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_LVL1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (r7il != NULL) {
        *r7il = (value & TCA8418_REG_GPIO_INT_LVL1_R7IL_MSK) != 0;
    }
    if (r6il != NULL) {
        *r6il = (value & TCA8418_REG_GPIO_INT_LVL1_R6IL_MSK) != 0;
    }
    if (r5il != NULL) {
        *r5il = (value & TCA8418_REG_GPIO_INT_LVL1_R5IL_MSK) != 0;
    }
    if (r4il != NULL) {
        *r4il = (value & TCA8418_REG_GPIO_INT_LVL1_R4IL_MSK) != 0;
    }
    if (r3il != NULL) {
        *r3il = (value & TCA8418_REG_GPIO_INT_LVL1_R3IL_MSK) != 0;
    }
    if (r2il != NULL) {
        *r2il = (value & TCA8418_REG_GPIO_INT_LVL1_R2IL_MSK) != 0;
    }
    if (r1il != NULL) {
        *r1il = (value & TCA8418_REG_GPIO_INT_LVL1_R1IL_MSK) != 0;
    }
    if (r0il != NULL) {
        *r0il = (value & TCA8418_REG_GPIO_INT_LVL1_R0IL_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_lvl1(tca8418_handle_t* handle, bool r7il, bool r6il, bool r5il, bool r4il, bool r3il,
                                    bool r2il, bool r1il, bool r0il) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= r7il ? TCA8418_REG_GPIO_INT_LVL1_R7IL_MSK : 0;
    value         |= r6il ? TCA8418_REG_GPIO_INT_LVL1_R6IL_MSK : 0;
    value         |= r5il ? TCA8418_REG_GPIO_INT_LVL1_R5IL_MSK : 0;
    value         |= r4il ? TCA8418_REG_GPIO_INT_LVL1_R4IL_MSK : 0;
    value         |= r3il ? TCA8418_REG_GPIO_INT_LVL1_R3IL_MSK : 0;
    value         |= r2il ? TCA8418_REG_GPIO_INT_LVL1_R2IL_MSK : 0;
    value         |= r1il ? TCA8418_REG_GPIO_INT_LVL1_R1IL_MSK : 0;
    value         |= r0il ? TCA8418_REG_GPIO_INT_LVL1_R0IL_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_LVL1, value);
}

esp_err_t tca8418_get_gpio_int_lvl2(tca8418_handle_t* handle, bool* c7il, bool* c6il, bool* c5il, bool* c4il,
                                    bool* c3il, bool* c2il, bool* c1il, bool* c0il) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_LVL2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c7il != NULL) {
        *c7il = (value & TCA8418_REG_GPIO_INT_LVL2_C7IL_MSK) != 0;
    }
    if (c6il != NULL) {
        *c6il = (value & TCA8418_REG_GPIO_INT_LVL2_C6IL_MSK) != 0;
    }
    if (c5il != NULL) {
        *c5il = (value & TCA8418_REG_GPIO_INT_LVL2_C5IL_MSK) != 0;
    }
    if (c4il != NULL) {
        *c4il = (value & TCA8418_REG_GPIO_INT_LVL2_C4IL_MSK) != 0;
    }
    if (c3il != NULL) {
        *c3il = (value & TCA8418_REG_GPIO_INT_LVL2_C3IL_MSK) != 0;
    }
    if (c2il != NULL) {
        *c2il = (value & TCA8418_REG_GPIO_INT_LVL2_C2IL_MSK) != 0;
    }
    if (c1il != NULL) {
        *c1il = (value & TCA8418_REG_GPIO_INT_LVL2_C1IL_MSK) != 0;
    }
    if (c0il != NULL) {
        *c0il = (value & TCA8418_REG_GPIO_INT_LVL2_C0IL_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_lvl2(tca8418_handle_t* handle, bool c7il, bool c6il, bool c5il, bool c4il, bool c3il,
                                    bool c2il, bool c1il, bool c0il) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c7il ? TCA8418_REG_GPIO_INT_LVL2_C7IL_MSK : 0;
    value         |= c6il ? TCA8418_REG_GPIO_INT_LVL2_C6IL_MSK : 0;
    value         |= c5il ? TCA8418_REG_GPIO_INT_LVL2_C5IL_MSK : 0;
    value         |= c4il ? TCA8418_REG_GPIO_INT_LVL2_C4IL_MSK : 0;
    value         |= c3il ? TCA8418_REG_GPIO_INT_LVL2_C3IL_MSK : 0;
    value         |= c2il ? TCA8418_REG_GPIO_INT_LVL2_C2IL_MSK : 0;
    value         |= c1il ? TCA8418_REG_GPIO_INT_LVL2_C1IL_MSK : 0;
    value         |= c0il ? TCA8418_REG_GPIO_INT_LVL2_C0IL_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_LVL2, value);
}

esp_err_t tca8418_get_gpio_int_lvl3(tca8418_handle_t* handle, bool* c9il, bool* c8il) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_INT_LVL3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c9il != NULL) {
        *c9il = (value & TCA8418_REG_GPIO_INT_LVL3_C9IL_MSK) != 0;
    }
    if (c8il != NULL) {
        *c8il = (value & TCA8418_REG_GPIO_INT_LVL3_C8IL_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_int_lvl3(tca8418_handle_t* handle, bool c9il, bool c8il) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c9il ? TCA8418_REG_GPIO_INT_LVL3_C9IL_MSK : 0;
    value         |= c8il ? TCA8418_REG_GPIO_INT_LVL3_C8IL_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_INT_LVL3, value);
}

esp_err_t tca8418_get_debounce_dis1(tca8418_handle_t* handle, bool* r7dd, bool* r6dd, bool* r5dd, bool* r4dd,
                                    bool* r3dd, bool* r2dd, bool* r1dd, bool* r0dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_DEBOUNCE_DIS1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (r7dd != NULL) {
        *r7dd = (value & TCA8418_REG_DEBOUNCE_DIS1_R7DD_MSK) != 0;
    }
    if (r6dd != NULL) {
        *r6dd = (value & TCA8418_REG_DEBOUNCE_DIS1_R6DD_MSK) != 0;
    }
    if (r5dd != NULL) {
        *r5dd = (value & TCA8418_REG_DEBOUNCE_DIS1_R5DD_MSK) != 0;
    }
    if (r4dd != NULL) {
        *r4dd = (value & TCA8418_REG_DEBOUNCE_DIS1_R4DD_MSK) != 0;
    }
    if (r3dd != NULL) {
        *r3dd = (value & TCA8418_REG_DEBOUNCE_DIS1_R3DD_MSK) != 0;
    }
    if (r2dd != NULL) {
        *r2dd = (value & TCA8418_REG_DEBOUNCE_DIS1_R2DD_MSK) != 0;
    }
    if (r1dd != NULL) {
        *r1dd = (value & TCA8418_REG_DEBOUNCE_DIS1_R1DD_MSK) != 0;
    }
    if (r0dd != NULL) {
        *r0dd = (value & TCA8418_REG_DEBOUNCE_DIS1_R0DD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_debounce_dis1(tca8418_handle_t* handle, bool r7dd, bool r6dd, bool r5dd, bool r4dd, bool r3dd,
                                    bool r2dd, bool r1dd, bool r0dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= r7dd ? TCA8418_REG_DEBOUNCE_DIS1_R7DD_MSK : 0;
    value         |= r6dd ? TCA8418_REG_DEBOUNCE_DIS1_R6DD_MSK : 0;
    value         |= r5dd ? TCA8418_REG_DEBOUNCE_DIS1_R5DD_MSK : 0;
    value         |= r4dd ? TCA8418_REG_DEBOUNCE_DIS1_R4DD_MSK : 0;
    value         |= r3dd ? TCA8418_REG_DEBOUNCE_DIS1_R3DD_MSK : 0;
    value         |= r2dd ? TCA8418_REG_DEBOUNCE_DIS1_R2DD_MSK : 0;
    value         |= r1dd ? TCA8418_REG_DEBOUNCE_DIS1_R1DD_MSK : 0;
    value         |= r0dd ? TCA8418_REG_DEBOUNCE_DIS1_R0DD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_DEBOUNCE_DIS1, value);
}

esp_err_t tca8418_get_debounce_dis2(tca8418_handle_t* handle, bool* c7dd, bool* c6dd, bool* c5dd, bool* c4dd,
                                    bool* c3dd, bool* c2dd, bool* c1dd, bool* c0dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_DEBOUNCE_DIS2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c7dd != NULL) {
        *c7dd = (value & TCA8418_REG_DEBOUNCE_DIS2_C7DD_MSK) != 0;
    }
    if (c6dd != NULL) {
        *c6dd = (value & TCA8418_REG_DEBOUNCE_DIS2_C6DD_MSK) != 0;
    }
    if (c5dd != NULL) {
        *c5dd = (value & TCA8418_REG_DEBOUNCE_DIS2_C5DD_MSK) != 0;
    }
    if (c4dd != NULL) {
        *c4dd = (value & TCA8418_REG_DEBOUNCE_DIS2_C4DD_MSK) != 0;
    }
    if (c3dd != NULL) {
        *c3dd = (value & TCA8418_REG_DEBOUNCE_DIS2_C3DD_MSK) != 0;
    }
    if (c2dd != NULL) {
        *c2dd = (value & TCA8418_REG_DEBOUNCE_DIS2_C2DD_MSK) != 0;
    }
    if (c1dd != NULL) {
        *c1dd = (value & TCA8418_REG_DEBOUNCE_DIS2_C1DD_MSK) != 0;
    }
    if (c0dd != NULL) {
        *c0dd = (value & TCA8418_REG_DEBOUNCE_DIS2_C0DD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_debounce_dis2(tca8418_handle_t* handle, bool c7dd, bool c6dd, bool c5dd, bool c4dd, bool c3dd,
                                    bool c2dd, bool c1dd, bool c0dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c7dd ? TCA8418_REG_DEBOUNCE_DIS2_C7DD_MSK : 0;
    value         |= c6dd ? TCA8418_REG_DEBOUNCE_DIS2_C6DD_MSK : 0;
    value         |= c5dd ? TCA8418_REG_DEBOUNCE_DIS2_C5DD_MSK : 0;
    value         |= c4dd ? TCA8418_REG_DEBOUNCE_DIS2_C4DD_MSK : 0;
    value         |= c3dd ? TCA8418_REG_DEBOUNCE_DIS2_C3DD_MSK : 0;
    value         |= c2dd ? TCA8418_REG_DEBOUNCE_DIS2_C2DD_MSK : 0;
    value         |= c1dd ? TCA8418_REG_DEBOUNCE_DIS2_C1DD_MSK : 0;
    value         |= c0dd ? TCA8418_REG_DEBOUNCE_DIS2_C0DD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_DEBOUNCE_DIS2, value);
}

esp_err_t tca8418_get_debounce_dis3(tca8418_handle_t* handle, bool* c9dd, bool* c8dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_DEBOUNCE_DIS3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c9dd != NULL) {
        *c9dd = (value & TCA8418_REG_DEBOUNCE_DIS3_C9DD_MSK) != 0;
    }
    if (c8dd != NULL) {
        *c8dd = (value & TCA8418_REG_DEBOUNCE_DIS3_C8DD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_debounce_dis3(tca8418_handle_t* handle, bool c9dd, bool c8dd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c9dd ? TCA8418_REG_DEBOUNCE_DIS3_C9DD_MSK : 0;
    value         |= c8dd ? TCA8418_REG_DEBOUNCE_DIS3_C8DD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_DEBOUNCE_DIS3, value);
}

esp_err_t tca8418_get_gpio_pull1(tca8418_handle_t* handle, bool* r7pd, bool* r6pd, bool* r5pd, bool* r4pd, bool* r3pd,
                                 bool* r2pd, bool* r1pd, bool* r0pd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_PULL1, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (r7pd != NULL) {
        *r7pd = (value & TCA8418_REG_GPIO_PULL1_R7PD_MSK) != 0;
    }
    if (r6pd != NULL) {
        *r6pd = (value & TCA8418_REG_GPIO_PULL1_R6PD_MSK) != 0;
    }
    if (r5pd != NULL) {
        *r5pd = (value & TCA8418_REG_GPIO_PULL1_R5PD_MSK) != 0;
    }
    if (r4pd != NULL) {
        *r4pd = (value & TCA8418_REG_GPIO_PULL1_R4PD_MSK) != 0;
    }
    if (r3pd != NULL) {
        *r3pd = (value & TCA8418_REG_GPIO_PULL1_R3PD_MSK) != 0;
    }
    if (r2pd != NULL) {
        *r2pd = (value & TCA8418_REG_GPIO_PULL1_R2PD_MSK) != 0;
    }
    if (r1pd != NULL) {
        *r1pd = (value & TCA8418_REG_GPIO_PULL1_R1PD_MSK) != 0;
    }
    if (r0pd != NULL) {
        *r0pd = (value & TCA8418_REG_GPIO_PULL1_R0PD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_pull1(tca8418_handle_t* handle, bool r7pd, bool r6pd, bool r5pd, bool r4pd, bool r3pd,
                                 bool r2pd, bool r1pd, bool r0pd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= r7pd ? TCA8418_REG_GPIO_PULL1_R7PD_MSK : 0;
    value         |= r6pd ? TCA8418_REG_GPIO_PULL1_R6PD_MSK : 0;
    value         |= r5pd ? TCA8418_REG_GPIO_PULL1_R5PD_MSK : 0;
    value         |= r4pd ? TCA8418_REG_GPIO_PULL1_R4PD_MSK : 0;
    value         |= r3pd ? TCA8418_REG_GPIO_PULL1_R3PD_MSK : 0;
    value         |= r2pd ? TCA8418_REG_GPIO_PULL1_R2PD_MSK : 0;
    value         |= r1pd ? TCA8418_REG_GPIO_PULL1_R1PD_MSK : 0;
    value         |= r0pd ? TCA8418_REG_GPIO_PULL1_R0PD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_PULL1, value);
}

esp_err_t tca8418_get_gpio_pull2(tca8418_handle_t* handle, bool* c7pd, bool* c6pd, bool* c5pd, bool* c4pd, bool* c3pd,
                                 bool* c2pd, bool* c1pd, bool* c0pd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_PULL2, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c7pd != NULL) {
        *c7pd = (value & TCA8418_REG_GPIO_PULL2_C7PD_MSK) != 0;
    }
    if (c6pd != NULL) {
        *c6pd = (value & TCA8418_REG_GPIO_PULL2_C6PD_MSK) != 0;
    }
    if (c5pd != NULL) {
        *c5pd = (value & TCA8418_REG_GPIO_PULL2_C5PD_MSK) != 0;
    }
    if (c4pd != NULL) {
        *c4pd = (value & TCA8418_REG_GPIO_PULL2_C4PD_MSK) != 0;
    }
    if (c3pd != NULL) {
        *c3pd = (value & TCA8418_REG_GPIO_PULL2_C3PD_MSK) != 0;
    }
    if (c2pd != NULL) {
        *c2pd = (value & TCA8418_REG_GPIO_PULL2_C2PD_MSK) != 0;
    }
    if (c1pd != NULL) {
        *c1pd = (value & TCA8418_REG_GPIO_PULL2_C1PD_MSK) != 0;
    }
    if (c0pd != NULL) {
        *c0pd = (value & TCA8418_REG_GPIO_PULL2_C0PD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_pull2(tca8418_handle_t* handle, bool c7pd, bool c6pd, bool c5pd, bool c4pd, bool c3pd,
                                 bool c2pd, bool c1pd, bool c0pd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c7pd ? TCA8418_REG_GPIO_PULL2_C7PD_MSK : 0;
    value         |= c6pd ? TCA8418_REG_GPIO_PULL2_C6PD_MSK : 0;
    value         |= c5pd ? TCA8418_REG_GPIO_PULL2_C5PD_MSK : 0;
    value         |= c4pd ? TCA8418_REG_GPIO_PULL2_C4PD_MSK : 0;
    value         |= c3pd ? TCA8418_REG_GPIO_PULL2_C3PD_MSK : 0;
    value         |= c2pd ? TCA8418_REG_GPIO_PULL2_C2PD_MSK : 0;
    value         |= c1pd ? TCA8418_REG_GPIO_PULL2_C1PD_MSK : 0;
    value         |= c0pd ? TCA8418_REG_GPIO_PULL2_C0PD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_PULL2, value);
}

esp_err_t tca8418_get_gpio_pull3(tca8418_handle_t* handle, bool* c9pd, bool* c8pd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t   value = 0;
    esp_err_t res   = tca8418_read_reg(handle, TCA8418_REG_GPIO_PULL3, &value);
    if (res != ESP_OK) {
        return res;
    }
    if (c9pd != NULL) {
        *c9pd = (value & TCA8418_REG_GPIO_PULL3_C9PD_MSK) != 0;
    }
    if (c8pd != NULL) {
        *c8pd = (value & TCA8418_REG_GPIO_PULL3_C8PD_MSK) != 0;
    }
    return ESP_OK;
}

esp_err_t tca8418_set_gpio_pull3(tca8418_handle_t* handle, bool c9pd, bool c8pd) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t value  = 0;
    value         |= c9pd ? TCA8418_REG_GPIO_PULL3_C9PD_MSK : 0;
    value         |= c8pd ? TCA8418_REG_GPIO_PULL3_C8PD_MSK : 0;
    return tca8418_write_reg(handle, TCA8418_REG_GPIO_PULL3, value);
}

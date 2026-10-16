#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// Types
struct tca8418_handle;

typedef void (*tca8418_cb_t)(struct tca8418_handle*);

typedef struct tca8418_handle {
    i2c_master_dev_handle_t dev_handle;
    SemaphoreHandle_t       interrupt_semaphore;
    TaskHandle_t            interrupt_handler_thread;
    gpio_num_t              reset_pin;
    gpio_num_t              interrupt_pin;
    tca8418_cb_t            interrupt_callback_cad;
    tca8418_cb_t            interrupt_callback_overflow;
    tca8418_cb_t            interrupt_callback_lock;
    tca8418_cb_t            interrupt_callback_gpi;
    tca8418_cb_t            interrupt_callback_key;
} tca8418_handle_t;

// Functions
esp_err_t tca8418_initialize(tca8418_handle_t* handle, i2c_master_bus_handle_t i2c_bus, gpio_num_t reset_pin,
                             gpio_num_t interrupt_pin);

esp_err_t tca8418_set_cad_callback(tca8418_handle_t* handle, tca8418_cb_t callback);
esp_err_t tca8418_set_overflow_callback(tca8418_handle_t* handle, tca8418_cb_t callback);
esp_err_t tca8418_set_lock_callback(tca8418_handle_t* handle, tca8418_cb_t callback);
esp_err_t tca8418_set_gpi_callback(tca8418_handle_t* handle, tca8418_cb_t callback);
esp_err_t tca8418_set_key_callback(tca8418_handle_t* handle, tca8418_cb_t callback);

esp_err_t tca8418_get_cfg(tca8418_handle_t* handle, bool* ai, bool* gpi_e_cfg, bool* ovr_flow_m, bool* int_cfg,
                          bool* ovr_flow_ien, bool* k_lck_ien, bool* gpi_ien, bool* ke_ien);
esp_err_t tca8418_set_cfg(tca8418_handle_t* handle, bool ai, bool gpi_e_cfg, bool ovr_flow_m, bool int_cfg,
                          bool ovr_flow_ien, bool k_lck_ien, bool gpi_ien, bool ke_ien);
esp_err_t tca8418_get_int_stat(tca8418_handle_t* handle, bool* cad_int, bool* ovr_flow_int, bool* k_lck_int,
                               bool* gpi_int, bool* k_int);
esp_err_t tca8418_set_int_stat(tca8418_handle_t* handle, bool cad_int, bool ovr_flow_int, bool k_lck_int, bool gpi_int,
                               bool k_int);
esp_err_t tca8418_get_key_lck_ec(tca8418_handle_t* handle, bool* k_lck_en, bool* lck2, bool* lck1, uint8_t* kec);
esp_err_t tca8418_set_key_lck_ec(tca8418_handle_t* handle, bool k_lck_en, bool lck2, bool lck1, uint8_t kec);
esp_err_t tca8418_get_key_event_a(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_a(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_b(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_b(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_c(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_c(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_d(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_d(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_e(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_e(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_f(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_f(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_g(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_g(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_h(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_h(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_i(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_i(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_key_event_j(tca8418_handle_t* handle, bool* pressed, uint8_t* code);
esp_err_t tca8418_set_key_event_j(tca8418_handle_t* handle, bool pressed, uint8_t code);
esp_err_t tca8418_get_kp_lck_timer(tca8418_handle_t* handle, uint8_t* int_mask_timer, uint8_t* lock_timer);
esp_err_t tca8418_set_kp_lck_timer(tca8418_handle_t* handle, uint8_t int_mask_timer, uint8_t lock_timer);
esp_err_t tca8418_get_unlock1(tca8418_handle_t* handle, uint8_t* uk1);
esp_err_t tca8418_set_unlock1(tca8418_handle_t* handle, uint8_t uk1);
esp_err_t tca8418_get_unlock2(tca8418_handle_t* handle, uint8_t* uk2);
esp_err_t tca8418_set_unlock2(tca8418_handle_t* handle, uint8_t uk2);
esp_err_t tca8418_get_gpio_int_stat1(tca8418_handle_t* handle, bool* r7is, bool* r6is, bool* r5is, bool* r4is,
                                     bool* r3is, bool* r2is, bool* r1is, bool* r0is);
esp_err_t tca8418_set_gpio_int_stat1(tca8418_handle_t* handle, bool r7is, bool r6is, bool r5is, bool r4is, bool r3is,
                                     bool r2is, bool r1is, bool r0is);
esp_err_t tca8418_get_gpio_int_stat2(tca8418_handle_t* handle, bool* c7is, bool* c6is, bool* c5is, bool* c4is,
                                     bool* c3is, bool* c2is, bool* c1is, bool* c0is);
esp_err_t tca8418_set_gpio_int_stat2(tca8418_handle_t* handle, bool c7is, bool c6is, bool c5is, bool c4is, bool c3is,
                                     bool c2is, bool c1is, bool c0is);
esp_err_t tca8418_get_gpio_int_stat3(tca8418_handle_t* handle, bool* c9is, bool* c8is);
esp_err_t tca8418_set_gpio_int_stat3(tca8418_handle_t* handle, bool c9is, bool c8is);
esp_err_t tca8418_get_gpio_dat_stat1(tca8418_handle_t* handle, bool* r7ds, bool* r6ds, bool* r5ds, bool* r4ds,
                                     bool* r3ds, bool* r2ds, bool* r1ds, bool* r0ds);
esp_err_t tca8418_set_gpio_dat_stat1(tca8418_handle_t* handle, bool r7ds, bool r6ds, bool r5ds, bool r4ds, bool r3ds,
                                     bool r2ds, bool r1ds, bool r0ds);
esp_err_t tca8418_get_gpio_dat_stat2(tca8418_handle_t* handle, bool* c7ds, bool* c6ds, bool* c5ds, bool* c4ds,
                                     bool* c3ds, bool* c2ds, bool* c1ds, bool* c0ds);
esp_err_t tca8418_set_gpio_dat_stat2(tca8418_handle_t* handle, bool c7ds, bool c6ds, bool c5ds, bool c4ds, bool c3ds,
                                     bool c2ds, bool c1ds, bool c0ds);
esp_err_t tca8418_get_gpio_dat_stat3(tca8418_handle_t* handle, bool* c9ds, bool* c8ds);
esp_err_t tca8418_set_gpio_dat_stat3(tca8418_handle_t* handle, bool c9ds, bool c8ds);
esp_err_t tca8418_get_gpio_dat_out1(tca8418_handle_t* handle, bool* r7do, bool* r6do, bool* r5do, bool* r4do,
                                    bool* r3do, bool* r2do, bool* r1do, bool* r0do);
esp_err_t tca8418_set_gpio_dat_out1(tca8418_handle_t* handle, bool r7do, bool r6do, bool r5do, bool r4do, bool r3do,
                                    bool r2do, bool r1do, bool r0do);
esp_err_t tca8418_get_gpio_dat_out2(tca8418_handle_t* handle, bool* c7do, bool* c6do, bool* c5do, bool* c4do,
                                    bool* c3do, bool* c2do, bool* c1do, bool* c0do);
esp_err_t tca8418_set_gpio_dat_out2(tca8418_handle_t* handle, bool c7do, bool c6do, bool c5do, bool c4do, bool c3do,
                                    bool c2do, bool c1do, bool c0do);
esp_err_t tca8418_get_gpio_dat_out3(tca8418_handle_t* handle, bool* c9do, bool* c8do);
esp_err_t tca8418_set_gpio_dat_out3(tca8418_handle_t* handle, bool c9do, bool c8do);
esp_err_t tca8418_get_gpio_int_en1(tca8418_handle_t* handle, bool* r7ie, bool* r6ie, bool* r5ie, bool* r4ie, bool* r3ie,
                                   bool* r2ie, bool* r1ie, bool* r0ie);
esp_err_t tca8418_set_gpio_int_en1(tca8418_handle_t* handle, bool r7ie, bool r6ie, bool r5ie, bool r4ie, bool r3ie,
                                   bool r2ie, bool r1ie, bool r0ie);
esp_err_t tca8418_get_gpio_int_en2(tca8418_handle_t* handle, bool* c7ie, bool* c6ie, bool* c5ie, bool* c4ie, bool* c3ie,
                                   bool* c2ie, bool* c1ie, bool* c0ie);
esp_err_t tca8418_set_gpio_int_en2(tca8418_handle_t* handle, bool c7ie, bool c6ie, bool c5ie, bool c4ie, bool c3ie,
                                   bool c2ie, bool c1ie, bool c0ie);
esp_err_t tca8418_get_gpio_int_en3(tca8418_handle_t* handle, bool* c9ie, bool* c8ie);
esp_err_t tca8418_set_gpio_int_en3(tca8418_handle_t* handle, bool c9ie, bool c8ie);
esp_err_t tca8418_get_kp_gpio1(tca8418_handle_t* handle, bool* row7, bool* row6, bool* row5, bool* row4, bool* row3,
                               bool* row2, bool* row1, bool* row0);
esp_err_t tca8418_set_kp_gpio1(tca8418_handle_t* handle, bool row7, bool row6, bool row5, bool row4, bool row3,
                               bool row2, bool row1, bool row0);
esp_err_t tca8418_get_kp_gpio2(tca8418_handle_t* handle, bool* col7, bool* col6, bool* col5, bool* col4, bool* col3,
                               bool* col2, bool* col1, bool* col0);
esp_err_t tca8418_set_kp_gpio2(tca8418_handle_t* handle, bool col7, bool col6, bool col5, bool col4, bool col3,
                               bool col2, bool col1, bool col0);
esp_err_t tca8418_get_kp_gpio3(tca8418_handle_t* handle, bool* col9, bool* col8);
esp_err_t tca8418_set_kp_gpio3(tca8418_handle_t* handle, bool col9, bool col8);
esp_err_t tca8418_get_gpi_em1(tca8418_handle_t* handle, bool* row7, bool* row6, bool* row5, bool* row4, bool* row3,
                              bool* row2, bool* row1, bool* row0);
esp_err_t tca8418_set_gpi_em1(tca8418_handle_t* handle, bool row7, bool row6, bool row5, bool row4, bool row3,
                              bool row2, bool row1, bool row0);
esp_err_t tca8418_get_gpi_em2(tca8418_handle_t* handle, bool* col7, bool* col6, bool* col5, bool* col4, bool* col3,
                              bool* col2, bool* col1, bool* col0);
esp_err_t tca8418_set_gpi_em2(tca8418_handle_t* handle, bool col7, bool col6, bool col5, bool col4, bool col3,
                              bool col2, bool col1, bool col0);
esp_err_t tca8418_get_gpi_em3(tca8418_handle_t* handle, bool* col9, bool* col8);
esp_err_t tca8418_set_gpi_em3(tca8418_handle_t* handle, bool col9, bool col8);
esp_err_t tca8418_get_gpio_dir1(tca8418_handle_t* handle, bool* r7dd, bool* r6dd, bool* r5dd, bool* r4dd, bool* r3dd,
                                bool* r2dd, bool* r1dd, bool* r0dd);
esp_err_t tca8418_set_gpio_dir1(tca8418_handle_t* handle, bool r7dd, bool r6dd, bool r5dd, bool r4dd, bool r3dd,
                                bool r2dd, bool r1dd, bool r0dd);
esp_err_t tca8418_get_gpio_dir2(tca8418_handle_t* handle, bool* c7dd, bool* c6dd, bool* c5dd, bool* c4dd, bool* c3dd,
                                bool* c2dd, bool* c1dd, bool* c0dd);
esp_err_t tca8418_set_gpio_dir2(tca8418_handle_t* handle, bool c7dd, bool c6dd, bool c5dd, bool c4dd, bool c3dd,
                                bool c2dd, bool c1dd, bool c0dd);
esp_err_t tca8418_get_gpio_dir3(tca8418_handle_t* handle, bool* c9dd, bool* c8dd);
esp_err_t tca8418_set_gpio_dir3(tca8418_handle_t* handle, bool c9dd, bool c8dd);
esp_err_t tca8418_get_gpio_int_lvl1(tca8418_handle_t* handle, bool* r7il, bool* r6il, bool* r5il, bool* r4il,
                                    bool* r3il, bool* r2il, bool* r1il, bool* r0il);
esp_err_t tca8418_set_gpio_int_lvl1(tca8418_handle_t* handle, bool r7il, bool r6il, bool r5il, bool r4il, bool r3il,
                                    bool r2il, bool r1il, bool r0il);
esp_err_t tca8418_get_gpio_int_lvl2(tca8418_handle_t* handle, bool* c7il, bool* c6il, bool* c5il, bool* c4il,
                                    bool* c3il, bool* c2il, bool* c1il, bool* c0il);
esp_err_t tca8418_set_gpio_int_lvl2(tca8418_handle_t* handle, bool c7il, bool c6il, bool c5il, bool c4il, bool c3il,
                                    bool c2il, bool c1il, bool c0il);
esp_err_t tca8418_get_gpio_int_lvl3(tca8418_handle_t* handle, bool* c9il, bool* c8il);
esp_err_t tca8418_set_gpio_int_lvl3(tca8418_handle_t* handle, bool c9il, bool c8il);
esp_err_t tca8418_get_debounce_dis1(tca8418_handle_t* handle, bool* r7dd, bool* r6dd, bool* r5dd, bool* r4dd,
                                    bool* r3dd, bool* r2dd, bool* r1dd, bool* r0dd);
esp_err_t tca8418_set_debounce_dis1(tca8418_handle_t* handle, bool r7dd, bool r6dd, bool r5dd, bool r4dd, bool r3dd,
                                    bool r2dd, bool r1dd, bool r0dd);
esp_err_t tca8418_get_debounce_dis2(tca8418_handle_t* handle, bool* c7dd, bool* c6dd, bool* c5dd, bool* c4dd,
                                    bool* c3dd, bool* c2dd, bool* c1dd, bool* c0dd);
esp_err_t tca8418_set_debounce_dis2(tca8418_handle_t* handle, bool c7dd, bool c6dd, bool c5dd, bool c4dd, bool c3dd,
                                    bool c2dd, bool c1dd, bool c0dd);
esp_err_t tca8418_get_debounce_dis3(tca8418_handle_t* handle, bool* c9dd, bool* c8dd);
esp_err_t tca8418_set_debounce_dis3(tca8418_handle_t* handle, bool c9dd, bool c8dd);
esp_err_t tca8418_get_gpio_pull1(tca8418_handle_t* handle, bool* r7pd, bool* r6pd, bool* r5pd, bool* r4pd, bool* r3pd,
                                 bool* r2pd, bool* r1pd, bool* r0pd);
esp_err_t tca8418_set_gpio_pull1(tca8418_handle_t* handle, bool r7pd, bool r6pd, bool r5pd, bool r4pd, bool r3pd,
                                 bool r2pd, bool r1pd, bool r0pd);
esp_err_t tca8418_get_gpio_pull2(tca8418_handle_t* handle, bool* c7pd, bool* c6pd, bool* c5pd, bool* c4pd, bool* c3pd,
                                 bool* c2pd, bool* c1pd, bool* c0pd);
esp_err_t tca8418_set_gpio_pull2(tca8418_handle_t* handle, bool c7pd, bool c6pd, bool c5pd, bool c4pd, bool c3pd,
                                 bool c2pd, bool c1pd, bool c0pd);
esp_err_t tca8418_get_gpio_pull3(tca8418_handle_t* handle, bool* c9pd, bool* c8pd);
esp_err_t tca8418_set_gpio_pull3(tca8418_handle_t* handle, bool c9pd, bool c8pd);


// SPDX-License-Identifier: MIT

// Private include file, not intended to be included by end users

#pragma once

// Internal I2C bus
#define BSP_I2C_BUS     0
#define BSP_I2C_SDA_PIN 18
#define BSP_I2C_SCL_PIN 20


#define BSP_KBD_INT       2
#define BSP_KBD_SCL       BSP_I2C_SCL_PIN
#define BSP_KBD_SDA       BSP_I2C_SDA_PIN
#define BSP_KBD_RST       -1


// MIPI DSI display
#define BSP_DSI_LDO_CHAN       3
#define BSP_DSI_LDO_VOLTAGE_MV 2500
#define BSP_LCD_RESET_PIN      17  // Note: low for normal operation, high for reset

// Radio (ESP32-C6 on the carrier, over SDIO). The C6's EN line is shared with the TCA8418 keypad
// controller's reset (net C6_CHIP_PU, M.2 pin 32). The C6's GPIO 15 / GPIO 10 drive the display
// and keyboard backlight PWM (AP3032 CTRL, 100k pull-downs); the P4 has no path to them (the R16
// link from P4 GPIO 5 is not populated), so holding the C6 in reset is what switches them off.
#define BSP_RADIO_ENABLE_PIN   12
#define BSP_RADIO_BOOT_PIN     13

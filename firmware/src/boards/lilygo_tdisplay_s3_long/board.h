#pragma once
#include <stdint.h>

// LilyGo T-Display-S3-Long — 3.4" 180x640 bar display, used here in LANDSCAPE.
//
// The panel is an AXS15231B driven over QSPI in its native portrait geometry
// (180 wide x 640 tall). LVGL renders a 640x180 landscape UI and display.cpp
// rotates each flushed strip on the CPU before pushing it to the panel — the
// controller's MADCTL rotation is unreliable on this long panel, and the
// project's HAL already has the rotate-strip pattern (AMOLED-2.16 port).
//
// Pin map from the LilyGo schematic (T-Display-S3-Long-3.4-V1.0.pdf) — NOT
// from the examples' pins_config.h, which carries two copy-paste mistakes:
// PIN_BUTTON_2 = 21 (that's QSPI D2) and PIN_BAT_VOLT = 8 (the divider is on
// GPIO2). Only one push button reaches the MCU: KEY1 = BOOT / GPIO0; KEY2 is
// wired to CHIP_PU (hardware reset).

#define BOARD_NAME           "LilyGo T-Display-S3-Long"

// ---- Logical (LVGL) geometry — landscape ----
#define LCD_WIDTH            640
#define LCD_HEIGHT           180
// ---- Physical panel geometry — portrait, as the controller sees it ----
#define PANEL_WIDTH          180
#define PANEL_HEIGHT         640
// Rotation quadrant applied in display.cpp (quarter turns CW of the logical
// image onto the panel): 1 or 3 — which one is "USB on the right" depends on
// how the board sits on the desk. Default below; hold BOOT during power-on to
// flip by 180° (persisted in NVS, see board_init.cpp).
#define LCD_ROTATION_DEFAULT 1

// ---- QSPI display pins (AXS15231B) ----
#define LCD_CS               12
#define LCD_SCLK             17
#define LCD_SDIO0            13
#define LCD_SDIO1            18
#define LCD_SDIO2            21
#define LCD_SDIO3            14
#define LCD_RESET            16    // shared with the touch controller's reset
#define LCD_BL               1     // PT4103 backlight EN, LEDC PWM
#define LCD_QSPI_HZ          32000000   // 40 MHz shows artifacts per LilyGo

// ---- I2C bus (touch + charger) ----
#define IIC_SDA              15
#define IIC_SCL              10

// ---- Touch (built into the AXS15231B, vendor protocol at 0x3B) ----
#define TP_INT               11
#define TP_RST               16    // = LCD_RESET; pulsed by the panel driver
#define AXS_TOUCH_ADDR       0x3B

// ---- Charger / power path (SY6970, BQ2589x-compatible register map) ----
#define SY6970_ADDR          0x6A

// ---- Battery (VBAT -> 100K/100K divider -> GPIO2) ----
#define BAT_ADC_PIN          2
#define BAT_VOLT_DIVIDER     2.0f

// ---- Buttons ----
// BOOT (GPIO0) is the only MCU-connected button. It takes the PWR role
// (power.cpp): short press = cycle animations / brightness, hold ~3 s +
// release = pairing mode, hold 8 s = deep sleep. No HID PTT button here.
#define BTN_PWR_GPIO         0

// ---- Capability flags ----
#define BOARD_HAS_SECONDARY_BUTTON 0
#define BOARD_HAS_ROTATION         0    // fixed landscape; CPU rotation is static
#define BOARD_HAS_IMU              0
#define BOARD_HAS_BATTERY          1
#define BOARD_HAS_IO_EXPANDER      0
#define BOARD_HAS_SOUND            0

// Runtime rotation quadrant (1 or 3), loaded from NVS in board_init().
uint8_t board_rotation_quadrant(void);

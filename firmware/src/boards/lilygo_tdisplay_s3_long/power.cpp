#include "../../hal/power_hal.h"
#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <esp_sleep.h>

// Power on the T-Display-S3-Long:
//   - SY6970 charger/power-path IC on the I2C bus (BQ2589x-compatible map).
//     Reg 0x0B: VBUS_STAT [7:5] (0 = no input), CHRG_STAT [4:3] (1/2 = charging,
//     3 = done). Gives real VBUS / charging state for the idle-sleep policy.
//   - VBAT through a 100K/100K divider on GPIO2 — battery percentage (linear
//     3.3 V..4.2 V). Reads as "no battery" (-1) when nothing is attached.
//   - BOOT (GPIO0) as the PWR-role button with the same software edge
//     synthesis as the LCD-1.54 port: short press on release (< PWR_LONG_MS),
//     long once at PWR_LONG_MS (starts the hold-to-pair gesture in main.cpp),
//     release edge always. Holding 8 s deep-sleeps the chip (there is no
//     rail switch on this board); BOOT press wakes it.

#define BATTERY_POLL_MS  2000
#define CHARGER_POLL_MS  1000
#define PWR_POLL_MS      50
#define PWR_LONG_MS      1500
#define PWR_OFF_HOLD_MS  8000

static int      cached_pct        = -1;
static bool     vbus_in           = true;   // assume USB until the charger says otherwise
static bool     charging          = false;
static bool     charger_ok        = false;
static bool     pwr_pressed_flag  = false;
static bool     pwr_long_flag     = false;
static bool     pwr_released_flag = false;
static bool     last_pwr_state    = false;
static uint32_t pwr_press_started_ms = 0;
static bool     pwr_long_fired    = false;
static uint32_t last_battery_ms   = 0;
static uint32_t last_charger_ms   = 0;
static uint32_t last_pwr_ms       = 0;

static bool sy6970_read(uint8_t reg, uint8_t* out) {
    Wire.beginTransmission(SY6970_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((uint8_t)SY6970_ADDR, (uint8_t)1) != 1) return false;
    *out = Wire.read();
    return true;
}

static void sample_charger(void) {
    uint8_t st;
    if (!sy6970_read(0x0B, &st)) { charger_ok = false; return; }
    charger_ok = true;
    vbus_in  = ((st >> 5) & 0x07) != 0;
    uint8_t chrg = (st >> 3) & 0x03;
    charging = (chrg == 1 || chrg == 2);
}

static void sample_battery(void) {
    uint32_t mv = 0;
    for (int i = 0; i < 4; i++) mv += analogReadMilliVolts(BAT_ADC_PIN);
    float vbat = (mv / 4) * BAT_VOLT_DIVIDER / 1000.0f;
    if (vbat < 3.0f) { cached_pct = -1; return; }   // divider floating — no battery
    int pct = (int)((vbat - 3.3f) * (100.0f / 0.9f) + 0.5f);
    cached_pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

static void power_off(void) {
    Serial.println("PWR held 8s — deep sleep (press BOOT to wake)");
    Serial.flush();
    display_hal_set_brightness(0);
    delay(50);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)BTN_PWR_GPIO, 0);
    delay(200);
    esp_deep_sleep_start();
}

void power_hal_init(void) {
    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    analogReadResolution(12);
    sample_charger();
    sample_battery();
    Serial.printf("Power: SY6970 %s, vbus=%d charging=%d, battery=%d%%\n",
        charger_ok ? "OK" : "not found", vbus_in ? 1 : 0, charging ? 1 : 0, cached_pct);
}

void power_hal_tick(void) {
    uint32_t now = millis();

    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        sample_battery();
    }
    if (now - last_charger_ms >= CHARGER_POLL_MS) {
        last_charger_ms = now;
        sample_charger();
    }
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        bool pwr_now = (digitalRead(BTN_PWR_GPIO) == LOW);   // active LOW
        if (pwr_now && !last_pwr_state) {            // press edge
            pwr_press_started_ms = now;
            pwr_long_fired = false;
        } else if (pwr_now && last_pwr_state) {      // held
            if (!pwr_long_fired && (now - pwr_press_started_ms >= PWR_LONG_MS)) {
                pwr_long_flag  = true;
                pwr_long_fired = true;
            }
            if (now - pwr_press_started_ms >= PWR_OFF_HOLD_MS) {
                power_off();   // does not return
            }
        } else if (!pwr_now && last_pwr_state) {     // release edge
            pwr_released_flag = true;
            if (!pwr_long_fired) pwr_pressed_flag = true;  // short press
        }
        last_pwr_state = pwr_now;
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return charging; }
bool power_hal_is_vbus_in(void)  { return vbus_in; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}

bool power_hal_pwr_long_pressed(void) {
    if (pwr_long_flag) { pwr_long_flag = false; return true; }
    return false;
}

bool power_hal_pwr_released(void) {
    if (pwr_released_flag) { pwr_released_flag = false; return true; }
    return false;
}

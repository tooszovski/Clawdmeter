#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_system.h>

// Bring up the shared I2C bus (touch + SY6970 charger) and resolve the panel
// orientation. Holding BOOT while the board powers on flips the landscape
// orientation by 180° and persists it, so the same binary works with the USB
// cable leaving on either side.

static uint8_t rotation = LCD_ROTATION_DEFAULT;

uint8_t board_rotation_quadrant(void) { return rotation; }

static const char* reset_reason_str(esp_reset_reason_t r) {
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "sdio";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_JTAG:      return "jtag";
    default:                return "unknown";
    }
}

extern "C" void board_init(void) {
    Serial.printf("Reset reason: %s (%d)\n", reset_reason_str(esp_reset_reason()), (int)esp_reset_reason());
    // Touch/panel reset (shared GPIO16). LilyGo's reference brings INT up
    // with a pull-up BEFORE pulsing reset; the AXS15231B samples INT at reset
    // and comes up dead (constant 0x02 frames, no interrupts) if it floats.
    pinMode(TP_INT, INPUT_PULLUP);
    pinMode(LCD_RESET, OUTPUT);
    digitalWrite(LCD_RESET, HIGH); delay(2);
    digitalWrite(LCD_RESET, LOW);  delay(100);
    digitalWrite(LCD_RESET, HIGH); delay(2);

    Wire.begin(IIC_SDA, IIC_SCL);
    // 100 kHz, as in LilyGo's reference. At 400 kHz the AXS15231B touch block
    // answered polls with constant junk bytes and stalled the bus for seconds.
    Wire.setClock(100000);
    Wire.setTimeOut(30);   // ms — never let a wedged transaction stall the LVGL loop

    Preferences prefs;
    prefs.begin("tdlong", false);
    uint8_t saved = prefs.getUChar("rot", LCD_ROTATION_DEFAULT);
    if (saved != 1 && saved != 3) saved = LCD_ROTATION_DEFAULT;

    pinMode(BTN_PWR_GPIO, INPUT_PULLUP);
    delay(10);
    if (digitalRead(BTN_PWR_GPIO) == LOW) {
        saved = (saved == 1) ? 3 : 1;
        prefs.putUChar("rot", saved);
        Serial.printf("BOOT held at power-on: orientation flipped, rotation=%u saved\n", saved);
        // Wait for release so power.cpp doesn't see this hold as a long press.
        uint32_t t0 = millis();
        while (digitalRead(BTN_PWR_GPIO) == LOW && millis() - t0 < 5000) delay(10);
    }
    prefs.end();
    rotation = saved;
    Serial.printf("Board init: %s, rotation quadrant %u\n", BOARD_NAME, rotation);
}

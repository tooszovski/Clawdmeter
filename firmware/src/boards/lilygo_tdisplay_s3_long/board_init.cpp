#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>

// Bring up the shared I2C bus (touch + SY6970 charger) and resolve the panel
// orientation. Holding BOOT while the board powers on flips the landscape
// orientation by 180° and persists it, so the same binary works with the USB
// cable leaving on either side.

static uint8_t rotation = LCD_ROTATION_DEFAULT;

uint8_t board_rotation_quadrant(void) { return rotation; }

extern "C" void board_init(void) {
    Wire.begin(IIC_SDA, IIC_SCL);
    Wire.setClock(400000);

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

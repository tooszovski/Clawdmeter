#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>

// Touch is integrated in the AXS15231B and read over I2C (0x3B) with the
// vendor's 11-byte read command; the reply is 8 bytes:
//   [0] gesture  [1] finger count  [2] event(hi nibble) | X hi(lo nibble)
//   [3] X lo     [4] id(hi) | Y hi(lo nibble)           [5] Y lo  [6..7] weight/area
// Coordinates are in the panel's portrait frame (X 0..179, Y 0..639, Y grows
// toward the connector end per LilyGo's example, which flips it). They are
// mapped here into the 640x180 landscape frame LVGL renders, matching the
// rotation quadrant display.cpp uses. Vendored reader — the LilyGo example
// depends on the GPLv3 Arduino_DriveBus library.
//
// The controller's reset line is LCD_RESET, which the panel driver pulses in
// gfx->begin(); touch_hal_init() runs after display_hal_begin() in setup(),
// so no separate reset here.

static const uint8_t READ_CMD[11] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00};

static volatile bool touch_irq = false;
static bool     pressed_state = false;
static uint16_t last_x = 0, last_y = 0;
static uint32_t last_read_ms = 0;

static void IRAM_ATTR touch_isr(void) { touch_irq = true; }

static void map_to_landscape(uint16_t px, uint16_t py, uint16_t* lx, uint16_t* ly) {
    // Inverse of display.cpp's rotate_strip mapping.
    if (px >= PANEL_WIDTH)  px = PANEL_WIDTH - 1;
    if (py >= PANEL_HEIGHT) py = PANEL_HEIGHT - 1;
    if (board_rotation_quadrant() == 1) {
        *lx = py;
        *ly = PANEL_WIDTH - 1 - px;
    } else {
        *lx = PANEL_HEIGHT - 1 - py;
        *ly = px;
    }
}

static void read_controller(void) {
    uint8_t buf[8] = {0};
    Wire.beginTransmission(AXS_TOUCH_ADDR);
    Wire.write(READ_CMD, sizeof(READ_CMD));
    if (Wire.endTransmission(false) != 0) { pressed_state = false; return; }
    if (Wire.requestFrom((uint8_t)AXS_TOUCH_ADDR, (uint8_t)sizeof(buf)) != sizeof(buf)) {
        pressed_state = false;
        return;
    }
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = Wire.read();

    const uint8_t fingers = buf[1];
    const uint8_t event   = buf[2] >> 4;     // 0x08 = contact per LilyGo's example
    if (fingers == 0 || fingers > 2 || event != 0x08) {
        pressed_state = false;
        return;
    }
    const uint16_t raw_x = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];
    const uint16_t raw_y = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    const uint16_t px = raw_x;
    const uint16_t py = (raw_y < PANEL_HEIGHT) ? (PANEL_HEIGHT - 1 - raw_y) : 0;
    map_to_landscape(px, py, &last_x, &last_y);
    pressed_state = true;
}

void touch_hal_init(void) {
    pinMode(TP_INT, INPUT_PULLUP);
    attachInterrupt(TP_INT, touch_isr, FALLING);

    // Probe: a read that ACKs means the controller is alive on the bus.
    Wire.beginTransmission(AXS_TOUCH_ADDR);
    Wire.write(READ_CMD, sizeof(READ_CMD));
    bool ok = (Wire.endTransmission(false) == 0) &&
              (Wire.requestFrom((uint8_t)AXS_TOUCH_ADDR, (uint8_t)8) == 8);
    while (Wire.available()) Wire.read();
    Serial.printf("Touch AXS15231B @0x%02X: %s\n", AXS_TOUCH_ADDR, ok ? "OK" : "no response");
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    const uint32_t now = millis();
    // Read on interrupt, and re-poll while pressed so a missed release edge
    // (finger-up report between polls) can't leave a stuck press.
    if (touch_irq || (pressed_state && now - last_read_ms >= 20)) {
        touch_irq = false;
        last_read_ms = now;
        read_controller();
    }
    *x = last_x;
    *y = last_y;
    *pressed = pressed_state;
}

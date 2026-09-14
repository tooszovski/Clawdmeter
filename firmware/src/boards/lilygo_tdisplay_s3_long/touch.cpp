#include "../../hal/touch_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <stdlib.h>
#include <string.h>

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
static bool     release_pending = false;
static uint16_t last_x = 0, last_y = 0;
static uint32_t last_read_ms = 0;
static uint32_t press_started_ms = 0;
static uint32_t last_valid_ms = 0;

// Without a fresh INT the controller answers a poll with a junk frame (seen as
// 40 0A 10 .. with "10 fingers"), so a naive poll-while-pressed loop releases
// the touch ~20 ms after the press and LVGL misses every other tap. Contact is
// therefore held until a valid finger-count-0 frame or RELEASE_TIMEOUT_MS
// without a valid report, and never shorter than MIN_PRESS_MS so the indev
// read (33 ms period) always sees the press before the release.
#define RELEASE_TIMEOUT_MS 120
#define MIN_PRESS_MS        60

static void IRAM_ATTR touch_isr(void) { touch_irq = true; }

static void map_to_landscape(uint16_t px, uint16_t py, uint16_t* lx, uint16_t* ly) {
    // Inverse of display.cpp's rotate mapping.
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

// Returns 1 = valid contact report (coords updated), 0 = valid "no finger"
// report, -1 = bus error or junk frame (caller keeps its state).
static int read_controller(void) {
    uint8_t buf[8] = {0};
    Wire.beginTransmission(AXS_TOUCH_ADDR);
    Wire.write(READ_CMD, sizeof(READ_CMD));
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)AXS_TOUCH_ADDR, (uint8_t)sizeof(buf)) != sizeof(buf)) return -1;
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = Wire.read();

    const uint8_t fingers = buf[1];
    if (fingers > 2) return -1;            // junk frame
    if (fingers == 0) return 0;
    // Event nibble (buf[2] >> 4) is 0x4 on every contact report from this
    // unit (LilyGo's example expects 0x8) — the finger count is the signal.
    const uint16_t raw_x = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];
    const uint16_t raw_y = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    if (raw_x >= PANEL_WIDTH || raw_y >= PANEL_HEIGHT) return -1;
    const uint16_t px = raw_x;
    const uint16_t py = PANEL_HEIGHT - 1 - raw_y;
    map_to_landscape(px, py, &last_x, &last_y);
    return 1;
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

// Serial "tdbg": force a controller read every 100 ms and print the raw
// frame + INT level regardless of interrupts (diagnostics).
static bool dbg_poll = false;
int  display_debug_set_mode(int m);
void display_debug_log(bool on);
extern "C" bool board_debug_cmd(const char* cmd) {
    if (strncmp(cmd, "disp ", 5) == 0) {
        int m = display_debug_set_mode(atoi(cmd + 5));
        Serial.printf("display push mode = %d\n", m);
        return true;
    }
    if (strcmp(cmd, "displog") == 0) { static bool on = false; on = !on; display_debug_log(on); Serial.printf("display log %d\n", on); return true; }
    if (strcmp(cmd, "redraw") == 0) { lv_obj_invalidate(lv_screen_active()); Serial.println("redraw"); return true; }
    if (strcmp(cmd, "reboot") == 0) { Serial.println("rebooting"); Serial.flush(); delay(50); ESP.restart(); return true; }
    if (strcmp(cmd, "i2cscan") == 0) {
        Serial.print("i2c:");
        for (uint8_t a = 1; a < 127; a++) {
            Wire.beginTransmission(a);
            if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
        }
        Serial.println();
        return true;
    }
    if (strcmp(cmd, "tdbg") == 0) {
        dbg_poll = !dbg_poll;
        Serial.printf("touch debug poll %s\n", dbg_poll ? "ON" : "OFF");
        return true;
    }
    return false;
}

void touch_hal_read(uint16_t* x, uint16_t* y, bool* pressed) {
    const uint32_t now = millis();
    if (dbg_poll) {
        static uint32_t dbg_poll_ms = 0;
        if (now - dbg_poll_ms >= 100) {
            dbg_poll_ms = now;
            uint8_t buf[8] = {0};
            Wire.beginTransmission(AXS_TOUCH_ADDR);
            Wire.write(READ_CMD, sizeof(READ_CMD));
            int e = Wire.endTransmission(false);
            int n = (e == 0) ? Wire.requestFrom((uint8_t)AXS_TOUCH_ADDR, (uint8_t)8) : 0;
            for (int i = 0; i < n && i < 8; i++) buf[i] = Wire.read();
            Serial.printf("tdbg int=%d i2c=%d n=%d raw %02X %02X %02X %02X %02X %02X %02X %02X irq=%d\n",
                digitalRead(TP_INT), e, n, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
                touch_irq ? 1 : 0);
        }
    }
    // Read ONLY on INT. A blind poll between reports made the controller
    // stall the I2C bus for seconds (loop frozen, LVGL never saw the release
    // until the next touch — which is why only double taps ever registered).
    // The controller keeps reporting while a finger is down, so the release is
    // "no report for RELEASE_TIMEOUT_MS".
    bool want_read = false;
    if (touch_irq) { touch_irq = false; want_read = true; }

    if (want_read) {
        last_read_ms = now;
        int r = read_controller();
        if (r == 1) {
            if (!pressed_state) press_started_ms = now;
            pressed_state = true;
            release_pending = false;
            last_valid_ms = now;
        } else if (r == 0 && pressed_state) {
            release_pending = true;
        }
        // r == -1: junk / bus hiccup — keep the current state.
    }

    if (pressed_state &&
        (release_pending || now - last_valid_ms >= RELEASE_TIMEOUT_MS) &&
        now - press_started_ms >= MIN_PRESS_MS) {
        pressed_state = false;
        release_pending = false;
    }

    *x = last_x;
    *y = last_y;
    *pressed = pressed_state;
}

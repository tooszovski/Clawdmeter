#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

// AXS15231B over QSPI, portrait 180x640 controller geometry. LVGL runs at
// 640x180; every flushed strip is rotated here into panel coordinates. The
// rotation quadrant is fixed for the session (board_rotation_quadrant()), so
// unlike the AMOLED-2.16 port there is no IMU and no transition ramp.
//
// Brightness: the panel's own brightness command is not wired to anything
// useful on this LCD — the PT4103 backlight driver on GPIO1 takes LEDC PWM,
// which keeps the idle fade working unchanged.

// Strip buffer sized to the largest LVGL partial flush (LCD_WIDTH × BUF_LINES
// in main.cpp = 640 × 40 px).
#define ROT_BUF_LINES 40
static uint16_t* rot_buf = nullptr;

static Arduino_DataBus*   bus = nullptr;
static Arduino_AXS15231B* gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // Portrait, rotation 0 — orientation is handled in software below. The
    // default init sequence in Arduino_GFX is the 180x640 one this panel needs.
    gfx = new Arduino_AXS15231B(bus, LCD_RESET, 0 /* rotation */, false /* ips */,
                                PANEL_WIDTH, PANEL_HEIGHT);
}

void display_hal_begin(void) {
    // Backlight off while the panel initializes (avoids a white flash).
    ledcAttach(LCD_BL, 2000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 0);

    gfx->begin(LCD_QSPI_HZ);
    gfx->fillScreen(0x0000);

    rot_buf = (uint16_t*)heap_caps_malloc(LCD_WIDTH * ROT_BUF_LINES * 2, MALLOC_CAP_SPIRAM);
    if (!rot_buf) Serial.println("display: rot_buf alloc failed");

    ledcWrite(LCD_BL, 200);
    Serial.printf("Display AXS15231B %dx%d (panel %dx%d), rotation %u\n",
        LCD_WIDTH, LCD_HEIGHT, PANEL_WIDTH, PANEL_HEIGHT, board_rotation_quadrant());
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_fill_screen(uint16_t color) {
    if (gfx) gfx->fillScreen(color);
}

// Rotate a w×h logical strip at (sx, sy) into rot_buf and compute its
// destination rectangle on the portrait panel.
//   quadrant 1 (90° CW):  logical (x,y) -> panel (PANEL_WIDTH-1-y, x)
//   quadrant 3 (270° CW): logical (x,y) -> panel (y, PANEL_HEIGHT-1-x)
static void rotate_strip(const uint16_t* src, int32_t w, int32_t h,
                         int32_t sx, int32_t sy, uint8_t r,
                         int32_t* dx, int32_t* dy, int32_t* dw, int32_t* dh) {
    *dw = h; *dh = w;
    if (r == 1) {
        *dx = PANEL_WIDTH - sy - h;
        *dy = sx;
        for (int32_t y = 0; y < h; y++) {
            const uint16_t* row = src + y * w;
            const int32_t col = h - 1 - y;
            for (int32_t x = 0; x < w; x++) rot_buf[x * h + col] = row[x];
        }
    } else {  // 3
        *dx = sy;
        *dy = PANEL_HEIGHT - sx - w;
        for (int32_t y = 0; y < h; y++) {
            const uint16_t* row = src + y * w;
            for (int32_t x = 0; x < w; x++) rot_buf[(w - 1 - x) * h + y] = row[x];
        }
    }
}

void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!gfx) return;
    if (!rot_buf || h > ROT_BUF_LINES) {
        // Should not happen (BUF_LINES == ROT_BUF_LINES); draw unrotated so
        // something is visible rather than nothing.
        gfx->draw16bitRGBBitmap(x, y, (uint16_t*)pixels, w, h);
        return;
    }
    int32_t dx, dy, dw, dh;
    rotate_strip(pixels, w, h, x, y, board_rotation_quadrant(), &dx, &dy, &dw, &dh);
    gfx->draw16bitRGBBitmap(dx, dy, rot_buf, dw, dh);
}

void display_hal_tick(void) {
    // Fixed orientation — nothing to do.
}

// Keep flush regions even-aligned on both axes; the logical y axis becomes the
// panel's column axis after rotation, and QSPI panels of this class prefer
// even column windows. Harmless if the controller doesn't care.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}

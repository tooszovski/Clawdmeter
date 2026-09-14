#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <string.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

// AXS15231B over QSPI, portrait 180x640 controller geometry. LVGL runs at
// 640x180 and every flushed strip is rotated into a full-frame shadow buffer
// in PSRAM; the panel is then refreshed as ONE window (0,0,180,640) per loop
// iteration from display_hal_tick().
//
// Two hardware findings behind this file (verified on the board):
//   * Partial windows scramble on this panel — a label redraw or a bar update
//     comes out as garbage, while full-height boot strips render fine. LilyGo's
//     own LVGL demo runs with full_refresh = 1. A full frame is 230 KB, about
//     15-20 ms over 32 MHz QSPI, pushed only when something changed.
//   * Mainline Arduino_GFX's long vendor init for generic 180x640 AXS modules
//     kills the touch engine of this panel (one INT, then silence, junk on
//     poll). LilyGo's short init sequence (see axs_lilygo_init) keeps touch
//     alive, so it is used instead.
//
// Brightness: the PT4103 backlight driver on GPIO1 takes LEDC PWM, which
// keeps the idle fade working unchanged.

static uint16_t* fb = nullptr;          // PANEL_WIDTH x PANEL_HEIGHT RGB565, panel orientation
static bool      fb_dirty = false;

static Arduino_DataBus*   bus = nullptr;
static Arduino_AXS15231B* gfx = nullptr;

// Panel init: the SHORT sequence LilyGo's own Arduino_GFX fork uses for this
// board (sleep out, normal mode, 16-bit pixels, display on, brightness ctrl),
// instead of mainline's long vendor register dump for a generic 180x640 AXS
// module. The vendor dump reprograms timing/scan registers the LilyGo panel
// already has right, and it coincided with scrambled partial windows and a
// touch engine that stops reporting.
static const uint8_t axs_lilygo_init[] = {
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x11,        // SLPOUT
    END_WRITE,
    DELAY, 120,
    BEGIN_WRITE,
    WRITE_COMMAND_8, 0x13,        // NORON
    WRITE_COMMAND_8, 0x20,        // INVOFF
    WRITE_C8_D8, 0x3A, 0x05,      // COLMOD 16 bpp
    WRITE_COMMAND_8, 0x29,        // DISPON
    WRITE_C8_D8, 0x53, 0x28,      // brightness control on + dimming
    WRITE_C8_D8, 0x51, 0x00,      // panel brightness (backlight is PWM anyway)
    WRITE_C8_D8, 0x58, 0x00,      // sunlight-readability enhancement off
    END_WRITE,
    DELAY, 10,
};

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // Portrait, rotation 0 — orientation is handled in software below. The
    // default init sequence in Arduino_GFX is the 180x640 one this panel needs.
    gfx = new Arduino_AXS15231B(bus, LCD_RESET, 0 /* rotation */, false /* ips */,
                                PANEL_WIDTH, PANEL_HEIGHT, 0, 0, 0, 0,
                                axs_lilygo_init, sizeof(axs_lilygo_init));
}

// Push strategy, switchable at runtime for diagnostics (serial: `disp N`):
//   0 = full frame once per dirty loop (window always 0,0,180,640)
//   1 = per-strip windows, exactly the rotated LVGL area
//   2 = per-strip, but full-height columns (rows 0..639, only dx..dx+dw)
static int push_mode = 0;   // full frame by default; `disp 1/2` for diagnostics
static uint16_t* strip_buf = nullptr;    // PANEL_WIDTH x PANEL_HEIGHT scratch for modes 1/2
static bool      dbg_log = false;

static void push_frame(void) {
    if (!gfx || !fb) return;
    gfx->draw16bitRGBBitmap(0, 0, fb, PANEL_WIDTH, PANEL_HEIGHT);
    fb_dirty = false;
}

// Copy a fb sub-rectangle (panel coords) into strip_buf and push it as one window.
static void push_rect(int32_t dx, int32_t dy, int32_t dw, int32_t dh) {
    if (!gfx || !fb || !strip_buf) return;
    for (int32_t r = 0; r < dh; r++)
        memcpy(strip_buf + (size_t)r * dw, fb + (size_t)(dy + r) * PANEL_WIDTH + dx, (size_t)dw * 2);
    gfx->draw16bitRGBBitmap(dx, dy, strip_buf, dw, dh);
    if (dbg_log) Serial.printf("push rect %ld,%ld %ldx%ld\n", (long)dx, (long)dy, (long)dw, (long)dh);
}

int display_debug_set_mode(int m) {
    if (m >= 0 && m <= 2) push_mode = m;
    return push_mode;
}
void display_debug_log(bool on) { dbg_log = on; }

void display_hal_begin(void) {
    // Backlight off while the panel initializes (avoids a white flash).
    ledcAttach(LCD_BL, 2000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 0);

    gfx->begin(LCD_QSPI_HZ);
    gfx->fillScreen(0x0000);

    fb = (uint16_t*)heap_caps_malloc((size_t)PANEL_WIDTH * PANEL_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (fb) memset(fb, 0, (size_t)PANEL_WIDTH * PANEL_HEIGHT * 2);
    else    Serial.println("display: framebuffer alloc failed");
    strip_buf = (uint16_t*)heap_caps_malloc((size_t)PANEL_WIDTH * PANEL_HEIGHT * 2, MALLOC_CAP_SPIRAM);

    ledcWrite(LCD_BL, 200);
    Serial.printf("Display AXS15231B %dx%d (panel %dx%d), rotation %u, full-frame refresh\n",
        LCD_WIDTH, LCD_HEIGHT, PANEL_WIDTH, PANEL_HEIGHT, board_rotation_quadrant());
}

void display_hal_set_brightness(uint8_t level) {
    ledcWrite(LCD_BL, level);
}

void display_hal_fill_screen(uint16_t color) {
    if (!fb) { if (gfx) gfx->fillScreen(color); return; }
    for (size_t i = 0; i < (size_t)PANEL_WIDTH * PANEL_HEIGHT; i++) fb[i] = color;
    push_frame();
}

// Rotate a w×h logical strip at (sx, sy) into the shadow frame.
//   quadrant 1 (90° CW):  logical (x,y) -> panel (PANEL_WIDTH-1-y, x)
//   quadrant 3 (270° CW): logical (x,y) -> panel (y, PANEL_HEIGHT-1-x)
void display_hal_draw_bitmap(int32_t x, int32_t y, int32_t w, int32_t h,
                             const uint16_t* pixels) {
    if (!fb) return;
    const uint8_t r = board_rotation_quadrant();
    // Destination rectangle on the panel for this strip.
    int32_t dx, dy;
    if (r == 1) { dx = PANEL_WIDTH - y - h;  dy = x; }
    else        { dx = y;                    dy = PANEL_HEIGHT - x - w; }
    const int32_t dw = h, dh = w;
    for (int32_t yy = 0; yy < h; yy++) {
        const int32_t ly = y + yy;
        if (ly < 0 || ly >= LCD_HEIGHT) continue;
        const uint16_t* row = pixels + (size_t)yy * w;
        for (int32_t xx = 0; xx < w; xx++) {
            const int32_t lx = x + xx;
            if (lx < 0 || lx >= LCD_WIDTH) continue;
            int32_t px, py;
            if (r == 1) { px = PANEL_WIDTH - 1 - ly;  py = lx; }
            else        { px = ly;                    py = PANEL_HEIGHT - 1 - lx; }
            fb[(size_t)py * PANEL_WIDTH + px] = row[xx];
        }
    }
    if (push_mode == 1)      push_rect(dx, dy, dw, dh);
    else if (push_mode == 2) push_rect(dx, 0, dw, PANEL_HEIGHT);
    else                     fb_dirty = true;
}

// Called once per loop while awake: push the frame if anything changed.
void display_hal_tick(void) {
    if (fb_dirty) push_frame();
}

// Windows are always the full panel; no alignment constraints to enforce.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    *x1 = *x1 & ~1;
    *y1 = *y1 & ~1;
    *x2 = *x2 | 1;
    *y2 = *y2 | 1;
}

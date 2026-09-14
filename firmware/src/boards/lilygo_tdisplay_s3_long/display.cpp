#include "../../hal/display_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <esp_heap_caps.h>

// AXS15231B over QSPI, portrait 180x640 controller geometry. LVGL runs at
// 640x180 and every flushed strip is rotated into a full-frame shadow buffer
// in PSRAM; the panel is then refreshed as ONE window (0,0,180,640) per loop
// iteration from display_hal_tick().
//
// Why a full-frame push instead of per-strip windows: on this panel the boot
// strips (full-height, partial-width windows) render fine, but small partial
// windows — a label redraw, a bar update — come out scrambled. LilyGo's own
// LVGL demo for the board runs with full_refresh = 1 for the same reason. A
// full frame is 230 KB ≈ 15-20 ms over 32 MHz QSPI, once per dirty loop.
//
// Brightness: the PT4103 backlight driver on GPIO1 takes LEDC PWM, which
// keeps the idle fade working unchanged.

static uint16_t* fb = nullptr;          // PANEL_WIDTH x PANEL_HEIGHT RGB565, panel orientation
static bool      fb_dirty = false;

static Arduino_DataBus*   bus = nullptr;
static Arduino_AXS15231B* gfx = nullptr;

void display_hal_init(void) {
    bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
    // Portrait, rotation 0 — orientation is handled in software below. The
    // default init sequence in Arduino_GFX is the 180x640 one this panel needs.
    gfx = new Arduino_AXS15231B(bus, LCD_RESET, 0 /* rotation */, false /* ips */,
                                PANEL_WIDTH, PANEL_HEIGHT);
}

static void push_frame(void) {
    if (!gfx || !fb) return;
    gfx->draw16bitRGBBitmap(0, 0, fb, PANEL_WIDTH, PANEL_HEIGHT);
    fb_dirty = false;
}

void display_hal_begin(void) {
    // Backlight off while the panel initializes (avoids a white flash).
    ledcAttach(LCD_BL, 2000 /* Hz */, 8 /* bits */);
    ledcWrite(LCD_BL, 0);

    gfx->begin(LCD_QSPI_HZ);
    gfx->fillScreen(0x0000);

    fb = (uint16_t*)heap_caps_malloc((size_t)PANEL_WIDTH * PANEL_HEIGHT * 2, MALLOC_CAP_SPIRAM);
    if (fb) memset(fb, 0, (size_t)PANEL_WIDTH * PANEL_HEIGHT * 2);
    else    Serial.println("display: framebuffer alloc failed");

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
    fb_dirty = true;
}

// Called once per loop while awake: push the frame if anything changed.
void display_hal_tick(void) {
    if (fb_dirty) push_frame();
}

// Windows are always the full panel; no alignment constraints to enforce.
void display_hal_round_area(int32_t* x1, int32_t* y1, int32_t* x2, int32_t* y2) {
    (void)x1; (void)y1; (void)x2; (void)y2;
}

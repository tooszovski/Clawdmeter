#include "../../hal/input_hal.h"

// The only MCU-wired button (BOOT / GPIO0) is the PWR-role button and lives in
// power.cpp, so there is no HID primary/secondary button on this board.

void input_hal_init(void) {}

bool input_hal_is_held(InputButton btn) {
    (void)btn;
    return false;
}

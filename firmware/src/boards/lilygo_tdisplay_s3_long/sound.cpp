#include "../../hal/sound_hal.h"

// No speaker or buzzer on this board — the session-reset chime is a no-op.

void sound_hal_init(void) {}
void sound_hal_tick(void) {}
void sound_hal_play_reset(void) {}

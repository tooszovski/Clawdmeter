#pragma once
#include "data.h"
#include "ble.h"

enum screen_t {
    SCREEN_SPLASH,
    SCREEN_USAGE,
    SCREEN_COUNT,
};

void ui_init(void);
void ui_update(const UsageData* data);
// Per-account columns (wide layouts only; a no-op elsewhere). `accounts` holds
// `count` entries in stable display order, each with label + age_s filled.
void ui_update_accounts(const UsageData* accounts, int count);
void ui_tick_anim(void);
void ui_show_screen(screen_t screen);
// Screen to land on after boot: the splash on square/portrait boards, the
// usage columns on the wide desk-meter layout (tap still flips to the splash).
screen_t ui_default_screen(void);
void ui_toggle_splash(void);
screen_t ui_get_current_screen(void);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);

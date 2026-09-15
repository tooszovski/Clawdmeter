#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // utilization 0-100 (5h window Pro/Max; spending % Enterprise)
    int session_reset_mins;  // minutes until reset
    float weekly_pct;        // 7-day utilization (Pro/Max only; 0 for Enterprise)
    int weekly_reset_mins;   // minutes until weekly reset (Pro/Max only)
    char status[16];         // "allowed", "limited", etc.
    bool chime;              // play the session-reset chime; false unless daemon opts in
    bool enterprise;         // true = Enterprise spending-limit account
    int time_pct;            // 0-100: fraction of billing period elapsed (Enterprise)
    int period_days;         // total billing period length in days (Enterprise)
    char reset_date[12];     // formatted reset date e.g. "Jul 1" (Enterprise)
    long clock_epoch;        // local wall-clock epoch (s) from daemon; 0 = not provided
    int  clock_fmt;          // 12 or 24 (hour format from daemon); defaults to 24
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    // Multi-account payloads ("accounts":[...]) fill one UsageData per
    // account with the two fields below; single-account payloads leave them
    // empty / -1.
    char label[16];          // account label shown as the column header
    int  age_s;              // seconds since the host last refreshed this account; -1 = unknown
    // Optional model-scoped weekly window (e.g. "Fable" on Max plans).
    char  model_label[12];   // "" = none
    float model_pct;         // -1 = none
    int   model_reset_mins;
    // Agent working/idle flag from the host's Claude Code hooks ("ag").
    int   agent;             // 1 = working, 0 = idle (waiting for you), -1 = unknown
};

// Max accounts the wide two-column layout can show side by side.
#define MAX_ACCOUNTS 2

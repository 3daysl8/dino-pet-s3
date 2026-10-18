// Countdown timers set by voice ("a timer for 10 minutes"). Stored as RTC
// wall-clock end times in NVS, so they survive the screen sleeping, deep sleep
// and reboots; power.c wakes the board for the next one.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TIMERS_MAX 4
#define TIMER_LABEL_LEN 21

void timers_load(void);                                   // after settings_load(), before the UI
bool timers_add(uint32_t seconds, const char *label);    // false if all slots are in use
// A timer that is due at or before `now`: copies its label, removes it, returns true.
bool timers_take_due(uint32_t now, char *label, size_t n);
uint32_t timers_next_in(uint32_t now);                    // seconds to the next one (UINT32_MAX = none)
int timers_count(void);

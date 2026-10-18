// Wall-clock time from the PCF85063 RTC. The RTC holds local time; we treat it
// as UTC for epoch maths, which is fine because only differences matter to the pet.
#pragma once

#include <stddef.h>
#include <stdint.h>

void app_clock_init(void);              // sets the RTC from the build time if it was never set
uint32_t app_now(void);                 // seconds since 1970 (RTC wall time)
void app_clock_hhmm(char *buf, size_t n);                       // current time, per the 12/24 h setting
void app_clock_fmt(char *buf, size_t n, unsigned h, unsigned m);  // "19:05" or "7:05"
const char *app_clock_ampm(unsigned h);                         // "AM"/"PM", or "" on a 24-hour clock

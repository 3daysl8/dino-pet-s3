// Battery level, charging estimate, warnings and a CSV log on the SD card.
// Hardware: 1S LiPo (800 mAh fitted) via the ETA6098 charger; voltage on GPIO8
// through a 200k/100k divider (BAT_Driver.c). The charger's STAT pin only drives
// the green LED, so "charging" here is an estimate (computer USB or rising volts).
#pragma once

#include <stdbool.h>
#include <stdint.h>

void battery_sample(void);          // ~1 Hz while awake
uint16_t battery_mv(void);
int battery_pct(void);
bool battery_charging(void);
bool battery_low(void);             // under 15 %
bool battery_critical(void);        // under ~3.35 V and not charging

// Flat: too low to stay awake. The toy goes to sleep whatever the Voice setting says.
#define BATTERY_FLAT_PCT 10
bool battery_flat(void);            // under 10 % and not charging
bool battery_flat_first(void);      // true once per flat spell (kept across deep sleep)
void battery_flat_clear(void);      // charged again: the next flat spell may beep

uint8_t battery_pct_for_mv(uint16_t mv);

// Log: one CSV row per call to /sdcard/battery.csv ("epoch,mv,pct,event").
// Deep-sleep check-ins can't touch the SD card, so they record into RTC memory
// and battery_log_flush() writes those rows on the next full boot.
void battery_log(const char *event);
void battery_log_rtc(uint32_t epoch, uint16_t mv, char event);
void battery_log_flush(void);

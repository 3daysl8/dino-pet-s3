// Internet time (SNTP) -> the PCF85063 RTC, in Melbourne local time
// (AEST/AEDT, daylight saving handled by the TZ rule). Runs whenever WiFi connects.
#pragma once

#include <stdbool.h>

void clock_sync_start(void);
bool clock_sync_done(void);     // synced at least once since boot

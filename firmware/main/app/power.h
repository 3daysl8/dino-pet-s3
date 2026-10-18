// Screen-off and deep sleep. The pet is simulated from the RTC, so the board can
// sleep for real between visits and still wake to the right state.
//
// Wake sources: touch (TP_INT, GPIO4), the BOOT button (GPIO0) as a fallback, and
// a timer: the next alarm, or a 30-minute check-in that updates the pet, beeps if
// it needs attention (daytime only), logs the battery and goes back to sleep
// without starting the screen.
#pragma once

#include <stdbool.h>
#include <stdint.h>

void power_boot_prepare(void);       // first thing in app_main: release sleep pin holds
bool power_woke_by_timer(void);
void power_checkin_and_sleep(void);  // timer wake: never returns (sleeps or restarts)

void power_screen_off(void);
void power_screen_on(void);
bool power_can_deep_sleep(void);     // not on a computer, voice not "always"
void power_deep_sleep(void);         // full-UI path: save, panel off, sleep. Never returns.
void power_deep_sleep_no_timer(void);// critical battery: wake only on touch/BOOT

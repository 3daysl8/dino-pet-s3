// Debug memory report: free heap per region and task stack high-water marks,
// printed as greppable lines on serial:
//   MEM|label|int_free|int_largest|int_min_ever|dma_free|dma_largest|psram_free|psram_largest
//   STK|label|task|unused_bytes
#pragma once

#define MEM_REPORT_ENABLED 1   // report on every screen change and every 30 s
#define MEM_SELFTEST 0         // 1: scenarios, 2: radio sweep, 3: WiFi on/off cycles, 4: alarm days, 5: talk round trip, 6: mic check, 7: tap timeout, 8: WiFi start headroom, 9: voice actions, 10: good morning, 0: off

void mem_report(const char *label);
void mem_report_start(void);    // starts the 30 s timer (call once LVGL is up)
void mem_selftest_start(void);  // scripted scenarios; runs when MEM_SELFTEST is 1 or 2

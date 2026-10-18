#include "battery.h"

#include <stdio.h>
#include <sys/stat.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "BAT_Driver.h"
#include "app_clock.h"
#include "voice.h"

static const char *TAG = "battery";

#define CRITICAL_MV   3350
#define LOG_PATH      "/sdcard/battery.csv"
#define RTC_LOG_MAX   96

static float s_mv_avg;
static uint16_t s_mv_ref;            // reading ~5 min ago, for the rising trend
static int64_t s_ref_us;
static bool s_rising;

typedef struct { uint32_t t; uint16_t mv; char ev; } rtc_row_t;
RTC_NOINIT_ATTR static rtc_row_t s_rtc_rows[RTC_LOG_MAX];
RTC_NOINIT_ATTR static uint32_t s_rtc_count;
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
#define RTC_MAGIC 0x7A3A5E01u

// Resting LiPo voltage -> charge %, typical single-cell curve.
static const uint16_t CURVE_MV[] = {3300, 3610, 3690, 3710, 3730, 3750, 3770, 3790, 3800, 3820, 3840,
                                    3850, 3870, 3910, 3950, 3980, 4020, 4080, 4110, 4150, 4200};

uint8_t battery_pct_for_mv(uint16_t mv)
{
    const int n = sizeof CURVE_MV / sizeof CURVE_MV[0];   // 21 points, 5 % apart
    if (mv <= CURVE_MV[0]) return 0;
    if (mv >= CURVE_MV[n - 1]) return 100;
    for (int i = 1; i < n; i++) {
        if (mv < CURVE_MV[i]) {
            int lo = CURVE_MV[i - 1], hi = CURVE_MV[i];
            return (uint8_t)((i - 1) * 5 + (mv - lo) * 5 / (hi - lo));
        }
    }
    return 100;
}

void battery_sample(void)
{
    float v = BAT_analogVolts * 1000.0f;          // refreshed every 100 ms by Driver_Loop
    if (v < 2000 || v > 5000) return;              // not read yet / nonsense
    s_mv_avg = s_mv_avg < 1 ? v : s_mv_avg * 0.9f + v * 0.1f;
    int64_t now = esp_timer_get_time();
    if (!s_ref_us) { s_ref_us = now; s_mv_ref = (uint16_t)s_mv_avg; }
    if (now - s_ref_us > 300LL * 1000000) {
        s_rising = s_mv_avg - s_mv_ref > 20;
        s_mv_ref = (uint16_t)s_mv_avg;
        s_ref_us = now;
    }
}

uint16_t battery_mv(void) { return (uint16_t)s_mv_avg; }
int battery_pct(void) { return battery_pct_for_mv(battery_mv()); }
bool battery_charging(void) { return voice_usb_connected() || s_rising; }
bool battery_low(void) { return s_mv_avg > 1 && battery_pct() < 15 && !battery_charging(); }
bool battery_critical(void) { return s_mv_avg > 1 && battery_mv() < CRITICAL_MV && !battery_charging(); }
bool battery_flat(void) { return s_mv_avg > 1 && battery_pct() < BATTERY_FLAT_PCT && !battery_charging(); }

// One beep per flat spell: remembered across deep sleep, cleared once it has charged.
RTC_NOINIT_ATTR static uint32_t s_flat_told;
#define FLAT_TOLD 0xF1A70001u

bool battery_flat_first(void)
{
    if (s_flat_told == FLAT_TOLD) return false;
    s_flat_told = FLAT_TOLD;
    return true;
}

void battery_flat_clear(void) { s_flat_told = 0; }

static FILE *open_log(void)
{
    struct stat st;
    bool fresh = stat(LOG_PATH, &st) != 0;
    FILE *f = fopen(LOG_PATH, "a");
    if (f && fresh) fprintf(f, "epoch,mv,pct,event\n");
    return f;
}

void battery_log(const char *event)
{
    if (battery_mv() == 0) return;
    FILE *f = open_log();
    if (!f) return;                                  // no SD card: skip quietly
    fprintf(f, "%lu,%u,%u,%s\n", (unsigned long)app_now(), battery_mv(), battery_pct(), event);
    fclose(f);
}

void battery_log_rtc(uint32_t epoch, uint16_t mv, char event)
{
    if (s_rtc_magic != RTC_MAGIC) { s_rtc_magic = RTC_MAGIC; s_rtc_count = 0; }
    if (s_rtc_count >= RTC_LOG_MAX) return;
    s_rtc_rows[s_rtc_count++] = (rtc_row_t){epoch, mv, event};
}

void battery_log_flush(void)
{
    if (s_rtc_magic != RTC_MAGIC || s_rtc_count == 0) return;
    FILE *f = open_log();
    if (!f) return;
    for (uint32_t i = 0; i < s_rtc_count && i < RTC_LOG_MAX; i++) {
        const rtc_row_t *r = &s_rtc_rows[i];
        fprintf(f, "%lu,%u,%u,%s\n", (unsigned long)r->t, r->mv, battery_pct_for_mv(r->mv),
                r->ev == 'T' ? "checkin" : "sleep");
    }
    fclose(f);
    ESP_LOGI(TAG, "flushed %lu sleep-time rows", (unsigned long)s_rtc_count);
    s_rtc_count = 0;
}

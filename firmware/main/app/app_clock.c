#include "app_clock.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "PCF85063.h"
#include "settings.h"

static const char *TAG = "clock";

// Build time as a fallback for a never-set RTC ("Oct  8 2026", "16:04:31").
static datetime_t build_time(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int day = 1, year = 2026, h = 0, m = 0, s = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &h, &m, &s);
    const char *p = strstr(months, mon);
    datetime_t t = {
        .year = (uint16_t)year,
        .month = (uint8_t)(p ? (p - months) / 3 + 1 : 1),
        .day = (uint8_t)day,
        .dotw = 0,
        .hour = (uint8_t)h,
        .minute = (uint8_t)m,
        .second = (uint8_t)s,
    };
    return t;
}

void app_clock_init(void)
{
    datetime_t now;
    PCF85063_Read_Time(&now);
    if (now.year < 2026 || now.month == 0 || now.month > 12 || now.day == 0) {
        datetime_t b = build_time();
        ESP_LOGW(TAG, "RTC not set (%04u-%02u-%02u); setting from build time %04u-%02u-%02u %02u:%02u",
                 now.year, now.month, now.day, b.year, b.month, b.day, b.hour, b.minute);
        PCF85063_Set_All(b);
        datetime = b;
    } else {
        ESP_LOGI(TAG, "RTC %04u-%02u-%02u %02u:%02u:%02u", now.year, now.month, now.day,
                 now.hour, now.minute, now.second);
        datetime = now;
    }
}

uint32_t app_now(void)
{
    datetime_t d = datetime;   // refreshed every 100 ms by Driver_Loop
    struct tm tm = {
        .tm_year = d.year - 1900,
        .tm_mon = d.month - 1,
        .tm_mday = d.day,
        .tm_hour = d.hour,
        .tm_min = d.minute,
        .tm_sec = d.second,
    };
    time_t t = mktime(&tm);    // no TZ configured -> UTC
    return t < 0 ? 0 : (uint32_t)t;
}

void app_clock_fmt(char *buf, size_t n, unsigned h, unsigned m)
{
    if (g_settings.clock_12h) snprintf(buf, n, "%u:%02u", h % 12 ? h % 12 : 12, m);
    else snprintf(buf, n, "%02u:%02u", h, m);
}

const char *app_clock_ampm(unsigned h)
{
    return g_settings.clock_12h ? (h < 12 ? "AM" : "PM") : "";
}

void app_clock_hhmm(char *buf, size_t n)
{
    datetime_t d = datetime;
    app_clock_fmt(buf, n, d.hour, d.minute);
}

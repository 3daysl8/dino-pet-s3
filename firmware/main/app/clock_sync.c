#include "clock_sync.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "PCF85063.h"

static const char *TAG = "clock_sync";

// Melbourne: UTC+10, daylight saving (UTC+11) from the first Sunday of October
// to the first Sunday of April.
#define TZ_MELBOURNE "AEST-10AEDT,M10.1.0,M4.1.0/3"

static bool s_started, s_done;

static void on_sync(struct timeval *tv)
{
    setenv("TZ", TZ_MELBOURNE, 1);
    tzset();
    struct tm lt;
    localtime_r(&tv->tv_sec, &lt);
    datetime_t d = {
        .year = (uint16_t)(lt.tm_year + 1900),
        .month = (uint8_t)(lt.tm_mon + 1),
        .day = (uint8_t)lt.tm_mday,
        .dotw = (uint8_t)lt.tm_wday,
        .hour = (uint8_t)lt.tm_hour,
        .minute = (uint8_t)lt.tm_min,
        .second = (uint8_t)lt.tm_sec,
    };
    // The RTC holds local wall time; app_clock.c treats it as the pet's timeline.
    PCF85063_Set_All(d);
    datetime = d;
    // Leave the C library on UTC so mktime() in app_clock.c keeps treating the
    // RTC's wall time as "UTC" (consistent pet maths).
    setenv("TZ", "UTC0", 1);
    tzset();
    s_done = true;
    ESP_LOGI(TAG, "RTC set to %04u-%02u-%02u %02u:%02u:%02u local", d.year, d.month, d.day, d.hour, d.minute, d.second);
}

void clock_sync_start(void)
{
    if (s_started) { esp_sntp_restart(); return; }
    s_started = true;
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("au.pool.ntp.org");
    cfg.sync_cb = on_sync;
    esp_netif_sntp_init(&cfg);
}

bool clock_sync_done(void) { return s_done; }

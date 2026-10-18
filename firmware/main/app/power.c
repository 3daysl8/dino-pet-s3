#include "power.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "BAT_Driver.h"
#include "esp_board_init.h"
#include "PCF85063.h"
#include "ST77916.h"
#include "CST816.h"
#include "app_clock.h"
#include "battery.h"
#include "pet_store.h"
#include "settings.h"
#include "timers.h"
#include "sfx.h"
#include "voice.h"

static const char *TAG = "power";

#define PIN_BACKLIGHT  GPIO_NUM_5
#define PIN_PA         GPIO_NUM_15
#define PIN_TOUCH_INT  GPIO_NUM_4
#define PIN_BOOT       GPIO_NUM_0
#define CHECKIN_S      (30 * 60)
#define BEEP_EVERY_S   (30 * 60)

RTC_NOINIT_ATTR static uint32_t s_last_beep;
RTC_NOINIT_ATTR static uint32_t s_beep_magic;

void power_boot_prepare(void)
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(PIN_BACKLIGHT);
    gpio_hold_dis(PIN_PA);
    rtc_gpio_deinit(PIN_TOUCH_INT);      // back to a normal GPIO for the touch driver
    rtc_gpio_deinit(PIN_BOOT);
    if (s_beep_magic != 0xBEE9u) { s_beep_magic = 0xBEE9u; s_last_beep = 0; }
}

bool power_woke_by_timer(void)
{
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

// Seconds until the next thing that needs the board awake.
static uint32_t next_wake_s(uint32_t now)
{
    uint32_t wait = CHECKIN_S;
    if (g_settings.alarm_on) {
        uint32_t tod = now % 86400u, at = g_settings.alarm_h * 3600u + g_settings.alarm_m * 60u;
        uint32_t until = at > tod ? at - tod : at + 86400u - tod;
        // land just inside the alarm minute, unless the alarm is off that day
        if (until + 2 < wait && settings_alarm_day(now + until)) wait = until + 2;
    }
    uint32_t t = timers_next_in(now);              // a voice timer: wake just after it ends
    if (t != UINT32_MAX && t + 1 < wait) wait = t + 1;
    return wait < 30 ? (t != UINT32_MAX && t + 1 < 30 ? t + 1 : 30) : wait;
}

static void sleep_now(uint32_t seconds)
{
    // Codecs stay powered through deep sleep unless told to stand by.
    esp_board_audio_off();

    // Keep the backlight and speaker amp firmly off while asleep.
    gpio_set_direction(PIN_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BACKLIGHT, 0);
    gpio_hold_en(PIN_BACKLIGHT);
    gpio_set_direction(PIN_PA, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_PA, 0);
    gpio_hold_en(PIN_PA);
    gpio_deep_sleep_hold_en();

    // Touch: only if the line is idle-high now, so a stuck line can't cause wake loops.
    gpio_set_direction(PIN_TOUCH_INT, GPIO_MODE_INPUT);
    if (gpio_get_level(PIN_TOUCH_INT) == 1) {
        rtc_gpio_pullup_en(PIN_TOUCH_INT);
        rtc_gpio_pulldown_dis(PIN_TOUCH_INT);
        esp_sleep_enable_ext0_wakeup(PIN_TOUCH_INT, 0);
    } else {
        ESP_LOGW(TAG, "touch line low, not using it as a wake source");
    }
    // BOOT button as a fallback wake (active low, external pull-up on the board).
    esp_sleep_enable_ext1_wakeup(1ULL << PIN_BOOT, ESP_EXT1_WAKEUP_ANY_LOW);
    if (seconds) esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);

    ESP_LOGI(TAG, "deep sleep for %lu s", (unsigned long)seconds);
    esp_deep_sleep_start();
}

// ---------------------------------------------------------------- timer check-in

void power_checkin_and_sleep(void)
{
    // esp_board_init() has already brought up I2C (RTC) and the audio codec.
    PCF85063_Init();
    PCF85063_Read_Time(&datetime);
    BAT_Init();
    uint16_t mv = (uint16_t)(BAT_Get_Volts() * 1000.0f);
    settings_load();
    uint32_t now = app_now();

    // An alarm is due: boot the full UI so it can ring.
    if (g_settings.alarm_on) {
        uint32_t tod = now % 86400u, at = g_settings.alarm_h * 3600u + g_settings.alarm_m * 60u;
        if (tod >= at && tod < at + 60 && settings_alarm_day(now)) {
            ESP_LOGI(TAG, "alarm due: full boot");
            esp_restart();
        }
    }

    timers_load();
    if (timers_next_in(now) == 0) {
        ESP_LOGI(TAG, "timer due: full boot");
        esp_restart();
    }

    pet_t pet;
    bool have_pet = pet_store_load(&pet);
    if (have_pet) {
        pet_update(&pet, now);
        pet_store_save(&pet);
    }
    battery_log_rtc(now, mv, 'T');

    if (mv && mv < 3350) {
        ESP_LOGW(TAG, "battery critical (%u mV): sleeping until touched", mv);
        sleep_now(0);
    }

    // Daytime nag beep if the pet needs you (rate-limited). Not on a flat battery.
    if (SFX_WARNINGS && have_pet && pet_needs_attention(&pet) && !pet_is_night(now) &&
        battery_pct_for_mv(mv) >= BATTERY_FLAT_PCT &&
        g_settings.sound != SOUND_OFF && now - s_last_beep >= BEEP_EVERY_S) {
        s_last_beep = now;
        gpio_reset_pin(PIN_PA);
        gpio_set_direction(PIN_PA, GPIO_MODE_OUTPUT);
        sfx_init();
        sfx_play(SFX_ALERT);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
    sleep_now(next_wake_s(now));
}

// ---------------------------------------------------------------- full-UI path

void power_screen_off(void)
{
    Set_Backlight(0);
    if (panel_handle) esp_lcd_panel_disp_on_off(panel_handle, false);
}

void power_screen_on(void)
{
    if (panel_handle) esp_lcd_panel_disp_on_off(panel_handle, true);
}

bool power_can_deep_sleep(void)
{
    return !voice_usb_connected() && g_settings.voice != VOICE_ALWAYS;
}

static void panel_sleep(void)
{
    Set_Backlight(0);
    if (panel_handle) esp_lcd_panel_disp_on_off(panel_handle, false);
    if (panel_io_handle) {
        // ST77916 over QSPI: SLPIN (0x10), framed like the driver's tx_param().
        esp_lcd_panel_io_tx_param(panel_io_handle, (0x02 << 24) | (0x10 << 8), NULL, 0);
    }
    // Touch: the driver keeps the CST816 scanning at full power. Let it idle
    // down; a touch still pulls INT low and wakes us. It stays that way through
    // the timer check-ins (they don't reset it); a full boot resets it.
    CST816_AutoSleep(true);
}

void power_deep_sleep(void)
{
    uint32_t now = app_now();
    battery_log("sleep");
    panel_sleep();
    vTaskDelay(pdMS_TO_TICKS(20));
    sleep_now(next_wake_s(now));
}

void power_deep_sleep_no_timer(void)
{
    battery_log("critical");
    panel_sleep();
    vTaskDelay(pdMS_TO_TICKS(20));
    sleep_now(0);
}

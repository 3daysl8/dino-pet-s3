#include "actions.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "Audio_Driver.h"
#include "app_clock.h"
#include "music.h"
#include "radio.h"
#include "settings.h"
#include "talk.h"
#include "timers.h"
#include "ui.h"
#include "ui_clock.h"
#include "ui_light.h"

static const char *TAG = "actions";

#define WAIT_SPEECH_MAX_US (20LL * 1000000)   // don't hold an action forever if playback never ends

static talk_action_t s_pending;
static bool s_has;
static int64_t s_since;

static bool waits_for_speech(const talk_action_t *a)
{
    // The light and the radio change the screen / use the speaker: let Deadlee finish first.
    return a->type == TALK_ACT_NIGHTLIGHT || a->type == TALK_ACT_RADIO;
}

static int find_station(const char *name)
{
    for (int i = 0; name[0] && i < radio_count(); i++)
        if (strcasestr(radio_name(i), name) || strcasestr(name, radio_name(i))) return i;
    return -1;
}

static void run(const talk_action_t *a)
{
    switch (a->type) {
    case TALK_ACT_TIMER:
        if (a->a < 1 || a->a > 4 * 3600) break;
        if (!timers_add((uint32_t)a->a, a->text)) ui_toast("TOO MANY TIMERS");
        break;
    case TALK_ACT_ALARM:
        if (a->a < 0 || a->a > 23 || a->b < 0 || a->b > 59) break;
        g_settings.alarm_h = (uint8_t)a->a;
        g_settings.alarm_m = (uint8_t)a->b;
        g_settings.alarm_days = (uint8_t)(a->c & 0x7F);
        g_settings.alarm_on = 1;
        settings_save();
        break;
    case TALK_ACT_NIGHTLIGHT:
        if (!a->a) {
            if (ui_view() == VIEW_LIGHT) { light_set_sleep_lamp(false); ui_go(VIEW_HOME); }
            break;
        }
        if (a->b >= 0 && a->b <= 3) g_settings.lamp_color = (uint8_t)a->b;
        if (a->c >= 1 && a->c <= 7) g_settings.lamp_level = (uint8_t)a->c;
        if (a->d >= 1 && a->d <= 4) g_settings.lamp_timer = (uint8_t)a->d;
        settings_save();
        ui_wake_screen();
        light_open_sleep_lamp();                  // glow only; a tap turns it off
        break;
    case TALK_ACT_RADIO:
        if (!a->a) {
            if (music_playing()) music_toggle();
            break;
        }
        ui_go(VIEW_MUSIC);                        // builds the music view (loads the stations)
        music_set_source(MUSIC_SRC_RADIO);
        int i = find_station(a->text);
        if (i >= 0) radio_play(i);
        else if (!music_playing()) music_toggle();
        break;
    }
}

void actions_tick(void)
{
    if (!s_has) {
        if (!talk_take_action(&s_pending)) return;
        s_has = true;
        s_since = esp_timer_get_time();
    }
    int64_t waited = esp_timer_get_time() - s_since;
    if (waits_for_speech(&s_pending) && waited < WAIT_SPEECH_MAX_US &&
        (waited < 1500000 || audio_is_playing() || talk_status() == TALK_THINKING))   // playback starts a beat late
        return;
    ESP_LOGI(TAG, "run %d", (int)s_pending.type);
    s_has = false;
    run(&s_pending);
}

void timers_tick(void)
{
    if (clock_ringing() || !timers_count()) return;
    char label[TIMER_LABEL_LEN];
    if (!timers_take_due(app_now(), label, sizeof label)) return;
    ESP_LOGI(TAG, "timer done: %s", label);
    ui_wake_screen();
    ui_go(VIEW_CLOCK);                            // builds the clock view if needed
    clock_ring_timer(label);
}

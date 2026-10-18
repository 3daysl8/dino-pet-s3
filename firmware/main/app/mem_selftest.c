// Memory benchmark: drives the UI through fixed scenarios (all screens, WiFi,
// radio, SD music, voice off/on) and logs a mem_report at each. Set MEM_SELFTEST
// to 1 in mem_report.h, flash, and capture ~150 s of serial. Plays the radio
// quietly for ~40 s; puts back the volume and voice setting afterwards.
// MEM_SELFTEST 2 instead plays every radio station for 10 s and prints
//   SWEEP|index|name|state   (state 2 = RADIO_PLAYING)
// then "SWEEP done" (13 stations ~ 140 s).
#include <stdio.h>

#include "lvgl.h"
#include "mem_report.h"
#include "music.h"
#include "radio.h"
#include "settings.h"
#include "ui.h"
#include "ui_home.h"

#if MEM_SELFTEST == 1

static int s_t;
static uint8_t s_vol, s_voice;

static void step(lv_timer_t *t)
{
    s_t++;
    switch (s_t) {
    case 8:   mem_report("boot-idle"); break;
    case 10:  ui_go(VIEW_RING); break;
    case 12:  ui_go(VIEW_CLOCK); break;
    case 14:  ui_go(VIEW_MUSIC); break;
    case 16:  ui_go(VIEW_TALK); break;
    case 18:  ui_go(VIEW_LIGHT); break;
    case 20:  ui_go(VIEW_STATS); break;
    case 22:  ui_go(VIEW_SETTINGS); break;
    case 24:  ui_go(VIEW_HOME); break;
    case 27:  mem_report("all-screens-built"); break;
    case 28:  ui_go(VIEW_WIFI); break;                       // WiFi on + join saved network
    case 45:  mem_report("wifi-screen-15s"); break;
    case 46:  ui_go(VIEW_SETTINGS); break;                   // releases WiFi
    case 52:  mem_report("wifi-left"); break;
    case 53:  ui_go(VIEW_MUSIC);
              s_vol = g_settings.music_vol; music_set_vol(1);
              music_set_source(MUSIC_SRC_RADIO); music_toggle(); break;
    case 75:  mem_report("radio-20s"); break;
    case 95:  mem_report("radio-40s"); break;
    case 96:  music_toggle(); break;                         // stop radio
    case 102: mem_report("radio-stopped"); break;
    case 103: music_set_source(MUSIC_SRC_SD); music_toggle(); break;
    case 113: mem_report("sd-music-10s"); break;
    case 114: music_toggle(); music_set_vol(s_vol); break;
    case 116: s_voice = g_settings.voice; g_settings.voice = VOICE_OFF; break;   // not saved
    case 122: mem_report("voice-off"); break;
    case 123: g_settings.voice = s_voice; break;
    case 129: mem_report("voice-on"); break;
    case 130: ui_go(VIEW_HOME); break;
    case 133: mem_report("end");
              printf("SELFTEST done\n");
              lv_timer_del(t); break;
    }
}

void mem_selftest_start(void)
{
    lv_timer_create(step, 1000, NULL);
}
#elif MEM_SELFTEST == 2

static int s_t, s_vol;

static void sweep(lv_timer_t *t)
{
    s_t++;
    if (s_t == 1) {
        ui_go(VIEW_MUSIC);
        s_vol = g_settings.music_vol; music_set_vol(1);
        music_set_source(MUSIC_SRC_RADIO);
        music_toggle();                                      // station 0
        return;
    }
    int i = radio_index();
    printf("SWEEP|%d|%s|%d\n", i, radio_name(i), (int)radio_state());
    mem_report("sweep");
    if (i + 1 < radio_count()) {
        music_next();
    } else {
        music_toggle();
        music_set_vol(s_vol);
        printf("SWEEP done\n");
        lv_timer_del(t);
    }
}

void mem_selftest_start(void)
{
    lv_timer_create(sweep, 10 * 1000, NULL);
}
#elif MEM_SELFTEST == 3

#include "wifi_mgr.h"

// WiFi on/off cycles: shows whether start/stop leaks or fragments internal RAM.
static int s_t, s_cycle;

static void cycles(lv_timer_t *t)
{
    s_t++;
    if (s_t % 12 == 1) {
        wifi_mgr_acquire();
    } else if (s_t % 12 == 9) {
        printf("CYCLE|%d|state %d\n", s_cycle, (int)wifi_mgr_state());   // 2 = connected
        wifi_mgr_release();
    } else if (s_t % 12 == 11) {
        char label[16];
        snprintf(label, sizeof label, "cycle-%d", s_cycle);
        mem_report(label);
        if (++s_cycle == 8) { printf("CYCLES done\n"); lv_timer_del(t); }
    }
}

void mem_selftest_start(void)
{
    lv_timer_create(cycles, 1000, NULL);
}
#elif MEM_SELFTEST == 4

#include "app_clock.h"

// Alarm repeat days: today unticked must not ring, today ticked must ring, and
// a one-time alarm (no days) rings once then switches itself off. Restores the
// real settings afterwards. Prints ALARMTEST|case|rang|alarm_on.
static settings_t s_saved;
static int s_t, s_case;

static void arm_next_minute(uint8_t days)
{
    uint32_t next = app_now() + 60;
    g_settings.alarm_h = (uint8_t)(next % 86400u / 3600u);
    g_settings.alarm_m = (uint8_t)(next % 3600u / 60u);
    g_settings.alarm_days = days;
    g_settings.alarm_on = 1;
}

static void alarm_test(lv_timer_t *t)
{
    static const char *const NAMES[3] = {"today-off", "today-on", "once"};
    uint8_t today = (uint8_t)(1u << ((app_now() / 86400u + 3) % 7));
    s_t++;
    if (s_t == 1) s_saved = g_settings;
    if (s_t % 75 == 2) {                                       // arm for the next minute
        arm_next_minute(s_case == 0 ? (uint8_t)(0x7F & ~today) : s_case == 1 ? 0x7F : 0);
    } else if (s_t % 75 == 0) {                               // >60 s later: did it ring?
        printf("ALARMTEST|%s|%d|%d\n", NAMES[s_case], (int)clock_ringing(), (int)g_settings.alarm_on);
        if (clock_ringing()) clock_ring(false);
        if (++s_case == 3) {
            g_settings = s_saved;
            settings_save();
            printf("ALARMTEST done\n");
            lv_timer_del(t);
        }
    }
}

void mem_selftest_start(void)
{
    lv_timer_create(alarm_test, 1000, NULL);
}
#elif MEM_SELFTEST == 5

#include "talk.h"

// Talk round trip: open Talk, send a line; talk.c logs the result with memory.
static int s_t;

static void talk_test(lv_timer_t *t)
{
    s_t++;
    if (s_t == 8) ui_go(VIEW_TALK);
    if (s_t == 10) {
        char state[300];
        home_state_json(state, sizeof state);
        printf("TALKTEST|state|%s\n", state);
        talk_send_text("Hello! How are you feeling?", state);
    }
    if (s_t == 30) { mem_report("talk-30s"); printf("TALKTEST done\n"); lv_timer_del(t); }
}

void mem_selftest_start(void)
{
    lv_timer_create(talk_test, 1000, NULL);
}
#elif MEM_SELFTEST == 6

#include "talk.h"

// Mic check: record the room for 4 s from the Talk screen and send it.
static int s_t;

static void mic_test(lv_timer_t *t)
{
    s_t++;
    if (s_t == 8) ui_go(VIEW_TALK);
    if (s_t == 14) printf("MICTEST|start|%d\n", (int)talk_record_start());
    if (s_t == 18) {
        char state[300];
        home_state_json(state, sizeof state);
        printf("MICTEST|send|%d\n", (int)talk_record_send(state));
    }
    if (s_t == 30) { printf("MICTEST done\n"); lv_timer_del(t); }
}

void mem_selftest_start(void)
{
    lv_timer_create(mic_test, 1000, NULL);
}
#elif MEM_SELFTEST == 7

#include "talk.h"

// Tap-to-talk in a quiet room: the conversation should time out with nothing sent.
static int s_t;

static void tap_test(lv_timer_t *t)
{
    s_t++;
    if (s_t == 8) ui_go(VIEW_TALK);
    if (s_t == 14) printf("TAPTEST|start|%d\n", (int)talk_session_start());
    if (s_t >= 14 && s_t <= 24) printf("TAPTEST|%ds|active=%d listen=%d\n", s_t - 14, (int)talk_session_active(), (int)talk_listen_state());
    if (s_t == 25) { mem_report("tap-end"); printf("TAPTEST done\n"); lv_timer_del(t); }
}

void mem_selftest_start(void)
{
    lv_timer_create(tap_test, 1000, NULL);
}
#elif MEM_SELFTEST == 8

#include <stdlib.h>
#include "esp_heap_caps.h"
#include "wifi_mgr.h"

// WiFi start headroom: before each start, fill internal RAM with ballast so
// less is free, then see if WiFi still connects. Steps up until it fails (a
// failed esp_wifi_init leaks, so later results would be wrong).
//   BALLAST|ballast_kb|free_before|state   (state 2 = connected)
#define BALLAST_CHUNK 2048
static int s_t, s_kb;
static void *s_ballast[64];
static int s_n;
static uint8_t s_name_wake;

static void ballast_fill(int kb)
{
    while (s_n * BALLAST_CHUNK < kb * 1024 && s_n < 64) {
        void *p = heap_caps_malloc(BALLAST_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!p) break;
        s_ballast[s_n++] = p;
    }
}

static void ballast_free(void)
{
    while (s_n) free(s_ballast[--s_n]);
}

static void headroom(lv_timer_t *t)
{
    s_t++;
    if (s_t == 2) { s_name_wake = g_settings.name_wake; g_settings.name_wake = 0; }   // not saved
    if (s_t < 10) return;
    switch ((s_t - 10) % 16) {
    case 0:
        ballast_fill(s_kb);
        mem_report("ballast");
        wifi_mgr_acquire();
        break;
    case 12: {
        int st = (int)wifi_mgr_state();
        printf("BALLAST|%d|%u|%d\n", s_kb, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), st);
        wifi_mgr_release();
        if (st != WIFI_CONNECTED || s_kb >= 100) {
            ballast_free();
            g_settings.name_wake = s_name_wake;
            printf("BALLAST done\n");
            lv_timer_del(t);
        }
        break;
    }
    case 15:
        ballast_free();
        s_kb += 10;
        break;
    }
}

void mem_selftest_start(void)
{
    lv_timer_create(headroom, 1000, NULL);
}
#elif MEM_SELFTEST == 9

#include "settings.h"
#include "talk.h"
#include "timers.h"
#include "ui_clock.h"
#include "wifi_mgr.h"

// Voice actions end to end, through the real bridge: a 1-minute timer and a red
// night light. Prints ACT|... lines; the timer should ring ~60 s after it's set.
static int s_t;

static void say(const char *text)
{
    char state[300];
    home_state_json(state, sizeof state);
    printf("ACT|say|%s|%d\n", text, (int)talk_send_text(text, state));
}

static void act_test(lv_timer_t *t)
{
    s_t++;
    if (s_t == 3) wifi_mgr_acquire();
    if (s_t == 12) say("Set a timer for 1 minute called test");
    if (s_t == 30) printf("ACT|timers|%d\n", timers_count());
    if (s_t == 32) say("Turn on the night light and make it red");
    if (s_t == 50) printf("ACT|light|view=%d colour=%d\n", (int)ui_view(), g_settings.lamp_color);
    if (s_t >= 60 && s_t <= 95 && clock_ringing()) {
        printf("ACT|rang|%ds\n", s_t);
        clock_ring(false);
        s_t = 96;
    }
    if (s_t == 100) {
        printf("ACT|done|timers=%d\n", timers_count());
        wifi_mgr_release();
        lv_timer_del(t);
    }
}

void mem_selftest_start(void)
{
    lv_timer_create(act_test, 1000, NULL);
}
#elif MEM_SELFTEST == 10

#include "talk.h"
#include "wifi_mgr.h"

// Good morning from Deadlee: the ritual request, its line on the Talk screen, then
// the ritual's WiFi let go (prints RIT|... lines).
static int s_t;

static void ritual_test(lv_timer_t *t)
{
    s_t++;
    if (s_t == 10) {
        char state[300];
        home_state_json(state, sizeof state);
        printf("RIT|start|%d\n", (int)talk_routine("morning", state));
    }
    if (s_t == 30) printf("RIT|view=%d status=%d wifi=%d\n", (int)ui_view(), (int)talk_status(), (int)wifi_mgr_state());
    if (s_t == 45) { printf("RIT|done\n"); lv_timer_del(t); }
}

void mem_selftest_start(void)
{
    lv_timer_create(ritual_test, 1000, NULL);
}
#else
void mem_selftest_start(void) {}
#endif

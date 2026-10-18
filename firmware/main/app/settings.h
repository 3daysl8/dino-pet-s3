// User settings, persisted in NVS (namespace "tama", key "settings").
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum { VOICE_OFF = 0, VOICE_USB_ONLY, VOICE_ALWAYS };
enum { WAKE_HI_ANDY = 0, WAKE_HI_ESP, WAKE_COUNT };
enum { SOUND_ON = 0, SOUND_QUIET, SOUND_OFF };

typedef struct {
    uint8_t version;
    uint8_t voice;        // VOICE_*
    uint8_t wake;         // WAKE_* (applies after restart)
    uint8_t sound;        // SOUND_*
    uint8_t bright;       // 0..3 -> 40/60/80/100 %
    uint8_t dim_after;    // 0..2 -> 15/30/60 s
    uint8_t alarm_on;
    uint8_t alarm_h, alarm_m;
    uint8_t lamp_color;   // 0..3: red, amber, blue, white
    uint8_t lamp_level;   // 1..7
    uint8_t lamp_timer;   // 1..4 -> 15/30/60 min / always on (0 = OFF, never saved)
    uint8_t music_vol;    // 0..20
    uint8_t alarm_days;   // bit 0 = Monday .. bit 6 = Sunday; 0 = once (switches off after ringing)
    uint8_t clock_12h;    // 0 = 24-hour clock, 1 = 12-hour with AM/PM
    char pet_name[13];    // what the dino calls itself in Talk (letters, space, ' and -)
    uint8_t name_wake;    // 1 = listen for "Hey <name>" via the server while plugged in
    uint8_t rituals;      // 1 = Deadlee says good morning / goodnight (via the server) when the pet wakes / sleeps
} settings_t;

extern settings_t g_settings;

void settings_load(void);
void settings_save(void);

uint8_t settings_backlight_pct(void);
bool settings_alarm_day(uint32_t t);    // does the alarm ring on the day of local time t?
uint32_t settings_dim_ms(void);
const char *settings_wake_model(void);   // esp-sr model name filter, e.g. "hiandy"
const char *settings_wake_label(uint8_t wake);

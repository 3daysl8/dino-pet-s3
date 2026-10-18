#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "settings";
#define SETTINGS_VERSION 8

settings_t g_settings;

static void defaults(settings_t *s)
{
    *s = (settings_t){
        .version = SETTINGS_VERSION,
        .voice = VOICE_USB_ONLY,
        .wake = WAKE_HI_ANDY,
        .sound = SOUND_ON,
        .bright = 2,
        .dim_after = 1,
        .alarm_on = 0,
        .alarm_h = 7,
        .alarm_m = 0,
        .lamp_color = 1,
        .lamp_level = 4,
        .lamp_timer = 2,
        .music_vol = 13,
        .alarm_days = 0x7F,
        .pet_name = "Andy",
        .name_wake = 1,
        .rituals = 1,
    };
}

// Older saves are upgraded one version at a time, so each change converts once.
static void migrate(settings_t *s)
{
    if (s->version == 1) {                    // v2: music volume 0..8 -> 0..20
        s->music_vol = (uint8_t)((s->music_vol * 20 + 4) / 8);
        s->version = 2;
    }
    if (s->version == 2) {                    // v3: alarm repeat days (every day, as before)
        s->alarm_days = 0x7F;
        s->version = 3;
    }
    if (s->version == 3) {                    // v4: 12/24-hour clock (24-hour, as before)
        s->clock_12h = 0;
        s->version = 4;
    }
    if (s->version == 4) {                    // v5: nightlight redesign
        // 6 colours -> red, amber, blue, white (warm white -> white; blue/purple/green -> blue)
        static const uint8_t colour[6] = {0, 1, 3, 2, 2, 2};
        s->lamp_color = colour[s->lamp_color < 6 ? s->lamp_color : 1];
        s->lamp_level = (uint8_t)((s->lamp_level * 7 + 2) / 5);          // 1..5 -> 1..7
        s->lamp_timer = (uint8_t)((s->lamp_timer & 3) + 1);              // 15/30/60/on -> 1..4 (0 = OFF)
        s->version = 5;
    }
    if (s->version == 5) {                    // v6: the dino's name
        strlcpy(s->pet_name, "Andy", sizeof s->pet_name);
        s->version = 6;
    }
    if (s->version == 6) {                    // v7: "Hey <name>" (on)
        s->name_wake = 1;
        s->version = 7;
    }
    if (s->version == 7) {                    // v8: good morning / goodnight from Deadlee (on)
        s->rituals = 1;
        s->version = 8;
    }
}

void settings_load(void)
{
    defaults(&g_settings);
    nvs_handle_t h;
    if (nvs_open("tama", NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return;
    }
    settings_t s;
    memset(&s, 0, sizeof s);
    size_t len = sizeof(s);
    bool upgraded = false;
    if (nvs_get_blob(h, "settings", &s, &len) == ESP_OK && len <= sizeof(s) &&
        s.version >= 1 && s.version <= SETTINGS_VERSION &&
        (s.version < SETTINGS_VERSION || len == sizeof(s))) {
        uint8_t from = s.version;
        migrate(&s);
        g_settings = s;
        if (from != SETTINGS_VERSION) {
            ESP_LOGI(TAG, "upgraded settings v%u -> v%u", from, SETTINGS_VERSION);
            upgraded = true;
        } else {
            ESP_LOGI(TAG, "loaded v%u", from);
        }
    } else {
        ESP_LOGI(TAG, "no saved settings, using defaults");
    }
    nvs_close(h);
    if (g_settings.music_vol > 20) g_settings.music_vol = 20;
    g_settings.pet_name[sizeof g_settings.pet_name - 1] = 0;
    if (!g_settings.pet_name[0]) strlcpy(g_settings.pet_name, "Andy", sizeof g_settings.pet_name);
    if (upgraded) settings_save();
    if (g_settings.wake >= WAKE_COUNT) g_settings.wake = WAKE_HI_ANDY;
}

void settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("tama", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "settings", &g_settings, sizeof(g_settings));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
}

uint8_t settings_backlight_pct(void)
{
    static const uint8_t pct[] = {40, 60, 80, 100};
    return pct[g_settings.bright & 3];
}

bool settings_alarm_day(uint32_t t)
{
    int weekday = (int)((t / 86400u + 3) % 7);        // 1970-01-01 was a Thursday; 0 = Monday
    return g_settings.alarm_days == 0 || (g_settings.alarm_days >> weekday) & 1;
}

uint32_t settings_dim_ms(void)
{
    static const uint32_t ms[] = {15000, 30000, 60000};
    return ms[g_settings.dim_after < 3 ? g_settings.dim_after : 1];
}

const char *settings_wake_model(void)
{
    return g_settings.wake == WAKE_HI_ESP ? "hiesp" : "hiandy";
}

const char *settings_wake_label(uint8_t wake)
{
    return wake == WAKE_HI_ESP ? "HI ESP" : "HI ANDY";
}

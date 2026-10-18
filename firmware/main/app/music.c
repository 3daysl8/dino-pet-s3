#include "music.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "Audio_Driver.h"
#include "SD_MMC.h"
#include "radio.h"

static const char *TAG = "music";
#define MAX_TRACKS 64
#define NAME_LEN 100            // must match Folder_retrieval()'s File_Name[][100]

static char (*s_names)[NAME_LEN];
static int s_count, s_index;
static bool s_playing, s_started;     // SD only; the radio keeps its own state
static music_src_t s_src = MUSIC_SRC_SD;

music_src_t music_source(void) { return s_src; }

void music_set_source(music_src_t src)
{
    if (src == s_src) return;
    if (s_src == MUSIC_SRC_RADIO) radio_stop();
    else if (s_playing) { audio_stop_play(); s_playing = false; s_started = false; }
    s_src = src;
}

void music_init(void)
{
    radio_init();
    if (!s_names) s_names = heap_caps_calloc(MAX_TRACKS, NAME_LEN, MALLOC_CAP_SPIRAM);
    if (!s_names) return;
    s_count = Folder_retrieval("/sdcard", ".mp3", s_names, MAX_TRACKS);
    ESP_LOGI(TAG, "%d mp3 files on SD", s_count);
}

int music_count(void) { return s_count; }
const char *music_name(int i) { return (s_names && i >= 0 && i < s_count) ? s_names[i] : ""; }
int music_index(void) { return s_index; }
bool music_playing(void) { return s_src == MUSIC_SRC_RADIO ? radio_active() : s_playing; }

static void start(int i)
{
    if (!s_count) return;
    s_index = (i + s_count) % s_count;
    Music_Next_Flag = 0;
    Play_Music("/sdcard", s_names[s_index]);
    s_playing = true;
    s_started = true;
}

void music_toggle(void)
{
    if (s_src == MUSIC_SRC_RADIO) {
        if (radio_active()) radio_stop();
        else radio_play(radio_index());
        return;
    }
    if (!s_count) return;
    if (s_playing) {
        Music_pause();
        s_playing = false;
    } else if (s_started) {
        Music_resume();
        s_playing = true;
    } else {
        start(s_index);
    }
}

void music_next(void)
{
    if (s_src == MUSIC_SRC_RADIO) radio_step(1);
    else if (s_count) start(s_index + 1);
}

void music_prev(void)
{
    if (s_src == MUSIC_SRC_RADIO) radio_step(-1);
    else if (s_count) start(s_index - 1);
}

void music_set_vol(uint8_t level)
{
    if (level > 20) level = 20;
    Volume_adjustment(level * 5);
}

void music_tick(void)
{
    if (s_src == MUSIC_SRC_RADIO) radio_tick();
    else if (s_playing && Music_Next_Flag) music_next();
}

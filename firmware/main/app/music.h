// Music: MP3s from the SD card over the Waveshare Audio_Driver
// (esp_audio_simple_player), or internet radio (radio.c). The calls below act
// on whichever source is selected.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { MUSIC_SRC_SD = 0, MUSIC_SRC_RADIO } music_src_t;

void music_init(void);          // scans /sdcard for .mp3 files, sets up the radio
void music_set_source(music_src_t src);
music_src_t music_source(void);
bool music_playing(void);
void music_toggle(void);
void music_next(void);
void music_prev(void);
void music_set_vol(uint8_t level);   // 0..20
void music_tick(void);               // call periodically: next track / radio reconnects

// SD tracks
int music_count(void);
const char *music_name(int i);
int music_index(void);

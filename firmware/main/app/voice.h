// Glue between the esp-sr tasks (MIC_Speech.c) and the UI. The speech tasks
// run outside LVGL, so they only post events here; the UI polls them.
#pragma once

#include <stdbool.h>

typedef enum { VOICE_EV_NONE = 0, VOICE_EV_WAKE, VOICE_EV_COMMAND, VOICE_EV_TIMEOUT } voice_ev_t;

bool voice_enabled(void);          // per the Voice setting (off / USB only / always)
bool voice_usb_connected(void);

// Called from the speech tasks.
void voice_post(voice_ev_t ev, int command_id);

// Called from the LVGL task. Returns VOICE_EV_NONE when nothing is pending.
voice_ev_t voice_poll(int *command_id);

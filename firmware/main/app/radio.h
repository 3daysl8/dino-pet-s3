// Internet radio (needs WiFi). Stations: built-in list, or /sdcard/stations.txt
// with one "Name|URL" per line if that file exists. Network work runs in a
// worker task, so a bad stream can never freeze the screen.
#pragma once

#include <stdbool.h>

typedef enum { RADIO_IDLE = 0, RADIO_WAIT_WIFI, RADIO_PLAYING, RADIO_NO_WIFI, RADIO_OFFLINE } radio_state_t;

void radio_init(void);          // loads the stations, starts the worker (once)
void radio_play(int i);         // tune to station i (wraps) and play
void radio_stop(void);
void radio_step(int delta);     // next / previous: retunes if playing, else just moves
bool radio_active(void);        // playing or trying to (counts as busy: no dim / deep sleep)
void radio_tick(void);          // call periodically: reconnects dropped streams

int radio_count(void);
const char *radio_name(int i);
int radio_index(void);
radio_state_t radio_state(void);

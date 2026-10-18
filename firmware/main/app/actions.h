// Runs what Deadlee was asked to do by voice (talk.c queues the bridge's
// "actions"): timers, the alarm, the night light and the radio. UI thread only.
#pragma once

void actions_tick(void);        // call often (every 250 ms) from the UI tick
void timers_tick(void);         // call once a second: rings a voice timer that has run out

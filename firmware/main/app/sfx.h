// Chiptune sound effects, generated in code (no files). Non-blocking: queued
// to a player task that drives the ES8311 + speaker amp directly.
#pragma once

typedef enum {
    SFX_BOOT,
    SFX_CHIRP,
    SFX_YUM,
    SFX_NO,
    SFX_PLAY,
    SFX_CLEAN,
    SFX_MED,
    SFX_LIGHT,
    SFX_EVOLVE,
    SFX_ALERT,
    SFX_DEATH,
    SFX_COUNT,
} sfx_t;

// Warning beeps (pet needs care, battery low). 0 = silent for now: the toasts
// and the home-screen alert still show. Alarms and timers are not affected.
#define SFX_WARNINGS 0

void sfx_init(void);
void sfx_play(sfx_t s);

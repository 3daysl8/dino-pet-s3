// Home view: the pet, clock, stat rings, alerts, and the swipe-up care tray.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "pet.h"
#include "voice.h"

lv_obj_t *home_create(void);
void home_tick(void);           // every second, whatever view is showing (the pet lives on)
void home_on_show(void);
bool home_gesture(lv_dir_t dir);
void home_voice(voice_ev_t ev, int cmd);
void home_save(void);
const pet_t *home_pet(void);
bool home_lights_off(void);
bool home_sleeping(void);       // asleep with the lights out
void home_alarm_wake(void);     // alarm rang: wake the pet gently
void home_state_json(char *buf, size_t n);   // pet state for pet-bridge

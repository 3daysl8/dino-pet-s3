// Nightlight view, also the sleep-mode lamp.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

lv_obj_t *light_create(void);
void light_tick(void);
void light_on_show(void);
uint8_t light_backlight_pct(void);
void light_open_sleep_lamp(void);   // sleep mode: tap on = lamp, tap off = back to the pet
void light_set_sleep_lamp(bool on);
bool light_is_sleep_lamp(void);

// Clock + alarm view.
#pragma once

#include <stdbool.h>
#include "lvgl.h"

lv_obj_t *clock_create(void);
void clock_tick(void);
void clock_ring(bool on);       // start / stop the alarm ringing overlay
void clock_ring_timer(const char *label);   // "TIME'S UP!" with the timer's label (view must be built)
bool clock_ringing(void);

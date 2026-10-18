// Music view (SD card + radio). The player itself is music.c.
#pragma once

#include "lvgl.h"

lv_obj_t *musicv_create(void);
void musicv_tick(void);

// Talk view. Recording, the bridge and listening are in talk.c.
#pragma once

#include "lvgl.h"

lv_obj_t *talk_create(void);
void talk_tick(void);
void talk_on_show(void);
void talk_on_hide(void);

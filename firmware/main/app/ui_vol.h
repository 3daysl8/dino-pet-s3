// Speaker volume control shared by Music and Talk: a rainbow arc of ticks around
// the bottom edge that you can drag, with -/+ at its two ends.
#pragma once

#include "lvgl.h"

typedef struct ui_vol ui_vol_t;

// Create it before the buttons that sit near the bottom, so they stay on top.
ui_vol_t *ui_vol_create(lv_obj_t *scr);
void ui_vol_refresh(ui_vol_t *v);     // redraw if the volume was changed somewhere else

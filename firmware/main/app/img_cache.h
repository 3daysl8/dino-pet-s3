// Nearest-neighbour upscaling of the 4-bit indexed pixel-art assets into
// LV_IMG_CF_TRUE_COLOR_ALPHA images in PSRAM. LVGL 8 cannot zoom indexed images
// that its decoder reads line by line, so we pre-scale once and cache forever.
#pragma once

#include "lvgl.h"

const lv_img_dsc_t *img_scaled(const lv_img_dsc_t *src, int scale);

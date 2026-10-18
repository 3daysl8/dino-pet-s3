// LVGL's allocator (LV_MEM_CUSTOM). Prefers PSRAM so the many small UI objects
// don't use up internal RAM that WiFi and esp-sr need; falls back to the
// default heap if PSRAM is ever full. Wired up in the top-level CMakeLists.txt.
#pragma once

#include <stddef.h>
#include "esp_heap_caps.h"

static inline void *lv_psram_malloc(size_t size)
{
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

static inline void *lv_psram_realloc(void *p, size_t size)
{
    return heap_caps_realloc_prefer(p, size, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

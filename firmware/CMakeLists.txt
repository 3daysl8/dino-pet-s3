cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# LVGL allocates through include/lv_psram_mem.h (LV_MEM_CUSTOM=y with
# CONFIG_LV_MEM_CUSTOM_INCLUDE="lv_psram_mem.h"): PSRAM first, so UI objects
# don't eat the internal RAM that WiFi and esp-sr need.
idf_build_set_property(INCLUDE_DIRECTORIES "${CMAKE_CURRENT_LIST_DIR}/include" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_CUSTOM_ALLOC=lv_psram_malloc" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_CUSTOM_REALLOC=lv_psram_realloc" APPEND)
idf_build_set_property(COMPILE_DEFINITIONS "LV_MEM_CUSTOM_FREE=heap_caps_free" APPEND)

project(ESP32-S3-Touch-LCD-1.85C-Test)

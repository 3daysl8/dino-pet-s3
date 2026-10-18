#include "mem_report.h"

#include <stdio.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define MAX_TASKS 48

void mem_report(const char *label)
{
#if MEM_REPORT_ENABLED
    const uint32_t INT = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t DMA = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA;
    printf("MEM|%s|%u|%u|%u|%u|%u|%u|%u\n", label,
           (unsigned)heap_caps_get_free_size(INT),
           (unsigned)heap_caps_get_largest_free_block(INT),
           (unsigned)heap_caps_get_minimum_free_size(INT),
           (unsigned)heap_caps_get_free_size(DMA),
           (unsigned)heap_caps_get_largest_free_block(DMA),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

    // The task table goes in PSRAM so the report doesn't cost internal RAM.
    TaskStatus_t *tasks = heap_caps_malloc(MAX_TASKS * sizeof *tasks, MALLOC_CAP_SPIRAM);
    if (!tasks) return;
    UBaseType_t n = uxTaskGetSystemState(tasks, MAX_TASKS, NULL);
    for (UBaseType_t i = 0; i < n; i++) {
        // On ESP-IDF the high-water mark is in bytes.
        printf("STK|%s|%s|%u\n", label, tasks[i].pcTaskName, (unsigned)tasks[i].usStackHighWaterMark);
    }
    heap_caps_free(tasks);
#else
    (void)label;
#endif
}

#if MEM_REPORT_ENABLED
static void periodic_cb(lv_timer_t *t)
{
    (void)t;
    mem_report("periodic");
}
#endif

void mem_report_start(void)
{
#if MEM_REPORT_ENABLED
    lv_timer_create(periodic_cb, 30 * 1000, NULL);
    mem_report("ui-start");
#endif
}

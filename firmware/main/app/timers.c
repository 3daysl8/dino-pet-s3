#include "timers.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "app_clock.h"

static const char *TAG = "timers";

typedef struct {
    uint32_t end;                       // RTC wall time (app_now()); 0 = free slot
    char label[TIMER_LABEL_LEN];
} slot_t;

static slot_t s_t[TIMERS_MAX];

static void save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("tama", NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, "timers", s_t, sizeof s_t);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
}

void timers_load(void)
{
    memset(s_t, 0, sizeof s_t);
    nvs_handle_t h;
    if (nvs_open("tama", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_t;
    if (nvs_get_blob(h, "timers", s_t, &len) != ESP_OK || len != sizeof s_t) memset(s_t, 0, sizeof s_t);
    nvs_close(h);
    for (int i = 0; i < TIMERS_MAX; i++) s_t[i].label[TIMER_LABEL_LEN - 1] = 0;
}

bool timers_add(uint32_t seconds, const char *label)
{
    for (int i = 0; i < TIMERS_MAX; i++) {
        if (s_t[i].end) continue;
        s_t[i].end = app_now() + seconds;
        strlcpy(s_t[i].label, label ? label : "", sizeof s_t[i].label);
        save();
        ESP_LOGI(TAG, "timer %d: %lu s \"%s\"", i, (unsigned long)seconds, s_t[i].label);
        return true;
    }
    return false;
}

bool timers_take_due(uint32_t now, char *label, size_t n)
{
    for (int i = 0; i < TIMERS_MAX; i++) {
        if (!s_t[i].end || s_t[i].end > now) continue;
        strlcpy(label, s_t[i].label, n);
        memset(&s_t[i], 0, sizeof s_t[i]);
        save();
        return true;
    }
    return false;
}

uint32_t timers_next_in(uint32_t now)
{
    uint32_t best = UINT32_MAX;
    for (int i = 0; i < TIMERS_MAX; i++) {
        if (!s_t[i].end) continue;
        uint32_t in = s_t[i].end > now ? s_t[i].end - now : 0;
        if (in < best) best = in;
    }
    return best;
}

int timers_count(void)
{
    int n = 0;
    for (int i = 0; i < TIMERS_MAX; i++) n += s_t[i].end != 0;
    return n;
}

#include "img_cache.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "img_cache";

#define MAX_ENTRIES 96

typedef struct {
    const lv_img_dsc_t *src;
    int scale;
    lv_img_dsc_t dsc;
} entry_t;

static entry_t s_cache[MAX_ENTRIES];
static int s_count;

const lv_img_dsc_t *img_scaled(const lv_img_dsc_t *src, int scale)
{
    for (int i = 0; i < s_count; i++)
        if (s_cache[i].src == src && s_cache[i].scale == scale) return &s_cache[i].dsc;
    if (s_count == MAX_ENTRIES) {
        ESP_LOGE(TAG, "cache full");
        return src;
    }

    const int sw = src->header.w, sh = src->header.h;
    const int dw = sw * scale, dh = sh * scale;
    const uint8_t *pal = src->data;                 // 16 x BGRA
    const uint8_t *px = src->data + 16 * 4;
    const int stride = (sw + 1) / 2;
    const size_t size = (size_t)dw * dh * LV_IMG_PX_SIZE_ALPHA_BYTE;
    uint8_t *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no PSRAM for %dx%d", dw, dh);
        return src;
    }

    lv_color_t colors[16];
    for (int i = 0; i < 16; i++) colors[i] = lv_color_make(pal[i * 4 + 2], pal[i * 4 + 1], pal[i * 4]);

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            uint8_t b = px[y * stride + x / 2];
            int idx = (x & 1) ? (b & 0x0F) : (b >> 4);
            uint8_t a = pal[idx * 4 + 3];
            for (int dy = 0; dy < scale; dy++) {
                uint8_t *row = buf + ((size_t)(y * scale + dy) * dw + x * scale) * LV_IMG_PX_SIZE_ALPHA_BYTE;
                for (int dx = 0; dx < scale; dx++) {
                    memcpy(row, &colors[idx], sizeof(lv_color_t));
                    row[LV_IMG_PX_SIZE_ALPHA_BYTE - 1] = a;
                    row += LV_IMG_PX_SIZE_ALPHA_BYTE;
                }
            }
        }
    }

    entry_t *e = &s_cache[s_count++];
    e->src = src;
    e->scale = scale;
    e->dsc = (lv_img_dsc_t){
        .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
        .header.w = dw,
        .header.h = dh,
        .data_size = size,
        .data = buf,
    };
    return &e->dsc;
}

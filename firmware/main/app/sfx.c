#include "sfx.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "esp_board_init.h"
#include "music.h"
#include "settings.h"

// Speaker amp enable on GPIO15, defined in Audio_Driver.c.
void Audio_PA_EN(void);
void Audio_PA_DIS(void);

static const char *TAG = "sfx";

// Must match esp_board_init(16000, 2, 16) in main.c.
#define RATE       16000
#define RAMP       48        // samples of fade in/out per note, avoids clicks
#define CHUNK      256       // stereo frames per write

typedef struct { uint16_t hz; uint16_t ms; } note_t;
#define END {0, 0}

// Note frequencies (Hz).
enum { C5 = 523, D5 = 587, E5 = 659, F5 = 698, G5 = 784, A5 = 880, B5 = 988,
       C6 = 1047, E6 = 1319, G6 = 1568, C4 = 262, E4 = 330, G4 = 392, REST = 0 };

static const note_t s_boot[]   = {{C5, 80}, {E5, 80}, {G5, 80}, {C6, 160}, END};
static const note_t s_chirp[]  = {{G6, 40}, {C6, 50}, END};
static const note_t s_yum[]    = {{E5, 70}, {REST, 30}, {E5, 70}, {G5, 120}, END};
static const note_t s_no[]     = {{E4, 120}, {C4, 200}, END};
static const note_t s_play[]   = {{C5, 60}, {E5, 60}, {G5, 60}, {E5, 60}, {C6, 120}, END};
static const note_t s_clean[]  = {{C6, 40}, {G5, 40}, {E5, 40}, {C5, 40}, {E6, 80}, END};
static const note_t s_med[]    = {{A5, 100}, {F5, 100}, {A5, 150}, END};
static const note_t s_light[]  = {{G5, 60}, {C5, 100}, END};
static const note_t s_evolve[] = {{C5, 90}, {E5, 90}, {G5, 90}, {C6, 90}, {E6, 90}, {G6, 250}, END};
static const note_t s_alert[]  = {{B5, 90}, {REST, 60}, {B5, 90}, {REST, 60}, {B5, 90}, END};
static const note_t s_death[]  = {{G4, 250}, {E4, 250}, {C4, 600}, END};

static const note_t *const s_tunes[SFX_COUNT] = {
    [SFX_BOOT] = s_boot, [SFX_CHIRP] = s_chirp, [SFX_YUM] = s_yum, [SFX_NO] = s_no,
    [SFX_PLAY] = s_play, [SFX_CLEAN] = s_clean, [SFX_MED] = s_med, [SFX_LIGHT] = s_light,
    [SFX_EVOLVE] = s_evolve, [SFX_ALERT] = s_alert, [SFX_DEATH] = s_death,
};

static QueueHandle_t s_queue;
static int16_t s_buf[CHUNK * 2];

static void play_note(note_t n)
{
    const int32_t AMP = g_settings.sound == SOUND_QUIET ? 900 : 3000;   // codec volume does the rest
    uint32_t total = (uint32_t)RATE * n.ms / 1000;
    uint32_t half_period = n.hz ? RATE / (2u * n.hz) : 0;
    uint32_t i = 0;
    while (i < total) {
        uint32_t frames = total - i < CHUNK ? total - i : CHUNK;
        for (uint32_t f = 0; f < frames; f++, i++) {
            int32_t v = 0;
            if (half_period) {
                v = ((i / half_period) & 1) ? AMP : -AMP;
                uint32_t edge = i < total - i ? i : total - i;
                if (edge < RAMP) v = v * (int32_t)edge / RAMP;
            }
            s_buf[2 * f] = s_buf[2 * f + 1] = (int16_t)v;
        }
        esp_audio_play(s_buf, (int)(frames * 2 * sizeof(int16_t)), portMAX_DELAY);
    }
}

static void sfx_task(void *arg)
{
    uint8_t id;
    for (;;) {
        if (xQueueReceive(s_queue, &id, portMAX_DELAY) != pdTRUE || id >= SFX_COUNT) continue;
        Audio_PA_EN();
        do {
            for (const note_t *n = s_tunes[id]; n->ms; n++) play_note(*n);
        } while (xQueueReceive(s_queue, &id, 0) == pdTRUE && id < SFX_COUNT);
        play_note((note_t){REST, 40});   // let the last samples drain before the amp turns off
        Audio_PA_DIS();
    }
}

void sfx_init(void)
{
    s_queue = xQueueCreate(4, sizeof(uint8_t));
    xTaskCreatePinnedToCoreWithCaps(sfx_task, "sfx", 3072, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "ready");
}

void sfx_play(sfx_t s)
{
    // Music owns the speaker (and the amp enable pin) while it plays.
    if (g_settings.sound == SOUND_OFF || music_playing()) return;
    uint8_t id = (uint8_t)s;
    if (s_queue) xQueueSend(s_queue, &id, 0);   // drop if busy rather than block the UI
}

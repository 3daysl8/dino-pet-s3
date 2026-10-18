// Offline wake word + short commands (esp-sr AFE + WakeNet + MultiNet).
// Rewritten from the Waveshare demo: no UI calls from these tasks (they post
// to app/voice.c instead), the mic is idle when the Voice setting is off, and a
// fetch error no longer kills the detect task.
#include "MIC_Speech.h"

#include <string.h>
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "../app/settings.h"
#include "../app/voice.h"

static const char *TAG = "speech";

static const esp_afe_sr_iface_t *afe_handle = NULL;
static volatile int task_flag = 0;

static mic_vad_sink_t s_vad_sink;
static volatile bool s_force_listen;
void mic_listen(bool on) { s_force_listen = on; }
void mic_set_vad_sink(mic_vad_sink_t sink) { s_vad_sink = sink; }

// Talk recording: while set, the feed task copies mic 1 here (16 kHz mono).
static int16_t *volatile s_rec_buf;
static volatile size_t s_rec_len, s_rec_max;

void mic_record_start(int16_t *buf, size_t max_samples)
{
    s_rec_len = 0;
    s_rec_max = max_samples;
    s_rec_buf = buf;
}

size_t mic_record_stop(void)
{
    s_rec_buf = NULL;
    vTaskDelay(pdMS_TO_TICKS(60));          // let the feed task finish the chunk it's copying
    return s_rec_len;
}
static srmodel_list_t *models = NULL;

static void feed_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int audio_chunksize = afe_handle->get_feed_chunksize(afe_data);
    int nch = afe_handle->get_feed_channel_num(afe_data);
    int feed_channel = esp_get_feed_channel();
    assert(nch == feed_channel);
    int16_t *i2s_buff = heap_caps_malloc(audio_chunksize * sizeof(int16_t) * feed_channel, MALLOC_CAP_SPIRAM);
    assert(i2s_buff);

    while (task_flag) {
        int16_t *rec = s_rec_buf;
        if (!voice_enabled() && !rec && !s_force_listen) {   // mic idle: saves CPU and battery
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        esp_get_feed_data(true, i2s_buff, audio_chunksize * sizeof(int16_t) * feed_channel);
        if (rec) {                          // Talk is recording: keep mic 1, skip the wake word
            size_t n = s_rec_len;
            for (int i = 0; i < audio_chunksize && n < s_rec_max; i++)
                rec[n++] = i2s_buff[i * feed_channel + 1];      // raw format "RMNM": index 1 = mic 1
            s_rec_len = n;
            continue;
        }
        afe_handle->feed(afe_data, i2s_buff);
    }
    free(i2s_buff);
    vTaskDelete(NULL);
}

static void detect_Task(void *arg)
{
    esp_afe_sr_data_t *afe_data = arg;
    int afe_chunksize = afe_handle->get_fetch_chunksize(afe_data);
    char *mn_name = esp_srmodel_filter(models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *model_data = multinet->create(mn_name, 6000);
    esp_mn_commands_update_from_sdkconfig(multinet, model_data);
    assert(multinet->get_samp_chunksize(model_data) == afe_chunksize);
    ESP_LOGI(TAG, "multinet %s ready", mn_name);

    bool awake = false;
    while (task_flag) {
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);
        if (!res || res->ret_value == ESP_FAIL) {   // e.g. nothing fed while voice is off
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_vad_sink)                            // Talk's "Hey <name>" listener
            s_vad_sink(res->data, res->data_size / 2, res->vad_state == VAD_SPEECH,
                       res->vad_cache_size > 0 ? res->vad_cache : NULL, res->vad_cache_size / 2);
        if (res->wakeup_state == WAKENET_DETECTED) {
            multinet->clean(model_data);
        }
        if ((res->raw_data_channels == 1 && res->wakeup_state == WAKENET_DETECTED) ||
            (res->raw_data_channels > 1 && res->wakeup_state == WAKENET_CHANNEL_VERIFIED)) {
            ESP_LOGI(TAG, "wake word");
            awake = true;
            voice_post(VOICE_EV_WAKE, 0);
        }
        if (!awake) continue;

        esp_mn_state_t mn_state = multinet->detect(model_data, res->data);
        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = multinet->get_results(model_data);
            ESP_LOGI(TAG, "command %d (%s) p=%.2f", r->command_id[0], r->string, r->prob[0]);
            voice_post(VOICE_EV_COMMAND, r->command_id[0]);
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            afe_handle->enable_wakenet(afe_data);
            awake = false;
            voice_post(VOICE_EV_TIMEOUT, 0);
        }
    }
    multinet->destroy(model_data);
    vTaskDelete(NULL);
}

void MIC_Speech_init(void)
{
    models = esp_srmodel_init("model");   // partition label in partitions.csv
    afe_config_t *afe_config = afe_config_init(esp_get_input_format(), models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    const char *wn = esp_srmodel_filter(models, ESP_WN_PREFIX, (char *)settings_wake_model());
    if (wn) afe_config->wakenet_model_name = strdup(wn);   // afe_config_free() may free it
    ESP_LOGI(TAG, "wake word model: %s", afe_config->wakenet_model_name ? afe_config->wakenet_model_name : "?");
    afe_handle = esp_afe_handle_from_config(afe_config);
    esp_afe_sr_data_t *afe_data = afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    task_flag = 1;
    // Stacks in PSRAM: internal RAM is needed by WiFi and the display DMA buffers.
    xTaskCreatePinnedToCoreWithCaps(&detect_Task, "detect", 8 * 1024, (void *)afe_data, 5, NULL, 1, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(&feed_Task, "feed", 8 * 1024, (void *)afe_data, 5, NULL, 0, MALLOC_CAP_SPIRAM);
}

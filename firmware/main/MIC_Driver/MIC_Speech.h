#pragma once

#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "model_path.h"
#include "esp_process_sdkconfig.h"

#include "esp_board_init.h"
#include "ST77916.h"

void MIC_Speech_init(void);

// Talk: record mic 1 (16 kHz mono) into buf; works even when the Voice setting is off.
void mic_record_start(int16_t *buf, size_t max_samples);
size_t mic_record_stop(void);          // returns the number of samples recorded

// Every processed 16 kHz chunk with the voice detector's verdict; cache = the audio
// just before speech was detected (on the first speech chunk), else NULL.
typedef void (*mic_vad_sink_t)(const int16_t *data, int samples, bool speech,
                               const int16_t *cache, int cache_samples);
void mic_set_vad_sink(mic_vad_sink_t sink);
void mic_listen(bool on);              // keep the mic + voice detector running (Talk tap-to-talk)




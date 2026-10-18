#include "talk.h"

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "Audio_Driver.h"
#include "MIC_Speech.h"
#include "settings.h"
#include "voice.h"
#include "wifi_mgr.h"

// secrets_talk.h (never committed) defines PET_BRIDGE_URL and PET_TOKEN.
// Make it with tools/make_talk_secrets.sh, which copies the token from the server.
#if __has_include("secrets_talk.h")
#include "secrets_talk.h"
#define TALK_READY 1
#else
#define TALK_READY 0
#define PET_BRIDGE_URL ""
#define PET_TOKEN ""
#endif

static const char *TAG = "talk";

#define RESP_MAX   2048
#define TEXT_MAX   300
#define STATE_MAX  300
#define WIFI_WAIT_MS 20000

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile talk_status_t s_status = TALK_IDLE;
static char s_req_text[TEXT_MAX], s_req_state[STATE_MAX];
static char s_req_routine[12];                     // "morning"/"bedtime": a ritual, not a question
static bool s_routine_wifi;                        // talk_routine() turned WiFi on: release when done
static int s_routine_idle_s;                       // seconds the ritual has been finished and quiet

#define REC_RATE    16000
#define REC_MAX_S   15
#define REC_MIN_SAMPLES (REC_RATE / 2)            // shorter than half a second = a tap, not speech
static int16_t *s_rec;                             // 44-byte WAV header + samples, in PSRAM
static bool s_recording;
static int64_t s_rec_started_us;
static size_t s_req_samples;                       // > 0: send audio instead of text
static volatile int s_forget;                      // 1 = requested; result 2 = wiped, 3 = failed

// "Hey <name>" listening (clips are collected on the speech engine's detect task).
#define CLIP_MAX       (REC_RATE * 8)               // longest phrase
#define CLIP_MIN       (REC_RATE * 4 / 10)          // shorter than 0.4 s = a noise
#define CLIP_END_MS    700                          // this much silence ends a phrase
#define QUIET_MS       700                          // ignore the room this long after the dino speaks
#define FOLLOW_MS      8000                         // after a reply: talk on without the name
static int16_t *s_clip;                             // 44-byte WAV header + samples, in PSRAM
static size_t s_clip_n, s_clip_speech;
static int s_clip_silence_ms;
static bool s_in_clip;
static volatile bool s_listen_armed, s_clip_ready, s_open_view, s_follow_pending;
static volatile bool s_clip_follow;                 // the clip was said in the follow-up window
static int64_t s_quiet_until, s_follow_until;
static bool s_wifi_held;
static char s_listen_state[STATE_MAX];
#define TAP_WAIT_MS    6000                         // tap, then this long to start talking
static volatile bool s_session;                     // a tap-to-talk conversation is on
static int64_t s_session_until;
static char s_heard[TEXT_MAX], s_reply[TEXT_MAX];

#define ACT_MAX 4
static talk_action_t s_act[ACT_MAX];
static int s_act_n;

static void ensure_task(void);
static void session_check(void);

bool talk_configured(void) { return TALK_READY; }
talk_status_t talk_status(void) { return s_status; }

static void finish(talk_status_t st, const char *heard, const char *reply)
{
    ESP_LOGI(TAG, "%s: reply \"%.80s\" (internal free %u, min %u)", st == TALK_DONE ? "done" : "error",
             reply ? reply : "", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_heard, heard ? heard : "", sizeof s_heard);
    strlcpy(s_reply, reply ? reply : "", sizeof s_reply);
    s_status = st;
    xSemaphoreGive(s_lock);
}

void talk_take(char *heard, size_t heard_n, char *reply, size_t reply_n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(heard, s_heard, heard_n);
    strlcpy(reply, s_reply, reply_n);
    s_status = TALK_IDLE;
    xSemaphoreGive(s_lock);
}

static int32_t num(const cJSON *o, const char *key, int32_t dflt)
{
    const cJSON *v = cJSON_GetObjectItem(o, key);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return cJSON_IsNumber(v) ? (int32_t)v->valuedouble : dflt;
}

// Queue the reply's "actions" for the UI thread. The bridge has already checked
// them; the ranges are checked again where they're used.
static void queue_actions(const cJSON *r)
{
    const cJSON *list = cJSON_GetObjectItem(r, "actions"), *o;
    cJSON_ArrayForEach(o, list) {
        const cJSON *type = cJSON_GetObjectItem(o, "type");
        if (!cJSON_IsString(type)) continue;
        talk_action_t a = {0};
        const char *t = type->valuestring;
        const cJSON *text = NULL;
        if (!strcmp(t, "timer")) {
            a = (talk_action_t){.type = TALK_ACT_TIMER, .a = num(o, "seconds", 0)};
            text = cJSON_GetObjectItem(o, "label");
        } else if (!strcmp(t, "alarm")) {
            a = (talk_action_t){.type = TALK_ACT_ALARM, .a = num(o, "h", -1), .b = num(o, "m", -1), .c = num(o, "days", 0)};
        } else if (!strcmp(t, "nightlight")) {
            a = (talk_action_t){.type = TALK_ACT_NIGHTLIGHT, .a = num(o, "on", 1), .b = num(o, "colour", -1),
                                .c = num(o, "level", -1), .d = num(o, "timer", -1)};
        } else if (!strcmp(t, "radio")) {
            a = (talk_action_t){.type = TALK_ACT_RADIO, .a = num(o, "play", 1)};
            text = cJSON_GetObjectItem(o, "station");
        } else {
            continue;
        }
        if (cJSON_IsString(text)) strlcpy(a.text, text->valuestring, sizeof a.text);
        ESP_LOGI(TAG, "action %s", t);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_act_n < ACT_MAX) s_act[s_act_n++] = a;
        xSemaphoreGive(s_lock);
    }
}

bool talk_take_action(talk_action_t *out)
{
    if (!s_lock || !s_act_n) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool got = s_act_n > 0;
    if (got) {
        *out = s_act[0];
        memmove(s_act, s_act + 1, (size_t)(--s_act_n) * sizeof s_act[0]);
    }
    xSemaphoreGive(s_lock);
    return got;
}

static void put_le(uint8_t *p, uint32_t v, int bytes)
{
    for (int i = 0; i < bytes; i++) p[i] = (uint8_t)(v >> (8 * i));
}

// Fill the 44-byte WAV header in front of the samples.
static void wav_header(uint8_t *h, size_t samples)
{
    uint32_t data = (uint32_t)(samples * 2);
    memcpy(h, "RIFF", 4); put_le(h + 4, 36 + data, 4); memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4); put_le(h + 20, 1, 2); put_le(h + 22, 1, 2);       // PCM, mono
    put_le(h + 24, REC_RATE, 4); put_le(h + 28, REC_RATE * 2, 4);
    put_le(h + 32, 2, 2); put_le(h + 34, 16, 2);
    memcpy(h + 36, "data", 4); put_le(h + 40, data, 4);
}

// POST a text line or a recording to /talk. Returns the HTTP status (or -1).
static int bridge_talk(const char *text, int16_t *wav, size_t samples, const char *mode, const char *state,
                       char *resp, int resp_max, const char *routine)
{
    char *json = NULL;
    const char *payload;
    int len;
    if (samples) {
        wav_header((uint8_t *)wav, samples);
        payload = (const char *)wav;
        len = 44 + (int)samples * 2;
    } else {
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "text", text);
        if (routine && routine[0]) cJSON_AddStringToObject(body, "routine", routine);
        json = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        if (!json) return -1;
        payload = json;
        len = (int)strlen(json);
    }

    char url[96];
    snprintf(url, sizeof url, "%s/talk%s%s", PET_BRIDGE_URL, mode ? "?mode=" : "", mode ? mode : "");
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", samples ? "audio/wav" : "application/json");
    esp_http_client_set_header(c, "X-Pet-Token", PET_TOKEN);
    if (state && state[0]) esp_http_client_set_header(c, "X-Pet-State", state);

    int status = -1;
    if (esp_http_client_open(c, len) == ESP_OK && esp_http_client_write(c, payload, len) == len) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        int got = esp_http_client_read_response(c, resp, resp_max - 1);
        resp[got > 0 ? got : 0] = 0;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (json) cJSON_free(json);
    return status;
}

static int bridge_forget(void)
{
    esp_http_client_config_t cfg = {.url = PET_BRIDGE_URL "/forget", .method = HTTP_METHOD_POST, .timeout_ms = 10000};
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "X-Pet-Token", PET_TOKEN);
    int status = esp_http_client_perform(c) == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    return status;
}

static void talk_task(void *arg)
{
    (void)arg;
    char *resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_clip_ready) {                            // a phrase the pet overheard
            char state[STATE_MAX];
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(state, s_listen_state, sizeof state);
            xSemaphoreGive(s_lock);
            for (int ms = 0; s_session && wifi_mgr_state() != WIFI_CONNECTED && ms < WIFI_WAIT_MS; ms += 250)
                vTaskDelay(pdMS_TO_TICKS(250));                // tapped before WiFi had joined
            int status = resp && wifi_mgr_state() == WIFI_CONNECTED
                       ? bridge_talk("", s_clip, s_clip_n, s_clip_follow ? "follow" : "wake", state, resp, RESP_MAX, NULL) : -1;
            cJSON *r = status == 200 ? cJSON_Parse(resp) : NULL;
            if (cJSON_IsTrue(cJSON_GetObjectItem(r, "wake"))) {
                const cJSON *heard = cJSON_GetObjectItem(r, "heard"), *reply = cJSON_GetObjectItem(r, "reply"),
                            *audio = cJSON_GetObjectItem(r, "audio");
                if (cJSON_IsString(audio) && audio->valuestring[0] == '/') {
                    char url[160];
                    snprintf(url, sizeof url, "%s%s", PET_BRIDGE_URL, audio->valuestring);
                    Play_Url(url);
                }
                queue_actions(r);
                finish(TALK_DONE, cJSON_IsString(heard) ? heard->valuestring : "",
                       cJSON_IsString(reply) ? reply->valuestring : "...");
                s_open_view = true;
                s_follow_pending = true;
                if (s_session) s_session_until = esp_timer_get_time() + 60LL * 1000000;   // until it has spoken
            } else if (status != 200) {
                ESP_LOGW(TAG, "overheard clip: bridge status %d", status);
            }
            cJSON_Delete(r);
            s_clip_ready = false;
            if (s_forget != 1 && s_status != TALK_WAIT_WIFI) continue;
        }

        if (s_forget == 1) {                           // Setup: forget everything
            for (int ms = 0; wifi_mgr_state() != WIFI_CONNECTED && ms < WIFI_WAIT_MS; ms += 250)
                vTaskDelay(pdMS_TO_TICKS(250));
            int status = wifi_mgr_state() == WIFI_CONNECTED ? bridge_forget() : -1;
            ESP_LOGI(TAG, "forget: %d", status);
            wifi_mgr_release();
            s_forget = status == 200 ? 2 : 3;
            continue;
        }

        if (s_status != TALK_WAIT_WIFI) continue;      // woken for something else, no chat queued

        // The Talk screen turned WiFi on; give it time to join.
        s_status = TALK_WAIT_WIFI;
        for (int ms = 0; wifi_mgr_state() != WIFI_CONNECTED && ms < WIFI_WAIT_MS; ms += 250)
            vTaskDelay(pdMS_TO_TICKS(250));
        if (wifi_mgr_state() != WIFI_CONNECTED) {
            if (s_req_routine[0]) { s_req_routine[0] = 0; s_status = TALK_IDLE; continue; }   // a ritual: skip quietly
            finish(TALK_ERROR, "", "NO WIFI - CHECK SETUP");
            continue;
        }

        s_status = TALK_THINKING;
        int status = resp ? bridge_talk(s_req_text, s_rec, s_req_samples, NULL, s_req_state, resp, RESP_MAX,
                                        s_req_routine) : -1;
        bool routine = s_req_routine[0];
        s_req_routine[0] = 0;               // talk_poll() releases its WiFi once the line has played
        if (status != 200) {
            ESP_LOGW(TAG, "bridge status %d", status);
            finish(TALK_ERROR, s_req_text, status < 0 ? "CAN'T REACH SERVER - IS IT ON?" :
                                           status == 401 ? "SERVER SAID NO (TOKEN)" : "SERVER HAD A PROBLEM");
            continue;
        }
        cJSON *r = cJSON_Parse(resp);
        const cJSON *heard = cJSON_GetObjectItem(r, "heard"), *reply = cJSON_GetObjectItem(r, "reply"),
                    *audio = cJSON_GetObjectItem(r, "audio");
        if (cJSON_IsString(audio) && audio->valuestring[0] == '/') {
            char url[160];
            snprintf(url, sizeof url, "%s%s", PET_BRIDGE_URL, audio->valuestring);
            Play_Url(url);                                    // the spoken reply
        }
        queue_actions(r);
        finish(TALK_DONE, cJSON_IsString(heard) ? heard->valuestring : "",
               cJSON_IsString(reply) ? reply->valuestring : "...");
        if (s_listen_armed || s_session) s_follow_pending = true;   // keep listening for an answer
        if (routine && status == 200) s_open_view = true;           // show the line on the Talk screen
        cJSON_Delete(r);
    }
}

// ---------------------------------------------------------------- "Hey <name>"

static void clip_add(const int16_t *d, int n)
{
    if (n <= 0) return;
    if (s_clip_n + n > CLIP_MAX) n = CLIP_MAX - s_clip_n;
    memcpy(s_clip + 22 + s_clip_n, d, n * sizeof(int16_t));
    s_clip_n += n;
}

// Runs on the speech engine's detect task for every 16 kHz chunk.
static void vad_sink(const int16_t *data, int n, bool speech, const int16_t *cache, int cache_n)
{
    int64_t now = esp_timer_get_time();
    bool busy = s_recording || s_clip_ready || s_status == TALK_WAIT_WIFI || s_status == TALK_THINKING;
    if (!(s_listen_armed || s_session) || busy) { s_in_clip = false; return; }
    if (audio_is_playing()) {                          // never answer its own voice
        s_quiet_until = now + QUIET_MS * 1000LL;
        s_in_clip = false;
        return;
    }
    if (now < s_quiet_until) return;
    if (s_follow_pending) {                            // the reply just finished: follow-up window
        s_follow_pending = false;
        s_follow_until = now + FOLLOW_MS * 1000LL;
        if (s_session) s_session_until = s_follow_until;
    }
    if (!s_in_clip) {
        if (!speech) return;
        s_in_clip = true;
        s_clip_n = s_clip_speech = 0;
        s_clip_silence_ms = 0;
        s_clip_follow = s_session || now < s_follow_until;
        if (s_session) s_session_until = now + 60LL * 1000000;    // hearing you: don't time out
        if (cache) clip_add(cache, cache_n);           // the start of the first word
    }
    clip_add(data, n);
    int ms = n * 1000 / REC_RATE;
    if (speech) { s_clip_silence_ms = 0; s_clip_speech += n; }
    else s_clip_silence_ms += ms;
    if (s_clip_silence_ms >= CLIP_END_MS || s_clip_n >= CLIP_MAX) {
        s_in_clip = false;
        if (s_clip_speech >= CLIP_MIN) {
            s_clip_ready = true;
            xTaskNotifyGive(s_task);
        }
    }
}

void talk_poll(const char *state_json)
{
    if (!TALK_READY) return;
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_listen_state, state_json, sizeof s_listen_state);   // pet state for overheard / tap clips
        xSemaphoreGive(s_lock);
    }
    session_check();
    if (s_routine_wifi) {                              // a ritual's WiFi: let go once it has been said
        bool busy = s_req_routine[0] || s_status == TALK_WAIT_WIFI || s_status == TALK_THINKING || audio_is_playing();
        s_routine_idle_s = busy ? 0 : s_routine_idle_s + 1;
        if (s_routine_idle_s >= 3) { wifi_mgr_release(); s_routine_wifi = false; s_routine_idle_s = 0; }
    }
    bool want = g_settings.name_wake && voice_enabled() && voice_usb_connected();
    if (want && !s_clip) {
        s_clip = heap_caps_malloc(44 + CLIP_MAX * 2, MALLOC_CAP_SPIRAM);
        if (!s_clip) return;
    }
    if (want) {
        if (!s_task) {
            if (!s_lock) s_lock = xSemaphoreCreateMutex();
            xTaskCreatePinnedToCoreWithCaps(talk_task, "talk", 6144, NULL, 4, &s_task, 0, MALLOC_CAP_SPIRAM);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_listen_state, state_json, sizeof s_listen_state);
        xSemaphoreGive(s_lock);
        mic_set_vad_sink(vad_sink);
    }
    if (want != s_wifi_held) {                         // WiFi stays on while listening
        if (want) wifi_mgr_acquire();
        else wifi_mgr_release();
        s_wifi_held = want;
    }
    s_listen_armed = want && wifi_mgr_state() == WIFI_CONNECTED;
}

bool talk_listening(void) { return s_listen_armed; }

bool talk_session_start(void)
{
    if (!TALK_READY || s_recording || s_clip_ready || s_status == TALK_WAIT_WIFI || s_status == TALK_THINKING)
        return false;
    if (!s_clip) s_clip = heap_caps_malloc(44 + CLIP_MAX * 2, MALLOC_CAP_SPIRAM);
    if (!s_clip) return false;
    ensure_task();
    s_in_clip = false;
    s_follow_pending = false;
    s_session_until = esp_timer_get_time() + TAP_WAIT_MS * 1000LL;
    s_quiet_until = 0;
    mic_set_vad_sink(vad_sink);
    mic_listen(true);
    s_session = true;
    ESP_LOGI(TAG, "conversation started");
    return true;
}

void talk_session_end(void)
{
    if (s_session) ESP_LOGI(TAG, "conversation ended");
    s_session = false;
    s_in_clip = false;
    s_follow_pending = false;
    s_follow_until = 0;
    mic_listen(false);
}

bool talk_session_active(void) { return s_session; }

talk_listen_t talk_listen_state(void)
{
    if (!s_session) return LISTEN_OFF;
    return s_in_clip ? LISTEN_HEARING : LISTEN_READY;
}

// Called once a second: a conversation ends after a quiet spell.
static void session_check(void)
{
    if (!s_session) return;
    bool waiting_on_bridge = s_clip_ready || s_status == TALK_WAIT_WIFI || s_status == TALK_THINKING;
    if (!waiting_on_bridge && !s_in_clip && !s_follow_pending && !audio_is_playing() &&
        esp_timer_get_time() > s_session_until)
        talk_session_end();
}

bool talk_wants_view(void)
{
    if (!s_open_view) return false;
    s_open_view = false;
    return true;
}

static bool send_request(const char *text, const char *state_json);

bool talk_send_text(const char *text, const char *state_json)
{
    if (text[0]) s_req_samples = 0;                    // a typed/test line, not the recording
    return send_request(text, state_json);
}

bool talk_routine(const char *kind, const char *state_json)
{
    if (!TALK_READY || s_recording || s_session || s_status == TALK_WAIT_WIFI || s_status == TALK_THINKING)
        return false;
    s_req_samples = 0;
    strlcpy(s_req_routine, kind, sizeof s_req_routine);
    if (!s_routine_wifi) { wifi_mgr_acquire(); s_routine_wifi = true; }
    if (send_request("", state_json)) return true;
    s_req_routine[0] = 0;
    wifi_mgr_release();
    s_routine_wifi = false;
    return false;
}

static bool send_request(const char *text, const char *state_json)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!TALK_READY) { finish(TALK_ERROR, "", "TALK ISN'T SET UP YET"); return true; }
    if (s_status == TALK_WAIT_WIFI || s_status == TALK_THINKING) return false;
    if (!s_task)
        xTaskCreatePinnedToCoreWithCaps(talk_task, "talk", 6144, NULL, 4, &s_task, 0, MALLOC_CAP_SPIRAM);
    strlcpy(s_req_text, text, sizeof s_req_text);
    strlcpy(s_req_state, state_json ? state_json : "", sizeof s_req_state);
    s_status = TALK_WAIT_WIFI;
    xTaskNotifyGive(s_task);
    return true;
}

static void ensure_task(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_task)
        xTaskCreatePinnedToCoreWithCaps(talk_task, "talk", 6144, NULL, 4, &s_task, 0, MALLOC_CAP_SPIRAM);
}

bool talk_forget(void)
{
    if (!TALK_READY || s_forget == 1 || s_status == TALK_WAIT_WIFI || s_status == TALK_THINKING) return false;
    ensure_task();
    wifi_mgr_acquire();                                // the worker releases it when done
    s_forget = 1;
    xTaskNotifyGive(s_task);
    return true;
}

int talk_forget_result(void)
{
    int r = s_forget;
    if (r < 2) return 0;
    s_forget = 0;
    return r == 2 ? 1 : -1;
}

bool talk_record_start(void)
{
    if (s_recording || s_status == TALK_WAIT_WIFI || s_status == TALK_THINKING) return false;
    if (!s_rec) s_rec = heap_caps_malloc(44 + REC_RATE * REC_MAX_S * 2, MALLOC_CAP_SPIRAM);
    if (!s_rec) return false;
    mic_record_start(s_rec + 22, REC_RATE * REC_MAX_S);        // samples start after the header
    s_recording = true;
    s_rec_started_us = esp_timer_get_time();
    return true;
}

float talk_record_seconds(void)
{
    return s_recording ? (esp_timer_get_time() - s_rec_started_us) / 1e6f : 0;
}

bool talk_record_send(const char *state_json)
{
    if (!s_recording) return false;
    s_recording = false;
    size_t n = mic_record_stop();
    int peak = 0;
    double sq = 0;
    for (size_t i = 0; i < n; i++) {
        int v = s_rec[22 + i] < 0 ? -s_rec[22 + i] : s_rec[22 + i];
        if (v > peak) peak = v;
        sq += (double)v * v;
    }
    ESP_LOGI(TAG, "recorded %.1f s, peak %d, rms %d", n / (float)REC_RATE, peak, n ? (int)sqrt(sq / n) : 0);
    if (n < REC_MIN_SAMPLES) return false;
    s_req_samples = n;
    bool ok = talk_send_text("", state_json);
    if (!ok) s_req_samples = 0;
    return ok;
}

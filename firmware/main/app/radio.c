#include "radio.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "Audio_Driver.h"
#include "wifi_mgr.h"

extern esp_asp_handle_t handle;  // the simple player, owned by Audio_Driver.c

static const char *TAG = "radio";

// A "#.mp3" suffix tags streams whose URL has no extension (the player picks the
// decoder from the last '.' in the URL; the fragment is never sent to the server).
typedef struct { const char *name; const char *url; } station_t;
static const station_t STATIONS[] = {
    {"iHeartCountry", "http://playerservices.streamtheworld.com/api/livestream-redirect/ARN_IHRT9.mp3"},
    {"KIX Country", "http://playerservices.streamtheworld.com/api/livestream-redirect/KIXCOUNTRY.mp3"},
    {"Totally Country", "http://playerservices.streamtheworld.com/api/livestream-redirect/T_RAD_COUNTRY_S01.mp3"},
    {"IMC Country", "http://servidor36.brlogic.com:7050/live#.mp3"},   // same stream as https, no TLS needed
    {"Gold 104.3", "http://playerservices.streamtheworld.com/api/livestream-redirect/ARN_GOLD1043.mp3"},
    {"KIIS 101.1", "http://playerservices.streamtheworld.com/api/livestream-redirect/ARN_KIIS1011.mp3"},
    {"Nova 100", "http://playerservices.streamtheworld.com/api/livestream-redirect/NOVA_100.mp3"},
    {"Smooth 91.5", "http://playerservices.streamtheworld.com/api/livestream-redirect/SMOOTH915.mp3"},
    {"3AW 693", "http://playerservices.streamtheworld.com/api/livestream-redirect/3AW.mp3"},
    {"SEN 1116", "http://13.54.221.214:8000/sen.mp3"},
    {"Triple R 102.7", "http://ondemand.rrr.org.au/getstream#.mp3"},
    {"3ZZZ 92.3", "http://playerservices.streamtheworld.com/api/livestream-redirect/3ZZZ.mp3"},
    {"Kiss FM 87.6", "http://cc.net2streams.com:8565/kissfm.mp3"},
};
#define MAX_STATIONS 24
static station_t s_st[MAX_STATIONS];
static char (*s_st_text)[2][160];       // SD-loaded names/urls (PSRAM)
static int s_st_count, s_st_index;
// Radio state is shared between the UI (LVGL task) and the radio worker task.
// All slow network work (connect, redirects, stop) happens in the worker so a
// bad stream can never freeze the screen.
static volatile radio_state_t s_radio = RADIO_IDLE;
static volatile bool s_active;           // wanted on (set by the UI, cleared by the worker on failure)
static bool s_wifi_held;                 // worker-only
static int s_reconnects;                 // silent retries on the current station
#define MAX_RECONNECTS 3
static int64_t s_play_since;             // last (re)connect
typedef enum { CMD_PLAY, CMD_RECONNECT, CMD_STOP } radio_cmd_t;
typedef struct { uint8_t cmd; int8_t index; } radio_msg_t;
static QueueHandle_t s_radio_q;

static void load_stations(void)
{
    s_st_count = 0;
    FILE *f = fopen("/sdcard/stations.txt", "r");
    if (f) {
        if (!s_st_text) s_st_text = heap_caps_calloc(MAX_STATIONS, sizeof *s_st_text, MALLOC_CAP_SPIRAM);
        char line[340];
        while (s_st_text && s_st_count < MAX_STATIONS && fgets(line, sizeof line, f)) {
            char *bar = strchr(line, '|');
            if (!bar || line[0] == '#') continue;
            *bar = 0;
            char *url = bar + 1;
            url[strcspn(url, "\r\n")] = 0;
            strlcpy(s_st_text[s_st_count][0], line, 160);
            strlcpy(s_st_text[s_st_count][1], url, 160);
            s_st[s_st_count] = (station_t){s_st_text[s_st_count][0], s_st_text[s_st_count][1]};
            s_st_count++;
        }
        fclose(f);
        ESP_LOGI(TAG, "%d stations from SD", s_st_count);
    }
    if (s_st_count == 0) {
        for (int i = 0; i < (int)(sizeof STATIONS / sizeof STATIONS[0]); i++) s_st[s_st_count++] = STATIONS[i];
    }
}

int radio_count(void) { return s_st_count; }
const char *radio_name(int i) { return (i >= 0 && i < s_st_count) ? s_st[i].name : ""; }
int radio_index(void) { return s_st_index; }
radio_state_t radio_state(void) { return s_radio; }
bool radio_active(void) { return s_active; }

// Follow redirects ourselves (the player only handles 301/302; e.g. Triple R
// uses 307) and return the final stream URL, tagged with an extension the
// player's decoder picker understands.
static bool resolve_url(const char *url, char *out, size_t n)
{
    char clean[200];
    strlcpy(clean, url, sizeof clean);
    char *frag = strchr(clean, '#');
    if (frag) *frag = 0;
    esp_http_client_config_t cfg = {
        .url = clean,
        .timeout_ms = 6000,
        .buffer_size = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    bool ok = false;
    for (int hop = 0; hop < 6; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(c);
        int code = esp_http_client_get_status_code(c);
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        ok = code == 200;
        if (ok) esp_http_client_get_url(c, out, n - 8);
        break;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    if (!ok) return false;
    // The decoder is chosen from the last '.' in the URL.
    const char *dot = strrchr(out, '.'), *slash = strrchr(out, '/');
    bool has_ext = dot && slash && dot > slash &&
                   (!strncasecmp(dot, ".mp3", 4) || !strncasecmp(dot, ".aac", 4));
    if (!has_ext) strlcat(out, strstr(url, "#.aac") ? "#.aac" : "#.mp3", n);
    return true;
}

// ---------------------------------------------------------------- worker task

static bool cmd_waiting(void)
{
    radio_msg_t m;
    return xQueuePeek(s_radio_q, &m, 0) == pdTRUE;
}

static void worker_stop_player(void)
{
    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(handle, &st);
    if (st != ESP_ASP_STATE_NONE && st != ESP_ASP_STATE_STOPPED) audio_stop_play();
}

static void worker_play(int index, bool reconnect)
{
    worker_stop_player();
    if (!s_wifi_held) { wifi_mgr_acquire(); s_wifi_held = true; }
    if (!reconnect) s_reconnects = 0;
    s_radio = RADIO_WAIT_WIFI;
    int64_t start = esp_timer_get_time();
    while (wifi_mgr_state() != WIFI_CONNECTED) {
        if (cmd_waiting()) return;                       // superseded (skip / stop)
        if (wifi_mgr_state() == WIFI_NO_NETWORK || esp_timer_get_time() - start > 25LL * 1000000) {
            wifi_mgr_release(); s_wifi_held = false;
            s_active = false;
            s_radio = RADIO_NO_WIFI;
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    static char final_url[256];
    if (!resolve_url(s_st[index].url, final_url, sizeof final_url)) {
        ESP_LOGW(TAG, "station %d: could not open stream", index);
        if (cmd_waiting()) return;
        wifi_mgr_release(); s_wifi_held = false;
        s_active = false;
        s_radio = RADIO_OFFLINE;
        return;
    }
    if (cmd_waiting()) return;
    Music_Next_Flag = 0;
    Play_Url(final_url);
    s_play_since = esp_timer_get_time();
    s_radio = RADIO_PLAYING;
}

static void radio_task(void *arg)
{
    (void)arg;
    radio_msg_t m;
    for (;;) {
        if (xQueueReceive(s_radio_q, &m, portMAX_DELAY) != pdTRUE) continue;
        switch (m.cmd) {
        case CMD_PLAY:      worker_play(m.index, false); break;
        case CMD_RECONNECT: worker_play(m.index, true); break;
        case CMD_STOP:
            worker_stop_player();
            if (s_wifi_held) { wifi_mgr_release(); s_wifi_held = false; }
            // Keep "no wifi" / "offline" on screen; otherwise back to idle.
            if (!cmd_waiting() && s_radio != RADIO_OFFLINE && s_radio != RADIO_NO_WIFI) s_radio = RADIO_IDLE;
            break;
        }
    }
}

static void post(radio_cmd_t cmd, int index)
{
    radio_msg_t m = {(uint8_t)cmd, (int8_t)index};
    xQueueSend(s_radio_q, &m, pdMS_TO_TICKS(50));
}

// ---------------------------------------------------------------- UI side

void radio_init(void)
{
    load_stations();
    if (!s_radio_q) {
        s_radio_q = xQueueCreate(4, sizeof(radio_msg_t));
        xTaskCreatePinnedToCoreWithCaps(radio_task, "radio", 6144, NULL, 4, NULL, 0, MALLOC_CAP_SPIRAM);
    }
}

void radio_stop(void)
{
    s_active = false;
    post(CMD_STOP, 0);
}

void radio_play(int i)
{
    if (!s_st_count) return;
    s_st_index = (i % s_st_count + s_st_count) % s_st_count;
    s_active = true;
    s_radio = RADIO_WAIT_WIFI;
    post(CMD_PLAY, s_st_index);
}

void radio_step(int delta)
{
    if (s_active) radio_play(s_st_index + delta);
    else if (s_st_count) s_st_index = ((s_st_index + delta) % s_st_count + s_st_count) % s_st_count;
}

void radio_tick(void)
{
    if (s_radio != RADIO_PLAYING) return;
    esp_asp_state_t st = ESP_ASP_STATE_NONE;
    esp_audio_simple_player_get_state(handle, &st);
    if (st == ESP_ASP_STATE_RUNNING && esp_timer_get_time() - s_play_since > 60LL * 1000000)
        s_reconnects = 0;                          // a minute of steady play: fresh retries
    if (st == ESP_ASP_STATE_ERROR || st == ESP_ASP_STATE_FINISHED || Music_Next_Flag) {
        // Streams drop (ad breaks, network blips): reconnect quietly a few times.
        ESP_LOGW(TAG, "stream stopped (state %d), reconnect %d/%d", st, s_reconnects + 1, MAX_RECONNECTS);
        Music_Next_Flag = 0;
        if (s_reconnects < MAX_RECONNECTS) {
            s_reconnects++;
            s_radio = RADIO_WAIT_WIFI;
            post(CMD_RECONNECT, s_st_index);
        } else {
            radio_stop();
            s_radio = RADIO_OFFLINE;
        }
    }
}

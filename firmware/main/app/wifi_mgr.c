#include "wifi_mgr.h"

#include <string.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#include "clock_sync.h"

static const char *TAG = "wifi";

typedef struct {
    char ssid[33];
    char pass[65];
    uint8_t home;
} saved_t;

typedef struct {
    uint8_t version;
    uint8_t count;
    saved_t net[WIFI_MAX_SAVED];
} store_t;

static store_t s_store;
static SemaphoreHandle_t s_lock;
static int s_users;
static bool s_started, s_inited, s_driver;   // s_driver: esp_wifi_init done
static volatile wifi_state_t s_state = WIFI_OFF;
static char s_ssid[33];
static volatile bool s_scanning, s_scan_done, s_join_failed;
static int s_try;                       // index into the saved list during auto-join
static int s_attempt;                   // tries on the current network
#define ATTEMPTS_PER_NET 3
static bool s_pending_save;
static saved_t s_pending;

// ---------------------------------------------------------------- storage

static void store_load(void)
{
    memset(&s_store, 0, sizeof s_store);
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_store;
    if (nvs_get_blob(h, "nets", &s_store, &len) != ESP_OK || len != sizeof s_store || s_store.version != 1) {
        memset(&s_store, 0, sizeof s_store);
    }
    nvs_close(h);
}

static void store_save(void)
{
    s_store.version = 1;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", &s_store, sizeof s_store);
    nvs_commit(h);
    nvs_close(h);
}

static int find_saved(const char *ssid)
{
    for (int i = 0; i < s_store.count; i++)
        if (strcmp(s_store.net[i].ssid, ssid) == 0) return i;
    return -1;
}

static void remember(const saved_t *n)
{
    int i = find_saved(n->ssid);
    if (i < 0) {
        if (s_store.count == WIFI_MAX_SAVED) {         // drop the oldest
            memmove(&s_store.net[0], &s_store.net[1], sizeof(saved_t) * (WIFI_MAX_SAVED - 1));
            s_store.count--;
        }
        i = s_store.count++;
        s_store.net[i].home = 0;
    }
    uint8_t home = s_store.net[i].home;
    s_store.net[i] = *n;
    s_store.net[i].home = home;
    store_save();
    ESP_LOGI(TAG, "saved network \"%s\"", n->ssid);   // never the password
}

// ---------------------------------------------------------------- connecting

static void connect_to(const char *ssid, const char *pass)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof cfg.sta.ssid);
    strlcpy((char *)cfg.sta.password, pass, sizeof cfg.sta.password);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    cfg.sta.pmf_cfg.capable = true;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    strlcpy(s_ssid, ssid, sizeof s_ssid);
    s_state = WIFI_CONNECTING;
    esp_wifi_connect();
}

static void try_next_saved(void)
{
    if (s_pending_save) return;                         // a manual join is in progress
    if (s_try >= s_store.count) {
        s_state = WIFI_NO_NETWORK;
        s_ssid[0] = 0;
        return;
    }
    const saved_t *n = &s_store.net[s_try++];
    s_attempt = 1;
    connect_to(n->ssid, n->pass);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_try = 0;
        try_next_saved();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_started) return;
        const wifi_event_sta_disconnected_t *d = data;
        if (s_pending_save) {
            // Leaving the previous network also lands here: only a disconnect
            // from the network being joined means wrong password / out of range.
            size_t len = strlen(s_pending.ssid);
            if (d->ssid_len != len || memcmp(d->ssid, s_pending.ssid, len) != 0) return;
            s_pending_save = false;
            s_join_failed = true;
            memset(&s_pending, 0, sizeof s_pending);
        }
        ESP_LOGW(TAG, "disconnected from \"%.32s\" (reason %d)", (const char *)d->ssid, d->reason);
        if (s_state == WIFI_CONNECTED) { s_try = 0; try_next_saved(); return; }   // dropped: start over
        // First attempts after power-up often fail (auth/handshake timeouts): retry
        // the same network a few times before moving on.
        if (s_try > 0 && s_attempt < ATTEMPTS_PER_NET && !s_pending_save) {
            s_attempt++;
            const saved_t *n = &s_store.net[s_try - 1];
            connect_to(n->ssid, n->pass);
            return;
        }
        try_next_saved();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "scan done");
        s_scanning = false;
        s_scan_done = true;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_state = WIFI_CONNECTED;
        ESP_LOGI(TAG, "connected to \"%s\"", s_ssid);
        if (s_pending_save) {
            s_pending_save = false;
            remember(&s_pending);
            memset(&s_pending, 0, sizeof s_pending);
        }
        clock_sync_start();
    }
}

void wifi_mgr_init(void)
{
    if (s_inited) return;
    s_lock = xSemaphoreCreateMutex();
    store_load();
    s_inited = true;
}

static esp_err_t s_start_err = ESP_OK;
esp_err_t wifi_mgr_last_error(void) { return s_start_err; }

#define CHECK_STEP(call) do { esp_err_t _e = (call);     if (_e != ESP_OK && _e != ESP_ERR_INVALID_STATE) {         ESP_LOGE(TAG, "%s failed: %s", #call, esp_err_to_name(_e)); s_start_err = _e; return; } } while (0)

static void log_heap(const char *what)
{
    ESP_LOGI(TAG, "%s: internal free %u, largest %u, min ever %u", what,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

// The netif, event loop and handlers are set up once and kept. The driver
// itself (esp_wifi_init) is created on every start and deleted when the last
// user releases WiFi: it holds ~60 KB of internal RAM even when stopped.
static void start_wifi(void)
{
    static bool netif_ready;
    static esp_netif_t *sta;
    s_start_err = ESP_OK;
    log_heap("starting");
    if (!netif_ready) {
        CHECK_STEP(esp_netif_init());
        CHECK_STEP(esp_event_loop_create_default());
        if (!sta) sta = esp_netif_create_default_wifi_sta();      // once, even if a later step fails
        if (!sta) { ESP_LOGE(TAG, "netif create failed"); s_start_err = ESP_FAIL; return; }
        CHECK_STEP(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
        CHECK_STEP(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
        netif_ready = true;
    }
    if (!s_driver) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        CHECK_STEP(esp_wifi_init(&cfg));
        s_driver = true;
        log_heap("driver ready");
        CHECK_STEP(esp_wifi_set_storage(WIFI_STORAGE_RAM));   // our own store holds the credentials
        CHECK_STEP(esp_wifi_set_mode(WIFI_MODE_STA));
    }
    s_started = true;
    s_state = WIFI_CONNECTING;
    CHECK_STEP(esp_wifi_start());                       // STA_START -> try saved networks
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    log_heap("started");
}

static void stop_wifi(void)
{
    s_started = false;
    s_scanning = false;                     // a scan cut short never reports SCAN_DONE
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (s_driver) {
        esp_err_t e = esp_wifi_deinit();
        if (e == ESP_OK) s_driver = false;
        else ESP_LOGE(TAG, "esp_wifi_deinit failed: %s", esp_err_to_name(e));
    }
    s_state = WIFI_OFF;
    s_ssid[0] = 0;
    log_heap("off");
}

void wifi_mgr_acquire(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_users++ == 0) start_wifi();
    else if (s_state == WIFI_NO_NETWORK) { s_try = 0; try_next_saved(); }
    xSemaphoreGive(s_lock);
}

void wifi_mgr_release(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_users > 0 && --s_users == 0) stop_wifi();
    xSemaphoreGive(s_lock);
}

wifi_state_t wifi_mgr_state(void) { return s_state; }
const char *wifi_mgr_ssid(void) { return s_state == WIFI_CONNECTED ? s_ssid : ""; }

bool wifi_mgr_is_home(void)
{
    if (s_state != WIFI_CONNECTED) return false;
    int i = find_saved(s_ssid);
    return i >= 0 && s_store.net[i].home;
}

int wifi_mgr_saved_count(void) { return s_store.count; }
const char *wifi_mgr_saved_ssid(int i) { return (i >= 0 && i < s_store.count) ? s_store.net[i].ssid : ""; }
bool wifi_mgr_saved_home(int i) { return i >= 0 && i < s_store.count && s_store.net[i].home; }

void wifi_mgr_set_home(int i, bool home)
{
    if (i < 0 || i >= s_store.count) return;
    s_store.net[i].home = home;
    store_save();
}

void wifi_mgr_forget(int i)
{
    if (i < 0 || i >= s_store.count) return;
    memset(&s_store.net[i], 0, sizeof(saved_t));
    memmove(&s_store.net[i], &s_store.net[i + 1], sizeof(saved_t) * (s_store.count - i - 1));
    s_store.count--;
    memset(&s_store.net[s_store.count], 0, sizeof(saved_t));
    store_save();
}

bool wifi_mgr_scan_start(void)
{
    if (!s_started || s_scanning) {
        ESP_LOGW(TAG, "scan refused: started=%d scanning=%d err=%s", s_started, s_scanning, esp_err_to_name(s_start_err));
        return false;
    }
    // Scanning while a join attempt is running fails, so pause auto-join.
    if (s_state != WIFI_CONNECTED) { s_try = s_store.count; esp_wifi_disconnect(); }
    s_scan_done = false;
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t e = esp_wifi_scan_start(&sc, false);
    if (e != ESP_OK) { ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(e)); s_start_err = e; }
    else ESP_LOGI(TAG, "scanning");
    s_scanning = e == ESP_OK;
    return s_scanning;
}

bool wifi_mgr_scan_done(void) { return s_scan_done; }

int wifi_mgr_scan_results(wifi_ap_t *out, int max)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&n, recs);              // already strongest first
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < k; j++) if (strcmp(out[j].ssid, (char *)recs[i].ssid) == 0) dup = true;
        if (dup) continue;
        strlcpy(out[k].ssid, (char *)recs[i].ssid, sizeof out[k].ssid);
        out[k].rssi = recs[i].rssi;
        out[k].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        out[k].saved = find_saved(out[k].ssid) >= 0;
        k++;
    }
    free(recs);
    s_scan_done = false;
    return k;
}

void wifi_mgr_join(const char *ssid, const char *password)
{
    if (!s_started) return;
    memset(&s_pending, 0, sizeof s_pending);
    strlcpy(s_pending.ssid, ssid, sizeof s_pending.ssid);
    strlcpy(s_pending.pass, password, sizeof s_pending.pass);
    s_pending_save = true;
    s_join_failed = false;
    esp_wifi_disconnect();
    connect_to(s_pending.ssid, s_pending.pass);
}

bool wifi_mgr_join_failed(void) { return s_join_failed; }

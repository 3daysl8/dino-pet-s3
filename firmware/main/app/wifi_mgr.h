// WiFi: up to 5 saved networks (NVS), scan, connect, and on-demand power.
// WiFi is only switched on while something holds a reference (radio, clock
// sync, the WiFi settings screen) and is stopped again afterwards.
// Passwords never leave the device: they are not logged or shown on screen.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define WIFI_MAX_SAVED 5

typedef enum { WIFI_OFF = 0, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_NO_NETWORK } wifi_state_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
    bool saved;
} wifi_ap_t;

void wifi_mgr_init(void);
void wifi_mgr_acquire(void);           // turn on (if needed) and join a saved network
void wifi_mgr_release(void);           // turn off when the last user lets go
wifi_state_t wifi_mgr_state(void);
const char *wifi_mgr_ssid(void);       // connected network, or ""
bool wifi_mgr_is_home(void);           // connected to a network marked "home"

// Saved networks
int wifi_mgr_saved_count(void);
const char *wifi_mgr_saved_ssid(int i);
bool wifi_mgr_saved_home(int i);
void wifi_mgr_set_home(int i, bool home);
void wifi_mgr_forget(int i);

// Scan (needs WiFi on: acquire first). Results are sorted strongest first.
bool wifi_mgr_scan_start(void);
bool wifi_mgr_scan_done(void);
int wifi_mgr_scan_results(wifi_ap_t *out, int max);

// Join a network now and save it if the connection works.
void wifi_mgr_join(const char *ssid, const char *password);
bool wifi_mgr_join_failed(void);       // last join attempt was rejected / timed out
esp_err_t wifi_mgr_last_error(void);   // why WiFi failed to start or scan (ESP_OK if fine)

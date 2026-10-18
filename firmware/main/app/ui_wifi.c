// WiFi settings: saved networks (home flag, forget), scan, and password entry on
// an on-screen keyboard. WiFi is on only while this screen is open (plus radio).
#include <stdio.h>
#include <string.h>

#include "settings.h"
#include "t9.h"
#include "sfx.h"
#include "ui.h"
#include "ui_wifi.h"
#include "wifi_mgr.h"

#define MAX_SCAN 6

static lv_obj_t *s_scr, *s_status, *s_list, *s_add;
static lv_obj_t *s_scan_panel, *s_scan_list, *s_scan_note;
static lv_obj_t *s_key_panel, *s_key_title, *s_ta, *s_show_btn;
static lv_obj_t *s_act_panel, *s_act_title, *s_act_home;
static wifi_ap_t s_aps[MAX_SCAN];
static int s_ap_count, s_act_index = -1;
static char s_join_ssid[33];
static bool s_joining, s_scanning;

static lv_obj_t *panel(void)
{
    lv_obj_t *p = ui_box(s_scr);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);         // block taps to what's underneath
    lv_obj_set_size(p, SCREEN_PX, SCREEN_PX);
    lv_obj_set_style_bg_color(p, UI_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

// ---------------------------------------------------------------- saved list

static void on_saved(lv_event_t *e);

static void rebuild_saved(void)
{
    lv_obj_clean(s_list);
    int n = wifi_mgr_saved_count();
    for (int i = 0; i < n; i++) {
        char text[48];
        snprintf(text, sizeof text, "%s%.24s", wifi_mgr_saved_home(i) ? "* " : "", wifi_mgr_saved_ssid(i));
        lv_obj_t *r = ui_chip(s_list, 236, 34, text, on_saved, (void *)(intptr_t)i);
        lv_obj_set_style_radius(r, 10, 0);
        bool live = strcmp(wifi_mgr_ssid(), wifi_mgr_saved_ssid(i)) == 0;
        ui_chip_set_on(r, live);
    }
    if (n == 0) {
        lv_obj_t *l = ui_label(s_list, &font_ss_12, UI_MUTED, "NO SAVED NETWORKS");
        lv_obj_set_width(l, 236);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
}

static void status_refresh(void)
{
    char buf[48];
    switch (wifi_mgr_state()) {
    case WIFI_CONNECTED: snprintf(buf, sizeof buf, "ON: %.22s%s", wifi_mgr_ssid(), wifi_mgr_is_home() ? " (HOME)" : ""); break;
    case WIFI_CONNECTING: snprintf(buf, sizeof buf, s_joining ? "JOINING %.20s..." : "CONNECTING...", s_join_ssid); break;
    case WIFI_NO_NETWORK: snprintf(buf, sizeof buf, "NO SAVED NETWORK NEARBY"); break;
    default: snprintf(buf, sizeof buf, "WIFI OFF"); break;
    }
    ui_set_text(s_status, buf);
}

// ---------------------------------------------------------------- saved: actions

static void on_saved(lv_event_t *e)
{
    s_act_index = (int)(intptr_t)lv_event_get_user_data(e);
    char t[40];
    snprintf(t, sizeof t, "%.26s", wifi_mgr_saved_ssid(s_act_index));
    lv_label_set_text(s_act_title, t);
    ui_chip_set_text(s_act_home, wifi_mgr_saved_home(s_act_index) ? "HOME: YES" : "HOME: NO");
    ui_chip_set_on(s_act_home, wifi_mgr_saved_home(s_act_index));
    ui_show(s_act_panel, true);
}

static void on_act(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (a == 0) {                                        // toggle home
        wifi_mgr_set_home(s_act_index, !wifi_mgr_saved_home(s_act_index));
        ui_chip_set_text(s_act_home, wifi_mgr_saved_home(s_act_index) ? "HOME: YES" : "HOME: NO");
        ui_chip_set_on(s_act_home, wifi_mgr_saved_home(s_act_index));
        return;
    }
    if (a == 1) { wifi_mgr_forget(s_act_index); ui_toast("FORGOTTEN"); }
    ui_show(s_act_panel, false);
    rebuild_saved();
}

// ---------------------------------------------------------------- scan

static void on_ap(lv_event_t *e);

static void on_add(lv_event_t *e)
{
    (void)e;
    lv_obj_clean(s_scan_list);
    ui_set_text(s_scan_note, "SCANNING...");
    ui_show(s_scan_panel, true);
    s_scanning = wifi_mgr_scan_start();
    if (!s_scanning) {
        char t[48];
        snprintf(t, sizeof t, "WIFI ERROR: %.30s", esp_err_to_name(wifi_mgr_last_error()));
        ui_set_text(s_scan_note, wifi_mgr_last_error() == ESP_OK ? "WIFI STARTING - TRY AGAIN" : t);
    }
}

static void scan_poll(void)
{
    if (!s_scanning || !wifi_mgr_scan_done()) return;
    s_scanning = false;
    s_ap_count = wifi_mgr_scan_results(s_aps, MAX_SCAN);
    lv_obj_clean(s_scan_list);
    for (int i = 0; i < s_ap_count; i++) {
        char t[48];
        const char *bars = s_aps[i].rssi > -60 ? "|||" : s_aps[i].rssi > -72 ? "|| " : "|  ";
        snprintf(t, sizeof t, "%s %.20s%s", bars, s_aps[i].ssid, s_aps[i].secure ? "" : " (OPEN)");
        lv_obj_t *r = ui_chip(s_scan_list, 236, 34, t, on_ap, (void *)(intptr_t)i);
        lv_obj_set_style_radius(r, 10, 0);
        lv_obj_t *l = lv_obj_get_child(r, 0);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
    }
    ui_set_text(s_scan_note, s_ap_count ? "TAP YOUR NETWORK" : "NOTHING FOUND - TRY AGAIN");
}

static void on_scan_back(lv_event_t *e) { (void)e; ui_show(s_scan_panel, false); }
static void on_rescan(lv_event_t *e) { on_add(e); }

static t9_t *s_kp;                                  // password keypad (t9.c)

// ---------------------------------------------------------------- password entry

static void start_join(const char *pass)
{
    s_joining = true;
    wifi_mgr_join(s_join_ssid, pass);
    t9_commit(s_kp);
    lv_textarea_set_text(s_ta, "");                    // don't keep the password on screen
    ui_show(s_key_panel, false);
    ui_show(s_scan_panel, false);
    status_refresh();
}

static void on_ap(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_join_ssid, s_aps[i].ssid, sizeof s_join_ssid);
    if (!s_aps[i].secure) { start_join(""); return; }
    char t[48];
    snprintf(t, sizeof t, "PASSWORD FOR %.14s", s_join_ssid);
    lv_label_set_text(s_key_title, t);
    lv_textarea_set_text(s_ta, "");
    lv_textarea_set_password_mode(s_ta, true);
    ui_chip_set_text(s_show_btn, "SHOW");
    t9_reset(s_kp);
    ui_show(s_key_panel, true);
}

static void on_key_btn(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (a == 0) {                                        // show / hide
        bool hidden = lv_textarea_get_password_mode(s_ta);
        lv_textarea_set_password_mode(s_ta, !hidden);
        ui_chip_set_text(s_show_btn, hidden ? "HIDE" : "SHOW");
    } else if (a == 1) {                                 // cancel
        t9_commit(s_kp);
        lv_textarea_set_text(s_ta, "");
        ui_show(s_key_panel, false);
    } else {                                             // join
        start_join(lv_textarea_get_text(s_ta));
    }
}

// ---------------------------------------------------------------- lifecycle

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    if (ui_view() != VIEW_WIFI) return;
    scan_poll();
    if (s_joining) {
        if (wifi_mgr_state() == WIFI_CONNECTED && strcmp(wifi_mgr_ssid(), s_join_ssid) == 0) {
            s_joining = false;
            sfx_play(SFX_YUM);
            ui_toast("CONNECTED!");
            rebuild_saved();
        } else if (wifi_mgr_join_failed()) {
            s_joining = false;
            sfx_play(SFX_NO);
            ui_toast("WRONG PASSWORD?");
        }
    }
    status_refresh();
}

void wifiv_on_show(void)
{
    wifi_mgr_acquire();
    rebuild_saved();
    status_refresh();
}

void wifiv_on_hide(void)
{
    ui_show(s_key_panel, false);
    ui_show(s_scan_panel, false);
    ui_show(s_act_panel, false);
    lv_textarea_set_text(s_ta, "");
    s_joining = s_scanning = false;
    wifi_mgr_release();
}

lv_obj_t *wifiv_create(void)
{
    s_scr = ui_screen_new();
    ui_header(s_scr, "WIFI");
    s_status = ui_label(s_scr, &font_ss_12, UI_MUTED, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 72);

    s_list = ui_box(s_scr);
    lv_obj_set_size(s_list, 244, 176);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 94);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_add = ui_chip(s_scr, 170, 40, "ADD NETWORK", on_add, NULL);
    lv_obj_align(s_add, LV_ALIGN_TOP_MID, 0, 278);
    ui_chip_set_on(s_add, true);

    // Scan results
    s_scan_panel = panel();
    lv_obj_t *t = ui_label(s_scan_panel, &font_ss_12, UI_MUTED, "NEARBY NETWORKS");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 50);
    s_scan_note = ui_label(s_scan_panel, &font_ss_12, UI_DIM, "");
    lv_obj_align(s_scan_note, LV_ALIGN_TOP_MID, 0, 70);
    s_scan_list = ui_box(s_scan_panel);
    lv_obj_set_size(s_scan_list, 244, 180);
    lv_obj_align(s_scan_list, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_scan_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_scan_list, 6, 0);
    lv_obj_add_flag(s_scan_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scan_list, LV_DIR_VER);
    lv_obj_t *b = ui_chip(s_scan_panel, 90, 36, "BACK", on_scan_back, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_MID, -50, 282);
    b = ui_chip(s_scan_panel, 90, 36, "RESCAN", on_rescan, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 50, 282);

    // Password entry: T9 keypad where the circle is widest.
    s_key_panel = panel();
    lv_obj_clear_flag(s_key_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);   // no swipe-back while typing
    s_key_title = ui_label(s_key_panel, &font_ss_12, UI_MUTED, "");
    lv_obj_align(s_key_title, LV_ALIGN_TOP_MID, 0, 44);
    s_ta = lv_textarea_create(s_key_panel);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, 63);
    lv_textarea_set_password_mode(s_ta, true);
    lv_obj_set_size(s_ta, 256, 38);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(s_ta, UI_PANEL, 0);
    lv_obj_set_style_border_color(s_ta, UI_RED, 0);
    lv_obj_set_style_text_color(s_ta, UI_INK, 0);
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_16, 0);
    s_kp = t9_create(s_key_panel, s_ta, 102);
    s_show_btn = ui_chip(s_key_panel, 74, 36, "SHOW", on_key_btn, (void *)0);
    lv_obj_align(s_show_btn, LV_ALIGN_TOP_MID, -80, 274);
    b = ui_chip(s_key_panel, 74, 36, "CANCEL", on_key_btn, (void *)1);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 274);
    b = ui_chip(s_key_panel, 74, 36, "JOIN", on_key_btn, (void *)2);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 80, 274);
    ui_chip_set_on(b, true);

    // Saved network actions
    s_act_panel = panel();
    s_act_title = ui_label(s_act_panel, &font_ss_16, UI_INK, "");
    lv_obj_align(s_act_title, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_t *n = ui_label(s_act_panel, &font_ss_12, UI_MUTED, "HOME NETWORK UNLOCKS TALK");
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 118);
    s_act_home = ui_chip(s_act_panel, 180, 40, "HOME: NO", on_act, (void *)0);
    lv_obj_align(s_act_home, LV_ALIGN_TOP_MID, 0, 146);
    b = ui_chip(s_act_panel, 180, 40, "FORGET", on_act, (void *)1);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 196);
    b = ui_chip(s_act_panel, 180, 40, "DONE", on_act, (void *)2);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 246);

    lv_timer_create(poll_cb, 300, NULL);
    return s_scr;
}

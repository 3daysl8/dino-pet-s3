// Settings: WiFi, name (T9 keypad), the option rows, and forget memory.
// Layout follows docs/mockup/dino-mockup-v3.html.
#include <stdio.h>
#include <string.h>

#include "esp_system.h"
#include "esp_timer.h"
#include "settings.h"
#include "t9.h"
#include "talk.h"
#include "voice.h"
#include "wifi_mgr.h"
#include "ui.h"
#include "ui_home.h"
#include "ui_settings.h"

typedef struct { const char *label; uint8_t *value; uint8_t count; const char *const *names; } setting_row_t;
static const char *const VOICE_NAMES[] = {"OFF", "USB ONLY", "ALWAYS"};
static const char *const WAKE_NAMES[] = {"HI ANDY", "HI ESP"};
static const char *const SOUND_NAMES[] = {"ON", "QUIET", "OFF"};
static const char *const BRIGHT_NAMES[] = {"40%", "60%", "80%", "100%"};
static const char *const DIM_NAMES[] = {"15 S", "30 S", "60 S"};
static const char *const CLOCK_NAMES[] = {"24 HOUR", "12 HOUR"};
static const char *const NAMEWAKE_NAMES[] = {"OFF", "PLUGGED IN"};
static const char *const ONOFF_NAMES[] = {"OFF", "ON"};
#define SET_ROWS 8
static setting_row_t ROWS[SET_ROWS] = {
    {"VOICE", &g_settings.voice, 3, VOICE_NAMES},
    {"WAKE WORD", &g_settings.wake, 2, WAKE_NAMES},
    {"SOUND", &g_settings.sound, 3, SOUND_NAMES},
    {"BRIGHTNESS", &g_settings.bright, 4, BRIGHT_NAMES},
    {"DIM AFTER", &g_settings.dim_after, 3, DIM_NAMES},
    {"CLOCK", &g_settings.clock_12h, 2, CLOCK_NAMES},
    {"HEY + NAME", &g_settings.name_wake, 2, NAMEWAKE_NAMES},
    {"GOOD MORNING", &g_settings.rituals, 2, ONOFF_NAMES},
};
static lv_obj_t *s_row[SET_ROWS], *s_row_val[SET_ROWS], *s_set_note, *s_wifi_val, *s_name_val, *s_forget_val;
static lv_obj_t *s_name_panel, *s_name_ta;
static t9_t *s_name_kp;
static int64_t s_forget_armed_us;
static uint8_t s_wake_at_boot = 0xFF;

static void setv_refresh(void)
{
    for (int i = 0; i < SET_ROWS; i++) ui_set_text(s_row_val[i], ROWS[i].names[*ROWS[i].value % ROWS[i].count]);
    if (g_settings.voice == VOICE_OFF) lv_obj_add_state(s_row[1], LV_STATE_DISABLED);
    else lv_obj_clear_state(s_row[1], LV_STATE_DISABLED);
    static const char *const notes[] = {"MIC OFF - BEST BATTERY", "LISTENS WHEN ON A COMPUTER", "MIC ALWAYS ON - MORE BATTERY"};
    ui_set_text(s_set_note, g_settings.wake != s_wake_at_boot ? "RESTARTING TO SWITCH WORD..." : notes[g_settings.voice % 3]);
    lv_obj_set_style_text_color(s_set_note, g_settings.voice == VOICE_ALWAYS ? UI_RED : UI_DIM, 0);
    char buf[16];
    strlcpy(buf, g_settings.pet_name, sizeof buf);
    for (char *c = buf; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
    ui_set_text(s_name_val, buf);
    bool armed = s_forget_armed_us && esp_timer_get_time() - s_forget_armed_us < 3000000;
    ui_set_text(s_forget_val, armed ? "TAP AGAIN" : "");
}

void setv_on_show(void)
{
    char buf[32];
    int n = wifi_mgr_saved_count();
    if (n) snprintf(buf, sizeof buf, "%d SAVED", n);
    else snprintf(buf, sizeof buf, "SET UP");
    ui_set_text(s_wifi_val, buf);
    ui_show(s_name_panel, false);
    setv_refresh();
}

void setv_tick(void)
{
    int r = talk_forget_result();
    if (r) ui_toast(r > 0 ? "MEMORY WIPED" : "COULDN'T REACH SERVER");
    if (s_forget_armed_us && esp_timer_get_time() - s_forget_armed_us >= 3000000) {
        s_forget_armed_us = 0;
        setv_refresh();
    }
}

static void on_wifi_row(lv_event_t *e) { (void)e; ui_go(VIEW_WIFI); }

static void on_name_row(lv_event_t *e)
{
    (void)e;
    lv_textarea_set_text(s_name_ta, g_settings.pet_name);
    t9_reset(s_name_kp);
    ui_show(s_name_panel, true);
}

static void on_name_btn(lv_event_t *e)
{
    t9_commit(s_name_kp);
    if (lv_event_get_user_data(e)) {                           // SAVE
        char name[sizeof g_settings.pet_name];
        strlcpy(name, lv_textarea_get_text(s_name_ta), sizeof name);
        char *start = name, *end;
        while (*start == ' ') start++;
        end = start + strlen(start);
        while (end > start && end[-1] == ' ') *--end = 0;
        if (*start) {
            strlcpy(g_settings.pet_name, start, sizeof g_settings.pet_name);
            settings_save();
            ui_toast("NEW NAME!");
        }
    }
    ui_show(s_name_panel, false);
    setv_refresh();
}

// Two taps within 3 s, so a stray touch can't wipe the dino's memories.
static void on_forget_row(lv_event_t *e)
{
    (void)e;
    if (s_forget_armed_us && esp_timer_get_time() - s_forget_armed_us < 3000000) {
        s_forget_armed_us = 0;
        ui_toast(talk_forget() ? "FORGETTING..." : "TALK ISN'T READY");
    } else {
        s_forget_armed_us = esp_timer_get_time();
    }
    setv_refresh();
}

static void restart_cb(lv_timer_t *t)
{
    (void)t;
    home_save();
    esp_restart();
}

static void on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    *ROWS[i].value = (*ROWS[i].value + 1) % ROWS[i].count;
    settings_save();
    setv_refresh();
    ui_backlight_refresh();
    // The wake word model is chosen when the speech engine starts.
    if (i == 1 && g_settings.wake != s_wake_at_boot) lv_timer_set_repeat_count(lv_timer_create(restart_cb, 1500, NULL), 1);
}

// One list row: label on the left, value on the right.
static lv_obj_t *set_row(lv_obj_t *list, int y, const char *label, lv_event_cb_t cb, void *ud, lv_obj_t **val)
{
    lv_obj_t *r = ui_chip(list, 240, 30, "", cb, ud);
    lv_obj_set_style_radius(r, 10, 0);
    lv_obj_align(r, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_t *k = lv_obj_get_child(r, 0);
    lv_obj_set_style_text_font(k, &font_ss_12, 0);
    lv_obj_set_style_text_color(k, UI_MUTED, 0);
    lv_label_set_text(k, label);
    lv_obj_align(k, LV_ALIGN_LEFT_MID, 12, 0);
    *val = ui_label(r, &font_ss_12, UI_INK, "");
    lv_obj_align(*val, LV_ALIGN_RIGHT_MID, -12, 0);
    return r;
}

lv_obj_t *setv_create(void)
{
    if (s_wake_at_boot == 0xFF) s_wake_at_boot = g_settings.wake;
    lv_obj_t *scr = ui_screen_new();
    ui_header(scr, "SETTINGS");

    // The rows scroll: about seven fit inside the circle, swipe up for the rest.
    lv_obj_t *list = ui_box(scr);
    lv_obj_set_size(list, 260, 244);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);   // gaps between rows are not "back"
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    int y = 2;
    set_row(list, y, "WIFI", on_wifi_row, NULL, &s_wifi_val);
    y += 34;
    set_row(list, y, "NAME", on_name_row, NULL, &s_name_val);
    y += 34;
    for (int i = 0; i < SET_ROWS; i++, y += 34) {
        s_row[i] = set_row(list, y, ROWS[i].label, on_row, (void *)(intptr_t)i, &s_row_val[i]);
        if (i < 2) lv_obj_set_style_border_color(s_row[i], lv_color_hex(0x4A1A20), 0);
    }
    lv_obj_t *f = set_row(list, y, "FORGET MEMORY", on_forget_row, NULL, &s_forget_val);
    lv_obj_set_style_border_color(f, UI_RED_EDGE, 0);
    lv_obj_set_style_text_color(s_forget_val, UI_RED, 0);
    y += 38;
    s_set_note = ui_label(list, &font_ss_12, UI_DIM, "");
    lv_obj_align(s_set_note, LV_ALIGN_TOP_MID, 0, y);

    // Name editor (T9 keypad), over the whole screen.
    s_name_panel = ui_box(scr);
    lv_obj_set_size(s_name_panel, SCREEN_PX, SCREEN_PX);
    lv_obj_set_style_bg_color(s_name_panel, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_name_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_name_panel, LV_OBJ_FLAG_CLICKABLE);              // block taps to the list
    lv_obj_clear_flag(s_name_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);       // no swipe-back while typing
    lv_obj_t *t = ui_label(s_name_panel, &font_ss_12, UI_MUTED, "NAME YOUR DINO");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 40);
    s_name_ta = lv_textarea_create(s_name_panel);
    lv_textarea_set_one_line(s_name_ta, true);
    lv_textarea_set_max_length(s_name_ta, sizeof g_settings.pet_name - 1);
    lv_textarea_set_accepted_chars(s_name_ta, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz '-");
    lv_obj_set_size(s_name_ta, 256, 38);
    lv_obj_align(s_name_ta, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_color(s_name_ta, UI_PANEL, 0);
    lv_obj_set_style_border_color(s_name_ta, UI_RED, 0);
    lv_obj_set_style_text_color(s_name_ta, UI_INK, 0);
    lv_obj_set_style_text_font(s_name_ta, &lv_font_montserrat_16, 0);
    s_name_kp = t9_create(s_name_panel, s_name_ta, 102);
    lv_obj_t *b = ui_chip(s_name_panel, 90, 36, "CANCEL", on_name_btn, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_MID, -50, 274);
    b = ui_chip(s_name_panel, 90, 36, "SAVE", on_name_btn, (void *)1);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 50, 274);
    ui_chip_set_on(b, true);
    ui_show(s_name_panel, false);

    setv_refresh();
    return scr;
}

// Clock + alarm: time, date, alarm card with repeat days, and the ringing overlay
// (also used when a voice timer runs out).
// Layout follows docs/mockup/dino-mockup-v3.html.
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "timers.h"
#include "app_clock.h"
#include "PCF85063.h"
#include "settings.h"
#include "sfx.h"
#include "ui.h"
#include "ui_clock.h"
#include "ui_home.h"

static lv_obj_t *s_clk_time, *s_clk_sec, *s_clk_ampm, *s_clk_date, *s_ring_alarm, *s_alarm_ampm, *s_alarm_time, *s_alarm_sw, *s_alarm_rep, *s_day[7];
static lv_obj_t *s_ringing, *s_ring_pet, *s_ring_edge, *s_ring_sub;
static lv_timer_t *s_alarm_timer;
static bool s_is_ringing, s_ring_timer;     // s_ring_timer: ringing for a voice timer, not the alarm
static char s_ring_label[TIMER_LABEL_LEN];
static int64_t s_ring_started_us;

static void alarm_text(char *buf, size_t n)
{
    app_clock_fmt(buf, n, g_settings.alarm_h, g_settings.alarm_m);
}

static void clock_refresh(void)
{
    char buf[24];
    datetime_t d = datetime;
    app_clock_fmt(buf, sizeof buf, d.hour, d.minute);
    ui_set_text(s_clk_time, buf);
    snprintf(buf, sizeof buf, "%02u", d.second);
    ui_set_text(s_clk_sec, buf);
    ui_set_text(s_clk_ampm, app_clock_ampm(d.hour));         // AM/PM sits above the seconds
    lv_obj_update_layout(s_clk_time);
    lv_obj_align_to(s_clk_sec, s_clk_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -8);
    lv_obj_align_to(s_clk_ampm, s_clk_time, LV_ALIGN_OUT_RIGHT_TOP, 4, 8);
    ui_date_text(buf, sizeof buf);
    ui_set_text(s_clk_date, buf);
    alarm_text(buf, sizeof buf);
    ui_set_text(s_alarm_time, buf);
    ui_set_text(s_alarm_ampm, app_clock_ampm(g_settings.alarm_h));
    lv_obj_update_layout(s_alarm_time);
    lv_obj_align_to(s_alarm_ampm, s_alarm_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -3);
    lv_obj_set_style_text_color(s_alarm_time, g_settings.alarm_on ? UI_INK : UI_DIM, 0);
    if (g_settings.alarm_on) lv_obj_add_state(s_alarm_sw, LV_STATE_CHECKED);
    else lv_obj_clear_state(s_alarm_sw, LV_STATE_CHECKED);
    uint8_t days = g_settings.alarm_days & 0x7F;
    ui_set_text(s_alarm_rep, days == 0 ? "ALARM - ONCE" : days == 0x7F ? "ALARM - DAILY" :
                             days == 0x1F ? "ALARM - WEEKDAYS" : days == 0x60 ? "ALARM - WEEKENDS" : "ALARM");
    for (int i = 0; i < 7; i++) ui_chip_set_on(s_day[i], (days >> i) & 1);
}

void clock_tick(void) { if (!s_is_ringing) clock_refresh(); }
bool clock_ringing(void) { return s_is_ringing; }

static void alarm_beep_cb(lv_timer_t *t)
{
    (void)t;
    if (esp_timer_get_time() - s_ring_started_us > 120LL * 1000000) { clock_ring(false); return; }
    sfx_play(SFX_ALERT);
    ui_wake_screen();
}

static void ring(bool on, const char *title, const char *sub)
{
    if (!s_ringing) return;                                 // clock screen not built yet
    s_is_ringing = on;
    ui_show(s_ringing, on);
    if (on) {
        ui_set_text(s_ring_alarm, title);
        ui_set_text(s_ring_sub, sub);
        s_ring_started_us = esp_timer_get_time();
        lv_img_set_src(s_ring_pet, ui_pet_look(DINO_HAPPY, 3));
        ui_bounce(s_ring_pet, 8, 160);
        lv_timer_resume(s_alarm_timer);
        alarm_beep_cb(NULL);
    } else {
        ui_stop_bounce(s_ring_pet);
        lv_timer_pause(s_alarm_timer);
        clock_refresh();
    }
}

void clock_ring(bool on)
{
    char buf[16], t[8];
    s_ring_timer = false;
    alarm_text(t, sizeof t);
    snprintf(buf, sizeof buf, "%s %s", t, app_clock_ampm(g_settings.alarm_h));
    if (on && s_ringing) home_alarm_wake();                  // the dino gets up with you
    ring(on, buf, "RISE AND SHINE!");
}

void clock_ring_timer(const char *label)
{
    char sub[TIMER_LABEL_LEN + 8];
    s_ring_timer = true;
    strlcpy(s_ring_label, label ? label : "", sizeof s_ring_label);
    snprintf(sub, sizeof sub, "%s", s_ring_label[0] ? s_ring_label : "TIMER DONE");
    for (char *c = sub; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;     // pixel font is caps
    ring(true, "TIME'S UP!", sub);
}

static void edge_pulse(void *o, int32_t v) { lv_obj_set_style_border_opa(o, (lv_opa_t)v, 0); }

static void on_alarm_sw(lv_event_t *e)
{
    g_settings.alarm_on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    settings_save();
    clock_refresh();
}

static void on_alarm_adj(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);    // minutes
    int m = (g_settings.alarm_h * 60 + g_settings.alarm_m + delta + 24 * 60) % (24 * 60);
    g_settings.alarm_h = m / 60;
    g_settings.alarm_m = m % 60;
    g_settings.alarm_on = 1;
    settings_save();
    clock_refresh();
}

static void on_alarm_day(lv_event_t *e)
{
    g_settings.alarm_days ^= (uint8_t)(1u << (int)(intptr_t)lv_event_get_user_data(e));
    settings_save();
    clock_refresh();
}

static void on_test_alarm(lv_event_t *e) { (void)e; clock_ring(true); }

static void snooze_cb(lv_timer_t *t)
{
    (void)t;
    ui_wake_screen();
    ui_go(VIEW_CLOCK);
    clock_ring(true);
}

static void on_snooze(lv_event_t *e)
{
    (void)e;
    bool was_timer = s_ring_timer;
    clock_ring(false);
    if (was_timer) timers_add(5 * 60, s_ring_label);        // a timer snoozes as a new 5-minute timer
    // One-shot re-ring in 5 minutes; the saved alarm time stays as it is.
    else lv_timer_set_repeat_count(lv_timer_create(snooze_cb, 5 * 60 * 1000, NULL), 1);
    ui_toast("5 MORE MIN");
    ui_go(VIEW_HOME);
}

static void on_stop(lv_event_t *e) { (void)e; clock_ring(false); ui_go(VIEW_HOME); }

lv_obj_t *clock_create(void)
{
    lv_obj_t *scr = ui_screen_new();
    ui_header(scr, "CLOCK");
    s_clk_time = ui_label(scr, &font_ss_bold_48, UI_INK, "");
    lv_obj_align(s_clk_time, LV_ALIGN_TOP_MID, -12, 68);
    s_clk_sec = ui_label(scr, &font_ss_16, UI_MUTED, "");
    s_clk_ampm = ui_label(scr, &font_ss_12, UI_MUTED, "");
    s_clk_date = ui_label(scr, &font_ss_12, UI_MUTED, "");
    lv_obj_align(s_clk_date, LV_ALIGN_TOP_MID, 0, 124);

    lv_obj_t *card = ui_box(scr);
    lv_obj_set_size(card, 272, 102);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 144);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x141417), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, UI_LINE, 0);
    lv_obj_align(ui_icon(card, &icon_bell, 2), LV_ALIGN_TOP_LEFT, 14, 18);
    s_alarm_rep = ui_label(card, &font_ss_12, UI_MUTED, "ALARM");
    lv_obj_align(s_alarm_rep, LV_ALIGN_TOP_LEFT, 48, 6);
    s_alarm_time = ui_label(card, &font_ss_bold_24, UI_INK, "");
    lv_obj_align(s_alarm_time, LV_ALIGN_TOP_LEFT, 48, 20);
    s_alarm_ampm = ui_label(card, &font_ss_12, UI_MUTED, "");
    s_alarm_sw = lv_switch_create(card);
    lv_obj_set_size(s_alarm_sw, 52, 28);
    lv_obj_align(s_alarm_sw, LV_ALIGN_TOP_RIGHT, -12, 14);
    lv_obj_set_style_bg_color(s_alarm_sw, UI_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_alarm_sw, UI_RED, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_alarm_sw, UI_INK, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s_alarm_sw, 10);
    lv_obj_add_event_cb(s_alarm_sw, on_alarm_sw, LV_EVENT_VALUE_CHANGED, NULL);
    // Repeat days, Monday first. None selected = a one-time alarm.
    static const char *const DAYS[7] = {"M", "T", "W", "T", "F", "S", "S"};
    for (int i = 0; i < 7; i++) {
        s_day[i] = ui_chip(card, 30, 28, DAYS[i], on_alarm_day, (void *)(intptr_t)i);
        lv_obj_align(s_day[i], LV_ALIGN_TOP_MID, (i - 3) * 35, 62);
        lv_obj_set_ext_click_area(s_day[i], 3);
    }

    // Adjust: five buttons sized to the circle's width at this height.
    static const struct { const char *t; int min; } ADJ[5] = {
        {"-10M", -10}, {"-1M", -1}, {"+1H", 60}, {"+1M", 1}, {"+10M", 10}};
    lv_obj_t *c;
    for (int i = 0; i < 5; i++) {
        c = ui_chip(scr, 50, 40, ADJ[i].t, on_alarm_adj, (void *)(intptr_t)ADJ[i].min);
        lv_obj_align(c, LV_ALIGN_TOP_MID, (i - 2) * 54, 254);
    }
    c = ui_chip(scr, 140, 30, "TEST ALARM", on_test_alarm, NULL);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 302);

    // Ringing overlay
    s_ringing = ui_box(scr);
    lv_obj_add_flag(s_ringing, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(s_ringing, SCREEN_PX, SCREEN_PX);
    lv_obj_set_style_bg_color(s_ringing, UI_BG, 0);
    lv_obj_set_style_bg_opa(s_ringing, LV_OPA_COVER, 0);
    s_ring_edge = ui_box(s_ringing);
    lv_obj_set_size(s_ring_edge, SCREEN_PX - 8, SCREEN_PX - 8);
    lv_obj_center(s_ring_edge);
    lv_obj_set_style_radius(s_ring_edge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_ring_edge, 10, 0);
    lv_obj_set_style_border_color(s_ring_edge, UI_RED, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ring_edge);
    lv_anim_set_exec_cb(&a, edge_pulse);
    lv_anim_set_values(&a, 90, 255);
    lv_anim_set_time(&a, 380);
    lv_anim_set_playback_time(&a, 380);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
    s_ring_alarm = ui_label(s_ringing, &font_ss_bold_32, UI_INK, "");
    lv_obj_align(s_ring_alarm, LV_ALIGN_TOP_MID, 0, 58);
    s_ring_sub = ui_label(s_ringing, &font_ss_12, UI_RED, "RISE AND SHINE!");
    lv_obj_align(s_ring_sub, LV_ALIGN_TOP_MID, 0, 98);
    s_ring_pet = lv_img_create(s_ringing);
    lv_obj_align(s_ring_pet, LV_ALIGN_TOP_MID, 0, 114);
    c = ui_chip(s_ringing, 110, 48, "SNOOZE", on_snooze, NULL);
    lv_obj_align(c, LV_ALIGN_TOP_MID, -62, 258);
    c = ui_chip(s_ringing, 110, 48, "STOP", on_stop, NULL);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 62, 258);
    ui_chip_set_on(c, true);
    lv_obj_add_flag(s_ringing, LV_OBJ_FLAG_HIDDEN);
    s_alarm_timer = lv_timer_create(alarm_beep_cb, 1500, NULL);
    lv_timer_pause(s_alarm_timer);

    clock_refresh();
    return scr;
}

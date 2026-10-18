// Nightlight: colour, brightness and timer; also the sleep-mode lamp.
// Layout follows docs/mockup/dino-mockup-v3.html.
#include "esp_timer.h"
#include "settings.h"
#include "ui.h"
#include "ui_light.h"

// Red, amber, blue, white. Brightness 1..7. Timer: 1..4 saved (15/30/60 min,
// always on); 0 = OFF is only ever a moment in the cycle: if the pill is left
// on OFF for 2 s, the lamp turns off.
#define LAMP_N      4
#define LAMP_LEVELS 7
#define LAMP_TIMER_OFF 0
static const uint32_t LAMP_COLORS[LAMP_N] = {0xFF2A2A, 0xFFB347, 0x5AB8F0, 0xF2F2FF};
static const char *const LAMP_TIMERS[5] = {"OFF", "15 MIN", "30 MIN", "60 MIN", "ALWAYS ON"};
static const uint32_t LAMP_SECONDS[5] = {0, 15 * 60, 30 * 60, 60 * 60, 0};
static lv_obj_t *s_light_scr, *s_light_ctrl, *s_swatch[LAMP_N], *s_level[LAMP_LEVELS], *s_timer_pill, *s_light_hint;
static int64_t s_light_started_us, s_light_touch_us, s_light_off_at_us;
static int s_timer_sel;                    // what the pill shows: 0 (OFF) .. 4
static bool s_sleep_lamp;

void light_set_sleep_lamp(bool on) { s_sleep_lamp = on; }
bool light_is_sleep_lamp(void) { return s_sleep_lamp; }

void light_open_sleep_lamp(void)
{
    s_sleep_lamp = true;
    ui_go(VIEW_LIGHT);
}

uint8_t light_backlight_pct(void)
{
    static const uint8_t pct[LAMP_LEVELS + 1] = {0, 10, 18, 25, 45, 60, 80, 100};
    return pct[g_settings.lamp_level <= LAMP_LEVELS ? g_settings.lamp_level : 4];
}

static void light_refresh(void)
{
    lv_obj_set_style_bg_color(s_light_scr, lv_color_hex(LAMP_COLORS[g_settings.lamp_color % LAMP_N]), 0);
    for (int i = 0; i < LAMP_N; i++)
        lv_obj_set_style_border_color(s_swatch[i], i == g_settings.lamp_color ? UI_INK : UI_BG, 0);
    for (int i = 0; i < LAMP_LEVELS; i++)
        lv_obj_set_style_bg_color(s_level[i], i < g_settings.lamp_level ? UI_INK : UI_LINE, 0);
    ui_chip_set_text(s_timer_pill, LAMP_TIMERS[s_timer_sel]);
    ui_chip_set_on(s_timer_pill, s_timer_sel != LAMP_TIMER_OFF);
    ui_backlight_refresh();
}

static void lamp_off(void)
{
    s_sleep_lamp = false;
    ui_go(VIEW_HOME);
}

void light_on_show(void)
{
    s_light_started_us = s_light_touch_us = esp_timer_get_time();
    s_timer_sel = g_settings.lamp_timer >= 1 && g_settings.lamp_timer <= 4 ? g_settings.lamp_timer : 2;
    light_refresh();
    ui_show(s_light_ctrl, !s_sleep_lamp);         // sleep lamp: just the glow
}

void light_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_timer_sel == LAMP_TIMER_OFF) {
        if (now >= s_light_off_at_us) { ui_toast("LIGHT OFF"); lamp_off(); }
        return;
    }
    // Controls fade away after a few seconds so the whole screen is the lamp.
    if (s_sleep_lamp || now - s_light_touch_us > 5LL * 1000000) ui_show(s_light_ctrl, false);
    uint32_t secs = LAMP_SECONDS[s_timer_sel];
    if (secs && now - s_light_started_us > (int64_t)secs * 1000000) {
        if (!s_sleep_lamp) ui_toast("NIGHT NIGHT");
        lamp_off();
    }
}

static void light_changed(void)
{
    s_light_touch_us = esp_timer_get_time();
    light_refresh();
    ui_back_hint_pulse(s_light_hint);          // set: you can swipe back now
}

static void level_set(int v)
{
    if (v < 1) v = 1;
    if (v > LAMP_LEVELS) v = LAMP_LEVELS;
    if (v == g_settings.lamp_level) return;
    g_settings.lamp_level = (uint8_t)v;
    s_light_touch_us = esp_timer_get_time();
    light_refresh();
}

static void on_light(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    if (v < LAMP_N) g_settings.lamp_color = (uint8_t)v;
    else if (v == 10) level_set(g_settings.lamp_level - 1);
    else if (v == 11) level_set(g_settings.lamp_level + 1);
    else if (v == 20) {                                            // timer pill: next option
        s_timer_sel = (s_timer_sel + 1) % 5;
        s_light_started_us = esp_timer_get_time();
        if (s_timer_sel == LAMP_TIMER_OFF) s_light_off_at_us = s_light_started_us + 2LL * 1000000;
        else g_settings.lamp_timer = (uint8_t)s_timer_sel;
    }
    settings_save();
    light_changed();
}

// Brightness: drag along the bar (finger x picks the level).
static void on_level_drag(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        settings_save();
        light_changed();
        return;
    }
    lv_obj_t *bar = lv_event_get_target(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_area_t a;
    lv_obj_get_coords(bar, &a);
    int w = lv_area_get_width(&a);
    level_set((p.x - a.x1) * LAMP_LEVELS / (w > 0 ? w : 1) + 1);
}

static void on_light_tap(lv_event_t *e)
{
    (void)e;
    if (s_sleep_lamp) {                        // second tap in sleep mode: lamp off
        lv_indev_wait_release(lv_indev_get_act());
        lamp_off();
        return;
    }
    s_light_touch_us = esp_timer_get_time();
    ui_show(s_light_ctrl, true);
}

lv_obj_t *light_create(void)
{
    s_light_scr = ui_screen_new();
    lv_obj_add_event_cb(s_light_scr, on_light_tap, LV_EVENT_PRESSED, NULL);
    s_light_ctrl = ui_box(s_light_scr);
    lv_obj_set_size(s_light_ctrl, SCREEN_PX, SCREEN_PX);
    lv_obj_t *panel = ui_box(s_light_ctrl);                        // dark band for legibility
    lv_obj_set_size(panel, 316, 176);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 150);
    lv_obj_set_style_radius(panel, 30, 0);
    lv_obj_set_style_bg_color(panel, UI_BG, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
    s_light_hint = ui_header(s_light_ctrl, "NIGHTLIGHT");
    lv_obj_t *pet = lv_img_create(s_light_ctrl);
    lv_img_set_src(pet, ui_pet_look(DINO_ASLEEP, 2));
    lv_obj_align(pet, LV_ALIGN_TOP_MID, 0, 72);

    // Four big colour circles.
    for (int i = 0; i < LAMP_N; i++) {
        lv_obj_t *s = ui_chip(s_light_ctrl, 54, 54, "", on_light, (void *)(intptr_t)i);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(LAMP_COLORS[i]), 0);
        lv_obj_set_style_border_width(s, 4, 0);
        lv_obj_align(s, LV_ALIGN_TOP_MID, (i * 2 - 3) * 34, 160);
        s_swatch[i] = s;
    }

    // Brightness: -, a bar you can drag, +.
    lv_obj_t *c = ui_chip(s_light_ctrl, 40, 34, "-", on_light, (void *)10);
    lv_obj_align(c, LV_ALIGN_TOP_MID, -118, 226);
    c = ui_chip(s_light_ctrl, 40, 34, "+", on_light, (void *)11);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 118, 226);
    lv_obj_t *bar = ui_box(s_light_ctrl);
    lv_obj_set_size(bar, 7 * 22 + 6 * 4, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 223);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);     // dragging right must not mean "back"
    lv_obj_add_event_cb(bar, on_level_drag, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(bar, on_level_drag, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(bar, on_level_drag, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(bar, on_level_drag, LV_EVENT_PRESS_LOST, NULL);
    for (int i = 0; i < LAMP_LEVELS; i++) {
        s_level[i] = ui_box(bar);
        lv_obj_set_size(s_level[i], 22, 26);
        lv_obj_set_style_radius(s_level[i], 4, 0);
        lv_obj_set_style_bg_opa(s_level[i], LV_OPA_COVER, 0);
        lv_obj_align(s_level[i], LV_ALIGN_LEFT_MID, i * 26, 0);
    }

    // Timer: one pill, each tap moves to the next option.
    s_timer_pill = ui_chip(s_light_ctrl, 200, 44, "", on_light, (void *)20);
    lv_obj_set_style_radius(s_timer_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(s_timer_pill, 0), &font_ss_16, 0);
    lv_obj_align(s_timer_pill, LV_ALIGN_TOP_MID, 0, 274);

    s_timer_sel = 2;
    light_refresh();
    return s_light_scr;
}

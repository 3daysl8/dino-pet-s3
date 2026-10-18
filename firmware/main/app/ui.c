#include "ui.h"
#include "actions.h"
#include "mem_report.h"
#include "ui_clock.h"
#include "ui_home.h"
#include "ui_light.h"
#include "ui_music.h"
#include "ui_ring.h"
#include "ui_settings.h"
#include "ui_stats.h"
#include "ui_talk.h"
#include "ui_wifi.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "ST77916.h"
#include "PCF85063.h"
#include "img_cache.h"
#include "battery.h"
#include "music.h"
#include "power.h"
#include "settings.h"
#include "talk.h"
#include "app_clock.h"
#include "sfx.h"

static const char *TAG = "ui";

#define DIMMED_BL 4

typedef struct {
    lv_obj_t *(*create)(void);
    void (*tick)(void);
    void (*on_show)(void);
    void (*on_hide)(void);
} view_def_t;

static const view_def_t s_defs[VIEW_COUNT] = {
    [VIEW_HOME]     = {home_create, NULL, home_on_show},
    [VIEW_RING]     = {ring_create, NULL, NULL},
    [VIEW_CLOCK]    = {clock_create, clock_tick, NULL},
    [VIEW_MUSIC]    = {musicv_create, musicv_tick, NULL},
    [VIEW_TALK]     = {talk_create, talk_tick, talk_on_show, talk_on_hide},
    [VIEW_LIGHT]    = {light_create, light_tick, light_on_show},
    [VIEW_STATS]    = {stats_create, stats_tick, NULL},
    [VIEW_SETTINGS] = {setv_create, setv_tick, setv_on_show},
    [VIEW_WIFI]     = {wifiv_create, NULL, wifiv_on_show, wifiv_on_hide},
};

static lv_obj_t *s_scr[VIEW_COUNT];
static view_t s_view = VIEW_HOME;
static lv_obj_t *s_toast, *s_wake_catcher;
static lv_timer_t *s_toast_timer;
static bool s_dimmed, s_screen_off;
static int s_applied_bl = -1;
static int s_last_alarm_minute = -1;

// ---------------------------------------------------------------- widgets

lv_obj_t *ui_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void on_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
    bool used = false;
    if (s_view == VIEW_HOME) used = home_gesture(dir);
    else if (dir == LV_DIR_RIGHT) { ui_back(); used = true; }
    else if (s_view == VIEW_RING && dir == LV_DIR_TOP) { ui_go(VIEW_HOME); used = true; }
    if (used) lv_indev_wait_release(lv_indev_get_act());   // no click after a swipe
}

// Slow swipes don't count as gestures; they arrive as a click on the screen
// background. If the finger travelled, act on the direction it went.
static lv_point_t s_scr_press_at;

static void on_scr_press(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(lv_indev_get_act(), &s_scr_press_at);
}

static void on_scr_click(lv_event_t *e)
{
    (void)e;
    if (s_view == VIEW_HOME) return;                         // the pet screen has its own handling
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    int mx = p.x - s_scr_press_at.x, my = p.y - s_scr_press_at.y;
    if (mx > 30 && mx > LV_ABS(my)) ui_back();
    else if (s_view == VIEW_RING && my < -30 && -my > LV_ABS(mx)) ui_go(VIEW_HOME);
}

lv_obj_t *ui_screen_new(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, UI_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, UI_INK, 0);
    lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(scr, on_scr_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, on_scr_click, LV_EVENT_CLICKED, NULL);
    return scr;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text ? text : "");
    return l;
}

void ui_set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

lv_obj_t *ui_icon(lv_obj_t *parent, const lv_img_dsc_t *src, int scale)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_img_set_src(img, img_scaled(src, scale));
    return img;
}

void ui_icon_set(lv_obj_t *img, const lv_img_dsc_t *src, int scale)
{
    const lv_img_dsc_t *d = img_scaled(src, scale);
    if (lv_img_get_src(img) != d) lv_img_set_src(img, d);
}

lv_obj_t *ui_chip(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, const char *text,
                  lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2 < 12 ? h / 2 : 12, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, UI_PANEL, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, UI_LINE, 0);
    lv_obj_set_style_bg_color(b, UI_RED_DK, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, UI_RED, LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(b, 4);
    lv_obj_t *l = ui_label(b, h >= 40 ? &font_ss_16 : &font_ss_12, UI_SOFT, text);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    return b;
}

void ui_chip_set_on(lv_obj_t *chip, bool on)
{
    lv_obj_set_style_bg_color(chip, on ? UI_RED_DK : UI_PANEL, 0);
    lv_obj_set_style_border_color(chip, on ? UI_RED : UI_LINE, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(chip, 0), on ? UI_INK : UI_SOFT, 0);
}

void ui_chip_set_text(lv_obj_t *chip, const char *text)
{
    ui_set_text(lv_obj_get_child(chip, 0), text);
}

lv_obj_t *ui_round_btn(lv_obj_t *parent, lv_coord_t d, const lv_img_dsc_t *icon, int scale,
                       lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = ui_chip(parent, d, d, "", cb, user_data);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_del(lv_obj_get_child(b, 0));
    if (icon) lv_obj_center(ui_icon(b, icon, scale));
    return b;
}

void ui_show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void anim_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, (lv_coord_t)v, 0); }

void ui_bounce(lv_obj_t *o, int amp, int ms)
{
    lv_anim_del(o, anim_y);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, 0, -amp);
    lv_anim_set_time(&a, ms);
    lv_anim_set_playback_time(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_step);
    lv_anim_start(&a);
}

void ui_stop_bounce(lv_obj_t *o)
{
    lv_anim_del(o, anim_y);
    lv_obj_set_style_translate_y(o, 0, 0);
}

const lv_img_dsc_t *ui_pet_look(dino_face_t face, int scale)
{
    const pet_t *p = home_pet();
    int stage = p->stage == PET_STAGE_EGG || p->stage == PET_STAGE_DEAD ? PET_STAGE_BABY : p->stage;
    return img_scaled(dino_sprite(stage, p->form, face), scale);
}

void ui_date_text(char *buf, size_t n)
{
    static const char *const WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *const MON[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    time_t t = (time_t)app_now();
    struct tm tm;
    gmtime_r(&t, &tm);                       // RTC wall time stored as "UTC"
    snprintf(buf, n, "%s %d %s", WDAY[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
}

static void on_back(lv_event_t *e) { (void)e; ui_back(); }

lv_obj_t *ui_header(lv_obj_t *scr, const char *title)
{
    lv_obj_t *t = ui_label(scr, &font_ss_12, UI_MUTED, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 50);
    // Back: a generous tap area on the left of the title (swipe right also works).
    lv_obj_t *b = ui_box(scr);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, 120, 60);
    lv_obj_align(b, LV_ALIGN_TOP_MID, -70, 22);
    lv_obj_add_event_cb(b, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ic = ui_icon(b, &icon_back, 2);
    lv_obj_align(ic, LV_ALIGN_CENTER, 6, 8);
    return ui_back_hint(scr);
}

// Touch zone behind the hint: the rim from about 9 to 11 o'clock, ~55 px deep.
static void on_hint_hit(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_param(e);
    int dx = info->point->x - SCREEN_PX / 2, dy = info->point->y - SCREEN_PX / 2;
    int r2 = dx * dx + dy * dy;
    float a = atan2f((float)dy, (float)dx) * 180.0f / (float)M_PI;
    if (a < 0) a += 360.0f;
    info->res = r2 >= 125 * 125 && a >= 170.0f && a <= 250.0f;
}

// A short arc on the upper-left edge with dots trailing off it: "swipe from
// here to go back". Tapping it, or a drag of any speed that starts on it, goes back.
lv_obj_t *ui_back_hint(lv_obj_t *scr)
{
    lv_obj_t *zone = ui_box(scr);
    lv_obj_set_size(zone, 140, 205);
    lv_obj_set_pos(zone, 0, 8);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(zone, on_hint_hit, LV_EVENT_HIT_TEST, NULL);
    lv_obj_add_event_cb(zone, on_back, LV_EVENT_CLICKED, NULL);

    lv_obj_t *arc = lv_arc_create(scr);
    lv_obj_set_size(arc, SCREEN_PX - 12, SCREEN_PX - 12);
    lv_obj_center(arc);
    lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_arc_set_bg_angles(arc, 200, 238);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, UI_SOFT, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 4, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_70, LV_PART_MAIN);

    static const struct { int16_t deg; lv_opa_t opa; } dots[3] = {{191, LV_OPA_60}, {184, LV_OPA_40}, {177, LV_OPA_20}};
    for (int i = 0; i < 3; i++) {
        float a = dots[i].deg * (float)M_PI / 180.0f;
        lv_obj_t *d = ui_box(scr);
        lv_obj_set_size(d, 5, 5);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(d, UI_SOFT, 0);
        lv_obj_set_style_bg_opa(d, dots[i].opa, 0);
        lv_obj_set_pos(d, (lv_coord_t)(180 + 172 * cosf(a)) - 2, (lv_coord_t)(180 + 172 * sinf(a)) - 2);
    }
    return arc;
}

static void hint_width_cb(void *o, int32_t v) { lv_obj_set_style_arc_width(o, (lv_coord_t)v, LV_PART_MAIN); }

void ui_back_hint_pulse(lv_obj_t *hint)
{
    if (!hint) return;
    lv_anim_del(hint, hint_width_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, hint);
    lv_anim_set_exec_cb(&a, hint_width_cb);
    lv_anim_set_values(&a, 4, 10);
    lv_anim_set_time(&a, 220);
    lv_anim_set_playback_time(&a, 220);
    lv_anim_set_repeat_count(&a, 2);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------- toast

static void toast_hide(lv_timer_t *t)
{
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(t);
}

void ui_toast(const char *msg)
{
    lv_label_set_text(s_toast, msg);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_timer_reset(s_toast_timer);
    lv_timer_resume(s_toast_timer);
}

// ---------------------------------------------------------------- navigation

view_t ui_view(void) { return s_view; }

void ui_go(view_t v)
{
    if (v >= VIEW_COUNT) return;
    if (!s_scr[v]) s_scr[v] = s_defs[v].create();
    if (v != s_view && s_defs[s_view].on_hide) s_defs[s_view].on_hide();
    s_view = v;
    if (s_defs[v].on_show) s_defs[v].on_show();
    if (s_defs[v].tick) s_defs[v].tick();
    lv_scr_load_anim(s_scr[v], LV_SCR_LOAD_ANIM_FADE_ON, 150, 0, false);
    ui_backlight_refresh();
#if MEM_REPORT_ENABLED
    static const char *const NAMES[VIEW_COUNT] = {"home", "ring", "clock", "music", "talk",
                                                  "light", "stats", "settings", "wifi"};
    mem_report(NAMES[v]);
#endif
}

void ui_back(void)
{
    if (s_view == VIEW_CLOCK && clock_ringing()) return;
    if (s_view == VIEW_LIGHT && light_is_sleep_lamp()) { ui_go(VIEW_HOME); return; }
    if (s_view == VIEW_WIFI) { ui_go(VIEW_SETTINGS); return; }
    ui_go(s_view == VIEW_RING || s_view == VIEW_HOME ? VIEW_HOME : VIEW_RING);
}

// ---------------------------------------------------------------- backlight + dim

void ui_backlight_refresh(void)
{
    int bl;
    if (s_screen_off) bl = 0;
    else if (s_dimmed) bl = DIMMED_BL;
    else if (s_view == VIEW_LIGHT) bl = light_backlight_pct();
    else if (s_view == VIEW_HOME && home_lights_off()) bl = 8;
    else bl = settings_backlight_pct();
    if (bl != s_applied_bl) {
        Set_Backlight((uint8_t)bl);
        s_applied_bl = bl;
    }
}

static void wake_catcher_cb(lv_event_t *e)
{
    (void)e;
    bool sleep_tap = s_view == VIEW_HOME && home_sleeping();
    ui_wake_screen();
    if (sleep_tap) light_open_sleep_lamp();   // sleep mode: a tap is the nightlight switch
}

void ui_wake_screen(void)
{
    if (!s_dimmed) return;
    if (s_screen_off) { s_screen_off = false; power_screen_on(); }
    s_dimmed = false;
    lv_obj_add_flag(s_wake_catcher, LV_OBJ_FLAG_HIDDEN);
    ui_backlight_refresh();
}

static void dim_screen(void)
{
    s_dimmed = true;
    lv_obj_clear_flag(s_wake_catcher, LV_OBJ_FLAG_HIDDEN);   // first tap only wakes
    ui_backlight_refresh();
}

static bool busy(void)
{
    return clock_ringing() || music_playing() || s_view == VIEW_LIGHT;
}

// ---------------------------------------------------------------- periodic work

static void alarm_check(void)
{
    if (!g_settings.alarm_on) return;
    datetime_t d = datetime;
    int minute = d.hour * 60 + d.minute;
    if (d.hour == g_settings.alarm_h && d.minute == g_settings.alarm_m && minute != s_last_alarm_minute &&
        settings_alarm_day(app_now())) {
        s_last_alarm_minute = minute;
        ESP_LOGI(TAG, "alarm %02u:%02u", d.hour, d.minute);
        if (g_settings.alarm_days == 0) {          // a one-time alarm switches itself off
            g_settings.alarm_on = 0;
            settings_save();
        }
        ui_wake_screen();
        ui_go(VIEW_CLOCK);
        clock_ring(true);
    }
}

static void voice_check(void)
{
    int cmd = 0;
    voice_ev_t ev;
    while ((ev = voice_poll(&cmd)) != VOICE_EV_NONE) {
        ui_wake_screen();
        if (s_view == VIEW_HOME) { home_voice(ev, cmd); continue; }
        if (ev == VOICE_EV_WAKE) { sfx_play(SFX_CHIRP); ui_toast("YES?"); }
        else if (ev == VOICE_EV_COMMAND) home_voice(ev, cmd);
    }
}

// Idle ladder: dim -> screen off (+10 s) -> deep sleep (+60 s more).
static void idle_check(void)
{
    if (busy()) return;
    uint32_t idle = lv_disp_get_inactive_time(NULL), dim = settings_dim_ms();
    if (!s_dimmed && idle > dim) dim_screen();
    if (s_dimmed && !s_screen_off && idle > dim + 10000) {
        s_screen_off = true;
        power_screen_off();
        s_applied_bl = 0;
    }
    if (s_screen_off && idle > dim + 70000 && power_can_deep_sleep()) {
        home_save();
        power_deep_sleep();                    // never returns; wakes into a fresh boot
    }
}

static void battery_check(uint32_t secs)
{
    static uint32_t critical_secs, flat_secs;
    battery_sample();
    if (secs == 3) { battery_log_flush(); battery_log("wake"); }
    if (secs % 300 == 0) battery_log(s_screen_off ? "off" : s_dimmed ? "dim" : "awake");
    if (battery_critical()) {
        if (++critical_secs == 10) {           // sustained, not a load dip
            ui_wake_screen();
            ui_toast("CHARGE ME!");
            if (SFX_WARNINGS) sfx_play(SFX_DEATH);
        } else if (critical_secs == 14) {
            home_save();
            power_deep_sleep_no_timer();
        }
    } else {
        critical_secs = 0;
    }
    // Flat: say so, beep once per flat spell, then sleep (also with Voice always on).
    if (battery_flat() && !clock_ringing()) {
        if (++flat_secs == 30) {               // sustained, not a dip while it talks
            ui_wake_screen();
            ui_toast("LOW BATTERY");
            if (SFX_WARNINGS && battery_flat_first()) sfx_play(SFX_ALERT);
        } else if (flat_secs == 34) {
            home_save();
            power_deep_sleep();
        }
    } else {
        flat_secs = 0;
        if (battery_pct() >= 15) battery_flat_clear();
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t n;
    voice_check();
    idle_check();
    if (talk_wants_view()) {                   // "Hey <name>": show the dino answering
        ui_wake_screen();
        if (s_view != VIEW_TALK) ui_go(VIEW_TALK);
    }
    actions_tick();                            // timers, alarm, light, radio asked for by voice
    if (++n % 4) return;                       // the rest once per second
    {
        char state[300];
        home_state_json(state, sizeof state);
        talk_poll(state);
    }
    battery_check(n / 4);
    home_tick();
    music_tick();
    alarm_check();
    timers_tick();
    if (s_defs[s_view].tick) s_defs[s_view].tick();
    ui_backlight_refresh();
}

void ui_start(void)
{
    s_toast = ui_label(lv_layer_top(), &font_ss_16, UI_RED, "");
    lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, 128);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = lv_timer_create(toast_hide, 1400, NULL);
    lv_timer_pause(s_toast_timer);

    s_wake_catcher = ui_box(lv_layer_top());
    lv_obj_set_size(s_wake_catcher, SCREEN_PX, SCREEN_PX);
    lv_obj_add_flag(s_wake_catcher, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_wake_catcher, wake_catcher_cb, LV_EVENT_PRESSED, NULL);

    s_scr[VIEW_HOME] = home_create();
    s_view = VIEW_HOME;
    home_on_show();
    lv_scr_load(s_scr[VIEW_HOME]);
    ui_backlight_refresh();
    lv_timer_create(tick_cb, 250, NULL);
    sfx_play(SFX_BOOT);
    mem_report_start();
    mem_selftest_start();
}

// Home view: the pet, clock, stat rings, alerts, and the swipe-up care tray.
#include <math.h>
#include <stdio.h>

#include "esp_log.h"
#include "app_clock.h"
#include "PCF85063.h"
#include "battery.h"
#include "img_cache.h"
#include "pet_store.h"
#include "settings.h"
#include "sfx.h"
#include "talk.h"
#include "ui.h"
#include "ui_home.h"
#include "ui_light.h"

static const char *TAG = "home";

#define SAVE_EVERY_S   300
#define ALERT_EVERY_S  600
#define GROUND_Y       272          // pet's feet, screen px
#define TRAY_H         160
#define TRAY_LIFT      76           // pet moves up this much when the tray opens
#define TILE           58

typedef enum { ACT_EAT, ACT_PLAY, ACT_WASH, ACT_MED, ACT_MORE, ACT_SLEEP, ACT_BACK } act_t;

static pet_t s_pet;
static uint32_t s_last_save, s_last_alert;
static bool s_tray_open, s_overflow;

static lv_obj_t *s_scr, *s_clock, *s_clock_ampm, *s_alert, *s_bell, *s_ring[4], *s_egg_info;
static lv_obj_t *s_batt, *s_batt_box, *s_batt_fill, *s_batt_bolt, *s_batt_pct;
static lv_obj_t *s_pet_img, *s_shadow, *s_halo, *s_zzz, *s_heart, *s_bubble, *s_sick[2], *s_poop[2];
static lv_obj_t *s_night, *s_hint, *s_egg_btn, *s_wake_btn, *s_tray, *s_tile[4], *s_tile_icon[4], *s_tile_text[4];
static act_t s_tile_act[4];
static int s_face = -1, s_stage = -1, s_mood = -1, s_crack = -1;
static int s_ring_val[4] = {-1, -1, -1, -1};
static bool s_blink;
static lv_coord_t s_lift;           // current tray lift, animated

// ---------------------------------------------------------------- helpers

const pet_t *home_pet(void) { return &s_pet; }
bool home_lights_off(void) { return s_pet.lights_off; }
bool home_sleeping(void) { return s_pet.asleep && s_pet.lights_off && s_pet.stage != PET_STAGE_DEAD; }

// The pet's state in words for Talk (the dino is told never to read out numbers).
static const char *level_word(int32_t v, const char *lo, const char *mid, const char *hi)
{
    return v < PET_STAT_MAX / 4 ? lo : v < PET_STAT_MAX * 3 / 5 ? mid : hi;
}

void home_state_json(char *buf, size_t n)
{
    static const char *const STAGE[] = {"egg", "baby", "child", "teen", "adult", "gone"};
    snprintf(buf, n,
             "{\"name\":\"%s\",\"stage\":\"%s\",\"hunger\":\"%s\",\"happiness\":\"%s\",\"energy\":\"%s\","
             "\"cleanliness\":\"%s\",\"sick\":%s,\"asleep\":%s,\"poops\":%u}",
             g_settings.pet_name, STAGE[s_pet.stage <= PET_STAGE_DEAD ? s_pet.stage : 0],
             level_word(s_pet.hunger, "starving", "a bit hungry", "full"),
             level_word(s_pet.happy, "sad", "okay", "happy"),
             level_word(s_pet.energy, "exhausted", "a bit tired", "full of energy"),
             level_word(s_pet.hygiene, "very dirty", "a bit grubby", "clean"),
             s_pet.sick ? "true" : "false", s_pet.asleep ? "true" : "false", s_pet.poops);
}

void home_save(void)
{
    pet_store_save(&s_pet);
    s_last_save = app_now();
}

static dino_face_t face_for(pet_mood_t m)
{
    switch (m) {
    case PET_MOOD_HAPPY:  return DINO_HAPPY;
    case PET_MOOD_HUNGRY: return DINO_HUNGRY;
    case PET_MOOD_SICK:   return DINO_SICK;
    case PET_MOOD_ASLEEP: return DINO_ASLEEP;
    default:              return DINO_OK;
    }
}

static int need_slot(pet_mood_t m)
{
    switch (m) {
    case PET_MOOD_HUNGRY: return 0;
    case PET_MOOD_DIRTY:  return 2;
    case PET_MOOD_SICK:   return 3;
    default:              return -1;
    }
}

static void anim_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, (lv_coord_t)v, 0); }
static void anim_x(void *o, int32_t v) { lv_obj_set_style_translate_x(o, (lv_coord_t)v, 0); }

// ---------------------------------------------------------------- pet drawing

static void place_pet(void)
{
    // The 40x40 sprite is scaled 4x; its feet sit on row 37.5.
    lv_coord_t top = GROUND_Y - s_lift - (lv_coord_t)(37.5f * 4);
    lv_obj_set_pos(s_pet_img, (SCREEN_PX - 160) / 2, top);
    lv_obj_set_pos(s_shadow, (SCREEN_PX - lv_obj_get_width(s_shadow)) / 2, GROUND_Y - s_lift - 5);
    lv_obj_set_pos(s_halo, SCREEN_PX / 2 - 18 - 22, top + 12);
    lv_obj_set_pos(s_zzz, SCREEN_PX / 2 + 30, top + 6);
    lv_obj_set_pos(s_heart, SCREEN_PX / 2 + 52, top + 30);
    lv_obj_set_pos(s_bubble, SCREEN_PX / 2 + 46, top + 18);
    lv_obj_set_pos(s_sick[0], SCREEN_PX / 2 + 40, top + 20);
    lv_obj_set_pos(s_sick[1], SCREEN_PX / 2 + 52, top + 12);
    lv_obj_set_pos(s_poop[0], SCREEN_PX / 2 - 84 - 12, GROUND_Y - s_lift - 24);
    lv_obj_set_pos(s_poop[1], SCREEN_PX / 2 + 88 - 12, GROUND_Y - s_lift - 24);
}

static void set_idle_anim(pet_mood_t mood)
{
    lv_anim_del(s_pet_img, NULL);
    lv_obj_set_style_translate_x(s_pet_img, 0, 0);
    lv_obj_set_style_translate_y(s_pet_img, 0, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pet_img);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    if (s_pet.stage == PET_STAGE_EGG) {
        int prog = pet_egg_progress(&s_pet, app_now());
        int amp = s_pet.egg_started ? 2 + prog / 40 : 1;
        lv_anim_set_exec_cb(&a, anim_x);
        lv_anim_set_values(&a, -amp, amp);
        lv_anim_set_time(&a, 90);
        lv_anim_set_playback_time(&a, 90);
        lv_anim_set_repeat_delay(&a, s_pet.egg_started ? 2000 - prog * 16 : 4000);
        lv_anim_start(&a);
        return;
    }
    int amp, ms;
    switch (mood) {
    case PET_MOOD_HAPPY:  amp = 6; ms = 260; break;
    case PET_MOOD_OK:     amp = 4; ms = 420; break;
    case PET_MOOD_ASLEEP: amp = 2; ms = 1400; break;
    case PET_MOOD_DEAD:   amp = 4; ms = 900; break;
    default:              amp = 2; ms = 800; break;
    }
    // Two-frame pixel bob (step path), like a handheld LCD.
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, 0, -amp);
    lv_anim_set_time(&a, ms);
    lv_anim_set_playback_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_step);
    lv_anim_start(&a);
}

static void hop_ready(lv_anim_t *a)
{
    (void)a;
    s_mood = -1;                       // force the idle animation to restart
}

static void hop(void)
{
    lv_anim_del(s_pet_img, NULL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pet_img);
    lv_anim_set_exec_cb(&a, anim_y);
    lv_anim_set_values(&a, 0, -12);
    lv_anim_set_time(&a, 110);
    lv_anim_set_playback_time(&a, 110);
    lv_anim_set_ready_cb(&a, hop_ready);
    lv_anim_start(&a);
}

static void blink_cb(lv_timer_t *t)
{
    pet_mood_t m = pet_mood(&s_pet);
    if (s_pet.stage == PET_STAGE_EGG || s_pet.stage == PET_STAGE_DEAD || m == PET_MOOD_ASLEEP) return;
    s_blink = !s_blink;
    lv_timer_set_period(t, s_blink ? 140 : 3200);
    s_face = -1;
    lv_img_set_src(s_pet_img, img_scaled(dino_sprite(s_pet.stage, s_pet.form,
                   s_blink ? DINO_BLINK : face_for(m)), 4));
}

// ---------------------------------------------------------------- tray

static void fill_tray(void)
{
    static const struct { const lv_img_dsc_t *icon; const char *text; } look[] = {
        [ACT_EAT] = {&icon_apple, "EAT"},   [ACT_PLAY] = {&icon_pad, "PLAY"},
        [ACT_WASH] = {&icon_bubbles, "WASH"}, [ACT_MED] = {&icon_pill, "MED"},
        [ACT_MORE] = {&icon_more, "MORE"},  [ACT_SLEEP] = {&icon_moon, "SLEEP"},
        [ACT_BACK] = {&icon_back, "BACK"},
    };
    act_t acts[4];
    int n;
    if (s_overflow) {
        acts[0] = ACT_SLEEP; acts[1] = ACT_MED; acts[2] = ACT_BACK; n = 3;
    } else {
        acts[0] = ACT_EAT; acts[1] = ACT_PLAY; acts[2] = ACT_WASH;
        acts[3] = pet_mood(&s_pet) == PET_MOOD_SICK ? ACT_MED : ACT_MORE; n = 4;
    }
    for (int i = 0; i < 4; i++) {
        ui_show(s_tile[i], i < n);
        ui_show(s_tile_text[i], i < n);
        if (i >= n) continue;
        s_tile_act[i] = acts[i];
        lv_obj_set_x(s_tile[i], (lv_coord_t)(SCREEN_PX / 2 + (i - (n - 1) / 2.0f) * 66 - TILE / 2));
        ui_icon_set(s_tile_icon[i], look[acts[i]].icon, 3);
        ui_set_text(s_tile_text[i], acts[i] == ACT_SLEEP && s_pet.lights_off ? "WAKE" : look[acts[i]].text);
        lv_obj_align_to(s_tile_text[i], s_tile[i], LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    }
}

static void tray_lift_cb(void *o, int32_t v)
{
    (void)o;
    s_lift = (lv_coord_t)v;
    lv_obj_set_y(s_tray, SCREEN_PX - (lv_coord_t)(v * TRAY_H / TRAY_LIFT));
    lv_obj_set_style_opa(s_clock, (lv_opa_t)(255 - v * 255 / TRAY_LIFT), 0);
    lv_obj_set_style_opa(s_clock_ampm, (lv_opa_t)(255 - v * 255 / TRAY_LIFT), 0);
    place_pet();
}

static void tray_open(bool open)
{
    if (open && (s_pet.stage == PET_STAGE_EGG || s_pet.stage == PET_STAGE_DEAD)) return;
    s_tray_open = open;
    if (!open) s_overflow = false;
    fill_tray();
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_tray);
    lv_anim_set_exec_cb(&a, tray_lift_cb);
    lv_anim_set_values(&a, s_lift, open ? TRAY_LIFT : 0);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void refresh(void);

static void do_action(act_t act)
{
    pet_update(&s_pet, app_now());
    bool ok = false;
    switch (act) {
    case ACT_MORE: s_overflow = true; fill_tray(); return;
    case ACT_BACK: s_overflow = false; fill_tray(); return;
    case ACT_EAT:
        ok = pet_feed_meal(&s_pet);
        ui_toast(s_pet.asleep ? "SHH..." : ok ? "YUM!" : "I'M FULL!");
        sfx_play(ok ? SFX_YUM : SFX_NO);
        break;
    case ACT_PLAY:
        ok = pet_play(&s_pet);
        ui_toast(s_pet.asleep ? "SHH..." : ok ? "WHEEE!" : "TOO TIRED");
        sfx_play(ok ? SFX_PLAY : SFX_NO);
        break;
    case ACT_WASH:
        ok = pet_clean(&s_pet);
        ui_toast("SQUEAKY!");
        sfx_play(SFX_CLEAN);
        break;
    case ACT_MED:
        ok = pet_medicine(&s_pet);
        ui_toast(ok ? "BETTER!" : "NOT SICK!");
        sfx_play(ok ? SFX_MED : SFX_NO);
        break;
    case ACT_SLEEP:
        ok = pet_toggle_lights(&s_pet);
        ui_toast(s_pet.lights_off ? (s_pet.asleep ? "NIGHT NIGHT" : "NOT SLEEPY!") : "MORNING!");
        sfx_play(SFX_LIGHT);
        break;
    }
    if (ok) hop();
    home_save();
    tray_open(false);                  // auto-dismiss back to the pet
    refresh();
    ui_backlight_refresh();
}

static void on_tile(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    do_action(s_tile_act[i]);
}

// ---------------------------------------------------------------- refresh

static void refresh(void)
{
    pet_mood_t mood = pet_mood(&s_pet);
    bool dead = s_pet.stage == PET_STAGE_DEAD, egg = s_pet.stage == PET_STAGE_EGG;

    char buf[8];
    app_clock_hhmm(buf, sizeof buf);
    ui_set_text(s_clock, buf);
    const char *ampm = app_clock_ampm(datetime.hour);
    ui_set_text(s_clock_ampm, ampm);
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, ampm[0] ? -10 : 0, 66);      // keep time + AM/PM centred
    lv_obj_update_layout(s_clock);
    lv_obj_align_to(s_clock_ampm, s_clock, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -6);
    ui_show(s_bell, g_settings.alarm_on);

    int32_t stats[4] = {s_pet.hunger, s_pet.happy, s_pet.energy, s_pet.hygiene};
    int prog = egg ? pet_egg_progress(&s_pet, app_now()) : 0;
    for (int i = 0; i < 4; i++) {
        int v = egg ? prog : pet_pct(stats[i]);
        if (v < 2) v = 2;
        if (v != s_ring_val[i]) { lv_arc_set_value(s_ring[i], v); s_ring_val[i] = v; }
    }

    int face = s_pet.stage == PET_STAGE_DEAD ? 0 : face_for(mood);
    int crack = !egg || !s_pet.egg_started ? 0 : prog < 40 ? 0 : prog < 70 ? 1 : prog < 90 ? 2 : 3;
    if (face != s_face || s_pet.stage != s_stage || crack != s_crack) {
        lv_img_set_src(s_pet_img, img_scaled(egg ? egg_sprite(crack) : dino_sprite(s_pet.stage, s_pet.form, (dino_face_t)face), 4));
        if (egg && crack != s_crack && s_crack >= 0) s_mood = -1;   // livelier wobble
        s_face = face;
        s_crack = crack;
    }
    if (egg) {
        char info[24];
        uint32_t h = pet_hatch_time(&s_pet);
        if (h) snprintf(info, sizeof info, "HATCHES %02lu:%02lu", (unsigned long)(h % 86400 / 3600), (unsigned long)(h % 3600 / 60));
        else snprintf(info, sizeof info, "READY WHEN YOU ARE");
        ui_set_text(s_egg_info, info);
    }
    ui_show(s_egg_info, egg && !s_tray_open);

    if (battery_mv()) {
        char pct[8];
        int b = battery_pct();
        bool low = battery_low(), chg = battery_charging();
        snprintf(pct, sizeof pct, "%d%%", b);
        ui_set_text(s_batt_pct, pct);
        lv_obj_set_width(s_batt_fill, b < 8 ? 2 : 14 * b / 100 + 1);
        lv_obj_set_style_bg_color(s_batt_fill, low ? UI_RED : UI_SOFT, 0);
        lv_obj_set_style_text_color(s_batt_pct, low ? UI_RED : UI_MUTED, 0);
        ui_show(s_batt_bolt, chg);
        lv_obj_set_x(s_batt_pct, chg ? 38 : 28);
    }
    ui_show(s_batt, battery_mv() && !s_tray_open);
    if ((int)mood != s_mood || s_pet.stage != s_stage) {
        s_mood = mood;
        s_stage = s_pet.stage;
        set_idle_anim(mood);
        lv_obj_set_width(s_shadow, s_pet.stage == PET_STAGE_BABY ? 76 : 112);
        lv_obj_set_style_img_opa(s_pet_img, dead ? LV_OPA_70 : LV_OPA_COVER, 0);
        place_pet();
    }

    ui_show(s_halo, dead);
    ui_show(s_zzz, mood == PET_MOOD_ASLEEP);
    ui_show(s_heart, mood == PET_MOOD_HAPPY && !egg);
    ui_show(s_bubble, mood == PET_MOOD_HUNGRY);
    ui_show(s_sick[0], mood == PET_MOOD_SICK);
    ui_show(s_sick[1], mood == PET_MOOD_SICK);
    for (int i = 0; i < 2; i++) ui_show(s_poop[i], s_pet.poops > i);
    ui_show(s_night, s_pet.lights_off && !dead);
    ui_show(s_egg_btn, dead || (egg && !s_pet.egg_started));
    ui_chip_set_text(s_egg_btn, dead ? "NEW EGG" : "START EGG");
    ui_show(s_wake_btn, s_pet.lights_off && !dead && !egg && !s_tray_open);
    ui_show(s_hint, !s_tray_open && !egg && !dead && !s_pet.lights_off);
    ui_show(s_alert, need_slot(mood) >= 0 || (pet_needs_attention(&s_pet) && !s_pet.asleep));

    if (s_tray_open) {
        int need = s_overflow ? -1 : need_slot(mood);
        for (int i = 0; i < 4; i++) {
            bool glow = i == need;
            lv_obj_set_style_border_color(s_tile[i], glow ? UI_RED : UI_LINE, 0);
            lv_obj_set_style_shadow_width(s_tile[i], glow ? 16 : 0, 0);
            bool off = s_pet.asleep && (s_tile_act[i] == ACT_EAT || s_tile_act[i] == ACT_PLAY);
            if (off) lv_obj_add_state(s_tile[i], LV_STATE_DISABLED);
            else lv_obj_clear_state(s_tile[i], LV_STATE_DISABLED);
        }
    }
}

// ---------------------------------------------------------------- input

static lv_point_t s_press_at;

static void on_screen_press(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(lv_indev_get_act(), &s_press_at);
}

#define SWIPE_PX 25

static void on_screen_click(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    // LVGL only reports fast swipes as gestures; a slow one arrives here as a
    // click. If the finger travelled, treat it as the swipe it was meant to be.
    int mx = p.x - s_press_at.x, my = p.y - s_press_at.y;
    if (mx * mx + my * my > SWIPE_PX * SWIPE_PX) {
        if (LV_ABS(my) >= LV_ABS(mx)) home_gesture(my < 0 ? LV_DIR_TOP : LV_DIR_BOTTOM);
        return;
    }
    if (s_tray_open) {
        if (p.y < SCREEN_PX - TRAY_H) tray_open(false);
        return;
    }
    if (p.y < 120) { ui_go(VIEW_RING); return; }               // tap the clock for apps (asleep too)
    if (home_sleeping()) {                                      // asleep: tap the dino = nightlight
        int dx = p.x - SCREEN_PX / 2, dy = p.y - 205;
        if (dx * dx + dy * dy < 70 * 70) light_open_sleep_lamp();
        return;
    }
    if (s_pet.stage == PET_STAGE_EGG) {
        int dx = p.x - SCREEN_PX / 2, dy = p.y - 205;
        if (dx * dx + dy * dy > 80 * 80) return;
        if (!s_pet.egg_started) { ui_toast("PRESS START"); return; }
        if (pet_warm_egg(&s_pet)) { sfx_play(SFX_CHIRP); ui_toast("WARMER!"); }
        else ui_toast("TOASTY ENOUGH");
        s_mood = -1;
        home_save();
        refresh();
        return;
    }
    if (s_pet.stage == PET_STAGE_DEAD) return;
    if (p.y > 290) { tray_open(true); return; }
    int dx = p.x - SCREEN_PX / 2, dy = p.y - 205;
    if (dx * dx + dy * dy < 70 * 70 && pet_pat(&s_pet)) {
        sfx_play(SFX_CHIRP);
        ui_toast("<3");
        hop();
        refresh();
    }
}

bool home_gesture(lv_dir_t dir)
{
    if (dir == LV_DIR_TOP) { if (!s_tray_open) tray_open(true); return true; }
    if (dir == LV_DIR_BOTTOM) {
        if (s_tray_open) tray_open(false);
        else ui_go(VIEW_RING);
        return true;
    }
    return false;
}

static void on_wake_btn(lv_event_t *e)
{
    (void)e;
    if (s_pet.lights_off) do_action(ACT_SLEEP);                 // same as the tray's WAKE tile
}

static void on_egg_btn(lv_event_t *e)
{
    (void)e;
    if (s_pet.stage == PET_STAGE_DEAD) {
        pet_new_egg(&s_pet, app_now(), s_pet.generation + 1);   // dormant until Start
        sfx_play(SFX_BOOT);
        ui_toast("A NEW EGG!");
    } else if (pet_start_egg(&s_pet, app_now())) {
        sfx_play(SFX_EVOLVE);
        ui_toast("KEEP ME WARM!");
    }
    s_mood = -1;
    home_save();
    refresh();
}

void home_voice(voice_ev_t ev, int cmd)
{
    if (ev == VOICE_EV_WAKE) {
        sfx_play(SFX_CHIRP);
        ui_toast("YES?");
        if (s_pet.stage != PET_STAGE_EGG && s_pet.stage != PET_STAGE_DEAD) hop();
        return;
    }
    if (ev != VOICE_EV_COMMAND) return;
    // Built-in MultiNet commands (sdkconfig CONFIG_EN_SPEECH_COMMAND_ID0-5). They work
    // offline, after the wake word, even when the server or WiFi is down.
    switch (cmd) {
    case 0: g_settings.bright = 3; settings_save(); ui_toast("BRIGHTER"); break;   // backlight is brightest
    case 1: g_settings.bright = 0; settings_save(); ui_toast("DIMMER"); break;     // backlight is darkest
    case 2: if (!s_pet.lights_off) do_action(ACT_SLEEP); break;                     // turn off the backlight
    case 3: if (s_pet.lights_off) do_action(ACT_SLEEP); break;                      // turn on the backlight
    case 4: light_open_sleep_lamp(); return;                                        // turn on the (night) light
    case 5:                                                                          // turn off the (night) light
        if (ui_view() == VIEW_LIGHT) { light_set_sleep_lamp(false); ui_go(VIEW_HOME); }
        break;
    default: break;
    }
    ui_backlight_refresh();
}

// ---------------------------------------------------------------- periodic

void home_alarm_wake(void)
{
    pet_update(&s_pet, app_now());
    bool egg = s_pet.stage == PET_STAGE_EGG;
    if (pet_wake(&s_pet)) {
        if (egg) { sfx_play(SFX_EVOLVE); ui_toast("IT HATCHED!"); }
        home_save();
        if (ui_view() == VIEW_HOME) refresh();
    }
}

void home_tick(void)
{
    uint32_t now = app_now();
    uint8_t stage_before = s_pet.stage;
    bool sick_before = s_pet.sick, asleep_before = s_pet.asleep;
    if (pet_update(&s_pet, now)) {
        if (s_pet.asleep != asleep_before && s_pet.stage == stage_before) {
            sfx_play(SFX_LIGHT);
            ui_toast(s_pet.asleep ? "BEDTIME..." : "GOOD MORNING!");
            ui_backlight_refresh();
            if (g_settings.rituals && talk_configured()) {      // Deadlee says it out loud (via the server)
                char state[300];
                home_state_json(state, sizeof state);
                talk_routine(s_pet.asleep ? "bedtime" : "morning", state);
            }
        }
        if (s_pet.stage == PET_STAGE_DEAD && stage_before != PET_STAGE_DEAD) {
            sfx_play(SFX_DEATH);
            ui_toast("GOODBYE...");
        } else if (s_pet.stage != stage_before) {
            sfx_play(SFX_EVOLVE);
            ui_toast(stage_before == PET_STAGE_EGG ? "IT HATCHED!" : "IT GREW UP!");
        } else if (s_pet.sick && !sick_before) {
            ui_toast("I FEEL SICK");
        }
        home_save();
    }
    if (pet_needs_attention(&s_pet) && now - s_last_alert >= ALERT_EVERY_S) {
        s_last_alert = now;
        if (SFX_WARNINGS) sfx_play(SFX_ALERT);
    }
    if (now - s_last_save >= SAVE_EVERY_S) home_save();
    if (ui_view() == VIEW_HOME) refresh();
}

void home_on_show(void)
{
    s_face = s_mood = -1;
    refresh();
}

// ---------------------------------------------------------------- build

static void build_rings(void)
{
    static const lv_img_dsc_t *const icon[4] = {&icon_food, &icon_heart, &icon_bolt, &icon_drop};
    static const uint32_t color[4] = {0xD1202F, 0xFF4D5E, 0xE9E9EE, 0x9A9CA6};
    for (int i = 0; i < 4; i++) {
        uint16_t a0 = 194 + i * 40;
        lv_obj_t *a = lv_arc_create(s_scr);
        lv_obj_set_size(a, 342, 342);
        lv_obj_center(a);
        lv_arc_set_bg_angles(a, a0, a0 + 32);
        lv_arc_set_range(a, 0, 100);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(a, 6, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a, UI_TRACK, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(color[i]), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
        s_ring[i] = a;
        float mid = (a0 + 16) * (float)M_PI / 180.0f;
        lv_obj_t *ic = ui_icon(s_scr, icon[i], 2);
        lv_obj_align(ic, LV_ALIGN_CENTER, (lv_coord_t)(148 * cosf(mid)), (lv_coord_t)(148 * sinf(mid)));
    }
}

static void build_tray(void)
{
    s_tray = ui_box(s_scr);
    lv_obj_add_flag(s_tray, LV_OBJ_FLAG_CLICKABLE);          // swallow taps between tiles
    lv_obj_set_size(s_tray, SCREEN_PX, TRAY_H);
    lv_obj_set_pos(s_tray, 0, SCREEN_PX);
    lv_obj_set_style_bg_color(s_tray, UI_TRAY, 0);
    lv_obj_set_style_bg_opa(s_tray, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_tray, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(s_tray, 1, 0);
    lv_obj_set_style_border_color(s_tray, UI_LINE, 0);
    lv_obj_t *handle = ui_box(s_tray);
    lv_obj_set_size(handle, 36, 4);
    lv_obj_set_style_radius(handle, 2, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0x3A3A42), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, 10);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *t = ui_chip(s_tray, TILE, TILE, "", on_tile, (void *)(intptr_t)i);
        lv_obj_set_style_radius(t, 12, 0);
        lv_obj_set_style_shadow_color(t, UI_RED, 0);
        lv_obj_set_style_shadow_width(t, 0, 0);
        lv_obj_set_y(t, 62 - TILE / 2);
        lv_obj_del(lv_obj_get_child(t, 0));
        s_tile_icon[i] = ui_icon(t, &icon_more, 3);
        lv_obj_align(s_tile_icon[i], LV_ALIGN_CENTER, 0, -6);
        s_tile_text[i] = ui_label(s_tray, &font_ss_12, UI_SOFT, "");
        lv_obj_set_style_text_align(s_tile_text[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s_tile_text[i], 70);
        lv_obj_align_to(s_tile_text[i], t, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        s_tile[i] = t;
    }
    fill_tray();
}

lv_obj_t *home_create(void)
{
    uint32_t now = app_now();
    if (!pet_store_load(&s_pet)) {
        ESP_LOGI(TAG, "no save found, laying the first egg");
        pet_new_egg(&s_pet, now, 1);
        pet_store_save(&s_pet);
    } else {
        ESP_LOGI(TAG, "loaded gen %lu %s, catching up %lu s", (unsigned long)s_pet.generation,
                 pet_stage_name(s_pet.stage),
                 (unsigned long)(now > s_pet.last_update ? now - s_pet.last_update : 0));
        pet_update(&s_pet, now);
    }
    s_last_save = now;

    s_scr = ui_screen_new();
    lv_obj_add_event_cb(s_scr, on_screen_press, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scr, on_screen_click, LV_EVENT_CLICKED, NULL);
    build_rings();

    s_clock = ui_label(s_scr, &font_ss_bold_32, UI_INK, "--:--");
    lv_obj_align(s_clock, LV_ALIGN_TOP_MID, 0, 66);
    s_clock_ampm = ui_label(s_scr, &font_ss_12, UI_MUTED, "");
    s_bell = ui_icon(s_scr, &icon_bell, 1);
    lv_obj_align(s_bell, LV_ALIGN_TOP_MID, -92, 76);
    s_egg_info = ui_label(s_scr, &font_ss_12, UI_MUTED, "");
    lv_obj_align(s_egg_info, LV_ALIGN_TOP_MID, 0, 124);

    // Battery: outline + fill + % under the clock; a bolt while (probably) charging.
    s_batt = ui_box(s_scr);
    lv_obj_set_size(s_batt, 76, 14);
    lv_obj_align(s_batt, LV_ALIGN_TOP_MID, 0, 104);
    s_batt_box = ui_box(s_batt);
    lv_obj_set_size(s_batt_box, 22, 12);
    lv_obj_set_pos(s_batt_box, 0, 1);
    lv_obj_set_style_border_width(s_batt_box, 2, 0);
    lv_obj_set_style_border_color(s_batt_box, UI_SOFT, 0);
    lv_obj_set_style_radius(s_batt_box, 2, 0);
    s_batt_fill = ui_box(s_batt_box);
    lv_obj_set_pos(s_batt_fill, 2, 2);
    lv_obj_set_size(s_batt_fill, 14, 4);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_t *nub = ui_box(s_batt);
    lv_obj_set_size(nub, 2, 6);
    lv_obj_set_pos(nub, 22, 4);
    lv_obj_set_style_bg_color(nub, UI_SOFT, 0);
    lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
    s_batt_bolt = ui_icon(s_batt, &icon_bolt, 1);
    lv_obj_set_pos(s_batt_bolt, 27, 2);
    s_batt_pct = ui_label(s_batt, &font_ss_12, UI_MUTED, "");
    lv_obj_set_pos(s_batt_pct, 38, 1);
    s_alert = ui_label(s_scr, &font_ss_bold_32, UI_RED, "!");
    lv_obj_align(s_alert, LV_ALIGN_TOP_MID, 86, 66);

    s_shadow = ui_box(s_scr);
    lv_obj_set_size(s_shadow, 112, 14);
    lv_obj_set_style_radius(s_shadow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_shadow, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_shadow, LV_OPA_60, 0);

    for (int i = 0; i < 2; i++) s_poop[i] = ui_icon(s_scr, &icon_poop, 3);
    s_pet_img = lv_img_create(s_scr);
    s_halo = ui_box(s_scr);
    lv_obj_set_size(s_halo, 44, 12);
    lv_obj_set_style_radius(s_halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_halo, 3, 0);
    lv_obj_set_style_border_color(s_halo, UI_INK, 0);
    s_zzz = ui_label(s_scr, &font_ss_16, lv_color_hex(0x9A9CA6), "z Z z");
    s_heart = ui_icon(s_scr, &icon_heart, 1);
    s_bubble = ui_box(s_scr);
    lv_obj_set_size(s_bubble, 46, 34);
    lv_obj_set_style_radius(s_bubble, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_bubble, lv_color_hex(0x1C1C21), 0);
    lv_obj_set_style_bg_opa(s_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_bubble, 2, 0);
    lv_obj_set_style_border_color(s_bubble, lv_color_hex(0x3A3A42), 0);
    lv_obj_center(ui_icon(s_bubble, &icon_food, 2));
    for (int i = 0; i < 2; i++) s_sick[i] = ui_label(s_scr, &font_ss_16, lv_color_hex(0x9A9CA6), "~");

    s_night = ui_box(s_scr);
    lv_obj_set_size(s_night, SCREEN_PX, SCREEN_PX);
    lv_obj_set_style_bg_color(s_night, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_night, LV_OPA_70, 0);
    lv_obj_t *moon = ui_icon(s_night, &icon_moon, 2);
    lv_obj_align(moon, LV_ALIGN_CENTER, -96, -30);

    s_hint = ui_icon(s_scr, &icon_chev, 2);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_set_style_img_opa(s_hint, LV_OPA_50, 0);

    s_egg_btn = ui_chip(s_scr, 150, 40, "NEW EGG", on_egg_btn, NULL);
    lv_obj_align(s_egg_btn, LV_ALIGN_BOTTOM_MID, 0, -28);
    ui_chip_set_on(s_egg_btn, true);

    // Lights off: wake straight from the pet screen instead of via the care tray.
    s_wake_btn = ui_chip(s_scr, 150, 40, "WAKE", on_wake_btn, NULL);
    lv_obj_align(s_wake_btn, LV_ALIGN_BOTTOM_MID, 0, -28);

    build_tray();
    place_pet();
    lv_timer_create(blink_cb, 3200, NULL);
    refresh();
    return s_scr;
}

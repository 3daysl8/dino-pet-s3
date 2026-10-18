// Stats: the five care bars, age, weight, generation and care stars.
// Layout follows docs/mockup/dino-mockup-v3.html.
#include <stdio.h>

#include "app_clock.h"
#include "ui.h"
#include "ui_home.h"
#include "ui_stats.h"

static lv_obj_t *s_bar[5], *s_bar_val[5], *s_age, *s_weight, *s_gen, *s_star[3];

void stats_tick(void)
{
    const pet_t *p = home_pet();
    bool egg = p->stage == PET_STAGE_EGG;
    int32_t v[5] = {p->hunger, p->happy, p->energy, p->hygiene, p->health};
    char buf[24];
    for (int i = 0; i < 5; i++) {
        int pct = egg ? 0 : pet_pct(v[i]);
        lv_obj_set_width(s_bar[i], pct < 6 ? 10 : 160 * pct / 100);
        snprintf(buf, sizeof buf, egg ? "--" : "%d", pct);
        ui_set_text(s_bar_val[i], buf);
    }
    uint32_t age = pet_age_s(p, app_now());
    if (age >= 86400) snprintf(buf, sizeof buf, "%luD %luH", (unsigned long)(age / 86400), (unsigned long)(age % 86400 / 3600));
    else snprintf(buf, sizeof buf, "%luH %luM", (unsigned long)(age / 3600), (unsigned long)(age % 3600 / 60));
    ui_set_text(s_age, buf);
    snprintf(buf, sizeof buf, "%luG", (unsigned long)p->weight_g);
    ui_set_text(s_weight, buf);
    snprintf(buf, sizeof buf, "%lu", (unsigned long)p->generation);
    ui_set_text(s_gen, buf);
    int stars = p->care_mistakes <= 2 ? 3 : p->care_mistakes <= 6 ? 2 : 1;
    for (int i = 0; i < 3; i++) ui_icon_set(s_star[i], i < stars ? &icon_star : &icon_starO, 2);
}

lv_obj_t *stats_create(void)
{
    static const lv_img_dsc_t *const icon[5] = {&icon_food, &icon_heart, &icon_bolt, &icon_drop, &icon_plus};
    static const uint32_t color[5] = {0xD1202F, 0xFF4D5E, 0xE9E9EE, 0x9A9CA6, 0xD1202F};
    lv_obj_t *scr = ui_screen_new();
    ui_header(scr, "STATS");
    for (int i = 0; i < 5; i++) {
        lv_coord_t y = 92 + i * 30;
        lv_obj_t *ic = ui_icon(scr, icon[i], 2);
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 70, y - 9);
        lv_obj_t *track = ui_box(scr);
        lv_obj_set_size(track, 160, 10);
        lv_obj_set_pos(track, 102, y - 5);
        lv_obj_set_style_radius(track, 5, 0);
        lv_obj_set_style_bg_color(track, UI_TRACK, 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        s_bar[i] = ui_box(track);
        lv_obj_set_size(s_bar[i], 10, 10);
        lv_obj_set_style_radius(s_bar[i], 5, 0);
        lv_obj_set_style_bg_color(s_bar[i], lv_color_hex(color[i]), 0);
        lv_obj_set_style_bg_opa(s_bar[i], LV_OPA_COVER, 0);
        s_bar_val[i] = ui_label(scr, &font_ss_12, UI_INK, "");
        lv_obj_align(s_bar_val[i], LV_ALIGN_TOP_LEFT, 272, y - 7);
    }
    const char *keys[4] = {"AGE", "WEIGHT", "GEN", "CARE"};
    lv_obj_t **vals[3] = {&s_age, &s_weight, &s_gen};
    for (int i = 0; i < 4; i++) {
        lv_coord_t x = i % 2 ? 56 : -56, y = 240 + (i / 2) * 46;
        lv_obj_t *k = ui_label(scr, &font_ss_12, UI_MUTED, keys[i]);
        lv_obj_align(k, LV_ALIGN_TOP_MID, x, y);
        if (i < 3) {
            *vals[i] = ui_label(scr, &font_ss_16, UI_INK, "");
            lv_obj_align(*vals[i], LV_ALIGN_TOP_MID, x, y + 16);
        } else {
            for (int s = 0; s < 3; s++) {
                s_star[s] = ui_icon(scr, &icon_starO, 2);
                lv_obj_align(s_star[s], LV_ALIGN_TOP_MID, x - 18 + s * 18, y + 18);
            }
        }
    }
    stats_tick();
    return scr;
}

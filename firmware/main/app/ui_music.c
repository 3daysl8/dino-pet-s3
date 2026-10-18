// Music: SD card or radio, prev/next beside the dino, rainbow volume arc you can drag.
// Layout follows docs/mockup/dino-mockup-v3.html.
#include <stdio.h>

#include "music.h"
#include "radio.h"
#include "settings.h"
#include "ui.h"
#include "ui_music.h"
#include "ui_vol.h"

static lv_obj_t *s_mus_pet, *s_mus_title, *s_mus_sub, *s_mus_play_icon, *s_mus_sd, *s_mus_radio;
static ui_vol_t *s_mus_vol;
static bool s_mus_bouncing;

static void music_refresh(void)
{
    char buf[48];
    int n = music_count();
    bool radio = music_source() == MUSIC_SRC_RADIO;
    ui_chip_set_on(s_mus_sd, !radio);
    ui_chip_set_on(s_mus_radio, radio);
    if (radio) {
        snprintf(buf, sizeof buf, "%.22s", radio_name(radio_index()));
        for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        ui_set_text(s_mus_title, buf);
        static const char *const st[] = {"TAP PLAY - %d OF %d", "CONNECTING...", "LIVE - %d OF %d",
                                         "NO WIFI - SET UP IN SETUP", "STATION OFFLINE"};
        snprintf(buf, sizeof buf, st[radio_state()], radio_index() + 1, radio_count());
        ui_set_text(s_mus_sub, buf);
    } else if (n == 0) {
        ui_set_text(s_mus_title, "NO MP3 FILES");
        ui_set_text(s_mus_sub, "COPY MP3S TO THE SD CARD");
    } else {
        snprintf(buf, sizeof buf, "%.26s", music_name(music_index()));
        for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        ui_set_text(s_mus_title, buf);
        snprintf(buf, sizeof buf, "TRACK %d OF %d", music_index() + 1, n);
        ui_set_text(s_mus_sub, buf);
    }
    bool playing = music_playing();
    ui_icon_set(s_mus_play_icon, playing ? &icon_pause : &icon_playI, 3);
    if (playing != s_mus_bouncing) {
        s_mus_bouncing = playing;
        lv_img_set_src(s_mus_pet, ui_pet_look(playing ? DINO_HAPPY : DINO_OK, 3));
        if (playing) ui_bounce(s_mus_pet, 6, 230);
        else ui_stop_bounce(s_mus_pet);
    }
    ui_vol_refresh(s_mus_vol);
}

void musicv_tick(void) { music_refresh(); }

static void on_mus(lv_event_t *e)
{
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case 0: music_prev(); break;
    case 1: music_toggle(); break;
    case 2: music_next(); break;
    case 5: music_set_source(MUSIC_SRC_SD); break;
    case 6: music_set_source(MUSIC_SRC_RADIO); break;
    }
    music_refresh();
}

lv_obj_t *musicv_create(void)
{
    music_init();
    lv_obj_t *scr = ui_screen_new();
    ui_header(scr, "MUSIC");
    s_mus_sd = ui_chip(scr, 92, 26, "SD CARD", on_mus, (void *)5);
    lv_obj_align(s_mus_sd, LV_ALIGN_TOP_MID, -50, 72);
    s_mus_radio = ui_chip(scr, 92, 26, "RADIO", on_mus, (void *)6);
    lv_obj_align(s_mus_radio, LV_ALIGN_TOP_MID, 50, 72);

    s_mus_pet = lv_img_create(scr);
    lv_img_set_src(s_mus_pet, ui_pet_look(DINO_OK, 3));
    lv_obj_align(s_mus_pet, LV_ALIGN_TOP_MID, 0, 100);
    s_mus_title = ui_label(scr, &font_ss_12, UI_INK, "");
    lv_obj_align(s_mus_title, LV_ALIGN_TOP_MID, 0, 222);
    s_mus_sub = ui_label(scr, &font_ss_12, UI_MUTED, "");
    lv_obj_align(s_mus_sub, LV_ALIGN_TOP_MID, 0, 238);

    // Previous / next (track on SD, station on radio) either side of the dino.
    lv_obj_t *b = ui_round_btn(scr, 68, &icon_prev, 3, on_mus, (void *)0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, -104, 114);
    b = ui_round_btn(scr, 68, &icon_next, 3, on_mus, (void *)2);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 104, 114);

    s_mus_vol = ui_vol_create(scr);       // before Play, so Play stays on top of its drag band

    b = ui_round_btn(scr, 64, &icon_playI, 3, on_mus, (void *)1);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 256);
    lv_obj_set_style_bg_color(b, UI_RED, 0);
    lv_obj_set_style_border_color(b, UI_RED, 0);
    s_mus_play_icon = lv_obj_get_child(b, 0);

    music_refresh();
    return scr;
}

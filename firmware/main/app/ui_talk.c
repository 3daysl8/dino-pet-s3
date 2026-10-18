// Talk: tap or hold the mic; the reply shows in a bubble and is spoken (talk.c does the work).
// Layout follows docs/mockup/dino-mockup-v3.html.
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "Audio_Driver.h"
#include "music.h"
#include "settings.h"
#include "talk.h"
#include "wifi_mgr.h"
#include "ui.h"
#include "ui_home.h"
#include "ui_talk.h"
#include "ui_vol.h"

// Hold the mic and talk: the recording goes to pet-bridge on the server, the reply
// shows in a speech bubble and is spoken in the dino's voice.
static lv_obj_t *s_talk_pet, *s_talk_heard, *s_talk_bubble, *s_talk_reply, *s_talk_status, *s_talk_mic;
static ui_vol_t *s_talk_vol;
static bool s_talk_mic_cancel, s_talk_replied;      // press ended a conversation / dino has answered in this one
static int64_t s_talk_bounce_until;

static void talk_status_text(const char *t) { ui_set_text(s_talk_status, t); }

static void talk_mic_lit(bool on)
{
    lv_obj_set_style_bg_color(s_talk_mic, on ? UI_RED : UI_PANEL, 0);
    lv_obj_set_style_border_color(s_talk_mic, on ? UI_RED : UI_RED_EDGE, 0);
}

static void talk_finish_recording(void)
{
    talk_mic_lit(false);
    char state[300];
    home_state_json(state, sizeof state);
    if (talk_record_send(state)) {
        ui_set_text(s_talk_heard, "");
        ui_show(s_talk_bubble, false);
    } else {
        talk_status_text("HOLD THE MIC WHILE YOU TALK");
    }
}

// Tap = hands-free conversation (listens until you stop, then for your next reply);
// press and hold = talk while holding. A tap during a conversation ends it.
static void on_talk_mic(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        if (!talk_configured()) return;
        if (talk_session_active()) {
            talk_session_end();
            s_talk_mic_cancel = true;
            talk_mic_lit(false);
            return;
        }
        if (talk_record_start()) {
            if (music_playing()) music_toggle();
            audio_stop_play();                               // stop the dino mid-sentence
            talk_mic_lit(true);
            talk_status_text("LISTENING...");
        }
        return;
    }
    if (s_talk_mic_cancel) { s_talk_mic_cancel = false; return; }
    float held = talk_record_seconds();
    if (held <= 0) return;
    if (held >= 0.5f) { talk_finish_recording(); return; }  // released after holding: send it
    talk_record_send("");                                    // a tap: drop the blip, start listening
    if (talk_session_start()) {
        s_talk_replied = false;
        ui_set_text(s_talk_heard, "");
        ui_show(s_talk_bubble, false);
        talk_mic_lit(true);
        talk_status_text("LISTENING... TALK NOW");
    } else {
        talk_mic_lit(false);
    }
}

void talk_tick(void)
{
    ui_vol_refresh(s_talk_vol);                                     // it may have been changed in Music
    if (s_talk_bounce_until && esp_timer_get_time() > s_talk_bounce_until) {   // a happy hop while it speaks
        ui_stop_bounce(s_talk_pet);
        s_talk_bounce_until = 0;
    }
    switch (talk_status()) {
    case TALK_IDLE: {
        talk_listen_t ls = talk_listen_state();
        if (ls != LISTEN_OFF) {                                     // a tap conversation
            talk_mic_lit(true);
            talk_status_text(ls == LISTEN_HEARING ? "HEARING YOU..." :
                             s_talk_replied ? "GO AHEAD..." : "LISTENING... TALK NOW");
            return;
        }
        float secs = talk_record_seconds();
        if (secs > 0) {
            if (secs >= 15.0f) { talk_finish_recording(); return; }   // longest message
            char buf[32];
            snprintf(buf, sizeof buf, "LISTENING... %dS", (int)secs);
            talk_status_text(buf);
            return;
        }
        break;
    }
    case TALK_WAIT_WIFI: talk_status_text("CONNECTING..."); return;
    case TALK_THINKING: talk_status_text("THINKING..."); return;
    case TALK_DONE:
    case TALK_ERROR: {
        bool ok = talk_status() == TALK_DONE;
        char heard[300], reply[300];
        talk_take(heard, sizeof heard, reply, sizeof reply);
        char you[72];
        for (char *c = heard; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        snprintf(you, sizeof you, heard[0] ? "YOU: %.60s" : "%s", heard);
        ui_set_text(s_talk_heard, you);
        for (char *c = reply; ok && *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;   // pixel font is caps
        ui_set_text(s_talk_reply, reply);
        lv_obj_set_style_text_color(s_talk_reply, ok ? UI_INK : UI_RED, 0);
        ui_show(s_talk_bubble, true);
        if (ok) { ui_bounce(s_talk_pet, 6, 200); s_talk_bounce_until = esp_timer_get_time() + 5000000; }
        if (talk_session_active()) { s_talk_replied = true; return; }   // keep the conversation going
        break;
    }
    }
    talk_mic_lit(false);                                            // idle: not listening
    if (!talk_configured()) talk_status_text("TALK ISN'T SET UP YET");
    else if (talk_listening()) {
        char hint[40], name[16];
        strlcpy(name, g_settings.pet_name, sizeof name);
        for (char *c = name; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
        snprintf(hint, sizeof hint, "TAP MIC OR SAY HEY %s", name);
        talk_status_text(hint);
    } else talk_status_text("TAP OR HOLD THE MIC");
}

void talk_on_show(void)
{
    if (music_playing()) music_toggle();                     // the dino needs the speaker
    wifi_mgr_acquire();                                      // start joining now, before the first tap
}

void talk_on_hide(void)
{
    if (talk_record_seconds() > 0) talk_finish_recording();
    talk_session_end();
    ui_stop_bounce(s_talk_pet);
    wifi_mgr_release();
}

lv_obj_t *talk_create(void)
{
    lv_obj_t *scr = ui_screen_new();
    ui_header(scr, "TALK");
    s_talk_pet = lv_img_create(scr);
    lv_img_set_src(s_talk_pet, ui_pet_look(DINO_HAPPY, 2));
    lv_obj_align(s_talk_pet, LV_ALIGN_TOP_MID, 0, 66);

    s_talk_heard = ui_label(scr, &font_ss_12, UI_MUTED, "");
    lv_obj_align(s_talk_heard, LV_ALIGN_TOP_MID, 0, 140);

    // Speech bubble: grows with the reply, up to four lines.
    s_talk_bubble = ui_box(scr);
    lv_obj_set_size(s_talk_bubble, 280, LV_SIZE_CONTENT);
    lv_obj_align(s_talk_bubble, LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_set_style_radius(s_talk_bubble, 14, 0);
    lv_obj_set_style_bg_color(s_talk_bubble, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_talk_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_talk_bubble, 2, 0);
    lv_obj_set_style_border_color(s_talk_bubble, UI_LINE, 0);
    lv_obj_set_style_pad_all(s_talk_bubble, 10, 0);
    s_talk_reply = ui_label(s_talk_bubble, &font_ss_12, UI_INK, "");
    lv_obj_set_width(s_talk_reply, 256);
    lv_label_set_long_mode(s_talk_reply, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_talk_reply, LV_TEXT_ALIGN_CENTER, 0);
    ui_show(s_talk_bubble, false);

    s_talk_status = ui_label(scr, &font_ss_12, UI_DIM, "");
    lv_obj_align(s_talk_status, LV_ALIGN_TOP_MID, 0, 246);
    s_talk_vol = ui_vol_create(scr);      // before the mic, so the mic stays on top of its drag band
    s_talk_mic = ui_round_btn(scr, 72, &icon_mic, 3, NULL, NULL);
    lv_obj_align(s_talk_mic, LV_ALIGN_TOP_MID, 0, 262);               // clear of the volume ticks
    lv_obj_set_style_border_color(s_talk_mic, UI_RED_EDGE, 0);
    lv_obj_add_flag(s_talk_mic, LV_OBJ_FLAG_PRESS_LOCK);              // keep listening if the finger drifts
    lv_obj_clear_flag(s_talk_mic, LV_OBJ_FLAG_GESTURE_BUBBLE);        // holding must not swipe back
    lv_obj_add_event_cb(s_talk_mic, on_talk_mic, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_talk_mic, on_talk_mic, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_talk_mic, on_talk_mic, LV_EVENT_PRESS_LOST, NULL);
    talk_tick();
    return scr;
}

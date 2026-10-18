// UI core: theme, shared widgets, navigation between views, dimming.
// Design source: docs/mockup/dino-mockup-v3.html.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "assets/dino_assets.h"

typedef enum {
    VIEW_HOME = 0,
    VIEW_RING,
    VIEW_CLOCK,
    VIEW_MUSIC,
    VIEW_TALK,
    VIEW_LIGHT,
    VIEW_STATS,
    VIEW_SETTINGS,
    VIEW_WIFI,
    VIEW_COUNT,
} view_t;

// Palette (black / grey / red)
#define UI_BG       lv_color_hex(0x08080A)
#define UI_PANEL    lv_color_hex(0x17171B)
#define UI_TRAY     lv_color_hex(0x0E0E11)
#define UI_LINE     lv_color_hex(0x2E2E35)
#define UI_TRACK    lv_color_hex(0x1F1F25)
#define UI_INK      lv_color_hex(0xF0F0F3)
#define UI_SOFT     lv_color_hex(0xA6A6AE)
#define UI_MUTED    lv_color_hex(0x8B8B94)
#define UI_DIM      lv_color_hex(0x5B5B63)
#define UI_RED      lv_color_hex(0xD1202F)
#define UI_RED_DK   lv_color_hex(0x2A1015)
#define UI_RED_EDGE lv_color_hex(0x5A1A22)

LV_FONT_DECLARE(font_ss_12)
LV_FONT_DECLARE(font_ss_16)
LV_FONT_DECLARE(font_ss_bold_24)
LV_FONT_DECLARE(font_ss_bold_32)
LV_FONT_DECLARE(font_ss_bold_48)

#define SCREEN_PX 360

void ui_start(void);
void ui_go(view_t v);
void ui_back(void);
view_t ui_view(void);
void ui_toast(const char *msg);
void ui_wake_screen(void);
void ui_backlight_refresh(void);

// Widgets
lv_obj_t *ui_screen_new(void);
lv_obj_t *ui_box(lv_obj_t *parent);                         // plain, non-scrolling container
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_icon(lv_obj_t *parent, const lv_img_dsc_t *src, int scale);
void ui_icon_set(lv_obj_t *img, const lv_img_dsc_t *src, int scale);
lv_obj_t *ui_chip(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, const char *text,
                  lv_event_cb_t cb, void *user_data);
void ui_chip_set_on(lv_obj_t *chip, bool on);
void ui_chip_set_text(lv_obj_t *chip, const char *text);
lv_obj_t *ui_round_btn(lv_obj_t *parent, lv_coord_t d, const lv_img_dsc_t *icon, int scale,
                       lv_event_cb_t cb, void *user_data);
lv_obj_t *ui_header(lv_obj_t *scr, const char *title);   // returns the back-hint arc
lv_obj_t *ui_back_hint(lv_obj_t *scr);  // swipe-back cue on the left edge (ui_header adds it)
void ui_back_hint_pulse(lv_obj_t *hint);   // brief "you can go back now" pulse
void ui_set_text(lv_obj_t *label, const char *text);       // only invalidates when it changed

// Small shared helpers for views
void ui_show(lv_obj_t *o, bool on);                         // clear / set LV_OBJ_FLAG_HIDDEN
void ui_bounce(lv_obj_t *o, int amp, int ms);               // hop up and down until stopped
void ui_stop_bounce(lv_obj_t *o);
const lv_img_dsc_t *ui_pet_look(dino_face_t face, int scale);   // the pet's current look, for apps
void ui_date_text(char *buf, size_t n);                     // "FRI 9 OCT"

// Each view has its own header: ui_home.h, ui_ring.h, ui_clock.h, ui_music.h,
// ui_talk.h, ui_light.h, ui_stats.h, ui_settings.h, ui_wifi.h.

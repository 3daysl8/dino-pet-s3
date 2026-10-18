// App ring: six app buttons around the clock, inside a pixel house that goes home.
// Layout follows docs/mockup/dino-mockup-v3.html.
#include <math.h>

#include "app_clock.h"
#include "PCF85063.h"
#include "ui.h"
#include "ui_light.h"
#include "ui_ring.h"

typedef struct { view_t view; const lv_img_dsc_t *icon; const char *label; } app_t;
static const app_t APPS[6] = {
    {VIEW_CLOCK, &icon_bell, "CLOCK"}, {VIEW_MUSIC, &icon_note, "MUSIC"},
    {VIEW_TALK, &icon_talk, "TALK"},   {VIEW_LIGHT, &icon_bulb, "LIGHT"},
    {VIEW_STATS, &icon_bars, "STATS"}, {VIEW_SETTINGS, &icon_gear, "SETUP"},
};
static lv_obj_t *s_ring_time, *s_ring_date, *s_ring_ampm;

static void on_app(lv_event_t *e)
{
    view_t v = (view_t)(intptr_t)lv_event_get_user_data(e);
    if (v == VIEW_LIGHT) light_set_sleep_lamp(false);       // full nightlight app with controls
    ui_go(v);
}
static void on_ring_center(lv_event_t *e) { (void)e; ui_go(VIEW_HOME); }

static void ring_refresh_cb(lv_timer_t *t)
{
    (void)t;
    if (ui_view() != VIEW_RING) return;
    char buf[24];
    app_clock_hhmm(buf, sizeof buf);
    ui_set_text(s_ring_time, buf);
    const char *ampm = app_clock_ampm(datetime.hour);
    ui_set_text(s_ring_ampm, ampm);
    lv_obj_align(s_ring_time, LV_ALIGN_CENTER, ampm[0] ? -10 : 0, 6);
    lv_obj_update_layout(s_ring_time);
    lv_obj_align_to(s_ring_ampm, s_ring_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -4);
    ui_date_text(buf, sizeof buf);
    ui_set_text(s_ring_date, buf);
}

// Pixel house outline around the ring's clock: the way home. Offsets from the
// screen centre; it fits inside the six app buttons (free radius ~78 px).
#define HOUSE_W2    56                 // half width of the walls
#define HOUSE_EAVE  64                 // half width at the eaves (roof overhang)
#define HOUSE_TOP  (-20)               // eave line
#define HOUSE_BOT   54                 // floor
#define HOUSE_PEAK (-74)               // roof peak
#define ROOF_STEPS  9                  // staircase steps per side: the pixel-art look
static lv_point_t s_house_pts[6 + 4 * ROOF_STEPS];   // lv_line keeps a pointer to its points

static lv_obj_t *house_create(lv_obj_t *scr)
{
    int n = 0;
#define PT(x, y) (s_house_pts[n++] = (lv_point_t){(lv_coord_t)(SCREEN_PX / 2 + (x)), (lv_coord_t)(SCREEN_PX / 2 + (y))})
#define RX(i) (HOUSE_EAVE * (i) / ROOF_STEPS)                          // 0..EAVE
#define RY(i) (HOUSE_TOP + (HOUSE_PEAK - HOUSE_TOP) * (i) / ROOF_STEPS) // TOP..PEAK
    PT(-HOUSE_W2, HOUSE_BOT);
    PT(-HOUSE_W2, HOUSE_TOP);
    PT(-HOUSE_EAVE, HOUSE_TOP);
    for (int i = 1; i <= ROOF_STEPS; i++) {                 // up the left side: up, then right
        PT(-HOUSE_EAVE + RX(i - 1), RY(i));
        PT(-HOUSE_EAVE + RX(i), RY(i));
    }
    for (int i = 1; i <= ROOF_STEPS; i++) {                 // down the right side: right, then down
        PT(RX(i), RY(ROOF_STEPS - i + 1));
        PT(RX(i), RY(ROOF_STEPS - i));
    }
    PT(HOUSE_W2, HOUSE_TOP);
    PT(HOUSE_W2, HOUSE_BOT);
    PT(-HOUSE_W2, HOUSE_BOT);
#undef PT
#undef RX
#undef RY
    lv_obj_t *l = lv_line_create(scr);
    lv_line_set_points(l, s_house_pts, n);
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_style_line_width(l, 3, 0);
    lv_obj_set_style_line_color(l, UI_SOFT, 0);
    return l;
}

lv_obj_t *ring_create(void)
{
    lv_obj_t *scr = ui_screen_new();
    house_create(scr);
    // The whole house is the "go home" button.
    lv_obj_t *center = ui_box(scr);
    lv_obj_add_flag(center, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(center, 2 * HOUSE_EAVE, HOUSE_BOT - HOUSE_PEAK);
    lv_obj_align(center, LV_ALIGN_CENTER, 0, (HOUSE_BOT + HOUSE_PEAK) / 2);
    lv_obj_add_event_cb(center, on_ring_center, LV_EVENT_CLICKED, NULL);
    lv_obj_align(ui_icon(scr, &icon_home, 2), LV_ALIGN_CENTER, 0, -46);
    lv_obj_align(ui_label(scr, &font_ss_12, UI_SOFT, "HOME"), LV_ALIGN_CENTER, 0, -30);
    s_ring_time = ui_label(scr, &font_ss_bold_24, UI_INK, "");
    lv_obj_align(s_ring_time, LV_ALIGN_CENTER, 0, 6);
    s_ring_date = ui_label(scr, &font_ss_12, UI_MUTED, "");
    lv_obj_align(s_ring_date, LV_ALIGN_CENTER, 0, 34);
    s_ring_ampm = ui_label(scr, &font_ss_12, UI_MUTED, "");
    for (int i = 0; i < 6; i++) {
        float a = (-90 + i * 60) * (float)M_PI / 180.0f;
        lv_obj_t *b = ui_chip(scr, 68, 68, "", on_app, (void *)(intptr_t)APPS[i].view);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        if (APPS[i].view == VIEW_TALK) lv_obj_set_style_border_color(b, UI_RED_EDGE, 0);
        lv_obj_align(b, LV_ALIGN_CENTER, (lv_coord_t)(112 * cosf(a)), (lv_coord_t)(112 * sinf(a)));
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_obj_set_style_text_font(l, &font_ss_12, 0);
        lv_label_set_text(l, APPS[i].label);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 16);
        lv_obj_t *ic = ui_icon(b, APPS[i].icon, 2);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, -8);
    }
    lv_timer_create(ring_refresh_cb, 1000, NULL);
    ring_refresh_cb(NULL);
    return scr;
}

// Speaker volume control shared by Music and Talk.
// Layout follows docs/mockup/dino-mockup-v3.html (Music).
#include <math.h>

#include "music.h"
#include "settings.h"
#include "ui.h"
#include "ui_vol.h"

#define VOL_MAX    20
#define VOL_TICKS  40                  // two ticks per volume step
#define VOL_A0     155.0f              // arc start (left end, volume 0), degrees clockwise from 3 o'clock
#define VOL_A1     25.0f               // arc end (right end, volume max)

struct ui_vol {
    lv_obj_t *tick[VOL_TICKS];
    lv_point_t pts[VOL_TICKS][2];      // lv_line keeps a pointer to its points
    int drawn;
};
static bool s_vol_dirty;               // one volume setting, however many arcs show it

// Red -> orange -> green -> blue along the arc.
static lv_color_t vol_color(int i)
{
    static const uint32_t stop[4] = {0xD1202F, 0xFF8A00, 0x3DDC84, 0x4D7CFF};
    int t = i * 255 * 3 / (VOL_TICKS - 1), k = t / 255;
    if (k >= 3) return lv_color_hex(stop[3]);
    return lv_color_mix(lv_color_hex(stop[k + 1]), lv_color_hex(stop[k]), (uint8_t)(t % 255));
}

void ui_vol_refresh(ui_vol_t *v)
{
    if (!v || v->drawn == g_settings.music_vol) return;
    v->drawn = g_settings.music_vol;
    for (int i = 0; i < VOL_TICKS; i++)
        lv_obj_set_style_line_color(v->tick[i], i < v->drawn * 2 ? vol_color(i) : UI_LINE, 0);
}

static void vol_set(ui_vol_t *v, int level)
{
    if (level < 0) level = 0;
    if (level > VOL_MAX) level = VOL_MAX;
    if (level == g_settings.music_vol) return;
    g_settings.music_vol = (uint8_t)level;
    music_set_vol(g_settings.music_vol);
    s_vol_dirty = true;
    ui_vol_refresh(v);
}

static void vol_save(void)
{
    if (s_vol_dirty) { settings_save(); s_vol_dirty = false; }
}

// Angle of a screen point around the centre, degrees clockwise from 3 o'clock (0..360).
static float point_angle(const lv_point_t *p)
{
    float a = atan2f((float)(p->y - SCREEN_PX / 2), (float)(p->x - SCREEN_PX / 2)) * 180.0f / (float)M_PI;
    return a < 0 ? a + 360.0f : a;
}

// The drag band is a box over the bottom of the screen, but only the ring
// near the edge counts as a hit, so the buttons and text above it stay tappable.
static void on_vol_hit(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_param(e);
    int dx = info->point->x - SCREEN_PX / 2, dy = info->point->y - SCREEN_PX / 2;
    int r2 = dx * dx + dy * dy;
    float a = point_angle(info->point);
    info->res = r2 >= 128 * 128 && r2 <= 190 * 190 && a >= 8.0f && a <= 172.0f;
}

static void on_vol_drag(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) { vol_save(); return; }
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    float a = point_angle(&p);
    if (a > 270.0f) a = 0.0f;                                   // past the right end
    else if (a > 180.0f) a = 180.0f;                            // past the left end
    float t = (VOL_A0 - a) / (VOL_A0 - VOL_A1);                 // 0 at the left end, 1 at the right
    vol_set(lv_event_get_user_data(e), (int)lroundf(t * VOL_MAX));
}

static void on_vol_down(lv_event_t *e) { vol_set(lv_event_get_user_data(e), g_settings.music_vol - 1); vol_save(); }
static void on_vol_up(lv_event_t *e) { vol_set(lv_event_get_user_data(e), g_settings.music_vol + 1); vol_save(); }

ui_vol_t *ui_vol_create(lv_obj_t *scr)
{
    ui_vol_t *v = lv_mem_alloc(sizeof *v);                    // lives as long as the screen does
    if (!v) return NULL;

    // Tick marks around the bottom edge.
    for (int i = 0; i < VOL_TICKS; i++) {
        float a = (VOL_A0 - (VOL_A0 - VOL_A1) * i / (VOL_TICKS - 1)) * (float)M_PI / 180.0f;
        float c = cosf(a), sn = sinf(a);
        v->pts[i][0] = (lv_point_t){(lv_coord_t)(180 + 156 * c), (lv_coord_t)(180 + 156 * sn)};
        v->pts[i][1] = (lv_point_t){(lv_coord_t)(180 + 170 * c), (lv_coord_t)(180 + 170 * sn)};
        lv_obj_t *l = lv_line_create(scr);
        lv_line_set_points(l, v->pts[i], 2);
        lv_obj_set_pos(l, 0, 0);
        lv_obj_set_style_line_width(l, 4, 0);
        lv_obj_set_style_line_rounded(l, true, 0);
        v->tick[i] = l;
    }
    v->drawn = -1;

    // Drag band over the ticks.
    lv_obj_t *band = ui_box(scr);
    lv_obj_set_size(band, SCREEN_PX, SCREEN_PX - 180);
    lv_obj_align(band, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(band, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(band, LV_OBJ_FLAG_GESTURE_BUBBLE);      // dragging right must not mean "back"
    lv_obj_add_event_cb(band, on_vol_hit, LV_EVENT_HIT_TEST, NULL);
    lv_obj_add_event_cb(band, on_vol_drag, LV_EVENT_PRESSED, v);
    lv_obj_add_event_cb(band, on_vol_drag, LV_EVENT_PRESSING, v);
    lv_obj_add_event_cb(band, on_vol_drag, LV_EVENT_RELEASED, v);
    lv_obj_add_event_cb(band, on_vol_drag, LV_EVENT_PRESS_LOST, v);

    // -/+ at the two ends of the arc: one step each.
    static const struct { const char *t; int x; lv_event_cb_t cb; } ends[2] = {
        {"-", 32, on_vol_down}, {"+", 328, on_vol_up}};
    for (int k = 0; k < 2; k++) {
        lv_obj_t *t = ui_box(scr);
        lv_obj_set_size(t, 40, 40);
        lv_obj_set_pos(t, ends[k].x - 20, 192);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, ends[k].cb, LV_EVENT_CLICKED, v);
        lv_obj_center(ui_label(t, &font_ss_16, UI_SOFT, ends[k].t));
    }
    ui_vol_refresh(v);
    return v;
}

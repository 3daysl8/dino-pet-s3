#include "t9.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "ui.h"

enum { T9_LOWER = 0, T9_UPPER, T9_DIGITS };
// Bottom row: case key, digits key, space, delete.
enum { K_SYM = 0, K_CASE = 9, K_NUM = 10, K_SPACE = 11, K_DEL = 12 };
#define T9_COMMIT_MS 1000

static const char *const T9_KEYS[9] = {".,!?@#-_&*$%1", "abc2", "def3", "ghi4", "jkl5", "mno6", "pqrs7", "tuv8", "wxyz9"};
// The case and digits keys are labelled with what a tap does: CAPS / abc, 123 / 0.
static const char *const T9_MAP[3][17] = {
    {"1.,", "abc", "def", "\n", "ghi", "jkl", "mno", "\n", "pqrs", "tuv", "wxyz", "\n", "CAPS", "123", "SPACE", "DEL", ""},
    {"1.,", "ABC", "DEF", "\n", "GHI", "JKL", "MNO", "\n", "PQRS", "TUV", "WXYZ", "\n", "abc", "123", "SPACE", "DEL", ""},
    {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n", "abc", "0", "SPACE", "DEL", ""},
};

struct t9 {
    lv_obj_t *btnm, *ta;
    lv_timer_t *timer;
    int mode, key, idx;                     // key: the key with a letter still cycling (-1 = none)
};

void t9_commit(t9_t *k)
{
    if (k->key >= 0) lv_btnmatrix_clear_btn_ctrl(k->btnm, (uint16_t)k->key, LV_BTNMATRIX_CTRL_CHECKED);
    k->key = -1;
    lv_timer_pause(k->timer);
}

static void t9_timer_cb(lv_timer_t *t) { t9_commit(t->user_data); }

void t9_reset(t9_t *k)
{
    t9_commit(k);
    k->mode = T9_LOWER;
    lv_btnmatrix_set_map(k->btnm, (const char **)T9_MAP[k->mode]);
}

static char t9_char(t9_t *k, int key, int idx)
{
    const char *set = T9_KEYS[key];
    char c = set[idx % strlen(set)];
    if (k->mode == T9_UPPER && c >= 'a' && c <= 'z') c -= 32;
    return c;
}

static void on_t9(lv_event_t *e)
{
    t9_t *k = lv_event_get_user_data(e);
    uint16_t id = lv_btnmatrix_get_selected_btn(k->btnm);
    if (id == LV_BTNMATRIX_BTN_NONE) return;
    int key = id;
    if (key <= 8 && k->mode != T9_DIGITS) {
        if (key == k->key) {                                    // same key again: next letter
            lv_textarea_del_char(k->ta);
            k->idx++;
        } else {
            t9_commit(k);
            k->key = key;
            k->idx = 0;
            lv_btnmatrix_set_btn_ctrl(k->btnm, id, LV_BTNMATRIX_CTRL_CHECKED);
        }
        lv_textarea_add_char(k->ta, (uint32_t)t9_char(k, key, k->idx));
        lv_timer_reset(k->timer);
        lv_timer_resume(k->timer);
        return;
    }
    if (key == K_DEL) {                                         // also cancels a cycling letter
        t9_commit(k);
        lv_textarea_del_char(k->ta);
        return;
    }
    t9_commit(k);
    int mode = k->mode;
    if (key <= 8) lv_textarea_add_char(k->ta, (uint32_t)('1' + key));    // 123 mode
    else if (key == K_SPACE) lv_textarea_add_char(k->ta, ' ');
    else if (key == K_CASE) mode = mode == T9_LOWER ? T9_UPPER : T9_LOWER;   // CAPS <-> abc
    else if (key == K_NUM) {
        if (mode == T9_DIGITS) lv_textarea_add_char(k->ta, '0');
        else mode = T9_DIGITS;
    }
    if (mode != k->mode) {
        k->mode = mode;
        lv_btnmatrix_set_map(k->btnm, (const char **)T9_MAP[k->mode]);
    }
}

t9_t *t9_create(lv_obj_t *parent, lv_obj_t *textarea, lv_coord_t y)
{
    t9_t *k = heap_caps_calloc(1, sizeof *k, MALLOC_CAP_SPIRAM);
    k->ta = textarea;
    k->key = -1;
    lv_obj_t *b = k->btnm = lv_btnmatrix_create(parent);
    lv_btnmatrix_set_map(b, (const char **)T9_MAP[T9_LOWER]);
    lv_obj_set_size(b, 288, 166);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);           // swiping on the keys must not leave
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_pad_gap(b, 6, 0);
    lv_obj_set_style_text_font(b, &font_ss_16, LV_PART_ITEMS);
    lv_obj_set_style_text_color(b, UI_INK, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(b, UI_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_radius(b, 12, LV_PART_ITEMS);
    lv_obj_set_style_border_width(b, 2, LV_PART_ITEMS);
    lv_obj_set_style_border_color(b, UI_LINE, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(b, UI_RED_DK, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, UI_RED, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, UI_RED_DK, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, UI_INK, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_add_event_cb(b, on_t9, LV_EVENT_VALUE_CHANGED, k);
    k->timer = lv_timer_create(t9_timer_cb, T9_COMMIT_MS, k);
    lv_timer_pause(k->timer);
    return k;
}

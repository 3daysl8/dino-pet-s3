// T9 keypad: phone-style multi-tap typing into an lv_textarea. Tap a key
// repeatedly to cycle its letters (then its digit); the letter commits after a
// pause or when another key is pressed. Bottom row: CAPS/abc toggles capitals,
// 123 switches to digits (where it types 0), SPACE, DEL.
#pragma once

#include "lvgl.h"

typedef struct t9 t9_t;

t9_t *t9_create(lv_obj_t *parent, lv_obj_t *textarea, lv_coord_t y);   // 288 x 166, centred at y
void t9_commit(t9_t *k);      // lock in a letter that's still cycling
void t9_reset(t9_t *k);       // commit and go back to lowercase

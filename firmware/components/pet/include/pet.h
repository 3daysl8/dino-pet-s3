// Pet state machine. Pure C, no ESP-IDF dependencies, so it can be unit-tested
// on a PC (see firmware/test/pet). All times are Unix seconds from the RTC.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PET_SAVE_VERSION 3

// Stats are stored x100 (0..10000) so slow per-minute decay doesn't round to zero.
#define PET_STAT_MAX 10000

typedef enum {
    PET_STAGE_EGG = 0,
    PET_STAGE_BABY,
    PET_STAGE_CHILD,
    PET_STAGE_TEEN,
    PET_STAGE_ADULT,
    PET_STAGE_DEAD,
} pet_stage_t;

// Adult form is decided by how well it was looked after.
typedef enum {
    PET_FORM_NONE = 0,
    PET_FORM_STAR,    // 0-2 care mistakes
    PET_FORM_BUDDY,   // 3-6
    PET_FORM_GRUMP,   // 7+
} pet_form_t;

typedef enum {
    PET_MOOD_HAPPY = 0,
    PET_MOOD_OK,
    PET_MOOD_HUNGRY,
    PET_MOOD_SAD,
    PET_MOOD_TIRED,
    PET_MOOD_DIRTY,
    PET_MOOD_SICK,
    PET_MOOD_ASLEEP,
    PET_MOOD_DEAD,
} pet_mood_t;

typedef struct {
    uint32_t version;
    uint32_t generation;    // 1 for the first pet, +1 per new egg
    uint32_t born_at;       // egg laid
    uint32_t hatched_at;    // 0 while still an egg
    uint32_t last_update;   // time the stats were last advanced to
    uint32_t died_at;

    int32_t hunger;         // full = PET_STAT_MAX, starving = 0
    int32_t happy;
    int32_t energy;
    int32_t hygiene;
    int32_t health;

    uint32_t weight_g;      // grams, cosmetic + overfeeding penalty
    uint32_t poop_timer_s;  // awake seconds since last poop
    uint32_t sick_timer_s;  // seconds spent in unhealthy conditions
    uint32_t care_mistakes;
    uint8_t  poops;
    uint8_t  stage;         // pet_stage_t
    uint8_t  form;          // pet_form_t
    bool     sick;
    bool     asleep;
    bool     lights_off;
    uint8_t  zero_flags;    // bit per stat already counted as a care mistake

    // --- added in save version 2 (fields after this line may be missing in v1 saves)
    uint32_t bed_night;     // night_id() of the last automatic bedtime
    bool     night_sleep;   // asleep for the night: stays asleep until wake-up time

    // --- added in save version 3
    uint32_t egg_started;   // 0 = dormant egg (waiting for Start); else incubation start
    uint32_t egg_warm_s;    // seconds taken off incubation by warming taps
} pet_t;

// Sizes of older saves, for migration (new fields start zeroed).
#define PET_SAVE_V1_SIZE offsetof(pet_t, bed_night)
#define PET_SAVE_V2_SIZE offsetof(pet_t, egg_started)

#define PET_EGG_MIN_S        (6u * 3600u)   // incubation lasts at least this long...
#define PET_EGG_WARM_STEP_S  120u           // ...minus 2 min per warming tap
#define PET_EGG_WARM_MAX_S   (3u * 3600u)   // capped at 3 h of warming

// Daily schedule, minutes after midnight (RTC local time). Default 20:00 / 07:00.
// At bedtime the pet goes to sleep and the lights go out; it then sleeps through
// until wake-up time unless woken (pet_wake / pet_toggle_lights).
void pet_set_schedule(uint16_t bed_min, uint16_t wake_min);
bool pet_is_night(uint32_t now);

// Eggs: a new egg is dormant until pet_start_egg(). It then hatches at the first
// wake-up time at least PET_EGG_MIN_S after starting, minus any warming.
bool pet_start_egg(pet_t *p, uint32_t now);
bool pet_warm_egg(pet_t *p);
uint32_t pet_hatch_time(const pet_t *p);           // 0 while dormant
int pet_egg_progress(const pet_t *p, uint32_t now); // 0..100

// Start a fresh egg. generation is carried by the caller (old + 1).
void pet_new_egg(pet_t *p, uint32_t now, uint32_t generation);

// Advance the simulation to `now`. Safe to call with a big gap (power-off
// catch-up): it steps minute by minute, capped at PET_MAX_CATCHUP_S.
// Returns true if anything visible changed (stage, poop, sick, death...).
bool pet_update(pet_t *p, uint32_t now);
#define PET_MAX_CATCHUP_S (7u * 24u * 3600u)

// Actions. Each returns false if the action isn't allowed right now
// (dead, egg, asleep...). They do NOT advance time; call pet_update first.
bool pet_feed_meal(pet_t *p);
bool pet_feed_snack(pet_t *p);
bool pet_play(pet_t *p);
bool pet_clean(pet_t *p);
bool pet_medicine(pet_t *p);
bool pet_toggle_lights(pet_t *p);
bool pet_pat(pet_t *p);   // tap on the pet: tiny happiness bump
bool pet_wake(pet_t *p);  // gentle wake (alarm): no grumpiness, lights on; hatches a ready egg

pet_mood_t pet_mood(const pet_t *p);
bool pet_needs_attention(const pet_t *p);
uint32_t pet_age_s(const pet_t *p, uint32_t now);
static inline int pet_pct(int32_t stat) { return (stat + 50) / 100; }

const char *pet_stage_name(pet_stage_t s);
const char *pet_mood_name(pet_mood_t m);

#ifdef __cplusplus
}
#endif

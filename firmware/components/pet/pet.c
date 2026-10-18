#include "pet.h"

#include <string.h>

// ---- Tuning. Rates are "x100 units per simulated minute" (600 = 6%/hour). ----
#define STEP_S              60u
#define BABY_UNTIL_S        3600u           // 1 h after hatching
#define CHILD_UNTIL_S       (24u * 3600u)   // 1 day
#define TEEN_UNTIL_S        (72u * 3600u)   // 3 days

#define HUNGER_AWAKE        10              // 6 %/h  (full -> empty ~17 h)
#define HUNGER_AWAKE_BABY   20              // babies eat more often
#define HUNGER_ASLEEP       4
#define HAPPY_AWAKE         8
#define HAPPY_ASLEEP_LIGHT  6               // sleeping with the lights on
#define ENERGY_AWAKE        7
#define ENERGY_ASLEEP       (-30)           // recovers ~18 %/h
#define HYGIENE_AWAKE       5
#define HYGIENE_ASLEEP      3
#define HYGIENE_PER_POOP    5

#define POOP_EVERY_S        (3u * 3600u)
#define POOP_EVERY_BABY_S   (90u * 60u)
#define MAX_POOPS           4

#define SICK_AFTER_S        3600u           // 1 h of bad conditions
#define HEALTH_SICK         12              // ~14 h from full to dead
#define HEALTH_STARVING     15
#define HEALTH_MISERABLE    3
#define HEALTH_RECOVER      5

#define LOW                 2500
#define VERY_LOW            1500

enum { ZF_HUNGER = 1, ZF_HAPPY = 2, ZF_HYGIENE = 4 };

static uint16_t s_bed_min = 20 * 60, s_wake_min = 7 * 60;

void pet_set_schedule(uint16_t bed_min, uint16_t wake_min)
{
    s_bed_min = bed_min % 1440;
    s_wake_min = wake_min % 1440;
}

bool pet_is_night(uint32_t t)
{
    uint32_t m = (t % 86400u) / 60u;
    if (s_bed_min > s_wake_min) return m >= s_bed_min || m < s_wake_min;   // e.g. 20:00-07:00
    return m >= s_bed_min && m < s_wake_min;
}

// Each night (bedtime to wake-up) gets one id, so bedtime triggers once per night.
static uint32_t night_id(uint32_t t)
{
    return (t - s_wake_min * 60u) / 86400u;
}

static int32_t clamp(int32_t v)
{
    if (v < 0) return 0;
    if (v > PET_STAT_MAX) return PET_STAT_MAX;
    return v;
}

void pet_new_egg(pet_t *p, uint32_t now, uint32_t generation)
{
    memset(p, 0, sizeof(*p));
    p->version = PET_SAVE_VERSION;
    p->generation = generation ? generation : 1;
    p->born_at = now;
    p->last_update = now;
    p->hunger = p->happy = p->energy = p->hygiene = p->health = PET_STAT_MAX;
    p->weight_g = 5;
    p->stage = PET_STAGE_EGG;
}

static pet_stage_t stage_for_age(uint32_t since_hatch)
{
    if (since_hatch < BABY_UNTIL_S) return PET_STAGE_BABY;
    if (since_hatch < CHILD_UNTIL_S) return PET_STAGE_CHILD;
    if (since_hatch < TEEN_UNTIL_S) return PET_STAGE_TEEN;
    return PET_STAGE_ADULT;
}

// Count a care mistake the first time a stat bottoms out; re-arm once it recovers.
static void track_zero(pet_t *p, int32_t stat, uint8_t flag)
{
    if (stat == 0 && !(p->zero_flags & flag)) {
        p->zero_flags |= flag;
        p->care_mistakes++;
    } else if (stat > LOW) {
        p->zero_flags &= (uint8_t)~flag;
    }
}

// One simulated minute ending at time t. Returns true on a visible event.
static bool step(pet_t *p, uint32_t t)
{
    bool event = false;

    if (p->stage == PET_STAGE_EGG) {
        uint32_t hatch = pet_hatch_time(p);
        if (hatch && t >= hatch) {
            p->stage = PET_STAGE_BABY;
            p->hatched_at = t;
            event = true;
        }
        return event;
    }

    pet_stage_t s = stage_for_age(t - p->hatched_at);
    if (s != p->stage) {
        if (s == PET_STAGE_ADULT) {
            p->form = p->care_mistakes <= 2 ? PET_FORM_STAR
                    : p->care_mistakes <= 6 ? PET_FORM_BUDDY
                    : PET_FORM_GRUMP;
        }
        p->stage = (uint8_t)s;
        p->weight_g += 5;
        event = true;
    }
    bool baby = p->stage == PET_STAGE_BABY;

    // Daily schedule: bedtime once per night, then sleep through to wake-up time.
    bool night = pet_is_night(t);
    if (night && !p->asleep && p->bed_night != night_id(t)) {
        p->asleep = true;
        p->lights_off = true;
        p->bed_night = night_id(t);
        event = true;
    }
    if (p->asleep && night) p->night_sleep = true;       // put to bed at night counts too
    if (p->asleep && p->night_sleep && !night) {          // morning
        p->asleep = false;
        p->lights_off = false;
        p->night_sleep = false;
        event = true;
    }

    if (p->asleep) {
        p->hunger -= HUNGER_ASLEEP;
        if (!p->lights_off) p->happy -= HAPPY_ASLEEP_LIGHT;
        p->energy -= ENERGY_ASLEEP;
        p->hygiene -= HYGIENE_ASLEEP + HYGIENE_PER_POOP * p->poops;
        if (p->energy >= PET_STAT_MAX && !p->night_sleep) {   // nap over: wake, lights back on
            p->asleep = false;
            p->lights_off = false;
            event = true;
        }
    } else {
        p->hunger -= baby ? HUNGER_AWAKE_BABY : HUNGER_AWAKE;
        p->happy -= HAPPY_AWAKE;
        p->energy -= ENERGY_AWAKE;
        p->hygiene -= HYGIENE_AWAKE + HYGIENE_PER_POOP * p->poops;

        p->poop_timer_s += STEP_S;
        if (p->poop_timer_s >= (baby ? POOP_EVERY_BABY_S : POOP_EVERY_S)) {
            p->poop_timer_s = 0;
            if (p->poops < MAX_POOPS) p->poops++;
            event = true;
        }
        if (p->energy <= 0) {                      // collapses asleep
            p->asleep = true;
            event = true;
        }
    }

    p->hunger = clamp(p->hunger);
    p->happy = clamp(p->happy);
    p->energy = clamp(p->energy);
    p->hygiene = clamp(p->hygiene);

    track_zero(p, p->hunger, ZF_HUNGER);
    track_zero(p, p->happy, ZF_HAPPY);
    track_zero(p, p->hygiene, ZF_HYGIENE);

    // Sickness builds up under bad conditions and fades when they clear.
    bool bad = p->hygiene < VERY_LOW || p->poops >= 3 || p->hunger == 0;
    if (bad) {
        p->sick_timer_s += STEP_S;
        if (!p->sick && p->sick_timer_s >= SICK_AFTER_S) {
            p->sick = true;
            event = true;
        }
    } else if (p->sick_timer_s >= STEP_S) {
        p->sick_timer_s -= STEP_S;
    }

    int32_t dh = 0;
    if (p->sick) dh -= HEALTH_SICK;
    if (p->hunger == 0) dh -= HEALTH_STARVING;
    if (p->happy == 0) dh -= HEALTH_MISERABLE;
    if (dh == 0 && p->hunger > 3000 && p->hygiene > 3000) dh = HEALTH_RECOVER;
    p->health = clamp(p->health + dh);

    if (p->health == 0) {
        p->stage = PET_STAGE_DEAD;
        p->died_at = t;
        p->asleep = false;
        event = true;
    }
    return event;
}

bool pet_update(pet_t *p, uint32_t now)
{
    if (p->stage == PET_STAGE_DEAD || now <= p->last_update) return false;
    if (p->stage == PET_STAGE_EGG && !p->egg_started) {   // dormant: time doesn't count
        p->last_update = now;
        return false;
    }
    if (now - p->last_update > PET_MAX_CATCHUP_S) p->last_update = now - PET_MAX_CATCHUP_S;

    bool event = false;
    while (now - p->last_update >= STEP_S && p->stage != PET_STAGE_DEAD) {
        p->last_update += STEP_S;
        event |= step(p, p->last_update);
    }
    return event;
}

static bool can_act(const pet_t *p)
{
    return p->stage != PET_STAGE_EGG && p->stage != PET_STAGE_DEAD;
}

static bool awake_and_alive(const pet_t *p)
{
    return can_act(p) && !p->asleep;
}

bool pet_feed_meal(pet_t *p)
{
    if (!awake_and_alive(p)) return false;
    if (p->hunger > PET_STAT_MAX - 1000) {         // already full: overeating
        p->weight_g += 2;
        p->happy = clamp(p->happy - 500);
        return false;
    }
    p->hunger = clamp(p->hunger + 3000);
    p->weight_g += 1;
    return true;
}

bool pet_feed_snack(pet_t *p)
{
    if (!awake_and_alive(p)) return false;
    p->happy = clamp(p->happy + 1500);
    p->hunger = clamp(p->hunger + 500);
    p->weight_g += 2;
    return true;
}

bool pet_play(pet_t *p)
{
    if (!awake_and_alive(p) || p->energy < 1000) return false;
    p->happy = clamp(p->happy + 2500);
    p->energy = clamp(p->energy - 1000);
    p->hunger = clamp(p->hunger - 500);
    if (p->weight_g > 5) p->weight_g -= 1;
    return true;
}

bool pet_clean(pet_t *p)
{
    if (!can_act(p)) return false;
    p->poops = 0;
    p->hygiene = PET_STAT_MAX;
    return true;
}

bool pet_medicine(pet_t *p)
{
    if (!can_act(p) || !p->sick) return false;
    p->sick = false;
    p->sick_timer_s = 0;
    // Medicine cures, but it is not food: no health back on an empty belly.
    if (p->hunger > 0) p->health = clamp(p->health + 2000);
    p->happy = clamp(p->happy - 500);              // nobody likes medicine
    return true;
}

bool pet_toggle_lights(pet_t *p)
{
    if (!can_act(p)) return false;
    p->lights_off = !p->lights_off;
    if (p->lights_off && !p->asleep && p->energy < 8000) {
        p->asleep = true;
    } else if (!p->lights_off && p->asleep) {
        p->asleep = false;                         // woken up early, grumpy
        p->night_sleep = false;
        p->happy = clamp(p->happy - 500);
    }
    return true;
}

uint32_t pet_hatch_time(const pet_t *p)
{
    if (p->stage != PET_STAGE_EGG || !p->egg_started) return 0;
    uint32_t base = p->egg_started + PET_EGG_MIN_S;
    uint32_t wake = base - base % 86400u + s_wake_min * 60u;   // that day's wake-up
    if (wake < base) wake += 86400u;
    uint32_t warm = p->egg_warm_s < PET_EGG_WARM_MAX_S ? p->egg_warm_s : PET_EGG_WARM_MAX_S;
    return wake - warm;
}

int pet_egg_progress(const pet_t *p, uint32_t now)
{
    uint32_t hatch = pet_hatch_time(p);
    if (!hatch || now <= p->egg_started) return 0;
    if (now >= hatch) return 100;
    return (int)((uint64_t)(now - p->egg_started) * 100u / (hatch - p->egg_started));
}

bool pet_start_egg(pet_t *p, uint32_t now)
{
    if (p->stage != PET_STAGE_EGG || p->egg_started) return false;
    p->egg_started = now;
    p->last_update = now;
    return true;
}

bool pet_warm_egg(pet_t *p)
{
    if (p->stage != PET_STAGE_EGG || !p->egg_started || p->egg_warm_s >= PET_EGG_WARM_MAX_S) return false;
    p->egg_warm_s += PET_EGG_WARM_STEP_S;
    return true;
}

bool pet_wake(pet_t *p)
{
    // An alarm hatches a started egg once it has incubated the minimum time.
    if (p->stage == PET_STAGE_EGG) {
        if (!p->egg_started || p->last_update - p->egg_started < PET_EGG_MIN_S) return false;
        p->stage = PET_STAGE_BABY;
        p->hatched_at = p->last_update;
        return true;
    }
    if (!can_act(p) || !p->asleep) return false;
    p->asleep = false;
    p->lights_off = false;
    p->night_sleep = false;
    return true;
}

bool pet_pat(pet_t *p)
{
    if (!awake_and_alive(p)) return false;
    p->happy = clamp(p->happy + 200);
    return true;
}

pet_mood_t pet_mood(const pet_t *p)
{
    if (p->stage == PET_STAGE_DEAD) return PET_MOOD_DEAD;
    if (p->stage == PET_STAGE_EGG) return PET_MOOD_OK;
    if (p->asleep) return PET_MOOD_ASLEEP;
    if (p->sick) return PET_MOOD_SICK;
    if (p->hunger < LOW) return PET_MOOD_HUNGRY;
    if (p->hygiene < LOW || p->poops > 0) return PET_MOOD_DIRTY;
    if (p->energy < 2000) return PET_MOOD_TIRED;
    if (p->happy < 3000) return PET_MOOD_SAD;
    if (p->happy > 7000 && p->hunger > 6000) return PET_MOOD_HAPPY;
    return PET_MOOD_OK;
}

bool pet_needs_attention(const pet_t *p)
{
    if (!can_act(p)) return false;
    if (p->asleep) return !p->lights_off || p->sick;
    return p->hunger < LOW || p->happy < LOW || p->poops > 0 || p->sick || p->energy < VERY_LOW;
}

uint32_t pet_age_s(const pet_t *p, uint32_t now)
{
    if (!p->hatched_at) return 0;
    uint32_t end = p->stage == PET_STAGE_DEAD ? p->died_at : now;
    return end > p->hatched_at ? end - p->hatched_at : 0;
}

const char *pet_stage_name(pet_stage_t s)
{
    static const char *n[] = {"Egg", "Baby", "Child", "Teen", "Adult", "Ghost"};
    return s <= PET_STAGE_DEAD ? n[s] : "?";
}

const char *pet_mood_name(pet_mood_t m)
{
    static const char *n[] = {"Happy", "OK", "Hungry", "Sad", "Tired",
                              "Dirty", "Sick", "Asleep", "Gone"};
    return m <= PET_MOOD_DEAD ? n[m] : "?";
}

// Host unit tests for components/pet. Build + run: firmware/test/pet/run.sh
#include <stdio.h>
#include <stdlib.h>

#include "pet.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define T0 1760000000u      // arbitrary fixed "now"
#define MIN 60u
#define HOUR 3600u

static pet_t hatched(void)
{
    // A baby that hatched at T0 (test shortcut past the egg stage).
    pet_t p;
    pet_new_egg(&p, T0, 1);
    p.stage = PET_STAGE_BABY;
    p.hatched_at = T0;
    p.last_update = T0;
    return p;
}

#define DAY 86400u
static uint32_t at(uint32_t day_start, unsigned hh, unsigned mm) { return day_start + hh * HOUR + mm * MIN; }
static uint32_t midnight(uint32_t t) { return t - t % DAY; }

static void test_dormant_egg_waits_for_start(void)
{
    pet_t p;
    pet_new_egg(&p, T0, 1);
    pet_update(&p, T0 + 3 * DAY);
    CHECK(p.stage == PET_STAGE_EGG);           // never started: still an egg
    CHECK(pet_hatch_time(&p) == 0);
    CHECK(!pet_feed_meal(&p));
    CHECK(!pet_warm_egg(&p));                  // can't warm a dormant egg
}

static void test_egg_hatches_next_morning(void)
{
    uint32_t d = midnight(T0);
    pet_t p;
    pet_new_egg(&p, at(d, 9, 0), 1);
    CHECK(pet_start_egg(&p, at(d, 9, 0)));     // started in the morning
    CHECK(!pet_start_egg(&p, at(d, 9, 5)));    // only once
    CHECK(pet_hatch_time(&p) == at(d + DAY, 7, 0));
    pet_update(&p, at(d, 23, 0));
    CHECK(p.stage == PET_STAGE_EGG);
    CHECK(pet_egg_progress(&p, at(d, 23, 0)) > 50);
    pet_update(&p, at(d + DAY, 7, 1));
    CHECK(p.stage == PET_STAGE_BABY);          // hatched at wake-up

    pet_t q;                                   // started at bedtime: hatches that morning
    pet_new_egg(&q, at(d, 23, 0), 1);
    pet_start_egg(&q, at(d, 23, 0));
    CHECK(pet_hatch_time(&q) == at(d + DAY, 7, 0));
}

static void test_warming_shaves_time_capped(void)
{
    uint32_t d = midnight(T0);
    pet_t p;
    pet_new_egg(&p, at(d, 9, 0), 1);
    pet_start_egg(&p, at(d, 9, 0));
    for (int i = 0; i < 10; i++) CHECK(pet_warm_egg(&p));
    CHECK(pet_hatch_time(&p) == at(d + DAY, 7, 0) - 20 * MIN);   // 10 taps = 20 min
    for (int i = 0; i < 500; i++) pet_warm_egg(&p);
    CHECK(pet_hatch_time(&p) == at(d + DAY, 4, 0));              // capped at 3 h
    CHECK(!pet_warm_egg(&p));
}

static void test_alarm_hatches_ready_egg(void)
{
    uint32_t d = midnight(T0);
    pet_t p;
    pet_new_egg(&p, at(d, 23, 0), 1);
    pet_start_egg(&p, at(d, 23, 0));
    pet_update(&p, at(d + DAY, 2, 0));
    CHECK(!pet_wake(&p));                      // only 3 h in: too early
    pet_update(&p, at(d + DAY, 6, 0));
    CHECK(pet_wake(&p));                       // 6 am alarm, 7 h in: hatches
    CHECK(p.stage == PET_STAGE_BABY);
}

static void test_medicine_is_not_food(void)
{
    pet_t p = hatched();
    uint32_t t = p.last_update;
    while (p.stage != PET_STAGE_DEAD && t < T0 + 10 * DAY) {
        t += 10 * MIN;
        pet_update(&p, t);
        pet_clean(&p);
        if (p.sick) pet_medicine(&p);          // dosed every time, never fed
    }
    CHECK(p.stage == PET_STAGE_DEAD);
    CHECK(p.died_at - p.hatched_at < 48 * HOUR);
}

static void test_stats_decay_and_feeding_restores(void)
{
    pet_t p = hatched();
    int32_t h0 = p.hunger;
    pet_update(&p, p.last_update + HOUR);
    CHECK(p.hunger < h0);
    int32_t before = p.hunger;
    CHECK(pet_feed_meal(&p));
    CHECK(p.hunger > before);
}

static void test_overfeeding_refused_and_adds_weight(void)
{
    pet_t p = hatched();
    uint32_t w = p.weight_g;
    CHECK(!pet_feed_meal(&p));                 // already full
    CHECK(p.weight_g > w);
}

static void test_catch_up_after_12h_off(void)
{
    // One 12 h jump must equal 720 one-minute updates.
    pet_t a = hatched(), b = hatched();
    pet_update(&a, a.last_update + 12 * HOUR);
    for (uint32_t i = 1; i <= 12 * 60; i++) pet_update(&b, b.last_update + MIN);
    CHECK(a.hunger == b.hunger);
    CHECK(a.happy == b.happy);
    CHECK(a.health == b.health);
    CHECK(a.poops == b.poops);
    CHECK(a.stage == b.stage);
    CHECK(a.stage == PET_STAGE_CHILD);         // past 1 h -> child
    CHECK(a.poops > 0);
}

static void test_partial_minutes_are_kept(void)
{
    pet_t p = hatched();
    uint32_t start = p.last_update;
    pet_update(&p, start + 90);                // 1.5 min
    CHECK(p.last_update == start + 60);        // the extra 30 s is not lost
    pet_update(&p, start + 120);
    CHECK(p.last_update == start + 120);
}

static void test_neglect_kills_within_two_days(void)
{
    pet_t p = hatched();
    uint32_t t = p.last_update;
    while (p.stage != PET_STAGE_DEAD && t < T0 + 7 * 24 * HOUR) {
        t += HOUR;
        pet_update(&p, t);
    }
    CHECK(p.stage == PET_STAGE_DEAD);
    CHECK(p.died_at - p.hatched_at < 48 * HOUR);
    CHECK(p.died_at - p.hatched_at > 12 * HOUR);   // but not instantly
    CHECK(p.care_mistakes >= 2);
    CHECK(pet_mood(&p) == PET_MOOD_DEAD);
    CHECK(!pet_feed_meal(&p));
    CHECK(!pet_update(&p, t + HOUR));          // dead pets don't change
}

static void test_good_care_survives_a_week_and_becomes_star(void)
{
    pet_t p = hatched();
    uint32_t t = p.last_update;
    for (int h = 0; h < 7 * 24; h++) {
        t += HOUR;
        pet_update(&p, t);
        if (p.asleep) continue;
        if (p.hunger < 6000) pet_feed_meal(&p);
        if (p.happy < 6000) pet_play(&p);
        if (p.poops || p.hygiene < 5000) pet_clean(&p);
        if (p.sick) pet_medicine(&p);
        if (p.energy < 3000) pet_toggle_lights(&p);   // put to bed
    }
    CHECK(p.stage == PET_STAGE_ADULT);
    CHECK(p.form == PET_FORM_STAR);
    CHECK(p.health > 5000);
}

static void test_dirty_pet_gets_sick_and_medicine_cures(void)
{
    pet_t p = hatched();
    uint32_t t = p.last_update;
    while (!p.sick && t < T0 + 24 * HOUR) {
        t += 10 * MIN;
        pet_update(&p, t);
        if (p.hunger < 6000) pet_feed_meal(&p);   // fed, but never cleaned
        if (p.asleep) pet_toggle_lights(&p);
        pet_play(&p);
    }
    CHECK(p.sick);
    pet_wake(&p);                              // may have got sick after bedtime
    CHECK(pet_mood(&p) == PET_MOOD_SICK);
    CHECK(pet_needs_attention(&p));
    CHECK(pet_medicine(&p));
    CHECK(!p.sick);
    CHECK(!pet_medicine(&p));                  // nothing to cure now
}

static void test_lights_put_to_bed_and_sleep_restores_energy(void)
{
    pet_t p = hatched();
    p.energy = 2000;
    CHECK(pet_toggle_lights(&p));
    CHECK(p.asleep && p.lights_off);
    CHECK(!pet_needs_attention(&p) || p.sick);
    CHECK(!pet_play(&p));                      // can't play while asleep
    pet_update(&p, p.last_update + 8 * HOUR);
    CHECK(!p.asleep);                          // woke up rested
    CHECK(!p.lights_off);
    CHECK(p.energy > 8000);                    // full on waking (~4.5 h), then awake decay
}

static void test_huge_gap_is_capped(void)
{
    pet_t p = hatched();
    pet_update(&p, p.last_update + 400u * 24u * HOUR);   // a year+ unplugged
    CHECK(p.stage == PET_STAGE_DEAD);                    // no infinite loop
}

static void test_new_egg_after_death(void)
{
    pet_t p = hatched();
    pet_update(&p, p.last_update + 7 * 24 * HOUR);
    CHECK(p.stage == PET_STAGE_DEAD);
    uint32_t gen = p.generation;
    pet_new_egg(&p, T0 + 8 * 24 * HOUR, gen + 1);
    CHECK(p.stage == PET_STAGE_EGG);
    CHECK(p.generation == gen + 1);
    CHECK(p.health == PET_STAT_MAX);
}



static void test_bedtime_and_morning(void)
{
    pet_t p = hatched();
    uint32_t d = midnight(T0);
    p.energy = PET_STAT_MAX;
    pet_update(&p, at(d, 19, 59));
    CHECK(!p.asleep);
    pet_update(&p, at(d, 20, 1));
    CHECK(p.asleep && p.lights_off);               // 8 pm: bed, lights out
    pet_update(&p, at(d + DAY, 3, 0));
    CHECK(p.energy == PET_STAT_MAX);
    CHECK(p.asleep);                               // rested but it's still night
    pet_update(&p, at(d + DAY, 6, 59));
    CHECK(p.asleep);
    pet_update(&p, at(d + DAY, 7, 1));
    CHECK(!p.asleep && !p.lights_off);             // 7 am: up
}

// Keeps a pet fed and clean so a test can focus on the schedule.
static void top_up(pet_t *p)
{
    p->hunger = p->happy = p->hygiene = p->health = PET_STAT_MAX;
    p->poops = 0;
    p->sick = false;
}

static void test_woken_at_night_stays_up(void)
{
    pet_t p = hatched();
    uint32_t d = midnight(T0);
    top_up(&p); pet_update(&p, at(d, 20, 5));
    CHECK(p.asleep);
    CHECK(pet_toggle_lights(&p));                  // "Wake" from the tray
    CHECK(!p.asleep);
    top_up(&p); pet_update(&p, at(d, 22, 0));
    CHECK(!p.asleep);                              // bedtime doesn't re-trigger tonight
    CHECK(pet_toggle_lights(&p));                  // put back to bed at 22:00
    CHECK(p.asleep);
    top_up(&p); pet_update(&p, at(d + DAY, 4, 0));
    CHECK(p.asleep);                               // sleeps through, even rested
    top_up(&p); pet_update(&p, at(d + DAY, 7, 1));   // steps are whole minutes from hatch time
    CHECK(!p.asleep);
    top_up(&p); pet_update(&p, at(d + DAY, 12, 0));
    top_up(&p); pet_update(&p, at(d + DAY, 20, 1));
    CHECK(p.stage != PET_STAGE_DEAD);
    CHECK(p.asleep);                               // next night's bedtime works again
}

static void test_alarm_wakes_gently(void)
{
    pet_t p = hatched();
    uint32_t d = midnight(T0);
    pet_update(&p, at(d + DAY, 6, 0));
    CHECK(p.asleep);
    int32_t happy = p.happy;
    CHECK(pet_wake(&p));                           // alarm at 6:00
    CHECK(!p.asleep && !p.lights_off);
    CHECK(p.happy == happy);                       // no grumpiness for an alarm
    CHECK(!pet_wake(&p));                          // already awake
    pet_update(&p, at(d + DAY, 6, 30));
    CHECK(!p.asleep);
}

static void test_day_nap_ends_when_rested(void)
{
    pet_t p = hatched();
    uint32_t d = midnight(T0);
    pet_update(&p, at(d, 12, 0));
    p.energy = 4000;
    CHECK(pet_toggle_lights(&p));
    CHECK(p.asleep && !p.night_sleep);
    pet_update(&p, at(d, 16, 0));
    CHECK(!p.asleep);                              // rested within ~3.5 h
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"dormant egg waits for Start", test_dormant_egg_waits_for_start},
        {"egg hatches at next wake-up", test_egg_hatches_next_morning},
        {"warming shaves time, capped at 3 h", test_warming_shaves_time_capped},
        {"alarm hatches a ready egg", test_alarm_hatches_ready_egg},
        {"medicine can't replace food", test_medicine_is_not_food},
        {"stats decay, feeding restores", test_stats_decay_and_feeding_restores},
        {"overfeeding refused, adds weight", test_overfeeding_refused_and_adds_weight},
        {"12 h power-off catch-up == per-minute", test_catch_up_after_12h_off},
        {"partial minutes kept", test_partial_minutes_are_kept},
        {"neglect kills in 12-48 h", test_neglect_kills_within_two_days},
        {"good care -> adult STAR after a week", test_good_care_survives_a_week_and_becomes_star},
        {"dirty -> sick, medicine cures", test_dirty_pet_gets_sick_and_medicine_cures},
        {"lights -> sleep -> wakes rested", test_lights_put_to_bed_and_sleep_restores_energy},
        {"huge gap capped", test_huge_gap_is_capped},
        {"new egg after death", test_new_egg_after_death},
        {"bed at 8 pm, sleeps through, up at 7 am", test_bedtime_and_morning},
        {"woken at night stays up, next bedtime works", test_woken_at_night_stays_up},
        {"alarm wakes gently", test_alarm_wakes_gently},
        {"daytime nap ends when rested", test_day_nap_ends_when_rested},
    };
    int n = sizeof tests / sizeof tests[0], passed = 0;
    for (int i = 0; i < n; i++) {
        int before = failures;
        tests[i].fn();
        passed += failures == before;
        printf("%s %s\n", failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d/%d tests passed\n", passed, n);
    return failures ? 1 : 0;
}

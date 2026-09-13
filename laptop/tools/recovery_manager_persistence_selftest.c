/*
 * Standalone self-test for recovery_manager_persistence_streak_step()
 * (core/recovery_manager.c) - no hardware required, same pattern as every
 * other selftest in this project's laptop/tools/ directory.
 *
 * This is the hard-consecutive-tick persistence mechanism for evaluate_
 * fault_predictor()'s "sustained AI persistence" corroboration path (v4
 * architecture doc Section 3). A cumulative up/down counter (decay a
 * small amount on a bad tick instead of resetting to 0) was designed as
 * an alternative and REJECTED after measurement - see core/recovery_
 * manager_config.h's FAULT_PERSISTENCE_FLOOR/_TRIP_TICKS doc for the
 * full false-reject/false-accept overlap analysis that rejected it (no
 * DOWN_STEP choice improved separation over the hard streak on the same
 * dataset/methodology). This selftest exercises the hard-streak mechanism
 * that was actually kept, confirming the property the overlap analysis
 * measured in aggregate - noise rejection vs. sustained-signal detection
 * - also holds on concrete synthetic sequences, not just offline stats.
 *
 * Build (same whole-core-directory pattern as this project's `tron`
 * target in the Makefile - recovery_manager.c's translation unit pulls in
 * enough of the rest of the simulator - cbba.c, comms_transport.c,
 * snapshot.c, etc. - that linking it alone against a hand-picked subset
 * of core's .c files is impractical; simplest correct option is all of
 * them):
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic \
 *       -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -Icore -Ilaptop \
 *       -o recovery_manager_persistence_selftest \
 *       laptop/tools/recovery_manager_persistence_selftest.c \
 *       $(find core -maxdepth 1 -name "*.c") laptop/debug.c laptop/snapshot.c -lm
 * Run:
 *   ./recovery_manager_persistence_selftest
 *
 * (recovery_manager_persistence_streak_step() itself is a pure function
 * with no dependency on any of the above beyond FAULT_PERSISTENCE_FLOOR -
 * the wide build line above is only to get one valid translation unit
 * containing recovery_manager.c to link at all. Nothing below touches
 * per-team state or fault_predictor_run().)
 */

#include "recovery_manager.h"
#include "recovery_manager_config.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/* Runs one sequence of confidence values through the streak step,
   starting from streak=0, and returns the FINAL streak value. */
static uint8_t run_sequence(const float *confidences, int n)
{
    uint8_t streak = 0u;
    for (int i = 0; i < n; i++)
        streak = recovery_manager_persistence_streak_step(streak, confidences[i]);
    return streak;
}

/* Runs the sequence but returns the number of ticks (1-based) at which
   the streak FIRST reaches trip_ticks, or -1 if it never does within n
   ticks - i.e. the same "ticks-to-trip" quantity the offline overlap
   analysis measured, applied to one concrete sequence. */
static int ticks_to_trip(const float *confidences, int n, uint8_t trip_ticks)
{
    uint8_t streak = 0u;
    for (int i = 0; i < n; i++)
    {
        streak = recovery_manager_persistence_streak_step(streak, confidences[i]);
        if (streak >= trip_ticks)
            return i + 1;
    }
    return -1;
}

/* ---------------------------------------------------------------
 * 1. Sustained-high: confidence stays >= FAULT_PERSISTENCE_FLOOR for
 *    longer than FAULT_PERSISTENCE_TRIP_TICKS straight through. Must
 *    trip - this is the core "real, sustained fault" case the mechanism
 *    exists to detect.
 * --------------------------------------------------------------- */
static void test_sustained_high_trips(void)
{
    float seq[20];
    for (int i = 0; i < 20; i++)
        seq[i] = 0.60f;  /* comfortably above FAULT_PERSISTENCE_FLOOR (0.20) */

    int trip_tick = ticks_to_trip(seq, 20, FAULT_PERSISTENCE_TRIP_TICKS);

    CHECK(trip_tick == (int)FAULT_PERSISTENCE_TRIP_TICKS);
    CHECK(run_sequence(seq, 20) == 20u);  /* keeps climbing past the trip point */
}

/* ---------------------------------------------------------------
 * 2. Single-spike-then-normal: one tick at high confidence, then back to
 *    0 (or below floor) forever. Must NEVER trip, regardless of how high
 *    the single spike was - this is exactly the noise case the mechanism
 *    exists to reject (a single-tick blip is not a sustained fault).
 * --------------------------------------------------------------- */
static void test_single_spike_then_normal_never_trips(void)
{
    float seq[30];
    seq[0] = 1.0f;  /* max-confidence single spike */
    for (int i = 1; i < 30; i++)
        seq[i] = 0.0f;

    CHECK(ticks_to_trip(seq, 30, FAULT_PERSISTENCE_TRIP_TICKS) == -1);
    CHECK(run_sequence(seq, 30) == 0u);

    /* Also confirm the streak resets to exactly 0 the tick immediately
       after the spike, not decayed toward 0 - the defining behavioral
       difference from the rejected cumulative up/down counter. */
    uint8_t streak = recovery_manager_persistence_streak_step(0u, seq[0]);
    CHECK(streak == 1u);
    streak = recovery_manager_persistence_streak_step(streak, seq[1]);
    CHECK(streak == 0u);
}

/* ---------------------------------------------------------------
 * 3. Noisy-but-trending-high: confidence oscillates above/below floor
 *    with never more than (FAULT_PERSISTENCE_TRIP_TICKS - 1) consecutive
 *    above-floor ticks before a dip - even though the AVERAGE trend is
 *    "mostly elevated", it must NOT trip, because no single run ever
 *    reaches the required consecutive length. This is the mechanism's
 *    known, accepted cost (the false-reject side measured in the offline
 *    analysis: a genuinely-sustained-but-flickery real fault can miss
 *    the trip too) - the test exists to make that behavior explicit and
 *    intentional, not a bug to "fix" later without re-reading this file.
 * --------------------------------------------------------------- */
static void test_noisy_trending_high_does_not_trip(void)
{
    /* Pattern: (TRIP_TICKS - 1) high ticks, 1 low tick, repeated - each
       run is one tick short of tripping, by construction. */
    float seq[60];
    int idx = 0;
    while (idx < 60)
    {
        for (uint8_t k = 0; k < FAULT_PERSISTENCE_TRIP_TICKS - 1u && idx < 60; k++)
            seq[idx++] = 0.70f;
        if (idx < 60)
            seq[idx++] = 0.05f;  /* below FAULT_PERSISTENCE_FLOOR */
    }

    CHECK(ticks_to_trip(seq, 60, FAULT_PERSISTENCE_TRIP_TICKS) == -1);

    /* Sanity: confirm the max streak actually reached is exactly
       TRIP_TICKS - 1, i.e. this sequence was a genuine near-miss, not
       accidentally too weak a test. */
    uint8_t streak = 0u;
    uint8_t max_streak = 0u;
    for (int i = 0; i < 60; i++)
    {
        streak = recovery_manager_persistence_streak_step(streak, seq[i]);
        if (streak > max_streak)
            max_streak = streak;
    }
    CHECK(max_streak == FAULT_PERSISTENCE_TRIP_TICKS - 1u);
}

/* ---------------------------------------------------------------
 * 4. Boundary: confidence exactly AT the floor counts as "above" (>=,
 *    not >) - and one tick below floor after a long climb still fully
 *    resets, even at streak = TRIP_TICKS - 1 (one tick away from
 *    tripping). Confirms there is no partial credit / grace tick.
 * --------------------------------------------------------------- */
static void test_floor_boundary_and_reset_at_the_edge(void)
{
    CHECK(recovery_manager_persistence_streak_step(0u, FAULT_PERSISTENCE_FLOOR) == 1u);

    float just_below = FAULT_PERSISTENCE_FLOOR - 0.001f;
    CHECK(recovery_manager_persistence_streak_step(5u, just_below) == 0u);

    /* One tick short of tripping, then a single bad tick: must go all
       the way back to 0, not to TRIP_TICKS - 2 or any partial value. */
    uint8_t streak = FAULT_PERSISTENCE_TRIP_TICKS - 1u;
    streak = recovery_manager_persistence_streak_step(streak, 0.0f);
    CHECK(streak == 0u);
}

/* ---------------------------------------------------------------
 * 5. Saturation: streak does not wrap/overflow past UINT8_MAX under a
 *    very long sustained-high run.
 * --------------------------------------------------------------- */
static void test_saturates_without_overflow(void)
{
    uint8_t streak = 255u;
    streak = recovery_manager_persistence_streak_step(streak, 0.90f);
    CHECK(streak == 255u);
}

/* ---------------------------------------------------------------
 * 6. Bounded-forgiveness variant (recovery_manager_persistence_streak_
 *    step_bounded_forgiveness() - EXPERIMENTAL, NOT the production
 *    mechanism, see that function's own doc for the full measured
 *    verdict against real simulator data, laptop/tools/persistence_
 *    bounded_forgiveness_ablation.py). Confirms the two behaviors that
 *    define it, on concrete sequences:
 *      (a) it closes the dip-every-Nth-tick pattern the hard-reset
 *          mechanism (tests 1-5 above) is vulnerable to;
 *      (b) it still hard-resets on two CONSECUTIVE misses, so it is
 *          not an unlimited-forgiveness mechanism either.
 * --------------------------------------------------------------- */
static uint8_t run_sequence_bounded_forgiveness(const float *confidences, int n)
{
    uint8_t streak = 0u;
    uint8_t was_last_miss = 0u;
    for (int i = 0; i < n; i++)
        streak = recovery_manager_persistence_streak_step_bounded_forgiveness(
            streak, confidences[i], &was_last_miss);
    return streak;
}

static void test_bounded_forgiveness_closes_dip_every_nth_tick(void)
{
    /* Same dip-every-7th-tick pattern confirmed exploitable against the
       hard-reset mechanism earlier this session (recovery_predictor_
       stress_test.c's scenario 2: max streak reached there was capped
       well below FAULT_PERSISTENCE_TRIP_TICKS, forever). The bounded-
       forgiveness variant must NOT be defeated by it - one isolated
       miss every 7 ticks is never two consecutive misses. */
    float seq[80];
    for (int i = 0; i < 80; i++)
        seq[i] = ((i + 1) % 7 == 0) ? 0.0f : 0.85f;

    uint8_t streak = 0u;
    uint8_t was_last_miss = 0u;
    uint8_t max_streak = 0u;
    int trip_tick = -1;
    for (int i = 0; i < 80; i++)
    {
        streak = recovery_manager_persistence_streak_step_bounded_forgiveness(
            streak, seq[i], &was_last_miss);
        if (streak > max_streak)
            max_streak = streak;
        if (trip_tick < 0 && streak >= FAULT_PERSISTENCE_TRIP_TICKS)
            trip_tick = i + 1;
    }

    CHECK(trip_tick > 0); /* DOES trip, unlike the hard-reset mechanism on this exact input */
    CHECK(max_streak >= FAULT_PERSISTENCE_TRIP_TICKS);
    CHECK(run_sequence_bounded_forgiveness(seq, 80) > 0u);
}

static void test_bounded_forgiveness_still_resets_on_two_consecutive_misses(void)
{
    float seq[12] = {0.85f, 0.85f, 0.85f, 0.85f, 0.85f, 0.0f, 0.0f, /* two misses in a row */
                      0.85f, 0.85f, 0.85f, 0.85f, 0.85f};

    uint8_t streak = 0u;
    uint8_t was_last_miss = 0u;
    for (int i = 0; i < 7; i++) /* through the two consecutive misses */
        streak = recovery_manager_persistence_streak_step_bounded_forgiveness(
            streak, seq[i], &was_last_miss);

    CHECK(streak == 0u); /* fully reset - two consecutive misses are NOT forgiven */

    /* One isolated miss (not two in a row) must NOT reset. */
    uint8_t s2 = 3u;
    uint8_t wlm2 = 0u;
    s2 = recovery_manager_persistence_streak_step_bounded_forgiveness(s2, 0.0f, &wlm2);
    CHECK(s2 == 3u); /* unchanged, not reset */
    s2 = recovery_manager_persistence_streak_step_bounded_forgiveness(s2, 0.85f, &wlm2);
    CHECK(s2 == 4u); /* resumes climbing normally right after the forgiven miss */
}

int main(void)
{
    test_sustained_high_trips();
    test_single_spike_then_normal_never_trips();
    test_noisy_trending_high_does_not_trip();
    test_floor_boundary_and_reset_at_the_edge();
    test_saturates_without_overflow();
    test_bounded_forgiveness_closes_dip_every_nth_tick();
    test_bounded_forgiveness_still_resets_on_two_consecutive_misses();

    if (g_failures == 0)
    {
        printf("All recovery_manager_persistence_streak_step() checks passed "
               "(FAULT_PERSISTENCE_FLOOR=%.2f, FAULT_PERSISTENCE_TRIP_TICKS=%u).\n",
               FAULT_PERSISTENCE_FLOOR, (unsigned)FAULT_PERSISTENCE_TRIP_TICKS);
        return 0;
    }

    printf("%d check(s) FAILED.\n", g_failures);
    return 1;
}

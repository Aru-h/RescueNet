/*
 * Standalone self-test for core/fault_predictor_features.c (no
 * hardware/board, no real simulation run - desktop gcc only, same
 * convention as laptop/tools/mission_link_selftest.c and
 * laptop/tools/lora_frame_selftest.c).
 *
 * This does NOT run the simulator. It writes directly into
 * g_teams[0], g_incidents[0], and g_mission_execution_twin[0] by
 * hand, tick by tick, then checks fault_predictor_stall_frac(),
 * fault_predictor_delta_priority(), and
 * fault_predictor_delta_victim_count() against values hand-calculated
 * for this exact synthetic sequence (the same rolling-window /
 * groupby-first logic data_prep.py uses, worked out by hand here).
 * The intent is to prove the C ring buffer and baseline memory are
 * correct BEFORE core/simulation.c's tick loop or
 * core/recovery_manager.c ever touch this code.
 *
 * Sequence (team 0, 15 ticks):
 *   ticks 1-10  : unassigned, parked at (0,0), remaining_path_cost=0
 *                 -> idle-at-base guard fires every tick, so every
 *                 tick counts as "moved" (not stalled) despite the
 *                 position never changing. delta_priority/
 *                 delta_victim_count must read 0 throughout (no
 *                 incident assigned).
 *   tick  11    : assigned to incident slot 0 (priority=HIGH(2),
 *                 victim_count=3) -> baseline captured this tick.
 *                 Team is now "traveling" (remaining_path_cost=5,
 *                 so the idle guard no longer applies) but does NOT
 *                 move this tick -> stalled.
 *   ticks 12-13 : still assigned, still stalled (position unchanged),
 *                 incident fields unchanged.
 *   tick  14    : still stalled, but a mission fault lands on the
 *                 incident: priority nudges to LOW(4), victim_count
 *                 rises to 6. delta_priority/delta_victim_count
 *                 should reflect this against the tick-11 baseline,
 *                 not the pre-fault-14 value.
 *   tick  15    : team moves again (position changes). Incident
 *                 fields unchanged from tick 14 (baseline never
 *                 re-captures once set).
 *
 * Because FAULT_PREDICTOR_STALL_WINDOW_LONG is 10, ticks 1-10 exactly
 * fill the ring buffer, and tick 11 onward starts overwriting the
 * oldest entries (tick 1, then tick 2, ...) - this sequence is
 * deliberately long enough to exercise that wraparound, not just the
 * min_periods=1 early-tick behavior.
 *
 * Build (from a folder containing exactly these 7 files - verified
 * this is sufficient, nothing else is needed):
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -o fault_predictor_features_selftest \
 *       fault_predictor_features_selftest.c fault_predictor_features.c mission_state.c
 * Run:
 *   ./fault_predictor_features_selftest
 *
 * (mission_state.c is required to link g_teams[]/g_incidents[]/
 * g_mission_execution_twin[] - same rationale as mission_link_
 * selftest.c's note on needing mission_state.c: those arrays are
 * only `extern`-declared in mission_state.h, mission_state.c is
 * where the actual storage lives.)
 */

#include "fault_predictor_features.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define FLOAT_TOLERANCE 1e-4f

static int g_failures = 0;
static int g_checks = 0;

static void check_float(
    const char *tick_desc,
    const char *what,
    float expected,
    float actual)
{
    g_checks++;
    if(fabsf(expected - actual) > FLOAT_TOLERANCE)
    {
        g_failures++;
        fprintf(stderr,
            "[FAIL] %-10s %-22s expected=%.6f actual=%.6f (diff=%.6f)\n",
            tick_desc, what, (double)expected, (double)actual,
            (double)fabsf(expected - actual));
    }
}

/* Convenience: run one tick's checks against team 0. */
static void check_tick(
    const char *tick_desc,
    float expected_frac5,
    float expected_frac10,
    float expected_delta_priority,
    float expected_delta_victim)
{
    check_float(tick_desc, "stall_frac_last_5",
        expected_frac5,
        fault_predictor_stall_frac(0, FAULT_PREDICTOR_STALL_WINDOW_SHORT));
    check_float(tick_desc, "stall_frac_last_10",
        expected_frac10,
        fault_predictor_stall_frac(0, FAULT_PREDICTOR_STALL_WINDOW_LONG));
    check_float(tick_desc, "delta_priority",
        expected_delta_priority,
        fault_predictor_delta_priority(0));
    check_float(tick_desc, "delta_victim_count",
        expected_delta_victim,
        fault_predictor_delta_victim_count(0));
}

int main(void)
{
    fault_predictor_features_init();

    /* Team 0, starting state: unassigned, parked at the origin,
     * nothing left to travel. */
    g_teams[0].team_id = 0;
    g_teams[0].loc_x = 0;
    g_teams[0].loc_y = 0;
    g_teams[0].assigned_task_pool = TASK_POOL_NONE;
    g_teams[0].assigned_task_slot = NO_ASSIGNMENT;
    g_mission_execution_twin[0].remaining_path_cost = 0;

    /* --- Ticks 1-10: unassigned, idle-at-base guard fires every
     * tick, so every tick counts as "moved" regardless of position
     * never changing. min_periods=1 behavior: the fraction's
     * denominator grows with history_count until it caps at the
     * window size. */
    {
        int tick;
        for(tick = 1; tick <= 10; tick++)
        {
            char desc[16];
            float expected_n5;
            float expected_n10;

            fault_predictor_features_update_tick(0);

            /* n = min(tick, window); 0 stalled ticks throughout this
             * phase, so both fractions are always 0.0 regardless of
             * n - but n still needs to be nonzero for the function to
             * return a defined 0.0 rather than an early-out. */
            expected_n5 = 0.0f;
            expected_n10 = 0.0f;
            snprintf(desc, sizeof(desc), "tick%d", tick);
            check_tick(desc, expected_n5, expected_n10, 0.0f, 0.0f);
        }
    }

    /* --- Tick 11: assign team 0 to incident slot 0. Baseline gets
     * captured THIS tick, so delta_priority/delta_victim_count must
     * still read 0 immediately after. Team is now "traveling"
     * (remaining_path_cost=5) but doesn't move this tick -> stalled,
     * and the idle guard no longer applies since an incident is now
     * assigned. */
    g_incidents[0].incident_id = 1;
    g_incidents[0].slot_occupied = 1;
    g_incidents[0].priority = PRIORITY_HIGH;      /* raw value 2 */
    g_incidents[0].victim_count = 3;
    g_teams[0].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[0].assigned_task_slot = 0;
    g_mission_execution_twin[0].remaining_path_cost = 5;
    /* loc_x/loc_y deliberately left unchanged -> stalled this tick */
    fault_predictor_features_update_tick(0);
    /* Ring buffer: ticks 2-11 are the last 10 recorded (tick 1
     * overwritten). 9 moved (ticks 2-10) + 1 stalled (tick 11) = 1/10
     * stalled. Last 5 = ticks 7-11 = [moved,moved,moved,moved,
     * stalled] = 1/5. */
    check_tick("tick11", 0.2f, 0.1f, 0.0f, 0.0f);

    /* --- Ticks 12-13: still stalled, incident fields unchanged. */
    fault_predictor_features_update_tick(0);
    /* last 10 = ticks 3-12: 8 moved + 2 stalled = 2/10.
     * last 5 = ticks 8-12: [moved,moved,moved,stalled,stalled]=2/5. */
    check_tick("tick12", 0.4f, 0.2f, 0.0f, 0.0f);

    fault_predictor_features_update_tick(0);
    /* last 10 = ticks 4-13: 7 moved + 3 stalled = 3/10.
     * last 5 = ticks 9-13: [moved,moved,stalled,stalled,stalled]=3/5. */
    check_tick("tick13", 0.6f, 0.3f, 0.0f, 0.0f);

    /* --- Tick 14: a mission fault lands on the incident mid-run.
     * Baseline was captured at tick 11 (priority=2, victim=3) and
     * must NOT move - only the current-value side of the delta
     * changes. */
    g_incidents[0].priority = PRIORITY_LOW;   /* raw value 4 */
    g_incidents[0].victim_count = 6;
    fault_predictor_features_update_tick(0);
    /* last 10 = ticks 5-14: 6 moved + 4 stalled = 4/10.
     * last 5 = ticks 10-14: [moved,stalled,stalled,stalled,stalled]=4/5.
     * delta_priority = 4 - 2 = 2. delta_victim_count = 6 - 3 = 3. */
    check_tick("tick14", 0.8f, 0.4f, 2.0f, 3.0f);

    /* --- Tick 15: team moves again. Incident fields unchanged from
     * tick 14 - baseline never re-captures once set, so deltas stay
     * exactly where they were. */
    g_teams[0].loc_x = 1; /* actual position change this tick */
    fault_predictor_features_update_tick(0);
    /* last 10 = ticks 6-15: 5 moved + 4 stalled + 1 moved = 4/10.
     * last 5 = ticks 11-15: [stalled,stalled,stalled,stalled,moved]=4/5.
     * deltas unchanged: 2.0 / 3.0. */
    check_tick("tick15", 0.8f, 0.4f, 2.0f, 3.0f);

    /* --- Regression: slot recycling for a brand-new incident must
     * NOT keep using the old incident's baseline. Resolve incident 1
     * (slot 0) and immediately hand the same slot to a new incident
     * (id=2, priority=CRITICAL(1), victim_count=1) via a fresh
     * assignment on the very next tick. The delta must read 0/0 the
     * instant the new incident is first observed - if it instead
     * showed 1-4=-3 / 1-6=-5 (against incident 1's stale baseline),
     * the recycling bug is back. */
    g_incidents[0].incident_id = 2;
    g_incidents[0].priority = PRIORITY_CRITICAL; /* raw value 1 */
    g_incidents[0].victim_count = 1;
    /* team 0 stays assigned to slot 0 throughout - same slot, new
     * incident occupying it, exactly the recycling scenario. */
    fault_predictor_features_update_tick(0);
    check_float("tick16", "delta_priority", 0.0f, fault_predictor_delta_priority(0));
    check_float("tick16", "delta_victim_count", 0.0f, fault_predictor_delta_victim_count(0));

    /* One more tick with a real change against the NEW baseline,
     * proving it's actually the new baseline being used, not that the
     * old one just happened to also read 0 by coincidence. */
    g_incidents[0].priority = PRIORITY_HIGH;   /* raw value 2 */
    g_incidents[0].victim_count = 4;
    fault_predictor_features_update_tick(0);
    check_float("tick17", "delta_priority", 1.0f, fault_predictor_delta_priority(0));   /* 2-1 */
    check_float("tick17", "delta_victim_count", 3.0f, fault_predictor_delta_victim_count(0)); /* 4-1 */

    /* --- Regression: a team's very first observed tick, with an
     * incident ALREADY assigned and nonzero remaining_path_cost (so
     * the idle-at-base guard does NOT fire), must be raw-stalled to
     * match data_prep.py's actual pandas semantics (see the .c file's
     * comment on diff().sum(axis=1) for a group's first row) - not
     * raw-moved. Use team 1, untouched until now. */
    g_teams[1].team_id = 1;
    g_teams[1].loc_x = 10;
    g_teams[1].loc_y = 10;
    g_teams[1].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[1].assigned_task_slot = 1;
    g_incidents[1].incident_id = 5;
    g_incidents[1].priority = PRIORITY_MEDIUM;
    g_incidents[1].victim_count = 2;
    g_mission_execution_twin[1].remaining_path_cost = 8; /* nonzero -> idle guard does NOT fire */
    fault_predictor_features_update_tick(1);
    /* n=1 tick observed, and it must be stalled (not moved) ->
     * fraction = 1/1 = 1.0 for both windows. */
    check_float("team1-tick1", "stall_frac_last_5",
        1.0f, fault_predictor_stall_frac(1, FAULT_PREDICTOR_STALL_WINDOW_SHORT));
    check_float("team1-tick1", "stall_frac_last_10",
        1.0f, fault_predictor_stall_frac(1, FAULT_PREDICTOR_STALL_WINDOW_LONG));

    /* --- Boundary/safety checks: out-of-range team_id and an
     * invalid window value must return 0.0f, not crash or read
     * garbage (per the header's documented fail-safe behavior). */
    check_float("oob", "stall_frac(bad team)", 0.0f,
        fault_predictor_stall_frac(250, FAULT_PREDICTOR_STALL_WINDOW_SHORT));
    check_float("oob", "stall_frac(bad window)", 0.0f,
        fault_predictor_stall_frac(0, 7));
    check_float("oob", "delta_priority(bad team)", 0.0f,
        fault_predictor_delta_priority(250));
    check_float("oob", "delta_victim(bad team)", 0.0f,
        fault_predictor_delta_victim_count(250));

    if(g_failures > 0)
    {
        fprintf(stderr, "\n%d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }

    printf("All %d checks passed.\n", g_checks);
    return 0;
}

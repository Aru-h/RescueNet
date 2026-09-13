/*
 * Confirms core/fault_comms.c's fault_comms_broadcast_local_teams() new
 * per-team RECOVERY_ACTION_ABORT_MISSION gate: an aborted team stops
 * transmitting FAULT_BROADCAST frames (and the same-board vote pass
 * fault_comms_cast_vote() would otherwise run for it), while every
 * other, still-active team is unaffected.
 *
 * Links the real core/recovery_manager.c (decision logic) and the real
 * core/fault_comms.c/comms_transport.c/cbba.c/fault_buffer.c (transport
 * + the gate under test), but excludes core/fault_predictor.c and core/
 * watchdog.c - this file provides its own strong fault_predictor_run()/
 * watchdog_get_status()/watchdog_get_evidence_vector() so team 0's real
 * recommendation can be driven all the way to ABORT_MISSION with a
 * synthetic confidence sequence, same technique laptop/tools/recovery_
 * predictor_stress_test_stubs.c already uses (see that file for why
 * this needs the two real .c files excluded from the link rather than
 * just providing weak-overriding stubs: neither function is declared
 * __attribute__((weak)) in its real .c file, so linking both would be
 * a duplicate-symbol error).
 *
 * Build: see Makefile's check-fault-comms-abort-gate target.
 */
#include "fault_comms.h"
#include "fault_buffer.h"
#include "fault_predictor.h"
#include "watchdog.h"
#include "recovery_manager.h"
#include "mission_state.h"
#include "comms_transport.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

/* --- test-controlled stubs, replacing fault_predictor.c/watchdog.c --- */
static FaultPredictorScores_t g_test_scores[NUM_TEAMS];
static WatchdogStatus_t g_test_watchdog[NUM_TEAMS];

/* The real signature has no team_id - unlike recovery_predictor_stress_
   test_stubs.c (which only ever cares about team 0's own trajectory,
   so any team getting the same score is harmless there), THIS test
   needs team 0 and team 1 to diverge, so it can't just always return
   g_test_scores[0]. recovery_manager_step() (core/recovery_manager.c)
   calls evaluate_fault_predictor() - and therefore this function -
   exactly once per team, strictly in team_id order (0..NUM_TEAMS-1),
   every tick; a call counter that wraps at NUM_TEAMS recovers which
   team this call belongs to without needing the real signature to
   carry one. */
static uint8_t g_call_index = 0u;

FaultPredictorScores_t fault_predictor_run(const float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES])
{
    (void)evidence;
    FaultPredictorScores_t scores = g_test_scores[g_call_index];
    g_call_index = (uint8_t)((g_call_index + 1u) % NUM_TEAMS);
    return scores;
}

WatchdogStatus_t watchdog_get_status(uint8_t team_id) { return g_test_watchdog[team_id]; }

void watchdog_get_evidence_vector(uint8_t team_id, float out_evidence[7],
    uint8_t *out_confirmed_mobility_impairment)
{
    (void)team_id;
    memset(out_evidence, 0, 7 * sizeof(float));
    *out_confirmed_mobility_impairment = 0u;
}

/* watchdog_init()/watchdog_step() are referenced by core/simulation.c
   (linked in for g_simulation_trace_enabled and friends, even though
   this test never calls simulation_step()/generate_world()) - no-op
   stubs, never exercised, purely to satisfy the linker now that core/
   watchdog.c itself is excluded from this test's link. */
void watchdog_init(void) { }
void watchdog_step(void) { }

static void set_confidence_and_watchdog(uint8_t team_id, float confidence, uint8_t watchdog_agrees)
{
    memset(&g_test_scores[team_id], 0, sizeof(g_test_scores[team_id]));
    g_test_scores[team_id].score[0] = confidence;
    g_test_watchdog[team_id].anomaly_detected = watchdog_agrees;
    g_test_watchdog[team_id].anomaly_confidence = watchdog_agrees ? 0.5f : 0.0f;
    g_test_watchdog[team_id].dominant_failure_mode = 0;
}

static const char *action_name(RecoveryAction_t a)
{
    switch (a) {
        case RECOVERY_ACTION_CONTINUE_MONITORING: return "CONTINUE_MONITORING";
        case RECOVERY_ACTION_LOCAL_RECOVERY: return "LOCAL_RECOVERY";
        case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE: return "RETURN_TO_SAFE_ZONE";
        case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION: return "REQUEST_TASK_REALLOCATION";
        case RECOVERY_ACTION_ABORT_MISSION: return "ABORT_MISSION";
        default: return "?";
    }
}

int main(void)
{
    memset(g_teams, 0, sizeof(g_teams));
    memset(g_incidents, 0, sizeof(g_incidents));
    memset(g_system_tasks, 0, sizeof(g_system_tasks));
    memset(g_mission_execution_twin, 0, sizeof(g_mission_execution_twin));
    memset(g_test_watchdog, 0, sizeof(g_test_watchdog));
    memset(g_test_scores, 0, sizeof(g_test_scores));

    for (uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        g_teams[t].team_id = t;
        g_teams[t].stamina = 100.0f;
        g_teams[t].equipment_level = 100u;
        g_teams[t].status = 1u; /* STATUS_EN_ROUTE */
        g_teams[t].degraded = 1u; /* so team 0's fault_predictor path can reach
                                      the ABORT_MISSION branch, not just RETURN */
        g_mission_execution_twin[t].team_id = t;
        g_mission_execution_twin[t].mission_stage = 1u; /* MISSION_STAGE_TRAVEL */
    }

    recovery_manager_init();
    fault_comms_init();
    g_current_tick = 0u;

    printf("=== Driving team 0 to a REAL ABORT_MISSION recommendation; team 1 stays healthy ===\n");
    RecoveryAction_t team0_reached = RECOVERY_ACTION_CONTINUE_MONITORING;
    for (int tick = 1; tick <= 30; tick++)
    {
        g_current_tick = (uint32_t)tick;
        set_confidence_and_watchdog(0, 0.90f, 1u); /* team 0: severe, corroborated fault */
        set_confidence_and_watchdog(1, 0.0f, 0u);  /* team 1: nothing wrong */
        for (uint8_t t = 2; t < NUM_TEAMS; t++)
            set_confidence_and_watchdog(t, 0.0f, 0u);

        recovery_manager_step();

        RecoveryAction_t r0 = recovery_manager_get_recommendation(0);
        RecoveryAction_t r1 = recovery_manager_get_recommendation(1);
        if (tick % 5 == 0 || r0 == RECOVERY_ACTION_ABORT_MISSION)
            printf("  tick %2d: team0=%-24s team1=%s\n", tick, action_name(r0), action_name(r1));

        if (r0 == RECOVERY_ACTION_ABORT_MISSION)
        {
            team0_reached = r0;
            break;
        }
    }
    CHECK(team0_reached == RECOVERY_ACTION_ABORT_MISSION);
    CHECK(recovery_manager_get_recommendation(1) == RECOVERY_ACTION_CONTINUE_MONITORING);
    printf("Confirmed: team 0 = ABORT_MISSION (real), team 1 = CONTINUE_MONITORING.\n\n");

    printf("=== Calling fault_comms_broadcast_local_teams() once (paced gate forced open) ===\n");
    /* cbba_now_ms()'s weak default auto-advances 10ms/call starting from
       0 - the FIRST call is therefore always < FAULT_COMMS_BROADCAST_
       INTERVAL_MS and would never broadcast anything, defeating this
       test before it starts. Warm the static s_last_broadcast_ms/clock
       state past the interval first with throwaway calls, exactly the
       real cadence the pacing test (fault_comms_broadcast_pacing_test.c)
       already confirms - this test is about the ABORT_MISSION gate, not
       pacing, so it just needs one call that's actually allowed through. */
    for (int i = 0; i < 12001; i++) /* 12001 * 10ms = 120010ms > 120000ms interval */
        fault_comms_broadcast_local_teams();

    FaultBufferLocalSample_t team0_sample_before = fault_buffer_get_local_sample(0);
    FaultBufferLocalSample_t team1_sample_before = fault_buffer_get_local_sample(1);

    /* Advance g_current_tick so the NEXT successful broadcast pass
       records a genuinely different tick value than the one above -
       fault_comms_broadcast_local_teams() stamps fault_buffer_record_
       local_evidence() with whatever g_current_tick currently holds,
       independent of cbba_now_ms()'s own separate simulated clock. */
    g_current_tick += 1u;

    /* One more call: the interval has already been consumed by the last
       of the warm-up calls above (each of which, once past the gate,
       resets s_last_broadcast_ms) - so re-warm once more immediately
       before the call actually under test. */
    for (int i = 0; i < 12001; i++)
        fault_comms_broadcast_local_teams();

    FaultBufferLocalSample_t team0_sample_after = fault_buffer_get_local_sample(0);
    FaultBufferLocalSample_t team1_sample_after = fault_buffer_get_local_sample(1);

    printf("  team 0 (ABORT_MISSION) sample tick: before=%u after=%u (known before=%u after=%u)\n",
        team0_sample_before.tick, team0_sample_after.tick, team0_sample_before.known, team0_sample_after.known);
    printf("  team 1 (healthy)       sample tick: before=%u after=%u (known before=%u after=%u)\n",
        team1_sample_before.tick, team1_sample_after.tick, team1_sample_before.known, team1_sample_after.known);

    /* The gate: team 0 is ABORT_MISSION for every one of these calls
       (recovery_manager_step() was never called again after the loop
       above, so its recommendation is frozen at ABORT_MISSION) - it
       must NEVER get a fresh local-evidence record, at any point in
       either warm-up phase or the final call. */
    CHECK(team0_sample_after.known == 0u);

    /* Team 1 (still CONTINUE_MONITORING) must be unaffected - it should
       have real, fresh samples recorded across the same calls. */
    CHECK(team1_sample_after.known == 1u);
    CHECK(team1_sample_after.tick != team1_sample_before.tick || team1_sample_before.known == 0u);

    printf("\n%s: %d check(s) failed\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}

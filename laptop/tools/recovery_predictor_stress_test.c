/*
 * Adversarial/stress tests for core/fault_predictor.c + core/
 * recovery_manager.c's single-team classification path (Config A -
 * the confirmed, chosen architecture; no cross-board reclassification).
 *
 * Drives the REAL recovery_manager.c decision/tiering/hysteresis logic
 * (recovery_manager_step(), recovery_manager_get_status()) with
 * synthetic model-confidence sequences, via recovery_predictor_stress_
 * test_stubs.c's replacement of watchdog_get_status()/fault_predictor_
 * run() - see that file's own header comment. Only team 0 is
 * meaningfully driven/observed; team 0 is configured EN_ROUTE
 * (MISSION_STAGE_TRAVEL) with all OTHER evaluators (progress/stamina/
 * mobility/communication/equipment) held quiescent (zero error, full
 * stamina/equipment) so evaluate_fault_predictor()'s own signal is
 * always the only nonzero one, isolating exactly the path this test
 * targets.
 *
 * Build: see Makefile's check-recovery-predictor-stress target.
 * Run:
 *   ./recovery_predictor_stress_test
 */
#include "recovery_manager.h"
#include "mission_state.h"
#include "watchdog.h"
#include "fault_predictor.h"

#include <stdio.h>
#include <string.h>

extern WatchdogStatus_t g_test_watchdog_status[NUM_TEAMS];
extern FaultPredictorScores_t g_test_predictor_scores[NUM_TEAMS];

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

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

/* Resets all global sim state and configures team 0 EN_ROUTE with every
   OTHER evaluator quiescent (zero confidence), so evaluate_fault_
   predictor() is the only evaluator ever producing a nonzero signal. */
static void reset_world(uint8_t team0_degraded)
{
    memset(g_teams, 0, sizeof(g_teams));
    memset(g_incidents, 0, sizeof(g_incidents));
    memset(g_system_tasks, 0, sizeof(g_system_tasks));
    memset(g_mission_execution_twin, 0, sizeof(g_mission_execution_twin));
    memset(g_test_watchdog_status, 0, sizeof(g_test_watchdog_status));
    memset(g_test_predictor_scores, 0, sizeof(g_test_predictor_scores));
    g_current_tick = 0;

    for (uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        g_teams[t].team_id = t;
        g_teams[t].stamina = 100.0f;
        g_teams[t].equipment_level = 100u;
        g_teams[t].status = 1u; /* STATUS_EN_ROUTE - keeps evaluate_mobility/
                                    communication out of their RESTING
                                    early-return branch uniformly */
        g_mission_execution_twin[t].team_id = t;
        g_mission_execution_twin[t].mission_stage = MISSION_STAGE_TRAVEL;
        /* progress_error/stamina_error/communication_health default to 0
           via memset above - quiescent for progress/stamina/communication */
    }

    g_teams[0].degraded = team0_degraded;
    g_teams[0].assigned_task_pool = 1u; /* TASK_POOL_INCIDENT */
    g_teams[0].assigned_task_slot = 0u;
    g_incidents[0].priority = 2;
    g_incidents[0].victim_count = 3;

    recovery_manager_init();
}

/* Sets team 0's synthetic model confidence for this tick (a single
   fault class flagged, class 0 = MOBILITY - "non-environment" per this
   prompt's scenario 1). */
static void set_team0_confidence(float confidence)
{
    memset(&g_test_predictor_scores[0], 0, sizeof(g_test_predictor_scores[0]));
    g_test_predictor_scores[0].score[0] = confidence; /* FAULT_CLASS_MOBILITY */
}

static void set_team0_watchdog(uint8_t anomaly_detected)
{
    g_test_watchdog_status[0].anomaly_detected = anomaly_detected;
    g_test_watchdog_status[0].anomaly_confidence = anomaly_detected ? 0.5f : 0.0f;
    g_test_watchdog_status[0].dominant_failure_mode = 0; /* WATCHDOG_FAILURE_NONE */
}

/* ===================================================================
 * Scenario 1: severe single-team mobility fault, corroboration via
 * PERSISTENCE ONLY (watchdog never independently agrees) - does this
 * ever escalate past LOCAL_RECOVERY, given x>=2 cross-board
 * corroboration is structurally unreachable (not even compiled in -
 * RECOVERY_MANAGER_ENABLE_CONSENSUS_FAULT=0)?
 * ===================================================================
 */
static void scenario_1_persistence_alone(void)
{
    printf("\n=== Scenario 1: severe single-team fault, persistence-only corroboration ===\n");
    reset_world(/*degraded=*/0u);

    RecoveryAction_t highest_seen = RECOVERY_ACTION_CONTINUE_MONITORING;
    RecoveryAction_t action_at_tick8 = RECOVERY_ACTION_CONTINUE_MONITORING;
    int reached_return_at = -1;

    for (int tick = 1; tick <= 30; tick++)
    {
        g_current_tick = (uint32_t)tick;
        set_team0_confidence(0.85f);   /* >= ESCALATE_CONFIDENCE (0.80) every tick */
        set_team0_watchdog(0u);        /* watchdog NEVER corroborates */

        recovery_manager_step();

        RecoveryStatus_t st = recovery_manager_get_status(0);
        printf("  tick %2d: action=%-24s confidence=%.3f\n", tick, action_name(st.recommendation), st.confidence);

        if (tick == 8) action_at_tick8 = st.recommendation;
        if (st.recommendation > highest_seen) highest_seen = st.recommendation;
        if (st.recommendation == RECOVERY_ACTION_RETURN_TO_SAFE_ZONE && reached_return_at < 0)
            reached_return_at = tick;
    }

    printf("  Action at tick 8 (persistence streak just reached H=8): %s\n", action_name(action_at_tick8));
    printf("  Highest action ever reached (30 ticks, no watchdog corroboration): %s\n", action_name(highest_seen));
    printf("  First tick RETURN_TO_SAFE_ZONE reached: %d\n", reached_return_at);

    CHECK(action_at_tick8 == RECOVERY_ACTION_CONTINUE_MONITORING || action_at_tick8 == RECOVERY_ACTION_LOCAL_RECOVERY);
    /* Confirms: persistence ALONE (no watchdog, no cross-board) is
       sufficient to escalate past LOCAL_RECOVERY given sustained
       high confidence - it is NOT capped at LOCAL_RECOVERY forever. */
    CHECK(highest_seen > RECOVERY_ACTION_LOCAL_RECOVERY);
    CHECK(reached_return_at > 0);
}

/* Same as scenario 1, but team 0 is degraded and EN_ROUTE the whole
   time - checks whether persistence-only corroboration can climb all
   the way to ABORT_MISSION, not just RETURN_TO_SAFE_ZONE. */
static void scenario_1b_persistence_alone_to_abort(void)
{
    printf("\n=== Scenario 1b: same, but team is 'degraded' (EN_ROUTE) - does persistence alone reach ABORT_MISSION? ===\n");
    reset_world(/*degraded=*/1u);

    RecoveryAction_t highest_seen = RECOVERY_ACTION_CONTINUE_MONITORING;
    int reached_abort_at = -1;

    for (int tick = 1; tick <= 40; tick++)
    {
        g_current_tick = (uint32_t)tick;
        set_team0_confidence(0.90f);
        set_team0_watchdog(0u);

        recovery_manager_step();

        RecoveryStatus_t st = recovery_manager_get_status(0);
        if (tick <= 30 || st.recommendation != highest_seen)
            printf("  tick %2d: action=%-24s confidence=%.3f\n", tick, action_name(st.recommendation), st.confidence);

        if (st.recommendation > highest_seen) highest_seen = st.recommendation;
        if (st.recommendation == RECOVERY_ACTION_ABORT_MISSION && reached_abort_at < 0)
            reached_abort_at = tick;
    }

    printf("  Highest action ever reached (40 ticks, degraded team, persistence-only): %s\n", action_name(highest_seen));
    printf("  First tick ABORT_MISSION reached: %d\n", reached_abort_at);
    CHECK(reached_abort_at > 0);
}

/* ===================================================================
 * Scenario 2: confidence dips below the 0.20 persistence floor every
 * 7th tick - one tick short of the H=8 streak. Confirm this defeats
 * persistence-based escalation INDEFINITELY.
 * ===================================================================
 */
static void scenario_2_streak_reset_evasion(void)
{
    printf("\n=== Scenario 2: confidence dips below 0.20 every 7th tick (H=8 streak never completes) ===\n");
    reset_world(/*degraded=*/0u);

    RecoveryAction_t highest_seen = RECOVERY_ACTION_CONTINUE_MONITORING;
    int ticks_run = 200;

    for (int tick = 1; tick <= ticks_run; tick++)
    {
        g_current_tick = (uint32_t)tick;
        /* Every 7th tick: dip to 0.0 (well below FAULT_PERSISTENCE_FLOOR
           0.20). All other ticks: 0.85 (>= ESCALATE_CONFIDENCE). */
        float confidence = (tick % 7 == 0) ? 0.0f : 0.85f;
        set_team0_confidence(confidence);
        set_team0_watchdog(0u); /* no watchdog corroboration either */

        recovery_manager_step();

        RecoveryStatus_t st = recovery_manager_get_status(0);
        if (st.recommendation > highest_seen)
        {
            highest_seen = st.recommendation;
            printf("  tick %3d: NEW highest action=%-24s confidence=%.3f\n", tick, action_name(st.recommendation), st.confidence);
        }
    }

    RecoveryStatus_t final_st = recovery_manager_get_status(0);
    printf("  Final action after %d ticks of sustained (but periodically-dipping) fault: %s\n",
        ticks_run, action_name(final_st.recommendation));
    printf("  Highest action ever reached: %s\n", action_name(highest_seen));

    /* This is the exploit: a genuinely sustained fault that dips one
       tick short of H=8 defeats persistence-based escalation forever,
       and with watchdog never corroborating, this team is stuck at
       LOCAL_RECOVERY (or below) for the entire 200-tick run despite
       the fault being real and ongoing. */
    CHECK(highest_seen <= RECOVERY_ACTION_LOCAL_RECOVERY);
}

/* ===================================================================
 * Scenario 4: watchdog/model threshold desync. Force watchdog->
 * anomaly_detected to a constant (simulating WATCHDOG_ANOMALY_THRESHOLD
 * miscalibrated to 0.0 - always fires - or to >1.0 - never fires) and
 * check whether `corroborated` becomes a silent constant.
 * ===================================================================
 */
static void scenario_4a_watchdog_always_true(void)
{
    printf("\n=== Scenario 4a: watchdog desynced to ALWAYS fire (threshold ~0.0) ===\n");
    reset_world(/*degraded=*/0u);

    /* No persistence streak built at all (single fresh call each -
       predictor_persistence_streak_update() still runs every tick, but
       we only need corroborated=true from watchdog, not from sustained).
       If watchdog corroboration is stuck "always true" from tick 1, the
       hysteresis ratchet (which needs >=1 tick regardless) should still
       reach RETURN_TO_SAFE_ZONE far faster than scenario 1's 8-tick
       persistence-build + ratchet - trace several ticks to see how fast. */
    RecoveryAction_t highest_seen = RECOVERY_ACTION_CONTINUE_MONITORING;
    int reached_return_at = -1;
    for (int tick = 1; tick <= 10; tick++)
    {
        g_current_tick = (uint32_t)tick;
        set_team0_confidence(0.85f);
        set_team0_watchdog(1u); /* forced anomaly_detected=1 every tick, simulating a threshold of 0.0 */
        recovery_manager_step();
        RecoveryStatus_t st = recovery_manager_get_status(0);
        printf("  tick %2d (watchdog forced true from tick 1, no persistence needed): action=%-24s confidence=%.3f\n",
            tick, action_name(st.recommendation), st.confidence);
        if (st.recommendation > highest_seen) highest_seen = st.recommendation;
        if (st.recommendation == RECOVERY_ACTION_RETURN_TO_SAFE_ZONE && reached_return_at < 0)
            reached_return_at = tick;
    }

    /* Watchdog-forced-true makes `corroborated` true from tick 1 onward
       with ZERO persistence built - confirms a desynced-low watchdog
       threshold silently substitutes for genuine corroboration. Compare
       reached_return_at here (tick 2, per the hysteresis floor of
       RETURN_PERSISTENCE=2) against scenario 1's tick 9 - an 7-tick
       speedup purely from the watchdog threshold being broken, with no
       change in how real/genuine the underlying fault is. */
    CHECK(reached_return_at > 0 && reached_return_at < 9);
}

static void scenario_4b_watchdog_always_false(void)
{
    printf("\n=== Scenario 4b: watchdog desynced to NEVER fire (threshold > 1.0) ===\n");
    reset_world(/*degraded=*/0u);

    RecoveryAction_t highest_seen = RECOVERY_ACTION_CONTINUE_MONITORING;
    for (int tick = 1; tick <= 30; tick++)
    {
        g_current_tick = (uint32_t)tick;
        set_team0_confidence(0.85f);
        set_team0_watchdog(0u); /* forced anomaly_detected=0 every tick, simulating threshold > 1.0 (unreachable) */
        recovery_manager_step();
        RecoveryStatus_t st = recovery_manager_get_status(0);
        if (st.recommendation > highest_seen) highest_seen = st.recommendation;
    }
    printf("  Highest action reached over 30 ticks with watchdog permanently silent: %s\n", action_name(highest_seen));
    printf("  (Escalation here depends ENTIRELY on the persistence path - identical to scenario 1,\n"
           "   confirming a permanently-false watchdog doesn't zero out escalation, it just removes\n"
           "   one of the two OR-branches, leaving persistence as the sole remaining corroboration path.)\n");
    CHECK(highest_seen > RECOVERY_ACTION_LOCAL_RECOVERY); /* persistence path still escalates it eventually */
}

int main(void)
{
    scenario_1_persistence_alone();
    scenario_1b_persistence_alone_to_abort();
    scenario_2_streak_reset_evasion();
    scenario_4a_watchdog_always_true();
    scenario_4b_watchdog_always_false();

    printf("\n%s: %d check(s) failed\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}

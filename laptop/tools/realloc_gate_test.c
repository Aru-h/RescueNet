/*
 * Test for the core/allocator.c:246-247 fix: team_can_accept_incident()
 * must refuse a NEW assignment while recovery_manager_get_recommendation()
 * reports RECOVERY_ACTION_REQUEST_TASK_REALLOCATION, same as it already
 * refused for ABORT_MISSION.
 *
 * Drives team 0 to a REAL REQUEST_TASK_REALLOCATION recommendation via
 * recovery_manager_step() + injected fault-predictor confidence (same
 * technique as the prior stress-test harness), then exercises the race
 * this fix closes: team 0 becomes STATUS_AVAILABLE (representing its
 * current task completing/being released) in the SAME tick the stale-
 * but-still-active REQUEST_TASK_REALLOCATION recommendation is in force,
 * and confirms allocate_all_incidents() - the REAL function recovery_
 * dispatch() calls for this action - refuses to hand it a second,
 * unrelated system task.
 *
 * Build: see Makefile's check-realloc-gate target (links realloc_gate_
 * test_stubs.c, core/allocator.c, core/recovery_manager.c, core/
 * mission_state.c).
 */
#include "recovery_manager.h"
#include "mission_state.h"
#include "watchdog.h"
#include "fault_predictor.h"
#include "allocator.h"
#include "incident.h"

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

static void reset_world(void)
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
        g_teams[t].status = 1u; /* STATUS_EN_ROUTE for every OTHER team so
                                    they never compete for the test's
                                    system task */
        g_mission_execution_twin[t].team_id = t;
        g_mission_execution_twin[t].mission_stage = 1u; /* MISSION_STAGE_TRAVEL */
    }

    /* Team 0: EN_ROUTE with an active INCIDENT-pool task - "an active
       task assignment", per the prompt's scenario. */
    g_teams[0].status = 1u; /* STATUS_EN_ROUTE */
    g_teams[0].assigned_task_pool = 1u; /* TASK_POOL_INCIDENT */
    g_teams[0].assigned_task_slot = 0u;
    g_incidents[0].slot_occupied = 1u;
    g_incidents[0].status = 1u; /* INC_ASSIGNED */
    g_incidents[0].assigned_team = 0u;
    g_incidents[0].discovered = 1u;
    g_incidents[0].priority = 2;
    g_incidents[0].victim_count = 3;

    recovery_manager_init();
}

/* A second, unrelated open SYSTEM task - the "second new incident" the
   prompt's scenario asks about, reachable only via allocate_all_
   incidents()'s own direct SYSTEM-task assignment loop (core/
   allocator.c's second for-loop), which doesn't need real CBBA bidding
   to exercise. */
static void set_up_fresh_system_task(void)
{
    g_system_tasks[0].slot_occupied = 1u;
    g_system_tasks[0].status = 0u; /* INC_OPEN */
    g_system_tasks[0].discovered = 1u;
    g_system_tasks[0].unreachable_streak = 0u;
    g_system_tasks[0].req_capability = 0u;
}

static void set_confidence_and_watchdog(float confidence, uint8_t watchdog_agrees)
{
    memset(&g_test_predictor_scores[0], 0, sizeof(g_test_predictor_scores[0]));
    g_test_predictor_scores[0].score[0] = confidence;
    g_test_watchdog_status[0].anomaly_detected = watchdog_agrees;
    g_test_watchdog_status[0].anomaly_confidence = watchdog_agrees ? 0.5f : 0.0f;
    g_test_watchdog_status[0].dominant_failure_mode = 0;
}

int main(void)
{
    printf("=== Driving team 0 to a REAL REQUEST_TASK_REALLOCATION recommendation ===\n");
    reset_world();

    RecoveryAction_t reached = RECOVERY_ACTION_CONTINUE_MONITORING;
    for (int tick = 1; tick <= 15; tick++)
    {
        g_current_tick = (uint32_t)tick;
        /* 0.60: clears REALLOCATE_CONFIDENCE (0.50) but stays below
           ESCALATE_CONFIDENCE (0.80), so the candidate signal is exactly
           REQUEST_TASK_REALLOCATION every tick, never RETURN/ABORT. */
        set_confidence_and_watchdog(0.60f, /*watchdog_agrees=*/1u);
        recovery_manager_step();
        RecoveryStatus_t st = recovery_manager_get_status(0);
        printf("  tick %2d: recommendation=%s\n", tick, action_name(st.recommendation));
        if (st.recommendation == RECOVERY_ACTION_REQUEST_TASK_REALLOCATION)
        {
            reached = st.recommendation;
            break;
        }
    }
    CHECK(reached == RECOVERY_ACTION_REQUEST_TASK_REALLOCATION);
    printf("Confirmed: team 0's recommendation is REQUEST_TASK_REALLOCATION (real, not asserted).\n\n");

    printf("=== Scenario: team 0 becomes AVAILABLE this same tick (task just freed), "
           "with a second, unrelated open system task on offer ===\n");
    set_up_fresh_system_task();
    g_teams[0].status = 0u; /* STATUS_AVAILABLE - the race window this fix closes */

    /* Sanity precondition: team_can_accept_incident's other gates
       (stamina/equipment/discovered) are all satisfied - if the recall
       fails, it must be BECAUSE of the REQUEST_TASK_REALLOCATION check,
       not because of something else. */
    CHECK(team_can_accept_incident(&g_teams[0], &g_system_tasks[0]) == false);
    printf("  team_can_accept_incident(team 0, fresh system task) = %s (expect false)\n",
        team_can_accept_incident(&g_teams[0], &g_system_tasks[0]) ? "true" : "false");

    allocate_all_incidents();

    printf("  After allocate_all_incidents(): team 0 status=%s, system_task[0].status=%s\n",
        g_teams[0].status == 0u ? "AVAILABLE" : (g_teams[0].status == 1u ? "EN_ROUTE" : "?"),
        g_system_tasks[0].status == 0u ? "INC_OPEN (still unassigned)" : "INC_ASSIGNED");

    CHECK(g_teams[0].status == 0u); /* still AVAILABLE - was NOT grabbed for the new task */
    CHECK(g_system_tasks[0].status == 0u); /* the fresh task is still open - nobody took it */
    CHECK(g_system_tasks[0].assigned_team != 0u || g_system_tasks[0].status == 0u);

    printf("\n=== Regression control: same setup, but recommendation forced back to "
           "CONTINUE_MONITORING - confirms team 0 WOULD be assigned if not for the fix's check ===\n");
    reset_world();
    set_up_fresh_system_task();
    g_teams[0].status = 0u; /* AVAILABLE, no active recommendation this time */
    /* recovery_manager_init() above reset status.recommendation to CONTINUE_MONITORING */

    CHECK(team_can_accept_incident(&g_teams[0], &g_system_tasks[0]) == true);
    allocate_all_incidents();
    printf("  After allocate_all_incidents() with NO reallocation flag: team 0 status=%s, task assigned_team=%u\n",
        g_teams[0].status == 1u ? "EN_ROUTE (assigned)" : "still AVAILABLE",
        g_system_tasks[0].assigned_team);
    CHECK(g_teams[0].status == 1u); /* this time it DOES get grabbed - proves the harness/test task is real and biddable */
    CHECK(g_system_tasks[0].assigned_team == 0u);

    printf("\n%s: %d check(s) failed\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}

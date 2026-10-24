/*
 * Test stubs for recovery_predictor_stress_test.c.
 *
 * Links against the REAL core/recovery_manager.c (the decision/tiering
 * logic under test) and core/mission_state.c (plain data storage), but
 * replaces watchdog.c and fault_predictor.c with test-controlled globals
 * so the test can drive exact confidence/corroboration sequences without
 * a trained model or real telemetry - same "stub only what's needed to
 * isolate the module under test" approach as laptop/tools/fault_comms_
 * receive_selftest.c used for fault_comms.c's receive path.
 *
 * Also stubs the handful of allocator.h/incident.h functions recovery_
 * manager.c references but this test never exercises (recovery_dispatch()
 * is never called here - only recovery_manager_step()/get_status()) -
 * these exist purely to satisfy the linker.
 */
#include "watchdog.h"
#include "environment.h"
#include "fault_predictor.h"
#include "mission_state.h"
#include "incident.h"
#include "allocator.h"
#include "mission_link.h"
#include "feature_pack.h"

#include <string.h>

/* ---- test-controlled watchdog stub ---- */
WatchdogStatus_t g_test_watchdog_status[NUM_TEAMS];

WatchdogStatus_t watchdog_get_status(uint8_t team_id)
{
    return g_test_watchdog_status[team_id];
}

void watchdog_get_evidence_vector(uint8_t team_id, float out_evidence[7],
    uint8_t *out_confirmed_mobility_impairment)
{
    (void)team_id;
    memset(out_evidence, 0, 7 * sizeof(float));
    *out_confirmed_mobility_impairment = 0u;
}

float watchdog_get_mobility_stall_fraction(uint8_t team_id)
{
    (void)team_id;
    return 0.0f;
}

uint8_t environment_cell_travel_cost(int x, int y)
{
    (void)x;
    (void)y;
    return 1u;
}

/* ---- test-controlled fault_predictor stub ---- */
FaultPredictorScores_t g_test_predictor_scores[NUM_TEAMS];

/* Counts real calls into fault_predictor_run() below - asserted nonzero
   at the end of the test so a broken/never-reached inference gate ahead
   of it doesn't let the test pass vacuously on an untaken code path. */
uint32_t g_fault_predictor_run_calls = 0u;

FaultPredictorScores_t fault_predictor_run(const float evidence[FEATURE_SPEC_NUM_INPUTS])
{
    (void)evidence;
    g_fault_predictor_run_calls++;
    /* Matches the real signature exactly: no team_id parameter, so every
       team gets the same score this tick regardless of which team's
       loop iteration is calling it. Harmless for this test - only team
       0's g_recovery_state is ever inspected (recovery_manager_get_
       status(0)); each team's persistence streak/hysteresis state is
       tracked independently in g_recovery_state[team_id], so teams 1-9
       incidentally escalating the same way as team 0 doesn't affect
       team 0's own observed trajectory. */
    return g_test_predictor_scores[0];
}

/* This test drives fault_predictor_run() directly via g_test_predictor_
   scores and never touches real mission-link map/district state, so the
   inference gate ahead of it (core/recovery_manager.c's evaluate_fault_
   predictor()) needs to see both maps as "fully arrived" without this
   test having to link and prime the real core/mission_link.c. */
uint32_t mission_link_map_rows_received(void) { return MISSION_LINK_MAP_HEIGHT; }
uint32_t mission_link_district_rows_received(void) { return MISSION_LINK_MAP_HEIGHT; }

/* core/recovery_manager.c's evaluate_fault_predictor() reads the Pass-1
   cache instead of building fresh (feature-contract Decision: pre-dispatch
   timing) - stubbed the same way the old feature_pack_build_vector() was,
   since fault_predictor_run() ignores its input entirely here. Nothing
   calls feature_pack_build_vector() anymore in this test's link, so that
   stub was removed rather than kept alongside this one. */
void feature_pack_get_cached_vector(uint8_t team_id, float out_scaled[FEATURE_SPEC_NUM_INPUTS])
{
    (void)team_id;
    memset(out_scaled, 0, FEATURE_SPEC_NUM_INPUTS * sizeof(float));
}

/* ---- link-satisfying stubs, never exercised (recovery_dispatch() is
   never called by this test) ---- */
void abandon_task_for_recovery(TeamState_t *team) { (void)team; }
float get_soft_stamina_floor(const TeamState_t *team) { (void)team; return 0.0f; }
void assign_incident(uint8_t team_id, uint8_t incident_slot) { (void)team_id; (void)incident_slot; }
void allocate_all_incidents(void) { }

#define STUB_NEED_ALLOCATOR_SYMS
#include "causal_link_stubs.inc"

/*
 * Stubs for realloc_gate_test.c - links the REAL core/allocator.c and
 * core/recovery_manager.c (the two files with the actual fix under
 * test: allocator.c's team_can_accept_incident()), plus core/
 * mission_state.c for storage. Everything else CBBA/incident-adjacent
 * that allocator.c references but this test doesn't exercise
 * (cbba_run's real bidding, sweep/evacuee generation, scoring,
 * randomness, snapshot logging) is stubbed to a no-op purely to
 * satisfy the linker - this test only needs allocate_all_incidents()'s
 * own direct SYSTEM-task assignment loop (core/allocator.c's second
 * for-loop) to run for real, which none of these touch.
 */
#include "watchdog.h"
#include "environment.h"
#include "fault_predictor.h"
#include "mission_state.h"
#include "incident.h"
#include "cbba.h"
#include "scoring.h"
#include "simulation.h"
#include "random.h"
#include "snapshot.h"

#include <string.h>

uint8_t g_simulation_trace_enabled = 0u;

WatchdogStatus_t g_test_watchdog_status[NUM_TEAMS];
FaultPredictorScores_t g_test_predictor_scores[NUM_TEAMS];

WatchdogStatus_t watchdog_get_status(uint8_t team_id) { return g_test_watchdog_status[team_id]; }

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

FaultPredictorScores_t fault_predictor_run(const float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES])
{
    (void)evidence;
    return g_test_predictor_scores[0];
}

void abandon_task_for_recovery(TeamState_t *team) { (void)team; }

/* Real CBBA bidding is out of scope for this test - the SYSTEM-task
   loop in allocate_all_incidents() (the one this test actually
   exercises) never calls cbba_run(), it just needs it to return. */
void cbba_run(void) { }
void cbba_sync_mission_execution_twin(void) { }

void search_generate_sweep_tasks(uint32_t current_tick) { (void)current_tick; }
void generate_evacuee_offboard_incidents(void) { }
int incident_needs_support(const IncidentState_t *incident) { (void)incident; return 0; }

float get_ready_stamina(const TeamState_t *team) { (void)team; return 0.0f; }

float calculate_score(const TeamState_t *team, const IncidentState_t *incident)
{
    (void)team; (void)incident;
    return 1.0f; /* constant - only one candidate team in this test anyway */
}

uint32_t random_range_stream(RandomStream_t stream, uint32_t min, uint32_t max)
{
    (void)stream;
    return min < max ? min : max;
}

void snapshot_record(uint32_t cycle, EventType_t event_type, uint8_t team_id)
{
    (void)cycle; (void)event_type; (void)team_id;
}

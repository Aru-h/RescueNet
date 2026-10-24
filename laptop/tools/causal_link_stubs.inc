/* Link stubs for the causal / planner symbols that core/recovery_manager.c (and core/allocator.c) reference since
 * the causal-profile steps (3.x). The two stub-based selftests that link only a few real core files
 * (realloc_gate_test, recovery_predictor_stress_test) never run a causal profile or the planner, so every stub
 * reports "off" and does nothing. Included by their *_stubs.c files; define STUB_NEED_ALLOCATOR_SYMS first when
 * core/allocator.c is not part of the link. */
#include "fault_config.h"
#include "fault_injector.h"
#include "recovery_planner.h"
#include "causal_actions.h"
#include "mission_state_utils.h"

PlannerParams_t g_planner;
CausalPersistParams_t g_causal_persist_params;

int fault_profile_causal_fix(unsigned fix) { (void)fix; return 0; }
int fault_profile_causal_field_repair(void) { return 0; }
int fault_profile_causal_actions_on(void) { return 0; }
int fault_profile_causal_action_enabled(unsigned action) { (void)action; return 0; }
int fault_profile_planner_on(void) { return 0; }
int fault_profile_planner_layer(unsigned layer) { (void)layer; return 0; }
void causal_actions_detector_flag(uint8_t team_id, OperationalFaultClass_t fault_class) { (void)team_id; (void)fault_class; }
int causal_actions_reinforce_wanted(uint8_t incident_slot) { (void)incident_slot; return 0; }
int fault_injector_causal_field_start(uint8_t team_id, uint8_t kind) { (void)team_id; (void)kind; return 0; }
uint8_t fault_injector_causal_field_state(uint8_t team_id) { (void)team_id; return 0u; }
void fault_injector_causal_hold_note(uint8_t team_id, uint8_t cause) { (void)team_id; (void)cause; }
void fault_injector_causal_hold_count(uint8_t counter, uint8_t idx) { (void)counter; (void)idx; }
int fault_injector_causal_field_just_repaired(uint8_t team_id) { (void)team_id; return 0; }
int planner_team_decide(const TeamState_t *team, const PlannerEvidence_t *ev, RecoveryStatus_t *out) { (void)team; (void)ev; (void)out; return 0; }
void planner_note_recommendation(uint8_t team_id, uint8_t action) { (void)team_id; (void)action; }
int planner_release_team_for_preempt(TeamState_t *team) { (void)team; return 0; }
void team_set_available(TeamState_t *team) { team->status = STATUS_AVAILABLE; }
#ifdef STUB_NEED_ALLOCATOR_SYMS
#include "allocator.h"
uint32_t g_diag_exclusions[3];
void allocator_exclude_team(uint8_t team_id, uint32_t incident_id, uint32_t until_tick) { (void)team_id; (void)incident_id; (void)until_tick; }
#endif

#ifndef INCIDENT_TYPES_H
#define INCIDENT_TYPES_H

#include "mission_state.h"
#include <stdint.h>

/* Consecutive no-path abandons after which an incident is quarantined:
 * left out of the INC_OPEN candidate pools (allocator.c / cbba.c) instead
 * of being re-bid and re-abandoned every cycle forever. See
 * abandon_task_for_recovery() and IncidentState_t.unreachable_streak. */
#define INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD 3u

typedef enum
{
    PASSIVE_NONE = 0,
    PASSIVE_GRID_DEGRADE,
    PASSIVE_COMMS_BOOST

} PassiveEffect_t;

typedef struct
{
    IncidentType_t incident_type;
    Priority_t priority;
    TeamSpecialization_t required_specialization;
    uint8_t hazard_flags;
    PassiveEffect_t passive_effect;
    uint8_t required_equipment;
    uint8_t preferred_equipment;
} IncidentArchetype_t;

extern const IncidentArchetype_t g_incident_archetypes[NUM_INCIDENT_TYPES];

int incident_degradation_radius(IncidentType_t type);
int incident_base_degradation(IncidentType_t type);
void initialize_incidents(void);
uint32_t allocate_incident_id(void);
void update_all_incident_work(void);
void update_all_passive_effects(void);
void complete_incident_stage(IncidentState_t *incident);
/* On-scene work for one stage of an incident on this floor (see
   INCIDENT_BASE_STAGE_WORK), and the value work_remaining resets to at
   each of this incident's stages. */
uint16_t incident_stage_work_for_floor(uint8_t floor);
uint16_t incident_stage_work(const IncidentState_t *incident);
/* See their definitions in incident.c - board side of laptop-issued
   stage changes and resolutions. */
void incident_release_teams_for_authority(IncidentState_t *task);
void incident_close_for_authority(IncidentState_t *task);
void complete_return_to_rest(TeamState_t *team);
int incident_needs_support(const IncidentState_t *incident);
/* Board-hosted-team work rate: deterministic, fault-adjusted (equipment_
 * level + fault_state.severity[]-derived, same spirit as core/team.c's
 * stamina-drain formula), NOT the STREAM_WORK_RATE RNG roll below - see
 * this function's definition for the formula and update_incident_work()
 * for where it replaces that roll for teams cbba_team_is_local() reports
 * true for. Exposed (not static) so core/mission_link.c's
 * mission_link_build_status() can report the exact same value that will
 * be (or was) applied to that team's work, rather than a second,
 * independently-computed figure that could silently disagree with it -
 * see mission_link.h's MissionLinkStatus_t doc. */
uint8_t team_effective_work_rate(const TeamState_t *team);
/* The fault-free component of team_effective_work_rate() above: pure
 * equipment_level -> rate scaling, no fault_state term. Exposed
 * (not static) so core/mission_execution_twin.c's nominal_work_rate()
 * can compute a true fault-free baseline using the exact same
 * equipment-scaling formula the real, fault-adjusted rate is built
 * from, instead of an independently-guessed flat constant that could
 * silently drift from what team_effective_work_rate() actually does -
 * see this function's definition for the formula. */
uint8_t team_base_work_rate(uint8_t equipment_level);
void initialize_evacuation_gateways(uint32_t seed);
void initialize_evacuation_teams(void);
void generate_incidents_if_needed(void);
/* Anti-starvation for OPEN incidents specifically - see this
 * function's own comment in incident.c for the full contract. Called
 * from simulation_step(). */
void escalate_starved_incidents(uint32_t current_tick);
/* Generates open sweep tasks for stale districts, left for the normal
 * competitive system-task loop to assign - see the large comment on
 * this function's definition in incident.c for why this isn't a
 * direct-assign bypass. */
void search_generate_sweep_tasks(uint32_t current_tick);
/* Coverage marking, cycle-completion, and reveal - runs once per tick. */
void search_sweep_step(uint32_t current_tick);
int create_equipment_cache_incident(TeamState_t *team);
int create_evacuee_offboard_incident(uint8_t safezone_id);
void generate_evacuee_offboard_incidents(void);
int begin_return_to_safe_zone(TeamState_t *team);
uint8_t find_nearest_safe_zone(const TeamState_t *team);
void abandon_task_for_recovery(TeamState_t *team);

#endif

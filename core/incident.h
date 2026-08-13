#ifndef INCIDENT_TYPES_H
#define INCIDENT_TYPES_H

#include "mission_state.h"
#include <stdint.h>

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
void complete_return_to_safe_zone_task(TeamState_t *team, IncidentState_t *incident);
int incident_needs_support(const IncidentState_t *incident);
void initialize_evacuation_gateways(uint32_t seed);
void initialize_evacuation_teams(void);
void generate_incidents_if_needed(void);
/* Not currently called from the live tick loop - see the large comment
 * on this function's definition in incident.c for why. */
void escalate_incidents_by_zone_trust(uint32_t current_tick);
/* Generates open sweep tasks for stale districts, left for the normal
 * competitive system-task loop to assign - see the large comment on
 * this function's definition in incident.c for why this isn't a
 * direct-assign bypass. */
void search_generate_sweep_tasks(uint32_t current_tick);
/* Coverage marking, cycle-completion, and reveal - runs once per tick. */
void search_sweep_step(uint32_t current_tick);
int create_equipment_cache_incident(TeamState_t *team);
int create_evacuee_offboard_incident(uint8_t safezone_id);
int create_return_to_safe_zone_task(TeamState_t *team);
void abandon_task_for_recovery(TeamState_t *team);
void reopen_future_queued_incidents(void);

#endif

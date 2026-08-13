#ifndef MISSION_STATE_UTILS_H
#define MISSION_STATE_UTILS_H

#include "mission_state.h"

const char *team_status_to_string(TeamStatus_t status);
const char *incident_status_to_string(IncidentStatus_t status);
const char *specialization_to_string(TeamSpecialization_t spec);
const char *priority_to_string(Priority_t priority);
float team_recovery_rate(const TeamState_t *team);
float team_travel_stamina_cost(const TeamState_t *team);
void team_set_available(TeamState_t *team);

#endif

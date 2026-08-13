#ifndef ALLOCATOR_H

#define ALLOCATOR_H
#include "mission_state.h"
#include "scoring.h"
#include <stdbool.h>

uint8_t select_best_team(const IncidentState_t *incident);

void assign_incident(uint8_t team_id, uint8_t incident_slot);

void allocate_all_incidents(void);

bool team_can_accept_incident(const TeamState_t *team,const IncidentState_t *incident);


#endif
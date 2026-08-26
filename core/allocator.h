#ifndef ALLOCATOR_H

#define ALLOCATOR_H
#include "mission_state.h"
#include "scoring.h"
#include <stdbool.h>

uint8_t select_best_team(const IncidentState_t *incident);

void assign_incident(uint8_t team_id, uint8_t incident_slot);

void allocate_all_incidents(void);

bool team_can_accept_incident(const TeamState_t *team,const IncidentState_t *incident);

/*
 * Single source of truth for "how much stamina does this team need to
 * take on new work" - shared with core/recovery_manager.c so its idle-
 * team rest-routing threshold can't silently drift away from the
 * threshold team_can_accept_incident() actually gates on. Previously
 * static/private to allocator.c; exposing it here is the fix for a
 * real bug: recovery_manager.c's evaluate_stamina() used a fixed,
 * much lower threshold (RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD =
 * 10.0) to decide whether an idle team needs to be routed to rest,
 * while this function (called from team_can_accept_incident() above)
 * returns 20.0-40.0 depending on fitness tier - so a team below this
 * function's floor but above 10.0 was ineligible for new work AND not
 * yet recommended to rest, with nothing in the tick loop able to move
 * it out of that state. See evaluate_stamina()'s own comment for the
 * fix.
 */
float get_soft_stamina_floor(const TeamState_t *team);

#endif
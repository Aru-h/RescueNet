#ifndef TEAM_H
#define TEAM_H

#include "mission_state.h"

void generate_teams(uint32_t seed);
void update_team(TeamState_t *team);
void update_all_teams(void);
void move_team(TeamState_t *team);
void move_all_teams(void);

/*
 * Single source of truth for "how much stamina does a resting team
 * need before it's genuinely ready to leave rest" - shared with
 * core/allocator.c so team_can_accept_incident()'s "steal a resting
 * team under high demand" eligibility check can't use a different,
 * lower bar than the one update_team() itself requires to naturally
 * promote a team out of STATUS_RESTING (get_soft_stamina_floor(),
 * 20.0-40.0, vs this function, 50.0-65.0 - see mission_state.h's
 * *_READY_STAMINA vs *_MIN_STAMINA defines).
 *
 * Previously static/private to team.c. Exposing it here is the fix
 * for a real bug: the steal-eligibility check used to compare against
 * get_soft_stamina_floor() instead, which let the greedy system-task
 * assignment loop (core/allocator.c) pull a team out of RESTING the
 * moment it cleared the much lower "eligible for new work at all"
 * bar - long before update_team() would have promoted it back to
 * STATUS_AVAILABLE on its own. The freshly-stolen team's sudden
 * reassignment then correctly tripped evaluate_progress()'s mission-
 * execution-twin deviation check (core/recovery_manager.c), which
 * sent it straight back toward the safe zone it had never really
 * left - and since it was already there, it re-entered RESTING
 * immediately, only to be stolen again the instant demand pressure
 * re-evaluated it as eligible, which for an oversubscribed
 * specialization (available_capacity < waiting_demand) was
 * essentially every tick. Confirmed empirically this session: a team
 * caught in this loop stayed stuck oscillating between RESTING and
 * EN_ROUTE, at the same location, indefinitely - up to 59.6% of an
 * entire 5000-tick run for one team in one seed - instead of the
 * ~200-400 ticks a genuine, uninterrupted rest cycle actually needs
 * to reach this same threshold.
 */
float get_ready_stamina(const TeamState_t *team);

#endif
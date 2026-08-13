#ifndef SIMULATION_H
#define SIMULATION_H

#include <stdint.h>
#include "mission_state.h"

extern uint8_t g_simulation_trace_enabled;

void generate_world(uint32_t seed);

void simulation_step(void);

void move_team(TeamState_t *team);

void move_all_teams(void);

#endif
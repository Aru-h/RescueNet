#ifndef SIMULATION_H
#define SIMULATION_H

#include <stdint.h>
#include "mission_state.h"

extern uint8_t g_simulation_trace_enabled;

/*
 * Which half of the mission this process simulates. Set after
 * sim_runner_init() (which resets it to SIM_ROLE_STANDALONE).
 *
 *   SIM_ROLE_STANDALONE    laptop data-collection runs: the whole world
 *                          and every team, as fast as the host allows.
 *   SIM_ROLE_BOARD         a board (sim_start_seed_board() sets it):
 *                          executes its own teams - CBBA, movement,
 *                          work, stamina, watchdog, recovery - on the
 *                          incidents and faults the laptop sends. No
 *                          world events: no new incidents, hazards,
 *                          faults, stage branching or sweep tasks of
 *                          its own.
 *   SIM_ROLE_WORLD_MIRROR  the laptop during a board mission
 *                          (laptop/mission_mirror.h): runs the world
 *                          events a board skips, and takes every
 *                          team's state from board status reports
 *                          instead of simulating the teams itself.
 */
typedef enum
{
    SIM_ROLE_STANDALONE = 0,
    SIM_ROLE_BOARD,
    SIM_ROLE_WORLD_MIRROR
} SimRole_t;

extern SimRole_t g_sim_role;

void generate_world(uint32_t seed);

void simulation_step(void);

void move_team(TeamState_t *team);

void move_all_teams(void);

#endif
#ifndef PATHFINDING_H
#define PATHFINDING_H

#include "mission_state.h"

int find_next_path_step(int start_x, int start_y,int dest_x, int dest_y,int cost,int *next_x, int *next_y);
int find_next_path_step_with_equipment(
    int start_x,
    int start_y,
    int dest_x,
    int dest_y,
    uint8_t equipment_flags,
    int *next_x,
    int *next_y);
/* Same search as find_next_path_step(), and also reports the cost of the
 * route that remains once the team has taken that first step. move_team()
 * and the mission-execution twin use this instead of subtracting the next
 * cell's travel cost themselves: with diagonal steps a step costs more
 * than the cell's travel cost, and the route cost is rounded from a finer
 * internal scale, so only the pathfinder knows the exact drop. */
int find_next_path_step_remaining(
    int start_x,
    int start_y,
    int dest_x,
    int dest_y,
    int *next_x,
    int *next_y,
    uint16_t *remaining_after_step);
uint16_t find_path_cost(int start_x, int start_y,int dest_x, int dest_y);
uint16_t find_path_cost_with_equipment(
    int start_x,
    int start_y,
    int dest_x,
    int dest_y,
    uint8_t equipment_flags);

/* Intra-call path cost cache. Safe ONLY across a span where the grid
 * (navigability/travel cost) and the set of queried points cannot change
 * mid-span -- e.g. for the duration of a single cbba_run() invocation,
 * since environment_update_hazard_propagation() and move_all_teams() do
 * not run until after allocate_all_incidents() returns. Callers MUST call
 * path_cache_end() before anything in the tick that can mutate the grid
 * or team positions runs, and MUST call path_cache_begin() again for any
 * subsequent span (e.g. a second allocate_all_incidents() call later in
 * the same tick) rather than reusing an old span's cache. */
void path_cache_begin(void);
void path_cache_end(void);
#endif

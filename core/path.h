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

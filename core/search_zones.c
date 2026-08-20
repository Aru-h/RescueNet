#include <stddef.h>
#include "search_zones.h"

SearchZoneState_t g_search_zones[NUM_SEARCH_ZONES];
uint32_t g_cell_last_covered_tick[GRID_WIDTH][GRID_HEIGHT];
uint32_t g_diag_sweep_genuine_complete[NUM_SEARCH_ZONES] = {0};
uint32_t g_diag_sweep_stalled[NUM_SEARCH_ZONES] = {0};
uint32_t g_diag_sweep_stall_coverage_sum[NUM_SEARCH_ZONES] = {0};
uint32_t g_diag_sweep_tasks_created[NUM_SEARCH_ZONES] = {0};

void search_zones_init(void)
{
    for(uint8_t i = 0; i < NUM_SEARCH_ZONES; i++)
    {
        g_search_zones[i].last_swept_tick = 0;
        g_search_zones[i].sweep_count = 0;
        g_search_zones[i].aftershock_hits_since_swept = 0;
        g_search_zones[i].cycle_started_tick = 0;
        g_search_zones[i].covered_cells = 0;
        g_search_zones[i].active_sweep_ticks = 0;
        g_search_zones[i].reclaimed_at_tick = 0;
    }

    for(int x = 0; x < GRID_WIDTH; x++)
        for(int y = 0; y < GRID_HEIGHT; y++)
            g_cell_last_covered_tick[x][y] = 0;
}

uint8_t zone_id_for_location(int16_t x, int16_t y)
{
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x >= GRID_WIDTH)  x = GRID_WIDTH  - 1;
    if(y >= GRID_HEIGHT) y = GRID_HEIGHT - 1;

    return g_environment[x][y].district_id;
}

uint8_t search_zone_trust(uint8_t district_id, uint32_t current_tick)
{
    if(district_id >= NUM_SEARCH_ZONES)
        return 0;

    const SearchZoneState_t *zone = &g_search_zones[district_id];

    uint32_t ticks_since_swept = (current_tick >= zone->last_swept_tick)
        ? (current_tick - zone->last_swept_tick) : 0;

    int32_t trust_base;
    if(ticks_since_swept >= ZONE_TRUST_DECAY_CAP_TICKS)
        trust_base = 0;
    else
        trust_base = (int32_t)(100 - (ticks_since_swept * 100u) / ZONE_TRUST_DECAY_CAP_TICKS);

    int32_t trust = trust_base - (int32_t)(zone->aftershock_hits_since_swept * ZONE_TRUST_AFTERSHOCK_PENALTY_PERCENT);
    if(trust < 0)
        trust = 0;
    if(trust > 100)
        trust = 100;

    return (uint8_t)trust;
}

void search_zone_mark_cell_covered(int16_t x, int16_t y, uint32_t current_tick)
{
    if(x < 0 || y < 0 || x >= GRID_WIDTH || y >= GRID_HEIGHT)
        return;

    uint8_t district_id = g_environment[x][y].district_id;
    if(district_id >= NUM_SEARCH_ZONES)
        return;

    SearchZoneState_t *zone = &g_search_zones[district_id];

    /* Only counts toward coverage the first time this cell is seen
     * within the CURRENT cycle - re-covering the same cell repeatedly
     * (e.g. Search idling near it) must not inflate covered_cells. */
    if(g_cell_last_covered_tick[x][y] < zone->cycle_started_tick)
        zone->covered_cells++;

    g_cell_last_covered_tick[x][y] = current_tick;
}

uint8_t search_zone_coverage_percent(uint8_t district_id)
{
    if(district_id >= NUM_SEARCH_ZONES)
        return 0;

    uint32_t navigable_total = 0;
    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            if(cell->district_id == district_id && cell->navigable)
                navigable_total++;
        }
    }

    if(navigable_total == 0)
        return 100;  /* nothing to cover - trivially complete, avoids div-by-zero */

    uint32_t covered = g_search_zones[district_id].covered_cells;
    if(covered > navigable_total)
        covered = navigable_total;  /* covered_cells can include now-non-navigable cells */

    return (uint8_t)((covered * 100u) / navigable_total);
}

extern uint32_t g_diag_sweep_genuine_complete[NUM_SEARCH_ZONES];
extern uint32_t g_diag_sweep_stalled[NUM_SEARCH_ZONES];
extern uint32_t g_diag_sweep_stall_coverage_sum[NUM_SEARCH_ZONES];

uint8_t search_zone_check_cycle_completion(uint8_t district_id, uint32_t current_tick)
{
    if(district_id >= NUM_SEARCH_ZONES)
        return 0;

    SearchZoneState_t *zone = &g_search_zones[district_id];
    uint8_t coverage = search_zone_coverage_percent(district_id);

    if(coverage >= ZONE_SWEEP_COMPLETION_FLOOR_PERCENT)
    {
        /* Genuine completion. */
        g_diag_sweep_genuine_complete[district_id]++;
        zone->last_swept_tick = current_tick;
        zone->sweep_count++;
        zone->aftershock_hits_since_swept = 0;
        zone->covered_cells = 0;
        zone->cycle_started_tick = current_tick;
        zone->active_sweep_ticks = 0;
        return 1;  /* caller should release the sweeping team */
    }

    if(zone->active_sweep_ticks >= ZONE_SWEEP_ACTIVE_TICK_CAP)
    {
        g_diag_sweep_stalled[district_id]++;
        g_diag_sweep_stall_coverage_sum[district_id] += coverage;
        /* Stalled, not completed. covered_cells and last_swept_tick
         * are left exactly as they are - progress carries over and
         * the district stays a legitimate stale candidate. Only the
         * "how long has this attempt been going" clock resets, so it
         * doesn't immediately re-timeout the instant it's picked
         * again. Still signals the caller to release the team - a
         * stalled attempt should stop consuming that team, same as a
         * genuine completion would, just without crediting the
         * district as freshly checked. */
        zone->active_sweep_ticks = 0;
        return 1;
    }

    return 0;
}

uint8_t search_zone_pick_next_target(uint32_t current_tick, uint16_t exclude_mask)
{
    uint8_t best_zone = NO_ASSIGNMENT;
    uint32_t best_staleness = 0;

    for(uint8_t z = 0; z < NUM_SEARCH_ZONES; z++)
    {
        if(exclude_mask & (1u << z))
            continue;

        uint32_t staleness = (current_tick >= g_search_zones[z].last_swept_tick)
            ? (current_tick - g_search_zones[z].last_swept_tick) : 0;

        if(best_zone == NO_ASSIGNMENT || staleness > best_staleness)
        {
            best_zone = z;
            best_staleness = staleness;
        }
    }

    return best_zone;
}

uint8_t search_zone_pick_frontier_cell(uint8_t district_id, int16_t from_x, int16_t from_y,
                                        int16_t *out_x, int16_t *out_y)
{
    if(district_id >= NUM_SEARCH_ZONES || out_x == NULL || out_y == NULL)
        return 0;

    uint32_t cycle_start = g_search_zones[district_id].cycle_started_tick;

    uint8_t found = 0;
    int32_t best_dist_sq = 0;
    int16_t best_x = 0, best_y = 0;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            EnvironmentCell_t *cell = &g_environment[x][y];
            if(cell->district_id != district_id || !cell->navigable)
                continue;
            if(g_cell_last_covered_tick[x][y] >= cycle_start)
                continue;  /* already covered this cycle */

            int32_t dx = x - from_x;
            int32_t dy = y - from_y;
            int32_t dist_sq = dx * dx + dy * dy;

            if(!found || dist_sq < best_dist_sq)
            {
                found = 1;
                best_dist_sq = dist_sq;
                best_x = (int16_t)x;
                best_y = (int16_t)y;
            }
        }
    }

    if(!found)
        return 0;

    *out_x = best_x;
    *out_y = best_y;
    return 1;
}



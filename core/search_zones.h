#ifndef SEARCH_ZONES_H
#define SEARCH_ZONES_H

#include <stdint.h>
#include "mission_state.h"  /* GRID_WIDTH, GRID_HEIGHT */
#include "environment.h"    /* NUM_DISTRICT_TYPES */

/* A "zone" is a real world-gen district instance, not a synthetic grid
 * partition. There's exactly one district instance per DISTRICT_* type
 * today (stage_generate_districts() calls assign_anchor() once per
 * type), so NUM_SEARCH_ZONES == NUM_DISTRICT_TYPES (10) and a zone id
 * is just a district_id. Every cell and building already carries its
 * own district_id from world-gen - nothing here computes geometry,
 * it just reads what's already there. */
#define NUM_SEARCH_ZONES NUM_DISTRICT_TYPES

/* Trust decay: 100% at 0 ticks since last full sweep, 0% at
 * DECAY_CAP_TICKS stale. Reused reasoning from STALL_THRESHOLD_TICKS -
 * ~3 real hours untouched is a genuinely serious staleness threshold
 * at this project's timescale. */
#define ZONE_TRUST_DECAY_CAP_TICKS (TICKS_PER_HOUR * 3)

/* Flat penalty per aftershock hit since last sweep, regardless of
 * staleness - a district that just took fresh hidden damage should
 * drop trust immediately, not wait for the staleness clock. */
#define ZONE_TRUST_AFTERSHOCK_PENALTY_PERCENT 10

/* A sweep cycle is "genuinely done" once this much of a district's
 * navigable cells have been covered - not literal 100%, since a few
 * cells can be practically unreachable (boxed in by blocked roads)
 * without the cycle ever being "wrong" to call complete. */
#define ZONE_SWEEP_COMPLETION_FLOOR_PERCENT 95

/* Escape valve for the other failure mode: a cycle that's stalled
 * (heavy fault injection, genuinely disconnected pockets) rather than
 * finished. Counts only ACTIVE sweep ticks against this district (not
 * wall-clock since the attempt started), so interruptions for a
 * critical SURVIVOR_SIGNAL don't count against it. ~1.5 real hours at
 * this project's tick rate - generous relative to the largest
 * measured district's ideal clear time (~26 min at radius 3), leaving
 * real room for frontier-based inefficiency before calling it stalled. */
#define ZONE_SWEEP_ACTIVE_TICK_CAP 150

/* Cooldown after a sweep task for a district gets reclaimed for a
 * higher-priority safe-zone/equipment task, before that district is
 * eligible to be nominated for a new sweep task again. Short relative
 * to ZONE_TRUST_DECAY_CAP_TICKS - just long enough to let the pool
 * pressure that caused the eviction clear, not so long it meaningfully
 * delays a real sweep. */
#define ZONE_SWEEP_RECLAIM_COOLDOWN_TICKS 30

/* Board missions only (SIM_ROLE_WORLD_MIRROR, see simulation.h): a
 * district gets a sweep task only once its trust (search_zone_trust())
 * falls below this, or if it has never been swept at all. Every sweep
 * task becomes an incident frame on the radio link, and a sweep task
 * moves to a new frontier cell each time the team finishes a spot, so
 * sending one for a district that was searched an hour ago spends
 * airtime and a Search team on ground that is still trusted. 50% is
 * 1.5 hours untouched (ZONE_TRUST_DECAY_CAP_TICKS is 3 hours), or less
 * after aftershock hits. Standalone data-collection runs keep sweeping
 * the stalest district whenever a Search team is free, so their
 * datasets stay comparable with earlier runs. */
#define ZONE_SWEEP_DISPATCH_TRUST_PERCENT 50

/* 1 if a board mission should send a sweep for this district now - see
 * ZONE_SWEEP_DISPATCH_TRUST_PERCENT. */
uint8_t search_zone_needs_sweep(uint8_t district_id, uint32_t current_tick);

/* Search's continuous full-radius sweep. Other teams use a smaller
 * radius, only while ON_SCENE - see the reveal-writer functions. */
#define SEARCH_SWEEP_RADIUS 3
#define OTHER_TEAM_REVEAL_RADIUS 2

typedef struct {
    uint32_t last_swept_tick;
    uint16_t sweep_count;      /* telemetry/debug only */

    /* New hidden damage in this district since its last full sweep -
     * incremented directly from apply_aftershocks() via the collapsing
     * building's own district_id. Reset to 0 on genuine cycle
     * completion (floor reached), NOT on a cap timeout. */
    uint16_t aftershock_hits_since_swept;

    /* Coverage bookkeeping for the current sweep cycle. covered_cells
     * persists across interruptions (a team pulled away for a critical
     * SURVIVOR_SIGNAL loses no progress) and only resets on genuine
     * completion. active_sweep_ticks only increments while a team is
     * actually actively sweeping THIS district - not wall-clock -
     * so interruptions don't count against the stall cap either. */
    uint32_t cycle_started_tick;
    uint16_t covered_cells;
    uint16_t active_sweep_ticks;

    /* Tick a sweep task for this district was last evicted by
     * find_reclaimable_system_task_slot() to free a slot for a
     * higher-priority RETURN_TO_SAFE_ZONE/EQUIPMENT_CACHE task. Without
     * this, the generator immediately re-nominates the same district
     * (it's still the stalest) the very next tick, that task gets
     * evicted again if the pool pressure hasn't let up, and the pair
     * repeats every tick - a tight create/evict thrash loop that never
     * makes progress and shows up on the dashboard as a task
     * flickering in and out of existence. */
    uint32_t reclaimed_at_tick;
} SearchZoneState_t;

extern SearchZoneState_t g_search_zones[NUM_SEARCH_ZONES];

/* 0 = never covered. Separate from EnvironmentCell_t deliberately -
 * this is sweep-tracking state, not core environment state. */
extern uint32_t g_cell_last_covered_tick[GRID_WIDTH][GRID_HEIGHT];

void search_zones_init(void);

/* Which district a grid location belongs to - a direct lookup against
 * the cell's own district_id, clamping out-of-range input to the
 * nearest valid cell rather than asserting. */
uint8_t zone_id_for_location(int16_t x, int16_t y);

/* 0-100. Safe to call any time. */
uint8_t search_zone_trust(uint8_t district_id, uint32_t current_tick);

/* Marks one cell covered by a Search team's radius this tick. Updates
 * g_cell_last_covered_tick and, the first time this cell is seen
 * within the current cycle, increments that district's covered_cells.
 * Also increments active_sweep_ticks for the district containing
 * (x,y) - called once per covering team per tick, not once per cell,
 * see search_mark_radius_covered() in simulation.c. */
void search_zone_mark_cell_covered(uint8_t district_id ,int16_t x, int16_t y, uint32_t current_tick);

/* Live-computed against currently-navigable cells (not cached - see
 * the fault-injection navigability note from earlier design). */
uint8_t search_zone_coverage_percent(uint8_t district_id);

/* Checks the current cycle against the floor/cap and applies whichever
 * outcome fires (genuine completion vs. stalled timeout - see the
 * struct comments for what each does differently). No-op if neither
 * threshold is crossed. Returns 1 if a genuine completion happened. */
uint8_t search_zone_check_cycle_completion(uint8_t district_id, uint32_t current_tick);

/* Highest ticks_since_last_swept among districts NOT set in
 * exclude_mask (bit i = district i already has a sweep task in
 * flight, open or assigned). Returns NO_ASSIGNMENT (0xFF) if every
 * district is excluded. */
uint8_t search_zone_pick_next_target(uint32_t current_tick, uint16_t exclude_mask);

/* Nearest still-uncovered navigable cell in a district to (from_x,
 * from_y), by straight-line distance (cheap - actual routing to it is
 * handled by the existing pathfinder, this just picks where). Returns
 * 0 if the district has nothing left uncovered (shouldn't normally be
 * called in that case - check search_zone_coverage_percent() first). */
uint8_t search_zone_pick_frontier_cell(uint8_t district_id, int16_t from_x, int16_t from_y,
                                        int16_t *out_x, int16_t *out_y);

#endif


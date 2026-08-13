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
void search_zone_mark_cell_covered(int16_t x, int16_t y, uint32_t current_tick);

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


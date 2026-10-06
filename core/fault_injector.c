#include "fault_injector.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "allocator.h"
#include "causal_actions.h"
#include "environment.h"
#include "incident.h"
#include "recovery_planner.h"
#include "mission_state.h"
#include "path.h"
#include "random.h"
#include "snapshot.h"
#include "team.h"

static uint32_t g_fault_seed = 1;

/* ------------------------------------------------------------------
 * FAULT_PROFILE_CAUSAL support.
 *
 * Draws: every random decision in the causal profile is a pure hash of
 * (seed, tick, team, class-salt), the same construction as
 * environment.c's deterministic_hash(), so a run's causal faults never
 * touch (or depend on) the shared STREAM_* generators and are
 * reproducible per seed regardless of what else consumes those streams.
 * ------------------------------------------------------------------ */
static CausalFaultStats_t s_causal_stats;
static CausalWorldStats_t s_causal_world;
static CausalTeamTruth_t s_causal_truth[NUM_TEAMS];
static uint8_t s_causal_repair_ticks[NUM_TEAMS];
static uint8_t s_causal_rest_dwell[NUM_TEAMS];
static float s_causal_wear[NUM_TEAMS];
static int16_t s_causal_prev_x[NUM_TEAMS];
static int16_t s_causal_prev_y[NUM_TEAMS];
static uint8_t s_causal_prev_valid[NUM_TEAMS];
static uint32_t s_causal_last_rest_tick[NUM_TEAMS];
/* Step 3.6 persistent faults: field repair / maintenance holds, the mobility base-repair dwell drawn at onset,
 * PREVENT rest orders, escalation flags, and the last crossed cell's roughness (for the hazard forecast). */
static CausalPersistStats_t s_persist_stats;
static uint8_t s_field_kind[NUM_TEAMS];               /* 0 none, 1 mobility repair, 2 resource repair, 3 maintenance */
static uint32_t s_field_end[NUM_TEAMS];
static uint8_t s_dwell_need[NUM_TEAMS];
static uint8_t s_prevent_order[NUM_TEAMS];
static uint32_t s_prevent_tick[NUM_TEAMS];
static uint8_t s_escalated[NUM_TEAMS][NUM_OPERATIONAL_FAULT_CLASSES];
static float s_rough_last[NUM_TEAMS];
/* Mission model bookkeeping, per incident slot (valid while the stored id
 * matches the slot's current incident_id). */
static uint32_t s_causal_inc_id[NUM_INCIDENTS];
static uint8_t s_causal_inc_escalations[NUM_INCIDENTS];

static uint32_t causal_hash(uint32_t tick, uint32_t team_id, uint32_t salt)
{
    uint32_t value =
        g_fault_seed ^
        (tick * 1664525u) ^
        ((team_id + 1u) * 1013904223u) ^
        ((salt + 1u) * 3266489917u);

    value ^= value >> 16;
    value *= 2246822519u;
    value ^= value >> 13;
    value *= 3266489917u;
    value ^= value >> 16;

    return value;
}

/* Salts: onset roll = class * 2, duration draw = class * 2 + 1. */
#define CAUSAL_SALT_ONSET(c)    ((uint32_t)(c) * 2u)
#define CAUSAL_SALT_DURATION(c) ((uint32_t)(c) * 2u + 1u)
/* Step 3.6 salts (the existing ones in use are 0-9, 100-102, 200, 300, 400-401, 410, 600). */
#define CAUSAL_SALT_NATURAL(c)  (700u + (uint32_t)(c))
#define CAUSAL_SALT_ESCALATE(c) (710u + (uint32_t)(c))
#define CAUSAL_SALT_DWELL       720u
#define CAUSAL_SALT_FIELD_TIME  721u
#define CAUSAL_SALT_FIELD_OK    722u

static uint32_t causal_persist_natural_ticks(uint8_t team_id, OperationalFaultClass_t fault_class);

const CausalFaultStats_t *fault_injector_causal_stats(void)
{
    return &s_causal_stats;
}

const CausalWorldStats_t *fault_injector_causal_world_stats(void)
{
    return &s_causal_world;
}

const CausalTeamTruth_t *fault_injector_causal_truth(uint8_t team_id)
{
    return &s_causal_truth[team_id < NUM_TEAMS ? team_id : 0];
}

/* Uniform in [0, 1). */
static float causal_unit(uint32_t tick, uint32_t a, uint32_t salt)
{
    return (float)(causal_hash(tick, a, salt) >> 8) * (1.0f / 16777216.0f);
}

static uint32_t fault_duration_for_class(uint8_t team_id, OperationalFaultClass_t fault_class)
{
    /* Step 3.6: mobility and resource faults persist (a long natural-recovery tail, not a 6-14 tick timer). */
    if(fault_profile_causal_persist() &&
       (fault_class == FAULT_CLASS_MOBILITY || fault_class == FAULT_CLASS_RESOURCE))
        return causal_persist_natural_ticks(team_id, fault_class);

    /* Consequential roads: unattended damage persists for hours (uint16
     * ticks; the shared duration ranges below are 8-bit). */
    if(fault_class == FAULT_CLASS_ENVIRONMENT && fault_profile_causal_roads_on())
    {
        uint32_t lo = g_causal_roads.persist_min_ticks;
        uint32_t hi = g_causal_roads.persist_max_ticks;

        if(hi < lo)
            hi = lo;
        return lo + causal_hash(g_current_tick, team_id, CAUSAL_SALT_DURATION(fault_class)) % (hi - lo + 1u);
    }

    uint8_t min_ticks = g_fault_config.duration_min_ticks[fault_class];
    uint8_t max_ticks = g_fault_config.duration_max_ticks[fault_class];

    if(min_ticks == 0 && max_ticks == 0)
        return 0;

    if(min_ticks == 0)
        min_ticks = 1;

    if(max_ticks < min_ticks)
        max_ticks = min_ticks;

    if(fault_profile_is_causal())
        return min_ticks + causal_hash(g_current_tick, team_id,
                                       CAUSAL_SALT_DURATION(fault_class)) %
                           ((uint32_t)max_ticks - min_ticks + 1u);

    return random_range_stream(STREAM_FAULTS, min_ticks, max_ticks);
}

static uint8_t fault_severity_for_class(OperationalFaultClass_t fault_class)
{
    uint8_t severity = g_fault_config.effect_strength[fault_class];

    if(severity == 0)
        severity = 1;

    return severity;
}

static void clear_fault_summary(TeamFaultState_t *fault_state, OperationalFaultClass_t fault_class)
{
    fault_state->active[fault_class] = 0;
    fault_state->start_tick[fault_class] = 0;
    fault_state->duration_ticks[fault_class] = 0;
    fault_state->recovery_tick[fault_class] = 0;
    fault_state->severity[fault_class] = 0;
    fault_state->target_team[fault_class] = NO_ASSIGNMENT;
    fault_state->target_x[fault_class] = 0;
    fault_state->target_y[fault_class] = 0;
    fault_state->original_travel_cost[fault_class] = 0;
    fault_state->original_road_blocked[fault_class] = 0;
    fault_state->original_navigable[fault_class] = 0;
    fault_state->pending_repair[fault_class] = 0;
    fault_state->repair_confirm_tick[fault_class] = 0;
    fault_state->target_incident_id[fault_class] = 0;
    /* This was the actual bug: active_mask only ever got bits SET (at
     * each inject_*_fault() call site), never cleared here on recovery.
     * Every other per-class field above correctly resets, but the
     * bitmask silently stayed stuck "dirty" from the first fault of
     * the whole run onward - corrupting fault_active_mask in the CSV
     * telemetry, debug.c's diagnostic prints, and recovery_manager's
     * change-detection signature (recovery_team_signature()). The
     * fault's actual mechanical effect was never affected by this -
     * everything that applies fault behavior reads the per-class
     * active[fault_class] flag directly, which correctly clears - only
     * downstream reporting/bookkeeping consumers of the aggregate
     * bitmask were silently wrong. */
    fault_state->active_mask &= ~(1u << fault_class);
    /* immune_until_tick[fault_class] is intentionally NOT reset here - it
     * must survive past this fault instance being cleared. */
}

static void mark_last_fault(TeamFaultState_t *fault_state, OperationalFaultClass_t fault_class)
{
    fault_state->last_fault_class = (uint8_t)fault_class;
    fault_state->last_fault_start_tick = fault_state->start_tick[fault_class];
    fault_state->last_fault_duration_ticks = fault_state->duration_ticks[fault_class];
    fault_state->last_fault_recovery_tick = fault_state->recovery_tick[fault_class];
    fault_state->last_fault_mask = fault_state->active_mask;
}

static void inject_mobility_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;

    if(fault_state->active[FAULT_CLASS_MOBILITY])
        return; /* already active — don't reset duration mid-fault */

    fault_state->active[FAULT_CLASS_MOBILITY] = 1;
    fault_state->start_tick[FAULT_CLASS_MOBILITY] = g_current_tick;
    fault_state->duration_ticks[FAULT_CLASS_MOBILITY] =
        fault_duration_for_class(team_id, FAULT_CLASS_MOBILITY);
    fault_state->recovery_tick[FAULT_CLASS_MOBILITY] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_MOBILITY];
    fault_state->severity[FAULT_CLASS_MOBILITY] =
        fault_severity_for_class(FAULT_CLASS_MOBILITY);
    fault_state->target_team[FAULT_CLASS_MOBILITY] = team_id;
    fault_state->active_mask |= (1u << FAULT_CLASS_MOBILITY);
    mark_last_fault(fault_state, FAULT_CLASS_MOBILITY);
    snapshot_record(g_sim_step_tick, EVENT_FAULT_INJECTED, team_id);
}

static void inject_communication_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;

    if(fault_state->active[FAULT_CLASS_COMMUNICATION])
        return;

    fault_state->active[FAULT_CLASS_COMMUNICATION] = 1;
    fault_state->start_tick[FAULT_CLASS_COMMUNICATION] = g_current_tick;
    fault_state->duration_ticks[FAULT_CLASS_COMMUNICATION] =
        fault_duration_for_class(team_id, FAULT_CLASS_COMMUNICATION);
    fault_state->recovery_tick[FAULT_CLASS_COMMUNICATION] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_COMMUNICATION];
    fault_state->severity[FAULT_CLASS_COMMUNICATION] =
        fault_severity_for_class(FAULT_CLASS_COMMUNICATION);
    fault_state->target_team[FAULT_CLASS_COMMUNICATION] = team_id;
    fault_state->active_mask |= (1u << FAULT_CLASS_COMMUNICATION);
    mark_last_fault(fault_state, FAULT_CLASS_COMMUNICATION);
    snapshot_record(g_sim_step_tick, EVENT_FAULT_INJECTED, team_id);
}

static void inject_resource_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;

    if(fault_state->active[FAULT_CLASS_RESOURCE])
        return;

    fault_state->active[FAULT_CLASS_RESOURCE] = 1;
    fault_state->start_tick[FAULT_CLASS_RESOURCE] = g_current_tick;
    fault_state->duration_ticks[FAULT_CLASS_RESOURCE] =
        fault_duration_for_class(team_id, FAULT_CLASS_RESOURCE);
    fault_state->recovery_tick[FAULT_CLASS_RESOURCE] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_RESOURCE];
    fault_state->severity[FAULT_CLASS_RESOURCE] =
        fault_severity_for_class(FAULT_CLASS_RESOURCE);
    fault_state->target_team[FAULT_CLASS_RESOURCE] = team_id;
    fault_state->active_mask |= (1u << FAULT_CLASS_RESOURCE);
    mark_last_fault(fault_state, FAULT_CLASS_RESOURCE);
    snapshot_record(g_sim_step_tick, EVENT_FAULT_INJECTED, team_id);
}

/* Environment faults edit the world grid, which only the laptop has
 * (environment.h, ENVIRONMENT_HAS_WORLD_STORAGE); fault_injector_step()
 * never runs on a board anyway. */
#if ENVIRONMENT_HAS_WORLD_STORAGE
/* M2: cell-level pre-fault baselines for road_blocked/navigable, the
 * same role env_fault_base_cost plays for travel_cost. They used to
 * live only in each team's fault_state, and a second fault landing on
 * an already-faulted cell (env_fault_ref_count > 0) snapshotted the
 * FAULTED values (road_blocked = 1). If that second fault was the last
 * to recover, it "restored" road_blocked = 1 and the cell stayed
 * blocked for the rest of the run. Written only on the 0 -> 1
 * ref_count transition and read only on the 1 -> 0 one, so no per-seed
 * reset is needed. Kept here rather than as EnvironmentCell_t fields
 * because environment.h is shared with the board build and every
 * mission-link/snapshot consumer of that struct. */
static uint8_t s_env_fault_base_road_blocked[GRID_WIDTH][GRID_HEIGHT];
static uint8_t s_env_fault_base_navigable[GRID_WIDTH][GRID_HEIGHT];

/* ------------------------------------------------------------------
 * Consequential roads (Step 3.1). Each damaged cell is either PARTIAL
 * debris (passable: a team stalls before stepping onto it, and A* sees the
 * cost in the cell's travel_cost) or BLOCKED (navigable = 0). Both live in
 * g_environment itself, so the map frames and the dashboard's STATICMAP
 * carry them with no wire change.
 * ------------------------------------------------------------------ */
#define ROAD_NONE    0u
#define ROAD_PARTIAL 1u
#define ROAD_BLOCKED 2u
static uint8_t s_road_kind[GRID_WIDTH][GRID_HEIGHT];
static CausalRoadStats_t s_road_stats;
static uint8_t s_label_now[NUM_TEAMS];
static uint8_t s_label_prev[NUM_TEAMS];

uint8_t fault_injector_causal_road_kind(int x, int y)
{
    if(x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT)
        return ROAD_NONE;
    return s_road_kind[x][y];
}

static int road_in_grid(int x, int y)
{
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}

/* Navigability for the connectivity checks. lift = 1 ignores road blocks
 * (the cell's pre-fault navigability); skip_* treats one more cell as
 * blocked (a block being considered). */
static int road_nav(int x, int y, int lift, int skip_x, int skip_y)
{
    if(!road_in_grid(x, y) || (x == skip_x && y == skip_y))
        return 0;
    if(lift && s_road_kind[x][y] == ROAD_BLOCKED)
        return s_env_fault_base_navigable[x][y] != 0;
    return g_environment[x][y].navigable != 0;
}

/* Cells that can reach a safe zone, searched outward from the safe zones
 * with the pathfinder's own move rules (8-way, no corner cutting). seen[]
 * is filled; returns the count. A safe zone's own cell is a source even if
 * it is not navigable. */
static int road_reach(uint8_t *seen, int lift, int skip_x, int skip_y)
{
    static const int DX[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
    static const int DY[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    static int queue[GRID_WIDTH * GRID_HEIGHT];
    int head = 0, tail = 0, count = 0;

    memset(seen, 0, (size_t)GRID_WIDTH * GRID_HEIGHT);

    for(uint8_t i = 0; i < MAX_SAFE_ZONES; i++)
    {
        int x, y;

        if(g_safezones[i].capacity == 0)
            continue;
        x = g_safezones[i].loc_x;
        y = g_safezones[i].loc_y;
        if(!road_in_grid(x, y) || seen[x * GRID_HEIGHT + y])
            continue;
        seen[x * GRID_HEIGHT + y] = 1;
        queue[tail++] = x * GRID_HEIGHT + y;
        count++;
    }

    while(head < tail)
    {
        int cur = queue[head++];
        int cx = cur / GRID_HEIGHT, cy = cur % GRID_HEIGHT;

        for(int d = 0; d < 8; d++)
        {
            int nx = cx + DX[d], ny = cy + DY[d];

            if(!road_nav(nx, ny, lift, skip_x, skip_y) || seen[nx * GRID_HEIGHT + ny])
                continue;
            if(DX[d] != 0 && DY[d] != 0 &&
               (!road_nav(nx, cy, lift, skip_x, skip_y) || !road_nav(cx, ny, lift, skip_x, skip_y)))
                continue;
            seen[nx * GRID_HEIGHT + ny] = 1;
            queue[tail++] = nx * GRID_HEIGHT + ny;
            count++;
        }
    }
    return count;
}

/* A full blockage is allowed only if every passable cell that could reach a
 * safe zone before still can afterwards (the blocked cell itself aside), and
 * the cell is not a place something has to be reached at. */
static int road_block_allowed(int x, int y)
{
    static uint8_t before[GRID_WIDTH * GRID_HEIGHT], after[GRID_WIDTH * GRID_HEIGHT];
    int n_before, n_after;

    for(uint8_t i = 0; i < MAX_SAFE_ZONES; i++)
        if(g_safezones[i].capacity != 0 && g_safezones[i].loc_x == x && g_safezones[i].loc_y == y)
            return 0;
    for(uint8_t i = 0; i < NUM_EVACUATION_GATEWAYS; i++)
        if(g_evacuation_gateways[i].loc_x == x && g_evacuation_gateways[i].loc_y == y)
            return 0;
    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
        if(g_incidents[i].slot_occupied && g_incidents[i].status != INC_RESOLVED &&
           g_incidents[i].loc_x == x && g_incidents[i].loc_y == y)
            return 0;
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
        if(g_system_tasks[i].slot_occupied && g_system_tasks[i].status != INC_RESOLVED &&
           g_system_tasks[i].loc_x == x && g_system_tasks[i].loc_y == y)
            return 0;

    n_before = road_reach(before, 0, -1, -1);
    n_after = road_reach(after, 0, x, y);
    return n_after == n_before - (before[x * GRID_HEIGHT + y] ? 1 : 0);
}

/* Every tick: no cell that could reach a safe zone with all road blocks
 * lifted may be cut off by them. Counts violations (the check target
 * asserts zero over a full run). */
static void road_audit(void)
{
    static uint8_t actual[GRID_WIDTH * GRID_HEIGHT], lifted[GRID_WIDTH * GRID_HEIGHT];
    uint32_t blocked_now = 0;

    road_reach(actual, 0, -1, -1);
    road_reach(lifted, 1, -1, -1);
    s_road_stats.audit_ticks++;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            int i = x * GRID_HEIGHT + y;

            if(s_road_kind[x][y] == ROAD_BLOCKED)
            {
                blocked_now++;
                continue;
            }
            if(lifted[i] && !actual[i])
                s_road_stats.audit_violations++;
        }
    }
    if(blocked_now > s_road_stats.max_blocked_cells)
        s_road_stats.max_blocked_cells = blocked_now;
}

/* Applies the cell damage for a new environment fault on (x, y). The first
 * owner decides partial vs blocked from a hash of (seed, tick, team). */
static void road_apply(uint8_t team_id, int x, int y, uint8_t severity)
{
    EnvironmentCell_t *cell = &g_environment[x][y];
    uint8_t kind;

    if(cell->env_fault_ref_count > 1)
        return;     /* another fault already damaged this cell; damage is per cell */

    kind = (causal_hash(g_current_tick, team_id, 600u) % 100u) < g_causal_roads.partial_pct
               ? ROAD_PARTIAL : ROAD_BLOCKED;
    if(kind == ROAD_BLOCKED && !road_block_allowed(x, y))
    {
        kind = ROAD_PARTIAL;
        s_road_stats.block_downgraded++;
    }
    s_road_kind[x][y] = kind;

    if(kind == ROAD_PARTIAL)
    {
        uint16_t add = g_causal_roads.route_cost_visible
                           ? (uint16_t)((uint16_t)g_causal_roads.crossing_delay_ticks * g_causal_roads.cost_per_tick)
                           : (uint16_t)severity;
        uint16_t cost = (uint16_t)cell->env_fault_base_cost + add;

        environment_set_cell_operational_state(x, y, cost > 255u ? 255u : (uint8_t)cost, cell->navigable, 1);
        s_road_stats.partial_injected++;
    }
    else
    {
        environment_set_cell_operational_state(x, y, cell->travel_cost, 0, 1);
        s_road_stats.blocked_injected++;
    }
}

/* Raises the cost of one cell on this team's behalf and starts its
 * environment fault. Split out of inject_environment_fault() unchanged so
 * the causal aftershock model can pick the cell itself. */
static void apply_environment_fault(uint8_t team_id, int next_x, int next_y)
{
    TeamState_t *team = &g_teams[team_id];
    TeamFaultState_t *fault_state = &team->fault_state;

    fault_state->active[FAULT_CLASS_ENVIRONMENT] = 1;
    fault_state->start_tick[FAULT_CLASS_ENVIRONMENT] = g_current_tick;
    fault_state->duration_ticks[FAULT_CLASS_ENVIRONMENT] = fault_duration_for_class(team_id, FAULT_CLASS_ENVIRONMENT);
    fault_state->recovery_tick[FAULT_CLASS_ENVIRONMENT] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_ENVIRONMENT];
    fault_state->severity[FAULT_CLASS_ENVIRONMENT] = fault_severity_for_class(FAULT_CLASS_ENVIRONMENT);
    fault_state->target_team[FAULT_CLASS_ENVIRONMENT] = team_id;
    fault_state->target_x[FAULT_CLASS_ENVIRONMENT] = (int16_t)next_x;
    fault_state->target_y[FAULT_CLASS_ENVIRONMENT] = (int16_t)next_y;

    EnvironmentCell_t *cell = &g_environment[next_x][next_y];

    /* Fix 2 (snapshot): only capture the true pre-fault ground truth when
     * this is the FIRST injection onto this cell (ref_count == 0). If
     * another team already injected here, the cell's cost is already
     * elevated; snapshotting that as "original" would make this team's
     * recovery restore to the elevated value instead of the real baseline.
     * The cell-level env_fault_base_cost holds the ground truth regardless
     * of how many subsequent teams pile on, so use that when available. */
    if(cell->env_fault_ref_count == 0)
    {
        cell->env_fault_base_cost = cell->travel_cost;
        s_env_fault_base_road_blocked[next_x][next_y] = cell->road_blocked;
        s_env_fault_base_navigable[next_x][next_y]    = cell->navigable;
    }

    /* First owner or not, record the cell-level (pre-all-faults) baseline:
     * reading cell->road_blocked/navigable here on a stacked fault would
     * capture the already-faulted state (M2). Recovery restores from the
     * cell-level baseline; these per-team copies are for telemetry. */
    fault_state->original_travel_cost[FAULT_CLASS_ENVIRONMENT]  = cell->env_fault_base_cost;
    fault_state->original_road_blocked[FAULT_CLASS_ENVIRONMENT] = s_env_fault_base_road_blocked[next_x][next_y];
    fault_state->original_navigable[FAULT_CLASS_ENVIRONMENT]    = s_env_fault_base_navigable[next_x][next_y];
    cell->env_fault_ref_count++;

    /* Fix 1 (cost-only): bump travel_cost but preserve navigable.
     * The previous code hard-wired navigable=0, which made A*'s
     * get_movement_cost() return -1 for this cell — a complete graph cut,
     * not a weighted detour. Teams routed to safe zones would find zero
     * reachable nodes when any chokepoint on their path was faulted.
     * road_blocked=1 is kept: it is used by scoring/telemetry only and
     * does not affect pathfinding. */
    uint8_t severity = fault_state->severity[FAULT_CLASS_ENVIRONMENT];
    if(fault_profile_causal_roads_on())
    {
        road_apply(team_id, next_x, next_y, severity);
    }
    else
    {
    uint8_t new_cost = cell->travel_cost + severity;
    if(new_cost < cell->travel_cost)   /* uint8_t overflow guard */
        new_cost = 255u;
    environment_set_cell_operational_state(
        next_x,
        next_y,
        new_cost,
        cell->navigable,   /* preserve — do not sever the graph */
        1);                /* road_blocked=1 for telemetry/scoring */
    }

    fault_state->active_mask |= (1u << FAULT_CLASS_ENVIRONMENT);
    mark_last_fault(fault_state, FAULT_CLASS_ENVIRONMENT);
    snapshot_record(g_sim_step_tick, EVENT_FAULT_INJECTED, team_id);
}

static void inject_environment_fault(uint8_t team_id)
{
    TeamState_t *team = &g_teams[team_id];
    int next_x = team->loc_x;
    int next_y = team->loc_y;

    /* M3: resolve the task through the pool it actually lives in. This
     * used to index g_incidents[] for ANY non-NONE pool, so a team on a
     * TASK_POOL_SYSTEM task (sweep, evacuation, equipment cache) got its
     * choke point computed toward whatever unrelated incident happened
     * to share that slot number (or none at all). */
    IncidentState_t *task = NULL;
    if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
       team->assigned_task_slot < NUM_INCIDENTS)
    {
        task = &g_incidents[team->assigned_task_slot];
    }
    else if(team->assigned_task_pool == TASK_POOL_SYSTEM &&
            team->assigned_task_slot < NUM_SYSTEM_TASKS)
    {
        task = &g_system_tasks[team->assigned_task_slot];
    }

    if(task != NULL)
    {
        if(task->slot_occupied &&
           find_next_path_step(team->loc_x, team->loc_y,
                               task->loc_x, task->loc_y,
                               0, &next_x, &next_y) > 0)
        {
            /* use the next path step as the choke point */
        }
    }

    apply_environment_fault(team_id, next_x, next_y);
}
#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */


static void inject_mission_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;
    IncidentState_t *task = NULL;

    /* Only ever target the team's own assigned incident. The previous
     * fallback ("grab the first open incident on the map") meant every
     * team whose current task wasn't an incident-pool task - which is
     * most teams, most of the time - would all independently target
     * the same shared incident. Since the active-fault guard is
     * per-team, multiple teams could each roll and land a mission
     * fault against that one incident within a few ticks, each
     * decrementing its priority and each capturing the already-
     * decremented value as their own "original" - compounding
     * LOW->CRITICAL in as few as 2-3 hits and corrupting recovery.
     * A team with no incident-pool assignment simply has nothing of
     * its own to fault right now. */
    if(g_teams[team_id].assigned_task_pool == TASK_POOL_INCIDENT &&
       g_teams[team_id].assigned_task_slot < NUM_INCIDENTS)
    {
        task = &g_incidents[g_teams[team_id].assigned_task_slot];
    }

    if(task == NULL)
        return;

    fault_state->active[FAULT_CLASS_MISSION] = 1;
    fault_state->start_tick[FAULT_CLASS_MISSION] = g_current_tick;
    fault_state->duration_ticks[FAULT_CLASS_MISSION] = fault_duration_for_class(team_id, FAULT_CLASS_MISSION);
    fault_state->recovery_tick[FAULT_CLASS_MISSION] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_MISSION];
    fault_state->severity[FAULT_CLASS_MISSION] = fault_severity_for_class(FAULT_CLASS_MISSION);
    fault_state->target_team[FAULT_CLASS_MISSION] = team_id;
    fault_state->target_x[FAULT_CLASS_MISSION]           = task->loc_x;
    fault_state->target_y[FAULT_CLASS_MISSION]           = task->loc_y;
    fault_state->target_incident_id[FAULT_CLASS_MISSION] = task->incident_id; 

    /* Causal profile: the fault only records that the team's incident is
     * deteriorating. The deterioration itself (victim loss, priority
     * escalation) is made by the survival/escalation model in
     * causal_mission_model_step(), so nothing is edited here. */
    if(fault_profile_is_causal())
    {
        fault_state->active_mask |= (1u << FAULT_CLASS_MISSION);
        mark_last_fault(fault_state, FAULT_CLASS_MISSION);
        snapshot_record(g_sim_step_tick, EVENT_FAULT_INJECTED, team_id);
        return;
    }

    /* Remember the incident's true pre-fault state exactly once - the
     * first time any mission fault lands on it while none is already
     * active. A second, simultaneous fault (e.g. a Support team
     * co-assigned to the same incident as Primary) must not overwrite
     * this with the already-decremented value. */
    if(task->mission_fault_active_count == 0)
    {
        task->mission_fault_original_priority = (uint8_t)task->priority;
        task->mission_fault_original_victim_count = task->victim_count;
    }
    task->mission_fault_active_count++;

    /* L7: a mission fault makes the incident WORSE (more victims, added
       just below), so it escalates one level toward PRIORITY_CRITICAL (1),
       the most urgent - not toward LOW, which is where this used to send
       it and made a deteriorating incident look less urgent. The value
       captured above as mission_fault_original_priority is what recovery
       restores, so the escalation is temporary. */
    if(task->priority > PRIORITY_CRITICAL)
        task->priority = (Priority_t)((uint8_t)task->priority - 1u);

    /* Cap victim_count with a post-add clamp, not a pre-add threshold */
    {
        uint8_t new_count = (uint8_t)(task->victim_count +
                             fault_state->severity[FAULT_CLASS_MISSION]);
        task->victim_count = (new_count > 25u) ? 25u : new_count;
    }

    fault_state->active_mask |= (1u << FAULT_CLASS_MISSION);
    mark_last_fault(fault_state, FAULT_CLASS_MISSION);
    snapshot_record(g_sim_step_tick, EVENT_FAULT_INJECTED, team_id);
}

#if ENVIRONMENT_HAS_WORLD_STORAGE
static void recover_environment_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;
    OperationalFaultClass_t fault_class = FAULT_CLASS_ENVIRONMENT;
    int x = fault_state->target_x[fault_class];
    int y = fault_state->target_y[fault_class];
    EnvironmentCell_t *cell = &g_environment[x][y];

    if(cell->env_fault_ref_count > 0)
        cell->env_fault_ref_count--;

    if(cell->env_fault_ref_count == 0)
    {
        /* Fix 2 (last owner): restore to the true pre-fault baseline that
         * was captured when the very first injection landed on this cell.
         * env_fault_base_cost is immune to subsequent injectors' snapshots,
         * so this always recovers to the correct pre-fault value regardless
         * of how many teams stacked faults here. */
        /* M2: road_blocked/navigable come from the same cell-level
         * baseline, not this team's own snapshot, which is the faulted
         * state whenever this team's fault was stacked on another. */
        environment_set_cell_operational_state(
            x, y,
            cell->env_fault_base_cost,
            s_env_fault_base_navigable[x][y],
            s_env_fault_base_road_blocked[x][y]);
        s_road_kind[x][y] = ROAD_NONE;
    }
    else if(fault_profile_causal_roads_on())
    {
        /* Consequential roads: the damage belongs to the cell, not to one
         * team's fault; it stays until the last fault on the cell ends. */
    }
    else
    {
        /* Fix 2 (not last owner): peel back only this team's severity
         * contribution. Other teams' cost bumps remain active, so subtracting
         * just our severity leaves the cell at the correct elevated-but-
         * partially-recovered cost. Guard against underflow — if somehow cost
         * is already at or below base, snap back to base rather than going
         * negative (uint8_t would wrap). */
        uint8_t severity = fault_state->severity[fault_class];
        uint8_t new_cost = (cell->travel_cost > severity)
                           ? (cell->travel_cost - severity)
                           : cell->env_fault_base_cost;

        /* Causal profile: subtracting this team's severity from a cost the
         * road-degradation hazard may have raised leaves a residual, so
         * rebuild the cost from the baseline plus the severities of the
         * faults that are still active on this cell. */
        if(fault_profile_is_causal())
        {
            uint16_t rebuilt = cell->env_fault_base_cost;

            for(uint8_t other = 0; other < NUM_TEAMS; other++)
            {
                const TeamFaultState_t *ofs = &g_teams[other].fault_state;

                if(other != team_id && ofs->active[fault_class] &&
                   ofs->target_x[fault_class] == x && ofs->target_y[fault_class] == y)
                    rebuilt = (uint16_t)(rebuilt + ofs->severity[fault_class]);
            }
            new_cost = rebuilt > 255u ? 255u : (uint8_t)rebuilt;
        }
        environment_set_cell_operational_state(
            x, y,
            new_cost,
            cell->navigable,
            cell->road_blocked);
    }

    fault_state->recovery_tick[fault_class] = g_current_tick;
    mark_last_fault(fault_state, fault_class);
    snapshot_record(g_sim_step_tick, EVENT_FAULT_RECOVERED, team_id);
    clear_fault_summary(fault_state, fault_class);
}
#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */


static void recover_mission_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;
    OperationalFaultClass_t fault_class = FAULT_CLASS_MISSION;
    uint32_t target_id = fault_state->target_incident_id[fault_class];

    /* Causal profile: nothing was edited on the incident at onset (the
     * survival model owns its state), so there is nothing to undo. */
    for(uint8_t i = 0; i < NUM_INCIDENTS && !fault_profile_is_causal(); i++)
    {
        IncidentState_t *task = &g_incidents[i];

        if(!task->slot_occupied)
            continue;

        /* Match by incident_id — location matching is unsafe because a
           resolved incident's cell can be reused by a new, unrelated one */
        if(task->incident_id != target_id)
            continue;

        if(task->mission_fault_active_count == 0)
            break; /* not tracking this incident — don't corrupt it */

        task->mission_fault_active_count--;

        /* Causal profile: deterioration is irreversible. A fault that
         * escalated the incident (priority up, victims added) does not
         * un-escalate when the fault record clears. */
        if(task->mission_fault_active_count == 0 && !fault_profile_is_causal())
        {
            task->priority     = (Priority_t)task->mission_fault_original_priority;
            task->victim_count = task->mission_fault_original_victim_count;
        }
        break;
    }

    fault_state->recovery_tick[fault_class] = g_current_tick;
    mark_last_fault(fault_state, fault_class);
    snapshot_record(g_sim_step_tick, EVENT_FAULT_RECOVERED, team_id);
    clear_fault_summary(fault_state, fault_class);
}

static void recover_fault(uint8_t team_id, OperationalFaultClass_t fault_class)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;

    switch(fault_class)
    {
        case FAULT_CLASS_MOBILITY:
        case FAULT_CLASS_COMMUNICATION:
        case FAULT_CLASS_RESOURCE:
            fault_state->recovery_tick[fault_class] = g_current_tick;
            mark_last_fault(fault_state, fault_class);
            snapshot_record(g_sim_step_tick, EVENT_FAULT_RECOVERED, team_id);
            clear_fault_summary(fault_state, fault_class);
            break;

        case FAULT_CLASS_ENVIRONMENT:
#if ENVIRONMENT_HAS_WORLD_STORAGE
            recover_environment_fault(team_id);
#endif
            break;

        case FAULT_CLASS_MISSION:
            recover_mission_fault(team_id);
            break;

        default:
            break;
    }
}

static void inject_fault(uint8_t team_id, OperationalFaultClass_t fault_class)
{
    switch(fault_class)
    {
        case FAULT_CLASS_MOBILITY:
            inject_mobility_fault(team_id);
            break;

        case FAULT_CLASS_COMMUNICATION:
            inject_communication_fault(team_id);
            break;

        case FAULT_CLASS_RESOURCE:
            inject_resource_fault(team_id);
            break;

        case FAULT_CLASS_ENVIRONMENT:
#if ENVIRONMENT_HAS_WORLD_STORAGE
            inject_environment_fault(team_id);
#endif
            break;

        case FAULT_CLASS_MISSION:
            inject_mission_fault(team_id);
            break;

        default:
            break;
    }
}

static uint8_t roll_injection(uint8_t probability)
{
    if(probability == 0)
        return 0;

    return random_range_stream(STREAM_FAULTS, 1, 100) <= probability;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int fault_injector_causal_symptom_skip(uint8_t team_id)
{
    const CausalFaultParams_t *k = &g_causal_params;

    if(!fault_profile_is_causal() || team_id >= NUM_TEAMS)
        return 0;
    if(s_causal_wear[team_id] <= k->wear_symptom_start)
        return 0;

    float p = clampf(k->wear_symptom_slope * (s_causal_wear[team_id] - k->wear_symptom_start),
                     0.0f, k->wear_symptom_max);
    return causal_unit(g_current_tick, team_id, 300u) < p;
}

/* The hazard/link/aftershock model uses libm and world-grid state, so it
 * is laptop-only (the board never injects faults and its link check
 * allows no libm references). fault_injector_causal_symptom_skip() above
 * needs neither and stays available everywhere. */
#if ENVIRONMENT_HAS_WORLD_STORAGE

/* ==================================================================
 * Causal fault model (FAULT_PROFILE_CAUSAL). See docs/CAUSAL_FAULT_MODEL.md
 * for every parameter and its source. Nothing below runs under another
 * profile, and nothing below draws from a shared STREAM_* generator.
 * ================================================================== */

static uint8_t causal_active_count(const TeamState_t *team)
{
    uint8_t n = 0;
    for(uint8_t c = 0; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
        n += team->fault_state.active[c] ? 1u : 0u;
    return n;
}

/* Standard-normal value for one lattice node, from a hash of (seed, node). */
static float causal_lattice_gauss(int i, int j)
{
    uint32_t key = (uint32_t)(i * 64 + j);
    float u1 = ((float)(causal_hash(key, 0xC0u, 400u) >> 8) + 1.0f) * (1.0f / 16777217.0f);
    float u2 = (float)(causal_hash(key, 0xC0u, 401u) >> 8) * (1.0f / 16777216.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(6.2831853f * u2);
}

/* Spatially correlated log-normal shadowing: hashed value noise. Independent
 * Gaussian values sit on a coarse lattice (spacing shadow_lattice_cells) and
 * are smoothly (smoothstep) interpolated, then divided by the root of the
 * summed squared weights so the marginal standard deviation stays
 * shadow_sigma_db everywhere. Nearby cells therefore share most of their
 * shadowing, as in a Gudmundson exponential-decorrelation model, and a dead
 * zone is a contiguous patch. Deterministic: a pure function of
 * (seed, cell), no RNG stream. */
static float causal_shadow_db(int x, int y)
{
    const CausalFaultParams_t *k = &g_causal_params;
    float fx = (float)x / k->shadow_lattice_cells;
    float fy = (float)y / k->shadow_lattice_cells;
    int i0 = (int)fx, j0 = (int)fy;
    float tx = fx - (float)i0, ty = fy - (float)j0;
    float sx = tx * tx * (3.0f - 2.0f * tx);
    float sy = ty * ty * (3.0f - 2.0f * ty);
    float wx[2] = { 1.0f - sx, sx };
    float wy[2] = { 1.0f - sy, sy };
    float sum = 0.0f, norm2 = 0.0f;

    for(int a = 0; a < 2; a++)
    {
        for(int b = 0; b < 2; b++)
        {
            float w = wx[a] * wy[b];
            sum += w * causal_lattice_gauss(i0 + a, j0 + b);
            norm2 += w * w;
        }
    }

    return k->shadow_sigma_db * sum / sqrtf(norm2);
}

/* Receiver-side link margin (dB) for a team standing on cell (x, y):
 *   margin = tx - body - PL(d) - shadow(cell) - building - rubble - rx_sens
 * Receivers are the evacuation gateways and the safe zones. Shadowing is a
 * zero-mean Gaussian drawn from a hash of (seed, cell), so a dead zone is
 * the same dead zone every tick and every visit. */
static float causal_link_margin_db(int x, int y)
{
    const CausalFaultParams_t *k = &g_causal_params;
    float best_d2 = 1e30f;

    for(uint8_t i = 0; i < NUM_EVACUATION_GATEWAYS; i++)
    {
        float dx = (float)(x - g_evacuation_gateways[i].loc_x);
        float dy = (float)(y - g_evacuation_gateways[i].loc_y);
        if(dx * dx + dy * dy < best_d2)
            best_d2 = dx * dx + dy * dy;
    }
    for(uint8_t i = 0; i < MAX_SAFE_ZONES; i++)
    {
        float dx, dy;
        if(g_safezones[i].capacity == 0)
            continue;
        dx = (float)(x - g_safezones[i].loc_x);
        dy = (float)(y - g_safezones[i].loc_y);
        if(dx * dx + dy * dy < best_d2)
            best_d2 = dx * dx + dy * dy;
    }

    float d_m = sqrtf(best_d2) * k->cell_size_m;
    if(d_m < 0.5f * k->cell_size_m)
        d_m = 0.5f * k->cell_size_m;
    float path_loss = k->pl0_db + 10.0f * k->path_loss_exponent * log10f(d_m / k->d0_m);

    float shadow = causal_shadow_db(x, y);

    float building = 0.0f, rubble = 0.0f;
#if ENVIRONMENT_HAS_WORLD_STORAGE
    if(x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT)
    {
        const EnvironmentCell_t *cell = &g_environment[x][y];
        building = k->building_penalty_db * (float)cell->district_density / 100.0f;
        rubble = k->rubble_penalty_db * (float)cell->infrastructure_damage / 100.0f;
    }
#endif

    /* Slow fading: a hash value per (cell, time window), interpolated
     * linearly between window boundaries so it drifts rather than jumps. */
    float window_pos = (float)g_current_tick / k->fade_window_ticks;
    uint32_t window = (uint32_t)window_pos;
    float frac = window_pos - (float)window;
    uint32_t cell_key = (uint32_t)(x * 64 + y);
    float f0 = 2.0f * causal_unit(window, cell_key, 410u) - 1.0f;
    float f1 = 2.0f * causal_unit(window + 1u, cell_key, 410u) - 1.0f;
    float fade = k->fade_amp_db * (f0 + (f1 - f0) * frac);

    return k->tx_power_dbm - k->body_loss_db - path_loss - shadow - fade - building - rubble -
           k->rx_sensitivity_dbm;
}

/* Roughness of the ground a team just entered, 0..1: slope, road damage,
 * structural damage and debris (a blocked road). */
static float causal_roughness(int x, int y)
{
#if ENVIRONMENT_HAS_WORLD_STORAGE
    if(x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT)
    {
        const EnvironmentCell_t *cell = &g_environment[x][y];
        return clampf(0.4f * (float)cell->terrain_slope / 100.0f +
                      0.3f * (float)cell->road_damage / 100.0f +
                      0.2f * (float)cell->infrastructure_damage / 100.0f +
                      (cell->road_blocked ? 0.1f : 0.0f), 0.0f, 1.0f);
    }
#else
    (void)x; (void)y;
#endif
    return 0.0f;
}

static void causal_register_onset(uint8_t team_id, OperationalFaultClass_t fault_class,
                                  CausalOnsetCause_t cause)
{
    if(!g_teams[team_id].fault_state.active[fault_class])
        return;

    s_causal_stats.onsets[fault_class]++;
    s_causal_truth[team_id].onset_cause[fault_class] = (uint8_t)cause;
    if(fault_class == FAULT_CLASS_MOBILITY)
        s_causal_repair_ticks[team_id] = 0;
    s_escalated[team_id][fault_class] = 0;
    if(fault_class == FAULT_CLASS_MOBILITY && fault_profile_causal_persist())
    {
        const CausalPersistParams_t *pp = &g_causal_persist_params;
        uint32_t span = (uint32_t)pp->base_dwell_max_ticks - pp->base_dwell_min_ticks + 1u;

        s_dwell_need[team_id] = (uint8_t)(pp->base_dwell_min_ticks +
                                          causal_hash(g_current_tick, team_id, CAUSAL_SALT_DWELL) % span);
    }
}

static void causal_clear(uint8_t team_id, OperationalFaultClass_t fault_class,
                         CausalClearCause_t how)
{
    TeamFaultState_t *fs = &g_teams[team_id].fault_state;

    if(how == CAUSAL_CLEAR_FIELD_REPAIR)
    {
        s_persist_stats.ticks_by_field[fault_class] += g_current_tick - fs->start_tick[fault_class];
    }
    else if(how == CAUSAL_CLEAR_CAUSE_REMOVED)
    {
        s_causal_stats.cleared_by_cause[fault_class]++;
        s_causal_stats.ticks_by_cause[fault_class] += g_current_tick - fs->start_tick[fault_class];
        s_causal_stats.planned_by_cause[fault_class] += fs->recovery_tick[fault_class] - fs->start_tick[fault_class];
    }
    else
        s_causal_stats.cleared_by_backstop[fault_class]++;
    s_causal_stats.active_ticks_total[fault_class] += g_current_tick - fs->start_tick[fault_class];
    s_causal_truth[team_id].clear_cause[fault_class] = (uint8_t)how;

    /* A field repair removes a fraction of the wear; a fault that merely
     * ran out its backstop leaves the wear (and so the hazard) where it was. */
    if(fault_class == FAULT_CLASS_MOBILITY &&
       (how == CAUSAL_CLEAR_CAUSE_REMOVED || how == CAUSAL_CLEAR_FIELD_REPAIR))
        s_causal_wear[team_id] *= 1.0f - g_causal_params.wear_repair_reduction;

    recover_fault(team_id, fault_class);
}

/* Has the cause of an active fault been removed? Communication returns 1
 * only when the margin is back above zero; there is no time backstop. */
static uint8_t causal_cause_removed(uint8_t team_id, OperationalFaultClass_t fault_class)
{
    const TeamState_t *team = &g_teams[team_id];

    switch(fault_class)
    {
        case FAULT_CLASS_RESOURCE:
            /* Rest/resupply: back at a safe zone with the kit restocked and
             * stamina above the level the team would be re-tasked at. */
            return team->status == STATUS_RESTING &&
                   team->equipment_level >= CAUSAL_RESOURCE_RESUPPLY_LEVEL &&
                   team->stamina >= get_soft_stamina_floor(team);

        case FAULT_CLASS_MOBILITY:
            /* Field repair: dwell at a safe zone (RESTING only ever
             * happens there). Progress is counted by the caller. */
            return s_causal_repair_ticks[team_id] >=
                   (fault_profile_causal_persist() ? s_dwell_need[team_id] : CAUSAL_MOBILITY_REPAIR_REST_TICKS);

        case FAULT_CLASS_COMMUNICATION:
            /* Hysteresis: the fault starts below 0 dB but only clears once
             * the margin is back above +comm_clear_hysteresis_db. */
            return s_causal_truth[team_id].link_margin_db > g_causal_params.comm_clear_hysteresis_db;

        case FAULT_CLASS_MISSION:
        {
            /* The incident is resolved, or this team is no longer on it. */
            uint8_t slot = team->assigned_task_slot;
            if(team->assigned_task_pool != TASK_POOL_INCIDENT || slot >= NUM_INCIDENTS)
                return 1;
            return !(g_incidents[slot].slot_occupied &&
                     g_incidents[slot].status != INC_RESOLVED &&
                     g_incidents[slot].incident_id == team->fault_state.target_incident_id[FAULT_CLASS_MISSION]);
        }

        default:
            return 0;
    }
}

#if ENVIRONMENT_HAS_WORLD_STORAGE
/* Walks the team's route (its own cell, then up to `lookahead` path steps
 * toward its destination) and returns 1 with the first cell inside the
 * aftershock radius. */
static int causal_route_hit(const TeamState_t *team, int cx, int cy, int *hit_x, int *hit_y)
{
    const CausalFaultParams_t *k = &g_causal_params;
    int x = team->loc_x, y = team->loc_y;
    int dest_x = x, dest_y = y;
    int have_dest = 0;

    if(team->status == STATUS_EN_ROUTE)
    {
        if(team->heading_to_rest && team->resting_zone_id < MAX_SAFE_ZONES)
        {
            dest_x = g_safezones[team->resting_zone_id].loc_x;
            dest_y = g_safezones[team->resting_zone_id].loc_y;
            have_dest = 1;
        }
        else
        {
            const IncidentState_t *task = NULL;
            if(team->assigned_task_pool == TASK_POOL_INCIDENT && team->assigned_task_slot < NUM_INCIDENTS)
                task = &g_incidents[team->assigned_task_slot];
            else if(team->assigned_task_pool == TASK_POOL_SYSTEM && team->assigned_task_slot < NUM_SYSTEM_TASKS)
                task = &g_system_tasks[team->assigned_task_slot];
            if(task != NULL && task->slot_occupied)
            {
                dest_x = task->loc_x;
                dest_y = task->loc_y;
                have_dest = 1;
            }
        }
    }

    for(uint8_t step = 0; step <= k->aftershock_route_lookahead; step++)
    {
        int dx = x - cx, dy = y - cy;
        if(dx < 0) dx = -dx;
        if(dy < 0) dy = -dy;
        if(dx + dy <= (int)k->aftershock_radius_cells)
        {
            *hit_x = x;
            *hit_y = y;
            return 1;
        }

        if(!have_dest || (x == dest_x && y == dest_y))
            break;
        {
            int nx = x, ny = y;
            if(find_next_path_step(x, y, dest_x, dest_y, 0, &nx, &ny) <= 0)
                break;
            x = nx;
            y = ny;
        }
    }
    return 0;
}

/* Omori-Utsu aftershock sequence: a world event at rate
 * R(t) = K * stress / (c + t)^p, t = ticks since the mainshock. Every team
 * whose route crosses the affected cells takes an environment fault there
 * (apply_environment_fault() raises the cell's cost; the existing
 * road-degradation hazard then keeps raising it while the road is flagged
 * blocked). */
static void causal_aftershock_step(void)
{
    const CausalFaultParams_t *k = &g_causal_params;
    float t = (float)g_current_tick + k->omori_t_offset_ticks;
    float rate = k->omori_k * g_fault_stress / powf(k->omori_c_ticks + t, k->omori_p);
    float p_event = 1.0f - expf(-clampf(rate, 0.0f, 4.0f));

    if(causal_unit(g_current_tick, 0xA5A5u, 100u) >= clampf(p_event, 0.0f, 0.5f))
        return;

    int cx = (int)(causal_hash(g_current_tick, 0xA5A5u, 101u) % GRID_WIDTH);
    int cy = (int)(causal_hash(g_current_tick, 0xA5A5u, 102u) % GRID_HEIGHT);

    s_causal_world.aftershock_events++;

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        TeamState_t *team = &g_teams[team_id];
        int hit_x, hit_y;

        if(team->fault_state.active[FAULT_CLASS_ENVIRONMENT] ||
           causal_active_count(team) >= k->max_concurrent_faults)
            continue;
        if(!causal_route_hit(team, cx, cy, &hit_x, &hit_y))
            continue;

        apply_environment_fault(team_id, hit_x, hit_y);
        causal_register_onset(team_id, FAULT_CLASS_ENVIRONMENT, CAUSAL_ONSET_AFTERSHOCK);
        s_causal_truth[team_id].aftershock_hit = 1;
        s_causal_world.aftershock_team_hits++;
    }
}
#endif

/* Entrapment survival and escalation. Each open incident ages in real
 * time (TICK_TIME_SEC per tick; independent of --fault-stress). Victims die
 * at the Weibull hazard fitted to MacIntyre 2006 (74% alive at 24 h, 22% at
 * 72 h) and the priority steps one level toward CRITICAL every
 * escalate_every_h. Both are irreversible. A team working a deteriorating
 * incident takes a mission fault. */
static void causal_mission_model_step(void)
{
    const CausalFaultParams_t *k = &g_causal_params;
    const float tick_h = (float)TICK_TIME_SEC / 3600.0f;

    for(uint8_t slot = 0; slot < NUM_INCIDENTS; slot++)
    {
        IncidentState_t *inc = &g_incidents[slot];
        uint8_t deteriorated = 0;

        if(!inc->slot_occupied || inc->status == INC_RESOLVED)
            continue;

        if(s_causal_inc_id[slot] != inc->incident_id)
        {
            s_causal_inc_id[slot] = inc->incident_id;
            s_causal_inc_escalations[slot] = 0;
            s_causal_world.victims_seen += inc->victim_count;
        }

        if(g_current_tick <= inc->reported_at_tick)
            continue;
        /* Real time: 1 tick = TICK_TIME_SEC. --fault-stress does not apply. */
        float age_h = (float)(g_current_tick - inc->reported_at_tick) * tick_h;

        if(inc->victim_count > 0)
        {
            float hazard_per_h = (k->survival_k / k->survival_lambda_h) *
                                 powf(age_h / k->survival_lambda_h, k->survival_k - 1.0f);
            float p_die = 1.0f - expf(-hazard_per_h * tick_h * causal_actions_survival_factor(slot));
            float expected = (float)inc->victim_count * p_die;
            uint8_t deaths = (uint8_t)expected;

            if(causal_unit(g_current_tick, inc->incident_id, 200u) < expected - (float)deaths)
                deaths++;
            if(deaths > inc->victim_count)
                deaths = inc->victim_count;
            if(deaths > 0)
            {
                inc->victim_count = (uint8_t)(inc->victim_count - deaths);
                s_causal_world.victims_lost += deaths;
                deteriorated = 1;
            }
        }

        {
            uint8_t levels = (uint8_t)clampf(age_h / k->escalate_every_h, 0.0f, 255.0f);
            while(s_causal_inc_escalations[slot] < levels)
            {
                s_causal_inc_escalations[slot]++;
                if(inc->priority > PRIORITY_CRITICAL)
                {
                    inc->priority = (Priority_t)((uint8_t)inc->priority - 1u);
                    s_causal_world.escalations++;
                    deteriorated = 1;
                }
            }
        }

        if(!deteriorated)
            continue;

        for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
        {
            TeamState_t *team = &g_teams[team_id];

            if(team->assigned_task_pool != TASK_POOL_INCIDENT || team->assigned_task_slot != slot ||
               team->fault_state.active[FAULT_CLASS_MISSION] ||
               causal_active_count(team) >= k->max_concurrent_faults)
                continue;

            inject_mission_fault(team_id);
            causal_register_onset(team_id, FAULT_CLASS_MISSION, CAUSAL_ONSET_DETERIORATION);
        }
    }
}

/* Per-tick bookkeeping of a team's hidden state: wear from distance and
 * ground roughness, time since rest, link margin. Returns the roughness of
 * the cell crossed this tick (0 when stationary). */
static float causal_track_team(uint8_t team_id)
{
    const CausalFaultParams_t *k = &g_causal_params;
    const TeamState_t *team = &g_teams[team_id];
    CausalTeamTruth_t *truth = &s_causal_truth[team_id];
    float rough_now = 0.0f;

    if(s_causal_prev_valid[team_id])
    {
        int moved = abs((int)team->loc_x - (int)s_causal_prev_x[team_id]) +
                    abs((int)team->loc_y - (int)s_causal_prev_y[team_id]);
        if(moved > 0)
        {
            if(moved > 2)
                moved = 2;
            rough_now = causal_roughness(team->loc_x, team->loc_y);
            s_causal_wear[team_id] += (float)moved * (k->wear_per_cell + k->wear_per_cell_rough * rough_now);
        }
    }
    s_causal_prev_x[team_id] = team->loc_x;
    s_causal_prev_y[team_id] = team->loc_y;
    s_causal_prev_valid[team_id] = 1;

    if(team->status == STATUS_RESTING)
    {
        s_causal_last_rest_tick[team_id] = g_current_tick;
        if(s_causal_rest_dwell[team_id] < 255u)
            s_causal_rest_dwell[team_id]++;

        /* A rest of at least the repair dwell with no mobility fault open
         * removes a smaller fraction of wear (routine maintenance). With a
         * fault open, the field repair above applies instead. */
        if(s_causal_rest_dwell[team_id] == CAUSAL_MOBILITY_REPAIR_REST_TICKS &&
           !team->fault_state.active[FAULT_CLASS_MOBILITY])
            s_causal_wear[team_id] *= 1.0f - k->wear_rest_reduction;
    }
    else
        s_causal_rest_dwell[team_id] = 0;

    truth->hidden_wear = s_causal_wear[team_id];
    truth->ticks_since_rest = g_current_tick - s_causal_last_rest_tick[team_id];
    truth->link_margin_db = causal_link_margin_db(team->loc_x, team->loc_y);
    return rough_now;
}


/* ------------------------------------------------------------------
 * Step 3.6 persistent faults (docs/PERSISTENT_FAULTS_PROPOSAL.md).
 * ------------------------------------------------------------------ */
static uint32_t causal_persist_natural_ticks(uint8_t team_id, OperationalFaultClass_t fault_class)
{
    float u = causal_unit(g_current_tick, team_id, CAUSAL_SALT_NATURAL(fault_class));
    float t = -g_causal_persist_params.natural_mean_ticks * logf(1.0f - u);

    if(t < 1.0f)
        t = 1.0f;
    if(t > 100000.0f)
        t = 100000.0f;
    return (uint32_t)t;
}

const CausalPersistStats_t *fault_injector_causal_persist_stats(void)
{
    return &s_persist_stats;
}

/* An unremedied fault can step up one severity level (mobility 3 to 4, resource 2 to 3), at the class's current
 * onset hazard times the existing cascade factor 1.5. */
static void causal_escalate(uint8_t team_id, OperationalFaultClass_t fault_class, float hazard)
{
    TeamFaultState_t *fs = &g_teams[team_id].fault_state;
    float p = 1.0f - expf(-g_causal_persist_params.escalation_mult * hazard);

    if(s_escalated[team_id][fault_class] || fs->severity[fault_class] >= 4u)
        return;
    if(causal_unit(g_current_tick, team_id, CAUSAL_SALT_ESCALATE(fault_class)) < p)
    {
        fs->severity[fault_class]++;
        s_escalated[team_id][fault_class] = 1;
        s_persist_stats.escalations[fault_class]++;
    }
}

static uint8_t field_team_in_service(const TeamState_t *team)
{
    return (uint8_t)((team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE) && !team->heading_to_rest);
}

int fault_injector_causal_field_start(uint8_t team_id, uint8_t kind)
{
    TeamState_t *team;
    const CausalPersistParams_t *pp = &g_causal_persist_params;
    uint32_t span;

    if(!fault_profile_causal_field_repair() || team_id >= NUM_TEAMS || kind < 1u || kind > 3u)
        return 0;
    team = &g_teams[team_id];
    if(s_field_kind[team_id] != 0u || !field_team_in_service(team))
        return 0;
    if(kind == 1u && !team->fault_state.active[FAULT_CLASS_MOBILITY])
        return 0;
    if(kind == 2u && !team->fault_state.active[FAULT_CLASS_RESOURCE])
        return 0;

    span = (uint32_t)pp->field_max_ticks - pp->field_min_ticks + 1u;
    s_field_kind[team_id] = kind;
    s_field_end[team_id] = g_current_tick + pp->field_min_ticks +
                           causal_hash(g_current_tick, team_id, CAUSAL_SALT_FIELD_TIME) % span;
    s_persist_stats.field_started[kind - 1u]++;
    return 1;
}

uint8_t fault_injector_causal_field_state(uint8_t team_id)
{
    return team_id < NUM_TEAMS ? s_field_kind[team_id] : 0u;
}

int fault_injector_causal_team_held(uint8_t team_id)
{
    return team_id < NUM_TEAMS && s_field_kind[team_id] != 0u;
}

/* Runs at the start of the tick, before movement and work. */
static void causal_field_step(uint8_t team_id)
{
    TeamState_t *team = &g_teams[team_id];
    CausalTeamTruth_t *truth = &s_causal_truth[team_id];
    uint8_t kind = s_field_kind[team_id];
    uint8_t valid;
    OperationalFaultClass_t cls = (kind == 1u) ? FAULT_CLASS_MOBILITY : FAULT_CLASS_RESOURCE;

    truth->field_state = 0;
    if(kind == 0u)
        return;
    valid = field_team_in_service(team);
    if(kind != 3u && !team->fault_state.active[cls])
        valid = 0;
    if(!valid)
    {
        s_persist_stats.field_cancelled[kind - 1u]++;
        s_field_kind[team_id] = 0;
        return;
    }

    causal_actions_note_hold(team_id);
    s_persist_stats.field_ticks++;
    truth->field_state = kind;

    if(g_current_tick >= s_field_end[team_id])
    {
        uint8_t ok = 1;

        if(kind != 3u)
            ok = (uint8_t)(causal_unit(g_current_tick, team_id, CAUSAL_SALT_FIELD_OK) <
                           g_causal_persist_params.field_success);
        if(ok)
        {
            if(kind == 1u)
                causal_clear(team_id, FAULT_CLASS_MOBILITY, CAUSAL_CLEAR_FIELD_REPAIR);
            else if(kind == 2u)
            {
                if(team->equipment_level < CAUSAL_RESOURCE_RESUPPLY_LEVEL)
                    team->equipment_level = CAUSAL_RESOURCE_RESUPPLY_LEVEL;
                causal_clear(team_id, FAULT_CLASS_RESOURCE, CAUSAL_CLEAR_FIELD_REPAIR);
            }
            else
                s_causal_wear[team_id] *= 1.0f - g_causal_persist_params.maintenance_wear_cut;
            s_persist_stats.field_ok[kind - 1u]++;
        }
        else
            s_persist_stats.field_failed[kind - 1u]++;
        s_field_kind[team_id] = 0;
    }
}

void fault_injector_causal_prevent_order(uint8_t team_id)
{
    if(!fault_profile_causal_persist() || team_id >= NUM_TEAMS || s_prevent_order[team_id])
        return;
    s_prevent_order[team_id] = 1;
    s_prevent_tick[team_id] = g_current_tick;
    s_persist_stats.prevent_orders++;
}

/* A PREVENT order keeps the team RESTING until the routine-maintenance dwell has removed wear. */
int fault_injector_causal_rest_hold(uint8_t team_id)
{
    return team_id < NUM_TEAMS && s_prevent_order[team_id] &&
           s_causal_rest_dwell[team_id] < CAUSAL_MOBILITY_REPAIR_REST_TICKS;
}

static void causal_prevent_step(uint8_t team_id)
{
    const TeamState_t *team = &g_teams[team_id];

    if(!s_prevent_order[team_id])
        return;
    if(team->status == STATUS_RESTING && s_causal_rest_dwell[team_id] >= CAUSAL_MOBILITY_REPAIR_REST_TICKS)
    {
        s_prevent_order[team_id] = 0;
        s_persist_stats.prevent_done++;
    }
    else if(g_current_tick - s_prevent_tick[team_id] > 400u)
        s_prevent_order[team_id] = 0;   /* the team never got home (re-tasked or stuck): the order lapses */
}

/* Hazard-oracle forecast from the hidden state: P(onset within H ticks) = 1 - exp(-H * h). */
void fault_injector_causal_forecast(uint8_t team_id, uint8_t after, float out[2])
{
    const CausalFaultParams_t *k = &g_causal_params;
    const TeamState_t *team;
    float wear, depletion, since_rest, cascade, h_mob, h_res, horizon;

    out[0] = out[1] = 0.0f;
    if(!fault_profile_is_causal() || team_id >= NUM_TEAMS)
        return;
    team = &g_teams[team_id];
    horizon = g_causal_persist_params.forecast_horizon_ticks;
    wear = s_causal_wear[team_id];
    since_rest = (float)s_causal_truth[team_id].ticks_since_rest;
    depletion = 1.0f - (float)team->equipment_level / (float)EQ_MAX_LEVEL;
    cascade = team->fault_state.active[FAULT_CLASS_MOBILITY] ? k->res_mobility_cascade : 1.0f;
    if(after == 1u)
    {
        wear *= 1.0f - k->wear_rest_reduction;
        since_rest = 0.0f;
        depletion = 0.0f;
        cascade = 1.0f;
    }
    else if(after == 2u)
        wear *= 1.0f - g_causal_persist_params.maintenance_wear_cut;

    h_mob = clampf((k->mob_hazard_base + k->mob_hazard_per_wear * wear + k->mob_hazard_rough * s_rough_last[team_id]) *
                       g_fault_stress, 0.0f, k->hazard_cap);
    h_res = clampf((k->res_hazard_base + k->res_hazard_battery * depletion * depletion +
                    k->res_hazard_since_rest * clampf(since_rest / k->res_since_rest_horizon, 0.0f, 2.0f)) *
                       cascade * g_fault_stress, 0.0f, k->hazard_cap);
    out[0] = 1.0f - expf(-horizon * h_mob);
    out[1] = 1.0f - expf(-horizon * h_res);
}

static void causal_step(void)
{
    const CausalFaultParams_t *k = &g_causal_params;

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        for(uint8_t c = 0; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
        {
            s_causal_truth[t].onset_cause[c] = CAUSAL_ONSET_NONE;
            s_causal_truth[t].clear_cause[c] = CAUSAL_CLEAR_NONE;
        }
        s_causal_truth[t].aftershock_hit = 0;
    }

#if ENVIRONMENT_HAS_WORLD_STORAGE
    causal_aftershock_step();
#endif
    causal_mission_model_step();

    if(fault_profile_causal_persist())
    {
        for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
        {
            causal_field_step(team_id);
            causal_prevent_step(team_id);
        }
    }

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        TeamState_t *team = &g_teams[team_id];
        CausalTeamTruth_t *truth = &s_causal_truth[team_id];
        float rough_now = causal_track_team(team_id);
        s_rough_last[team_id] = rough_now;

        /* Hazards, per tick, from state plus a small baseline. */
        float t_base = k->mob_hazard_base;
        float t_wear = k->mob_hazard_per_wear * s_causal_wear[team_id];
        float t_rough = k->mob_hazard_rough * rough_now;
        float h_mob = clampf((t_base + t_wear + t_rough) * g_fault_stress, 0.0f, k->hazard_cap);

        float depletion = 1.0f - (float)team->equipment_level / (float)EQ_MAX_LEVEL;
        float r_base = k->res_hazard_base;
        float r_batt = k->res_hazard_battery * depletion * depletion;
        float r_rest = k->res_hazard_since_rest *
                       clampf((float)truth->ticks_since_rest / k->res_since_rest_horizon, 0.0f, 2.0f);
        float cascade = team->fault_state.active[FAULT_CLASS_MOBILITY] ? k->res_mobility_cascade : 1.0f;
        float h_res = clampf((r_base + r_batt + r_rest) * cascade * g_fault_stress, 0.0f, k->hazard_cap);

        truth->hazard_mobility = h_mob;
        truth->hazard_resource = h_res;
        for(uint8_t c = 0; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
            truth->escalated[c] = s_escalated[team_id][c];
        if(fault_profile_causal_persist())
        {
            float risk[2];

            fault_injector_causal_forecast(team_id, 0u, risk);
            truth->risk_mobility = risk[0];
            truth->risk_resource = risk[1];
        }

        for(uint8_t fault_class = 0; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
        {
            OperationalFaultClass_t class_id = (OperationalFaultClass_t)fault_class;

            if(team->fault_state.active[fault_class])
            {
                s_causal_stats.exposure_ticks[fault_class]++;

                if(class_id == FAULT_CLASS_MOBILITY && team->status == STATUS_RESTING &&
                   s_causal_repair_ticks[team_id] < 255u)
                    s_causal_repair_ticks[team_id]++;

                if(class_id == FAULT_CLASS_COMMUNICATION)
                {
                    float deficit = -truth->link_margin_db;
                    if(causal_cause_removed(team_id, class_id))
                        causal_clear(team_id, class_id, CAUSAL_CLEAR_CAUSE_REMOVED);
                    else
                        team->fault_state.severity[fault_class] =
                            (uint8_t)clampf(1.0f + deficit / k->severity_db_per_level, 1.0f, 4.0f);
                    continue;
                }

                /* Cause removed, or the old duration as a maximum backstop.
                 * No PAR confirmation delay and no post-repair immunity. */
                if(causal_cause_removed(team_id, class_id))
                    causal_clear(team_id, class_id, CAUSAL_CLEAR_CAUSE_REMOVED);
                else if(g_current_tick >= team->fault_state.recovery_tick[fault_class])
                    causal_clear(team_id, class_id, CAUSAL_CLEAR_BACKSTOP);
                else if(fault_profile_causal_persist() &&
                        (class_id == FAULT_CLASS_MOBILITY || class_id == FAULT_CLASS_RESOURCE))
                    causal_escalate(team_id, class_id, class_id == FAULT_CLASS_MOBILITY ? h_mob : h_res);
                continue;
            }

            switch(class_id)
            {
                case FAULT_CLASS_COMMUNICATION:
                    /* Deterministic: no dice, no concurrency cap. */
                    if(truth->link_margin_db < 0.0f)
                    {
                        inject_communication_fault(team_id);
                        team->fault_state.severity[fault_class] =
                            (uint8_t)clampf(1.0f - truth->link_margin_db / k->severity_db_per_level, 1.0f, 4.0f);
                        causal_register_onset(team_id, class_id, CAUSAL_ONSET_LINK_MARGIN);
                    }
                    break;

                case FAULT_CLASS_MOBILITY:
                    if(causal_active_count(team) < k->max_concurrent_faults &&
                       causal_unit(g_current_tick, team_id, CAUSAL_SALT_ONSET(class_id)) <
                           1.0f - expf(-h_mob))
                    {
                        CausalOnsetCause_t why = CAUSAL_ONSET_BASELINE;
                        float best = t_base;
                        if(t_wear > best) { best = t_wear; why = CAUSAL_ONSET_WEAR; }
                        if(t_rough > best) { why = CAUSAL_ONSET_ROUGHNESS; }
                        inject_mobility_fault(team_id);
                        causal_register_onset(team_id, class_id, why);
                    }
                    break;

                case FAULT_CLASS_RESOURCE:
                    if(causal_active_count(team) < k->max_concurrent_faults &&
                       causal_unit(g_current_tick, team_id, CAUSAL_SALT_ONSET(class_id)) <
                           1.0f - expf(-h_res))
                    {
                        CausalOnsetCause_t why = CAUSAL_ONSET_BASELINE;
                        float best = r_base;
                        if(r_batt > best) { best = r_batt; why = CAUSAL_ONSET_BATTERY; }
                        if(r_rest > best) { why = CAUSAL_ONSET_SINCE_REST; }
                        inject_resource_fault(team_id);
                        causal_register_onset(team_id, class_id, why);
                    }
                    break;

                default:
                    /* Environment and mission onsets are world/model events
                     * (aftershocks, incident deterioration), raised above. */
                    break;
            }
        }
    }

    /* Consequential roads: audit global connectivity after this tick's
     * damage, and record which teams' routes touch damaged cells (sidecar
     * truth only). */
    if(fault_profile_causal_roads_on())
        road_audit();
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        uint8_t label;

        s_causal_truth[team_id].route_touch = causal_actions_route_touch(&g_teams[team_id]);
        label = s_causal_truth[team_id].route_touch != 0u;
        s_label_now[team_id] = label;
        if(label)
        {
            s_road_stats.label_ticks++;
            if(!s_label_prev[team_id])
                s_road_stats.label_episodes++;
        }
        s_label_prev[team_id] = label;
    }
}

uint8_t fault_injector_causal_env_label(uint8_t team_id)
{
    if(team_id >= NUM_TEAMS)
        return 0u;
    if(fault_profile_causal_fix(CAUSAL_FIX_LABEL))
        return s_label_now[team_id];
    return g_teams[team_id].fault_state.active[FAULT_CLASS_ENVIRONMENT];
}

/* CLEAR_ROAD completion: the cell goes back to its pre-fault state and every
 * team's environment fault on it ends. Restoring from the baseline in one
 * step (rather than owner by owner) leaves no residual cost. Returns how
 * many team faults ended. */
int fault_injector_clear_cell_faults(int x, int y)
{
    EnvironmentCell_t *cell;
    int ended = 0;

    if(x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT)
        return 0;
    cell = &g_environment[x][y];
    if(cell->env_fault_ref_count == 0)
        return 0;

    environment_set_cell_operational_state(
        x, y,
        cell->env_fault_base_cost,
        s_env_fault_base_navigable[x][y],
        s_env_fault_base_road_blocked[x][y]);
    cell->env_fault_ref_count = 0;
    s_road_kind[x][y] = ROAD_NONE;

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        TeamFaultState_t *fs = &g_teams[team_id].fault_state;

        if(!fs->active[FAULT_CLASS_ENVIRONMENT] ||
           fs->target_x[FAULT_CLASS_ENVIRONMENT] != x || fs->target_y[FAULT_CLASS_ENVIRONMENT] != y)
            continue;

        s_causal_stats.cleared_by_cause[FAULT_CLASS_ENVIRONMENT]++;
        s_causal_stats.ticks_by_cause[FAULT_CLASS_ENVIRONMENT] += g_current_tick - fs->start_tick[FAULT_CLASS_ENVIRONMENT];
        s_causal_stats.planned_by_cause[FAULT_CLASS_ENVIRONMENT] += fs->recovery_tick[FAULT_CLASS_ENVIRONMENT] - fs->start_tick[FAULT_CLASS_ENVIRONMENT];
        s_causal_stats.active_ticks_total[FAULT_CLASS_ENVIRONMENT] +=
            g_current_tick - fs->start_tick[FAULT_CLASS_ENVIRONMENT];
        s_causal_truth[team_id].clear_cause[FAULT_CLASS_ENVIRONMENT] = (uint8_t)CAUSAL_CLEAR_CAUSE_REMOVED;

        fs->recovery_tick[FAULT_CLASS_ENVIRONMENT] = g_current_tick;
        mark_last_fault(fs, FAULT_CLASS_ENVIRONMENT);
        snapshot_record(g_sim_step_tick, EVENT_FAULT_RECOVERED, team_id);
        clear_fault_summary(fs, FAULT_CLASS_ENVIRONMENT);
        ended++;
    }

    return ended;
}

const CausalRoadStats_t *fault_injector_causal_road_stats(void)
{
    return &s_road_stats;
}

float fault_injector_causal_link_margin_at(int x, int y)
{
    return causal_link_margin_db(x, y);
}

float fault_injector_causal_shadow_at(int x, int y)
{
    return causal_shadow_db(x, y);
}

#else /* !ENVIRONMENT_HAS_WORLD_STORAGE: the board never injects causal faults */

static uint32_t causal_persist_natural_ticks(uint8_t team_id, OperationalFaultClass_t fault_class)
{
    (void)team_id; (void)fault_class;
    return 0;
}
const CausalPersistStats_t *fault_injector_causal_persist_stats(void) { return &s_persist_stats; }
int fault_injector_causal_field_start(uint8_t team_id, uint8_t kind) { (void)team_id; (void)kind; return 0; }
uint8_t fault_injector_causal_field_state(uint8_t team_id) { (void)team_id; return 0u; }
int fault_injector_causal_team_held(uint8_t team_id) { (void)team_id; return 0; }
void fault_injector_causal_prevent_order(uint8_t team_id) { (void)team_id; }
int fault_injector_causal_rest_hold(uint8_t team_id) { (void)team_id; return 0; }
void fault_injector_causal_forecast(uint8_t team_id, uint8_t after, float out[2])
{
    (void)team_id; (void)after;
    out[0] = out[1] = 0.0f;
}

#endif /* ENVIRONMENT_HAS_WORLD_STORAGE (causal model) */

void fault_injector_init(uint32_t seed)
{
    g_fault_seed = seed;
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        s_causal_repair_ticks[t] = 0;
        s_causal_rest_dwell[t] = 0;
        s_causal_wear[t] = 0.0f;
        s_causal_prev_x[t] = 0;
        s_causal_prev_y[t] = 0;
        s_causal_prev_valid[t] = 0;
        s_causal_last_rest_tick[t] = 0;
        s_field_kind[t] = 0;
        s_field_end[t] = 0;
        s_dwell_need[t] = 0;
        s_prevent_order[t] = 0;
        s_prevent_tick[t] = 0;
        s_rough_last[t] = 0.0f;
        memset(s_escalated[t], 0, sizeof(s_escalated[t]));
        memset(&s_causal_truth[t], 0, sizeof(s_causal_truth[t]));
#if ENVIRONMENT_HAS_WORLD_STORAGE
        s_label_now[t] = 0;
        s_label_prev[t] = 0;
#endif
    }
    memset(&s_persist_stats, 0, sizeof(s_persist_stats));
    allocator_exclusion_reset();
    causal_actions_reset();
    planner_reset();
#if ENVIRONMENT_HAS_WORLD_STORAGE
    memset(s_road_kind, 0, sizeof(s_road_kind));
#endif
    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        s_causal_inc_id[i] = 0;
        s_causal_inc_escalations[i] = 0;
    }
    random_seed_stream(STREAM_FAULTS, derive_seed(seed, STREAM_FAULTS));
}

void fault_injector_set_profile(FaultProfile_t profile)
{
    fault_config_apply_profile(profile);
}

FaultProfile_t fault_injector_get_profile(void)
{
    return g_fault_config.profile;
}

void fault_injector_step(void)
{
    if(!g_fault_config.enabled)
        return;

    (void)g_fault_seed;

#if ENVIRONMENT_HAS_WORLD_STORAGE
    if(fault_profile_is_causal())
    {
        causal_step();
        return;
    }
#endif

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        TeamState_t *team = &g_teams[team_id];

        for(uint8_t fault_class = 0; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
        {
            OperationalFaultClass_t class_id = (OperationalFaultClass_t)fault_class;

            if(team->fault_state.active[fault_class])
            {
                /* Fix 3: environment faults bypass the two-stage confirm delay
                 * entirely. The 33-tick PAR-interval delay models human
                 * operator sign-off — appropriate for logical faults (mobility,
                 * comms, resource) where accountability reporting makes sense.
                 * Physical grid state should not stay walled for an extra
                 * 33 ticks after the fault duration expires: that inflated the
                 * effective block window from the configured 5–12 ticks to
                 * 38–45, causing the combined profile to disconnect the graph
                 * far more than intended. Recover environment faults directly
                 * at recovery_tick, then fall through to the shared immunity. */
                if(class_id == FAULT_CLASS_ENVIRONMENT &&
                   g_current_tick >= team->fault_state.recovery_tick[fault_class])
                {
                    recover_fault(team_id, class_id);
                    team->fault_state.immune_until_tick[fault_class] =
                        g_current_tick + FAULT_POST_REPAIR_IMMUNITY_TICKS;
                    continue;
                }

                /* Stage 1: the underlying technical fix is complete at
                 * recovery_tick, but the fault stays fully active until a
                 * separate confirmation delay elapses - modeling that nothing
                 * downstream treats it as resolved until the next PAR-style
                 * accountability check-in (the dashboard's "final say"). */
                if(!team->fault_state.pending_repair[fault_class] &&
                   g_current_tick >= team->fault_state.recovery_tick[fault_class])
                {
                    team->fault_state.pending_repair[fault_class] = 1;
                    team->fault_state.repair_confirm_tick[fault_class] =
                        g_current_tick + FAULT_REPAIR_CONFIRM_DELAY_TICKS;
                }

                /* Stage 2: confirmation delay elapsed - actually clear the
                 * fault now, and start this team's immunity window for this
                 * fault class. */
                if(team->fault_state.pending_repair[fault_class] &&
                   g_current_tick >= team->fault_state.repair_confirm_tick[fault_class])
                {
                    recover_fault(team_id, class_id);
                    team->fault_state.immune_until_tick[fault_class] =
                        g_current_tick + FAULT_POST_REPAIR_IMMUNITY_TICKS;
                }

                continue;
            }

            if(g_current_tick < team->fault_state.immune_until_tick[fault_class])
                continue;

            if(!roll_injection(g_fault_config.class_probability[fault_class]))
                continue;

            inject_fault(team_id, class_id);
        }
    }
}

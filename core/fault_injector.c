#include "fault_injector.h"

#include <stdbool.h>
#include <stddef.h>
#include "allocator.h"
#include "environment.h"
#include "incident.h"
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
static uint8_t s_causal_onset_x[NUM_TEAMS];
static uint8_t s_causal_onset_y[NUM_TEAMS];
static uint8_t s_causal_repair_ticks[NUM_TEAMS];

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

const CausalFaultStats_t *fault_injector_causal_stats(void)
{
    return &s_causal_stats;
}

static uint32_t fault_duration_for_class(uint8_t team_id, OperationalFaultClass_t fault_class)
{
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

static void inject_environment_fault(uint8_t team_id)
{
    TeamState_t *team = &g_teams[team_id];
    TeamFaultState_t *fault_state = &team->fault_state;
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
    uint8_t new_cost = cell->travel_cost + severity;
    if(new_cost < cell->travel_cost)   /* uint8_t overflow guard */
        new_cost = 255u;
    environment_set_cell_operational_state(
        next_x,
        next_y,
        new_cost,
        cell->navigable,   /* preserve — do not sever the graph */
        1);                /* road_blocked=1 for telemetry/scoring */

    fault_state->active_mask |= (1u << FAULT_CLASS_ENVIRONMENT);
    mark_last_fault(fault_state, FAULT_CLASS_ENVIRONMENT);
    snapshot_record(g_sim_step_tick, EVENT_FAULT_INJECTED, team_id);
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

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
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

static uint8_t roll_injection(uint8_t team_id, uint8_t fault_class, uint8_t probability)
{
    if(probability == 0)
        return 0;

    if(fault_profile_is_causal())
        /* per-mille under the causal profile */
        return (1u + causal_hash(g_current_tick, team_id,
                                 CAUSAL_SALT_ONSET(fault_class)) % 1000u) <= probability;

    return random_range_stream(STREAM_FAULTS, 1, 100) <= probability;
}

/* Has the cause of this fault been removed? Mobility/communication/
 * resource only; environment and mission have no actions that remove
 * their cause yet. */
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
            return s_causal_repair_ticks[team_id] >= CAUSAL_MOBILITY_REPAIR_REST_TICKS;

        case FAULT_CLASS_COMMUNICATION:
        {
            /* Reposition: out of the cell where the link dropped. */
            int dx = (int)team->loc_x - (int)s_causal_onset_x[team_id];
            int dy = (int)team->loc_y - (int)s_causal_onset_y[team_id];
            if(dx < 0) dx = -dx;
            if(dy < 0) dy = -dy;
            return (unsigned)(dx + dy) >= CAUSAL_COMM_REPOSITION_CELLS;
        }

        default:
            return 0;
    }
}

static void causal_note_onset(uint8_t team_id, OperationalFaultClass_t fault_class)
{
    if(!g_teams[team_id].fault_state.active[fault_class])
        return;

    s_causal_stats.onsets[fault_class]++;
    if(fault_class == FAULT_CLASS_COMMUNICATION)
    {
        s_causal_onset_x[team_id] = (uint8_t)g_teams[team_id].loc_x;
        s_causal_onset_y[team_id] = (uint8_t)g_teams[team_id].loc_y;
    }
    if(fault_class == FAULT_CLASS_MOBILITY)
        s_causal_repair_ticks[team_id] = 0;
}

void fault_injector_init(uint32_t seed)
{
    g_fault_seed = seed;
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        s_causal_onset_x[t] = 0;
        s_causal_onset_y[t] = 0;
        s_causal_repair_ticks[t] = 0;
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

    const uint8_t causal = (uint8_t)fault_profile_is_causal();

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        TeamState_t *team = &g_teams[team_id];

        for(uint8_t fault_class = 0; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
        {
            OperationalFaultClass_t class_id = (OperationalFaultClass_t)fault_class;

            if(team->fault_state.active[fault_class])
            {
                if(causal)
                {
                    s_causal_stats.exposure_ticks[fault_class]++;

                    if(class_id == FAULT_CLASS_MOBILITY && team->status == STATUS_RESTING &&
                       s_causal_repair_ticks[team_id] < 255u)
                        s_causal_repair_ticks[team_id]++;

                    if(class_id == FAULT_CLASS_MOBILITY ||
                       class_id == FAULT_CLASS_COMMUNICATION ||
                       class_id == FAULT_CLASS_RESOURCE)
                    {
                        /* Cleared by its cause, or at the old duration as a
                         * maximum backstop. No PAR confirmation delay and no
                         * post-repair immunity: nothing holds the fault open
                         * once the cause is gone, and nothing protects the
                         * team from the next one. */
                        uint8_t by_cause = causal_cause_removed(team_id, class_id);
                        uint8_t by_backstop = g_current_tick >= team->fault_state.recovery_tick[fault_class];

                        if(by_cause || by_backstop)
                        {
                            if(by_cause)
                                s_causal_stats.cleared_by_cause[fault_class]++;
                            else
                                s_causal_stats.cleared_by_backstop[fault_class]++;
                            s_causal_stats.active_ticks_total[fault_class] +=
                                g_current_tick - team->fault_state.start_tick[fault_class];
                            recover_fault(team_id, class_id);
                        }
                        continue;
                    }
                }

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
                    if(causal)
                    {
                        s_causal_stats.cleared_by_backstop[fault_class]++;
                        s_causal_stats.active_ticks_total[fault_class] +=
                            g_current_tick - team->fault_state.start_tick[fault_class];
                    }
                    recover_fault(team_id, class_id);
                    if(!causal)
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
                 * fault class (not in the causal profile). */
                if(team->fault_state.pending_repair[fault_class] &&
                   g_current_tick >= team->fault_state.repair_confirm_tick[fault_class])
                {
                    if(causal)
                    {
                        s_causal_stats.cleared_by_backstop[fault_class]++;
                        s_causal_stats.active_ticks_total[fault_class] +=
                            g_current_tick - team->fault_state.start_tick[fault_class];
                    }
                    recover_fault(team_id, class_id);
                    if(!causal)
                        team->fault_state.immune_until_tick[fault_class] =
                            g_current_tick + FAULT_POST_REPAIR_IMMUNITY_TICKS;
                }

                continue;
            }

            if(g_current_tick < team->fault_state.immune_until_tick[fault_class])
                continue;

            if(!roll_injection(team_id, fault_class, g_fault_config.class_probability[fault_class]))
                continue;

            inject_fault(team_id, class_id);
            if(causal)
                causal_note_onset(team_id, class_id);
        }
    }
}

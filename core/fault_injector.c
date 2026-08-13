#include "fault_injector.h"

#include <stdbool.h>
#include <stddef.h>
#include "environment.h"
#include "incident.h"
#include "mission_state.h"
#include "path.h"
#include "random.h"
#include "snapshot.h"

static uint32_t g_fault_seed = 1;

static uint32_t fault_duration_for_class(OperationalFaultClass_t fault_class)
{
    uint8_t min_ticks = g_fault_config.duration_min_ticks[fault_class];
    uint8_t max_ticks = g_fault_config.duration_max_ticks[fault_class];

    if(min_ticks == 0 && max_ticks == 0)
        return 0;

    if(min_ticks == 0)
        min_ticks = 1;

    if(max_ticks < min_ticks)
        max_ticks = min_ticks;

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

    fault_state->active[FAULT_CLASS_MOBILITY] = 1;
    fault_state->start_tick[FAULT_CLASS_MOBILITY] = g_current_tick;
    fault_state->duration_ticks[FAULT_CLASS_MOBILITY] = fault_duration_for_class(FAULT_CLASS_MOBILITY);
    fault_state->recovery_tick[FAULT_CLASS_MOBILITY] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_MOBILITY];
    fault_state->severity[FAULT_CLASS_MOBILITY] = fault_severity_for_class(FAULT_CLASS_MOBILITY);
    fault_state->target_team[FAULT_CLASS_MOBILITY] = team_id;
    fault_state->active_mask |= (1u << FAULT_CLASS_MOBILITY);
    mark_last_fault(fault_state, FAULT_CLASS_MOBILITY);
    snapshot_record(g_current_tick, EVENT_FAULT_INJECTED, team_id);
}

static void inject_communication_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;

    fault_state->active[FAULT_CLASS_COMMUNICATION] = 1;
    fault_state->start_tick[FAULT_CLASS_COMMUNICATION] = g_current_tick;
    fault_state->duration_ticks[FAULT_CLASS_COMMUNICATION] = fault_duration_for_class(FAULT_CLASS_COMMUNICATION);
    fault_state->recovery_tick[FAULT_CLASS_COMMUNICATION] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_COMMUNICATION];
    fault_state->severity[FAULT_CLASS_COMMUNICATION] = fault_severity_for_class(FAULT_CLASS_COMMUNICATION);
    fault_state->target_team[FAULT_CLASS_COMMUNICATION] = team_id;
    fault_state->active_mask |= (1u << FAULT_CLASS_COMMUNICATION);
    mark_last_fault(fault_state, FAULT_CLASS_COMMUNICATION);
    snapshot_record(g_current_tick, EVENT_FAULT_INJECTED, team_id);
}

static void inject_resource_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;

    fault_state->active[FAULT_CLASS_RESOURCE] = 1;
    fault_state->start_tick[FAULT_CLASS_RESOURCE] = g_current_tick;
    fault_state->duration_ticks[FAULT_CLASS_RESOURCE] = fault_duration_for_class(FAULT_CLASS_RESOURCE);
    fault_state->recovery_tick[FAULT_CLASS_RESOURCE] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_RESOURCE];
    fault_state->severity[FAULT_CLASS_RESOURCE] = fault_severity_for_class(FAULT_CLASS_RESOURCE);
    fault_state->target_team[FAULT_CLASS_RESOURCE] = team_id;
    fault_state->active_mask |= (1u << FAULT_CLASS_RESOURCE);
    mark_last_fault(fault_state, FAULT_CLASS_RESOURCE);
    snapshot_record(g_current_tick, EVENT_FAULT_INJECTED, team_id);
}

static void inject_environment_fault(uint8_t team_id)
{
    TeamState_t *team = &g_teams[team_id];
    TeamFaultState_t *fault_state = &team->fault_state;
    int next_x = team->loc_x;
    int next_y = team->loc_y;

    if(team->assigned_task_pool != TASK_POOL_NONE &&
       team->assigned_task_slot < NUM_INCIDENTS)
    {
        IncidentState_t *task = &g_incidents[team->assigned_task_slot];

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
    fault_state->duration_ticks[FAULT_CLASS_ENVIRONMENT] = fault_duration_for_class(FAULT_CLASS_ENVIRONMENT);
    fault_state->recovery_tick[FAULT_CLASS_ENVIRONMENT] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_ENVIRONMENT];
    fault_state->severity[FAULT_CLASS_ENVIRONMENT] = fault_severity_for_class(FAULT_CLASS_ENVIRONMENT);
    fault_state->target_team[FAULT_CLASS_ENVIRONMENT] = team_id;
    fault_state->target_x[FAULT_CLASS_ENVIRONMENT] = (int16_t)next_x;
    fault_state->target_y[FAULT_CLASS_ENVIRONMENT] = (int16_t)next_y;
    fault_state->original_travel_cost[FAULT_CLASS_ENVIRONMENT] =
        environment_cell_travel_cost(next_x, next_y);
    fault_state->original_road_blocked[FAULT_CLASS_ENVIRONMENT] =
        g_environment[next_x][next_y].road_blocked;
    fault_state->original_navigable[FAULT_CLASS_ENVIRONMENT] =
        g_environment[next_x][next_y].navigable;

    environment_set_cell_operational_state(
        next_x,
        next_y,
        (uint8_t)(g_environment[next_x][next_y].travel_cost +
                  fault_state->severity[FAULT_CLASS_ENVIRONMENT]),
        0,
        1);

    fault_state->active_mask |= (1u << FAULT_CLASS_ENVIRONMENT);
    mark_last_fault(fault_state, FAULT_CLASS_ENVIRONMENT);
    snapshot_record(g_current_tick, EVENT_FAULT_INJECTED, team_id);
}

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
    fault_state->duration_ticks[FAULT_CLASS_MISSION] = fault_duration_for_class(FAULT_CLASS_MISSION);
    fault_state->recovery_tick[FAULT_CLASS_MISSION] =
        g_current_tick + fault_state->duration_ticks[FAULT_CLASS_MISSION];
    fault_state->severity[FAULT_CLASS_MISSION] = fault_severity_for_class(FAULT_CLASS_MISSION);
    fault_state->target_team[FAULT_CLASS_MISSION] = team_id;
    fault_state->target_x[FAULT_CLASS_MISSION] = task->loc_x;
    fault_state->target_y[FAULT_CLASS_MISSION] = task->loc_y;

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

    if(task->priority > PRIORITY_CRITICAL)
        task->priority--;

    if(task->victim_count < 25)
        task->victim_count = (uint8_t)(task->victim_count + fault_state->severity[FAULT_CLASS_MISSION]);

    fault_state->active_mask |= (1u << FAULT_CLASS_MISSION);
    mark_last_fault(fault_state, FAULT_CLASS_MISSION);
    snapshot_record(g_current_tick, EVENT_FAULT_INJECTED, team_id);
}

static void recover_environment_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;
    OperationalFaultClass_t fault_class = FAULT_CLASS_ENVIRONMENT;

    environment_set_cell_operational_state(
        fault_state->target_x[fault_class],
        fault_state->target_y[fault_class],
        fault_state->original_travel_cost[fault_class],
        fault_state->original_navigable[fault_class],
        fault_state->original_road_blocked[fault_class]);

    fault_state->recovery_tick[fault_class] = g_current_tick;
    mark_last_fault(fault_state, fault_class);
    snapshot_record(g_current_tick, EVENT_FAULT_RECOVERED, team_id);
    clear_fault_summary(fault_state, fault_class);
}

static void recover_mission_fault(uint8_t team_id)
{
    TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;
    OperationalFaultClass_t fault_class = FAULT_CLASS_MISSION;

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        IncidentState_t *task = &g_incidents[i];

        if(task->slot_occupied &&
           task->loc_x == fault_state->target_x[fault_class] &&
           task->loc_y == fault_state->target_y[fault_class] &&
           task->status == INC_OPEN)
        {
            /* The (x,y) match can land on a different incident than the
             * one actually faulted, if the real target already resolved
             * and something unrelated now occupies the same cell. That
             * incident was never tracked here (active_count still its
             * default 0) - touching its priority/victim_count would
             * corrupt an incident this fault never touched. Only ever
             * decrement/restore an incident this mechanism is actually
             * tracking (active_count > 0 already, from inject time). */
            if(task->mission_fault_active_count == 0)
                break;

            task->mission_fault_active_count--;

            if(task->mission_fault_active_count == 0)
            {
                task->priority = (Priority_t)task->mission_fault_original_priority;
                task->victim_count = task->mission_fault_original_victim_count;
            }
            break;
        }
    }

    fault_state->recovery_tick[fault_class] = g_current_tick;
    mark_last_fault(fault_state, fault_class);
    snapshot_record(g_current_tick, EVENT_FAULT_RECOVERED, team_id);
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
            snapshot_record(g_current_tick, EVENT_FAULT_RECOVERED, team_id);
            clear_fault_summary(fault_state, fault_class);
            break;

        case FAULT_CLASS_ENVIRONMENT:
            recover_environment_fault(team_id);
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
            inject_environment_fault(team_id);
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

void fault_injector_init(uint32_t seed)
{
    g_fault_seed = seed;
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

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        TeamState_t *team = &g_teams[team_id];

        for(uint8_t fault_class = 0; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
        {
            OperationalFaultClass_t class_id = (OperationalFaultClass_t)fault_class;

            if(team->fault_state.active[fault_class])
            {
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

#include "mission_execution_twin.h"

#include <limits.h>
#include <stddef.h>

#include "environment.h"
#include "incident.h"
#include "mission_state.h"
#include "mission_state_utils.h"
#include "path.h"

#define NOMINAL_WORK_RATE_MIN 1u
#define NOMINAL_WORK_RATE_MAX 5u

static int task_ref_equal(TaskRef_t left, TaskRef_t right)
{
    return left.pool == right.pool && left.slot == right.slot;
}

static IncidentState_t *resolve_task_ref(TaskRef_t task_ref)
{
    if(task_ref.pool == TASK_POOL_INCIDENT && task_ref.slot < NUM_INCIDENTS)
    {
        IncidentState_t *incident = &g_incidents[task_ref.slot];
        return incident->slot_occupied ? incident : NULL;
    }

    if(task_ref.pool == TASK_POOL_SYSTEM && task_ref.slot < NUM_SYSTEM_TASKS)
    {
        IncidentState_t *task = &g_system_tasks[task_ref.slot];
        return task->slot_occupied ? task : NULL;
    }

    return NULL;
}

static IncidentState_t *resolve_assigned_task(const TeamState_t *team)
{
    if(team == NULL)
        return NULL;

    return resolve_task_ref((TaskRef_t){
        .pool = (TaskPool_t)team->assigned_task_pool,
        .slot = team->assigned_task_slot
    });
}

static float clamp_stamina(float stamina)
{
    if(stamina < MIN_STAMINA)
        return MIN_STAMINA;

    if(stamina > MAX_STAMINA)
        return MAX_STAMINA;

    return stamina;
}

static uint16_t resolve_actual_remaining_path_cost(
    int16_t start_x,
    int16_t start_y,
    int16_t dest_x,
    int16_t dest_y)
{
    if(start_x == dest_x && start_y == dest_y)
        return 0;

    return find_path_cost(start_x, start_y, dest_x, dest_y);
}

static uint8_t resolve_communication_health(const TeamState_t *team)
{
    if(!team->fault_state.active[FAULT_CLASS_COMMUNICATION])
        return 0;

    return team->fault_state.severity[FAULT_CLASS_COMMUNICATION];
}

static uint16_t nominal_work_rate(const IncidentState_t *task)
{
    uint16_t rate = (NOMINAL_WORK_RATE_MIN + NOMINAL_WORK_RATE_MAX) / 2u;

    if(task != NULL &&
       task->support_team != NO_ASSIGNMENT &&
       task->support_team < NUM_TEAMS)
    {
        rate += (NOMINAL_WORK_RATE_MIN + NOMINAL_WORK_RATE_MAX) / 2u;
    }

    return rate;
}

static float compute_progress_ratio(
    uint16_t baseline_path_cost,
    uint16_t baseline_work_remaining,
    uint16_t remaining_path_cost,
    uint16_t work_remaining)
{
    uint32_t total_units = (uint32_t)baseline_path_cost + (uint32_t)baseline_work_remaining;

    if(total_units == 0u)
        return 0.0f;

    if(remaining_path_cost == UINT16_MAX || remaining_path_cost > baseline_path_cost)
        remaining_path_cost = baseline_path_cost;

    if(work_remaining > baseline_work_remaining)
        work_remaining = baseline_work_remaining;

    return (float)(
        (uint32_t)(baseline_path_cost - remaining_path_cost) +
        (uint32_t)(baseline_work_remaining - work_remaining)) /
        (float)total_units;
}

static MissionStage_t resolve_expected_stage(const MissionExecutionTwin_t *twin)
{
    if(twin->assigned_task_pool == TASK_POOL_NONE ||
       twin->assigned_task_slot == NO_ASSIGNMENT)
    {
        return MISSION_STAGE_IDLE;
    }

    if(twin->expected_remaining_path_cost > 0u)
        return MISSION_STAGE_TRAVEL;

    if(twin->expected_work_remaining > 0u)
        return MISSION_STAGE_WORK;

    return MISSION_STAGE_WORK;
}

static uint8_t plan_contains_current_assignment(
    const MissionExecutionTwin_t *twin,
    const TeamState_t *team,
    uint8_t *path_index)
{
    TaskRef_t current = {
        .pool = (TaskPool_t)team->assigned_task_pool,
        .slot = team->assigned_task_slot
    };

    for(uint8_t i = 0; i < twin->observed_plan_size; i++)
    {
        if(task_ref_equal(twin->observed_plan[i], current))
        {
            if(path_index != NULL)
                *path_index = i;
            return 1;
        }
    }

    return 0;
}

static int twin_plan_changed(const MissionExecutionTwin_t *twin, const TeamState_t *team)
{
    uint8_t effective_size = twin->planned_path_size;

    if(effective_size == 0 &&
       team->assigned_task_pool != TASK_POOL_NONE &&
       team->assigned_task_slot != NO_ASSIGNMENT)
    {
        effective_size = 1;
    }

    if(twin->observed_plan_size != effective_size)
        return 1;

    for(uint8_t i = 0; i < twin->planned_path_size; i++)
    {
        if(!task_ref_equal(twin->observed_plan[i], twin->planned_path[i]))
            return 1;
    }

    if(twin->planned_path_size == 0 && effective_size == 1)
    {
        TaskRef_t assigned = {
            .pool = (TaskPool_t)team->assigned_task_pool,
            .slot = team->assigned_task_slot
        };

        if(!task_ref_equal(twin->observed_plan[0], assigned))
            return 1;
    }

    return 0;
}

static void twin_sync_observed_plan(MissionExecutionTwin_t *twin, const TeamState_t *team)
{
    twin->observed_plan_size = twin->planned_path_size;

    for(uint8_t i = 0; i < twin->planned_path_size; i++)
        twin->observed_plan[i] = twin->planned_path[i];

    if(twin->observed_plan_size == 0 &&
       team->assigned_task_pool != TASK_POOL_NONE &&
       team->assigned_task_slot != NO_ASSIGNMENT)
    {
        twin->observed_plan[0] = (TaskRef_t){
            .pool = (TaskPool_t)team->assigned_task_pool,
            .slot = team->assigned_task_slot
        };
        twin->observed_plan_size = 1;
    }

    if(!plan_contains_current_assignment(twin, team, &twin->expected_path_index))
    {
        twin->expected_path_index = twin->observed_plan_size > 0 ? 0 : NO_ASSIGNMENT;
    }
}

static IncidentState_t *twin_expected_task(
    const MissionExecutionTwin_t *twin,
    const TeamState_t *team)
{
    if(twin->expected_path_index != NO_ASSIGNMENT &&
       twin->expected_path_index < twin->observed_plan_size)
    {
        IncidentState_t *task = resolve_task_ref(
            twin->observed_plan[twin->expected_path_index]);
        if(task != NULL)
            return task;
    }

    return resolve_assigned_task(team);
}

static int twin_current_task_changed(
    const MissionExecutionTwin_t *twin,
    const TeamState_t *team,
    const IncidentState_t *task)
{
    if(task == NULL)
    {
        return twin->observed_task_id != 0 ||
               twin->assigned_task_pool != TASK_POOL_NONE ||
               twin->assigned_task_slot != NO_ASSIGNMENT;
    }

    return twin->assigned_task_pool != team->assigned_task_pool ||
           twin->assigned_task_slot != team->assigned_task_slot ||
           twin->observed_task_id != task->incident_id ||
           twin->observed_req_capability != task->req_capability;
}

static void reset_twin_for_current_mission(
    MissionExecutionTwin_t *twin,
    const TeamState_t *team,
    const IncidentState_t *task,
    uint16_t current_remaining_path_cost)
{
    twin->assigned_task_pool = team->assigned_task_pool;
    twin->assigned_task_slot = team->assigned_task_slot;
    twin->expected_arrival_tick = (task == NULL)
        ? g_current_tick
        : g_current_tick + current_remaining_path_cost;
    twin->expected_progress = 0.0f;
    twin->actual_progress = 0.0f;
    twin->progress_error = 0.0f;
    twin->expected_stamina = team->stamina;
    twin->actual_stamina = team->stamina;
    twin->stamina_error = 0.0f;
    twin->expected_work_remaining = 0;
    twin->remaining_path_cost = 0;
    twin->communication_health = resolve_communication_health(team);
    twin->expected_operating_state = team->status;
    twin->observed_task_id = 0;
    twin->observed_req_capability = NO_ASSIGNMENT;
    twin->baseline_path_cost = 0;
    twin->baseline_work_remaining = 0;
    twin->expected_remaining_path_cost = 0;
    twin->expected_loc_x = team->loc_x;
    twin->expected_loc_y = team->loc_y;
    twin->destination_x = team->loc_x;
    twin->destination_y = team->loc_y;

    if(task == NULL)
    {
        twin->mission_stage = team->status == STATUS_RESTING
            ? MISSION_STAGE_RECOVERY
            : MISSION_STAGE_IDLE;
        return;
    }

    twin->observed_task_id = task->incident_id;
    twin->observed_req_capability = task->req_capability;
    twin->destination_x = task->loc_x;
    twin->destination_y = task->loc_y;
    twin->remaining_path_cost = current_remaining_path_cost;
    twin->baseline_path_cost =
        current_remaining_path_cost == UINT16_MAX ? 0 : current_remaining_path_cost;
    twin->baseline_work_remaining = task->work_remaining;
    twin->expected_work_remaining = task->work_remaining;
    twin->expected_remaining_path_cost = twin->baseline_path_cost;
    twin->mission_stage = resolve_expected_stage(twin);
}

static void twin_advance_expected_position(MissionExecutionTwin_t *twin, const TeamState_t *team)
{
    int next_x = twin->expected_loc_x;
    int next_y = twin->expected_loc_y;

    if(twin->expected_remaining_path_cost == 0u)
        return;

    if(find_next_path_step(
        twin->expected_loc_x,
        twin->expected_loc_y,
        twin->destination_x,
        twin->destination_y,
        0,
        &next_x,
        &next_y) <= 0)
    {
        twin->expected_remaining_path_cost = resolve_actual_remaining_path_cost(
            twin->expected_loc_x,
            twin->expected_loc_y,
            twin->destination_x,
            twin->destination_y);
        return;
    }

    twin->expected_loc_x = (int16_t)next_x;
    twin->expected_loc_y = (int16_t)next_y;

    {
        uint16_t step_cost = environment_cell_travel_cost(next_x, next_y);

        if(step_cost >= twin->expected_remaining_path_cost)
            twin->expected_remaining_path_cost = 0;
        else
            twin->expected_remaining_path_cost =
                (uint16_t)(twin->expected_remaining_path_cost - step_cost);
    }

    twin->expected_stamina = clamp_stamina(twin->expected_stamina - team_travel_stamina_cost(team));
}

static void twin_advance_expected_work(
    MissionExecutionTwin_t *twin,
    const IncidentState_t *task)
{
    uint16_t rate;

    if(twin->expected_work_remaining == 0u)
        return;

    rate = nominal_work_rate(task);

    if(rate >= twin->expected_work_remaining)
        twin->expected_work_remaining = 0;
    else
        twin->expected_work_remaining = (uint16_t)(twin->expected_work_remaining - rate);

    twin->expected_stamina = clamp_stamina(
        twin->expected_stamina - STAMINA_OPERATION_COST);
}

void mission_execution_twin_reset(void)
{
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        MissionExecutionTwin_t *twin = &g_mission_execution_twin[team_id];
        TeamState_t *team = &g_teams[team_id];

        twin->team_id = team_id;
        twin->assigned_task_pool = TASK_POOL_NONE;
        twin->assigned_task_slot = NO_ASSIGNMENT;
        twin->planned_path_size = 0;
        twin->expected_path_index = NO_ASSIGNMENT;
        twin->destination_x = team->loc_x;
        twin->destination_y = team->loc_y;
        twin->expected_arrival_tick = g_current_tick;
        twin->expected_progress = 0.0f;
        twin->actual_progress = 0.0f;
        twin->progress_error = 0.0f;
        twin->expected_stamina = team->stamina;
        twin->actual_stamina = team->stamina;
        twin->stamina_error = 0.0f;
        twin->expected_work_remaining = 0;
        twin->remaining_path_cost = 0;
        twin->mission_stage = team->status == STATUS_RESTING
            ? MISSION_STAGE_RECOVERY
            : MISSION_STAGE_IDLE;
        twin->communication_health = 0;
        twin->expected_operating_state = team->status;
        twin->observed_task_id = 0;
        twin->observed_req_capability = NO_ASSIGNMENT;
        twin->observed_plan_size = 0;
        twin->baseline_path_cost = 0;
        twin->baseline_work_remaining = 0;
        twin->expected_remaining_path_cost = 0;
        twin->expected_loc_x = team->loc_x;
        twin->expected_loc_y = team->loc_y;
    }
}

void mission_execution_twin_step(void)
{
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        MissionExecutionTwin_t *twin = &g_mission_execution_twin[team_id];
        TeamState_t *team = &g_teams[team_id];
        IncidentState_t *actual_task = resolve_assigned_task(team);
        IncidentState_t *expected_task;
        uint16_t current_remaining_path_cost = 0;

        if(twin_plan_changed(twin, team))
            twin_sync_observed_plan(twin, team);

        if(actual_task != NULL)
        {
            if(team->cached_remaining_path_cost_tick == g_current_tick)
            {
                current_remaining_path_cost = team->cached_remaining_path_cost;
            }
            else
            {
                current_remaining_path_cost = resolve_actual_remaining_path_cost(
                    team->loc_x, team->loc_y, actual_task->loc_x, actual_task->loc_y);
            }
        }

        if(twin_current_task_changed(twin, team, actual_task))
        {
            reset_twin_for_current_mission(
                twin,
                team,
                actual_task,
                current_remaining_path_cost);
        }

        twin->team_id = team_id;
        twin->actual_stamina = team->stamina;
        twin->communication_health = resolve_communication_health(team);
        twin->assigned_task_pool = team->assigned_task_pool;
        twin->assigned_task_slot = team->assigned_task_slot;
        twin->remaining_path_cost = current_remaining_path_cost;

        if(actual_task == NULL)
        {
            twin->destination_x = team->loc_x;
            twin->destination_y = team->loc_y;
            twin->expected_loc_x = team->loc_x;
            twin->expected_loc_y = team->loc_y;
            twin->expected_work_remaining = 0;
            twin->expected_remaining_path_cost = 0;
            twin->expected_progress = 0.0f;
            twin->actual_progress = 0.0f;

            if(team->status == STATUS_RESTING)
            {
                twin->expected_stamina = clamp_stamina(
                    twin->expected_stamina + team_recovery_rate(team));
                twin->mission_stage = MISSION_STAGE_RECOVERY;
                twin->expected_operating_state = STATUS_RESTING;
            }
            else
            {
                twin->expected_stamina = team->stamina;
                twin->mission_stage = MISSION_STAGE_IDLE;
                twin->expected_operating_state = team->status;
            }

            twin->progress_error = 0.0f;
            twin->stamina_error = twin->expected_stamina - twin->actual_stamina;
            continue;
        }

        expected_task = twin_expected_task(twin, team);
        if(expected_task == NULL)
            expected_task = actual_task;

        twin->destination_x = expected_task->loc_x;
        twin->destination_y = expected_task->loc_y;

        if(twin->expected_remaining_path_cost > 0u)
        {
            twin_advance_expected_position(twin, team);
            twin->expected_operating_state = STATUS_EN_ROUTE;
        }
        else
        {
            twin_advance_expected_work(twin, expected_task);
            twin->expected_operating_state = STATUS_ON_SCENE;
        }

        twin->expected_progress = compute_progress_ratio(
            twin->baseline_path_cost,
            twin->baseline_work_remaining,
            twin->expected_remaining_path_cost,
            twin->expected_work_remaining);
        twin->actual_progress = compute_progress_ratio(
            twin->baseline_path_cost,
            twin->baseline_work_remaining,
            current_remaining_path_cost,
            actual_task->work_remaining);
        twin->progress_error = twin->expected_progress - twin->actual_progress;
        twin->stamina_error = twin->expected_stamina - twin->actual_stamina;
        twin->mission_stage = resolve_expected_stage(twin);
    }
}

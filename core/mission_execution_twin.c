#include "mission_execution_twin.h"
#include "causal_actions.h"
#include "fault_config.h"

#include <limits.h>
#include <stddef.h>

#include "environment.h"
#include "incident.h"
#include "mission_state.h"
#include "mission_state_utils.h"
#include "path.h"

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

/* Observed communication health: the worst heartbeat gap (ticks since
 * team->last_heartbeat_tick) seen over the last
 * TWIN_COMM_HEALTH_WINDOW_TICKS ticks, capped at TWIN_COMM_HEALTH_MAX.
 *
 * This used to return team->fault_state.severity[FAULT_CLASS_
 * COMMUNICATION] directly - the injector's ground truth, which made
 * this "measurement" identical to the fault label on every row and
 * leaked the answer into watchdog.c's communication evidence channel,
 * evaluate_communication() and every ML feature derived from
 * twin_communication_health. A comm fault's observable symptom is the
 * missed heartbeats update_team() (core/team.c) produces - a severity-s
 * fault only lands a heartbeat every s+1 ticks, so the worst gap inside
 * a window of s+1 ticks is s. Windowing is needed because the gap
 * itself saw-tooths back to 0 on every heartbeat that does land. The
 * window (4) covers the largest configured comm severity (3, see
 * core/fault_config.c), and the cap keeps the value on the same 0..3
 * scale downstream consumers (watchdog.c's /3.0f, recovery_manager_
 * config.h's COMMUNICATION_* thresholds, evidence_channels.py) expect.
 *
 * Updated once per tick from mission_execution_twin_step();
 * resolve_communication_health() only reads the current window. */
#define TWIN_COMM_HEALTH_WINDOW_TICKS 4u
#define TWIN_COMM_HEALTH_MAX 3u

static uint8_t g_comm_gap_window[NUM_TEAMS][TWIN_COMM_HEALTH_WINDOW_TICKS];

static void update_communication_health_window(const TeamState_t *team)
{
    uint32_t gap;

    if(team->team_id >= NUM_TEAMS)
        return;

    gap = g_current_tick - team->last_heartbeat_tick;
    if(gap > TWIN_COMM_HEALTH_MAX)
        gap = TWIN_COMM_HEALTH_MAX;

    g_comm_gap_window[team->team_id][g_current_tick % TWIN_COMM_HEALTH_WINDOW_TICKS] =
        (uint8_t)gap;
}

static uint8_t resolve_communication_health(const TeamState_t *team)
{
    uint8_t worst = 0;

    if(team->team_id >= NUM_TEAMS)
        return 0;

    for(uint8_t i = 0; i < TWIN_COMM_HEALTH_WINDOW_TICKS; i++)
    {
        if(g_comm_gap_window[team->team_id][i] > worst)
            worst = g_comm_gap_window[team->team_id][i];
    }

    return worst;
}

/* Fault-free baseline work rate: mirrors update_incident_work()'s
 * (core/incident.c) real two-call structure exactly - the primary
 * team's own equipment-scaled rate, plus (if a support team is on
 * scene) that support team's own independently equipment-scaled
 * rate, both via team_base_work_rate() (core/incident.h) - rather
 * than doubling the primary's rate as a stand-in for a support
 * team's real, possibly-different equipment level. No fault term:
 * this is deliberately what the team(s) would produce with no fault
 * active, so twin_progress_error stays sensitive to a real
 * RESOURCE/MOBILITY-fault-induced slowdown instead of partly
 * absorbing it into an already-too-low, equipment-blind constant. */
static uint16_t nominal_work_rate(const IncidentState_t *task, const TeamState_t *team)
{
    /* T3: a SUPPORT team's twin models the task's work, and the task's work
     * is what update_incident_work() does: it only ticks while the PRIMARY
     * team (task->assigned_team) is on scene, at primary + on-scene-support.
     * This used to treat whichever team it was modelling as the primary, so
     * for a support team it counted that team's own rate twice and started
     * expecting progress the moment the support team arrived - typically
     * before the primary did - which read as a progress deviation. */
    if(task != NULL &&
       team != NULL &&
       task->assigned_team != team->team_id &&
       task->support_team == team->team_id)
    {
        /* Primary released (the stage just completed and the task
         * reopened for the next one): nothing is being worked. */
        if(task->assigned_team >= NUM_TEAMS)
            return 0;

        const TeamState_t *primary = &g_teams[task->assigned_team];

        /* Same gate update_incident_work() applies: the task must be
         * IN_PROGRESS (the primary arrived at THIS task - it can be on
         * scene elsewhere while this one is only queued for it) and the
         * primary on scene. */
        if(task->status != INC_IN_PROGRESS || primary->status != STATUS_ON_SCENE)
            return 0;

        return (uint16_t)(team_base_work_rate(primary->equipment_level) +
                          team_base_work_rate(team->equipment_level));
    }

    uint16_t rate = team != NULL ? team_base_work_rate(team->equipment_level) : WORK_RATE_MIN;

    if(task != NULL &&
       task->support_team != NO_ASSIGNMENT &&
       task->support_team < NUM_TEAMS &&
       g_teams[task->support_team].status == STATUS_ON_SCENE)
    {
        rate += team_base_work_rate(g_teams[task->support_team].equipment_level);
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
        if(team->heading_to_rest)
        {
            const SafeZone_t *safezone = &g_safezones[team->resting_zone_id];
            return twin->mission_stage != MISSION_STAGE_TRAVEL ||
                   twin->destination_x != safezone->loc_x ||
                   twin->destination_y != safezone->loc_y;
        }

        return twin->observed_task_id != 0 ||
               twin->assigned_task_pool != TASK_POOL_NONE ||
               twin->assigned_task_slot != NO_ASSIGNMENT;
    }

    return twin->assigned_task_pool != team->assigned_task_pool ||
           twin->assigned_task_slot != team->assigned_task_slot ||
           twin->observed_task_id != task->incident_id ||
           twin->observed_req_capability != task->req_capability;
}

/* T1: the destination of the leg each twin is currently tracking. A
 * task can move under a team without changing id, pool, slot or
 * capability - a search sweep's patrol code retargets task->loc_x/loc_y
 * to the next frontier cell, an evacuation run's complete_incident_stage()
 * moves it from the safe zone to a gateway - and twin_current_task_changed()
 * cannot see that. Kept here, not in MissionExecutionTwin_t, because
 * twin->destination_x/y is overwritten from the task every tick and so
 * can't be compared against. */
static int16_t g_twin_leg_dest_x[NUM_TEAMS];
static int16_t g_twin_leg_dest_y[NUM_TEAMS];

/* Ticks a team needs for the route from its own cell to (dest_x, dest_y):
 * one cell per tick, exactly how move_team() advances. NOT the route COST
 * (cells cost 1-25 each), which is what expected_arrival_tick used to be
 * built from. The twin's model is nominal, so this deliberately ignores
 * the mobility-fault skip move_team() applies at severity >= 3 - a
 * team slowed by one is meant to run late against it. UINT16_MAX (no
 * route) keeps the old "never times out" behaviour. */
static uint32_t resolve_arrival_tick(const TeamState_t *team, int16_t dest_x, int16_t dest_y)
{
    return g_current_tick + (uint32_t)find_path_steps(team->loc_x, team->loc_y, dest_x, dest_y);
}

/* T1: same task, new destination. Restarts the TRAVEL baseline only - the
 * leg's path cost, expected position, arrival tick and stage - and keeps
 * everything that belongs to the task as a whole (expected stamina, which
 * keeps accumulating, and the task identity), so a team that keeps being
 * retargeted without making ground still falls behind on each leg's own
 * baseline instead of being forgiven by a full reset. Work is re-based
 * to the task's current work_remaining: a sweep has none (0 either way),
 * and an evacuation leg gets a fresh stage quantum from incident_stage_
 * work() when it is handed on - the previous leg's work is done by then. */
static void twin_retarget_leg(
    MissionExecutionTwin_t *twin,
    const TeamState_t *team,
    const IncidentState_t *task,
    uint16_t fresh_remaining_path_cost)
{
    twin->destination_x = task->loc_x;
    twin->destination_y = task->loc_y;
    twin->remaining_path_cost = fresh_remaining_path_cost;
    twin->baseline_path_cost =
        fresh_remaining_path_cost == UINT16_MAX ? 0 : fresh_remaining_path_cost;
    twin->expected_remaining_path_cost = twin->baseline_path_cost;
    twin->expected_loc_x = team->loc_x;
    twin->expected_loc_y = team->loc_y;
    twin->expected_arrival_tick = resolve_arrival_tick(team, task->loc_x, task->loc_y);
    twin->baseline_work_remaining = task->work_remaining;
    twin->expected_work_remaining = task->work_remaining;
    twin->mission_stage = resolve_expected_stage(twin);
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
        : resolve_arrival_tick(team, task->loc_x, task->loc_y);
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
        if(team->heading_to_rest)
        {
            const SafeZone_t *safezone = &g_safezones[team->resting_zone_id];
            uint16_t remaining = resolve_actual_remaining_path_cost(
                team->loc_x, team->loc_y, safezone->loc_x, safezone->loc_y);

            twin->destination_x = safezone->loc_x;
            twin->destination_y = safezone->loc_y;
            twin->remaining_path_cost = remaining;
            twin->baseline_path_cost = (remaining == UINT16_MAX) ? 0 : remaining;
            twin->baseline_work_remaining = 0;
            twin->expected_work_remaining = 0;
            twin->expected_remaining_path_cost = twin->baseline_path_cost;
            twin->expected_arrival_tick = resolve_arrival_tick(team, safezone->loc_x, safezone->loc_y);
            twin->mission_stage = MISSION_STAGE_TRAVEL;
            return;
        }

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

/* Causal stall-aware fix: while a team is held at a known partial-debris cell the plan waits with it, so the
 * delay is not read as lost progress. */
static int twin_debris_held(const TeamState_t *team)
{
    return fault_profile_causal_fix(CAUSAL_FIX_STALL) && causal_actions_debris_held_at(team->team_id, g_current_tick);
}

static void twin_advance_expected_position(MissionExecutionTwin_t *twin, const TeamState_t *team)
{
    int next_x = twin->expected_loc_x;
    int next_y = twin->expected_loc_y;
    uint16_t remaining_after_step = 0;
    int pre_step_cost;

    if(twin->expected_remaining_path_cost == 0u)
        return;

    pre_step_cost = find_next_path_step_remaining(
        twin->expected_loc_x,
        twin->expected_loc_y,
        twin->destination_x,
        twin->destination_y,
        &next_x,
        &next_y,
        &remaining_after_step);

    if(pre_step_cost <= 0)
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
        /* What this step takes off the route, as the pathfinder costs it
         * (a diagonal step costs 1.4x the cell it enters) - the same drop
         * move_team() applies to the real team's remaining cost. */
        uint16_t step_cost = (uint16_t)pre_step_cost > remaining_after_step
            ? (uint16_t)((uint16_t)pre_step_cost - remaining_after_step)
            : 0u;

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
    const IncidentState_t *task,
    const TeamState_t *team)
{
    uint16_t rate;

    if(twin->expected_work_remaining == 0u)
        return;

    rate = nominal_work_rate(task, team);

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
        for(uint8_t i = 0; i < TWIN_COMM_HEALTH_WINDOW_TICKS; i++)
            g_comm_gap_window[team_id][i] = 0;
        twin->expected_operating_state = team->status;
        twin->observed_task_id = 0;
        twin->observed_req_capability = NO_ASSIGNMENT;
        twin->observed_plan_size = 0;
        twin->baseline_path_cost = 0;
        twin->baseline_work_remaining = 0;
        twin->expected_remaining_path_cost = 0;
        twin->expected_loc_x = team->loc_x;
        twin->expected_loc_y = team->loc_y;
        g_twin_leg_dest_x[team_id] = team->loc_x;
        g_twin_leg_dest_y[team_id] = team->loc_y;
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
        else if(team->heading_to_rest)
        {
            const SafeZone_t *safezone = &g_safezones[team->resting_zone_id];

            if(team->cached_remaining_path_cost_tick == g_current_tick)
            {
                current_remaining_path_cost = team->cached_remaining_path_cost;
            }
            else
            {
                current_remaining_path_cost = resolve_actual_remaining_path_cost(
                    team->loc_x, team->loc_y, safezone->loc_x, safezone->loc_y);
            }
        }

        int twin_just_reset = 0;

        if(twin_current_task_changed(twin, team, actual_task))
        {
            /* T2: never trust team->cached_remaining_path_cost here. It was
             * stamped by move_team() for whatever the team was travelling to
             * when it moved this tick; if the team changed task after that
             * (e.g. it arrived, finished, and was handed the next one), the
             * cache describes the PREVIOUS destination - 0 for a team that
             * just arrived - and the twin would skip travel it still owes. */
            if(actual_task != NULL)
                current_remaining_path_cost = resolve_actual_remaining_path_cost(
                    team->loc_x, team->loc_y, actual_task->loc_x, actual_task->loc_y);
            else if(team->heading_to_rest)
                current_remaining_path_cost = resolve_actual_remaining_path_cost(
                    team->loc_x, team->loc_y,
                    g_safezones[team->resting_zone_id].loc_x,
                    g_safezones[team->resting_zone_id].loc_y);
            else
                current_remaining_path_cost = 0;

            reset_twin_for_current_mission(
                twin,
                team,
                actual_task,
                current_remaining_path_cost);
            twin_just_reset = 1;
            g_twin_leg_dest_x[team_id] = twin->destination_x;
            g_twin_leg_dest_y[team_id] = twin->destination_y;
        }
        else if(actual_task != NULL &&
                (actual_task->loc_x != g_twin_leg_dest_x[team_id] ||
                 actual_task->loc_y != g_twin_leg_dest_y[team_id]))
        {
            /* T1: same task, moved destination. The cached cost is for the
             * old destination too, so take a fresh one. */
            current_remaining_path_cost = resolve_actual_remaining_path_cost(
                team->loc_x, team->loc_y, actual_task->loc_x, actual_task->loc_y);
            twin_retarget_leg(twin, team, actual_task, current_remaining_path_cost);
            twin_just_reset = 1;
            g_twin_leg_dest_x[team_id] = actual_task->loc_x;
            g_twin_leg_dest_y[team_id] = actual_task->loc_y;
        }

        twin->team_id = team_id;
        twin->actual_stamina = team->stamina;
        update_communication_health_window(team);
        twin->communication_health = resolve_communication_health(team);
        twin->assigned_task_pool = team->assigned_task_pool;
        twin->assigned_task_slot = team->assigned_task_slot;
        twin->remaining_path_cost = current_remaining_path_cost;

        if(actual_task == NULL)
        {
            if(team->heading_to_rest)
            {
                const SafeZone_t *safezone = &g_safezones[team->resting_zone_id];

                twin->destination_x = safezone->loc_x;
                twin->destination_y = safezone->loc_y;

                if(twin->expected_remaining_path_cost > 0u)
                {
                    /* Skip the advance on the exact tick reset_twin_for_
                     * current_mission() just (re)synced expected to the
                     * team's current actual position (current_remaining_
                     * path_cost, captured post-move this tick via
                     * cached_remaining_path_cost - see above). Advancing
                     * here too would predict a SECOND step of travel
                     * within the same tick the mission started, one tick
                     * ahead of what the real team has actually done -
                     * see the fuller writeup on the equivalent branch
                     * below, which hits this same construction. */
                    if(!twin_just_reset && !twin_debris_held(team))
                        twin_advance_expected_position(twin, team);
                    twin->expected_operating_state = STATUS_EN_ROUTE;
                    twin->mission_stage = MISSION_STAGE_TRAVEL;
                }
                else
                {
                    /* Twin expects arrival: mirrors complete_return_to_rest()'s
                     * own transition, one tick ahead of the real team if the
                     * estimate is exact. */
                    twin->expected_operating_state = STATUS_RESTING;
                    twin->mission_stage = MISSION_STAGE_RECOVERY;
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
                    0);
                twin->progress_error = twin->expected_progress - twin->actual_progress;
                twin->stamina_error = twin->expected_stamina - twin->actual_stamina;
                continue;
            }

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
            /* Bug fix: reset_twin_for_current_mission() (called just
             * above, this same tick, whenever twin_current_task_changed()
             * fires) sets both baseline_path_cost and
             * expected_remaining_path_cost from current_remaining_path_cost
             * - the team's ACTUAL remaining distance as of THIS tick
             * (post-move, via cached_remaining_path_cost from move_team()
             * earlier in simulation_step()). twin->remaining_path_cost
             * (the "actual" side of the comparison below) is set to that
             * exact same current_remaining_path_cost a few lines up. So
             * right after a reset, expected and actual already agree
             * perfectly for this tick - correctly, since nothing new has
             * happened yet.
             *
             * But this advance call runs unconditionally afterward,
             * stepping the EXPECTED side forward by one more tick's worth
             * of travel, while the ACTUAL side (current_remaining_path_cost)
             * isn't recomputed again until NEXT tick. That leaves expected
             * permanently one tick ahead of actual for the rest of the
             * mission - a roughly constant absolute gap of about one
             * step's path cost, since both sides advance in lockstep
             * after this. For a long mission that's noise (a couple of
             * percent of the total). For a short one - exactly the kind
             * team_can_accept_incident()'s "steal a resting team" path
             * (core/allocator.c) and natural RESTING->AVAILABLE promotion
             * (core/team.c) hand out to a team that only just cleared its
             * stamina floor - that same fixed absolute gap is a large
             * fraction of a small total (baseline_path_cost +
             * baseline_work_remaining), enough for evaluate_progress()
             * (core/recovery_manager.c) to read it as a genuine deviation
             * and, after a few ticks of hysteresis, dispatch
             * RETURN_TO_SAFE_ZONE - cancelling a mission the team was
             * actually completing exactly on schedule, sending it back to
             * the safe zone it just left, and (once it's barely rested
             * again) re-triggering the same short-mission assignment and
             * the same false deviation call. Confirmed via instrumented
             * trace: a fresh short mission (baseline_path_cost=55,
             * baseline_work_remaining=15) showed a 6-7 unit expected/
             * actual gap from the tick of assignment onward, purely from
             * this double-step, with no real problem underneath it.
             *
             * Skipping the advance on the reset tick itself removes the
             * extra step: expected and actual start the mission in exact
             * agreement and then advance once per tick together from the
             * next tick on, same as any team whose mission wasn't just
             * reassigned. */
            if(!twin_just_reset && !twin_debris_held(team))
                twin_advance_expected_position(twin, team);
            twin->expected_operating_state = STATUS_EN_ROUTE;
        }
        else
        {
            if(!twin_just_reset)
                twin_advance_expected_work(twin, expected_task, team);
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

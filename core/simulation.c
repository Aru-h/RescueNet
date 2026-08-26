#include <stdint.h>
#include "debug.h"
#include "scoring.h"
#include "environment.h"
#include "mission_state.h"
#include "allocator.h"
#include "mission_state_utils.h"
#include "random.h"
#include "simulation.h"
#include "incident.h"
#include "grid.h"
#include "path.h"
#include "team.h"
#include "safezone.h"
#include "snapshot.h"
#include "environment.h"
#include "cbba.h"
#include "watchdog.h"
#include "recovery_manager.h"
#include "fault_injector.h"
#include "mission_execution_twin.h"

uint8_t g_simulation_trace_enabled = 1;
static uint8_t g_prev_anomaly_detected[NUM_TEAMS];
static uint8_t g_prev_recovery_action[NUM_TEAMS];
static uint8_t g_diag_prev_status[NUM_TEAMS];
static float g_diag_rest_start_stamina[NUM_TEAMS];
static uint32_t g_diag_rest_start_tick[NUM_TEAMS];

void generate_world(uint32_t seed)   //simulation
{
    g_world_seed = seed;
    g_current_tick = 0;
    random_seed_all(seed);
    generate_environment(seed);
    initialize_incidents();
    generate_safe_zones(seed);
    for(int i = 0; i < MAX_SAFE_ZONES; i++)
    {
        g_safezone_evacuation_active[i] = 0;
    }
    initialize_evacuation_gateways(seed);
    environment_apply_earthquake(seed);
    initialize_grid();
    generate_teams(seed);
    initialize_evacuation_teams();
    fault_injector_init(seed);
    watchdog_init();
    mission_execution_twin_reset();
    cbba_reset_improvement_counters();
    recovery_manager_init();

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        g_prev_anomaly_detected[team_id] = watchdog_get_status(team_id).anomaly_detected;
        g_prev_recovery_action[team_id] = (uint8_t)recovery_manager_get_status(team_id).recommendation;
        g_diag_prev_status[team_id] = g_teams[team_id].status;
        g_diag_rest_start_stamina[team_id] = 0.0f;
        g_diag_rest_start_tick[team_id] = 0;
    }
}


void simulation_step(void) //simulation
{
    g_current_tick++;
    environment_update_hazard_propagation(g_current_tick);
    generate_incidents_if_needed();
    update_all_teams();
    move_all_teams();
    search_sweep_step(g_current_tick);
    escalate_starved_incidents(g_current_tick);
    update_all_incident_work();
    update_all_passive_effects();
    mission_execution_twin_step();
    watchdog_step();
    recovery_manager_step();
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        uint8_t anomaly_now = watchdog_get_status(team_id).anomaly_detected;
        RecoveryAction_t action_now = recovery_manager_get_status(team_id).recommendation;

        if(anomaly_now != g_prev_anomaly_detected[team_id])
        {
            snapshot_record(g_current_tick, EVENT_WATCHDOG_ANOMALY_CHANGED, team_id);
            g_prev_anomaly_detected[team_id] = anomaly_now;
        }

        if((uint8_t)action_now != g_prev_recovery_action[team_id])
        {
            snapshot_record(g_current_tick, EVENT_RECOVERY_ACTION_CHANGED, team_id);
            g_prev_recovery_action[team_id] = (uint8_t)action_now;
        }

        recovery_dispatch(team_id, action_now);

        /* Rest-cycle and status-transition tracing for the dashboard's
         * Recovery/Rest tab (app.py's DIAG_STATUS_RE / DIAG_RESTED_RE).
         * Read AFTER recovery_dispatch() runs, not before, since dispatch
         * is what actually applies status changes like RESTING for this
         * tick - reading status earlier in this loop would miss them
         * until the following tick.
         *
         * Gated behind g_simulation_trace_enabled (off by default via
         * sim_runner_init()) so a plain `./tron` run stays silent while
         * it's actually running - previously this printed unconditionally
         * on every status change, which flooded a terminal running the
         * binary directly. app.py passes --trace when it launches the
         * binary for the dashboard, so the Recovery/Rest tab keeps
         * getting this data; only bare CLI usage goes quiet. */
        uint8_t status_now = g_teams[team_id].status;
        if(status_now != g_diag_prev_status[team_id])
        {
            if(g_simulation_trace_enabled)
                debug_trace(
                    "[DIAG] tick=%u team=%u status %u->%u stamina=%.2f hours=%u\n",
                    g_current_tick, team_id, g_diag_prev_status[team_id], status_now,
                    g_teams[team_id].stamina, g_teams[team_id].hours_worked);

            if(status_now == STATUS_RESTING)
            {
                g_diag_rest_start_stamina[team_id] = g_teams[team_id].stamina;
                g_diag_rest_start_tick[team_id] = g_current_tick;
            }
            else if(g_diag_prev_status[team_id] == STATUS_RESTING)
            {
                if(g_simulation_trace_enabled)
                    debug_trace(
                        "[DIAG] tick=%u team=%u RESTED ticks=%u stamina %.2f->%.2f\n",
                        g_current_tick, team_id,
                        g_current_tick - g_diag_rest_start_tick[team_id],
                        g_diag_rest_start_stamina[team_id], g_teams[team_id].stamina);
            }

            g_diag_prev_status[team_id] = status_now;
        }
    }
}

void move_team(TeamState_t *team) //simulation
{
    if(team->status != STATUS_EN_ROUTE)
        return;

    if(team->fault_state.active[FAULT_CLASS_MOBILITY])
    {
        uint8_t severity = team->fault_state.severity[FAULT_CLASS_MOBILITY];

        if(severity >= 3 && (g_current_tick % 2u) == 0u)
            return;

        if(severity >= 4 && (g_current_tick % 3u) != 0u)
            return;
    }

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

    if(task == NULL || !task->slot_occupied)
    {
        team->assigned_task_pool = TASK_POOL_NONE;
        team->assigned_task_slot = NO_ASSIGNMENT;
        team->status = STATUS_AVAILABLE;
        return;
    }

    int next_x, next_y;
    int pre_step_cost = find_next_path_step(team->loc_x, team->loc_y, task->loc_x, task->loc_y, 0, &next_x, &next_y);
    if(pre_step_cost > 0)
    {
        team->loc_x = next_x;
        team->loc_y = next_y;
        if(g_simulation_trace_enabled) debug_trace(" -> (%d,%d)", team->loc_x, team->loc_y);
        uint16_t step_cost = environment_cell_travel_cost(next_x, next_y);
        if(step_cost >= (uint16_t)pre_step_cost)
            team->cached_remaining_path_cost = 0;
        else
            team->cached_remaining_path_cost = (uint16_t)((uint16_t)pre_step_cost - step_cost);

        team->cached_remaining_path_cost_tick = g_current_tick;
    }

    if(g_simulation_trace_enabled) debug_trace(" | Destination: (%d,%d)\n", task->loc_x, task->loc_y);

    if(team->loc_x == task->loc_x && team->loc_y == task->loc_y)
    {
        if(task->incident_type == INCIDENT_RETURN_TO_SAFE_ZONE)
        {
            complete_return_to_safe_zone_task(team, task);
            return;
        }

        if(team->status != STATUS_ON_SCENE)
        {
            team->status = STATUS_ON_SCENE;
            snapshot_record(g_current_tick, EVENT_TEAM_ON_SCENE, team->team_id);

            if(task->assigned_team == team->team_id)
            {
                task->status = INC_IN_PROGRESS;
            }
        }
    }
}

void move_all_teams(void) //simulation
{
    for(uint8_t i=0;i<NUM_TEAMS;i++)
    {
        move_team(&g_teams[i]);
    }
}

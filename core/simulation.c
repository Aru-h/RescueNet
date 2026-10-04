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
#include "search_zones.h"
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
#include "fault_comms.h"
#include "mission_execution_twin.h"
#include "consensus.h"
#include "fault_predictor_features.h"
#include "feature_pack.h"

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
    g_sim_step_tick = 0;   /* sim_tick() advances this; keep it sane for
                              harnesses that call allocate_all_incidents()
                              directly without going through sim_tick() */
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
    environment_validate_hazard_reachability();
    /* Per-seed reset of search-zone coverage / cycle / reclaim state.
     * search_zones_init() otherwise has no caller, so an in-process
     * multi-seed run (the training-data pipeline, the P1 sweep tool) runs
     * every seed after the first with the previous seed's g_search_zones[]
     * and g_cell_last_covered_tick[][] still in place. */
    search_zones_init();
    initialize_grid();
    generate_teams(seed);
    initialize_evacuation_teams();
    fault_injector_init(seed);
    watchdog_init();
    mission_execution_twin_reset();
    cbba_reset_improvement_counters();
    recovery_manager_init();
    fault_comms_init();
#if VIRTUAL_BUS_ENABLED
    consensus_init(seed);
#endif
    fault_predictor_features_init();

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
    g_sim_step_tick = g_current_tick;   /* keep in sync now that the step's tick is live */
    /* See SimRole_t (simulation.h): world events run everywhere except
     * on a board; team execution runs everywhere except on the laptop
     * mirroring a board mission, where board reports supply it. */
    uint8_t runs_world = (g_sim_role != SIM_ROLE_BOARD);
    uint8_t runs_teams = (g_sim_role != SIM_ROLE_WORLD_MIRROR);

    if(runs_world)
    {
        environment_update_hazard_propagation(g_current_tick);
        generate_incidents_if_needed();
    }
    if(runs_teams)
    {
        update_all_teams();
        move_all_teams();
    }
    if(runs_world)
    {
        /* On the mirror this runs against reported positions: coverage,
         * fog-of-war reveals, cycle completion and the next frontier
         * cell all stay laptop-side, and reach the boards as incident
         * frames. */
        search_sweep_step(g_current_tick);
        escalate_starved_incidents(g_current_tick);
        release_quarantined_tasks(g_current_tick);
    }
    if(runs_teams)
    {
        update_all_incident_work();
    }
    if(runs_world)
    {
        update_all_passive_effects();
    }
    if(runs_teams)
    {
#if VIRTUAL_BUS_ENABLED
        /* After movement and incident work, so the beacon carries this
         * tick's final position and status, the same point the watchdog
         * samples them at. */
        consensus_step(g_current_tick);
#endif
        mission_execution_twin_step();
        watchdog_step();

        /* Track A: history features the fault predictor's input vector
         * needs (stall_frac_last_5/_10, delta_priority/delta_victim_count -
         * see fault_predictor_features.h). Must run after movement and
         * assignment (both already happened earlier this tick - move_
         * all_teams() above, allocate_all_incidents() in sim_tick() before
         * simulation_step() was called) and before recovery_manager_step(),
         * since a fault vector assembled from recovery_manager's evidence
         * this tick should see this tick's stall/delta values, not last
         * tick's. */
        for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
            fault_predictor_features_update_tick(team_id);

        /* Pre-dispatch feature-vector cache (core/feature_pack.h's own
         * doc on why this exists): must run after the history-feature
         * loop just above (so the cached vector includes this tick's
         * stall/delta values) and before recovery_manager_step(), whose
         * evaluate_fault_predictor() is this cache's reader this same
         * tick - any later and it would read last tick's cache instead.
         * Nothing between here and Pass 1's snapshot_record() calls
         * below mutates team/incident/twin state, so this cached vector
         * is exactly what that tick's training-CSV row will reflect. */
        for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
            feature_pack_cache_tick(team_id);

        recovery_manager_step();
    }

    /* Two passes, deliberately. Pass 1 records every team's snapshot rows
     * for this tick BEFORE any recovery_dispatch() runs, so a team's logged
     * row can never reflect another team's same-tick dispatch side effects
     * (dispatch can call allocate_all_incidents(), reassign tasks, etc.).
     * Pass 2 then runs the dispatches (unchanged order t=0..NUM_TEAMS-1, so
     * simulation behaviour is identical) plus the post-dispatch status
     * tracing that genuinely needs to read state after dispatch. */
    RecoveryAction_t action_now[NUM_TEAMS];

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        uint8_t anomaly_now = watchdog_get_status(team_id).anomaly_detected;
        action_now[team_id] = recovery_manager_get_status(team_id).recommendation;

        /* Unconditional per-team, per-tick snapshot row. */
        snapshot_record(g_current_tick, EVENT_TICK, team_id);

        if(anomaly_now != g_prev_anomaly_detected[team_id])
        {
            snapshot_record(g_current_tick, EVENT_WATCHDOG_ANOMALY_CHANGED, team_id);
            g_prev_anomaly_detected[team_id] = anomaly_now;
        }

        if((uint8_t)action_now[team_id] != g_prev_recovery_action[team_id])
        {
            snapshot_record(g_current_tick, EVENT_RECOVERY_ACTION_CHANGED, team_id);
            g_prev_recovery_action[team_id] = (uint8_t)action_now[team_id];
        }
    }

#if VIRTUAL_BUS_ENABLED
    if(runs_teams)
        snapshot_record_peer_obs(g_current_tick);
#endif

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        if(runs_teams)
            recovery_dispatch(team_id, action_now[team_id]);

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

unsigned long g_probe_no_path_hits = 0;
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

    /* Causal profile: hidden wear shows up as an occasional lost step
     * (the only symptom of wear a model can observe). */
    if(fault_profile_is_causal() && fault_injector_causal_symptom_skip(team->team_id))
        return;

    if(team->heading_to_rest)
    {
        SafeZone_t *safezone = &g_safezones[team->resting_zone_id];

        int next_x, next_y;
        uint16_t remaining_after_step = 0;
        int pre_step_cost = find_next_path_step_remaining(team->loc_x, team->loc_y, safezone->loc_x, safezone->loc_y, &next_x, &next_y, &remaining_after_step);

        if(pre_step_cost <= 0)
        {
            /* The route this team was given (at begin_return_to_safe_zone()
             * time) has since become unreachable - almost always a hazard
             * (aftershock collapse, road damage) cutting the path after the
             * fact, not at selection time (find_nearest_safe_zone() already
             * checks reachability then). Without this, the team simply
             * freezes here forever: stamina pinned at whatever it was,
             * heading_to_rest never clears, EVENT_TEAM_REST fires every
             * remaining tick of the run. This is not new to heading_to_rest
             * - the old task-based RETURN_TO_SAFE_ZONE had the identical
             * gap (move_team()'s old task-based branch below has the same
             * "if no path, do nothing" shape) - it's just easier to hit
             * once a route can be lost mid-trip under active fault
             * injection, which is when this was actually caught. */
            uint8_t alternate = find_nearest_safe_zone(team);

            if(alternate != NO_SAFE_ZONE)
            {
                team->resting_zone_id = alternate;
            }
            else
            {
                /* True dead end - nowhere reachable at all. Same last-resort
                 * reasoning team.c's STATUS_AVAILABLE case already uses when
                 * begin_return_to_safe_zone() itself returns NO_ASSIGNMENT:
                 * let the team recover in place rather than sit frozen and
                 * unable to ever reach STATUS_RESTING. */
                complete_return_to_rest(team);
            }

            return;
        }

        team->loc_x = next_x;
        team->loc_y = next_y;
        if(g_simulation_trace_enabled) debug_trace(" -> (%d,%d)", team->loc_x, team->loc_y);
        /* From the pathfinder, not pre_step_cost minus the cell's travel
         * cost: a diagonal step costs more than the cell it enters. */
        team->cached_remaining_path_cost = remaining_after_step;
        team->cached_remaining_path_cost_tick = g_current_tick;

        if(g_simulation_trace_enabled) debug_trace(" | Destination: (%d,%d)\n", safezone->loc_x, safezone->loc_y);

        if(team->loc_x == safezone->loc_x && team->loc_y == safezone->loc_y)
        {
            complete_return_to_rest(team);
        }

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
    uint16_t remaining_after_step = 0;
    int pre_step_cost = find_next_path_step_remaining(team->loc_x, team->loc_y, task->loc_x, task->loc_y, &next_x, &next_y, &remaining_after_step);
    if(pre_step_cost > 0)
    {
        team->loc_x = next_x;
        team->loc_y = next_y;
        if(g_simulation_trace_enabled) debug_trace(" -> (%d,%d)", team->loc_x, team->loc_y);
        team->cached_remaining_path_cost = remaining_after_step;
        team->cached_remaining_path_cost_tick = g_current_tick;
    }
    else
    {
        /* Same gap the heading_to_rest branch above had, and for the
         * same reason: the route this team was given at assignment time
         * (allocate_all_incidents()/create_equipment_cache_incident()/
         * search_sweep_step()'s UNSEARCHED_SECTOR dispatch/etc.) has
         * since become unreachable - a hazard cutting the path after
         * the fact. Without this, the team freezes here forever: never
         * arrives, task never completes or gets released, status stuck
         * at EN_ROUTE indefinitely with nothing active to explain it in
         * telemetry.
         *
         * Unlike the heading_to_rest case there's no alternate route to
         * try - this destination is a specific incident/system task, not
         * "any safe zone" - so there's nothing to re-target the way
         * find_nearest_safe_zone() re-targets a safe zone. The
         * equivalent move is to give up on this task exactly like every
         * other "task became unreachable/lost mid-EN_ROUTE" path in this
         * codebase already does (team.c's STATUS_EN_ROUTE stamina-
         * exhaustion case is the same shape): release it back to
         * INC_OPEN via abandon_task_for_recovery(), which itself tries
         * begin_return_to_safe_zone() and only falls back to
         * team_set_available() - tracked via g_diag_safezone_fallback_
         * count, not silently - if no safe zone is reachable either.
         * That fallback chain is what already backs every task-loss path
         * here, so this doesn't touch fault_state timing (start_tick/
         * duration_ticks/immunity windows live entirely in
         * fault_injector.c, untouched by abandon_task_for_recovery()) and
         * is the same EN_ROUTE->pool-change transition the watchdog's
         * assignment_evidence() is already built to score rather than
         * something novel for confirmed_mobility_impairment to trip on. */
        { extern unsigned long g_probe_no_path_hits; g_probe_no_path_hits++; }
        /* Track consecutive no-path abandons so abandon_task_for_recovery()
         * can quarantine a structurally-unreachable incident instead of
         * re-opening it for another doomed bid/abandon cycle. */
        /* L8: only the team that OWNS the task counts as evidence about
         * the task. A support team (task->support_team) failing to route
         * says something about that team's position, not the incident. */
        if(task->assigned_team == team->team_id && task->unreachable_streak < 255)
            task->unreachable_streak++;
        abandon_task_for_recovery(team);
        return;
    }

    if(g_simulation_trace_enabled) debug_trace(" | Destination: (%d,%d)\n", task->loc_x, task->loc_y);

    if(team->loc_x == task->loc_x && team->loc_y == task->loc_y)
    {
        task->unreachable_streak = 0;   /* reached it - not unreachable */

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

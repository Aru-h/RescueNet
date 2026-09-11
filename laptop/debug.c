#include "mission_state.h"
#include "incident.h"
#include "mission_state_utils.h"
#include "grid.h"
#include "random.h"
#include <stdio.h>
#include <stdarg.h>
#include "debug.h"
#include "simulation.h"
#include "environment.h"
#include "sim_runner.h"
#include "fault_config.h"
#include "path.h"

/*
 * Laptop implementation of the debug_trace() sink declared in debug.h.
 * Mirrors printf() so core/ trace call sites don't need platform
 * knowledge. The board implementation (board/STM32N6570-DK/debug.c) routes
 * the same calls to a UART/USB log sink instead of stdout.
 */
void debug_trace(const char *fmt, ...)
{
    /* stderr, not stdout: [DIAG ...] lines elsewhere in this codebase
     * (main.c's summary/work/throughput prints, simulation.c's
     * rest-cycle tracing below) all go to stderr, and app.py's batch
     * (non---live) endpoint captures stdout/stderr separately
     * (subprocess.run(..., capture_output=True)) then parses only
     * result.stderr for [DIAG ...] lines. --live mode merges the two
     * streams (stderr=subprocess.STDOUT) so it doesn't care either
     * way, but batch mode does - stdout here would silently vanish
     * from what the dashboard parses. */
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static const char *generator_load_to_string(uint8_t load)
{
    switch(load)
    {
        case 0: return "IDLE";
        case 1: return "LOW";
        case 2: return "MODERATE";
        case 3: return "HIGH";
        case 4: return "CRITICAL";
        default: return "UNKNOWN";
    }
}

void debug_print_generator_evaluation(uint32_t tick,
                                      uint8_t load,
                                      uint32_t probability,
                                      uint32_t roll,
                                      uint8_t spawned,
                                      uint8_t available_teams,
                                      uint8_t en_route_teams,
                                      uint8_t on_scene_teams,
                                      uint8_t resting_teams,
                                      uint8_t open_incidents,
                                      uint8_t assigned_incidents,
                                      uint8_t in_progress_incidents,
                                      uint8_t active_system_tasks,
                                      uint16_t degraded_cells,
                                      uint32_t next_evaluation_tick)
{
    if(!g_simulation_trace_enabled) return;

    printf("[GEN] tick=%lu load=%s prob=%lu roll=%lu spawn=%s | teams(A=%u,E=%u,O=%u,R=%u) | incidents(O=%u,A=%u,P=%u) | system=%u degraded=%u next=%lu\n",
           (unsigned long)tick,
           generator_load_to_string(load),
           (unsigned long)probability,
           (unsigned long)roll,
           spawned ? "YES" : "NO",
           available_teams,
           en_route_teams,
           on_scene_teams,
           resting_teams,
           open_incidents,
           assigned_incidents,
           in_progress_incidents,
           active_system_tasks,
           degraded_cells,
           (unsigned long)next_evaluation_tick);
}

void debug_print_generator_selection(const uint32_t *weights,
                                     uint32_t weight_count,
                                     uint32_t total_weight,
                                     uint32_t type_roll,
                                     uint8_t selected_type)
{
    if(!g_simulation_trace_enabled) return;

    printf("[GEN-TYPE] weights=[");

    for(uint32_t i = 0; i < weight_count; i++)
    {
        printf("%s%lu",
               i == 0 ? "" : ",",
               (unsigned long)weights[i]);
    }

    printf("] total=%lu roll=%lu selected_type=%u\n",
           (unsigned long)total_weight,
           (unsigned long)type_roll,
           selected_type);
}


/*
 * Static layout - buildings, roads, districts don't move once
 * generated, so this is sent exactly once per run, not every tick, to
 * keep LIVEFRAME payloads small. Desktop test tooling only (used by
 * the live dashboard) - not part of the firmware port surface, so no
 * platform.h wrapping.
 */
void debug_emit_static_environment(void)
{
    printf("STATICENV {\"buildings\":[");
    uint8_t first_b = 1;
    for(uint16_t i = 0; i < MAX_ENV_BUILDINGS; i++)
    {
        const EnvironmentBuilding_t *b = &g_environment_buildings[i];
        if(b->footprint == 0)
            continue; /* unused slot */
        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"w\":%u,\"h\":%u,\"type\":%u,"
               "\"floors\":%u,\"damage\":%u,\"risk\":%u,\"hazard_flags\":%u}",
               first_b ? "" : ",",
               b->building_id, b->loc_x, b->loc_y, b->footprint_width, b->footprint_height,
               b->building_type, b->floors, b->damage_state, b->risk_level, b->hazard_flags);
        first_b = 0;
    }
    printf("],\"epicenter\":{\"active\":%u,\"x\":%d,\"y\":%d,\"intensity\":%u}}\n",
           g_environment_summary.earthquake_applied,
           g_environment_summary.earthquake_epicenter_x,
           g_environment_summary.earthquake_epicenter_y,
           g_environment_summary.earthquake_intensity);
    fflush(stdout);
}

/* Emits one line of JSON to stdout describing the full world state for
 * this tick: every team's position/status/stamina/fault info, and every
 * active incident + system task. Desktop test tooling only (used by the
 * live dashboard) - not part of the firmware port surface, so no
 * platform.h wrapping. Caller is responsible for pacing (sleep) between
 * ticks if a human needs to watch it happen instead of it flying by. */
void debug_emit_live_frame(uint32_t tick)
{
    extern uint32_t g_diag_incidents_resolved[NUM_TEAMS];
    extern uint32_t g_diag_stage_completions[NUM_TEAMS];
    extern uint32_t g_diag_work_progress_calls;
    extern uint32_t g_diag_in_progress_entries;
    extern uint32_t g_diag_ticks_resting[NUM_TEAMS];
    extern uint32_t g_diag_ticks_working[NUM_TEAMS];
    extern uint32_t g_diag_ticks_available[NUM_TEAMS];
    extern uint32_t g_diag_safezone_fallback_count;
    extern uint32_t g_diag_discarded_signal_count;

    /* Cumulative, monotonic totals across all teams - these are the
       authoritative counters the sim itself tracks (incident.c,
       recovery_manager.c). Sent every tick so the frontend can build a
       live throughput history without waiting for end-of-run [DIAG]
       output. Per-team breakdowns are included on each team object
       below rather than duplicated here. */
    uint32_t total_incidents_resolved = 0, total_stage_completions = 0;
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        total_incidents_resolved += g_diag_incidents_resolved[t];
        total_stage_completions += g_diag_stage_completions[t];
    }

    printf("LIVEFRAME {\"tick\":%u,"
           "\"incidents_resolved\":%u,\"stage_completions\":%u,"
           "\"work_progress_calls\":%u,\"in_progress_entries\":%u,"
           "\"safezone_fallback_count\":%u,\"discarded_signal_count\":%u,"
           "\"teams\":[",
           tick, total_incidents_resolved, total_stage_completions,
           g_diag_work_progress_calls, g_diag_in_progress_entries,
           g_diag_safezone_fallback_count, g_diag_discarded_signal_count);
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        const TeamState_t *team = &g_teams[t];

        /* Resolve the task this team is currently assigned to (if any) so
           the dashboard can show "where is this team actually headed"
           without duplicating incident-lookup logic client-side. */
        int16_t dest_x = -1, dest_y = -1;
        uint16_t dest_id = 0;
        uint8_t has_dest = 0;
        if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
           team->assigned_task_slot < NUM_INCIDENTS &&
           g_incidents[team->assigned_task_slot].slot_occupied)
        {
            const IncidentState_t *task = &g_incidents[team->assigned_task_slot];
            dest_x = task->loc_x;
            dest_y = task->loc_y;
            dest_id = task->incident_id;
            has_dest = 1;
        }
        else if(team->assigned_task_pool == TASK_POOL_SYSTEM &&
                team->assigned_task_slot < NUM_SYSTEM_TASKS &&
                g_system_tasks[team->assigned_task_slot].slot_occupied)
        {
            const IncidentState_t *task = &g_system_tasks[team->assigned_task_slot];
            dest_x = task->loc_x;
            dest_y = task->loc_y;
            dest_id = task->incident_id;
            has_dest = 1;
        }

        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"stamina\":%.2f,\"status\":%u,"
               "\"spec\":%u,\"pool\":%u,\"slot\":%u,\"fault_mask\":%u,"
               "\"degraded\":%u,\"hours\":%u,\"has_dest\":%u,\"dest_x\":%d,"
               "\"dest_y\":%d,\"dest_id\":%u,\"incidents_resolved\":%u,"
               "\"stage_completions\":%u,\"ticks_resting\":%u,"
               "\"ticks_working\":%u,\"ticks_available\":%u}",
               t == 0 ? "" : ",",
               team->team_id, team->loc_x, team->loc_y, team->stamina,
               team->status, team->specialization,
               team->assigned_task_pool, team->assigned_task_slot,
               team->fault_state.active_mask, team->degraded,
               team->hours_worked, has_dest, dest_x, dest_y, dest_id,
               g_diag_incidents_resolved[t], g_diag_stage_completions[t],
               g_diag_ticks_resting[t], g_diag_ticks_working[t],
               g_diag_ticks_available[t]);
    }
    printf("],\"incidents\":[");
    uint8_t first = 1;
    for(uint16_t i = 0; i < NUM_INCIDENTS; i++)
    {
        const IncidentState_t *inc = &g_incidents[i];
        if(!inc->slot_occupied)
            continue;
        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"priority\":%u,\"type\":%u,"
               "\"status\":%u,\"assigned_team\":%u,\"support_team\":%u,"
               "\"req_capability\":%u,\"victims\":%u,\"work_remaining\":%u,"
               "\"discovered\":%u,"
               "\"pool\":0}",
               first ? "" : ",",
               inc->incident_id, inc->loc_x, inc->loc_y, inc->priority,
               inc->incident_type, inc->status, inc->assigned_team,
               inc->support_team, inc->req_capability, inc->victim_count,
               inc->work_remaining, inc->discovered);
        first = 0;
    }
    for(uint16_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        const IncidentState_t *inc = &g_system_tasks[i];
        if(!inc->slot_occupied)
            continue;
        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"priority\":%u,\"type\":%u,"
               "\"status\":%u,\"assigned_team\":%u,\"support_team\":%u,"
               "\"req_capability\":%u,\"victims\":%u,\"work_remaining\":%u,"
               "\"discovered\":%u,"
               "\"pool\":1}",
               first ? "" : ",",
               inc->incident_id, inc->loc_x, inc->loc_y, inc->priority,
               inc->incident_type, inc->status, inc->assigned_team,
               inc->support_team, inc->req_capability, inc->victim_count,
               inc->work_remaining, inc->discovered);
        first = 0;
    }
    printf("],\"safezones\":[");
    for(uint8_t s = 0; s < MAX_SAFE_ZONES; s++)
    {
        const SafeZone_t *sz = &g_safezones[s];
        printf("%s{\"id\":%u,\"x\":%d,\"y\":%d,\"capacity\":%u,\"evacuees\":%u}",
               s == 0 ? "" : ",",
               sz->safezone_id, sz->loc_x, sz->loc_y, sz->capacity, sz->evacuee_count);
    }
    printf("],\"hazard_cells\":[");
    /* Only cells that are actually blocked/damaged/hazardous are sent -
     * out of GRID_WIDTH*GRID_HEIGHT (1296) cells, sending all of them
     * every tick would bloat the frame for mostly-static terrain. Only
     * the exceptional ones are worth the map drawing anything special. */
    uint8_t first_h = 1;
    for(uint8_t gx = 0; gx < GRID_WIDTH; gx++)
    {
        for(uint8_t gy = 0; gy < GRID_HEIGHT; gy++)
        {
            const EnvironmentCell_t *cell = &g_environment[gx][gy];
            /* Deliberately a higher bar than "any damage at all" - the
             * earthquake leaves low-level road_damage almost everywhere,
             * which would swamp the map with noise. Only genuinely
             * actionable conditions are sent: actually blocked, an
             * active hazard flag, or damage severe enough to matter
             * (>=50, same rough scale as the 0-255 damage fields
             * elsewhere in this codebase using a meaningful-threshold
             * convention). */
            if(!cell->road_blocked && cell->hazard_flags == 0 &&
               cell->road_damage < 50 && cell->infrastructure_damage < 50)
            {
                continue;
            }
            printf("%s{\"x\":%u,\"y\":%u,\"blocked\":%u,\"hazard_flags\":%u,"
                   "\"road_damage\":%u,\"infra_damage\":%u}",
                   first_h ? "" : ",",
                   gx, gy, cell->road_blocked, cell->hazard_flags,
                   cell->road_damage, cell->infrastructure_damage);
            first_h = 0;
        }
    }
    printf("]}\n");
    fflush(stdout);
}

void print_world_state(void) //debug
{
    printf("Environment | Seed: %lu | Roads: %u | Navigable: %u | Buildings: %u | Damaged Buildings: %u | Blocked Roads: %u | Damaged Infrastructure: %u | Bridges: %u | Nav Nodes: %u | Nav Edges: %u | Validated: %s\n",
           (unsigned long)g_environment_summary.seed,
           g_environment_summary.road_cell_count,
           g_environment_summary.navigable_cell_count,
           g_environment_summary.building_count,
           g_environment_summary.damaged_building_count,
           g_environment_summary.blocked_road_count,
           g_environment_summary.damaged_infrastructure_count,
           g_environment_summary.bridge_count,
           g_environment_summary.nav_node_count,
           g_environment_summary.nav_edge_count,
           g_environment_summary.validation_passed ? "YES" : "NO");

    if(g_environment_summary.earthquake_applied)
    {
        printf("Earthquake | Epicenter: (%d,%d) | Intensity: %u\n",
               g_environment_summary.earthquake_epicenter_x,
               g_environment_summary.earthquake_epicenter_y,
               g_environment_summary.earthquake_intensity);
    }

    printf("Hazard Propagation | Fire Spread: %lu | Gas Explosions: %lu | Aftershock Collapses: %lu | Road Cost Increases: %lu\n",
           (unsigned long)g_environment_summary.fire_spread_count,
           (unsigned long)g_environment_summary.gas_explosion_count,
           (unsigned long)g_environment_summary.aftershock_collapse_count,
           (unsigned long)g_environment_summary.road_degradation_count);

    for(uint8_t district = 0; district < NUM_DISTRICT_TYPES; district++)
    {
        printf("District %-12s | Cells: %u\n",
               environment_district_name(district),
               g_environment_summary.district_count[district]);
    }

    for(int i = 0; i < NUM_TEAMS; i++)
    {
        printf("Team %d | Tier: %d | Status: %s%s | Stamina: %.3f | Equipment: %d\n",
               i,
               g_teams[i].fitness_tier,
               team_status_to_string(g_teams[i].status),
               g_teams[i].degraded ? " (DEGRADED)" : "",
               g_teams[i].stamina,
               g_teams[i].equipment_level);
    }

    for(int i = 0; i < NUM_INCIDENTS; i++)
    {
        if(!g_incidents[i].slot_occupied)
            continue;

        printf("Incident ID: %lu | Slot: %d | Type: %d | Status: %s | Assigned: %d | Spawned By: %d | Loc: (%d,%d)\n",
               (unsigned long)g_incidents[i].incident_id,
               i,
               g_incidents[i].incident_type,
               incident_status_to_string(g_incidents[i].status),
               g_incidents[i].assigned_team,
               g_incidents[i].spawned_by,
               g_incidents[i].loc_x,
               g_incidents[i].loc_y);
    }

    for(int i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        if(!g_system_tasks[i].slot_occupied)
            continue;

        printf("System Task ID: %lu | Slot: %d | Type: %d | Status: %s | Assigned: %d | Spawned By: %d | Loc: (%d,%d)\n",
               (unsigned long)g_system_tasks[i].incident_id,
               i,
               g_system_tasks[i].incident_type,
               incident_status_to_string(g_system_tasks[i].status),
               g_system_tasks[i].assigned_team,
               g_system_tasks[i].spawned_by,
               g_system_tasks[i].loc_x,
               g_system_tasks[i].loc_y);
    }


    for(int i = 0; i < g_num_safe_zones; i++)
    {
        printf(
            "Safe Zone %d | Evacuees: %d/%d | Loc: (%d,%d)\n",
            i,
            g_safezones[i].evacuee_count,
            g_safezones[i].capacity,
            g_safezones[i].loc_x,
            g_safezones[i].loc_y
        );
    }
}

void initialize_test_scenario(void)         //debug
{
    g_world_seed = 1234;
    g_current_tick = 0;
    random_seed_all(g_world_seed);
    generate_environment(g_world_seed);
    initialize_incidents();

    for(int i = 0; i < NUM_TEAMS; i++)
    {
        g_teams[i].team_id = i;
        g_teams[i].specialization = i;
        g_teams[i].status = STATUS_RESTING;
        g_teams[i].assigned_task_pool = TASK_POOL_NONE;
        g_teams[i].assigned_task_slot = NO_ASSIGNMENT;
        g_teams[i].incident_queue_size = 0;
        g_teams[i].incident_queue_index = 0;
        for(uint8_t q = 0; q < CBBA_MAX_BUNDLE_SIZE; q++)
        {
            g_teams[i].incident_queue_slot[q] = NO_ASSIGNMENT;
            g_teams[i].incident_queue_id[q] = 0;
        }
        g_teams[i].loc_x = 2;
        g_teams[i].loc_y = 2;
        g_teams[i].stamina = 100;
        g_teams[i].hours_worked = 0;
        g_teams[i].ticks_worked_this_period = 0;
        g_teams[i].equipment_flags = 0;
        g_teams[i].last_heartbeat_tick = 0;
        g_teams[i].resting_zone_id = 0;
        g_teams[i].heading_to_rest = 0;
        g_teams[i].equipment_level = EQ_MAX_LEVEL;

        switch(g_teams[i].specialization)
        {
            case SPEC_EXTRACTION:
                g_teams[i].fitness_tier = ARDUOUS;
                g_teams[i].stamina = ARDUOUS_MIN_STAMINA;
                break;

            case SPEC_SEARCH:
            case SPEC_PERIMETER:
                g_teams[i].fitness_tier = MODERATE;
                g_teams[i].stamina = MODERATE_MIN_STAMINA;
                break;

            case SPEC_MEDICAL:
            case SPEC_SUPPORT:
                g_teams[i].fitness_tier = LIGHT;
                g_teams[i].stamina = LIGHT_MIN_STAMINA;
                break;
        }
    }

    g_teams[0].status = STATUS_ON_SCENE;
    g_teams[0].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[0].assigned_task_slot = 0;
    g_teams[0].equipment_level = 1;

    g_incidents[0].incident_id = allocate_incident_id();
    g_incidents[0].slot_occupied = 1;
    g_incidents[0].status = INC_IN_PROGRESS;
    g_incidents[0].assigned_team = 0;
    g_incidents[0].priority = PRIORITY_HIGH;
    g_incidents[0].req_capability = SPEC_SEARCH;
    g_incidents[0].loc_x = 2;
    g_incidents[0].loc_y = 2;
    g_incidents[0].work_remaining = 100;
    g_incidents[0].incident_type = INCIDENT_UNSEARCHED_SECTOR;

    g_num_safe_zones = 1;
    g_safezones[0].safezone_id = 0;
    g_safezones[0].loc_x = 2;
    g_safezones[0].loc_y = 2;
    g_safezones[0].capacity = 20;
    g_safezones[0].evacuee_count = 16;
    environment_register_safe_zone(0, g_safezones[0].loc_x, g_safezones[0].loc_y);

    for(int i = 0; i < MAX_SAFE_ZONES; i++)
    {
        g_safezone_evacuation_active[i] = 0;
    }

    initialize_evacuation_gateways(1234);
    environment_apply_earthquake(g_world_seed);
    initialize_grid();
    initialize_evacuation_teams();
}

int debug_run_priority1_repro(void)         //debug
{
    initialize_test_scenario();

    /* initialize_test_scenario() parks team 0 ON_SCENE at the same
     * cell as its incident (2,2) - fine for other debug uses, but this
     * harness needs a team actually traveling so it can have its path
     * cut mid-route. Retarget the incident somewhere far across the
     * generated map and put the team EN_ROUTE to it. */
    g_incidents[0].loc_x = 25;
    g_incidents[0].loc_y = 25;
    g_teams[0].status = STATUS_EN_ROUTE;
    g_teams[0].heading_to_rest = 0;

    TeamState_t *team = &g_teams[0];
    IncidentState_t *task = &g_incidents[0];

    int nx, ny;
    int initial_cost = find_next_path_step(team->loc_x, team->loc_y, task->loc_x, task->loc_y, 0, &nx, &ny);
    printf("[REPRO P1] sanity: path (2,2)->(25,25) cost=%d before any wall-off\n", initial_cost);
    if(initial_cost <= 0)
    {
        printf("[REPRO P1] INVALID: no path even before wall-off on this map - can't run repro.\n");
        return 2;
    }

    /* Let the team actually travel for a few real ticks on the still-
     * open path first, so this matches "a road gets damaged while a
     * team's already traveling" rather than "destination was
     * unreachable from the moment it was assigned". */
    g_current_tick = 1;
    for(int t = 0; t < 5; t++)
    {
        move_team(team);
        g_current_tick++;
    }
    printf("[REPRO P1] after 5 real ticks of travel: team now at (%d,%d) (started at (2,2))\n",
           team->loc_x, team->loc_y);
    if(team->loc_x == 2 && team->loc_y == 2)
    {
        printf("[REPRO P1] INVALID: team never left the start cell in 5 ticks.\n");
        return 4;
    }

    /* Sever the path out from under the already-traveling team: wall
     * off a full ring around the destination so no route through it
     * survives, regardless of where the team currently stands. */
    for(int x = 20; x <= 30; x++)
    {
        for(int y = 20; y <= 30; y++)
        {
            if(x == 20 || x == 30 || y == 20 || y == 30)
                g_environment[x][y].navigable = 0;
        }
    }

    int post_wall_cost = find_next_path_step(team->loc_x, team->loc_y, task->loc_x, task->loc_y, 0, &nx, &ny);
    printf("[REPRO P1] after mid-route wall-off: find_next_path_step returns %d (<=0 = unreachable, as intended)\n",
           post_wall_cost);
    if(post_wall_cost > 0)
    {
        printf("[REPRO P1] INVALID: destination still reachable after wall-off - widen the ring.\n");
        return 3;
    }

    uint8_t start_pool = team->assigned_task_pool;
    uint8_t start_slot = team->assigned_task_slot;
    int start_x = team->loc_x, start_y = team->loc_y;
    uint8_t start_status = team->status;

    extern unsigned long g_probe_no_path_hits;
    extern uint32_t g_diag_safezone_fallback_count;

    int recovered_at_tick = -1;
    for(uint32_t t = 1; t <= 2000; t++)
    {
        g_current_tick++;
        move_team(team);

        int moved = (team->loc_x != start_x || team->loc_y != start_y);
        int pool_changed = (team->assigned_task_pool != start_pool || team->assigned_task_slot != start_slot);
        int status_changed = (team->status != start_status);

        if(moved || pool_changed || status_changed)
        {
            recovered_at_tick = (int)t;
            break;
        }
    }

    printf("[REPRO P1] g_probe_no_path_hits=%lu g_diag_safezone_fallback_count=%u\n",
           g_probe_no_path_hits, g_diag_safezone_fallback_count);
    printf("[REPRO P1] final: status=%u (was %u) pool/slot=%u/%u (was %u/%u) loc=(%d,%d) (was (%d,%d))\n",
           team->status, start_status, team->assigned_task_pool, team->assigned_task_slot,
           start_pool, start_slot, team->loc_x, team->loc_y, start_x, start_y);

    if(recovered_at_tick > 0)
    {
        printf("[REPRO P1] VERDICT: FIXED - un-stuck at tick %d (within 2000-tick cap).\n", recovered_at_tick);
        return 0;
    }

    printf("[REPRO P1] VERDICT: BUG REPRODUCED - frozen for the full 2000-tick budget.\n");
    return 10;
}

/* Any single cell's travel_cost is a uint8_t (max 255) and A* takes at
 * most one cell's cost worth of ticks to clear a single step. 600 ticks
 * frozen at the same (x,y) while EN_ROUTE is therefore not "an
 * unusually expensive step" under any cost value this codebase can
 * produce - it can only mean the team stopped making progress, which
 * is exactly the pre-fix move_team() task-branch signature. Padding
 * well above the theoretical max (255) rather than cutting it close
 * avoids flagging a legitimately slow-but-moving team as frozen. */
#define P1_SWEEP_STUCK_TICKS_THRESHOLD 10
#define P1_SWEEP_MAX_REPORTED_STREAKS 64

typedef struct
{
    uint32_t seed;
    uint8_t team_id;
    uint32_t start_tick;
    uint32_t streak_len;
} FrozenStreak_t;

int debug_run_priority1_sweep(uint32_t first_seed, uint32_t seed_count, uint32_t ticks_per_seed, int fault_profile_int)
{
    FaultProfile_t fault_profile = (FaultProfile_t)fault_profile_int;

    extern unsigned long g_probe_no_path_hits;
    extern uint32_t g_diag_safezone_fallback_count;
    extern unsigned long g_diag_carve_repair_count;
    extern unsigned long g_diag_carve_repair_failed;
    extern unsigned long g_diag_incidents_quarantined;
    extern uint32_t g_diag_sweep_cross_district_cells;
    extern unsigned long g_diag_gateway_degenerate_worlds;
    extern unsigned long g_diag_gateway_repaired;
    extern unsigned long g_diag_safezone_maxattempts_fallback;
    extern unsigned long g_diag_team_score_ties;

    uint32_t per_team_streak[NUM_TEAMS];
    int16_t per_team_last_x[NUM_TEAMS];
    int16_t per_team_last_y[NUM_TEAMS];
    uint32_t per_team_streak_start[NUM_TEAMS];

    FrozenStreak_t flagged[P1_SWEEP_MAX_REPORTED_STREAKS];
    uint32_t flagged_count = 0;
    uint32_t max_streak_seen = 0;

    unsigned long probe_hits_before_sweep = g_probe_no_path_hits;
    uint32_t fallback_before_sweep = g_diag_safezone_fallback_count;
    unsigned long carve_repair_before_sweep = g_diag_carve_repair_count;
    unsigned long carve_failed_before_sweep = g_diag_carve_repair_failed;
    unsigned long quarantined_before_sweep = g_diag_incidents_quarantined;
    uint32_t cross_district_before_sweep = g_diag_sweep_cross_district_cells;
    unsigned long gateway_degenerate_before_sweep = g_diag_gateway_degenerate_worlds;
    unsigned long gateway_repaired_before_sweep = g_diag_gateway_repaired;
    unsigned long safezone_fallback_before_sweep = g_diag_safezone_maxattempts_fallback;
    unsigned long team_score_ties_before_sweep = g_diag_team_score_ties;

    printf("[SWEEP P1] starting: seeds %u..%u (%u seeds), %u ticks/seed, fault_profile=%d\n",
           first_seed, first_seed + seed_count - 1, seed_count, ticks_per_seed, (int)fault_profile);
    printf("[SWEEP P1] stuck threshold: %u consecutive ticks EN_ROUTE at the same (x,y)\n",
           P1_SWEEP_STUCK_TICKS_THRESHOLD);

    for(uint32_t seed = first_seed; seed < first_seed + seed_count; seed++)
    {
        sim_start_seed(seed, fault_profile);

        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            per_team_streak[t] = 0;
            per_team_last_x[t] = -1;
            per_team_last_y[t] = -1;
            per_team_streak_start[t] = 0;
        }

        for(uint32_t tick = 0; tick < ticks_per_seed; tick++)
        {
            sim_tick();

            for(uint8_t t = 0; t < NUM_TEAMS; t++)
            {
                const TeamState_t *team = &g_teams[t];

                if(team->status != STATUS_EN_ROUTE)
                {
                    per_team_streak[t] = 0;
                    per_team_last_x[t] = -1;
                    per_team_last_y[t] = -1;
                    continue;
                }

                if(team->loc_x == per_team_last_x[t] && team->loc_y == per_team_last_y[t])
                {
                    per_team_streak[t]++;
                }
                else
                {
                    per_team_last_x[t] = team->loc_x;
                    per_team_last_y[t] = team->loc_y;
                    per_team_streak[t] = 1;
                    per_team_streak_start[t] = tick;
                }

                if(per_team_streak[t] > max_streak_seen)
                    max_streak_seen = per_team_streak[t];

                if(per_team_streak[t] == P1_SWEEP_STUCK_TICKS_THRESHOLD)
                {
                    printf("[SWEEP P1] FROZEN: seed=%u team=%u stuck at (%d,%d) since tick=%u, "
                           "still EN_ROUTE with no movement at tick=%u (%u ticks and counting)\n",
                           seed, t, team->loc_x, team->loc_y, per_team_streak_start[t], tick,
                           per_team_streak[t]);

                    if(flagged_count < P1_SWEEP_MAX_REPORTED_STREAKS)
                    {
                        flagged[flagged_count].seed = seed;
                        flagged[flagged_count].team_id = t;
                        flagged[flagged_count].start_tick = per_team_streak_start[t];
                        flagged[flagged_count].streak_len = per_team_streak[t];
                        flagged_count++;
                    }
                }
            }
        }

        sim_end_seed();

        if((seed - first_seed + 1) % 10 == 0 || seed == first_seed + seed_count - 1)
        {
            printf("[SWEEP P1] progress: %u/%u seeds done, %u frozen streaks so far, "
                   "max streak so far=%u, cumulative no_path_hits=%lu, safezone_fallback=%u\n",
                   seed - first_seed + 1, seed_count, flagged_count, max_streak_seen,
                   g_probe_no_path_hits, g_diag_safezone_fallback_count);
        }
    }

    unsigned long probe_hits_in_sweep = g_probe_no_path_hits - probe_hits_before_sweep;
    uint32_t fallback_in_sweep = g_diag_safezone_fallback_count - fallback_before_sweep;

    printf("\n[SWEEP P1] === SUMMARY ===\n");
    printf("[SWEEP P1] seeds run: %u (seed %u..%u), ticks/seed: %u, total team-ticks scanned: %llu\n",
           seed_count, first_seed, first_seed + seed_count - 1, ticks_per_seed,
           (unsigned long long)seed_count * ticks_per_seed * NUM_TEAMS);
    printf("[SWEEP P1] task-branch no-path hits this sweep:  %lu (cumulative process total: %lu)\n",
           probe_hits_in_sweep, g_probe_no_path_hits);
    printf("[SWEEP P1] safezone fallback count this sweep:   %u (cumulative process total: %u)\n",
           fallback_in_sweep, g_diag_safezone_fallback_count);
    printf("[SWEEP P1] carve_repair_to_nearest_road fires this sweep: %lu (of which carve failed: %lu)\n",
           g_diag_carve_repair_count - carve_repair_before_sweep,
           g_diag_carve_repair_failed - carve_failed_before_sweep);
    printf("[SWEEP P1] incidents quarantined this sweep (unreachable_streak >= %u): %lu (cumulative process total: %lu)\n",
           INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD,
           g_diag_incidents_quarantined - quarantined_before_sweep,
           g_diag_incidents_quarantined);
    printf("[SWEEP P1] passive-sweep cells straddling a district boundary (M37 misattribution, fix shelved) this sweep: %u (cumulative process total: %u)\n",
           g_diag_sweep_cross_district_cells - cross_district_before_sweep,
           g_diag_sweep_cross_district_cells);
    printf("[SWEEP P1] worlds with non-distinct evacuation gateways this sweep: %lu (expected 0; cumulative process total: %lu)\n",
           g_diag_gateway_degenerate_worlds - gateway_degenerate_before_sweep,
           g_diag_gateway_degenerate_worlds);
    printf("[SWEEP P1] evacuation gateways repaired (not road-connected when chosen) this sweep: %lu (cumulative process total: %lu)\n",
           g_diag_gateway_repaired - gateway_repaired_before_sweep,
           g_diag_gateway_repaired);
    printf("[SWEEP P1] safe zones placed via MAX_ATTEMPTS fallback this sweep: %lu (expected 0; cumulative process total: %lu)\n",
           g_diag_safezone_maxattempts_fallback - safezone_fallback_before_sweep,
           g_diag_safezone_maxattempts_fallback);
    printf("[SWEEP P1] select_best_team score ties this sweep: %lu (cumulative process total: %lu)\n",
           g_diag_team_score_ties - team_score_ties_before_sweep,
           g_diag_team_score_ties);
    printf("[SWEEP P1] longest EN_ROUTE-same-location streak observed anywhere: %u ticks "
           "(flag threshold: %u)\n", max_streak_seen, P1_SWEEP_STUCK_TICKS_THRESHOLD);
    printf("[SWEEP P1] frozen streaks flagged (>= threshold): %u\n", flagged_count);

    for(uint32_t i = 0; i < flagged_count; i++)
    {
        printf("[SWEEP P1]   #%u: seed=%u team=%u first stuck at tick=%u, streak>=%u ticks\n",
               i + 1, flagged[i].seed, flagged[i].team_id, flagged[i].start_tick, flagged[i].streak_len);
    }
    if(flagged_count == P1_SWEEP_MAX_REPORTED_STREAKS)
        printf("[SWEEP P1]   (further streaks not individually listed - cap of %u reached)\n",
               P1_SWEEP_MAX_REPORTED_STREAKS);

    if(flagged_count == 0)
    {
        printf("[SWEEP P1] VERDICT: PASS - no team was ever stuck EN_ROUTE at one location for "
               "%u+ ticks across %u seeds x %u ticks.\n", P1_SWEEP_STUCK_TICKS_THRESHOLD, seed_count, ticks_per_seed);
        return 0;
    }

    printf("[SWEEP P1] VERDICT: FAIL - %u frozen streak(s) detected. The task-branch no-path "
           "fallback is not preventing every freeze.\n", flagged_count);
    return (int)(flagged_count > 255 ? 255 : flagged_count);
}
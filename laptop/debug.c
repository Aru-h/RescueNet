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

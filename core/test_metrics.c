#include "test_metrics.h"
#include "mission_state.h"
#include "cbba.h"

/* ~3 hours real time (was a bare "2000" = 20 hours, longer than most
 * test runs' entire duration - meant no resolution activity for a
 * whole run would never actually trigger stall detection). */
#define STALL_THRESHOLD_TICKS (TICKS_PER_HOUR * 3)

static SimulationMetrics_t g_metrics;
static uint8_t g_prev_incident_status[NUM_INCIDENTS];
static uint8_t g_prev_incident_occupied[NUM_INCIDENTS];
static uint32_t g_zero_available_streak;
static uint32_t g_no_resolution_streak;
static uint32_t g_no_progress_streak;
static uint8_t g_progress_since_last_sample;
static uint32_t g_stage_completions;
static uint32_t g_cbba_path_samples;
static uint32_t g_cbba_path_size_sum;
static uint8_t g_max_cbba_path_size;
static uint32_t g_assignment_to_work_start_samples;
static uint64_t g_assignment_to_work_start_sum;
static uint8_t g_assignment_timer_active[NUM_INCIDENTS];
static uint32_t g_assignment_timer_incident_id[NUM_INCIDENTS];
static uint32_t g_assignment_timer_tick[NUM_INCIDENTS];

static uint8_t g_rest_timer_active[NUM_TEAMS];
static uint32_t g_rest_timer_entry_tick[NUM_TEAMS];

static uint32_t count_open_incidents(void)
{
    uint32_t count = 0;
    for(uint32_t i = 0; i < NUM_INCIDENTS; i++)
        if(g_incidents[i].slot_occupied && g_incidents[i].status == INC_OPEN)
            count++;
    return count;
}

static uint32_t count_active_incidents(void)
{
    uint32_t count = 0;
    for(uint32_t i = 0; i < NUM_INCIDENTS; i++)
        if(g_incidents[i].slot_occupied && g_incidents[i].status != INC_RESOLVED)
            count++;
    return count;
}

static uint32_t count_active_system_tasks(void)
{
    uint32_t count = 0;
    for(uint32_t i = 0; i < NUM_SYSTEM_TASKS; i++)
        if(g_system_tasks[i].slot_occupied && g_system_tasks[i].status != INC_RESOLVED)
            count++;
    return count;
}


void metrics_begin_run(uint32_t seed)
{
    g_metrics = (SimulationMetrics_t){0};
    g_metrics.seed = seed;
    g_zero_available_streak = 0;
    g_no_resolution_streak = 0;
    g_no_progress_streak = 0;
    g_progress_since_last_sample = 0;
    g_stage_completions = 0;
    g_cbba_path_samples = 0;
    g_cbba_path_size_sum = 0;
    g_max_cbba_path_size = 0;
    g_assignment_to_work_start_samples = 0;
    g_assignment_to_work_start_sum = 0;

    for(uint32_t i = 0; i < NUM_INCIDENTS; i++)
    {
        g_assignment_timer_active[i] = 0;
        g_assignment_timer_incident_id[i] = 0;
        g_assignment_timer_tick[i] = 0;
    }

    for(uint32_t i = 0; i < NUM_TEAMS; i++)
    {
        g_rest_timer_active[i] = 0;
        g_rest_timer_entry_tick[i] = 0;
    }

    for(uint32_t i = 0; i < NUM_INCIDENTS; i++)
    {
        g_prev_incident_occupied[i] = g_incidents[i].slot_occupied;
        g_prev_incident_status[i] = g_incidents[i].status;
    }
}

void metrics_sample_tick(void)
{
    uint32_t open_incidents = count_open_incidents();
    uint32_t active_incidents = count_active_incidents();
    uint32_t system_tasks = count_active_system_tasks();
    uint32_t available_teams = 0;
    uint8_t resolved_this_tick = 0;

    for(uint8_t i = 0; i < NUM_TEAMS; i++)
    {
        TeamState_t *team = &g_teams[i];

        switch(team->status)
        {
            case STATUS_AVAILABLE:
                available_teams++;
                g_metrics.available_team_ticks++;
                break;

            case STATUS_EN_ROUTE:
                g_metrics.en_route_team_ticks++;
                break;

            case STATUS_ON_SCENE:
                g_metrics.on_scene_team_ticks++;
                break;

            case STATUS_RESTING:
                g_metrics.resting_team_ticks++;
                break;

            default:
                break;
        }

        if(team->degraded)
            g_metrics.degraded_team_ticks++;
    }

    g_metrics.ticks_run++;

    if(open_incidents > g_metrics.peak_open_incidents)
        g_metrics.peak_open_incidents = open_incidents;
    if(active_incidents > g_metrics.peak_active_incidents)
        g_metrics.peak_active_incidents = active_incidents;
    if(system_tasks > g_metrics.peak_system_tasks)
        g_metrics.peak_system_tasks = system_tasks;

    if(available_teams == 0)
    {
        g_zero_available_streak++;
        if(g_zero_available_streak > g_metrics.longest_zero_available_streak)
            g_metrics.longest_zero_available_streak = g_zero_available_streak;
    }
    else
    {
        g_zero_available_streak = 0;
    }

    for(uint32_t i = 0; i < NUM_INCIDENTS; i++)
    {
        if(g_prev_incident_occupied[i] &&
           g_prev_incident_status[i] != INC_RESOLVED &&
           (!g_incidents[i].slot_occupied || g_incidents[i].status == INC_RESOLVED))
        {
            g_metrics.resolved_transitions++;
            resolved_this_tick = 1;
        }

        g_prev_incident_occupied[i] = g_incidents[i].slot_occupied;
        g_prev_incident_status[i] = g_incidents[i].status;
    }

    if(resolved_this_tick)
        g_no_resolution_streak = 0;
    else if(active_incidents > 0)
        g_no_resolution_streak++;
    else
        g_no_resolution_streak = 0;

    if(g_no_resolution_streak > g_metrics.longest_no_resolution_streak)
        g_metrics.longest_no_resolution_streak = g_no_resolution_streak;

    if(resolved_this_tick || g_progress_since_last_sample)
        g_no_progress_streak = 0;
    else if(active_incidents > 0)
        g_no_progress_streak++;
    else
        g_no_progress_streak = 0;

    if(g_no_progress_streak > g_metrics.longest_no_progress_streak)
        g_metrics.longest_no_progress_streak = g_no_progress_streak;

    g_progress_since_last_sample = 0;

    if(g_no_resolution_streak >= STALL_THRESHOLD_TICKS && active_incidents > 0)
        g_metrics.potential_stall = 1;
}

void test_metrics_record_equipment_exhaustion_recovery(uint8_t stamina)
{
    g_metrics.equipment_exhaustion_recoveries++;
    g_metrics.stamina_sum_at_rest_entry += stamina;
    g_metrics.rest_entry_samples++;
}

void test_metrics_record_support_reserve_recovery(uint8_t stamina)
{
    g_metrics.support_reserve_recoveries++;
    g_metrics.stamina_sum_at_rest_entry += stamina;
    g_metrics.rest_entry_samples++;
}

void test_metrics_record_rest_entry(uint8_t team_id, uint32_t entry_tick)
{
    if(team_id >= NUM_TEAMS)
        return;

    g_rest_timer_active[team_id] = 1;
    g_rest_timer_entry_tick[team_id] = entry_tick;
}

void test_metrics_record_rest_exit(uint8_t team_id, uint32_t exit_tick)
{
    if(team_id >= NUM_TEAMS)
        return;

    if(!g_rest_timer_active[team_id])
        return;

    if(exit_tick >= g_rest_timer_entry_tick[team_id])
    {
        uint32_t duration =
            exit_tick - g_rest_timer_entry_tick[team_id];

        g_metrics.rest_cycle_samples++;
        g_metrics.rest_cycle_duration_sum += duration;

        if(duration > g_metrics.max_rest_cycle_duration)
            g_metrics.max_rest_cycle_duration = duration;
    }

    g_rest_timer_active[team_id] = 0;
}

void test_metrics_record_stage_completion(void)
{
    g_stage_completions++;
    test_metrics_record_progress();
}

void test_metrics_record_progress(void)
{
    g_progress_since_last_sample = 1;
}

uint32_t test_metrics_current_tick(void)
{
    return g_metrics.ticks_run;
}

void test_metrics_record_cbba_path_size(uint8_t path_size)
{
    if(path_size == 0)
        return;

    g_cbba_path_samples++;
    g_cbba_path_size_sum += path_size;

    if(path_size > g_max_cbba_path_size)
        g_max_cbba_path_size = path_size;
}

void test_metrics_record_incident_assignment(
    uint8_t slot,
    uint32_t incident_id,
    uint32_t assigned_tick)
{
    if(slot >= NUM_INCIDENTS)
        return;

    g_assignment_timer_active[slot] = 1;
    g_assignment_timer_incident_id[slot] = incident_id;
    g_assignment_timer_tick[slot] = assigned_tick;
}

void test_metrics_record_incident_work_start(
    uint8_t slot,
    uint32_t incident_id,
    uint32_t work_start_tick)
{
    if(slot >= NUM_INCIDENTS)
        return;

    if(!g_assignment_timer_active[slot])
        return;

    if(g_assignment_timer_incident_id[slot] != incident_id)
        return;

    if(work_start_tick >= g_assignment_timer_tick[slot])
    {
        test_metrics_record_assignment_to_work_start(
            work_start_tick - g_assignment_timer_tick[slot]);
    }

    g_assignment_timer_active[slot] = 0;
}

void test_metrics_record_assignment_to_work_start(uint32_t delay_ticks)
{
    g_assignment_to_work_start_samples++;
    g_assignment_to_work_start_sum += delay_ticks;
}

SimulationMetrics_t metrics_end_run(void)
{
    g_metrics.final_open_incidents = count_open_incidents();
    g_metrics.final_active_incidents = count_active_incidents();
    g_metrics.final_system_tasks = count_active_system_tasks();
    g_metrics.stage_completions = g_stage_completions;
    g_metrics.cbba_path_samples = g_cbba_path_samples;
    g_metrics.cbba_path_size_sum = g_cbba_path_size_sum;
    g_metrics.max_cbba_path_size = g_max_cbba_path_size;
    g_metrics.cbba_two_opt_improvements = cbba_get_two_opt_improvements();
    g_metrics.cbba_relocate_improvements = cbba_get_relocate_improvements();
    g_metrics.assignment_to_work_start_samples =
        g_assignment_to_work_start_samples;
    g_metrics.assignment_to_work_start_sum =
        g_assignment_to_work_start_sum;
    return g_metrics;
}

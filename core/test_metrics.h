#ifndef TEST_METRICS_H
#define TEST_METRICS_H

#include <stdint.h>

typedef struct
{
    uint32_t seed;
    uint32_t ticks_run;
    uint32_t peak_open_incidents;
    uint32_t peak_active_incidents;
    uint32_t peak_system_tasks;
    uint32_t longest_zero_available_streak;
    uint32_t longest_no_resolution_streak;
    uint32_t longest_no_progress_streak;
    uint32_t resolved_transitions;
    uint32_t final_open_incidents;
    uint32_t final_active_incidents;
    uint32_t final_system_tasks;
    uint8_t potential_stall;
    uint32_t stage_completions;
    uint32_t cbba_path_samples;
    uint32_t cbba_path_size_sum;
    uint8_t max_cbba_path_size;
    uint32_t cbba_two_opt_improvements;
    uint32_t cbba_relocate_improvements;
    uint32_t assignment_to_work_start_samples;
    uint64_t assignment_to_work_start_sum;
    uint64_t available_team_ticks;
    uint64_t en_route_team_ticks;
    uint64_t on_scene_team_ticks;
    uint64_t resting_team_ticks;
    uint64_t degraded_team_ticks;
    uint32_t equipment_exhaustion_recoveries;
    uint32_t support_reserve_recoveries;
    uint64_t stamina_sum_at_rest_entry;
    uint32_t rest_entry_samples;
    uint32_t rest_cycle_samples;
    uint64_t rest_cycle_duration_sum;
    uint32_t max_rest_cycle_duration;
} SimulationMetrics_t;

void metrics_begin_run(uint32_t seed);
void metrics_sample_tick(void);
SimulationMetrics_t metrics_end_run(void);

void test_metrics_record_equipment_exhaustion_recovery(uint8_t stamina);
void test_metrics_record_support_reserve_recovery(uint8_t stamina);
void test_metrics_record_rest_entry(uint8_t team_id, uint32_t entry_tick);
void test_metrics_record_rest_exit(uint8_t team_id, uint32_t exit_tick);
void test_metrics_record_stage_completion(void);
void test_metrics_record_progress(void);
void test_metrics_record_cbba_path_size(uint8_t path_size);
void test_metrics_record_incident_assignment(uint8_t slot, uint32_t incident_id, uint32_t assigned_tick);
void test_metrics_record_incident_work_start(uint8_t slot, uint32_t incident_id, uint32_t work_start_tick);
void test_metrics_record_assignment_to_work_start(uint32_t delay_ticks);
uint32_t test_metrics_current_tick(void);
#endif

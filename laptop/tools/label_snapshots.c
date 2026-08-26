#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>


#include "mission_state.h"

typedef struct
{
    uint32_t run_id;
    uint32_t world_seed;
    uint32_t cycle;
    uint32_t team_id;
    int16_t loc_x;
    int16_t loc_y;
    float stamina;
    uint8_t specialization;
    uint8_t status;
    int32_t assigned_incident;
    int32_t incident_priority;
    int32_t incident_req_capability;
    int32_t incident_loc_x;
    int32_t incident_loc_y;
    int32_t victim_count;
    uint32_t hazard_flags;
    uint32_t hours_worked;
    uint8_t fitness_tier;
    uint32_t equipment_flags;
    uint32_t event_type;
    int32_t cbba_selected_team;
    uint16_t recovery_threshold_stamina;
    int32_t recovery_ticks_to_available;
    uint8_t recovery_within_horizon;
    float twin_expected_progress;
    float twin_actual_progress;
    float twin_progress_error;
    float twin_expected_stamina;
    float twin_actual_stamina;
    float twin_stamina_error;
    uint16_t twin_remaining_path_cost;
    uint8_t twin_mission_stage;
    uint8_t twin_communication_health;
    uint32_t twin_expected_arrival_tick;
    uint8_t twin_expected_operating_state;
    uint8_t twin_assigned_task_pool;
    uint8_t twin_assigned_task_slot;
} SnapshotRow_t;

static SnapshotRow_t *g_rows = NULL;
static size_t g_row_count = 0;

static void free_rows(void)
{
    free(g_rows);
    g_rows = NULL;
    g_row_count = 0;
}

static uint16_t recovery_threshold_for_tier(uint8_t fitness_tier)
{
    switch(fitness_tier)
    {
        case ARDUOUS:
            return (uint16_t)ARDUOUS_READY_STAMINA;

        case MODERATE:
            return (uint16_t)MODERATE_READY_STAMINA;

        case LIGHT:
            return (uint16_t)LIGHT_READY_STAMINA;

        default:
            return (uint16_t)LIGHT_READY_STAMINA;
    }
}

static int parse_row(char *line, SnapshotRow_t *row)
{
    char *token = NULL;
    char *saveptr = NULL;
    int field_index = 0;

    token = strtok_r(line, ",", &saveptr);
    while(token != NULL)
    {
        switch(field_index)
        {
            case 0: row->run_id = (uint32_t)strtoul(token, NULL, 10); break;
            case 1: row->world_seed = (uint32_t)strtoul(token, NULL, 10); break;
            case 2: row->cycle = (uint32_t)strtoul(token, NULL, 10); break;
            case 3: row->team_id = (uint32_t)strtoul(token, NULL, 10); break;
            case 4: row->loc_x = (int16_t)strtol(token, NULL, 10); break;
            case 5: row->loc_y = (int16_t)strtol(token, NULL, 10); break;
            case 6: row->stamina = strtof(token, NULL); break;
            case 7: row->specialization = (uint8_t)strtoul(token, NULL, 10); break;
            case 8: row->status = (uint8_t)strtoul(token, NULL, 10); break;
            case 9: row->assigned_incident = (int32_t)strtol(token, NULL, 10); break;
            case 10: row->incident_priority = (int32_t)strtol(token, NULL, 10); break;
            case 11: row->incident_req_capability = (int32_t)strtol(token, NULL, 10); break;
            case 12: row->incident_loc_x = (int32_t)strtol(token, NULL, 10); break;
            case 13: row->incident_loc_y = (int32_t)strtol(token, NULL, 10); break;
            case 14: row->victim_count = (int32_t)strtol(token, NULL, 10); break;
            case 15: row->hazard_flags = (uint32_t)strtoul(token, NULL, 10); break;
            case 16: row->hours_worked = (uint32_t)strtoul(token, NULL, 10); break;
            case 17: row->fitness_tier = (uint8_t)strtoul(token, NULL, 10); break;
            case 18: row->equipment_flags = (uint32_t)strtoul(token, NULL, 10); break;
            case 19: row->event_type = (uint32_t)strtoul(token, NULL, 10); break;
            case 20: row->cbba_selected_team = (int32_t)strtol(token, NULL, 10); break;
            case 21: row->twin_expected_progress = strtof(token, NULL); break;
            case 22: row->twin_actual_progress = strtof(token, NULL); break;
            case 23: row->twin_progress_error = strtof(token, NULL); break;
            case 24: row->twin_expected_stamina = strtof(token, NULL); break;
            case 25: row->twin_actual_stamina = strtof(token, NULL); break;
            case 26: row->twin_stamina_error = strtof(token, NULL); break;
            case 27: row->twin_remaining_path_cost = (uint16_t)strtoul(token, NULL, 10); break;
            case 29: row->twin_mission_stage = (uint8_t)strtoul(token, NULL, 10); break;
            case 30: row->twin_communication_health = (uint8_t)strtoul(token, NULL, 10); break;
            case 42: row->twin_expected_arrival_tick = (uint32_t)strtoul(token, NULL, 10); break;
            case 43: row->twin_expected_operating_state = (uint8_t)strtoul(token, NULL, 10); break;
            case 44: row->twin_assigned_task_pool = (uint8_t)strtoul(token, NULL, 10); break;
            case 45: row->twin_assigned_task_slot = (uint8_t)strtoul(token, NULL, 10); break;
            default:
                break;
        }

        field_index++;
        token = strtok_r(NULL, ",", &saveptr);
    }

    return field_index >= 46;
}

static int compare_row_indices(const void *left, const void *right)
{
    size_t left_index = *(const size_t *)left;
    size_t right_index = *(const size_t *)right;

    const SnapshotRow_t *left_row = &g_rows[left_index];
    const SnapshotRow_t *right_row = &g_rows[right_index];

    if(left_row->run_id < right_row->run_id) return -1;
    if(left_row->run_id > right_row->run_id) return 1;

    if(left_row->team_id < right_row->team_id) return -1;
    if(left_row->team_id > right_row->team_id) return 1;

    if(left_row->cycle < right_row->cycle) return -1;
    if(left_row->cycle > right_row->cycle) return 1;

    if(left_index < right_index) return -1;
    if(left_index > right_index) return 1;

    return 0;
}

static size_t lower_bound_cycle(
    const size_t *indices,
    size_t begin,
    size_t end,
    uint32_t min_cycle)
{
    size_t left = begin;
    size_t right = end;

    while(left < right)
    {
        size_t mid = left + (right - left) / 2;
        uint32_t cycle = g_rows[indices[mid]].cycle;

        if(cycle < min_cycle)
            left = mid + 1;
        else
            right = mid;
    }

    return left;
}

static void label_group(
    const size_t *indices,
    size_t begin,
    size_t end,
    uint32_t horizon_ticks,
    uint8_t *labels,
    int32_t *offsets)
{
    size_t recovered_capacity = end - begin;
    size_t *recovered_indices = malloc(recovered_capacity * sizeof(size_t));

    if(recovered_indices == NULL)
        return;

    size_t recovered_count = 0;

    for(size_t cursor = begin; cursor < end; cursor++)
    {
        size_t row_index = indices[cursor];

        if(g_rows[row_index].status == STATUS_AVAILABLE)
            recovered_indices[recovered_count++] = row_index;
    }

    for(size_t cursor = begin; cursor < end; cursor++)
    {
        size_t row_index = indices[cursor];
        SnapshotRow_t *row = &g_rows[row_index];

        labels[row_index] = 0;
        offsets[row_index] = -1;

        if(row->status == STATUS_AVAILABLE)
            continue;

        uint32_t recovery_threshold =
            recovery_threshold_for_tier(row->fitness_tier);
        uint32_t min_cycle = row->cycle + 1u;
        uint32_t max_cycle = row->cycle + horizon_ticks;

        size_t lower = lower_bound_cycle(
            recovered_indices,
            0,
            recovered_count,
            min_cycle);

        if(lower >= recovered_count)
            continue;

        for(size_t recovered_cursor = lower;
            recovered_cursor < recovered_count;
            recovered_cursor++)
        {
            size_t recovered_row_index = recovered_indices[recovered_cursor];
            SnapshotRow_t *recovered_row = &g_rows[recovered_row_index];

            if(recovered_row->cycle > max_cycle)
                break;

            if(recovered_row->stamina < (float)recovery_threshold)
                continue;

            labels[row_index] = 1;
            offsets[row_index] = (int32_t)(recovered_row->cycle - row->cycle);
            row->recovery_threshold_stamina = (uint16_t)recovery_threshold;
            row->recovery_ticks_to_available = offsets[row_index];
            row->recovery_within_horizon = 1;
            break;
        }
    }

    free(recovered_indices);
}

static void label_rows(uint32_t horizon_ticks, uint8_t *labels, int32_t *offsets)
{
    size_t *order = malloc(g_row_count * sizeof(size_t));

    if(order == NULL)
        return;

    for(size_t i = 0; i < g_row_count; i++)
        order[i] = i;

    qsort(order, g_row_count, sizeof(size_t), compare_row_indices);

    size_t group_begin = 0;
    while(group_begin < g_row_count)
    {
        size_t group_end = group_begin + 1;
        SnapshotRow_t *group_row = &g_rows[order[group_begin]];

        while(group_end < g_row_count)
        {
            SnapshotRow_t *candidate = &g_rows[order[group_end]];

            if(candidate->run_id != group_row->run_id ||
               candidate->team_id != group_row->team_id)
            {
                break;
            }

            group_end++;
        }

        label_group(order, group_begin, group_end, horizon_ticks, labels, offsets);
        group_begin = group_end;
    }

    free(order);
}

static int write_labeled_dataset(
    const char *output_path,
    uint32_t horizon_ticks,
    const uint8_t *labels,
    const int32_t *offsets)
{
    FILE *output = fopen(output_path, "w");

    if(output == NULL)
        return 0;

    fprintf(output,
        "run_id,world_seed,cycle,team_id,loc_x,loc_y,stamina,specialization,status,"
        "assigned_incident,incident_priority,incident_req_capability,"
        "incident_loc_x,incident_loc_y,victim_count,hazard_flags,"
        "hours_worked,fitness_tier,equipment_flags,event_type,cbba_selected_team,"
        "recovery_threshold_stamina,recovery_ticks_to_available,recovery_within_%u_ticks,"
        "twin_expected_progress,twin_actual_progress,twin_progress_error,"
        "twin_expected_stamina,twin_actual_stamina,twin_stamina_error,"
        "twin_remaining_path_cost,twin_mission_stage,twin_communication_health,"
        "twin_expected_arrival_tick,twin_expected_operating_state,"
        "twin_assigned_task_pool,twin_assigned_task_slot\n",
        horizon_ticks);

    for(size_t i = 0; i < g_row_count; i++)
    {
        const SnapshotRow_t *row = &g_rows[i];

        fprintf(output,
            "%u,%u,%u,%u,%d,%d,%.2f,%u,%u,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%d,%u,%d,%u,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%u,%u,%u\n",
            row->run_id,
            row->world_seed,
            row->cycle,
            row->team_id,
            row->loc_x,
            row->loc_y,
            row->stamina,
            row->specialization,
            row->status,
            row->assigned_incident,
            row->incident_priority,
            row->incident_req_capability,
            row->incident_loc_x,
            row->incident_loc_y,
            row->victim_count,
            row->hazard_flags,
            row->hours_worked,
            row->fitness_tier,
            row->equipment_flags,
            row->event_type,
            row->cbba_selected_team,
            row->recovery_threshold_stamina,
            offsets[i],
            labels[i],
            row->twin_expected_progress,
            row->twin_actual_progress,
            row->twin_progress_error,
            row->twin_expected_stamina,
            row->twin_actual_stamina,
            row->twin_stamina_error,
            row->twin_remaining_path_cost,
            row->twin_mission_stage,
            row->twin_communication_health,
            row->twin_expected_arrival_tick,
            row->twin_expected_operating_state,
            row->twin_assigned_task_pool,
            row->twin_assigned_task_slot);
    }

    fclose(output);
    return 1;
}

static int load_snapshot_rows(const char *input_path)
{
    FILE *input = fopen(input_path, "r");
    char line[1024];
    size_t capacity = 0;

    if(input == NULL)
        return 0;

    if(fgets(line, sizeof(line), input) == NULL)
    {
        fclose(input);
        return 0;
    }

    while(fgets(line, sizeof(line), input) != NULL)
    {
        SnapshotRow_t row;
        char copy[1024];

        if(line[0] == '\n' || line[0] == '\r' || line[0] == '\0')
            continue;

        strncpy(copy, line, sizeof(copy));
        copy[sizeof(copy) - 1] = '\0';

        if(!parse_row(copy, &row))
            continue;

        if(g_row_count == capacity)
        {
            size_t new_capacity = capacity == 0 ? 4096 : capacity * 2;
            SnapshotRow_t *new_rows = realloc(g_rows, new_capacity * sizeof(SnapshotRow_t));

            if(new_rows == NULL)
            {
                fclose(input);
                return 0;
            }

            g_rows = new_rows;
            capacity = new_capacity;
        }

        g_rows[g_row_count++] = row;
    }

    fclose(input);
    return g_row_count > 0;
}

static void usage(const char *program_name)
{
    fprintf(stderr,
        "usage: %s [input_csv] [output_csv] [horizon_ticks]\n"
        "defaults: tron_snapshot_dataset.csv tron_snapshot_dataset_labeled.csv 5\n",
        program_name);
}

int main(int argc, char **argv)
{
    const char *input_path = argc >= 2 ? argv[1] : "tron_snapshot_dataset.csv";
    const char *output_path = argc >= 3 ? argv[2] : "tron_snapshot_dataset_labeled.csv";
    uint32_t horizon_ticks = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 5u;
    uint8_t *labels = NULL;
    int32_t *offsets = NULL;
    int exit_code = 1;

    if(argc >= 2 && strcmp(argv[1], "-h") == 0)
    {
        usage(argv[0]);
        return 0;
    }

    if(!load_snapshot_rows(input_path))
    {
        fprintf(stderr, "failed to load snapshot dataset from %s\n", input_path);
        goto done;
    }

    labels = calloc(g_row_count, sizeof(uint8_t));
    offsets = malloc(g_row_count * sizeof(int32_t));

    if(labels == NULL || offsets == NULL)
    {
        fprintf(stderr, "failed to allocate label buffers\n");
        goto done;
    }

    for(size_t i = 0; i < g_row_count; i++)
    {
        offsets[i] = -1;
        g_rows[i].recovery_threshold_stamina = recovery_threshold_for_tier(g_rows[i].fitness_tier);
        g_rows[i].recovery_ticks_to_available = -1;
        g_rows[i].recovery_within_horizon = 0;
    }

    label_rows(horizon_ticks, labels, offsets);

    if(!write_labeled_dataset(output_path, horizon_ticks, labels, offsets))
    {
        fprintf(stderr, "failed to write labeled dataset to %s\n", output_path);
        goto done;
    }

    printf("wrote labeled dataset to %s\n", output_path);
    exit_code = 0;

done:
    free(labels);
    free(offsets);
    free_rows();
    return exit_code;
}

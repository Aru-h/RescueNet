#include "snapshot.h"

#include <stdio.h>
#include <time.h>

#include "team.h"
#include "incident.h"
#include "mission_state.h"
#include "recovery_manager.h"
#include "watchdog.h"
#include "search_zones.h"

static FILE *g_snapshot_file = NULL;
static uint32_t g_dataset_seed = 0;
static uint32_t g_run_id = 0;
static uint32_t g_process_epoch = 0;
static uint8_t g_process_epoch_set = 0;

void snapshot_begin_run(uint32_t seed)
{
    g_dataset_seed = seed;

    if(!g_process_epoch_set)
    {
        g_process_epoch = (uint32_t)time(NULL);
        g_process_epoch_set = 1;
    }

    g_run_id = g_process_epoch + seed;

    if(g_snapshot_file != NULL)
        return;

    g_snapshot_file = fopen("tron_snapshot_dataset.csv", "w");
    if(g_snapshot_file == NULL)
        return;

    fprintf(g_snapshot_file,
        "run_id,world_seed,cycle,team_id,loc_x,loc_y,stamina,specialization,status,"
        "assigned_incident,incident_priority,incident_req_capability,"
        "incident_loc_x,incident_loc_y,victim_count,hazard_flags,"
        "hours_worked,fitness_tier,equipment_flags,event_type,cbba_selected_team,"
        "equipment_level,support_team,fault_active_mask,"
        "twin_expected_progress,twin_actual_progress,twin_progress_error,"
        "twin_expected_stamina,twin_actual_stamina,twin_stamina_error,"
        "twin_remaining_path_cost,twin_expected_work_remaining,twin_mission_stage,"
        "twin_communication_health,twin_expected_loc_x,twin_expected_loc_y,"
        "watchdog_anomaly_detected,watchdog_anomaly_confidence,"
        "watchdog_failure_mode,watchdog_evidence_flags,watchdog_recommended_action,"
        "recovery_recommendation,recovery_reason,recovery_confidence,recovery_escalation_level,"
        "twin_expected_arrival_tick,twin_expected_operating_state,"
        "twin_assigned_task_pool,twin_assigned_task_slot,"
        "incident_discovered,team_zone_id,team_zone_trust\n");
}

/*
 * Shared row writer. Incident-context fields (incident_id, priority,
 * capability, location, victims, hazards, selected_team, support_team,
 * discovered) are passed in explicitly by the caller rather than
 * derived here from live team state - see snapshot_record() and
 * snapshot_record_resolved() below for why that split exists.
 */
static void write_snapshot_row(uint32_t cycle, EventType_t event_type,
    uint8_t team_id, int incident_id, int priority, int capability,
    int ix, int iy, int victims, unsigned hazards, int selected_team,
    int support_team, int discovered)
{
    TeamState_t *team;
    MissionExecutionTwin_t *twin;
    WatchdogStatus_t watchdog;
    RecoveryStatus_t recovery;
    uint8_t zone_id;
    uint8_t zone_trust;

    if(g_snapshot_file == NULL || team_id >= NUM_TEAMS)
        return;

    team = &g_teams[team_id];
    twin = &g_mission_execution_twin[team_id];
    watchdog = watchdog_get_status(team_id);
    recovery = recovery_manager_get_status(team_id);

    /* Zone context is meaningful for every team regardless of what it's
     * assigned to (or not) - unlike incident_discovered above, which
     * only means anything when the team actually has an incident-pool
     * assignment. */
    zone_id = zone_id_for_location(team->loc_x, team->loc_y);
    zone_trust = search_zone_trust(zone_id, cycle);

    fprintf(g_snapshot_file,
        "%u,%u,%u,%u,%d,%d,%.2f,%u,%u,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%d,%u,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%d,%d,%u,%.4f,%u,%u,%u,%u,%u,%.4f,%u,%u,%u,%u,%u,%d,%u,%u\n",
        g_run_id,
        g_dataset_seed,
        cycle,
        team->team_id,
        team->loc_x,
        team->loc_y,
        team->stamina,
        team->specialization,
        team->status,
        incident_id,
        priority,
        capability,
        ix,
        iy,
        victims,
        hazards,
        team->hours_worked,
        team->fitness_tier,
        team->equipment_flags,
        event_type,
        selected_team,
        team->equipment_level,
        support_team,
        team->fault_state.active_mask,
        twin->expected_progress,
        twin->actual_progress,
        twin->progress_error,
        twin->expected_stamina,
        twin->actual_stamina,
        twin->stamina_error,
        twin->remaining_path_cost,
        twin->expected_work_remaining,
        twin->mission_stage,
        twin->communication_health,
        twin->expected_loc_x,
        twin->expected_loc_y,
        watchdog.anomaly_detected,
        watchdog.anomaly_confidence,
        watchdog.dominant_failure_mode,
        watchdog.evidence_flags,
        watchdog.recommended_action,
        recovery.recommendation,
        recovery.reason,
        recovery.confidence,
        recovery.escalation_level,
        twin->expected_arrival_tick,
        twin->expected_operating_state,
        twin->assigned_task_pool,
        twin->assigned_task_slot,
        discovered,
        zone_id,
        zone_trust);
}

void snapshot_record(uint32_t cycle, EventType_t event_type, uint8_t team_id)
{
    TeamState_t *team;
    int incident_id = -1;
    int priority = -1;
    int capability = -1;
    int ix = -1;
    int iy = -1;
    int victims = -1;
    unsigned hazards = 0;
    int selected_team = NO_ASSIGNMENT;
    int support_team = -1;
    int discovered = -1;

    if(team_id >= NUM_TEAMS)
        return;

    team = &g_teams[team_id];

    /*
     * Derives incident context from the team's *current live* state.
     * Correct for every call site except one: complete_task()'s
     * EVENT_INCIDENT_RESOLVED log. By the time that fires, the
     * completing team can already have been dispatched to a new task
     * earlier in the same tick (allocate_all_incidents() runs before
     * simulation_step()'s completion detection) - so a live read at
     * that specific point captures the team's *next* assignment, not
     * the one that just finished. See snapshot_record_resolved() for
     * that case; every other event type (assignment, stage-complete,
     * rest, fault, etc.) logs at a point where the team's live state
     * genuinely does reflect the event being logged, so this is fine
     * for those.
     */
    if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
       team->assigned_task_slot != NO_ASSIGNMENT)
    {
        IncidentState_t *inc = &g_incidents[team->assigned_task_slot];
        incident_id = inc->incident_id;
        priority = inc->priority;
        capability = inc->req_capability;
        ix = inc->loc_x;
        iy = inc->loc_y;
        victims = inc->victim_count;
        hazards = inc->hazard_flags;
        selected_team = inc->assigned_team;
        support_team = (inc->support_team == NO_ASSIGNMENT) ? -1 : (int)inc->support_team;
        discovered = inc->discovered;
    }

    write_snapshot_row(cycle, event_type, team_id, incident_id, priority,
        capability, ix, iy, victims, hazards, selected_team, support_team,
        discovered);
}

/*
 * Same row format as snapshot_record(), but for logging a task's own
 * resolution (complete_task() in incident.c): incident context comes
 * directly from the task that just resolved, not from a live lookup
 * on the team. task->assigned_team is still the team_id the row gets
 * logged under - the fix is only in *where the incident-detail fields
 * come from*, not which team the row belongs to.
 */
void snapshot_record_resolved(uint32_t cycle, TaskPool_t pool, const IncidentState_t *task)
{
    int incident_id = -1;
    int priority = -1;
    int capability = -1;
    int ix = -1;
    int iy = -1;
    int victims = -1;
    unsigned hazards = 0;
    int selected_team = NO_ASSIGNMENT;
    int support_team = -1;
    int discovered = -1;

    if(task == NULL || task->assigned_team >= NUM_TEAMS)
        return;

    if(pool == TASK_POOL_INCIDENT)
    {
        incident_id = task->incident_id;
        priority = task->priority;
        capability = task->req_capability;
        ix = task->loc_x;
        iy = task->loc_y;
        victims = task->victim_count;
        hazards = task->hazard_flags;
        selected_team = task->assigned_team;
        support_team = (task->support_team == NO_ASSIGNMENT) ? -1 : (int)task->support_team;
        discovered = task->discovered;
    }

    write_snapshot_row(cycle, EVENT_INCIDENT_RESOLVED, task->assigned_team,
        incident_id, priority, capability, ix, iy, victims, hazards,
        selected_team, support_team, discovered);
}

void snapshot_end_run(void)
{
}

void snapshot_close_dataset(void)
{
    if(g_snapshot_file != NULL)
    {
        fclose(g_snapshot_file);
        g_snapshot_file = NULL;
    }
}

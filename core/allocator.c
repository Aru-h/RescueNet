#include "allocator.h"
#include <stdio.h>
#include <stdint.h>
#include "simulation.h"
#include "incident.h"
#include "snapshot.h"
#include "cbba.h"
#include "recovery_manager_config.h"
#include "recovery_manager.h"
#include "search_zones.h"
#include "team.h"
#include <string.h>


static uint8_t count_available_teams_for_capability(uint8_t capability);
static uint8_t count_open_incidents_for_capability(uint8_t capability);
static void allocate_support_team(void);

uint8_t select_best_team(const IncidentState_t *incident)
{
    float best_score=-1.0f;
    uint8_t best_team=NO_ASSIGNMENT;
    for(int i=0;i<NUM_TEAMS;i++)
    {
        if (!team_can_accept_incident(&g_teams[i], incident))
        {
            continue;
        }
        float score=calculate_score(&g_teams[i],incident);
        if(g_simulation_trace_enabled) printf("Team %d -> Score = %.3f\n", i, score);
        if(best_score<score){
            best_score=score;
            best_team=i;
        }
    }
    return best_team;


}

void assign_incident(uint8_t team_id, uint8_t incident_slot)
{
    TeamState_t *team = &g_teams[team_id];
    IncidentState_t *incident = &g_incidents[incident_slot];

    if(team->status == STATUS_RESTING)
        team->partial_rest_cycles++;

    team->status = STATUS_EN_ROUTE;
    team->assigned_task_pool = TASK_POOL_INCIDENT;
    team->assigned_task_slot = incident_slot;
    incident->status = INC_ASSIGNED;
    incident->assigned_team = team_id;
    /* World state is now fully updated */
    cbba_sync_mission_execution_twin();
    snapshot_record(g_current_tick, EVENT_TEAM_ASSIGNED, team_id);
}

void allocate_all_incidents(void)
{
    cbba_run();

    /* Generates open UNSEARCHED_SECTOR system tasks for stale districts
     * before the competitive system-task loop below runs, so a
     * freshly-generated task can be picked up in the same tick rather
     * than sitting open for one extra cycle. */
    search_generate_sweep_tasks(g_current_tick);

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        TeamState_t *team = &g_teams[team_id];
        CBBAAgentState_t *agent = &g_cbba_agent_state[team_id];

        if(team->status != STATUS_AVAILABLE &&
        team->status != STATUS_RESTING)
        {
            continue;
        }

        team->incident_queue_size = 0;
        team->incident_queue_index = 0;

        for(uint8_t j = 0; j < agent->path_size; j++)
        {
            TaskRef_t task = agent->path[j];

            if(task.pool != TASK_POOL_INCIDENT)
                continue;

            if(task.slot >= NUM_INCIDENTS)
                continue;

            if(agent->winner[task.slot] != team_id)
                continue;

            IncidentState_t *incident =
                &g_incidents[task.slot];

            if(!incident->slot_occupied ||incident->status != INC_OPEN)
            {
                continue;
            }
            team->incident_queue_slot[0] =task.slot;
            team->incident_queue_id[0] =incident->incident_id;
            team->incident_queue_size = 1;
            team->incident_queue_index = 0;
            assign_incident(team_id,task.slot);

            break;
        }
    }

    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        IncidentState_t *task = &g_system_tasks[i];

        if(!task->slot_occupied || task->status != INC_OPEN)
            continue;

        uint8_t team_id = select_best_team(task);

        if(team_id == NO_ASSIGNMENT)
            continue;

        TeamState_t *team = &g_teams[team_id];

        if(team->status == STATUS_RESTING)
            team->partial_rest_cycles++;

        team->status = STATUS_EN_ROUTE;
        team->assigned_task_pool = TASK_POOL_SYSTEM;
        team->assigned_task_slot = i;
        task->status = INC_ASSIGNED;
        task->assigned_team = team_id;
        cbba_sync_mission_execution_twin();
        snapshot_record(g_current_tick, EVENT_TEAM_ASSIGNED, team_id);
    }
    allocate_support_team();
    cbba_sync_mission_execution_twin();

    /* Sweep dispatch: no longer a bypass here. search_generate_sweep_tasks()
     * at the top of this function already created any open sweep tasks
     * needed; the competitive system-task loop above assigns them like
     * any other system task. */
}
bool team_can_accept_incident(const TeamState_t *team, const IncidentState_t *incident)
{
    if(team == NULL || incident == NULL)
        return false;

    /* Fog-of-war: an incident nobody's found yet is invisible to CBBA
     * entirely, not just deprioritized. System tasks (RETURN_TO_SAFE_ZONE,
     * EQUIPMENT_CACHE, UNSEARCHED_SECTOR itself) are unaffected - they
     * default discovered=1 since they're operational tasks, not part
     * of the reveal system. */
    if(!incident->discovered)
        return false;

    if(team->status == STATUS_AVAILABLE)
    {
        /* Equipment must clear the same critical threshold the recovery
         * manager uses to abort a mission (RECOVERY_MANAGER_EQUIPMENT_
         * CRITICAL_THRESHOLD). Without this, a team whose equipment hit
         * that threshold would get reassigned here, immediately trip the
         * recovery manager's ABORT_MISSION on the very next tick (correctly
         * - equipment that low genuinely can't do the work), land back on
         * AVAILABLE, and get reassigned again next tick - an infinite
         * assign/abort loop that never lets the team reach a real incident
         * or get routed to rest to actually replenish its equipment.
         *
         * Beyond equipment specifically, the same loop can happen for ANY
         * reason the recovery manager would immediately abort a fresh
         * assignment (e.g. an active severe communication fault) - so
         * check its current recommendation directly rather than
         * duplicating each evaluator's own threshold logic here. */
        if(recovery_manager_get_recommendation(team->team_id) == RECOVERY_ACTION_ABORT_MISSION)
            return false;

        return team->stamina >= get_soft_stamina_floor(team) &&
               team->equipment_level > RECOVERY_MANAGER_EQUIPMENT_CRITICAL_THRESHOLD;
    }

    if(team->status != STATUS_RESTING)
        return false;

    if(team->partial_rest_cycles >= MAX_CONSECUTIVE_PARTIAL_REST_CYCLES)
        return false;

    if(team->specialization != incident->req_capability)
        return false;

    if(incident->priority != PRIORITY_CRITICAL)
        return false;

    uint8_t available_capacity = count_available_teams_for_capability(incident->req_capability);
    uint8_t waiting_demand = count_open_incidents_for_capability(incident->req_capability);

    if(available_capacity >= waiting_demand)
        return false;

    if(team->equipment_level != EQ_MAX_LEVEL)
        return false;

    /*
     * Fix for a real bug (confirmed empirically this session, see
     * get_ready_stamina()'s own doc comment in core/team.h for the
     * full writeup): this used to compare against get_soft_stamina_
     * floor() - the much lower "eligible for new work at all" bar
     * (20.0-40.0) - instead of get_ready_stamina() - the higher bar
     * (50.0-65.0) update_team() itself requires before naturally
     * promoting a RESTING team back to STATUS_AVAILABLE. That let
     * this steal path grab a team the moment it cleared the low bar,
     * long before it was actually "ready" by the standard the rest of
     * the codebase uses - triggering a real, observed infinite
     * RESTING<->EN_ROUTE oscillation for oversubscribed specializations
     * (available_capacity < waiting_demand is essentially permanent
     * for them, so every steal-eligible tick got exploited). Using the
     * same ready-stamina bar update_team() uses means stealing a
     * resting team now only ever happens right around when it would
     * have been naturally promoted anyway - no premature interruption,
     * no mismatch for evaluate_progress() (core/recovery_manager.c) to
     * correctly flag as deviation and bounce straight back.
     */
    return team->stamina >= get_ready_stamina(team);
}

static uint8_t count_available_teams_for_capability(uint8_t capability)
{
    uint8_t count = 0;

    for(uint8_t i = 0; i < NUM_TEAMS; i++)
    {
        if(g_teams[i].status == STATUS_AVAILABLE &&
           g_teams[i].specialization == capability)
        {
            count++;
        }
    }

    return count;
}

static uint8_t count_open_incidents_for_capability(uint8_t capability)
{
    uint8_t count = 0;

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        if(g_incidents[i].slot_occupied &&
           g_incidents[i].status == INC_OPEN &&
           g_incidents[i].req_capability == capability &&
           g_incidents[i].discovered)
        {
            count++;
        }
    }

    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        if(g_system_tasks[i].slot_occupied &&
           g_system_tasks[i].status == INC_OPEN &&
           g_system_tasks[i].req_capability == capability)
        {
            count++;
        }
    }

    return count;
}

/* Declared non-static in allocator.h - see that header's own comment
 * for why: core/recovery_manager.c shares this exact threshold now,
 * rather than keeping its own, driftable copy. */
float get_soft_stamina_floor(const TeamState_t *team)
{
    switch(team->fitness_tier)
    {
        case ARDUOUS:
            return ARDUOUS_MIN_STAMINA;

        case MODERATE:
            return MODERATE_MIN_STAMINA;

        case LIGHT:
            return LIGHT_MIN_STAMINA;

        default:
            return LIGHT_MIN_STAMINA;
    }
}

static void allocate_support_team(void)
{
    TeamState_t *support=NULL;
    for(uint8_t i=0;i<NUM_TEAMS;i++)
    {
        if(g_teams[i].specialization==SPEC_SUPPORT && g_teams[i].status==STATUS_AVAILABLE)
        {
            support=&g_teams[i];
            break;
        }
    }
    if(support==NULL) return;
    for(uint8_t i=0;i<NUM_INCIDENTS;i++)
    {
        IncidentState_t *incident = &g_incidents[i];
        if(!incident->slot_occupied)
            continue;
        if(incident->status == INC_RESOLVED)
            continue;
        if(incident->assigned_team == NO_ASSIGNMENT)
            continue;
        if(incident->support_team != NO_ASSIGNMENT)
            continue;
        if(!incident_needs_support(incident))
            continue;
        incident->support_team = support->team_id;
        support->assigned_task_pool = TASK_POOL_INCIDENT;
        support->assigned_task_slot = i;
        support->status = STATUS_EN_ROUTE;
        return;
    }
}

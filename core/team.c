#include "team.h"
#include "mission_state.h"
#include "random.h"
#include "incident.h"
#include "snapshot.h"
#include "mission_state_utils.h"
#include <stddef.h>
#include <string.h>

#define EQ_RESERVE_LEVEL 15

static float get_ready_stamina(const TeamState_t *team);

void generate_teams(uint32_t seed) //team
{
    random_seed_stream(STREAM_TEAMS, derive_seed(seed, STREAM_TEAMS));

    for(int i = 0; i < NUM_TEAMS; i++)
    {
        memset(&g_teams[i], 0, sizeof(g_teams[i]));
        g_teams[i].specialization = i % 5;
        uint8_t zone = random_range_stream(STREAM_TEAMS, 0, g_num_safe_zones - 1);
        g_teams[i].resting_zone_id = zone;
        g_teams[i].loc_x = g_safezones[zone].loc_x;
        g_teams[i].loc_y = g_safezones[zone].loc_y;
        g_teams[i].team_id = i;
        g_teams[i].status = STATUS_AVAILABLE;
        g_teams[i].assigned_task_pool = TASK_POOL_NONE;
        g_teams[i].assigned_task_slot = NO_ASSIGNMENT;
        g_teams[i].incident_queue_size = 0;
        g_teams[i].incident_queue_index = 0;

        for(uint8_t q = 0; q < CBBA_MAX_BUNDLE_SIZE; q++)
        {
            g_teams[i].incident_queue_slot[q] = NO_ASSIGNMENT;
            g_teams[i].incident_queue_id[q] = 0;
        }

        g_teams[i].stamina = 100;
        g_teams[i].hours_worked = 0;
        g_teams[i].ticks_worked_this_period = 0;

        switch(g_teams[i].specialization)
        {
            case SPEC_SEARCH:
                g_teams[i].equipment_flags = EQ_ROPE | EQ_K9;
                break;

            case SPEC_MEDICAL:
                g_teams[i].equipment_flags = EQ_MEDICAL_KIT;
                break;

            case SPEC_EXTRACTION:
                g_teams[i].equipment_flags = EQ_ROPE | EQ_SPREADER | EQ_STRUCTURAL;
                break;

            case SPEC_PERIMETER:
                g_teams[i].equipment_flags = EQ_HAZMAT_SUIT | EQ_STRUCTURAL | EQ_ROPE;
                break;

            case SPEC_SUPPORT:
                g_teams[i].equipment_flags = EQ_GENERATOR | EQ_MEDICAL_KIT | EQ_ROPE;
                break;

            default:
                g_teams[i].equipment_flags = 0;
                break;
        }

        g_teams[i].last_heartbeat_tick = 0;
        g_teams[i].equipment_level = EQ_MAX_LEVEL;

        switch(g_teams[i].specialization)
        {
            case SPEC_PERIMETER:
            case SPEC_SEARCH:
                g_teams[i].fitness_tier = MODERATE;
                break;

            case SPEC_SUPPORT:
            case SPEC_MEDICAL:
                g_teams[i].fitness_tier = LIGHT;
                break;

            case SPEC_EXTRACTION:
                g_teams[i].fitness_tier = ARDUOUS;
                break;
        }
    }
}

static void change_stamina(TeamState_t *team, float delta)
{
    float new_stamina = team->stamina + delta;

    if(new_stamina > MAX_STAMINA)
        new_stamina = MAX_STAMINA;

    if(new_stamina < MIN_STAMINA)
        new_stamina = MIN_STAMINA;

    team->stamina = new_stamina;
}

void update_team(TeamState_t *team) //team
{
    uint8_t replenish_rate;
    switch (team->status){
        case(STATUS_AVAILABLE): 
            break;
        case(STATUS_EN_ROUTE):
            if(team->stamina <= MIN_STAMINA)
            {
                /* Same failure mode as the ON_SCENE equipment-exhaustion
                 * check below: a team that bottoms out mid-travel has to
                 * be pulled off and routed to actually recover, not left
                 * to keep traveling indefinitely at the stamina floor
                 * with no path back to RESTING. abandon_task_for_recovery
                 * sends it toward a real safe zone (see incident.c),
                 * consistent with every other abandonment path. */
                snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                abandon_task_for_recovery(team);
                break;
            }
            change_stamina(team,-team_travel_stamina_cost(team));
            break;
        case(STATUS_ON_SCENE):
        {
            IncidentState_t *task = NULL ;

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
                team_set_available(team);
                break;
            }

            if(task->status == INC_IN_PROGRESS)
            {
                if(team->equipment_level == 0)
                {
                    snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                    abandon_task_for_recovery(team);
                    break;
                }

                if(team->specialization == SPEC_SUPPORT &&
                   task->incident_type != INCIDENT_EQUIPMENT_CACHE &&
                   team->equipment_level <= EQ_RESERVE_LEVEL)
                {
                    snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                    abandon_task_for_recovery(team);
                    break;
                }

                change_stamina(team,-STAMINA_OPERATION_COST);

                if(!(team->specialization == SPEC_SUPPORT &&
                     task->incident_type == INCIDENT_EQUIPMENT_CACHE))
                {
                    uint8_t equipment_cost = random_range_stream(STREAM_EQUIPMENT,1,5);

                    if(team->equipment_level <= equipment_cost)
                    {
                        team->equipment_level = 0;
                    }
                    else
                    {
                        team->equipment_level -= equipment_cost;
                    }
                }

                if(team->equipment_level == 0)
                {
                    snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                    abandon_task_for_recovery(team);
                    break;
                }

                if(team->specialization == SPEC_SUPPORT &&
                   task->incident_type != INCIDENT_EQUIPMENT_CACHE &&
                   team->equipment_level <= EQ_RESERVE_LEVEL)
                {
                    snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                    abandon_task_for_recovery(team);
                    break;
                }

                if(team->equipment_level <= 20 &&
                   task->incident_type != INCIDENT_EQUIPMENT_CACHE)
                {
                    int cache_already_requested = 0;

                    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
                    {
                        if(g_system_tasks[i].slot_occupied &&
                           g_system_tasks[i].status != INC_RESOLVED &&
                           g_system_tasks[i].incident_type == INCIDENT_EQUIPMENT_CACHE &&
                           g_system_tasks[i].spawned_by == team->team_id)
                        {
                            cache_already_requested = 1;
                            break;
                        }
                    }

                    if(!cache_already_requested)
                    {
                        create_equipment_cache_incident(team);
                    }
                }
            }
            break;
        }
        case(STATUS_RESTING):
            change_stamina(team,team_recovery_rate(team));
            replenish_rate = random_range_stream(STREAM_EQUIPMENT,5,10);
            if(team->equipment_level+replenish_rate<=EQ_MAX_LEVEL)
            team->equipment_level+=replenish_rate;
            else team->equipment_level=EQ_MAX_LEVEL;
            {
                /* A team that has used up its budget of consecutive
                 * minimal-rest deployments (NFPA 1584: full, formal rehab
                 * mandatory once a second consecutive work cycle would
                 * otherwise occur without one) must be taken all the way to
                 * full stamina before it's eligible again, not just its
                 * normal READY_STAMINA threshold. */
                float required_stamina = get_ready_stamina(team);
                if(team->partial_rest_cycles >= MAX_CONSECUTIVE_PARTIAL_REST_CYCLES)
                    required_stamina = MAX_STAMINA;

                if(team->stamina >= required_stamina && team->equipment_level == EQ_MAX_LEVEL)
                {
                    if(team->stamina >= MAX_STAMINA)
                        team->partial_rest_cycles = 0;

                    team_set_available(team);
                }
            }
            break;
        default:
            break;
    }
    if(team->status != STATUS_RESTING)
    {
        team->ticks_worked_this_period++;
        if(team->ticks_worked_this_period >= TICKS_PER_HOUR)
        {
            team->ticks_worked_this_period -= TICKS_PER_HOUR;
            if(team->hours_worked < 255)
                team->hours_worked++;
        }
    }

    if(team->fault_state.active[FAULT_CLASS_RESOURCE])
    {
        /* A resting team isn't expending physical effort, so a resource fault
         * shouldn't keep draining stamina while it's trying to recover - that
         * drain (up to 0.36/tick at severity 3) can exceed the recovery rate
         * itself (0.108-0.168/tick), permanently trapping a team near 0
         * stamina. Equipment can still degrade from the fault either way. */
        if(team->status != STATUS_RESTING)
        {
            float extra_stamina_drain = (float)team->fault_state.severity[FAULT_CLASS_RESOURCE] * 0.12f;
            change_stamina(team, -extra_stamina_drain);
        }

        if(team->equipment_level > team->fault_state.severity[FAULT_CLASS_RESOURCE])
            team->equipment_level -= team->fault_state.severity[FAULT_CLASS_RESOURCE];
        else
            team->equipment_level = 0;
    }

    if(!team->fault_state.active[FAULT_CLASS_COMMUNICATION] ||
       ((g_current_tick - team->fault_state.start_tick[FAULT_CLASS_COMMUNICATION]) %
        (uint32_t)(team->fault_state.severity[FAULT_CLASS_COMMUNICATION] + 1u) == 0))
    {
        team->last_heartbeat_tick = g_current_tick;
    }
}

void update_all_teams(void) //team
{
    extern uint32_t g_diag_ticks_resting[NUM_TEAMS];
    extern uint32_t g_diag_ticks_working[NUM_TEAMS];
    extern uint32_t g_diag_ticks_available[NUM_TEAMS];
    for(uint8_t i=0;i<NUM_TEAMS;i++)
    {
        update_team(&g_teams[i]);
        if(g_teams[i].status == STATUS_RESTING)
            g_diag_ticks_resting[i]++;
        else if(g_teams[i].status == STATUS_EN_ROUTE || g_teams[i].status == STATUS_ON_SCENE)
            g_diag_ticks_working[i]++;
        else if(g_teams[i].status == STATUS_AVAILABLE)
            g_diag_ticks_available[i]++;
    }
}

static float get_ready_stamina(const TeamState_t *team) //team
{
    switch(team->fitness_tier)
    {
        case(ARDUOUS): return ARDUOUS_READY_STAMINA;
        case(MODERATE):return MODERATE_READY_STAMINA;
        case(LIGHT):return LIGHT_READY_STAMINA ;
        default: return LIGHT_READY_STAMINA ;
    }
}

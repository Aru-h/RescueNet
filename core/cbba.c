#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "mission_state.h"
#include "mission_state_utils.h"
#include "allocator.h"
#include "scoring.h"
#include "path.h"
#include "random.h"
#include "team.h"





#define CBBA_ROUTE_IMPROVEMENT_EPSILON 0.0001f
#define CBBA_RELOCATE_MARGIN 0.005f

static uint32_t g_cbba_two_opt_improvements = 0;
static uint32_t g_cbba_relocate_improvements = 0;

static bool cbba_resolve_task_location(const TaskRef_t *task, int16_t *loc_x, int16_t *loc_y)
{
    if(task == NULL || loc_x == NULL || loc_y == NULL)
        return false;

    if(task->pool == TASK_POOL_INCIDENT && task->slot < NUM_INCIDENTS)
    {
        const IncidentState_t *incident = &g_incidents[task->slot];
        if(incident->slot_occupied)
        {
            *loc_x = incident->loc_x;
            *loc_y = incident->loc_y;
            return true;
        }
    }
    else if(task->pool == TASK_POOL_SYSTEM && task->slot < NUM_SYSTEM_TASKS)
    {
        const IncidentState_t *task_state = &g_system_tasks[task->slot];
        if(task_state->slot_occupied)
        {
            *loc_x = task_state->loc_x;
            *loc_y = task_state->loc_y;
            return true;
        }
    }

    return false;
}

static void cbba_sync_team_execution_twin(uint8_t team_id)
{
    MissionExecutionTwin_t *twin = &g_mission_execution_twin[team_id];
    const TeamState_t *team = &g_teams[team_id];
    const CBBAAgentState_t *agent = &g_cbba_agent_state[team_id];

    twin->team_id = team_id;
    twin->assigned_task_pool = team->assigned_task_pool;
    twin->assigned_task_slot = team->assigned_task_slot;
    twin->planned_path_size = 0;
    twin->expected_path_index = NO_ASSIGNMENT;
    twin->destination_x = team->loc_x;
    twin->destination_y = team->loc_y;
    twin->expected_arrival_tick = g_current_tick;
    twin->expected_stamina = team->stamina;
    twin->expected_operating_state = team->status;

    TaskRef_t destination = { TASK_POOL_NONE, NO_ASSIGNMENT };

    if(agent->path_size > 0)
    {
        twin->planned_path_size = agent->path_size;
        for(uint8_t i = 0; i < agent->path_size; i++)
            twin->planned_path[i] = agent->path[i];
        destination = agent->path[0];
    }
    else if(team->assigned_task_pool != TASK_POOL_NONE &&
            team->assigned_task_slot != NO_ASSIGNMENT)
    {
        destination.pool = team->assigned_task_pool;
        destination.slot = team->assigned_task_slot;
        twin->planned_path[0] = destination;
        twin->planned_path_size = 1;
    }

    if(cbba_resolve_task_location(&destination, &twin->destination_x, &twin->destination_y))
    {
        twin->expected_path_index = 0;

        if(team->status == STATUS_RESTING)
        {
            twin->expected_operating_state = team->status;
        }
        else if(team->loc_x == twin->destination_x && team->loc_y == twin->destination_y)
        {
            twin->expected_operating_state = STATUS_ON_SCENE;
        }
        else
        {
            twin->expected_operating_state = STATUS_EN_ROUTE;
        }

        uint16_t travel_cost = find_path_cost(
            team->loc_x,
            team->loc_y,
            twin->destination_x,
            twin->destination_y);

        if(travel_cost != UINT16_MAX)
        {
            float expected_stamina = team->stamina - ((float)travel_cost * team_travel_stamina_cost(team));

            if(expected_stamina < MIN_STAMINA)
                expected_stamina = MIN_STAMINA;

            if(expected_stamina > MAX_STAMINA)
                expected_stamina = MAX_STAMINA;

            twin->expected_stamina = expected_stamina;
            twin->expected_arrival_tick = g_current_tick + travel_cost;
        }
    }
}

void cbba_sync_mission_execution_twin(void)
{
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
        cbba_sync_team_execution_twin(team_id);
}

static bool cbba_task_equal(TaskRef_t a, TaskRef_t b)
{
    return a.pool == b.pool && a.slot == b.slot;
}

static bool cbba_path_contains(const CBBAAgentState_t *agent, TaskRef_t task)
{
    for(uint8_t i = 0; i < agent->path_size; i++)
    {
        if(cbba_task_equal(agent->path[i], task))
            return true;
    }
    return false;
}

static bool cbba_team_can_execute_task(uint8_t team_id, TaskRef_t task)
{
    if(task.pool != TASK_POOL_INCIDENT || task.slot >= NUM_INCIDENTS)
        return false;

    IncidentState_t *incident = &g_incidents[task.slot];
    TeamState_t *team = &g_teams[team_id];

    if(!incident->slot_occupied || incident->status != INC_OPEN)
        return false;

    return team_can_accept_incident(team, incident) &&
           team_has_minimum_equipment(team, incident);
}

static void cbba_reverse_path_segment(TaskRef_t *path, uint8_t first, uint8_t last)
{
    while(first < last)
    {
        TaskRef_t temp = path[first];
        path[first] = path[last];
        path[last] = temp;
        first++;
        last--;
    }
}

static void cbba_two_opt_agent(uint8_t team_id)
{
    CBBAAgentState_t *agent = &g_cbba_agent_state[team_id];

    if(agent->path_size < 3)
        return;

    bool improved = true;

    while(improved)
    {
        improved = false;
        float current_utility = calculate_route_utility(
            &g_teams[team_id], agent->path, agent->path_size);

        for(uint8_t first = 1; first + 1 < agent->path_size && !improved; first++)
        {
            for(uint8_t last = first + 1; last < agent->path_size; last++)
            {
                TaskRef_t trial[CBBA_MAX_BUNDLE_SIZE];
                for(uint8_t i = 0; i < agent->path_size; i++)
                    trial[i] = agent->path[i];

                cbba_reverse_path_segment(trial, first, last);

                float trial_utility = calculate_route_utility(
                    &g_teams[team_id], trial, agent->path_size);

                if(trial_utility > current_utility + CBBA_ROUTE_IMPROVEMENT_EPSILON)
                {
                    for(uint8_t i = 0; i < agent->path_size; i++)
                        agent->path[i] = trial[i];
                    g_cbba_two_opt_improvements++;
                    improved = true;
                    break;
                }
            }
        }
    }
}

static void cbba_set_global_winner(uint8_t incident_slot, uint8_t winner)
{
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
        g_cbba_agent_state[team_id].winner[incident_slot] = winner;
}

static bool cbba_try_best_relocate(void)
{
    float best_delta = CBBA_RELOCATE_MARGIN;
    uint8_t best_donor = NO_ASSIGNMENT;
    uint8_t best_receiver = NO_ASSIGNMENT;
    uint8_t best_donor_index = 0;
    uint8_t best_insert_position = 0;

    for(uint8_t donor_id = 0; donor_id < NUM_TEAMS; donor_id++)
    {
        CBBAAgentState_t *donor = &g_cbba_agent_state[donor_id];
        if(donor->path_size < 2)
            continue;

        for(uint8_t donor_index = 1; donor_index < donor->path_size; donor_index++)
        {
            TaskRef_t task = donor->path[donor_index];

            for(uint8_t receiver_id = 0; receiver_id < NUM_TEAMS; receiver_id++)
            {
                if(receiver_id == donor_id)
                    continue;

                CBBAAgentState_t *receiver = &g_cbba_agent_state[receiver_id];
                if(receiver->path_size >= CBBA_COMMITMENT_LIMIT ||
                   cbba_path_contains(receiver, task) ||
                   !cbba_team_can_execute_task(receiver_id, task))
                {
                    continue;
                }

                float before = calculate_route_utility(
                    &g_teams[donor_id], donor->path, donor->path_size) +
                    calculate_route_utility(
                    &g_teams[receiver_id], receiver->path, receiver->path_size);

                TaskRef_t donor_trial[CBBA_MAX_BUNDLE_SIZE];
                uint8_t donor_trial_size = 0;
                for(uint8_t i = 0; i < donor->path_size; i++)
                {
                    if(i != donor_index)
                        donor_trial[donor_trial_size++] = donor->path[i];
                }

                for(uint8_t insert_position = 1;
                    insert_position <= receiver->path_size;
                    insert_position++)
                {
                    TaskRef_t receiver_trial[CBBA_MAX_BUNDLE_SIZE];
                    uint8_t receiver_trial_size = 0;

                    for(uint8_t i = 0; i <= receiver->path_size; i++)
                    {
                        if(i == insert_position)
                            receiver_trial[receiver_trial_size++] = task;
                        if(i < receiver->path_size)
                            receiver_trial[receiver_trial_size++] = receiver->path[i];
                    }

                    float after = calculate_route_utility(
                        &g_teams[donor_id], donor_trial, donor_trial_size) +
                        calculate_route_utility(
                        &g_teams[receiver_id], receiver_trial, receiver_trial_size);
                    float delta = after - before;

                    if(delta > best_delta)
                    {
                        best_delta = delta;
                        best_donor = donor_id;
                        best_receiver = receiver_id;
                        best_donor_index = donor_index;
                        best_insert_position = insert_position;
                    }
                }
            }
        }
    }

    if(best_donor == NO_ASSIGNMENT)
        return false;

    CBBAAgentState_t *donor = &g_cbba_agent_state[best_donor];
    CBBAAgentState_t *receiver = &g_cbba_agent_state[best_receiver];
    TaskRef_t moved_task = donor->path[best_donor_index];

    for(uint8_t i = best_donor_index; i + 1 < donor->path_size; i++)
        donor->path[i] = donor->path[i + 1];
    donor->path_size--;

    for(uint8_t i = receiver->path_size; i > best_insert_position; i--)
        receiver->path[i] = receiver->path[i - 1];
    receiver->path[best_insert_position] = moved_task;
    receiver->path_size++;

    cbba_set_global_winner(moved_task.slot, best_receiver);
    g_cbba_relocate_improvements++;
    return true;
}

void cbba_improve_routes(void)
{
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
        cbba_two_opt_agent(team_id);

    while(cbba_try_best_relocate())
    {
        for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
            cbba_two_opt_agent(team_id);
    }

    cbba_sync_mission_execution_twin();
}

uint32_t cbba_get_two_opt_improvements(void)
{
    return g_cbba_two_opt_improvements;
}

uint32_t cbba_get_relocate_improvements(void)
{
    return g_cbba_relocate_improvements;
}

void cbba_reset_improvement_counters(void)
{
    g_cbba_two_opt_improvements = 0;
    g_cbba_relocate_improvements = 0;
}

static bool cbba_bundle_contains(uint8_t team_id,TaskRef_t task)
{
        for(int j=0;j<g_cbba_agent_state[team_id].bundle_size;j++)
        {
            TaskRef_t Task=g_cbba_agent_state[team_id].bundle[j];
            if(Task.pool==task.pool && Task.slot==task.slot) return true;
        }
    return false;
}

void cbba_init(void)
{
    for(int i=0;i<NUM_TEAMS;i++)
    {
        CBBAAgentState_t *agent=&g_cbba_agent_state[i];
        MissionExecutionTwin_t *twin = &g_mission_execution_twin[i];
        agent->bundle_size = 0;
        agent->path_size = 0;
        for(int j=0;j<NUM_INCIDENTS;j++)
        {
            agent->winner[j]=NO_ASSIGNMENT;
            agent->winning_bid[j]=0.0;
        }

        for(int k=0;k<NUM_TEAMS;k++)
        {
            agent->timestamp[k]=0;
        }

        twin->team_id = (uint8_t)i;
        twin->assigned_task_pool = TASK_POOL_NONE;
        twin->assigned_task_slot = NO_ASSIGNMENT;
        twin->planned_path_size = 0;
        twin->expected_path_index = NO_ASSIGNMENT;
        twin->destination_x = g_teams[i].loc_x;
        twin->destination_y = g_teams[i].loc_y;
        twin->expected_arrival_tick = g_current_tick;
        twin->expected_stamina = g_teams[i].stamina;
        twin->expected_operating_state = g_teams[i].status;

    }
}

void cbba_build_bundle(uint8_t team_id)
{
    TeamState_t *team=&g_teams[team_id];
    CBBAAgentState_t *agent=&g_cbba_agent_state[team_id];
    
    TaskRef_t task, best_task;
    
    while(agent->bundle_size < CBBA_COMMITMENT_LIMIT)
    {
        float best_bid=-1;
        bool found_task=false;
        uint8_t selected_insert_position = 0;
        for(int i=0;i<NUM_INCIDENTS;i++)
        {
            if(g_incidents[i].slot_occupied==0) continue;
            if(g_incidents[i].status!=INC_OPEN) continue;
            IncidentState_t *incident=&g_incidents[i];
            task.pool=TASK_POOL_INCIDENT;
            task.slot=i;
            if(cbba_bundle_contains(team_id,task)) continue;
            if(!team_can_accept_incident(team, incident)) continue;
            if(!team_has_minimum_equipment(team, incident)) continue;
            uint8_t insert_position = 0;
            float bid = calculate_marginal_bundle_bid(
                team_id,
                agent,
                incident,
                &insert_position);
            if(agent->winner[i] != NO_ASSIGNMENT &&
            !candidate_claim_is_better(team_id,bid,agent->winner[i],agent->winning_bid[i],incident))
                    continue;
            if(!found_task || bid > best_bid)
            {
                best_bid = bid;
                found_task = true;
                best_task = task;
                selected_insert_position = insert_position;
            }
        }
        if(!found_task)
        {
            cbba_sync_team_execution_twin(team_id);
            return;
        }
        
        agent->bundle[agent->bundle_size] = best_task;
        agent->bundle_bid[agent->bundle_size] = best_bid;
        agent->bundle_size++;

        for(uint8_t j = agent->path_size;
            j > selected_insert_position;
            j--)
        {
            agent->path[j] = agent->path[j - 1];
        }

        agent->path[selected_insert_position] = best_task;
        agent->path_size++;

        agent->winner[best_task.slot] = team_id;
        agent->winning_bid[best_task.slot] = best_bid;
        
        
    }

    cbba_sync_team_execution_twin(team_id);
    return;
}

static void cbba_release_lost_tasks(void)
{
    for(int i=0;i<NUM_TEAMS;i++)
    {
        bool winner=true;
        int id,lost_index;
        CBBAAgentState_t *agent=&g_cbba_agent_state[i];
        TaskRef_t task;
        for(int j=0;j<agent->bundle_size;j++)
        {
            task=agent->bundle[j];
            if(i!=agent->winner[task.slot])
            {
                winner=false;
                lost_index=j;
                break;
            }
        }
        if(!winner)
        {
            id=lost_index;
            while(id<agent->bundle_size)
            {
                task.slot=agent->bundle[id].slot;
                agent->bundle[id].slot=NO_ASSIGNMENT;
                agent->bundle_bid[id]=0.0;
                if(agent->winner[task.slot]==i)
                {
                    agent->winner[task.slot]=NO_ASSIGNMENT;
                    agent->winning_bid[task.slot]=0.0;
                }
                id++;
            }

            agent->bundle_size = lost_index;

            uint8_t new_path_size = 0;

            for(uint8_t path_index = 0;
                path_index < agent->path_size;
                path_index++)
            {
                TaskRef_t path_task = agent->path[path_index];
                bool still_in_bundle = false;

                for(uint8_t bundle_index = 0;
                    bundle_index < agent->bundle_size;
                    bundle_index++)
                {
                    TaskRef_t bundle_task = agent->bundle[bundle_index];

                    if(path_task.pool == bundle_task.pool &&
                       path_task.slot == bundle_task.slot)
                    {
                        still_in_bundle = true;
                        break;
                    }
                }

                if(still_in_bundle)
                {
                    agent->path[new_path_size] = path_task;
                    new_path_size++;
                }
            }

            agent->path_size = new_path_size;
        }
    }
}

#define CBBA_NUM_CONSENSUS_PAIRS (NUM_TEAMS * (NUM_TEAMS - 1))

static void cbba_shuffle_pairs(uint8_t *pairs, uint8_t count)
{
    for(uint8_t i = count; i > 1; i--)
    {
        uint8_t j = (uint8_t)random_range_stream(STREAM_CBBA_CONSENSUS, 0, i - 1);
        uint8_t temp = pairs[i - 1];
        pairs[i - 1] = pairs[j];
        pairs[j] = temp;
    }
}

/* Pairwise CBBA consensus: receiver_id updates its (winner, winning_bid) belief
   for each open task after "hearing from" sender_id, following the CBBA action
   table (Choi/Brunet/How 2009 Table I): whether to adopt sender's claim depends
   on whether the sender claims the task itself, claims the receiver already
   has it, or is relaying a third agent's claim -- in which case timestamp
   freshness (agent->timestamp[]) breaks the tie instead of raw bid alone. */
/* Wire format only -- a copied snapshot of one agent's belief at broadcast
   time. This is what a real board would put on the wire; no function below
   is allowed to read another agent's live CBBAAgentState_t directly. */
typedef struct
{
    uint8_t sender_id;
    uint8_t winner[NUM_INCIDENTS];
    float winning_bid[NUM_INCIDENTS];
    uint32_t timestamp[NUM_TEAMS];
} CBBAMessage_t;

static void cbba_broadcast(uint8_t team_id, CBBAMessage_t *out)
{
    const CBBAAgentState_t *agent = &g_cbba_agent_state[team_id];

    out->sender_id = team_id;

    for(int j = 0; j < NUM_INCIDENTS; j++)
    {
        out->winner[j] = agent->winner[j];
        out->winning_bid[j] = agent->winning_bid[j];
    }

    for(int k = 0; k < NUM_TEAMS; k++)
        out->timestamp[k] = agent->timestamp[k];
}

/* Pairwise CBBA consensus, applied by receiver_id after receiving msg
   (Choi/Brunet/How 2009 Table I): whether to adopt the sender's claim
   depends on whether the sender claims the task itself, claims the receiver
   already has it, or is relaying a third agent's claim -- in which case
   timestamp freshness breaks the tie instead of raw bid alone. Only
   receiver's own struct is mutated; msg is a read-only copy, never a
   pointer into another agent's live state. */
static void cbba_receive_and_update(uint8_t receiver_id, const CBBAMessage_t *msg)
{
    CBBAAgentState_t *receiver = &g_cbba_agent_state[receiver_id];
    uint8_t sender_id = msg->sender_id;
    const float BID_EPSILON = 0.0001f;

    for(int j = 0; j < NUM_INCIDENTS; j++)
    {
        if(g_incidents[j].slot_occupied == 0) continue;
        if(g_incidents[j].status != INC_OPEN) continue;

        uint8_t z_i = receiver->winner[j];
        float   y_i = receiver->winning_bid[j];
        uint8_t z_k = msg->winner[j];
        float   y_k = msg->winning_bid[j];

        if(z_k == NO_ASSIGNMENT)
            continue;

        bool take_senders_claim = false;

        if(z_k == sender_id)
        {
            if(z_i == receiver_id)
                take_senders_claim = candidate_claim_is_better(
                    sender_id, y_k, receiver_id, y_i, &g_incidents[j]);
            else if(z_i == sender_id || z_i == NO_ASSIGNMENT)
                take_senders_claim = true;
            else
                take_senders_claim =
                    (y_k > y_i + BID_EPSILON) ||
                    (y_k > y_i - BID_EPSILON &&
                     receiver->timestamp[z_i] <= msg->timestamp[z_i]);
        }
        else if(z_k == receiver_id)
        {
            take_senders_claim = false;
        }
        else
        {
            uint8_t m = z_k;

            if(z_i == receiver_id)
                take_senders_claim = false;
            else if(z_i == sender_id)
                take_senders_claim = true;
            else if(z_i == m)
                take_senders_claim =
                    (y_k > y_i + BID_EPSILON) ||
                    (y_k > y_i - BID_EPSILON &&
                     receiver->timestamp[m] <= msg->timestamp[m]);
            else if(z_i == NO_ASSIGNMENT)
                take_senders_claim = true;
            else
                take_senders_claim =
                    (receiver->timestamp[m] < msg->timestamp[m]) ||
                    (receiver->timestamp[m] == msg->timestamp[m] &&
                     y_k > y_i + BID_EPSILON);
        }

        if(take_senders_claim)
        {
            receiver->winner[j] = z_k;
            receiver->winning_bid[j] = y_k;
        }
    }

    receiver->timestamp[sender_id] = g_current_tick;

    for(int k = 0; k < NUM_TEAMS; k++)
    {
        if(k == receiver_id || k == sender_id) continue;
        if(msg->timestamp[k] > receiver->timestamp[k])
            receiver->timestamp[k] = msg->timestamp[k];
    }
}

/* Mirrors the cadence gate team.c already uses for degraded heartbeats
   (FAULT_CLASS_COMMUNICATION reduces broadcast frequency rather than
   killing it outright, scaled by severity) -- same fault, same effect,
   applied to CBBA packets instead of heartbeats. */
static uint8_t cbba_communication_link_up(uint8_t team_id)
{
    const TeamFaultState_t *fault_state = &g_teams[team_id].fault_state;

    if(!fault_state->active[FAULT_CLASS_COMMUNICATION])
        return 1;

    uint32_t elapsed = g_current_tick - fault_state->start_tick[FAULT_CLASS_COMMUNICATION];
    uint32_t cadence = (uint32_t)fault_state->severity[FAULT_CLASS_COMMUNICATION] + 1u;

    return (elapsed % cadence) == 0;
}

void cbba_consensus(void)
{
    CBBAMessage_t outbox[NUM_TEAMS];
    uint8_t link_up[NUM_TEAMS];

    /* every agent broadcasts one snapshot of its own current belief --
       this happens before any consensus processing, so no agent's message
       can reflect an update it only received partway through this round */
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        cbba_broadcast(team_id, &outbox[team_id]);
        link_up[team_id] = cbba_communication_link_up(team_id);
    }

    uint8_t pairs[CBBA_NUM_CONSENSUS_PAIRS];
    uint8_t count = 0;

    for(uint8_t i = 0; i < NUM_TEAMS; i++)
    {
        for(uint8_t k = 0; k < NUM_TEAMS; k++)
        {
            if(i == k) continue;
            pairs[count++] = (uint8_t)(i * NUM_TEAMS + k);
        }
    }

    cbba_shuffle_pairs(pairs, count);

    for(uint8_t p = 0; p < count; p++)
    {
        uint8_t receiver_id = pairs[p] / NUM_TEAMS;
        uint8_t sender_id   = pairs[p] % NUM_TEAMS;

        if(!link_up[sender_id])
            continue; /* sender's radio is down this tick -- packet never went out */

        cbba_receive_and_update(receiver_id, &outbox[sender_id]);
    }

    cbba_release_lost_tasks();
    cbba_sync_mission_execution_twin();
}

void cbba_run(void)
{
    /* Safe because nothing between path_cache_begin() and path_cache_end()
     * touches the grid (navigability/travel cost) or moves any team --
     * environment_update_hazard_propagation() and move_all_teams() only
     * run later, inside simulation_step(), after this function returns.
     * See the comment on path_cache_begin() in path.h. */
    path_cache_begin();

    cbba_init();

    for(int round = 0; round < NUM_TEAMS * CBBA_MAX_BUNDLE_SIZE; round++)
    {
        for(int team_id = 0; team_id < NUM_TEAMS; team_id++)
        {
            cbba_build_bundle((uint8_t)team_id);
        }

        cbba_consensus();
    }

    cbba_improve_routes();
    cbba_sync_mission_execution_twin();

    path_cache_end();
}

#include "cbba.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "mission_state.h"
#include "mission_state_utils.h"
#include "allocator.h"
#include "scoring.h"
#include "path.h"
#include "random.h"
#include "team.h"
#include "comms_transport.h"

/* CBBA_DEBUG_TIMING: opt-in diagnostic logging for
   cbba_ingest_remote_mesh_replies()'s CBBA_ROUND_TIMEOUT_MS wait -
   see the log line inside that function below. Off by default (not
   defined anywhere in the board Makefiles), so it costs real boards
   nothing; the laptop two-board CBBA harness build turns it on via
   -DCBBA_DEBUG_TIMING so failing runs record whether the 530ms
   deadline was actually hit. printf/stdio is safe to pull in here
   only because this include is itself compiled out unless that flag
   is set - CORE_SOURCES (this file included) is also compiled
   straight into board firmware, which doesn't get a free pass on
   stdio the way a host build does. */
#ifdef CBBA_DEBUG_TIMING
#include <stdio.h>
#endif





#define CBBA_ROUTE_IMPROVEMENT_EPSILON 0.0001f
#define CBBA_RELOCATE_MARGIN 0.005f

/*
 * Manual dashboard override pin (core/mission_link_override.h writes
 * CBBAAgentState_t.override_active/override_incident_id; this file
 * reads them - see that header's end-to-end doc comment for the full
 * path from a drag-drop on the dashboard down to here).
 *
 * A sentinel winning_bid no normal marginal bid can ever reach:
 * calculate_marginal_bundle_bid()'s scores are built from capability_
 * score()/equipment_score()/stamina_score()/distance terms (see
 * scoring.c), all bounded, small multiples of 1.0 - nowhere near
 * 1e9f. Using this as both the pinned team's own winning_bid[] entry
 * AND the value candidate_claim_is_better() compares incoming bids
 * against is what makes the pin survive: it propagates through the
 * exact same CBBA consensus gossip (cbba_broadcast()/cbba_receive_
 * and_update()) every normal claim already uses, rather than
 * bypassing that mechanism - see cbba_apply_override_pin() below.
 */
#define CBBA_OVERRIDE_PIN_BID 1.0e9f

static uint32_t g_cbba_two_opt_improvements = 0;
static uint32_t g_cbba_relocate_improvements = 0;

/*
 * Per-board team-locality check: a board only computes bids for the
 * team_ids it hosts. Thin wrapper around
 * comms_transport_board_id_for_team() - reuse, don't reimplement the
 * team_id/5 rule (see comms_transport.h). g_cbba_agent_state stays
 * full NUM_TEAMS-wide regardless of locality: winner[]/timestamp[]
 * for a remote team hold this board's received belief about that
 * team (via mesh consensus - see cbba_ingest_remote_mesh_replies()
 * below), even though this board never builds a bundle/path on that
 * team's behalf.
 */
/* Not static: cbba_init()/cbba_consensus() follow the same "public,
   but not declared in cbba.h" convention already in this file for
   anything a standalone selftest legitimately needs to reach without
   standing up a full mission (grid/incidents/path cache) just to
   exercise per-board scoping. */
bool cbba_team_is_local(uint8_t team_id)
{
    return comms_transport_board_id_for_team(team_id) ==
           g_comms_transport_this_board_id;
}

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
        CBBAAgentState_t *donor;

        if(!cbba_team_is_local(donor_id))
            continue; /* this board never builds a path/bundle for a
                          remote team, so donor->path_size < 2 already
                          skips it below in practice - kept explicit
                          as a defensive guard against that invariant
                          changing later, and for readability */

        donor = &g_cbba_agent_state[donor_id];
        if(donor->path_size < 2)
            continue;

        for(uint8_t donor_index = 1; donor_index < donor->path_size; donor_index++)
        {
            TaskRef_t task = donor->path[donor_index];

            for(uint8_t receiver_id = 0; receiver_id < NUM_TEAMS; receiver_id++)
            {
                CBBAAgentState_t *receiver;

                if(receiver_id == donor_id || !cbba_team_is_local(receiver_id))
                    continue; /* this board must not mutate a remote
                                  team's path - relocation is local
                                  bid-adjustment, same board scope as
                                  cbba_build_bundle() */

                receiver = &g_cbba_agent_state[receiver_id];
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

/* Self-contained lookup, same "each file keeps its own copy rather
   than sharing" precedent core/mission_link.c's mission_link_find_
   incident_slot() already established for this exact pattern
   (incident_id is not a slot index - see that function's own
   comment). Distinct name/static scope, not a re-export - this file
   has no dependency on mission_link.c and shouldn't gain one just for
   a five-line loop. */
static uint8_t cbba_find_incident_slot_by_id(uint32_t incident_id)
{
    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        if(g_incidents[i].slot_occupied && g_incidents[i].incident_id == incident_id)
        {
            return i;
        }
    }

    return NO_ASSIGNMENT;
}

/*
 * Force-injects team_id's active override pin (if any) into its own
 * bundle/path, ahead of the normal marginal-bid competition loop
 * below, and stamps a CBBA_OVERRIDE_PIN_BID winning_bid no other
 * team's normal bid can beat once it propagates through consensus
 * (see the #define's own comment above).
 *
 * Called at the very top of cbba_build_bundle(), which cbba_run()'s
 * round loop invokes many times per tick for the same team_id (see
 * that loop's own comment) - idempotent across those repeat calls:
 * once injected, cbba_bundle_contains() below stops it from being
 * injected a second time, same guard the normal competitive loop
 * already relies on for every other task.
 *
 * Pin lifecycle - cleared here, lazily, in exactly two cases:
 *   1. The pinned incident_id no longer resolves to an occupied slot,
 *      or that slot's status is no longer INC_OPEN. The latter is the
 *      NORMAL, expected end of a successful pin: allocate_all_
 *      incidents() (core/allocator.c) only flips a slot to INC_ASSIGNED
 *      via assign_incident(), which runs AFTER cbba_run() (and
 *      therefore after every cbba_build_bundle() call) has already
 *      finished for that tick - so this function can't observe
 *      "mission accomplished" until the NEXT tick's cbba_run() calls
 *      it again. Leaving override_active set for that one extra tick
 *      is harmless: cbba_bundle_contains() already prevents a
 *      duplicate injection in the meantime, and nothing else reads
 *      override_active except this function.
 *   2. bundle_size is already at CBBA_COMMITMENT_LIMIT with the pinned
 *      task not among the entries (would only happen if something
 *      else filled the bundle first within the same tick's repeat
 *      calls, which the injection-at-the-top ordering here prevents
 *      in practice, but checked defensively rather than assumed).
 *      This one does NOT clear the pin - it's a "try again next
 *      round/tick" condition, not "no longer actionable", since
 *      bundle_size resets to 0 every cbba_init() (start of the next
 *      cbba_run()).
 */
static void cbba_apply_override_pin(uint8_t team_id)
{
    CBBAAgentState_t *agent = &g_cbba_agent_state[team_id];
    uint8_t pinned_slot;
    TaskRef_t task;

    if(!agent->override_active)
        return;

    pinned_slot = cbba_find_incident_slot_by_id(agent->override_incident_id);

    if(pinned_slot == NO_ASSIGNMENT ||
       !g_incidents[pinned_slot].slot_occupied ||
       g_incidents[pinned_slot].status != INC_OPEN)
    {
        /* No longer actionable (see case 1 above) - drop the stale
           pin so a future incident that happens to reuse this same
           incident_id (or team_id, if this pin's team later gets a
           fresh override) doesn't inherit a leftover force. */
        agent->override_active = 0u;
        return;
    }

    task.pool = TASK_POOL_INCIDENT;
    task.slot = pinned_slot;

    if(agent->bundle_size >= CBBA_COMMITMENT_LIMIT || cbba_bundle_contains(team_id, task))
        return; /* case 2 above, or already injected earlier this
                    tick's repeat calls - nothing to do */

    agent->bundle[agent->bundle_size] = task;
    agent->bundle_bid[agent->bundle_size] = CBBA_OVERRIDE_PIN_BID;
    agent->bundle_size++;

    /* Front of the path: a forced assignment should be this team's
       very next stop, not queued behind whatever it already committed
       to earlier this same bidding cycle. This also piggybacks on an
       existing invariant elsewhere in this file: cbba_try_best_
       relocate()'s donor loop starts at donor_index = 1, never 0
       (path[0] is this team's current/next destination - see cbba_
       sync_team_execution_twin()'s own use of path[0] - and is
       already treated as committed, not up for route-relocation). By
       landing the pin at path[0], the post-convergence relocate pass
       (cbba_improve_routes() -> cbba_try_best_relocate(), which runs
       every cbba_run() after this function's round loop finishes)
       can never pick it up as a donor candidate and hand it to a
       different team for a route-utility gain - exactly the "don't
       let normal [bidding OR route optimization] override it"
       requirement this pin exists to satisfy. */
    for(uint8_t j = agent->path_size; j > 0; j--)
    {
        agent->path[j] = agent->path[j - 1];
    }
    agent->path[0] = task;
    agent->path_size++;

    agent->winner[pinned_slot] = team_id;
    agent->winning_bid[pinned_slot] = CBBA_OVERRIDE_PIN_BID;
}

void cbba_build_bundle(uint8_t team_id)
{
    TeamState_t *team=&g_teams[team_id];
    CBBAAgentState_t *agent=&g_cbba_agent_state[team_id];
    
    TaskRef_t task, best_task;

    cbba_apply_override_pin(team_id);

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

/*
 * ---------------------------------------------------------------
 * Real mesh exchange (CBBA_ROUND_TIMEOUT_MS - see cbba.h)
 * ---------------------------------------------------------------
 * cbba_consensus() above is the in-process simulation of the whole
 * mesh (every team, same address space, same tick - see cbba_run()'s
 * round loop) and stays exactly as it was; it's what lets the
 * laptop/desktop build and any single-process test model a
 * fully-converged multi-team auction without real radios. The two
 * functions below are the actual comms_transport-backed path a real
 * board uses to reach teams hosted on a DIFFERENT board - something
 * cbba_consensus() cannot do for real, since a real board process
 * only ever has its own local NUM_TEAMS-wide array, not another
 * board's live state.
 */

/* Weak default: no real clock behind it - see cbba.h's cbba_now_ms()
   doc comment for the full explanation and the real-hardware
   override contract (same weak-default pattern as
   comms_transport.c's lora_spi_transmit). Advances a simulated
   millisecond counter by a fixed step every call, purely so the
   bounded wait loop below always terminates in a small, deterministic
   number of calls even with nothing to poll - never an infinite
   spin, and never an instant (zero-wait) timeout either. */
#ifndef CBBA_SIM_CLOCK_STEP_MS
#define CBBA_SIM_CLOCK_STEP_MS 10u
#endif

__attribute__((weak)) uint32_t cbba_now_ms(void)
{
    static uint32_t s_sim_clock_ms = 0u;
    s_sim_clock_ms += CBBA_SIM_CLOCK_STEP_MS;
    return s_sim_clock_ms;
}

/* Sends every locally-hosted team's current belief snapshot to every
   OTHER board over comms_transport (real LoRa once that driver
   exists, an in-process queue round-trip today when boards happen to
   coincide - see comms_transport_send()). One CBBAMessage_t per
   local team, the same payload cbba_consensus()'s in-process outbox
   already builds via cbba_broadcast() - reused here, not re-derived. */
/* Not static - see cbba_team_is_local()'s comment above for why. */
void cbba_broadcast_local_teams(void)
{
    uint8_t num_boards = (uint8_t)((NUM_TEAMS + 4u) / 5u); /* team_id/5
        rule - see comms_transport_board_id_for_team() */

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        CBBAMessage_t msg;

        if(!cbba_team_is_local(team_id))
            continue;

        cbba_broadcast(team_id, &msg);

        for(uint8_t board = 0; board < num_boards; board++)
        {
            uint8_t representative_team_id;

            if(board == g_comms_transport_this_board_id)
                continue;

            /* Any team_id on that board reaches the whole board -
               comms_transport routes by board, not by the specific
               receiver_team_id passed here (see comms_transport.h);
               the receiving board fans this out to all of ITS local
               teams itself, in cbba_ingest_remote_mesh_replies()
               below. */
            representative_team_id = (uint8_t)(board * 5u);

            (void)comms_transport_send(team_id, representative_team_id,
                                        LORA_FRAME_TYPE_CBBA,
                                        COMMS_PRECEDENCE_PRIORITY,
                                        (const uint8_t *)&msg,
                                        (uint8_t)sizeof(msg));
        }
    }
}

/* Drains this board's comms_transport inbox for CBBA frames and folds
   each one into every locally-hosted team's belief via
   cbba_receive_and_update() - the same function cbba_consensus()'s
   in-process gossip already uses, just fed from a real (or, until a
   real multi-board harness exists, locally-stubbed - see
   comms_transport_send()/comms_transport_poll_received(), both real,
   no stub needed) wire message instead of an in-memory outbox[]
   entry.

   Actively waits up to CBBA_ROUND_TIMEOUT_MS for something to arrive:
   comms_transport_poll_received() is itself non-blocking (returns 0
   immediately on an empty inbox), so without an explicit wait this
   function would finalize optimistically with zero wait - exactly
   what CBBA_ROUND_TIMEOUT_MS exists to rule out (see cbba.h). It is
   equally deliberate that this does NOT wait past the timeout even
   if nothing has arrived yet: a real board still has to move its
   teams every tick whether or not the other board replied in time,
   so this finalizes with whatever's been ingested so far rather than
   blocking indefinitely. */
/* Not static - see cbba_team_is_local()'s comment above for why. */
void cbba_ingest_remote_mesh_replies(void)
{
    uint32_t round_start = cbba_now_ms();
#ifdef CBBA_DEBUG_TIMING
    bool reply_ingested_this_round = false;
#endif

    while((cbba_now_ms() - round_start) < CBBA_ROUND_TIMEOUT_MS)
    {
        uint8_t sender_team_id;
        uint8_t frame_type;
        uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t payload_len;
        CBBAMessage_t msg;

        if(!comms_transport_poll_received(&sender_team_id, &frame_type,
                                           payload, &payload_len))
        {
            continue; /* nothing waiting right now - keep waiting until
                          the timeout, rather than giving up on the
                          first empty poll */
        }

        if(frame_type != LORA_FRAME_TYPE_CBBA)
            continue; /* mission-link's RX task (Prompt 3) owns those */

        if(payload_len != (uint8_t)sizeof(CBBAMessage_t))
            continue; /* protocol drift or a corrupt frame - drop it
                          and keep draining rather than aborting the
                          whole round over one bad message */

        memcpy(&msg, payload, sizeof(msg));
        (void)sender_team_id; /* msg.sender_id (wire field, set by
            cbba_broadcast()) is authoritative - see comms_transport.h's
            "Open question" note: sender identity for a genuinely
            separate board isn't on the wire at the comms_transport
            layer today, only inside CBBAMessage_t itself */

#ifdef CBBA_DEBUG_TIMING
        reply_ingested_this_round = true;
#endif

        for(uint8_t receiver_id = 0; receiver_id < NUM_TEAMS; receiver_id++)
        {
            if(!cbba_team_is_local(receiver_id))
                continue; /* only this board's own teams update their
                              belief from a mesh reply - the other
                              board's teams are that board's business */

            cbba_receive_and_update(receiver_id, &msg);
        }
    }

#ifdef CBBA_DEBUG_TIMING
    /* Fires exactly when the CBBA_ROUND_TIMEOUT_MS wait above ran out
       without ever ingesting a single valid CBBA reply this round -
       i.e. the 530ms deadline was hit dry. If a reply DID arrive
       (reply_ingested_this_round true) but the boards still end up
       disagreeing on the winner, this line stays silent and the bug
       is in cbba_receive_and_update()'s tie-break logic instead, not
       the timeout. */
    if(!reply_ingested_this_round)
    {
        fprintf(stderr,
                "[cbba] round timeout expired, no reply from peer "
                "(board=%u, waited=%ums, CBBA_ROUND_TIMEOUT_MS=%ums)\n",
                (unsigned)g_comms_transport_this_board_id,
                (unsigned)(cbba_now_ms() - round_start),
                (unsigned)CBBA_ROUND_TIMEOUT_MS);
    }
#endif
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
            if(!cbba_team_is_local((uint8_t)team_id))
                continue; /* a board only bids for the teams it hosts -
                              see cbba_team_is_local() */

            cbba_build_bundle((uint8_t)team_id);
        }

        cbba_consensus();
    }

    /* One real mesh round-trip per tick's bidding cycle, not per
       internal convergence round above: CBBA_ROUND_TIMEOUT_MS is a
       per-tick budget (see cbba.h), and NUM_TEAMS * CBBA_MAX_BUNDLE_SIZE
       rounds of waiting would be nowhere near real-time. Broadcast
       this board's now-converged local belief, then wait up to
       CBBA_ROUND_TIMEOUT_MS for the other board's reply before
       finalizing with whatever's been heard. */
    cbba_broadcast_local_teams();
    cbba_ingest_remote_mesh_replies();

    cbba_improve_routes();
    cbba_sync_mission_execution_twin();

    path_cache_end();
}
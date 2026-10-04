#include "cbba.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "mission_state.h"
#include "mission_state_utils.h"
#include "incident.h"
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





/*
 * Send-on-change cache for cbba_broadcast_local_teams() (definition
 * further down this file, where the send loop lives). Only winner[]/
 * winning_bid[] are cached - the fields that carry a local team's real
 * consensus claim - NOT timestamp[], which cbba_receive_and_update()
 * bumps on every received message regardless of whether this team's
 * own winner/bid changed (receiver->timestamp[sender_id] =
 * g_current_tick unconditionally). Caching timestamp[] too would make
 * "did anything change" true almost every round even when nothing
 * this team believes actually changed, defeating the point of this
 * cache.
 *
 * CBBA_FORCE_RESEND_TICKS bounds staleness for a peer that missed an
 * earlier change (a dropped frame, or a peer that rebooted mid-
 * session and lost what it had heard) - see core/lora_frame.c's
 * "drop and resync" policy for why frames can legitimately vanish.
 * Without a periodic unconditional resend, "send only on change"
 * could leave a desynced peer stuck forever once nothing new happens
 * to trigger a fresh send.
 */
#define CBBA_FORCE_RESEND_TICKS 4u

typedef struct
{
    uint8_t winner[NUM_INCIDENTS];
    float winning_bid[NUM_INCIDENTS];
    uint32_t ticks_since_send;
    bool ever_sent;
} CBBALastBroadcast_t;

static CBBALastBroadcast_t s_last_broadcast[NUM_TEAMS];

/* Monotonic count of frames cbba_broadcast_local_teams() actually handed to
   comms_transport_send() (per local team, per destination board) - i.e. how
   many times the send-on-change gate above let a send through, as opposed to
   skipping it because nothing changed. Test-observable proxy for "did this
   round's traffic reduction actually happen" - see cbba.h. */
static uint32_t s_cbba_broadcast_send_count;

uint32_t cbba_broadcast_send_count(void)
{
    return s_cbba_broadcast_send_count;
}

static void cbba_reset_broadcast_change_cache(void)
{
    memset(s_last_broadcast, 0, sizeof(s_last_broadcast));
    s_cbba_broadcast_send_count = 0u;
}

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

/*
 * Compact ON-WIRE encoding of CBBAMessage_t (defined further down this
 * file), used ONLY at the comms_transport_send()/poll_received()
 * boundary in cbba_broadcast_local_teams()/cbba_ingest_remote_mesh_
 * replies() - the real board-to-board mesh path. cbba_consensus()'s
 * in-process gossip (used by the laptop/desktop build and most
 * selftests) keeps working directly on full-precision CBBAMessage_t
 * and is completely unaffected by this encoding - only real boards pay
 * for the smaller wire size, and only real boards pay the quantization
 * cost below.
 *
 * winner[]/timestamp[] are unchanged (already 1 and 4 bytes/entry -
 * timestamp[] deliberately NOT compacted further here: cbba_receive_
 * and_update()'s third-party-relay tie-break (Choi/Brunet/How Table I)
 * depends on absolute ordering of these values across the whole mesh,
 * and a narrower wire width would risk silent wraparound/ordering bugs
 * for a long-running mission - a correctness risk judged not worth the
 * marginal extra bytes saved). winning_bid[] moves from a 4-byte float
 * to a 2-byte fixed-point winning_bid_q[] (values*1000, ~0.001
 * resolution) - see cbba_pack_bid()/cbba_unpack_bid() below for the
 * range/precision reasoning and the override-pin sentinel.
 *
 * Keyed by incident_id, NOT by the sender's g_incidents[] slot index.
 * Each board fills its own g_incidents[] independently (mission_link_
 * apply_incident() takes the first free slot), and the two boards'
 * slot layouts diverge in practice: the LoRa relay of incident frames
 * is lossy, a resolution frees a slot on one board before the other,
 * and frames can arrive in different orders. The original layout
 * (winner[NUM_INCIDENTS]/winning_bid_q[NUM_INCIDENTS] indexed by the
 * SENDER's slot) therefore let cbba_receive_and_update() merge a
 * winner/bid for one incident into a completely different incident on
 * the receiver. Now every claim carries the incident it is about, and
 * the receiver maps it onto its OWN slot (cbba_find_incident_slot_by_
 * key()), skipping incidents it doesn't hold at all.
 *
 * Sparse, not dense: only entries whose winner != NO_ASSIGNMENT go on
 * the wire. That is lossless - cbba_receive_and_update() already treats
 * a z_k == NO_ASSIGNMENT entry as a no-op ("continue"), so an entry the
 * receiver never sees behaves exactly like one it saw as unassigned.
 *
 * Layout (packed, native little-endian - both boards are Cortex-M, same
 * convention as every other struct this project memcpy()s onto the
 * wire):
 *
 *   CBBAWireHeader_t                                   42 B
 *     sender_id        u8     team whose belief this is
 *     entry_count      u8     number of CBBAWireEntry_t that follow
 *     timestamp[10]    u32    same meaning/width as before (see above)
 *   CBBAWireEntry_t x entry_count (0..NUM_INCIDENTS)   5 B each
 *     incident_key     u16    low 16 bits of incident_id
 *     winner           u8     team id
 *     winning_bid_q    u16    cbba_pack_bid() fixed-point
 *
 * Worst case 42 + 20 x 5 = 142 B, so a whole snapshot is always ONE
 * frame (_Static_assert below). Why a 16-bit key rather than the full
 * 32-bit incident_id: 20 x (4 + 1 + 2) + 42 = 182 B does not fit
 * LORA_FRAME_MAX_PAYLOAD (144), and splitting a snapshot across frames
 * does not survive the transport - comms_transport_tx_enqueue()
 * deliberately overwrites a still-queued CBBA frame with the same
 * team's newer one (a CBBA frame is normally that team's whole current
 * belief), so the second fragment would replace the first before it
 * was ever sent. It would also double the airtime of a full burst
 * (see CBBA_ROUND_TIMEOUT_MS in cbba.h).
 *
 * The truncation is safe by construction, not just by probability:
 * incident_id is a monotonic per-mission counter (allocate_incident_
 * id()), so two incidents only share a key if they are 65536 ids
 * apart, and the receiver only accepts a key that matches exactly ONE
 * incident it currently holds - an ambiguous key is skipped (treated
 * as NO_ASSIGNMENT, the same no-op as an incident it doesn't hold),
 * never merged into the wrong slot. A sender likewise skips a claim
 * whose key is shared by another incident in its own table.
 */
typedef struct __attribute__((packed))
{
    uint8_t sender_id;
    uint8_t entry_count;
    uint32_t timestamp[NUM_TEAMS];
} CBBAWireHeader_t;

typedef struct __attribute__((packed))
{
    uint16_t incident_key;
    uint8_t winner;
    uint16_t winning_bid_q;
} CBBAWireEntry_t;

#define CBBA_WIRE_INCIDENT_KEY(incident_id) ((uint16_t)((incident_id) & 0xFFFFu))
#define CBBA_WIRE_MAX_FRAME_SIZE \
    (sizeof(CBBAWireHeader_t) + NUM_INCIDENTS * sizeof(CBBAWireEntry_t))

_Static_assert(sizeof(CBBAWireHeader_t) == 2u + 4u * NUM_TEAMS, "CBBAWireHeader_t layout drifted");
_Static_assert(sizeof(CBBAWireEntry_t) == 5u, "CBBAWireEntry_t layout drifted");
_Static_assert(CBBA_WIRE_MAX_FRAME_SIZE <= LORA_FRAME_MAX_PAYLOAD,
               "worst-case CBBA wire frame (every incident assigned) must fit one LoRa frame");
_Static_assert(NUM_INCIDENTS <= 0xFF, "entry_count is a u8");

/* uint16 fixed-point scale for winning_bid_q[] above: value*1000, so
   0..65.534 is representable at ~0.001 resolution. calculate_score()/
   calculate_route_utility() (core/scoring.c) build every REAL bid from
   weighted 0..1 component factors (g_default_scoring_weights sums to
   exactly 1.0) accumulated over at most CBBA_MAX_BUNDLE_SIZE tasks, so
   a real bid comfortably fits well under 65 long before this range is
   a practical concern - the clamp below is a safety net against a
   pathological bundle, not something normal operation is expected to
   hit. 0xFFFF is reserved as a sentinel for CBBA_OVERRIDE_PIN_BID
   (1e9f, defined just above), which would otherwise overflow this
   encoding entirely; CBBA_WIRE_BID_MAX_RAW leaves that one value free
   for it. */
#define CBBA_WIRE_BID_SCALE      1000.0f
#define CBBA_WIRE_BID_OVERRIDE_SENTINEL 0xFFFFu
#define CBBA_WIRE_BID_MAX_RAW    0xFFFEu

static uint16_t cbba_pack_bid(float bid)
{
    float scaled;

    if(bid >= CBBA_OVERRIDE_PIN_BID)
        return CBBA_WIRE_BID_OVERRIDE_SENTINEL;

    if(bid < 0.0f)
        bid = 0.0f;

    scaled = bid * CBBA_WIRE_BID_SCALE + 0.5f; /* round to nearest */

    if(scaled > (float)CBBA_WIRE_BID_MAX_RAW)
        return CBBA_WIRE_BID_MAX_RAW;

    return (uint16_t)scaled;
}

static float cbba_unpack_bid(uint16_t raw)
{
    if(raw == CBBA_WIRE_BID_OVERRIDE_SENTINEL)
        return CBBA_OVERRIDE_PIN_BID;

    return (float)raw / CBBA_WIRE_BID_SCALE;
}

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

/* Wire-side counterpart of cbba_find_incident_slot_by_id(): the local
   slot of the ONE occupied incident whose CBBA_WIRE_INCIDENT_KEY()
   matches, or NO_ASSIGNMENT when none - or more than one - does. See
   CBBAWireHeader_t's comment for why an ambiguous key must resolve to
   "not held" rather than to either candidate. */
static uint8_t cbba_find_incident_slot_by_key(uint16_t incident_key)
{
    uint8_t found = NO_ASSIGNMENT;

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        if(g_incidents[i].slot_occupied &&
           CBBA_WIRE_INCIDENT_KEY(g_incidents[i].incident_id) == incident_key)
        {
            if(found != NO_ASSIGNMENT)
                return NO_ASSIGNMENT;
            found = i;
        }
    }

    return found;
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

        /* Steps, not path COST: a team moves one cell per tick whatever the
         * cell costs (1-25), and stamina drains per step moved (team.c). The
         * twin's own countdown (mission_execution_twin.c) already works in
         * steps, so this must too or the two disagree from the first tick.
         * Nominal on purpose - a mobility fault's skipped ticks are what the
         * watchdog is meant to notice against it. */
        uint16_t travel_steps = find_path_steps(
            team->loc_x,
            team->loc_y,
            twin->destination_x,
            twin->destination_y);

        if(travel_steps != UINT16_MAX)
        {
            float expected_stamina = team->stamina - ((float)travel_steps * team_travel_stamina_cost(team));

            if(expected_stamina < MIN_STAMINA)
                expected_stamina = MIN_STAMINA;

            if(expected_stamina > MAX_STAMINA)
                expected_stamina = MAX_STAMINA;

            twin->expected_stamina = expected_stamina;
            twin->expected_arrival_tick = g_current_tick + travel_steps;
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

    if(!incident->slot_occupied || incident->status != INC_OPEN ||
       incident->unreachable_streak >= INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD)
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
    int max_iter = NUM_TEAMS * CBBA_MAX_BUNDLE_SIZE * 4;

    while(improved && max_iter-- > 0)
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

        /* Start at 0 unless path[0] is a pinned override — pinned tasks
           must stay as the team's immediate next stop (see cbba_apply_
           override_pin()'s own comment on why path[0] is the pin slot).
           Non-pinned tasks at path[0] are valid relocation candidates. */
        uint8_t donor_start = (donor->override_active &&
                               donor->path_size > 0 &&
                               donor->path[0].pool == TASK_POOL_INCIDENT &&
                               donor->path[0].slot == cbba_find_incident_slot_by_id(
                                   donor->override_incident_id)) ? 1u : 0u;

        for(uint8_t donor_index = donor_start; donor_index < donor->path_size; donor_index++)
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

    int max_relocate = NUM_TEAMS * CBBA_MAX_BUNDLE_SIZE * 2;
    while(max_relocate-- > 0 && cbba_try_best_relocate())
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

    cbba_reset_broadcast_change_cache();
}


/*
 * Force-injects team_id's active override pin (if any) into its own
 * bundle/path, ahead of the normal marginal-bid competition loop
 * below, and stamps a CBBA_OVERRIDE_PIN_BID winning_bid no other
 * team's normal bid can beat once it propagates through consensus
 * (see the #define's own comment above).
 *
 * NOTE: a pin onto a system task (g_system_tasks - sector sweeps,
 * equipment runs, etc., not g_incidents) is not supported by either
 * override path (board USART1 or the laptop's own --live stdin
 * channel): cbba_find_incident_slot_by_id() below only ever searches
 * g_incidents, so such a pin would silently self-clear (case 1 below)
 * on the very next tick. The laptop-only sim path (laptop/debug.c's
 * debug_live_override_poll()) rejects this up front with reason
 * "system_task" instead of letting it silently fail; the board path
 * has no equivalent check and is left as-is - not fixed here, since
 * this function's own search scope is shared, working infrastructure
 * this note is flagging, not changing.
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

    /* Bug fix: this used to guard both writes below (bundle[bundle_size]
     * AND the path[] shift-insert) with a single check on path_size,
     * relying on an implicit, undocumented invariant - bundle_size <=
     * path_size - to make checking path_size alone also safe for the
     * bundle[] write. That invariant does hold today, but only because
     * nothing currently inserts a TASK_POOL_SYSTEM entry into
     * agent->path[]: system tasks are assigned straight to
     * team->assigned_task_pool/slot (core/allocator.c's allocate_all_
     * incidents()), entirely bypassing bundle[]/path[], even though
     * cbba_release_lost_tasks() (below) already has code anticipating a
     * future where path[] holds system-pool entries bundle[] never
     * does ("System tasks are never in the bundle - always keep
     * them") - currently dead code, since nothing produces that case
     * yet. Guard each counter against its own array's capacity
     * independently instead, so this stops depending on that invariant
     * at all - whoever eventually wires system tasks into path[] won't
     * silently reopen an overflow here. (cbba_build_bundle()'s own
     * bundle[]/path[] writes are each already independently guarded
     * this way; only this function shared one guard across both.) Note
     * this doesn't change the separate, already-known "orphaned pin"
     * functional issue where a task can be credited as won in bundle[]
     * without ever landing in path[] - that's a different fix (needs a
     * decision on what should happen when path fills up, not just a
     * bounds check) and is intentionally left alone here. */
    if(agent->bundle_size >= CBBA_COMMITMENT_LIMIT ||
       agent->path_size >= CBBA_MAX_BUNDLE_SIZE ||
       cbba_bundle_contains(team_id, task))
        return;

    agent->bundle[agent->bundle_size] = task;
    agent->bundle_bid[agent->bundle_size] = CBBA_OVERRIDE_PIN_BID;
    agent->bundle_size++;

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
        float best_bid = 0.0f;
        bool found_task=false;
        uint8_t selected_insert_position = 0;
        for(int i=0;i<NUM_INCIDENTS;i++)
        {
            if(g_incidents[i].slot_occupied==0) continue;
            if(g_incidents[i].status!=INC_OPEN) continue;
            if(g_incidents[i].unreachable_streak>=INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD) continue;
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
            /* calculate_marginal_bundle_bid() returns -1.0f as its
               "infeasible/unreachable" sentinel - never claim on it. */
            if(bid < 0.0f) continue;
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

        /* Guard path array using path_size, not bundle_size */
        if(agent->path_size < CBBA_MAX_BUNDLE_SIZE)
        {
            for(uint8_t j = agent->path_size;
                j > selected_insert_position;
                j--)
            {
                agent->path[j] = agent->path[j - 1];
            }
            agent->path[selected_insert_position] = best_task;
            agent->path_size++;
        }

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

                /* System tasks are never in the bundle — always keep them */
                if(path_task.pool == TASK_POOL_SYSTEM)
                {
                    agent->path[new_path_size] = path_task;
                    new_path_size++;
                    continue;
                }

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
        if(g_incidents[j].unreachable_streak >= INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD) continue;

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
   doc comment. core/cbba.c itself no longer calls it: the reply wait
   below is bounded by a count of wait steps, not by elapsed time, so a
   simulation result can never depend on how fast a CPU or a kernel
   timer happens to run. It stays as the shared elapsed-time source for
   the board's link-side pacing (heartbeat interval, handshake
   timeouts), which are real-time by nature and never feed sim state.
   Advances a simulated millisecond counter by a fixed step every call
   so those callers still make progress on a host with no clock. */
#ifndef CBBA_SIM_CLOCK_STEP_MS
#define CBBA_SIM_CLOCK_STEP_MS 10u
#endif

__attribute__((weak)) uint32_t cbba_now_ms(void)
{
    static uint32_t s_sim_clock_ms = 0u;
    s_sim_clock_ms += CBBA_SIM_CLOCK_STEP_MS;
    return s_sim_clock_ms;
}

/* Wait-step budget of the reply wait. A runtime value (default
   CBBA_REPLY_WAIT_STEPS) so a platform whose step takes longer than the
   nominal CBBA_REPLY_WAIT_STEP_MS can shrink the count and keep the same
   real-time bound, without a build flag that has to agree across the
   Makefile and the CubeIDE project. Set once at boot, before cbba_run(). */
static uint32_t s_reply_wait_steps = CBBA_REPLY_WAIT_STEPS;

void cbba_set_reply_wait_steps(uint32_t steps)
{
    s_reply_wait_steps = (steps == 0u) ? 1u : steps;
}

/* Weak default: nothing to service - see cbba.h. */
__attribute__((weak)) void cbba_wait_poll(void)
{
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
    /* The transport's own board split, not a re-derived team_id/5: the
       laptop build puts every team on board 0 (COMMS_TRANSPORT_TEAMS_PER_
       BOARD=NUM_TEAMS), and a hardcoded 2 boards made it "send" to board 1
       - team 5, which that build maps back to board 0 - so every tick
       looped 10 frames through pack/seal/open into its own inbox, which
       nothing drained (found by review: 16/16 slots full after 50 ticks). */
    uint8_t num_boards = comms_transport_board_count();

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        CBBAMessage_t msg;

        if(!cbba_team_is_local(team_id))
            continue;

        cbba_broadcast(team_id, &msg);

        {
            /* Send-on-change gate: skip the mesh broadcast entirely
               when this team's winner[]/winning_bid[] belief matches
               what was last actually put on the wire for it, unless
               CBBA_FORCE_RESEND_TICKS worth of ticks have passed since
               the last send (see CBBALastBroadcast_t's comment above
               for why timestamp[] is deliberately excluded from this
               comparison, and why the force-resend floor exists). */
            CBBALastBroadcast_t *last = &s_last_broadcast[team_id];
            bool changed = !last->ever_sent;
            const float CBBA_BROADCAST_BID_EPSILON = 0.0001f;

            if(!changed)
            {
                for(int j = 0; j < NUM_INCIDENTS; j++)
                {
                    float diff = last->winning_bid[j] - msg.winning_bid[j];

                    if(diff < 0.0f)
                        diff = -diff;

                    if(last->winner[j] != msg.winner[j] ||
                       diff > CBBA_BROADCAST_BID_EPSILON)
                    {
                        changed = true;
                        break;
                    }
                }
            }

            if(!changed && last->ticks_since_send < CBBA_FORCE_RESEND_TICKS)
            {
                last->ticks_since_send++;
                continue; /* nothing new to tell the other board(s) yet */
            }

            for(int j = 0; j < NUM_INCIDENTS; j++)
            {
                last->winner[j] = msg.winner[j];
                last->winning_bid[j] = msg.winning_bid[j];
            }
            last->ticks_since_send = 0u;
            last->ever_sent = true;
            s_cbba_broadcast_send_count++;
        }

        {
            /* Pack once per team, reused for every destination board -
               see CBBAWireHeader_t's own comment above for the layout,
               why it is keyed by incident rather than slot, and why
               only assigned entries are sent. */
            uint8_t wire[CBBA_WIRE_MAX_FRAME_SIZE];
            CBBAWireHeader_t header;
            uint8_t wire_len;

            header.sender_id = msg.sender_id;
            header.entry_count = 0u;
            for(int k = 0; k < NUM_TEAMS; k++)
                header.timestamp[k] = msg.timestamp[k];

            for(uint8_t j = 0; j < NUM_INCIDENTS; j++)
            {
                CBBAWireEntry_t entry;

                if(msg.winner[j] == NO_ASSIGNMENT || !g_incidents[j].slot_occupied)
                    continue; /* nothing to claim, or no incident to key it by */

                entry.incident_key = CBBA_WIRE_INCIDENT_KEY(g_incidents[j].incident_id);
                if(cbba_find_incident_slot_by_key(entry.incident_key) != j)
                    continue; /* key shared by two held incidents - see
                                  CBBAWireHeader_t's comment */

                entry.winner = msg.winner[j];
                entry.winning_bid_q = cbba_pack_bid(msg.winning_bid[j]);
                memcpy(wire + sizeof(header) + (size_t)header.entry_count * sizeof(entry),
                       &entry, sizeof(entry));
                header.entry_count++;
            }

            memcpy(wire, &header, sizeof(header));
            wire_len = (uint8_t)(sizeof(header) + (size_t)header.entry_count * sizeof(CBBAWireEntry_t));

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
                representative_team_id = comms_transport_representative_team_for_board(board);

                (void)comms_transport_send(team_id, representative_team_id,
                                            LORA_FRAME_TYPE_CBBA,
                                            COMMS_PRECEDENCE_PRIORITY,
                                            wire, wire_len);
            }
        }
    }
}

/* Monotonic counts of what cbba_decode_wire_message() threw away - see
   cbba.h. Not reset by cbba_init(), which cbba_run() calls every tick. */
static uint32_t s_cbba_wire_reject_count;
static uint32_t s_cbba_wire_entry_reject_count;

uint32_t cbba_wire_reject_count(void)
{
    return s_cbba_wire_reject_count;
}

uint32_t cbba_wire_entry_reject_count(void)
{
    return s_cbba_wire_entry_reject_count;
}

/* Decodes one CBBA wire frame (see CBBAWireHeader_t) into a
   full-precision CBBAMessage_t indexed by THIS board's own slots - the
   shape cbba_receive_and_update() (shared with cbba_consensus()'s
   in-process gossip) works on. Every slot starts as NO_ASSIGNMENT, so an
   incident the sender didn't claim, or one this board doesn't hold,
   stays a no-op for that function. Returns 0 on success, -1 when the
   frame is malformed and must be dropped whole.

   The wire is untrusted (review finding H5): the LoRa PSK is a public
   default in this repo, and a corrupt frame can in principle survive
   the transport. Everything a field is later used to INDEX is checked
   here, before cbba_receive_and_update() sees it:
     - sender_id indexes receiver->timestamp[] as a WRITE
       (timestamp[sender_id] = g_current_tick), so a frame whose
       sender_id is >= NUM_TEAMS is rejected whole. So is one claiming
       to come from a team hosted on THIS board: a real peer never
       sends that, and accepting it would let a forged frame speak for
       our own teams in the consensus table.
     - winner values index timestamp[] and g_teams[] (through
       candidate_claim_is_better()), so an entry whose winner is neither
       a team id nor NO_ASSIGNMENT is dropped; the rest of the frame is
       still usable. */
static int cbba_decode_wire_message(const uint8_t *payload, uint8_t payload_len,
                                    CBBAMessage_t *out)
{
    CBBAWireHeader_t header;

    if(payload_len < sizeof(CBBAWireHeader_t))
    {
        s_cbba_wire_reject_count++;
        return -1;
    }

    memcpy(&header, payload, sizeof(header));

    if(header.entry_count > NUM_INCIDENTS ||
       payload_len != sizeof(CBBAWireHeader_t) +
                      (size_t)header.entry_count * sizeof(CBBAWireEntry_t) ||
       header.sender_id >= NUM_TEAMS ||
       cbba_team_is_local(header.sender_id))
    {
        s_cbba_wire_reject_count++;
        return -1; /* protocol drift, a corrupt frame, or a forged sender */
    }

    out->sender_id = header.sender_id;
    for(int j = 0; j < NUM_INCIDENTS; j++)
    {
        out->winner[j] = NO_ASSIGNMENT;
        out->winning_bid[j] = 0.0f;
    }
    for(int k = 0; k < NUM_TEAMS; k++)
        out->timestamp[k] = header.timestamp[k];

    for(uint8_t e = 0u; e < header.entry_count; e++)
    {
        CBBAWireEntry_t entry;
        uint8_t local_slot;

        memcpy(&entry, payload + sizeof(CBBAWireHeader_t) + (size_t)e * sizeof(CBBAWireEntry_t),
               sizeof(entry));

        if(entry.winner >= NUM_TEAMS && entry.winner != NO_ASSIGNMENT)
        {
            s_cbba_wire_entry_reject_count++;
            continue;
        }

        /* Map the sender's incident onto THIS board's own slot; an
           incident this board doesn't hold has nothing to merge into. */
        local_slot = cbba_find_incident_slot_by_key(entry.incident_key);
        if(local_slot == NO_ASSIGNMENT)
            continue;

        out->winner[local_slot] = entry.winner;
        out->winning_bid[local_slot] = cbba_unpack_bid(entry.winning_bid_q);
    }

    return 0;
}

/* Drains this board's comms_transport inbox for CBBA frames and folds
   each one into every locally-hosted team's belief via
   cbba_receive_and_update() - the same function cbba_consensus()'s
   in-process gossip already uses, just fed from a real (or, until a
   real multi-board harness exists, locally-stubbed - see
   comms_transport_send()/comms_transport_poll_received(), both real,
   no stub needed) wire message instead of an in-memory outbox[]
   entry.

   Actively waits up to CBBA_REPLY_WAIT_STEPS wait steps (nominally
   CBBA_ROUND_TIMEOUT_MS of platform time) for something to arrive:
   comms_transport_poll_received() is itself non-blocking (returns 0
   immediately on an empty inbox), so without an explicit wait this
   function would finalize optimistically with zero wait - exactly
   what CBBA_ROUND_TIMEOUT_MS exists to rule out (see cbba.h). It is
   equally deliberate that this does NOT wait past the timeout even
   if nothing has arrived yet: a real board still has to move its
   teams every tick whether or not the other board replied in time,
   so this finalizes with whatever's been ingested so far rather than
   blocking indefinitely.

   Bug fix (docs/reports/AUDIT.md item #3): this used to busy-wait the FULL
   CBBA_ROUND_TIMEOUT_MS every single call regardless of whether every
   expected reply had already arrived, because cbba_run() (and
   therefore this function) runs every tick via allocate_all_
   incidents() - not just when something is actually outstanding. On
   the single-task board model (see comms_transport_inbox_push()'s
   concurrency-invariant comment) that meant every tick could burn up
   to 530ms of pure CPU spin ahead of simulation_step() and this
   board's own RX/TX/heartbeat servicing right after. cbba_broadcast_
   local_teams() sends one CBBAMessage_t per LOCALLY-hosted team to
   every OTHER board - so the number of distinct replies THIS board
   should expect this round is exactly the number of non-local teams,
   not comms_transport_board_count()-1 (that would be the wrong unit:
   a board can host several teams, each broadcasting separately, and
   board_count() also reflects configured boards, not which ones are
   actually live). Tallying ingested messages against that expected
   total and exiting as soon as it's met preserves the timeout as a
   fallback for a board that never replies (the tally simply never
   reaches its target that round, same behavior as before this fix)
   while eliminating the wait entirely on every round where all boards
   already responded. */
/* Not static - see cbba_team_is_local()'s comment above for why. */
void cbba_ingest_remote_mesh_replies(void)
{
    uint32_t wait_steps = 0u;
    uint8_t expected_replies = 0;
    uint8_t ingested_replies = 0;
#ifdef CBBA_DEBUG_TIMING
    bool reply_ingested_this_round = false;
#endif

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        if(!cbba_team_is_local(team_id))
            expected_replies++;
    }

    /* Bounded by wait STEPS (CBBA_REPLY_WAIT_STEPS), not by a clock read:
       one step is one idle cbba_wait_poll() call, and the platform decides
       how long a step takes (cbba.h). Draining frames that are already in
       the inbox costs no step, so a burst of replies is never cut short
       by the budget; that drain is bounded by the inbox size. */
    while(ingested_replies < expected_replies &&
          wait_steps < s_reply_wait_steps)
    {
        uint8_t sender_team_id;
        uint8_t frame_type;
        uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t payload_len;
        CBBAMessage_t msg;

        /* Takes only CBBA frames, from anywhere in the inbox. */
        if(!comms_transport_poll_received_types(COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_CBBA),
                                                 &sender_team_id, &frame_type,
                                                 payload, &payload_len))
        {
            /* No CBBA frame yet. Other consumers' frames may be waiting;
               they are left in place (poll_received_types() skips them)
               and the wait goes on - it used to give up the moment any
               non-CBBA frame (a status heartbeat, a relay) was queued,
               ending the round before the peer's reply could arrive.
               Only a full inbox ends it early: no reply can be delivered
               into it. */
            if(comms_transport_inbox_free_slots() == 0u)
                break;

            cbba_wait_poll();
            wait_steps++;
            continue;
        }

        if(frame_type != LORA_FRAME_TYPE_CBBA)
            continue; /* mission-link's RX task (Prompt 3) owns those */

        (void)sender_team_id; /* header.sender_id (wire field) is
            authoritative - see comms_transport.h's "Open question" note:
            sender identity for a genuinely separate board isn't on the
            wire at the comms_transport layer today, only inside the CBBA
            payload itself */

        /* Unpacks back to a full-precision CBBAMessage_t keyed by THIS
           board's slots before cbba_receive_and_update() sees it - that
           function (shared with cbba_consensus()'s in-process gossip)
           always works on full float bids indexed by local slot, so both
           the quantization and the incident_id -> slot mapping are
           entirely contained to the wire hop. */
        if(cbba_decode_wire_message(payload, payload_len, &msg) != 0)
            continue; /* malformed - drop it and keep draining rather
                          than aborting the whole round over one bad
                          message */

        ingested_replies++;
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
                "(board=%u, wait_steps=%u, budget=%u)\n",
                (unsigned)g_comms_transport_this_board_id,
                (unsigned)wait_steps,
                (unsigned)s_reply_wait_steps);
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
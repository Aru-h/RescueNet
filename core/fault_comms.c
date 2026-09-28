#include "fault_comms.h"

#include <stdbool.h>
#include <string.h>

#include "comms_precedence.h"
#include "comms_transport.h"
#include "environment.h"
#include "fault_buffer.h"
#include "fault_config.h"
#include "lora_frame.h"
#include "mission_state.h"
#include "watchdog.h"

/* Not declared in cbba.h - see cbba_team_is_local()'s own comment in
   cbba.c ("public, not-in-the-header" is deliberate there) for why this
   is forward-declared at each call site instead of given a header of
   its own. Same convention laptop/snapshot.c already follows. */
extern bool cbba_team_is_local(uint8_t team_id);

/* Wire payload for LORA_FRAME_TYPE_FAULT_BROADCAST. Private to this
   file, same convention as CBBAMessage_t being private to cbba.c. */
typedef struct
{
    uint8_t team_id;
    float   evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES];
} FaultBroadcastPayload_t;

_Static_assert(sizeof(FaultBroadcastPayload_t) <= LORA_FRAME_MAX_PAYLOAD,
    "FaultBroadcastPayload_t must fit LORA_FRAME_MAX_PAYLOAD (lora_frame.h)");

/* Audit #163: the fault_buffer local ring must store the exact same
   evidence vector that goes out on the wire. fault_comms_broadcast_local_
   teams() assembles all FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES (12) into
   payload.evidence - 7 watchdog slots plus equipment_level,
   confirmed_mobility_impairment, incident_priority, victim_count and
   env_raw_travel_cost written directly - THEN passes the whole thing to fault_buffer_record_
   local_evidence(), and sends that same struct on the wire. These asserts
   pin the three widths so a future edit to any one of them can't silently
   re-open the 7-vs-11 mismatch. */
_Static_assert(sizeof(((FaultBroadcastPayload_t *)0)->evidence) ==
               sizeof(((FaultBufferLocalSample_t *)0)->evidence),
    "wire evidence[] and fault_buffer local-sample evidence[] must be the "
    "same width (audit #163)");
_Static_assert(FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 5u ==
               FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES,
    "wire feature count must be watchdog evidence slots + 5 extras "
    "(equipment_level, confirmed_mobility_impairment, incident_priority, "
    "victim_count, env_raw_travel_cost) - audit #163");
_Static_assert((unsigned)NUM_WATCHDOG_EVIDENCE_SLOTS ==
               FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS,
    "watchdog_get_evidence_vector() fills the first "
    "FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS of payload.evidence; if the watchdog "
    "grows a slot the two +2 offset writes below land in the wrong place");

/* Wire payload for LORA_FRAME_TYPE_FAULT_VOTE. One of these represents
   an entire board's worth of agreement (item 4's bandwidth
   optimization), not one team's - voter_team_id is therefore a
   representative id (this board's board*FAULT_BUFFER_TEAMS_PER_BOARD
   team), not literally "which team voted". weight carries how many of
   that board's locally-hosted teams agree (today: always all of them,
   since one model run currently stands in for all 5 - see
   fault_predictor_run()'s own doc). */
typedef struct
{
    uint8_t  voter_team_id;
    uint8_t  target_team_id;
    uint32_t class_mask;
    float    weight;
} FaultVotePayload_t;

_Static_assert(sizeof(FaultVotePayload_t) <= LORA_FRAME_MAX_PAYLOAD,
    "FaultVotePayload_t must fit LORA_FRAME_MAX_PAYLOAD (lora_frame.h)");

void fault_comms_init(void)
{
    uint8_t num_boards = comms_transport_board_count();
    uint8_t teams_per_board = 0u;

    /* comms_transport.c doesn't expose its per-board team count as a
       getter (COMMS_TRANSPORT_TEAMS_PER_BOARD is a #define private to
       that file), and adding one is out of scope ("don't touch
       comms_transport.c") - so derive it here by counting how many
       teams board 0 actually owns, using the public comms_transport_
       board_id_for_team() this file already calls elsewhere. Works
       correctly for any real value that macro takes, including the
       laptop build's -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS
       override (this counts all NUM_TEAMS as board 0's). */
    for(uint8_t team_id = 0u; team_id < NUM_TEAMS; team_id++)
    {
        if(comms_transport_board_id_for_team(team_id) == 0u)
            teams_per_board++;
    }

    fault_buffer_init(num_boards, teams_per_board);
}

/*
 * fault_comms_cast_vote() (Item 4/Prompt 1 + item 3/Prompt 1b) used to
 * live here: it ran fault_predictor_run() against a just-built or just-
 * received FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES-wide (12-float)
 * evidence vector and cast a vote from the result. Removed - not
 * padded - because fault_predictor_run()'s signature is now the 74-
 * input feature-contract vector (core/feature_spec.h,
 * FEATURE_SPEC_NUM_INPUTS; see core/fault_predictor.h's own doc), and
 * this file's 12-feature vector was never updated to match. It was
 * already dead in practice before that mismatch even mattered -
 * fault_comms_broadcast_local_teams()/fault_comms_ingest_remote_
 * frames() below are themselves not wired into simulation_step() yet
 * (see this file's header doc), so nothing ever called this. GCC's
 * array-bounds analysis (not clang's, which is why this went
 * unnoticed until built with gcc) correctly flagged passing a 12-float
 * array where the callee's prototype now promises 74. Padding the
 * buffer would have kept shipping a vote-casting path built on a
 * feature vector nothing else in the codebase produces or trains
 * against; deleting it leaves fault_buffer's local/remote evidence and
 * vote *recording* (fault_buffer_record_local_evidence/_vote,
 * fault_buffer_record_remote_evidence/_vote below) intact - only the
 * model-scoring step is gone.
 */

/* Sender (item 2 local-evidence recording folded in here + item 3
   broadcast, plus Prompt 1b's same-board vote pass). Mirrors cbba_
   broadcast_local_teams() exactly: loop over locally-hosted teams,
   send to every other board via a representative team_id (comms_
   transport routes by board, not by the specific receiver_team_id -
   see comms_transport.h). */
void fault_comms_broadcast_local_teams(void)
{
    for(uint8_t team_id = 0u; team_id < NUM_TEAMS; team_id++)
    {
        FaultBroadcastPayload_t payload;
        uint8_t confirmed_mobility_impairment = 0u;

        if(!cbba_team_is_local(team_id))
            continue;

        /* Item 2: this is the one place that already loops "for
           locally-hosted teams, every tick" that this file owns, so
           the local-evidence-recording call lives here rather than
           reaching into watchdog.c/simulation.c for a second loop.
           watchdog_step() has already run for this tick by the time
           the caller gets here (see simulation.c's ordering), so this
           reads this tick's fresh evidence. */
        watchdog_get_evidence_vector(team_id, payload.evidence, &confirmed_mobility_impairment);

        payload.team_id = team_id;
        payload.evidence[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS] = (float)g_teams[team_id].equipment_level / 100.0f;
        payload.evidence[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 1u] = (float)confirmed_mobility_impairment;

        /* Same incident lookup as recovery_manager.c's evaluate_fault_
           predictor() - kept as an independent copy here rather than a
           shared helper, per audit #163's requirement that this call
           site assembles its own evidence vector for the wire. */
        {
            const IncidentState_t *task = NULL;
            TeamState_t *team = &g_teams[team_id];

            if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
               team->assigned_task_slot < NUM_INCIDENTS)
                task = &g_incidents[team->assigned_task_slot];
            else if(team->assigned_task_pool == TASK_POOL_SYSTEM &&
                    team->assigned_task_slot < NUM_SYSTEM_TASKS)
                task = &g_system_tasks[team->assigned_task_slot];

            payload.evidence[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 2u] =
                (task != NULL) ? (float)task->priority : 0.0f;
            payload.evidence[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 3u] =
                (task != NULL) ? (float)task->victim_count : 0.0f;
        }

        payload.evidence[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 4u] =
            (float)environment_cell_travel_cost(g_teams[team_id].loc_x,
                                                g_teams[team_id].loc_y);

        /* Record after all 12 features are populated so the ring stores
           the full vector, not just the 7-slot watchdog subset. Same
           struct is sent on the wire below, so fault_buffer_get_local_
           sample() and the FAULT_BROADCAST frame carry byte-identical
           evidence for this team/tick (audit #163; the _Static_asserts
           near FaultBroadcastPayload_t pin the widths). */
        fault_buffer_record_local_evidence(team_id, payload.evidence, g_current_tick);

        for(uint8_t board = 0u; board < comms_transport_board_count(); board++)
        {
            uint8_t representative_team_id;

            if(board == g_comms_transport_this_board_id)
                continue;

            representative_team_id = (uint8_t)(board * FAULT_BUFFER_TEAMS_PER_BOARD);

            (void)comms_transport_send(team_id, representative_team_id,
                LORA_FRAME_TYPE_FAULT_BROADCAST, COMMS_PRECEDENCE_PRIORITY,
                (const uint8_t *)&payload, (uint8_t)sizeof(payload));
        }
    }
}

/* Receiver (item 5) + round boundary (item 6). Drains this board's
   comms_transport inbox once per call (not a CBBA-style busy-wait for
   CBBA_ROUND_TIMEOUT_MS - nothing in this prompt asked for that, and
   see this file's header doc on why layering a second independent
   busy-wait on the same shared inbox would make the draining hazard
   worse, not better). */
void fault_comms_ingest_remote_frames(void)
{
    fault_buffer_round_reset();

    for(;;)
    {
        uint8_t sender_team_id;
        uint8_t frame_type;
        uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t payload_len;
        /* Takes only fault frames, from anywhere in the inbox - other
           consumers' frames stay put (see comms_transport_poll_received_
           types() on why stopping at a foreign head frame hung boot). */
        if(!comms_transport_poll_received_types(COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_FAULT_BROADCAST) |
                                                 COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_FAULT_VOTE),
                                                 &sender_team_id, &frame_type, payload, &payload_len))
            break; /* no fault frames waiting - done for this tick */

        (void)sender_team_id; /* the payload's own team_id/voter_team_id
            fields are authoritative, same reasoning cbba_ingest_
            remote_mesh_replies() gives for ignoring this parameter */

        if(frame_type == LORA_FRAME_TYPE_FAULT_BROADCAST)
        {
            FaultBroadcastPayload_t broadcast;

            if(payload_len != (uint8_t)sizeof(broadcast))
                continue; /* protocol drift or a corrupt frame - drop it,
                              keep draining, same as CBBA's receiver */

            memcpy(&broadcast, payload, sizeof(broadcast));

            fault_buffer_record_remote_evidence(broadcast.team_id,
                comms_transport_board_id_for_team(broadcast.team_id),
                broadcast.evidence, g_current_tick);
        }
        else if(frame_type == LORA_FRAME_TYPE_FAULT_VOTE)
        {
            FaultVotePayload_t vote;

            if(payload_len != (uint8_t)sizeof(vote))
                continue;

            memcpy(&vote, payload, sizeof(vote));

            fault_buffer_record_remote_vote(vote.voter_team_id,
                comms_transport_board_id_for_team(vote.voter_team_id),
                vote.target_team_id, vote.class_mask, vote.weight,
                g_current_tick);
        }
        /* else: not ours - core/cbba.c's and mission_link.h's receivers
           own everything else, same "not mine, continue" convention
           they already use for frame types that aren't theirs. */
    }
}

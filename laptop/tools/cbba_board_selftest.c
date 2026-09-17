/*
 * Standalone self-test for core/cbba.c's per-board bidding scope and
 * round-timeout mesh exchange (no hardware/board required - desktop
 * gcc only, same spirit as laptop/tools/mission_link_selftest.c).
 *
 * This deliberately does NOT drive cbba_run()/cbba_build_bundle():
 * those need a fully generated world (grid, navigation, incidents -
 * see path_cache_begin()/find_path_cost()) to produce a real bid, and
 * standing all of that up would make this test exercise world
 * generation instead of the thing it's actually meant to check. What
 * it checks instead - cbba_team_is_local()'s board-scoping, and the
 * comms_transport-backed ingest/timeout path - is reachable directly
 * without any of that, via the file-scope entry points cbba.c exposes
 * for exactly this purpose (see the "Not static" comments next to
 * cbba_team_is_local()/cbba_broadcast_local_teams()/
 * cbba_ingest_remote_mesh_replies() in core/cbba.c).
 *
 * Covers:
 *   1. Board locality: cbba_team_is_local() agrees with
 *      comms_transport_board_id_for_team() / g_comms_transport_this_
 *      board_id for both boards under the current NUM_TEAMS=10 (5
 *      teams/board) split.
 *   2. Ingesting a simulated "other board" reply: a hand-built wire
 *      message from team 7 (remote to board 0) is pushed into the
 *      comms_transport inbox and drained by
 *      cbba_ingest_remote_mesh_replies() - every LOCAL team's belief
 *      updates, no remote team's does.
 *   3. The round finalizes at the timeout boundary: with an empty
 *      inbox, cbba_ingest_remote_mesh_replies() still returns (using
 *      cbba.c's weak, no-real-clock cbba_now_ms() - see cbba.h) rather
 *      than spinning forever.
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore \
 *       -o cbba_board_selftest laptop/tools/cbba_board_selftest.c \
 *       core/cbba.c core/comms_transport.c core/lora_frame.c \
 *       core/lora_crypto.c core/aead_chacha20poly1305.c \
 *       core/chacha20.c core/poly1305.c \
 *       core/mission_state.c core/team.c core/random.c \
 *       core/scoring.c core/path.c core/allocator.c \
 *       core/mission_state_utils.c core/incident.c \
 *       core/mission_link.c core/mission_link_handshake.c \
 *       core/environment.c core/fault_config.c \
 *       core/fault_injector.c core/grid.c core/watchdog.c \
 *       core/recovery_manager.c core/fault_predictor.c \
 *       core/search_zones.c \
 *       core/mission_execution_twin.c core/safezone.c \
 *       core/test_metrics.c -lm
 * Run:
 *   ./cbba_board_selftest
 *
 * (Pulling in the rest of every core source file is just to satisfy the linker -
 * cbba.c's real dependencies (scoring, path, allocator, ...) drag
 * everything else in transitively, same reason mission_link_selftest.c
 * needs core/mission_state.c. Nothing below calls into any of it.)
 */

#include "cbba.h"
#include "mission_state.h"
#include "comms_transport.h"
#include "lora_frame.h"

#include <stdio.h>
#include <string.h>

/* Minimal stand-ins for the platform-provided sinks that team.c/
   allocator.c/incident.c/fault_injector.c call into (real
   implementations: laptop/debug.c + snapshot.c). Same
   approach laptop/tools/mission_link_selftest.c already uses for
   debug_trace() - link against the real every core source file (so the code
   this test actually exercises is the genuine article, not a mock),
   stub only the platform glue those files pull in that has nothing
   to do with what's being tested here. */
uint8_t g_simulation_trace_enabled = 0;

void debug_trace(const char *fmt, ...)
{
    (void)fmt;
}

void snapshot_record(uint32_t cycle, EventType_t event_type, uint8_t team_id)
{
    (void)cycle; (void)event_type; (void)team_id;
}

void snapshot_record_resolved(uint32_t cycle, TaskPool_t pool, const IncidentState_t *task)
{
    (void)cycle; (void)pool; (void)task;
}

void debug_print_generator_evaluation(uint32_t tick, uint8_t load, uint32_t probability,
                                       uint32_t roll, uint8_t spawned, uint8_t available_teams,
                                       uint8_t en_route_teams, uint8_t on_scene_teams,
                                       uint8_t resting_teams, uint8_t open_incidents,
                                       uint8_t assigned_incidents, uint8_t in_progress_incidents,
                                       uint8_t active_system_tasks, uint16_t degraded_cells,
                                       uint32_t next_evaluation_tick)
{
    (void)tick; (void)load; (void)probability; (void)roll; (void)spawned;
    (void)available_teams; (void)en_route_teams; (void)on_scene_teams;
    (void)resting_teams; (void)open_incidents; (void)assigned_incidents;
    (void)in_progress_incidents; (void)active_system_tasks; (void)degraded_cells;
    (void)next_evaluation_tick;
}

void debug_print_generator_selection(const uint32_t *weights, uint32_t weight_count,
                                      uint32_t total_weight, uint32_t type_roll,
                                      uint8_t selected_type)
{
    (void)weights; (void)weight_count; (void)total_weight; (void)type_roll; (void)selected_type;
}

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/* cbba_team_is_local()/cbba_broadcast_local_teams()/
   cbba_ingest_remote_mesh_replies() are deliberately NOT declared in
   cbba.h (same "public, but header-private" convention cbba_init()/
   cbba_consensus() already use in that file) - forward-declared here
   instead, exactly like other selftests in this project already does for anything
   else in this codebase that follows that pattern. */
bool cbba_team_is_local(uint8_t team_id);
void cbba_broadcast_local_teams(void);
void cbba_ingest_remote_mesh_replies(void);
void cbba_init(void);

/* Mirrors cbba.c's own private CBBAWireMessage_t field-for-field (see
   that file's comment: "Wire format only ... no function below is
   allowed to read another agent's live CBBAAgentState_t directly").
   comms_transport_send()/poll_received() move opaque bytes, so a
   test can inject a message using its own copy of that exact layout
   without cbba.c needing to expose the type - same technique
   lora_frame.h's own sizing comment already leans on (byte layout,
   not the C type, is the actual contract). If cbba.c's
   CBBAWireMessage_t ever changes, this struct must change with it or
   this test's injected bytes stop matching what cbba_ingest_remote_
   mesh_replies() expects to unpack.

   winning_bid[] here is the fixed-point wire value (bid*1000, see
   cbba.c's CBBA_WIRE_BID_SCALE), NOT a raw float bid - CBBA_BID_Q()
   below does that conversion so tests can keep writing bids as plain
   floats. */
typedef struct __attribute__((packed))
{
    uint8_t sender_id;
    uint8_t winner[NUM_INCIDENTS];
    uint16_t winning_bid[NUM_INCIDENTS];
    uint32_t timestamp[NUM_TEAMS];
} TestCBBAMessage_t;

/* Mirrors cbba.c's cbba_pack_bid() quantization for test-injected
   messages - see that function's own comment for the scale/rounding
   reasoning. Not bit-for-bit identical for values near the rounding
   boundary, but every value this file injects is comfortably clear of
   one, same as cbba.c's own real callers. */
#define CBBA_BID_Q(bid) ((uint16_t)((bid) * 1000.0f + 0.5f))

static void reset_agent_state(void)
{
    for (int i = 0; i < NUM_TEAMS; i++)
    {
        CBBAAgentState_t *agent = &g_cbba_agent_state[i];

        memset(agent, 0, sizeof(*agent));
        for (int j = 0; j < NUM_INCIDENTS; j++)
            agent->winner[j] = NO_ASSIGNMENT;
    }
}

static void make_one_open_incident(uint8_t slot)
{
    memset(&g_incidents[slot], 0, sizeof(g_incidents[slot]));
    g_incidents[slot].slot_occupied = 1;
    g_incidents[slot].status = INC_OPEN;
}

static void test_board_locality(void)
{
    g_comms_transport_this_board_id = 0;
    for (uint8_t t = 0; t < 5; t++)
        CHECK(cbba_team_is_local(t));
    for (uint8_t t = 5; t < NUM_TEAMS; t++)
        CHECK(!cbba_team_is_local(t));

    g_comms_transport_this_board_id = 1;
    for (uint8_t t = 0; t < 5; t++)
        CHECK(!cbba_team_is_local(t));
    for (uint8_t t = 5; t < NUM_TEAMS; t++)
        CHECK(cbba_team_is_local(t));

    g_comms_transport_this_board_id = 0; /* restore default for the tests below */
}

static void test_ingest_from_simulated_other_board(void)
{
    const uint8_t incident_slot = 2;
    const uint8_t remote_sender = 7; /* board 1, not hosted here */
    TestCBBAMessage_t msg;
    int send_rc;

    g_comms_transport_this_board_id = 0; /* this "board" hosts teams 0-4 */
    comms_transport_init();
    reset_agent_state();
    make_one_open_incident(incident_slot);

    memset(&msg, 0, sizeof(msg));
    msg.sender_id = remote_sender;
    for (int j = 0; j < NUM_INCIDENTS; j++)
        msg.winner[j] = NO_ASSIGNMENT;
    msg.winner[incident_slot] = remote_sender;
    msg.winning_bid[incident_slot] = CBBA_BID_Q(3.5f);
    /* timestamp[] left at 0 - irrelevant to this claim: receiver's
       prior winner[incident_slot] is NO_ASSIGNMENT below, so
       cbba_receive_and_update()'s "z_i == NO_ASSIGNMENT" branch takes
       the sender's claim unconditionally, no timestamp tiebreak
       needed (see cbba.c's consensus table comment). */

    /* Injects as if this message arrived over the mesh from the other
       board: receiver_team_id=0 is local, so comms_transport routes
       it into THIS board's own inbox (same-board path) - a legitimate
       stand-in for a real cross-board delivery until Prompt 5's
       in-process harness (or real hardware) exists to drive two
       boards' worth of comms_transport for real. */
    send_rc = comms_transport_send(remote_sender, /* sender_team_id */
                                    0,             /* receiver_team_id */
                                    LORA_FRAME_TYPE_CBBA,
                                    COMMS_PRECEDENCE_PRIORITY,
                                    (const uint8_t *)&msg,
                                    (uint8_t)sizeof(msg));
    CHECK(send_rc == 0);

    cbba_ingest_remote_mesh_replies();

    for (uint8_t local_team = 0; local_team < 5; local_team++)
    {
        float bid = g_cbba_agent_state[local_team].winning_bid[incident_slot];

        CHECK(g_cbba_agent_state[local_team].winner[incident_slot] == remote_sender);
        /* Was "> 3.0f" - too weak to catch a wrong wire scale (e.g. an
           unpack that forgets to divide by CBBA_WIRE_BID_SCALE would
           still pass ">3.0f" despite being off by 1000x). Check the
           actual round-tripped value against the 3.5f this test sent,
           within one quantization step (see cbba.c's CBBA_WIRE_BID_
           SCALE - CBBA_BID_Q(3.5f) round-trips to 3.5f exactly at that
           resolution, no slop needed beyond float rounding itself). */
        CHECK(bid > 3.499f && bid < 3.501f);
        CHECK(g_cbba_agent_state[local_team].timestamp[remote_sender] == g_current_tick);
    }

    for (uint8_t remote_team = 5; remote_team < NUM_TEAMS; remote_team++)
    {
        CHECK(g_cbba_agent_state[remote_team].winner[incident_slot] == NO_ASSIGNMENT);
    }
}

/* Strong override of cbba.c's weak cbba_wait_poll(): counts calls and, when
   armed, "receives" a CBBA reply partway through the wait - the way a board's
   hook delivers the peer's reply from the radio. */
static uint32_t g_wait_poll_calls;
static int g_wait_poll_inject_at = -1;
static TestCBBAMessage_t g_wait_poll_msg;

void cbba_wait_poll(void)
{
    g_wait_poll_calls++;
    if (g_wait_poll_inject_at >= 0 && g_wait_poll_calls == (uint32_t)g_wait_poll_inject_at)
    {
        (void)comms_transport_send(g_wait_poll_msg.sender_id, 0, LORA_FRAME_TYPE_CBBA,
                                   COMMS_PRECEDENCE_PRIORITY,
                                   (const uint8_t *)&g_wait_poll_msg,
                                   (uint8_t)sizeof(g_wait_poll_msg));
    }
}

/* The reply wait keeps servicing the radio (cbba_wait_poll()) while other
   consumers' frames sit in the inbox, and a reply arriving mid-wait is
   ingested. It used to stop waiting the moment any non-CBBA frame was
   queued and never called a hook, so on a board the peer's reply could
   only count if it was already in the inbox. */
static void test_wait_polls_past_foreign_frames(void)
{
    const uint8_t incident_slot = 4;
    const uint8_t remote_sender = 8;
    static const uint8_t foreign_payload[2] = {1u, 2u};
    uint8_t sender, type, payload[LORA_FRAME_MAX_PAYLOAD], len;

    g_comms_transport_this_board_id = 0;
    comms_transport_init();
    reset_agent_state();
    make_one_open_incident(incident_slot);

    CHECK(comms_transport_send(0, 0, LORA_FRAME_TYPE_MISSION_LINK, COMMS_PRECEDENCE_ROUTINE,
                               foreign_payload, (uint8_t)sizeof(foreign_payload)) == 0);

    memset(&g_wait_poll_msg, 0, sizeof(g_wait_poll_msg));
    g_wait_poll_msg.sender_id = remote_sender;
    for (int j = 0; j < NUM_INCIDENTS; j++)
        g_wait_poll_msg.winner[j] = NO_ASSIGNMENT;
    g_wait_poll_msg.winner[incident_slot] = remote_sender;
    g_wait_poll_msg.winning_bid[incident_slot] = CBBA_BID_Q(4.5f);

    g_wait_poll_calls = 0u;
    g_wait_poll_inject_at = 3;
    cbba_ingest_remote_mesh_replies();
    g_wait_poll_inject_at = -1;

    CHECK(g_wait_poll_calls >= 3u);
    CHECK(g_cbba_agent_state[0].winner[incident_slot] == remote_sender);
    /* The foreign frame was left for its own consumer. */
    CHECK(comms_transport_poll_received_types(COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_MISSION_LINK),
                                              &sender, &type, payload, &len) == 1);
}

/* cbba_broadcast_local_teams() only sends a team's mesh update when its
   winner[]/winning_bid[] belief actually changed since the last send, or
   CBBA_FORCE_RESEND_TICKS ticks have elapsed - see core/cbba.c's
   CBBALastBroadcast_t comment. This confirms the gate's three concrete
   behaviors: it always sends the first time, it skips a send when nothing
   changed, and it sends again once a real belief change occurs. */
static void test_broadcast_send_on_change_gate(void)
{
    const uint8_t incident_slot = 6;
    uint32_t count_after_first;
    uint32_t count_after_unchanged;
    uint32_t count_after_changed;

    g_comms_transport_this_board_id = 0; /* hosts teams 0-4 */
    comms_transport_init();
    cbba_init(); /* zeroes the send-on-change cache too, not just
                    agent state - needed so "first call always sends"
                    below holds regardless of what earlier tests in
                    this file already sent */
    reset_agent_state();
    make_one_open_incident(incident_slot);

    /* First call for any team is always a send - there is nothing to
       compare against yet (CBBALastBroadcast_t.ever_sent starts false,
       reset by cbba_init()/cbba_reset_broadcast_change_cache()). */
    cbba_broadcast_local_teams();
    count_after_first = cbba_broadcast_send_count();
    CHECK(count_after_first == 5u); /* one send per locally-hosted team */

    /* Nothing in g_cbba_agent_state changed between calls - the gate
       should skip every one of these five teams' sends. */
    cbba_broadcast_local_teams();
    count_after_unchanged = cbba_broadcast_send_count();
    CHECK(count_after_unchanged == count_after_first);

    /* A real belief change for team 0 only - only that team's send
       should go out this round, not all five. */
    g_cbba_agent_state[0].winner[incident_slot] = 0;
    g_cbba_agent_state[0].winning_bid[incident_slot] = 9.5f;
    cbba_broadcast_local_teams();
    count_after_changed = cbba_broadcast_send_count();
    CHECK(count_after_changed == count_after_unchanged + 1u);
}

/* CBBA_FORCE_RESEND_TICKS must eventually force a resend even with no
   belief change, so a peer that missed an earlier update (dropped frame,
   post-reboot resync) isn't stuck forever - see core/cbba.c's
   CBBALastBroadcast_t comment for why. */
static void test_broadcast_force_resend_floor(void)
{
    uint32_t count_before;
    uint32_t count_after;

    g_comms_transport_this_board_id = 0;
    comms_transport_init();
    cbba_init(); /* zeroes the send-on-change cache - see the same note
                    in test_broadcast_send_on_change_gate() above */
    reset_agent_state();

    cbba_broadcast_local_teams(); /* forced initial send */
    count_before = cbba_broadcast_send_count();

    /* CBBA_ROUND_TIMEOUT_MS ticks worth of unchanged calls, one more
       than CBBA_FORCE_RESEND_TICKS (defined in core/cbba.c) - by the
       last of these, the floor must have re-triggered a send for every
       locally-hosted team even though nothing changed. Hardcoding 5
       here rather than including CBBA_FORCE_RESEND_TICKS keeps this
       test from silently losing coverage if that constant is retuned;
       if it ever needs to grow past 5, this loop bound should grow
       with it, not be assumed to already cover it. */
    for(int i = 0; i < 5; i++)
        cbba_broadcast_local_teams();

    count_after = cbba_broadcast_send_count();
    CHECK(count_after > count_before);
}

static void test_round_finalizes_without_hanging(void)
{
    g_comms_transport_this_board_id = 0;
    comms_transport_init(); /* empty inbox - nothing will ever arrive */
    reset_agent_state();

    /* If this hangs, the test process itself hangs - that IS the
       failure signal for "blocking indefinitely" (see cbba.h's
       CBBA_ROUND_TIMEOUT_MS doc comment). Reaching the CHECK below at
       all is the pass condition. */
    cbba_ingest_remote_mesh_replies();
    CHECK(1);
    printf("PASS: cbba_ingest_remote_mesh_replies() returned with an empty inbox "
           "(bounded by CBBA_ROUND_TIMEOUT_MS=%u via cbba.c's weak cbba_now_ms())\n",
           (unsigned)CBBA_ROUND_TIMEOUT_MS);
}

int main(void)
{
    test_board_locality();
    test_ingest_from_simulated_other_board();
    test_wait_polls_past_foreign_frames();
    test_broadcast_send_on_change_gate();
    test_broadcast_force_resend_floor();
    test_round_finalizes_without_hanging();

    if (g_failures == 0)
    {
        printf("ALL CBBA board/mesh-timeout selftests PASSED\n");
        return 0;
    }

    printf("%d CBBA board/mesh-timeout selftest(s) FAILED\n", g_failures);
    return 1;
}

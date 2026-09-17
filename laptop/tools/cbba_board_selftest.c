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

/* Mirrors cbba.c's own private CBBAMessage_t field-for-field (see
   that file's comment: "Wire format only ... no function below is
   allowed to read another agent's live CBBAAgentState_t directly").
   comms_transport_send()/poll_received() move opaque bytes, so a
   test can inject a message using its own copy of that exact layout
   without cbba.c needing to expose the type - same technique
   lora_frame.h's own sizing comment already leans on (byte layout,
   not the C type, is the actual contract). If cbba.c's CBBAMessage_t
   ever changes, this struct must change with it or this test's
   injected bytes stop matching what cbba_ingest_remote_mesh_replies()
   expects to unpack. */
typedef struct
{
    uint8_t sender_id;
    uint8_t winner[NUM_INCIDENTS];
    float winning_bid[NUM_INCIDENTS];
    uint32_t timestamp[NUM_TEAMS];
} TestCBBAMessage_t;

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
    msg.winning_bid[incident_slot] = 3.5f;
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
        CHECK(g_cbba_agent_state[local_team].winner[incident_slot] == remote_sender);
        CHECK(g_cbba_agent_state[local_team].winning_bid[incident_slot] > 3.0f);
        CHECK(g_cbba_agent_state[local_team].timestamp[remote_sender] == g_current_tick);
    }

    for (uint8_t remote_team = 5; remote_team < NUM_TEAMS; remote_team++)
    {
        CHECK(g_cbba_agent_state[remote_team].winner[incident_slot] == NO_ASSIGNMENT);
    }
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
    test_round_finalizes_without_hanging();

    if (g_failures == 0)
    {
        printf("ALL CBBA board/mesh-timeout selftests PASSED\n");
        return 0;
    }

    printf("%d CBBA board/mesh-timeout selftest(s) FAILED\n", g_failures);
    return 1;
}

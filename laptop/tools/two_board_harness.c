/*
 * Two-board communication harness - the thing referenced (and never
 * built) by an old comment in cbba_board_selftest.c: "a legitimate
 * stand-in... until Prompt 5's in-process harness (or real hardware)
 * exists to drive two boards' worth of comms_transport for real."
 * This is that harness, for the mission-link/handshake side (CBBA's
 * own equivalent is a separate, later exercise - see this file's
 * closing comment).
 *
 * What this actually proves, and what it does NOT:
 *
 *   PROVES: the REAL, unmodified board firmware code (board/
 *   STM32N6570-DK/mission_link_task.c, core/mission_link_handshake.c,
 *   core/mission_link.c, core/comms_transport.c - nothing in this
 *   file is a mock or a rewritten stand-in for any of them) correctly
 *   completes a handshake and delivers a roster message across a REAL
 *   byte-stream transport (a Unix domain socket) between two SEPARATE
 *   OS processes, each running its own independent copy of every
 *   static/global state this project's mission-link code keeps (see
 *   "why two processes" below) - and that the receiving side's
 *   application state (g_teams[]) actually changes as a result, not
 *   just that bytes moved.
 *
 *   Does NOT prove: that real SPI/LoRa hardware works (no SX1262 HAL
 *   exists yet - see core/vendor/sx126x_driver/README.md), that
 *   either board's real firmware boots on real silicon (neither has
 *   ever been flashed), or that the µT-Kernel scheduler itself
 *   behaves correctly under real task preemption (this harness runs
 *   mission_link_task_step() as a plain function call in a loop, not
 *   under tk_cre_tsk/tk_sta_tsk). A Unix socket is a stand-in for the
 *   physical LoRa link - closer to real communication than the old
 *   same-process-inbox stand-in it replaces (see cbba_board_
 *   selftest.c's comment this file's own top references), but still
 *   not real hardware.
 *
 * ---------------------------------------------------------------
 * Why two OS processes, not two "board contexts" in one process
 * ---------------------------------------------------------------
 * core/comms_transport.c, core/mission_link_handshake.c, and
 * board/STM32N6570-DK/mission_link_task.c all keep their state in
 * file-static globals (g_comms_transport_this_board_id, g_inbox[],
 * g_local_ready, g_relay_reply_sent[], ...) - by design, matching how
 * real firmware works (one board, one process, one set of global
 * state - see e.g. comms_transport.c's own "Defaults to 0 so
 * single-board laptop/desktop builds... need no setup at all"
 * reasoning). That means a single process cannot host two
 * independent "boards" of this code simultaneously - both would
 * silently share the same g_inbox[], the same handshake state, etc.
 * fork() gives each side its own independent copy of every global
 * (standard COW semantics), which is the honest way to model "two
 * separate physical boards" without rewriting this project's global-
 * state architecture into a context-passing one just for a test
 * harness - a much bigger, riskier change than this harness is worth.
 *
 * ---------------------------------------------------------------
 * Transport: what stands in for the SX1262 radio
 * ---------------------------------------------------------------
 * A single socketpair(AF_UNIX, SOCK_STREAM) created BEFORE fork(), so
 * each child inherits one end. lora_spi_transmit() (weak in
 * comms_transport.c - see that file's own doc) is overridden here,
 * strong, to write( ) the packed wire bytes onto this process's own
 * socket fd. The receiving side's own harness_rx_pump() below reads
 * whatever bytes are available, accumulates them (a stream socket can
 * fragment a write across multiple reads, same reason mission_link_
 * task.c's own USART1 accumulate-buffer exists), and repeatedly calls
 * lora_frame_try_parse() - the SAME function the real same-board path
 * in comms_transport_send() already uses - handing each successfully
 * parsed frame to comms_transport_deliver_received() (new, real,
 * production code added alongside this harness - see comms_
 * transport.h's own doc on it).
 *
 * ---------------------------------------------------------------
 * The scenario driven end-to-end
 * ---------------------------------------------------------------
 * "Board 0" (parent, id 0, teams 0-4) has one byte source -
 * board_usart1_read_byte() (weak in mission_link_task.c) is overridden
 * here, strong, to serve TWO real pre-packed wire frames in sequence:
 * a MissionLinkHandshakeReq_t{board_id=0} (as if the laptop completed
 * its own handshake with DK first - required, see run_board0()'s own
 * comment on why this is needed and how it was found), then a
 * MissionLinkRoster_t for team_id=5. Nothing about the parent's role
 * differs from real board-0 firmware beyond this substitution for a
 * real UART peripheral.
 *
 *   1. Both sides complete the real handshake (mission_link_task_
 *      step() drives this automatically once board_init()/mark_
 *      local_ready() have been called - nothing in this file
 *      hand-drives handshake state directly).
 *   2. Board 0's real mission_link_usart1_rx_step() parses the
 *      injected roster bytes, sees team 5 belongs to board 1 (comms_
 *      transport_board_id_for_team(5) == 1, COMMS_TRANSPORT_TEAMS_
 *      PER_BOARD's real default of 5 - this harness does NOT override
 *      it to NUM_TEAMS the way the laptop build does, since a real
 *      2-board split is exactly what's being tested), and relays it
 *      over comms_transport_send() -> this harness's socket-backed
 *      lora_spi_transmit().
 *   3. Board 1 (child) receives the bytes via harness_rx_pump(),
 *      parses the real LoRa frame, and its own real mission_link_
 *      comms_transport_rx_step() applies it via mission_link_apply_
 *      roster() - mutating g_teams[5] for real.
 *   4. Board 1 checks g_teams[5] against the expected values and
 *      reports pass/fail via its exit code; the parent waits for the
 *      child and combines both processes' verdicts.
 *
 * ---------------------------------------------------------------
 * Build (see also: make check-two-board-harness)
 * ---------------------------------------------------------------
 *   gcc -std=gnu11 -Wall -Wextra -Icore -Iboard/STM32N6570-DK -Ilaptop \
 *       -Ilaptop/tools/harness_stubs \
 *       -o two_board_harness laptop/tools/two_board_harness.c \
 *       board/STM32N6570-DK/mission_link_task.c laptop/debug.c \
 *       laptop/snapshot.c $(echo core dir all .c files) -lm
 *
 * Ilaptop/tools/harness_stubs supplies a minimal tm/tmonitor.h so
 * mission_link_task.c's one real BSP dependency, tm_putchar, inside
 * mission_link_usart1_write, links without needing either real
 * vendored BSP - see that stub header's own doc. laptop/debug.c and
 * laptop/snapshot.c - not either board's own - supply the debug_
 * trace/snapshot_record family every core .c file calls, since a
 * plain stdio backend is all a desktop harness needs; a full
 * tm_printf emulation would be needed only to link board/STM32N6570-
 * DK/debug.c instead, which buys nothing extra for this harness's
 * purpose.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

#include "mission_state.h"
#include "comms_transport.h"
#include "lora_frame.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_link_handshake.h"
#include "cbba.h" /* cbba_now_ms() - see run_board0()'s own doc on
                      polling this board's own handshake state via
                      the same clock mission_link_task_step() uses
                      internally. */

extern void mission_link_task_init(void);
extern void mission_link_task_step(void);

/* Shared by both processes' copies of these weak overrides -
   independent after fork() (COW), which is the entire point (see
   this file's top comment on "why two processes"). */
static int g_harness_socket_fd = -1;

/* ---- lora_spi_transmit() override: writes onto this process's own
   socket fd, standing in for the SX1262 radio. Strong definition
   here supersedes comms_transport.c's own weak default (same
   linker-resolution mechanism that file's own doc describes for a
   REAL driver's strong definition - this harness is, in that sense,
   exercising exactly the same override mechanism a real SPI driver
   eventually will). ---- */
int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len)
{
    ssize_t written = write(g_harness_socket_fd, frame_bytes, frame_len);
    if (written < 0 || (uint32_t)written != frame_len)
    {
        return -1;
    }
    return 0;
}

/* ---- board_usart1_read_byte() override: board 0 (parent) only -
   serves a real MissionLinkHandshakeReq_t{board_id=0} wire frame
   FIRST (as if the laptop had completed its own handshake with DK -
   without this, board 0's own readiness gate, which correctly
   applies to RELAYING mission-link data and not just applying it
   locally - see mission_link_task.c's mission_link_handshake_ready_
   now() doc - never becomes true, and the roster below would never
   get relayed at all; this was found by actually running this
   harness and getting a real, honest failure, not assumed up front),
   followed by exactly one pre-packed MissionLinkRoster_t wire frame,
   byte by byte. Board 1 (child) never sets g_harness_injected_len, so
   its copy of this override always returns -1 (same as the real weak
   default) - no separate code path needed for "board 1 has nothing to
   read". ---- */
static uint8_t g_harness_req_bytes[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE];
static uint32_t g_harness_req_len;
static uint8_t g_harness_roster_bytes[MISSION_LINK_ROSTER_WIRE_SIZE];
static uint32_t g_harness_roster_len;

static uint8_t g_harness_injected_bytes[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE + MISSION_LINK_ROSTER_WIRE_SIZE];
static uint32_t g_harness_injected_len;
static uint32_t g_harness_injected_pos;

int board_usart1_read_byte(void)
{
    if (g_harness_injected_pos >= g_harness_injected_len)
    {
        return -1;
    }
    return (int)g_harness_injected_bytes[g_harness_injected_pos++];
}

/* Reads whatever bytes are currently available on the socket
   (non-blocking - matches board_usart1_read_byte()'s own "must never
   wait" contract, applied here to the LoRa side of the transport
   too), accumulates them, and repeatedly tries lora_frame_try_parse()
   - the exact same function comms_transport_send()'s own same-board
   path already uses - handing each parsed frame to comms_transport_
   deliver_received() (see that function's own doc in comms_
   transport.h/.c). Small flat buffer, same reasoning mission_link_
   task.c's own USART1 accumulate buffer already documents: at the
   pace this harness drives things, it never needs to hold more than
   a handful of bytes at once. */
static void harness_rx_pump(void)
{
    static uint8_t buf[512];
    static uint32_t buf_len;
    uint8_t chunk[256];
    ssize_t n;

    n = read(g_harness_socket_fd, chunk, sizeof(chunk));
    if (n > 0)
    {
        uint32_t i;
        for (i = 0u; i < (uint32_t)n && buf_len < sizeof(buf); i++)
        {
            buf[buf_len++] = chunk[i];
        }
    }
    /* n == 0 (peer closed) or n < 0 (EAGAIN/EWOULDBLOCK, nothing
       available right now) - both are "nothing new this pump", not
       an error worth reporting; the caller's own iteration cap is
       what bounds how long this harness waits overall. */

    for (;;)
    {
        uint8_t frame_type;
        uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t payload_len;
        int rc = lora_frame_try_parse(buf, buf_len, &frame_type, payload, &payload_len);

        if (rc == 0)
        {
            break; /* need more bytes */
        }

        if (rc == 1)
        {
            uint32_t consumed = 2u + payload_len + 1u; /* header + payload + checksum, per lora_frame.h */
            uint32_t i;

            (void)comms_transport_deliver_received(frame_type, payload, payload_len);

            for (i = consumed; i < buf_len; i++)
            {
                buf[i - consumed] = buf[i];
            }
            buf_len -= consumed;
            continue;
        }

        /* rc == -1: resync, same "drop exactly one byte and retry"
           contract mission_link_task.c's own accumulate loops already
           follow - this harness's socket transport is not expected to
           corrupt bytes, but the parser's own contract still applies
           uniformly regardless of why a resync might be needed. */
        {
            uint32_t i;
            for (i = 1u; i < buf_len; i++)
            {
                buf[i - 1u] = buf[i];
            }
            buf_len -= 1u;
        }
    }
}

static void make_socket_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
    {
        perror("fcntl(F_GETFL)");
        return;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl(F_SETFL, O_NONBLOCK)");
    }
}

#define HARNESS_MAX_ITERATIONS 2000

/* ---- Board 1 (child): receiver + the actual pass/fail check ---- */
static int run_board1(void)
{
    int i;
    const uint8_t expect_team_id = 5u;

    g_comms_transport_this_board_id = 1u;
    comms_transport_init();
    mission_link_task_init();
    mission_link_handshake_board_init(1u);
    mission_link_handshake_mark_local_ready();

    for (i = 0; i < HARNESS_MAX_ITERATIONS; i++)
    {
        harness_rx_pump();
        mission_link_task_step();

        if (g_teams[expect_team_id].team_id == expect_team_id &&
            g_teams[expect_team_id].loc_x == 1234 &&
            g_teams[expect_team_id].loc_y == -500 &&
            g_teams[expect_team_id].stamina > 79.4f && g_teams[expect_team_id].stamina < 80.6f)
        {
            fprintf(stderr, "[board1] PASS: g_teams[%u] roster applied correctly after "
                             "a real cross-process handshake + LoRa relay + apply "
                             "(iteration %d)\n", expect_team_id, i);
            return 0;
        }
    }

    fprintf(stderr, "[board1] FAIL: g_teams[%u] never reflected the expected roster "
                     "within %d iterations (loc_x=%d loc_y=%d stamina=%f team_id=%u)\n",
            expect_team_id, HARNESS_MAX_ITERATIONS,
            g_teams[expect_team_id].loc_x, g_teams[expect_team_id].loc_y,
            (double)g_teams[expect_team_id].stamina, g_teams[expect_team_id].team_id);
    return 1;
}

/* ---- Board 0 (parent): laptop-injected roster source + relay ---- */
static int run_board0(void)
{
    int i;
    int roster_injected = 0;
    MissionLinkRoster_t roster;

    g_comms_transport_this_board_id = 0u;
    comms_transport_init();
    mission_link_task_init();
    mission_link_handshake_board_init(0u);
    mission_link_handshake_mark_local_ready();

    roster.team_id = 5u;
    roster.specialization = 0u;
    roster.equipment_flags = 0u;
    roster.fitness_tier = 0u;
    roster.loc_x = 1234;
    roster.loc_y = -500;
    roster.stamina = 80u;

    {
        MissionLinkHandshakeReq_t laptop_req;

        laptop_req.board_id = 0u;
        laptop_req.protocol_version = MISSION_LINK_PROTOCOL_VERSION;

        g_harness_req_len = mission_link_handshake_pack_req(&laptop_req, g_harness_req_bytes);
        g_harness_roster_len = mission_link_pack_roster(&roster, g_harness_roster_bytes);
    }

    /* Req is available from iteration 0 - as if the laptop reached DK
       immediately, same as before. The roster is deliberately held
       back until board 0's OWN mission_link_handshake_get_relay_
       reply(1u, ...) genuinely confirms board 1 is ready - polling
       the exact function DK itself uses to decide what's safe to
       forward to the laptop, not an arbitrary iteration count. A
       first version of this fix used a fixed iteration-count delay
       instead; that was flaky (7/8 runs failed) because two forked
       processes' loop iterations have no real-time relationship to
       each other - found by actually running it multiple times, not
       assumed to be reliable after one passing run. Polling this
       board's own real handshake state instead is what makes this
       deterministic: it directly mirrors what a real laptop's
       HandshakeCoordinator.all_ready would actually be gated on (see
       laptop/tools/telemetry_bridge.py), not a guess at how long that
       might take. */
    for (i = 0; i < HARNESS_MAX_ITERATIONS; i++)
    {
        harness_rx_pump();

        if (i == 0)
        {
            memcpy(g_harness_injected_bytes, g_harness_req_bytes, g_harness_req_len);
            g_harness_injected_len = g_harness_req_len;
        }
        else if (!roster_injected)
        {
            MissionLinkHandshakeReply_t peer1_reply;
            uint32_t now_ms = cbba_now_ms();

            if (mission_link_handshake_get_relay_reply(1u, now_ms, &peer1_reply) &&
                peer1_reply.ready)
            {
                memcpy(g_harness_injected_bytes + g_harness_injected_len,
                       g_harness_roster_bytes, g_harness_roster_len);
                g_harness_injected_len += g_harness_roster_len;
                roster_injected = 1;
            }
        }

        mission_link_task_step();
    }

    /* Board 0 has no local assertion of its own to make - its whole
       job in this scenario is "receive from USART1, relay over LoRa"
       - the real check lives on board 1, where the data actually has
       to land. Board 0 always exits 0; run_board1()'s exit code is
       what the parent actually waits on. */
    return 0;
}

int main(void)
{
    int sockets[2];
    pid_t child_pid;
    int child_status = 1;
    int board0_rc;

    /* A real radio has no "broken pipe" concept - the peer being gone
       just means TX has nowhere to land, not a signal that kills the
       sender. Board 1 exiting slightly before board 0 finishes its
       own iteration count (timing between two forked processes isn't
       lockstep) would otherwise SIGPIPE-kill board 0's process on its
       next write() - found by actually running this harness and
       getting a real SIGPIPE crash, not anticipated up front. */
    signal(SIGPIPE, SIG_IGN);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    {
        perror("socketpair");
        return 1;
    }

    child_pid = fork();
    if (child_pid < 0)
    {
        perror("fork");
        return 1;
    }

    if (child_pid == 0)
    {
        /* Child: board 1 */
        close(sockets[0]);
        g_harness_socket_fd = sockets[1];
        make_socket_nonblocking(g_harness_socket_fd);
        _exit(run_board1());
    }

    /* Parent: board 0 */
    close(sockets[1]);
    g_harness_socket_fd = sockets[0];
    make_socket_nonblocking(g_harness_socket_fd);
    board0_rc = run_board0();

    if (waitpid(child_pid, &child_status, 0) < 0)
    {
        perror("waitpid");
        return 1;
    }

    if (board0_rc != 0)
    {
        fprintf(stderr, "[harness] FAIL: board 0 process reported failure\n");
        return 1;
    }

    if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
    {
        fprintf(stderr, "[harness] FAIL: board 1 process reported failure "
                         "(see [board1] output above)\n");
        return 1;
    }

    fprintf(stderr, "[harness] ALL CHECKS PASSED: real handshake + real LoRa-relayed "
                     "roster delivery, across two separate OS processes connected by "
                     "a real socket, using unmodified production mission-link code.\n");
    return 0;
}

/*
 * ---------------------------------------------------------------
 * What this harness does NOT cover (scoped deliberately, not an
 * oversight)
 * ---------------------------------------------------------------
 * - CBBA mesh consensus (cbba.c) - a separate exercise from mission-
 *   link; cbba_board_selftest.c's own same-process stand-in (the
 *   comment this file opens by referencing) still applies there.
 *   Could be extended into this same two-process shape later, using
 *   the same socketpair() transport, once/if that's worth doing.
 * - Incident/fault relay, or more than one roster message - this
 *   harness proves the pattern works end-to-end for one message type;
 *   extending coverage to the others is mechanical repetition of the
 *   same proof, not a new risk this harness needs to re-derive.
 * - Any board3+ scenario (mission_link_handshake.c is now N-board
 *   generic - see CHANGELOG.md - but this harness only drives 2,
 *   matching this project's actual board count).
 */

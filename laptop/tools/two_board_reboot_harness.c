/*
 * Two-board REBOOT harness: one board reboots while the other stays up,
 * and the link has to recover on its own.
 *
 * Same two-process setup as two_board_harness.c (read that file's top
 * comment for why two processes, and how lora_spi_transmit() /
 * board_usart1_read_byte() are overridden): board 0 is the parent, board
 * 1 the child, joined by a Unix socket standing in for the radio. The
 * production code under test is unmodified - core/ plus one board's
 * mission_link_task.c.
 *
 * A "reboot" here resets everything a real power cycle resets that this
 * code keeps in RAM: lora_crypto (replay floors, sessions, TX counter),
 * comms_transport (queues, inbox), the handshake, mission_link_task's own
 * state, g_teams[] - and draws a fresh session epoch, as main_board.c's
 * board_seed_lora_session_epoch() does from the SX1262 RNG.
 *
 * Usage: two_board_reboot_harness <board1|board0> <quiet|cbba>
 *
 *   board1  Board 1 (Pico role) reboots after its roster is applied.
 *           Before the fix: board 1's fresh counter restarted at 0 and
 *           board 0's replay floor silently dropped everything it sent,
 *           and board 0 never re-sent its Req. Passes when board 1
 *           completes a new handshake, board 0 counts exactly one peer
 *           reboot, and a roster re-sent after the reboot lands on the
 *           rebooted board 1.
 *
 *   board0  Board 0 (DK role) reboots after first raising board 1's
 *           replay floor for sender 0 well above the handful of Reqs a
 *           rebooted board 0 sends. Passes when board 0 sees board 1's
 *           Reply again and a second, different roster sent after the
 *           reboot lands on board 1 (which stayed up).
 *
 *   cbba    Once "simulating", board 0 also sends CBBA frames at PRIORITY
 *           every step, which board 1 (in a boot wait loop) never consumes
 *           - this is what hid Reqs/rosters behind foreign frames at the
 *           inbox head, and what starved ROUTINE roster relays. Board 0
 *           also first burns its TX counter past 70000 (frames packed but
 *           lost on air), and in board1 mode board 1 reboots only after
 *           seeing that traffic, so board 0's counter is past the old
 *           16-bit wire counter's range when board 1 has to re-sync.
 *
 * Exit code 0 only if both processes pass. make check-two-board-reboot-
 * harness runs every mode/traffic combination against both boards'
 * mission_link_task.c, several times each.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <time.h>

#include "mission_state.h"
#include "comms_transport.h"
#include "lora_frame.h"
#include "lora_crypto.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_link_handshake.h"

extern void mission_link_task_init(void);
extern void mission_link_task_step(void);
extern uint32_t mission_link_task_peer_reboot_count(void);

/* Generous: a passing run finishes each phase in well under a second, so
   these only decide how long a genuine hang takes to report. */
#define PHASE_BUDGET_MS            20000u
/* Board 0's TX counter is pushed past this in cbba mode - beyond the
   16-bit counter the wire used to carry (see board1/cbba above). Done by
   packing frames that never reach the socket, i.e. lost on air: sending
   70000 real frames takes far too long whenever the processes are
   contended. */
#define CBBA_TX_COUNTER_FLOOR      70000u
/* CBBA frames board 1 must have discarded before it reboots in
   board1/cbba mode: proof board 0 is simulating, so its counter is
   already past CBBA_TX_COUNTER_FLOOR. */
#define CBBA_FRAMES_BEFORE_REBOOT  50u
/* MISSION_LINK frames board 0 sends before rebooting in board0 mode, to
   raise board 1's floor for sender 0 far above what a rebooted board 0
   sends before recovering. Unknown sub-type, so board 1 drops them after
   they pass authentication and the replay check. */
#define BOARD0_FLOOR_FILLER_FRAMES 300u

static int g_socket_fd = -1;
static int g_peer_closed;
static int g_mode_board0;   /* 0 = board1 reboots, 1 = board0 reboots */
static int g_cbba_traffic;
/* Set on board 0 once its first roster is out - the point where real
   firmware leaves the boot wait loop and starts running CBBA. Cleared by
   board 0's own reboot, which puts it back in the boot wait loop. */
static int g_board0_simulating;
/* Frames board 1's boot-wait discard has dropped (CBBA traffic). */
static uint32_t g_board1_discarded;

/* ---- radio + USART1 stand-ins (strong over the weak defaults) ---- */

/* Whole frame or nothing lost: waits up to a second for socket space
   rather than dropping (see two_board_harness.c's harness_socket_write_
   frame() for why - under CPU load a plain non-blocking write lost most
   frames). */
int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len)
{
    uint32_t done = 0u;

    while (done < frame_len)
    {
        ssize_t n = write(g_socket_fd, frame_bytes + done, frame_len - done);

        if (n > 0)
        {
            done += (uint32_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct pollfd pfd;

            pfd.fd = g_socket_fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 1000) > 0)
            {
                continue;
            }
        }
        return -1;
    }
    return 0;
}

static uint8_t g_usart1_bytes[256];
static uint32_t g_usart1_len;
static uint32_t g_usart1_pos;

int board_usart1_read_byte(void)
{
    return g_usart1_pos < g_usart1_len ? (int)g_usart1_bytes[g_usart1_pos++] : -1;
}

/* Queues bytes as if the laptop had just written them to board 0's UART. */
static void usart1_inject(const uint8_t *bytes, uint32_t len)
{
    if (g_usart1_pos == g_usart1_len)
    {
        g_usart1_pos = 0u;
        g_usart1_len = 0u;
    }
    if (g_usart1_len + len <= sizeof(g_usart1_bytes))
    {
        memcpy(g_usart1_bytes + g_usart1_len, bytes, len);
        g_usart1_len += len;
    }
}

static void rx_pump(void)
{
    static uint8_t buf[512];
    static uint32_t buf_len;
    uint8_t chunk[256];
    ssize_t n = read(g_socket_fd, chunk, sizeof(chunk));

    if (n > 0)
    {
        ssize_t i;
        for (i = 0; i < n && buf_len < sizeof(buf); i++)
        {
            buf[buf_len++] = chunk[i];
        }
    }
    else if (n == 0)
    {
        g_peer_closed = 1;
    }

    for (;;)
    {
        uint8_t frame_type;
        uint8_t sender_board_id;
        uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t payload_len;
        uint32_t consumed;
        uint32_t i;
        int rc = lora_frame_try_parse(buf, buf_len, &frame_type, &sender_board_id,
                                       payload, &payload_len);

        if (rc == 0)
        {
            break;
        }
        if (rc == 1)
        {
            (void)comms_transport_deliver_received(frame_type, payload, payload_len);
            consumed = LORA_FRAME_HEADER_SIZE + payload_len + LORA_FRAME_TRAILER_SIZE;
        }
        else
        {
            consumed = 1u; /* resync: drop one byte and retry */
        }
        for (i = consumed; i < buf_len; i++)
        {
            buf[i - consumed] = buf[i];
        }
        buf_len -= consumed;
    }
}

static void make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
    {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

static uint32_t elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 +
                      (now.tv_nsec - start->tv_nsec) / 1000000);
}

/* ---- board lifecycle ---- */

/* Differs between boots and between the two processes, like the SX1262
   RNG main_board.c seeds from; never 0 (0 is "unset"). */
static uint32_t fresh_epoch(void)
{
    struct timespec now;
    uint32_t e;

    clock_gettime(CLOCK_MONOTONIC, &now);
    e = (uint32_t)now.tv_nsec ^ ((uint32_t)now.tv_sec * 2654435761u) ^
        ((uint32_t)getpid() * 40503u);
    return e != 0u ? e : 0x5EED0001u;
}

static void board_power_on(uint8_t board_id)
{
    memset(g_teams, 0, sizeof(g_teams));
    if (board_id == 0u)
    {
        g_board0_simulating = 0;
    }
    lora_crypto_reset();
    g_comms_transport_this_board_id = board_id;
    comms_transport_init();
    lora_crypto_set_tx_epoch(fresh_epoch());
    mission_link_task_init();
    mission_link_handshake_board_init(board_id);
    mission_link_handshake_mark_local_ready();
}

/* One pass of a board's loop. Board 1 in this harness is always in
   main_board.c's boot wait loop, which discards frames nothing reads
   before the simulation starts. */
static void board_step(void)
{
    if (g_comms_transport_this_board_id == 0u && g_board0_simulating && g_cbba_traffic)
    {
        /* PRIORITY, like cbba_broadcast_local_teams(), and only once board
           0 is "simulating" - real CBBA doesn't start until board 0's own
           roster wait is over. One send per dispatch is milder than the
           real board, where each tick queues one CBBA frame per local
           team (5) against one dispatch. Sent before dispatch so a slot
           the dispatch frees is open to other senders until next step. */
        static const uint8_t cbba_payload[2] = {0xC0u, 0u};
        static uint8_t sender_team;
        (void)comms_transport_send(sender_team, 5u, LORA_FRAME_TYPE_CBBA,
                                   COMMS_PRECEDENCE_PRIORITY, cbba_payload,
                                   (uint8_t)sizeof(cbba_payload));
        sender_team = (uint8_t)((sender_team + 1u) % 5u); /* board 0's teams 0-4 */
    }

    rx_pump();
    mission_link_task_step();
    comms_transport_dispatch_pending();

    if (g_comms_transport_this_board_id == 1u)
    {
        g_board1_discarded += comms_transport_discard_received_except(
            COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_HANDSHAKE) |
            COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_MISSION_LINK));
    }
}

/* ---- laptop-side frames ---- */

#define ROSTER_TEAM 5u /* hosted on board 1 (5 teams per board) */

static void laptop_send_req(void)
{
    MissionLinkHandshakeReq_t req;
    uint8_t bytes[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE];

    req.board_id = 0u;
    req.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
    usart1_inject(bytes, mission_link_handshake_pack_req(&req, bytes));
}

static void laptop_send_roster(int16_t loc_x, int16_t loc_y)
{
    MissionLinkRoster_t roster;
    uint8_t bytes[MISSION_LINK_ROSTER_WIRE_SIZE];

    memset(&roster, 0, sizeof(roster));
    roster.team_id = ROSTER_TEAM;
    roster.loc_x = loc_x;
    roster.loc_y = loc_y;
    roster.stamina = 80u;
    usart1_inject(bytes, mission_link_pack_roster(&roster, bytes));
}

static int roster_landed(int16_t loc_x, int16_t loc_y)
{
    return g_teams[ROSTER_TEAM].team_id == ROSTER_TEAM &&
           g_teams[ROSTER_TEAM].loc_x == loc_x && g_teams[ROSTER_TEAM].loc_y == loc_y;
}

/* Two distinct in-grid locations, so the second roster can't be mistaken
   for the first. */
#define ROSTER1_X 29
#define ROSTER1_Y 17
#define ROSTER2_X 12
#define ROSTER2_Y 8

/* ---- board 1 (child) ---- */

static int run_board1(void)
{
    struct timespec t;
    MissionLinkHandshakeReply_t reply;

    board_power_on(1u);

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!roster_landed(ROSTER1_X, ROSTER1_Y))
    {
        board_step();
        if (elapsed_ms(&t) > PHASE_BUDGET_MS)
        {
            fprintf(stderr, "[board1] FAIL: first roster never landed\n");
            return 1;
        }
    }

    if (g_mode_board0)
    {
        /* Board 1 stays up; board 0 reboots and must reach it again. */
        clock_gettime(CLOCK_MONOTONIC, &t);
        while (!roster_landed(ROSTER2_X, ROSTER2_Y))
        {
            board_step();
            if (elapsed_ms(&t) > 2u * PHASE_BUDGET_MS)
            {
                fprintf(stderr, "[board1] FAIL: roster sent after board 0's reboot never "
                                 "landed (auth_fail=%u hold=%u replay=%u)\n",
                        lora_frame_auth_fail_count(), lora_frame_session_hold_count(),
                        lora_frame_replay_count());
                return 1;
            }
        }
        fprintf(stderr, "[board1] PASS: stayed up; roster sent after board 0's reboot landed\n");
        return 0;
    }

    if (g_cbba_traffic)
    {
        clock_gettime(CLOCK_MONOTONIC, &t);
        while (g_board1_discarded < CBBA_FRAMES_BEFORE_REBOOT)
        {
            board_step();
            if (elapsed_ms(&t) > PHASE_BUDGET_MS)
            {
                fprintf(stderr, "[board1] FAIL: saw only %u CBBA frames before reboot, "
                                 "need %u\n", g_board1_discarded, CBBA_FRAMES_BEFORE_REBOOT);
                return 1;
            }
        }
    }

    /* Let board 0 finish recording this session before the reboot. */
    clock_gettime(CLOCK_MONOTONIC, &t);
    while (elapsed_ms(&t) < 200u)
    {
        board_step();
    }

    board_power_on(1u);

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!(mission_link_handshake_build_local_reply(&reply) && roster_landed(ROSTER1_X, ROSTER1_Y)))
    {
        board_step();
        if (elapsed_ms(&t) > PHASE_BUDGET_MS)
        {
            fprintf(stderr, "[board1] FAIL: after reboot, handshake/roster never recovered "
                             "(handshake_done=%d roster=%d auth_fail=%u hold=%u)\n",
                    mission_link_handshake_build_local_reply(&reply),
                    roster_landed(ROSTER1_X, ROSTER1_Y),
                    lora_frame_auth_fail_count(), lora_frame_session_hold_count());
            return 1;
        }
    }
    fprintf(stderr, "[board1] PASS: rebooted; handshake and roster recovered in %u ms\n",
            elapsed_ms(&t));
    return 0;
}

/* ---- board 0 (parent) ---- */

/* Steps until board 0 has board 1's Reply, then sends the roster. */
static int board0_deliver_roster(int16_t loc_x, int16_t loc_y, const char *phase)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!mission_link_handshake_relay_reply_seen(1u))
    {
        board_step();
        if (g_peer_closed || elapsed_ms(&t) > PHASE_BUDGET_MS)
        {
            fprintf(stderr, "[board0] FAIL: %s: never received board 1's Reply "
                             "(auth_fail=%u hold=%u replay=%u)\n", phase,
                    lora_frame_auth_fail_count(), lora_frame_session_hold_count(),
                    lora_frame_replay_count());
            return 1;
        }
    }
    laptop_send_roster(loc_x, loc_y);
    return 0;
}

static void board0_run_until_peer_exits(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!g_peer_closed && elapsed_ms(&t) < 3u * PHASE_BUDGET_MS)
    {
        board_step();
    }
}

static int run_board0(void)
{
    struct timespec t;

    board_power_on(0u);
    laptop_send_req();

    if (board0_deliver_roster(ROSTER1_X, ROSTER1_Y, "first boot"))
    {
        return 1;
    }
    /* Let the first roster reach board 1, then start "simulating". */
    clock_gettime(CLOCK_MONOTONIC, &t);
    while (elapsed_ms(&t) < 300u)
    {
        board_step();
    }
    if (g_cbba_traffic)
    {
        static const uint8_t lost_payload[2] = {0xC0u, 0u};
        uint8_t wire[LORA_FRAME_MAX_WIRE_SIZE];

        while (lora_crypto_next_tx_counter() <= CBBA_TX_COUNTER_FLOOR)
        {
            (void)lora_frame_pack(LORA_FRAME_TYPE_CBBA, 0u, lost_payload,
                                  (uint8_t)sizeof(lost_payload), wire);
        }
    }
    g_board0_simulating = 1;

    if (!g_mode_board0)
    {
        /* Board 1 reboots. Board 0 must notice (new session from a known
           peer), tell the laptop that peer isn't ready, and restart that
           peer's handshake leg; the laptop re-sends the roster once board
           1 is ready again. */
        clock_gettime(CLOCK_MONOTONIC, &t);
        while (mission_link_task_peer_reboot_count() == 0u)
        {
            board_step();
            if (g_peer_closed || elapsed_ms(&t) > 2u * PHASE_BUDGET_MS)
            {
                fprintf(stderr, "[board0] FAIL: board 1's reboot was never detected "
                                 "(peer sessions=%u)\n", lora_crypto_peer_session_count(1u));
                return 1;
            }
        }
        if (board0_deliver_roster(ROSTER1_X, ROSTER1_Y, "after board 1's reboot"))
        {
            return 1;
        }
        board0_run_until_peer_exits();
        if (mission_link_task_peer_reboot_count() != 1u)
        {
            fprintf(stderr, "[board0] FAIL: counted %u peer reboots, want 1\n",
                    mission_link_task_peer_reboot_count());
            return 1;
        }
        if (g_cbba_traffic && lora_crypto_next_tx_counter() <= CBBA_TX_COUNTER_FLOOR)
        {
            fprintf(stderr, "[board0] FAIL: TX counter %u never passed %u, so the re-sync "
                             "past the old 16-bit counter wasn't exercised\n",
                    lora_crypto_next_tx_counter(), CBBA_TX_COUNTER_FLOOR);
            return 1;
        }
        fprintf(stderr, "[board0] PASS: detected board 1's reboot once and re-delivered\n");
        return 0;
    }

    /* Board 0 reboots. First raise board 1's floor for sender 0. */
    {
        static const uint8_t filler[2] = {0xEEu, 0u};
        uint32_t sent = 0u;

        clock_gettime(CLOCK_MONOTONIC, &t);
        while (sent < BOARD0_FLOOR_FILLER_FRAMES)
        {
            if (comms_transport_send(0u, ROSTER_TEAM, LORA_FRAME_TYPE_MISSION_LINK,
                                     COMMS_PRECEDENCE_PRIORITY, filler,
                                     (uint8_t)sizeof(filler)) == 0)
            {
                sent++;
            }
            board_step();
            if (elapsed_ms(&t) > PHASE_BUDGET_MS)
            {
                fprintf(stderr, "[board0] FAIL: could only queue %u filler frames\n", sent);
                return 1;
            }
        }
        /* Drain the queue so board 1 has actually seen them. */
        clock_gettime(CLOCK_MONOTONIC, &t);
        while (elapsed_ms(&t) < 500u)
        {
            board_step();
        }
    }

    board_power_on(0u);
    laptop_send_req(); /* the laptop's keepalive Req reaches the rebooted board */

    if (board0_deliver_roster(ROSTER2_X, ROSTER2_Y, "after own reboot"))
    {
        return 1;
    }
    board0_run_until_peer_exits();
    fprintf(stderr, "[board0] PASS: rebooted and reached board 1 again\n");
    return 0;
}

int main(int argc, char **argv)
{
    int sockets[2];
    int child_status = 0;
    int board0_rc;
    pid_t child;

    if (argc != 3 ||
        (strcmp(argv[1], "board1") != 0 && strcmp(argv[1], "board0") != 0) ||
        (strcmp(argv[2], "quiet") != 0 && strcmp(argv[2], "cbba") != 0))
    {
        fprintf(stderr, "usage: %s <board1|board0> <quiet|cbba>\n", argv[0]);
        return 2;
    }
    g_mode_board0 = strcmp(argv[1], "board0") == 0;
    g_cbba_traffic = strcmp(argv[2], "cbba") == 0;

    /* A peer that has exited just means TX lands nowhere, as on a radio. */
    signal(SIGPIPE, SIG_IGN);

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0)
    {
        perror("socketpair");
        return 1;
    }

    child = fork();
    if (child < 0)
    {
        perror("fork");
        return 1;
    }
    if (child == 0)
    {
        close(sockets[0]);
        g_socket_fd = sockets[1];
        make_nonblocking(g_socket_fd);
        _exit(run_board1());
    }

    close(sockets[1]);
    g_socket_fd = sockets[0];
    make_nonblocking(g_socket_fd);
    board0_rc = run_board0();
    /* Unblocks a child still waiting on data if board 0 gave up early. */
    close(sockets[0]);

    if (waitpid(child, &child_status, 0) < 0)
    {
        perror("waitpid");
        return 1;
    }

    if (board0_rc != 0 || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
    {
        fprintf(stderr, "[harness] FAIL: %s reboot, %s traffic\n", argv[1], argv[2]);
        return 1;
    }
    fprintf(stderr, "[harness] PASS: %s reboot, %s traffic\n", argv[1], argv[2]);
    return 0;
}

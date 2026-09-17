/*
 * Two-board MISSION harness: the laptop-as-clock-master loop, end to end
 * through the real board code.
 *
 * Same two-process setup as two_board_reboot_harness.c (board 0 is the
 * parent, board 1 the child, joined by a Unix socket standing in for the
 * radio; read two_board_harness.c's top comment for why two processes).
 * The code under test is unmodified core/ plus one board's
 * mission_link_task.c.
 *
 * Passes when:
 *   1. a MissionLinkClock_t the "laptop" writes to board 0's USART1 is
 *      applied on board 0 AND relayed over the radio to board 1 - the only
 *      way board 1 is ever allowed to run a tick (core/mission_link.h
 *      section 5b);
 *   2. a later, LOWER clock tick replaces the first on both boards (latest
 *      wins, so a restarted laptop mission is followed);
 *   3. a route map row and the safe zone/gateway sites the laptop writes
 *      to board 0 are applied on board 1 too (sections 5c/5d);
 *   4. board 1 moves one of its teams, and board 0 writes that team's
 *      MissionLinkStatus_t - schema v3, with the new position - to USART1
 *      for the laptop.
 *
 * Exit code 0 only if both processes pass. make check-two-board-mission-
 * harness runs it against both boards' mission_link_task.c.
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
#include "environment.h"

extern void mission_link_task_init(void);
extern void mission_link_task_step(void);

#define PHASE_BUDGET_MS 20000u

#define MOVING_TEAM   6u   /* hosted on board 1 */
#define ROSTER_X      4
#define ROSTER_Y      5
#define MOVED_X       21
#define MOVED_Y       9
#define CLOCK_TICK_1  7u
#define CLOCK_TICK_2  3u   /* lower on purpose - see (2) above */
#define MAP_ROW       12u
#define MAP_WALL_X    20   /* the one blocked cell in MAP_ROW */
#define GATEWAY2_X    33
#define GATEWAY2_Y    34

static int g_socket_fd = -1;
static int g_peer_closed;

/* ---- radio + USART1 stand-ins (strong over the weak defaults) ---- */

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

static uint8_t g_usart1_in[256];
static uint32_t g_usart1_in_len;
static uint32_t g_usart1_in_pos;

int board_usart1_read_byte(void)
{
    return g_usart1_in_pos < g_usart1_in_len ? (int)g_usart1_in[g_usart1_in_pos++] : -1;
}

static void usart1_inject(const uint8_t *bytes, uint32_t len)
{
    if (g_usart1_in_pos == g_usart1_in_len)
    {
        g_usart1_in_pos = 0u;
        g_usart1_in_len = 0u;
    }
    if (g_usart1_in_len + len <= sizeof(g_usart1_in))
    {
        memcpy(g_usart1_in + g_usart1_in_len, bytes, len);
        g_usart1_in_len += len;
    }
}

/* Everything board 0 writes toward the laptop, as a sliding window. */
static uint8_t g_usart1_out[4096];
static uint32_t g_usart1_out_len;

void board_usart1_write(const uint8_t *bytes, uint32_t len)
{
    uint32_t i;

    for (i = 0u; i < len; i++)
    {
        if (g_usart1_out_len == sizeof(g_usart1_out))
        {
            memmove(g_usart1_out, g_usart1_out + sizeof(g_usart1_out) / 2u,
                    sizeof(g_usart1_out) / 2u);
            g_usart1_out_len = sizeof(g_usart1_out) / 2u;
        }
        g_usart1_out[g_usart1_out_len++] = bytes[i];
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
            consumed = 1u;
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
    lora_crypto_reset();
    g_comms_transport_this_board_id = board_id;
    comms_transport_init();
    lora_crypto_set_tx_epoch(fresh_epoch());
    mission_link_task_init();
    mission_link_handshake_board_init(board_id);
    mission_link_handshake_mark_local_ready();
}

static void board_step(void)
{
    rx_pump();
    mission_link_task_step();
    comms_transport_dispatch_pending();
    /* Board 1 would be in its tick loop; nothing else reads CBBA/fault
       frames here, so keep them from filling the inbox. */
    (void)comms_transport_discard_received_except(
        COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_HANDSHAKE) |
        COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_MISSION_LINK));
}

/* ---- laptop-side frames ---- */

static void laptop_send_req(void)
{
    MissionLinkHandshakeReq_t req;
    uint8_t bytes[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE];

    req.board_id = 0u;
    req.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
    usart1_inject(bytes, mission_link_handshake_pack_req(&req, bytes));
}

static void laptop_send_roster(uint8_t team_id, int16_t loc_x, int16_t loc_y)
{
    MissionLinkRoster_t roster;
    uint8_t bytes[MISSION_LINK_ROSTER_WIRE_SIZE];

    memset(&roster, 0, sizeof(roster));
    roster.team_id = team_id;
    roster.loc_x = loc_x;
    roster.loc_y = loc_y;
    roster.stamina = 80u;
    usart1_inject(bytes, mission_link_pack_roster(&roster, bytes));
}

static void laptop_send_map_and_sites(void)
{
    MissionLinkMapRow_t row;
    MissionLinkSites_t sites;
    uint8_t row_bytes[MISSION_LINK_MAP_ROW_WIRE_SIZE];
    uint8_t site_bytes[MISSION_LINK_SITES_WIRE_SIZE];

    memset(&row, 0, sizeof(row));
    row.row = MAP_ROW;
    memset(row.cells, 3, sizeof(row.cells));
    row.cells[MAP_WALL_X] = MISSION_LINK_MAP_BLOCKED;
    usart1_inject(row_bytes, mission_link_pack_map_row(&row, row_bytes));

    memset(&sites, 0, sizeof(sites));
    sites.num_safe_zones = 1u;
    sites.safe_zones[0].loc_x = 7;
    sites.safe_zones[0].loc_y = 8;
    sites.safe_zones[0].capacity = 30u;
    sites.gateway_x[2] = GATEWAY2_X;
    sites.gateway_y[2] = GATEWAY2_Y;
    usart1_inject(site_bytes, mission_link_pack_sites(&sites, site_bytes));
}

static int map_and_sites_landed(void)
{
    uint8_t cell;

    return mission_link_sites_received() &&
           g_evacuation_gateways[2].loc_x == GATEWAY2_X &&
           g_evacuation_gateways[2].loc_y == GATEWAY2_Y &&
           mission_link_map_cell(MAP_WALL_X, MAP_ROW, &cell) && cell == MISSION_LINK_MAP_BLOCKED &&
           mission_link_map_cell(0, MAP_ROW, &cell) && cell == 3u;
}

static void laptop_send_clock(uint32_t tick)
{
    MissionLinkClock_t clock;
    uint8_t bytes[MISSION_LINK_CLOCK_WIRE_SIZE];

    clock.tick = tick;
    usart1_inject(bytes, mission_link_pack_clock(&clock, bytes));
}

/* A status frame for team_id at (x, y) in what board 0 wrote to USART1. */
static int laptop_saw_status(uint8_t team_id, int16_t x, int16_t y)
{
    uint32_t i;

    for (i = 0u; i + MISSION_LINK_STATUS_WIRE_SIZE <= g_usart1_out_len; i++)
    {
        MissionLinkStatus_t status;

        if (mission_link_parse_status(&g_usart1_out[i], g_usart1_out_len - i, &status) == 1 &&
            status.team_id == team_id && status.loc_x == x && status.loc_y == y)
        {
            return 1;
        }
    }
    return 0;
}

/* ---- board 1 (child) ---- */

static int board1_wait_for_clock(uint32_t tick, const char *what)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!(mission_link_clock_received() && mission_link_authorized_tick() == tick))
    {
        board_step();
        if (g_peer_closed || elapsed_ms(&t) > PHASE_BUDGET_MS)
        {
            fprintf(stderr, "[board1] FAIL: %s never arrived over the radio "
                             "(clock_received=%u tick=%u)\n", what,
                    mission_link_clock_received(), mission_link_authorized_tick());
            return 1;
        }
    }
    return 0;
}

static int run_board1(void)
{
    struct timespec t;
    uint32_t start_tick;

    board_power_on(1u);

    if (board1_wait_for_clock(CLOCK_TICK_1, "first clock tick"))
    {
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!map_and_sites_landed())
    {
        board_step();
        if (g_peer_closed || elapsed_ms(&t) > PHASE_BUDGET_MS)
        {
            fprintf(stderr, "[board1] FAIL: map row / sites never arrived over the radio "
                             "(rows=%u sites=%u)\n", mission_link_map_rows_received(),
                    mission_link_sites_received());
            return 1;
        }
    }
    if (environment_cell_is_navigable(MAP_WALL_X, MAP_ROW) || environment_cell_travel_cost(0, MAP_ROW) != 3u)
    {
        fprintf(stderr, "[board1] FAIL: routing does not read the relayed map row\n");
        return 1;
    }
    /* What main_board.c's tick loop does with it: board 1 at tick 0 may
       run from 0, and was held before the clock arrived. */
    if (!mission_link_clock_step_plan(0u, mission_link_authorized_tick(), &start_tick) ||
        start_tick != CLOCK_TICK_1 - 1u)
    {
        fprintf(stderr, "[board1] FAIL: tick %u behind by %u should jump to %u, plan gave %u\n",
                0u, CLOCK_TICK_1, CLOCK_TICK_1 - 1u, start_tick);
        return 1;
    }

    if (board1_wait_for_clock(CLOCK_TICK_2, "lower clock tick"))
    {
        return 1;
    }

    /* The team moves; its board reports the new cell. */
    g_teams[MOVING_TEAM].loc_x = MOVED_X;
    g_teams[MOVING_TEAM].loc_y = MOVED_Y;
    g_teams[MOVING_TEAM].status = STATUS_EN_ROUTE;

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!g_peer_closed && elapsed_ms(&t) < PHASE_BUDGET_MS)
    {
        board_step();
    }
    if (!g_peer_closed)
    {
        fprintf(stderr, "[board1] FAIL: board 0 never confirmed the moved team's report\n");
        return 1;
    }
    fprintf(stderr, "[board1] PASS: clock (latest tick wins), map and sites relayed, position reported\n");
    return 0;
}

/* ---- board 0 (parent) ---- */

static int run_board0(void)
{
    struct timespec t;

    board_power_on(0u);
    laptop_send_req();

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!mission_link_handshake_relay_reply_seen(1u))
    {
        board_step();
        if (g_peer_closed || elapsed_ms(&t) > PHASE_BUDGET_MS)
        {
            fprintf(stderr, "[board0] FAIL: never received board 1's Reply\n");
            return 1;
        }
    }

    laptop_send_roster(MOVING_TEAM, ROSTER_X, ROSTER_Y);
    laptop_send_map_and_sites();
    laptop_send_clock(CLOCK_TICK_1);

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (mission_link_authorized_tick() != CLOCK_TICK_1)
    {
        board_step();
        if (g_peer_closed || elapsed_ms(&t) > PHASE_BUDGET_MS)
        {
            fprintf(stderr, "[board0] FAIL: clock from USART1 never applied\n");
            return 1;
        }
    }

    /* Give the first relay time to land before it is superseded, then the
       laptop "restarts" at a lower tick. */
    clock_gettime(CLOCK_MONOTONIC, &t);
    while (elapsed_ms(&t) < 300u)
    {
        board_step();
    }
    laptop_send_clock(CLOCK_TICK_2);

    clock_gettime(CLOCK_MONOTONIC, &t);
    while (!laptop_saw_status(MOVING_TEAM, MOVED_X, MOVED_Y))
    {
        board_step();
        if (g_peer_closed || elapsed_ms(&t) > 2u * PHASE_BUDGET_MS)
        {
            fprintf(stderr, "[board0] FAIL: no v3 status for team %u at (%d,%d) reached USART1 "
                             "(clock tick here %u)\n", MOVING_TEAM, MOVED_X, MOVED_Y,
                    mission_link_authorized_tick());
            return 1;
        }
    }
    if (mission_link_authorized_tick() != CLOCK_TICK_2)
    {
        fprintf(stderr, "[board0] FAIL: lower clock tick did not replace the first (%u)\n",
                mission_link_authorized_tick());
        return 1;
    }

    if (!map_and_sites_landed())
    {
        fprintf(stderr, "[board0] FAIL: map row / sites from USART1 not applied on board 0\n");
        return 1;
    }

    fprintf(stderr, "[board0] PASS: clock, map and sites applied and relayed, moved team's position reported\n");
    return 0;
}

int main(void)
{
    int sockets[2];
    int child_status = 0;
    int board0_rc;
    pid_t child;

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
    close(sockets[0]); /* tells board 1 it is done */

    if (waitpid(child, &child_status, 0) < 0)
    {
        perror("waitpid");
        return 1;
    }

    if (board0_rc != 0 || !WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)
    {
        fprintf(stderr, "[harness] FAIL\n");
        return 1;
    }
    fprintf(stderr, "[harness] PASS\n");
    return 0;
}

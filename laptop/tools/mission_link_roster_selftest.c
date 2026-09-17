/*
 * Board-side roster handling in board/STM32N6570-DK/mission_link_task.c
 * (the Pico copy is identical code), driven through USART1 exactly as the
 * laptop's mission feed sends it. No hardware.
 *
 * Covers:
 *   1. A rejected roster (location outside the grid) is counted as a
 *      reject and does NOT count as "a roster arrived" - it used to, which
 *      let a board's boot wait end on data it had just thrown away.
 *   2. The first valid roster for a team is applied and counted.
 *   3. A repeat roster for the same team is ignored: the laptop re-sends
 *      rosters, and re-applying one mid-mission would reset the team.
 *   4. An out-of-range team_id is rejected, not relayed.
 *   5. mission_link_task_init() (a reboot) clears the one-per-team latch.
 *
 * Build/run: make check-mission-link-roster-selftest
 */

#include <stdio.h>
#include <string.h>

#include "mission_state.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_link_handshake.h"
#include "comms_transport.h"

extern void mission_link_task_init(void);
extern void mission_link_task_step(void);
extern uint32_t mission_link_task_any_roster_applied(void);

static int g_failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++; \
        } \
    } while (0)

/* ---- USART1 input: strong override of mission_link_task.c's weak stub ---- */

static uint8_t g_usart1_in[256];
static uint32_t g_usart1_len;
static uint32_t g_usart1_pos;

int board_usart1_read_byte(void)
{
    if (g_usart1_pos >= g_usart1_len)
    {
        return -1;
    }
    return g_usart1_in[g_usart1_pos++];
}

static void usart1_inject(const uint8_t *bytes, uint32_t len)
{
    memcpy(&g_usart1_in[g_usart1_len], bytes, len);
    g_usart1_len += len;
}

static void laptop_send_roster(uint8_t team_id, int16_t loc_x, int16_t loc_y)
{
    MissionLinkRoster_t roster;
    uint8_t wire[MISSION_LINK_ROSTER_WIRE_SIZE];

    memset(&roster, 0, sizeof(roster));
    roster.team_id = team_id;
    roster.loc_x = loc_x;
    roster.loc_y = loc_y;
    roster.stamina = 77u;
    usart1_inject(wire, mission_link_pack_roster(&roster, wire));
    mission_link_task_step();
}

static void board0_boot_to_ready(void)
{
    MissionLinkHandshakeReq_t req;
    uint8_t wire[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE];
    MissionLinkHandshakeReply_t reply;

    g_usart1_len = 0u;
    g_usart1_pos = 0u;
    g_comms_transport_this_board_id = 0u;
    comms_transport_init();
    mission_link_task_init();
    mission_link_handshake_board_init(0u);
    mission_link_handshake_mark_local_ready();

    req.board_id = 0u;
    req.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
    usart1_inject(wire, mission_link_handshake_pack_req(&req, wire));
    mission_link_task_step();
    CHECK(mission_link_handshake_build_local_reply(&reply) != 0);
}

int main(void)
{
    uint32_t rejects;
    const uint8_t team = 2u; /* hosted on board 0 in every board split */

    board0_boot_to_ready();
    CHECK(mission_link_task_any_roster_applied() == 0u);

    /* 1. rejected roster: counted as a reject, not as an arrival */
    g_teams[team].loc_x = 1;
    g_teams[team].loc_y = 1;
    rejects = mission_link_roster_reject_count();
    laptop_send_roster(team, (int16_t)(GRID_WIDTH + 5), 3);
    CHECK(mission_link_roster_reject_count() == rejects + 1u);
    CHECK(mission_link_task_any_roster_applied() == 0u);
    CHECK(g_teams[team].loc_x == 1 && g_teams[team].loc_y == 1);

    /* 2. first valid roster applies */
    laptop_send_roster(team, 10, 11);
    CHECK(mission_link_task_any_roster_applied() == 1u);
    CHECK(g_teams[team].loc_x == 10 && g_teams[team].loc_y == 11);
    CHECK(g_teams[team].stamina == 77.0f);

    /* 3. repeat roster for the same team is ignored */
    g_teams[team].stamina = 40.0f; /* mid-mission state the repeat must not reset */
    laptop_send_roster(team, 20, 21);
    CHECK(mission_link_task_any_roster_applied() == 1u);
    CHECK(g_teams[team].loc_x == 10 && g_teams[team].loc_y == 11);
    CHECK(g_teams[team].stamina == 40.0f);

    /* 4. out-of-range team rejected */
    rejects = mission_link_roster_reject_count();
    laptop_send_roster(200u, 5, 5);
    CHECK(mission_link_roster_reject_count() == rejects + 1u);
    CHECK(mission_link_task_any_roster_applied() == 1u);

    /* 5. a reboot clears the latch - the re-sent roster applies again */
    board0_boot_to_ready();
    CHECK(mission_link_task_any_roster_applied() == 0u);
    laptop_send_roster(team, 20, 21);
    CHECK(mission_link_task_any_roster_applied() == 1u);
    CHECK(g_teams[team].loc_x == 20 && g_teams[team].loc_y == 21);

    if (g_failures == 0)
    {
        printf("mission_link_roster_selftest: all checks passed\n");
        return 0;
    }
    printf("mission_link_roster_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

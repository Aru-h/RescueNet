/*
 * Single-board DK harness: the submission target (all 10 teams on one
 * STM32N6570-DK, virtual bus + consensus enabled) proven end to end
 * through the REAL board code - board/STM32N6570-DK/mission_link_task.c
 * and the core sources, completely unmodified, built with DK_SINGLE_BOARD_FLAGS
 * (Makefile: -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS
 * -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1).
 *
 * Shape borrowed from laptop/tools/two_board_mission_harness.c (a
 * synthetic "laptop" injecting USART1 bytes via board_usart1_read_byte()/
 * board_usart1_write() overrides, driving the real mission_link_task.c),
 * but with ONE crucial difference: there is no second board and no
 * radio to simulate, so there is no socket and no forked child here at
 * all. Instead, lora_spi_transmit() - the weak seam core/comms_
 * transport.c calls at the exact point it physically transmits a frame
 * (see that function's own doc) - is overridden here to FAIL the test
 * if it is EVER called. That is the single decisive assertion this
 * harness exists for: once comms_transport_board_count() == 1, every
 * "local vs relay" decision in mission_link_task.c and core/mission_
 * link_override.c must take the local branch, so nothing should ever
 * reach the radio TX path - not the handshake, not a roster, not a
 * fault, not an override, not a status heartbeat, not a CBBA/fault-
 * comms mesh frame, for the whole run.
 *
 * Passes when, over BOARD_TICKS ticks with all 10 teams hosted locally:
 *   1. this board's own handshake completes with zero radio traffic and
 *      no peer board (mission_link_handshake_build_local_reply()
 *      becomes true from nothing but this board's own local-ready latch
 *      plus the laptop's Req over USART1);
 *   2. all 10 rosters, all 36 map rows and the sites apply locally
 *      (mission_link_task_any_roster_applied() reaches NUM_TEAMS,
 *      mission_link_map_rows_received()/mission_link_sites_received()
 *      both land, and routing reads the relayed map);
 *   3. a fault applied mid-run reaches g_teams[].fault_state locally;
 *   4. an override applied mid-run acks MISSION_LINK_OVERRIDE_ACK_
 *      APPLIED (never RELAYED/RELAY_FAILED/NOT_READY);
 *   5. the virtual bus and consensus genuinely ran the whole time
 *      (VbusStats_t.sent > 0, .delivered > 0, and .dropped stays within
 *      a sane margin of consensus_init()'s real 5% default link loss -
 *      CONSENSUS_DEFAULT_LINK_LOSS, core/consensus.h - rather than the
 *      much higher rate inbox overflow would produce);
 *   6. lora_spi_transmit() was called exactly zero times.
 *
 * make check-single-board-dk-harness builds and runs this.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mission_state.h"
#include "team.h"
#include "comms_transport.h"
#include "lora_frame.h"
#include "lora_crypto.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_link_handshake.h"
#include "mission_link_override.h"
#include "environment.h"
#include "sim_runner.h"
#include "vbus.h"
#include "fault_config.h"

extern void mission_link_task_init(void);
extern void mission_link_task_step(void);
extern uint8_t g_comms_transport_this_board_id;
extern uint32_t mission_link_task_any_roster_applied(void);

#define BOARD_TICKS       500u
#define BOOT_WAIT_MAX     100000u
#define MAP_TRAVEL_COST   3u
#define FAULT_TEAM        4u
#define OVERRIDE_TEAM     7u
#define OVERRIDE_INCIDENT 12345u

/* ---------------------------------------------------------------
 * The one assertion this whole harness exists to make: a real,
 * non-weak lora_spi_transmit() that fails the run if comms_transport.c
 * ever reaches it (see that function's own doc in core/comms_
 * transport.c for why this is the actual physical-TX seam, not a
 * proxy for it).
 * ---------------------------------------------------------------
 */
static int g_relay_attempted = 0;
static uint32_t g_relay_attempted_count = 0;

int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len)
{
    (void)frame_bytes;
    (void)frame_len;
    g_relay_attempted = 1;
    g_relay_attempted_count++;
    return 0;
}

/* ---- USART1 stand-in (single process: no socket, no peer) ---- */
static uint8_t g_usart1_in[16384];
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

static uint8_t g_usart1_out[16384];
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

/* ---- laptop-side frame builders (mirrors two_board_mission_harness.c) ---- */

static void send_req(void)
{
    MissionLinkHandshakeReq_t req;
    uint8_t bytes[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE];

    req.board_id = 0u;
    req.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
    usart1_inject(bytes, mission_link_handshake_pack_req(&req, bytes));
}

static void send_roster(uint8_t team_id, int16_t x, int16_t y)
{
    MissionLinkRoster_t roster;
    uint8_t bytes[MISSION_LINK_ROSTER_WIRE_SIZE];

    memset(&roster, 0, sizeof(roster));
    roster.team_id = team_id;
    roster.loc_x = x;
    roster.loc_y = y;
    roster.stamina = 90u;
    roster.equipment_level = 50u;
    usart1_inject(bytes, mission_link_pack_roster(&roster, bytes));
}

static void send_all_map_rows(void)
{
    uint32_t row;

    for (row = 0u; row < MISSION_LINK_MAP_HEIGHT; row++)
    {
        MissionLinkMapRow_t r;
        uint8_t bytes[MISSION_LINK_MAP_ROW_WIRE_SIZE];

        memset(&r, 0, sizeof(r));
        r.row = (uint8_t)row;
        memset(r.cells, MAP_TRAVEL_COST, sizeof(r.cells));
        usart1_inject(bytes, mission_link_pack_map_row(&r, bytes));
    }
}

static void send_sites(void)
{
    MissionLinkSites_t sites;
    uint8_t bytes[MISSION_LINK_SITES_WIRE_SIZE];

    memset(&sites, 0, sizeof(sites));
    sites.num_safe_zones = 1u;
    sites.safe_zones[0].loc_x = 5;
    sites.safe_zones[0].loc_y = 5;
    sites.safe_zones[0].capacity = 40u;
    sites.gateway_x[0] = 10;
    sites.gateway_y[0] = 10;
    usart1_inject(bytes, mission_link_pack_sites(&sites, bytes));
}

static void send_clock(uint32_t tick)
{
    MissionLinkClock_t c;
    uint8_t bytes[MISSION_LINK_CLOCK_WIRE_SIZE];

    c.tick = tick;
    usart1_inject(bytes, mission_link_pack_clock(&c, bytes));
}

static void send_fault(uint8_t team_id)
{
    MissionLinkFault_t f;
    uint8_t bytes[MISSION_LINK_FAULT_WIRE_SIZE];

    memset(&f, 0, sizeof(f));
    f.team_id = team_id;
    f.active[0] = 1u;
    f.severity[0] = 2u;
    usart1_inject(bytes, mission_link_pack_fault(&f, bytes));
}

static void send_override(uint8_t team_id, uint32_t incident_id)
{
    MissionLinkOverride_t o;
    uint8_t bytes[MISSION_LINK_OVERRIDE_WIRE_SIZE];

    o.team_id = team_id;
    o.incident_id = incident_id;
    usart1_inject(bytes, mission_link_override_pack(&o, bytes));
}

/* Every scan below is against a growing g_usart1_out buffer, so a
 * frame is only ever counted once it has actually been written -
 * repeated scans as the run progresses are intentional, not a bug. */
static int saw_override_ack(uint8_t team_id, uint32_t incident_id, uint8_t *out_result)
{
    uint32_t i;
    int found = 0;

    for (i = 0u; i + MISSION_LINK_OVERRIDE_ACK_WIRE_SIZE <= g_usart1_out_len; i++)
    {
        MissionLinkOverrideAck_t ack;

        if (mission_link_override_parse_ack(&g_usart1_out[i], g_usart1_out_len - i, &ack) == 1 &&
            ack.team_id == team_id && ack.incident_id == incident_id)
        {
            *out_result = ack.result;
            found = 1;
        }
    }
    return found;
}

static void board_service(void)
{
    mission_link_task_step();
    comms_transport_dispatch_pending();
}

int main(void)
{
    uint32_t tick;
    uint32_t waited;
    uint8_t team_id;
    uint8_t override_result = 0u;
    VbusStats_t vstats;

    /* ---- boot, mirroring main_board.c's sim_task() bring-up in order,
     * minus the hardware-only pieces (SX1262 SPI regs, USART1 FIFO,
     * the RNG-seeded epoch - none of that is reachable host-side, and
     * none of it is what this harness is proving). ---- */
    memset(g_teams, 0, sizeof(g_teams));
    lora_crypto_reset();
    g_comms_transport_this_board_id = 0u;
    comms_transport_init();
    lora_crypto_set_tx_epoch(0xA5A5A5A5u);
    mission_link_task_init();
    mission_link_handshake_board_init(0u);
    mission_link_handshake_mark_local_ready();

    if (sim_runner_init() != 0)
    {
        fprintf(stderr, "[single-board] FAIL: sim_runner_init (equipment matrix)\n");
        return 1;
    }

    /* The whole premise of DK_SINGLE_BOARD_FLAGS and this harness:
     * every team must resolve to board 0. If this is ever anything
     * else, every assertion below would still nominally "pass" for
     * the wrong reason (a lucky board topology), so check it directly
     * first and fail loudly instead. */
    if (comms_transport_board_count() != 1u)
    {
        fprintf(stderr,
                "[single-board] FAIL: comms_transport_board_count()=%u, expected 1 -- "
                "COMMS_TRANSPORT_TEAMS_PER_BOARD is not resolving to NUM_TEAMS\n",
                (unsigned)comms_transport_board_count());
        return 1;
    }

    /* ---- handshake: no peer, no radio ---- */
    send_req();
    for (waited = 0u; waited < BOOT_WAIT_MAX; waited++)
    {
        MissionLinkHandshakeReply_t scratch;

        board_service();
        if (mission_link_handshake_build_local_reply(&scratch) != 0)
        {
            break;
        }
    }
    {
        MissionLinkHandshakeReply_t scratch;

        if (mission_link_handshake_build_local_reply(&scratch) == 0)
        {
            fprintf(stderr, "[single-board] FAIL: handshake never completed locally (no peer, no radio needed)\n");
            return 1;
        }
    }

    /* ---- roster (all 10 teams), map (all 36 rows), sites ---- */
    for (team_id = 0u; team_id < NUM_TEAMS; team_id++)
    {
        send_roster(team_id, (int16_t)(5 + team_id), (int16_t)(5 + team_id));
    }
    send_all_map_rows();
    send_sites();
    send_clock(1u);

    for (waited = 0u; waited < BOOT_WAIT_MAX; waited++)
    {
        board_service();
        if (mission_link_task_any_roster_applied() >= NUM_TEAMS &&
            mission_link_map_rows_received() >= MISSION_LINK_MAP_HEIGHT &&
            mission_link_sites_received())
        {
            break;
        }
    }
    if (mission_link_task_any_roster_applied() < NUM_TEAMS)
    {
        fprintf(stderr, "[single-board] FAIL: only %u/%u rosters applied locally\n",
                (unsigned)mission_link_task_any_roster_applied(), (unsigned)NUM_TEAMS);
        return 1;
    }
    if (mission_link_map_rows_received() < MISSION_LINK_MAP_HEIGHT || !mission_link_sites_received())
    {
        fprintf(stderr, "[single-board] FAIL: map/sites never fully applied (rows=%u sites=%u)\n",
                (unsigned)mission_link_map_rows_received(), (unsigned)mission_link_sites_received());
        return 1;
    }
    if (!environment_cell_is_navigable(0, 0) || environment_cell_travel_cost(0, 0) != MAP_TRAVEL_COST)
    {
        fprintf(stderr, "[single-board] FAIL: routing does not read the locally-applied map\n");
        return 1;
    }
    for (team_id = 0u; team_id < NUM_TEAMS; team_id++)
    {
        if (g_teams[team_id].loc_x != (int16_t)(5 + team_id) ||
            g_teams[team_id].loc_y != (int16_t)(5 + team_id))
        {
            fprintf(stderr, "[single-board] FAIL: team %u roster not applied to g_teams[]\n",
                    (unsigned)team_id);
            return 1;
        }
    }

    /* ---- the real per-tick loop: same calls main_board.c's sim_task()
     * makes (clock -> mission_link_clock_step_plan -> sim_tick ->
     * service), for BOARD_TICKS ticks with all 10 teams live. ---- */
    sim_start_seed_board(1u, FAULT_PROFILE_NONE);

    for (tick = 1u; tick <= BOARD_TICKS; tick++)
    {
        uint32_t start_tick;
        uint32_t plan_wait;

        send_clock(tick);
        board_service();

        for (plan_wait = 0u;
             plan_wait < BOOT_WAIT_MAX &&
             !mission_link_clock_step_plan(g_current_tick, mission_link_authorized_tick(), &start_tick);
             plan_wait++)
        {
            board_service();
        }
        if (start_tick != g_current_tick)
        {
            g_current_tick = start_tick;
        }

        sim_tick();
        board_service();

        if (tick == 100u)
        {
            send_fault(FAULT_TEAM);
        }
        if (tick == 150u)
        {
            send_override(OVERRIDE_TEAM, OVERRIDE_INCIDENT);
        }
        if (tick == 150u || tick == 151u || tick == 160u)
        {
            /* Give the override a few ticks of servicing to be acked -
             * mission_link_task_step() parses and answers it inline,
             * so in practice one call already suffices, but this loop
             * already calls board_service() every tick anyway. */
        }

        if (g_relay_attempted)
        {
            fprintf(stderr,
                    "[single-board] FAIL: lora_spi_transmit() was called at/before tick %u (%u call(s) total) -- "
                    "some mission-link path took the cross-board relay branch instead of applying locally\n",
                    (unsigned)tick, (unsigned)g_relay_attempted_count);
            return 1;
        }
    }

    /* ---- fault applied locally ---- */
    if (!g_teams[FAULT_TEAM].fault_state.active[0])
    {
        fprintf(stderr, "[single-board] FAIL: fault for team %u never applied locally\n", (unsigned)FAULT_TEAM);
        return 1;
    }

    /* ---- override applied locally, not relayed ---- */
    if (!saw_override_ack(OVERRIDE_TEAM, OVERRIDE_INCIDENT, &override_result))
    {
        fprintf(stderr, "[single-board] FAIL: no override ack for team %u ever reached USART1\n",
                (unsigned)OVERRIDE_TEAM);
        return 1;
    }
    if (override_result != MISSION_LINK_OVERRIDE_ACK_APPLIED)
    {
        fprintf(stderr,
                "[single-board] FAIL: override ack result=%u, expected MISSION_LINK_OVERRIDE_ACK_APPLIED (%u) -- "
                "the override took a relay path instead of applying locally\n",
                (unsigned)override_result, (unsigned)MISSION_LINK_OVERRIDE_ACK_APPLIED);
        return 1;
    }

    /* ---- virtual bus + consensus genuinely ran ---- */
    vstats = vbus_get_stats();
    if (vstats.sent == 0u)
    {
        fprintf(stderr, "[single-board] FAIL: vbus_get_stats().sent == 0 -- consensus_step() never broadcast a beacon\n");
        return 1;
    }
    if (vstats.delivered == 0u)
    {
        fprintf(stderr, "[single-board] FAIL: vbus_get_stats().delivered == 0 -- no peer ever received a beacon\n");
        return 1;
    }
    /* consensus_init()'s default bus config (core/consensus.c) is not
     * loss-free: CONSENSUS_DEFAULT_LINK_LOSS (core/consensus.h) is a
     * real 5% IID per-link loss, decision D4 - "dropped == 0" is only
     * the expected reading for an explicitly loss-free bus config
     * (core/vbus.h's own doc), which this harness does not configure.
     * With 10 teams each broadcasting to 9 peers, expect dropped to
     * land near 5% of attempted deliveries (delivered + dropped); a
     * MUCH higher rate would instead point at inbox overflow (see
     * core/vbus.c's deliver_copy()) - a real capacity bug, not link
     * loss - so this still asserts a sane upper bound rather than
     * accepting any value. */
    {
        uint32_t attempted = vstats.delivered + vstats.dropped;
        uint32_t dropped_pct_x1000 = attempted ? (vstats.dropped * 1000u) / attempted : 0u;

        if (dropped_pct_x1000 > 150u) /* 15% - generous margin above the 5% default */
        {
            fprintf(stderr,
                    "[single-board] FAIL: vbus drop rate %u.%u%% (dropped=%u delivered=%u) is far above "
                    "the ~5%% CONSENSUS_DEFAULT_LINK_LOSS default -- looks like inbox overflow, not link loss\n",
                    (unsigned)(dropped_pct_x1000 / 10u), (unsigned)(dropped_pct_x1000 % 10u),
                    (unsigned)vstats.dropped, (unsigned)vstats.delivered);
            return 1;
        }
    }

    /* ---- final, whole-run relay check ---- */
    if (g_relay_attempted)
    {
        fprintf(stderr, "[single-board] FAIL: lora_spi_transmit() was called %u time(s) over the run\n",
                (unsigned)g_relay_attempted_count);
        return 1;
    }

    sim_end_seed();
    sim_runner_shutdown();

    fprintf(stderr,
            "[single-board] PASS: %u teams local, %u ticks, vbus sent=%u delivered=%u dropped=%u, "
            "fault + override applied locally, zero relay sends\n",
            (unsigned)NUM_TEAMS, (unsigned)BOARD_TICKS, (unsigned)vstats.sent,
            (unsigned)vstats.delivered, (unsigned)vstats.dropped);
    return 0;
}

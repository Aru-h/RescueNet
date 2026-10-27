/*
 * Stream-order test for the board's receive path (the REAL
 * board/STM32N6570-DK/mission_link_task.c, host-built like
 * single_board_dk_harness.c).
 *
 * The laptop sends a tick's changes and then, last, the clock frame that
 * authorises the tick (laptop/mission_feed.c). With the tick-boundary mode on,
 * the board must apply exactly the frames up to and including that clock frame
 * before the tick runs, and nothing behind it, no matter how many times the
 * link step runs or how soon the bytes behind it arrived:
 *
 *   stream:  fault(A)  clock(31)  fault(B)  clock(32)      current tick = 30
 *   step x N        -> A applied, B NOT, authorised tick = 31
 *   tick 31 "runs"  (g_current_tick = 31)
 *   step            -> B applied, authorised tick = 32
 *
 * and, as a control, with the mode off the same stream is applied in one go
 * (the behaviour before the rule existed).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "mission_state.h"
#include "comms_transport.h"
#include "lora_crypto.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_link_handshake.h"
#include "sim_runner.h"

extern void mission_link_task_init(void);
extern void mission_link_task_step(void);
extern void mission_link_task_set_tick_boundary_mode(uint8_t on);
extern uint8_t g_comms_transport_this_board_id;

static int g_fail;
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail = 1; } } while(0)

int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len)
{
    (void)frame_bytes;
    (void)frame_len;
    return 0;
}

static uint8_t g_in[4096];
static uint32_t g_in_len;
static uint32_t g_in_pos;

int board_usart1_read_byte(void)
{
    return g_in_pos < g_in_len ? (int)g_in[g_in_pos++] : -1;
}

void board_usart1_write(const uint8_t *bytes, uint32_t len)
{
    (void)bytes;
    (void)len;
}

static void inject(const uint8_t *bytes, uint32_t len)
{
    if (g_in_pos == g_in_len)
    {
        g_in_pos = 0u;
        g_in_len = 0u;
    }
    memcpy(g_in + g_in_len, bytes, len);
    g_in_len += len;
}

static void send_fault(uint8_t team_id)
{
    MissionLinkFault_t f;
    uint8_t bytes[MISSION_LINK_FAULT_WIRE_SIZE];

    memset(&f, 0, sizeof(f));
    f.team_id = team_id;
    f.active[0] = 1u;
    f.severity[0] = 2u;
    inject(bytes, mission_link_pack_fault(&f, bytes));
}

static void send_clock(uint32_t tick)
{
    MissionLinkClock_t c;
    uint8_t bytes[MISSION_LINK_CLOCK_WIRE_SIZE];

    c.tick = tick;
    inject(bytes, mission_link_pack_clock(&c, bytes));
}

static void service(uint32_t times)
{
    while (times-- > 0u)
    {
        mission_link_task_step();
        comms_transport_dispatch_pending();
    }
}

static int fault_applied(uint8_t team)
{
    return g_teams[team].fault_state.active[0] != 0u;
}

int main(void)
{
    MissionLinkHandshakeReq_t req;
    uint8_t bytes[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE];
    uint32_t i;

    memset(g_teams, 0, sizeof(g_teams));
    lora_crypto_reset();
    g_comms_transport_this_board_id = 0u;
    comms_transport_init();
    lora_crypto_set_tx_epoch(0xA5A5A5A5u);
    mission_link_task_init();
    mission_link_handshake_board_init(0u);
    mission_link_handshake_mark_local_ready();
    (void)sim_runner_init();

    req.board_id = 0u;
    req.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
    inject(bytes, mission_link_handshake_pack_req(&req, bytes));
    for (i = 0u; i < 1000u; i++)
    {
        MissionLinkHandshakeReply_t scratch;

        service(1u);
        if (mission_link_handshake_build_local_reply(&scratch) != 0)
        {
            break;
        }
    }
    CHECK(i < 1000u); /* handshake completed: frames are applied from here on */

    /* ---- control: mode off, the whole stream is applied in one step ---- */
    mission_link_task_set_tick_boundary_mode(0u);
    g_current_tick = 20u;
    send_fault(5u);
    send_clock(21u);
    send_fault(6u);
    send_clock(22u);
    service(1u);
    CHECK(fault_applied(5u) && fault_applied(6u));
    CHECK(mission_link_authorized_tick() == 22u);

    /* ---- mode on: stop at the authorisation ---- */
    mission_link_task_set_tick_boundary_mode(1u);
    g_current_tick = 30u; /* past the control's authorised tick (22) */
    send_fault(3u);
    send_clock(31u);
    send_fault(4u);
    send_clock(32u);

    service(1u);
    CHECK(fault_applied(3u));            /* before clock(31): input to tick 31 */
    CHECK(!fault_applied(4u));           /* behind clock(31): input to tick 32 */
    CHECK(mission_link_authorized_tick() == 31u);

    service(50u);                        /* more steps change nothing */
    CHECK(!fault_applied(4u));
    CHECK(mission_link_authorized_tick() == 31u);

    g_current_tick = 31u;                /* the sim ran tick 31 */
    service(1u);
    CHECK(fault_applied(4u));
    CHECK(mission_link_authorized_tick() == 32u);

    g_current_tick = 32u;
    service(5u);
    CHECK(mission_link_authorized_tick() == 32u);

    if (g_fail)
    {
        return 1;
    }
    printf("PASS: frames are applied in stream order and stop at each tick authorisation\n");
    return 0;
}

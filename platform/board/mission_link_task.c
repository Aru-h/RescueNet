/*
 * Board-side driver for the mission-link protocol (core/mission_link.h,
 * core/mission_link.c). STM32N6570-DK, µT-Kernel 3.0 BSP2 - mirrors
 * platform/board/snapshot.c's role for telemetry_frame.h: this file
 * owns the platform-specific I/O (USART1 bytes, comms_transport
 * calls), core/mission_link.c owns the portable pack/parse/apply
 * logic, same split as that file has with core/telemetry_frame.h.
 *
 * Two channels, two very different jobs for this file, matching the
 * task-doc's own two-channel split:
 *
 *   1. USART1 (laptop <-> this board, board 0 ONLY): the DK is the
 *      one board with a laptop-facing UART. RX: accumulate raw bytes,
 *      scan for a valid roster/incident/fault frame per the resync
 *      policy documented in core/mission_link_codec.h (try each
 *      receivable type at the front of the buffer; if none match,
 *      drop one byte and retry - same policy tools/telemetry_bridge.py
 *      already applies for telemetry_frame.h's single frame type,
 *      just spread across three types here). TX: this board's own
 *      status messages, plus any status message uplinked from board 1
 *      over comms_transport (see channel 2 below) - MissionLinkStatus_t
 *      is the one type this board only ever sends toward the laptop,
 *      never receives (mission_link.h's own doc).
 *
 *   2. comms_transport / LoRa (this board <-> other boards): every
 *      board runs this half, DK included. RX: drain comms_transport_
 *      poll_received() for LORA_FRAME_TYPE_MISSION_LINK frames, read
 *      the MISSION_LINK_MSG_* sub-type byte mission_link_codec.h
 *      defines, dispatch to the matching payload-only parse + apply.
 *      A STATUS sub-type here can only mean an uplink from another
 *      board (this board never applies status locally - it forwards
 *      it out over USART1 if it's the DK; a status frame arriving
 *      here on a non-DK board would mean the topology assumptions
 *      below are wrong, and is dropped rather than mis-handled).
 *      TX: when USART1 RX (channel 1, DK only) parses a roster/fault
 *      message addressed to a team this board doesn't host, or any
 *      incident message (which has no single addressed team - see
 *      "Incident relay has no single destination" below), relay it
 *      onward via comms_transport_send() instead of applying locally.
 *
 * Not a real µT-Kernel task (no tk_cre_tsk here): every other piece
 * of board-platform I/O this codebase has (board_snapshot_ring_drain()
 * in snapshot.c) is a plain function main_board.c's single sim_task
 * calls periodically, not its own kernel task - see main_board.c's
 * BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL comment for why (avowebrap kernel
 * task-switch overhead for something with no need to run
 * independently of the sim tick - a dedicated kernel task buys nothing
 * here and only adds task-switch overhead). This file follows the
 * same pattern: mission_link_task_step() is meant to be called from
 * that same loop.
 * The "_task.c" filename names the RESPONSIBILITY (the mission-link
 * RX/TX task described in the split doc), not a literal T_CTSK - same
 * as how snapshot.c's ring drain fulfils its "task" without one.
 *
 * ---------------------------------------------------------------
 * Handoff notes for main_board.c (intentionally not touched here -
 * same rule as Prompt 2 followed for its own hook)
 * ---------------------------------------------------------------
 * 1. Before the seed loop starts (once, after comms_transport_init()
 *    - see core/comms_transport.h's own handoff note 1 for the
 *    g_comms_transport_this_board_id-then-comms_transport_init()
 *    ordering this depends on, still pending wiring itself):
 *        mission_link_task_init();
 * 2. Inside the per-tick loop, alongside the existing snapshot-ring
 *    drain call - every tick is fine here (unlike the snapshot ring,
 *    there's no large backlog to bound; each call does a handful of
 *    non-blocking reads/polls and returns):
 *        mission_link_task_step();
 * 3. Both of the above are, like board_snapshot_ring_drain() already
 *    is in this same file's neighbor, not part of any portable core/
 *    header's public interface - forward-declare them at their call
 *    sites in main_board.c the same way board_snapshot_ring_drain()
 *    already is there, e.g.:
 *        extern void mission_link_task_init(void);
 *        extern void mission_link_task_step(void);
 */

#include <tm/tmonitor.h>

#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_state.h"
#include "comms_transport.h"
#include "lora_frame.h"

/*
 * USART1 byte-level RX hook - NOT implemented here, same status as
 * comms_transport.c's lora_spi_transmit(): the actual UART RX driver
 * (interrupt or DMA ring feeding this) is separate, still-pending
 * bring-up work. This weak default lets this file (and anything that
 * links against it) build and run standalone before that driver
 * exists, exactly like lora_spi_transmit's weak default does for the
 * SPI/LoRa side. Once a real, strong (non-weak) board_usart1_read_
 * byte() is linked in from the UART driver's own .c file, the linker
 * resolves every call to that definition automatically - nothing here
 * needs to change.
 *
 * Returns the next unread byte (0-255) if one is available, or -1 if
 * not - non-blocking, must never wait for a byte that hasn't arrived
 * (this is polled from the same loop driving the sim tick).
 */
extern int board_usart1_read_byte(void);

__attribute__((weak)) int board_usart1_read_byte(void)
{
    return -1;
}

/*
 * Writes one byte to USART1 (laptop-facing). tm_putchar is the same
 * primitive platform/board/snapshot.c's ring drain already uses for
 * raw binary frame bytes on this wire - see that file's top-of-file
 * comment for why tm_putchar (raw byte) and not tm_printf (text) is
 * the right call here.
 */
static void mission_link_usart1_write(const uint8_t *bytes, uint32_t len)
{
    uint32_t i;

    for (i = 0u; i < len; i++)
    {
        (void)tm_putchar((INT)bytes[i]);
    }
}

/* ---------------------------------------------------------------
 * Channel 1: USART1 RX (board 0 only) - accumulate + resync/parse
 * ---------------------------------------------------------------
 * Small flat buffer, not a ring: at BOARD_TICK_DELAY_MS=0 (flat-out
 * bring-up, per main_board.c's own default) this is drained every
 * tick, so it never needs to hold more than a handful of bytes -
 * matches the sizing reasoning platform/board/snapshot.c's ring
 * capacity comment already gives for a similar "drained often enough
 * that backlog stays small" case, just smaller here since there's no
 * equivalent overflow policy to design for.
 */
#ifndef MISSION_LINK_RX_BUF_CAPACITY
#define MISSION_LINK_RX_BUF_CAPACITY 64u
#endif

static uint8_t g_rx_buf[MISSION_LINK_RX_BUF_CAPACITY];
static uint32_t g_rx_buf_len;

/* Board-topology helper: which team id to address to reach `board_id`
   at all, given comms_transport addressing is per-team, not per-board
   (see comms_transport_board_id_for_team()'s team_id/5 rule). The
   lowest team_id on that board is as good a representative as any -
   comms_transport only uses this to resolve which board's inbox to
   deliver to, not to pick a specific team within it. */
static uint8_t mission_link_representative_team(uint8_t board_id)
{
    return (uint8_t)(board_id * 5u);
}

/*
 * Applies a roster/fault message locally if this board hosts the
 * addressed team, otherwise relays it toward whichever board does -
 * see this file's top-of-file comment, channel 2. sender_team_id is 0
 * for laptop-originated relays: these messages come from the laptop,
 * not from any team, and comms_transport.h documents that this value
 * is local bookkeeping only (never placed on the wire) for the
 * same-board path, which this call never takes anyway (it's only
 * reached when the target board is NOT this board).
 */
static void mission_link_route_or_apply_roster(const MissionLinkRoster_t *msg)
{
    uint8_t target_board = comms_transport_board_id_for_team(msg->team_id);

    if (target_board == g_comms_transport_this_board_id)
    {
        mission_link_apply_roster(msg);
        return;
    }

    {
        uint8_t relay_buf[MISSION_LINK_ROSTER_RELAY_SIZE];

        relay_buf[0] = MISSION_LINK_MSG_ROSTER;
        mission_link_pack_roster_payload(msg, &relay_buf[1]);

        (void)comms_transport_send(0u, msg->team_id, LORA_FRAME_TYPE_MISSION_LINK,
                                    relay_buf, MISSION_LINK_ROSTER_RELAY_SIZE);
    }
}

static void mission_link_route_or_apply_fault(const MissionLinkFault_t *msg)
{
    uint8_t target_board = comms_transport_board_id_for_team(msg->team_id);

    if (target_board == g_comms_transport_this_board_id)
    {
        mission_link_apply_fault(msg);
        return;
    }

    {
        uint8_t relay_buf[MISSION_LINK_FAULT_RELAY_SIZE];

        relay_buf[0] = MISSION_LINK_MSG_FAULT;
        mission_link_pack_fault_payload(msg, &relay_buf[1]);

        (void)comms_transport_send(0u, msg->team_id, LORA_FRAME_TYPE_MISSION_LINK,
                                    relay_buf, MISSION_LINK_FAULT_RELAY_SIZE);
    }
}

/*
 * Incident relay has no single destination: MissionLinkIncident_t
 * carries no team_id (an incident isn't owned by one team - see
 * mission_link.h's own doc), but every board's CBBA bidding needs to
 * see it. comms_transport has no broadcast primitive (flagged, not
 * silently worked around - see core/comms_transport.h's own "Open
 * question" note for the same kind of gap on the sender-identity
 * side), so this applies locally AND sends once per OTHER board that
 * hosts any team, addressed to that board's representative team id.
 * With today's NUM_TEAMS=10 / 5-teams-per-board split that's exactly
 * one extra send (to board 1), but this loop doesn't hardcode that -
 * it falls out of NUM_TEAMS the same way comms_transport_board_id_
 * for_team() does.
 */
static void mission_link_apply_and_relay_incident(const MissionLinkIncident_t *msg)
{
    uint8_t board_id;
    uint8_t board_count = (uint8_t)((NUM_TEAMS + 4u) / 5u);

    mission_link_apply_incident(msg);

    for (board_id = 0u; board_id < board_count; board_id++)
    {
        uint8_t relay_buf[MISSION_LINK_INCIDENT_RELAY_SIZE];

        if (board_id == g_comms_transport_this_board_id)
        {
            continue;
        }

        relay_buf[0] = MISSION_LINK_MSG_INCIDENT;
        mission_link_pack_incident_payload(msg, &relay_buf[1]);

        (void)comms_transport_send(0u, mission_link_representative_team(board_id),
                                    LORA_FRAME_TYPE_MISSION_LINK, relay_buf,
                                    MISSION_LINK_INCIDENT_RELAY_SIZE);
    }
}

/*
 * Tries to parse exactly one frame (any of the three USART1-receivable
 * types) at the front of g_rx_buf. Returns 1 and compacts g_rx_buf if
 * a frame was consumed (successfully applied/relayed, or dropped as
 * unparseable), 0 if the buffer doesn't hold a complete frame yet and
 * nothing was consumed. Never consumes more than one frame (or one
 * resync byte) per call - callers loop this until it returns 0, same
 * shape as TelemetryFrameParser.feed()'s while-loop in
 * tools/telemetry_bridge.py.
 */
static int mission_link_rx_try_consume_one(void)
{
    MissionLinkRoster_t roster;
    MissionLinkIncident_t incident;
    MissionLinkFault_t fault;
    int rc;
    uint32_t consumed;

    rc = mission_link_parse_roster(g_rx_buf, g_rx_buf_len, &roster);
    if (rc == 1)
    {
        mission_link_route_or_apply_roster(&roster);
        consumed = MISSION_LINK_ROSTER_WIRE_SIZE;
        goto consume;
    }
    if (rc == 0)
    {
        return 0;
    }

    rc = mission_link_parse_incident(g_rx_buf, g_rx_buf_len, &incident);
    if (rc == 1)
    {
        mission_link_apply_and_relay_incident(&incident);
        consumed = MISSION_LINK_INCIDENT_WIRE_SIZE;
        goto consume;
    }
    if (rc == 0)
    {
        return 0;
    }

    rc = mission_link_parse_fault(g_rx_buf, g_rx_buf_len, &fault);
    if (rc == 1)
    {
        mission_link_route_or_apply_fault(&fault);
        consumed = MISSION_LINK_FAULT_WIRE_SIZE;
        goto consume;
    }
    if (rc == 0)
    {
        return 0;
    }

    /* None of the three types matched at buf[0] (wrong magic bytes
       for all three, or a checksum mismatch on whichever one's magic
       did match) - a magic-byte collision inside free-running debug
       text, or a torn frame. Expected failure mode, not an error:
       drop exactly one byte and let the caller retry, same resync
       policy tools/telemetry_bridge.py already applies. */
    consumed = 1u;

consume:
    {
        uint32_t i;

        for (i = consumed; i < g_rx_buf_len; i++)
        {
            g_rx_buf[i - consumed] = g_rx_buf[i];
        }
        g_rx_buf_len -= consumed;
    }

    return 1;
}

static void mission_link_usart1_rx_step(void)
{
    int byte;

    if (g_comms_transport_this_board_id != 0u)
    {
        /* Only the DK (board 0) has a laptop-facing USART1. */
        return;
    }

    while (g_rx_buf_len < MISSION_LINK_RX_BUF_CAPACITY)
    {
        byte = board_usart1_read_byte();

        if (byte < 0)
        {
            break;
        }

        g_rx_buf[g_rx_buf_len] = (uint8_t)byte;
        g_rx_buf_len++;
    }

    /* Consume as many complete frames (or resync bytes) as are
       available right now; stop once a partial frame is all that's
       left, same "wait for more bytes" contract mission_link_parse_*()
       already documents via its 0 return. */
    while (mission_link_rx_try_consume_one())
    {
    }
}

/* ---------------------------------------------------------------
 * Channel 2: comms_transport / LoRa RX (every board)
 * ---------------------------------------------------------------
 */

static void mission_link_comms_transport_rx_step(void)
{
    uint8_t sender_team_id;
    uint8_t frame_type;
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t payload_len;

    while (comms_transport_poll_received(&sender_team_id, &frame_type,
                                          payload, &payload_len))
    {
        uint8_t sub_type;

        (void)sender_team_id;

        if (frame_type != LORA_FRAME_TYPE_MISSION_LINK)
        {
            /* Not ours (LORA_FRAME_TYPE_CBBA) - CBBA's own receive
               loop is responsible for those; this task only drains
               entries it actually recognizes, never the whole inbox
               indiscriminately, since comms_transport_poll_received()
               is a shared per-board inbox (core/comms_transport.h). */
            continue;
        }

        if (payload_len < 1u)
        {
            continue;
        }

        sub_type = payload[0];

        switch (sub_type)
        {
            case MISSION_LINK_MSG_ROSTER:
                if (payload_len >= MISSION_LINK_ROSTER_RELAY_SIZE)
                {
                    MissionLinkRoster_t roster;
                    mission_link_parse_roster_payload(&payload[1], &roster);
                    mission_link_apply_roster(&roster);
                }
                break;

            case MISSION_LINK_MSG_INCIDENT:
                if (payload_len >= MISSION_LINK_INCIDENT_RELAY_SIZE)
                {
                    MissionLinkIncident_t incident;
                    mission_link_parse_incident_payload(&payload[1], &incident);
                    /* Landing on the board this was relayed TO - apply
                       only, do not relay again (this board already
                       received it from the one board that does relay
                       incidents, and re-relaying would ping-pong it
                       back). */
                    mission_link_apply_incident(&incident);
                }
                break;

            case MISSION_LINK_MSG_FAULT:
                if (payload_len >= MISSION_LINK_FAULT_RELAY_SIZE)
                {
                    MissionLinkFault_t fault;
                    mission_link_parse_fault_payload(&payload[1], &fault);
                    mission_link_apply_fault(&fault);
                }
                break;

            case MISSION_LINK_MSG_STATUS:
                if (payload_len >= MISSION_LINK_STATUS_RELAY_SIZE &&
                    g_comms_transport_this_board_id == 0u)
                {
                    /* Uplink from a non-DK board, landing on the DK -
                       forward toward the laptop over USART1. Never
                       applied locally: mission_link.h is explicit that
                       this board only ever sends this message type,
                       never receives/acts on it. A status frame
                       arriving here on a non-DK board would mean the
                       "only the DK relays uplinks" assumption above is
                       wrong for this topology - dropped rather than
                       guessing at different handling. */
                    MissionLinkStatus_t status;
                    uint8_t wire_buf[MISSION_LINK_STATUS_WIRE_SIZE];

                    mission_link_parse_status_payload(&payload[1], &status);
                    (void)mission_link_pack_status(&status, wire_buf);
                    mission_link_usart1_write(wire_buf, MISSION_LINK_STATUS_WIRE_SIZE);
                }
                break;

            default:
                /* Unrecognized sub-type - drop. Not a resync case like
                   channel 1 (lora_frame.h's envelope already delimited
                   this as one complete packet; there's no byte-stream
                   to resync within), just an unknown message this
                   version doesn't handle. */
                break;
        }
    }
}

/* ---------------------------------------------------------------
 * Channel 1 TX: local status change detection (every board)
 * ---------------------------------------------------------------
 * Sent on change, not every tick - see mission_link.h's
 * MissionLinkStatus_t doc for why (STATUS_ON_SCENE is typically held
 * for many ticks in a row, so per-tick sends would be mostly
 * redundant airtime/UART traffic).
 */

typedef struct
{
    uint8_t status;
    uint8_t equipment_level;
    uint8_t known; /* 0 until the first status has been sent for this team */
} MissionLinkLastSentStatus_t;

static MissionLinkLastSentStatus_t g_last_sent_status[NUM_TEAMS];

static void mission_link_send_status(uint8_t team_id, const MissionLinkStatus_t *status)
{
    if (g_comms_transport_this_board_id == 0u)
    {
        uint8_t wire_buf[MISSION_LINK_STATUS_WIRE_SIZE];

        (void)mission_link_pack_status(status, wire_buf);
        mission_link_usart1_write(wire_buf, MISSION_LINK_STATUS_WIRE_SIZE);
    }
    else
    {
        uint8_t relay_buf[MISSION_LINK_STATUS_RELAY_SIZE];

        relay_buf[0] = MISSION_LINK_MSG_STATUS;
        mission_link_pack_status_payload(status, &relay_buf[1]);

        /* Addressed to team 0 purely to resolve to board 0 via
           comms_transport_board_id_for_team() (team 0 / 5 == 0) - the
           DK's relay step above forwards by sub-type, not by which
           specific team_id happened to route it there. */
        (void)comms_transport_send(team_id, 0u, LORA_FRAME_TYPE_MISSION_LINK,
                                    relay_buf, MISSION_LINK_STATUS_RELAY_SIZE);
    }
}

static void mission_link_status_tx_step(void)
{
    uint8_t team_id;

    for (team_id = 0u; team_id < NUM_TEAMS; team_id++)
    {
        MissionLinkStatus_t status;
        MissionLinkLastSentStatus_t *last;

        if (comms_transport_board_id_for_team(team_id) != g_comms_transport_this_board_id)
        {
            /* Not a local team - this board has no authority to
               report on it (and no up-to-date g_teams[] entry for it
               either, beyond whatever a fault/roster relay happened
               to populate). */
            continue;
        }

        mission_link_build_status(team_id, &status);
        last = &g_last_sent_status[team_id];

        if (last->known &&
            last->status == status.status &&
            last->equipment_level == status.equipment_level)
        {
            continue; /* No change since the last send. */
        }

        mission_link_send_status(team_id, &status);

        last->known = 1u;
        last->status = status.status;
        last->equipment_level = status.equipment_level;
    }
}

/* ---------------------------------------------------------------
 * Public entry points (see this file's top-of-file handoff notes)
 * ---------------------------------------------------------------
 */

void mission_link_task_init(void)
{
    uint8_t i;

    g_rx_buf_len = 0u;

    for (i = 0u; i < NUM_TEAMS; i++)
    {
        g_last_sent_status[i].known = 0u;
        g_last_sent_status[i].status = 0u;
        g_last_sent_status[i].equipment_level = 0u;
    }
}

void mission_link_task_step(void)
{
    mission_link_usart1_rx_step();
    mission_link_comms_transport_rx_step();
    mission_link_status_tx_step();
}

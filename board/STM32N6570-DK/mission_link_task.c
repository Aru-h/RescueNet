/*
 * Board-side driver for the mission-link protocol (core/mission_link.h,
 * core/mission_link.c). STM32N6570-DK, µT-Kernel 3.0 BSP2 - mirrors
 * board/STM32N6570-DK/snapshot.c's role for telemetry_frame.h: this file
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
 *      drop one byte and retry - same policy laptop/tools/telemetry_bridge.py
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
 *
 * ---------------------------------------------------------------
 * NOTE (added alongside the handshake readiness gates below): this
 * file's comments elsewhere still say "DK" and "NUCLEO" by name in
 * several places (this top comment included). Those are pre-existing
 * and NOT touched by this change — the gates themselves are written
 * to depend only on g_comms_transport_this_board_id's ROLE (id 0 =
 * USB-facing, others = LoRa-relay-only) and on
 * mission_link_handshake_local_is_ready(), never on which physical
 * board (DK, a Pico, anything else) happens to hold that role. If
 * the board originally shipped as "board 0"/DK ever changes which
 * physical hardware plays that role, search this file for "DK" and
 * "NUCLEO" by name — those are the comments (not the logic) still
 * describing the current config rather than the general case.
 * ---------------------------------------------------------------
 */

#include <tm/tmonitor.h>

#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_link_handshake.h" /* readiness gate (already wired) PLUS,
                                       as of this change, the actual
                                       handshake wire transport itself —
                                       see "Channel 0: handshake" section
                                       near the bottom of this file. */
#include "mission_link_override.h"
#include "mission_state.h"
#include "comms_transport.h"
#include "lora_frame.h"
#include "cbba.h"            /* cbba_now_ms() - reused as this link's
                                 wall-clock source for the heartbeat
                                 trigger below, same µT-Kernel timer
                                 CBBA_ROUND_TIMEOUT_MS is already
                                 enforced against - see mission_link.h
                                 section 5a for why this shares that
                                 clock rather than standing up a second
                                 one. */

/*
 * mission_link_task_step() (this file's public entry point) is now
 * called from main_board.c's per-tick loop - see this file's own
 * top-of-file handoff notes, item 2. That wiring is scoped narrowly
 * to what THIS prompt owns there (the heartbeat trigger - see
 * main_board.c's own comment at the call site): it drives
 * mission_link_status_tx_step() below often enough for the
 * cbba_now_ms()-based heartbeat check to actually fire on schedule.
 * The USART1/comms_transport RX halves of mission_link_task_step()
 * ride along for free (they're both no-ops today anyway - see
 * board_usart1_read_byte()'s weak stub above and comms_transport's
 * own pending-bring-up status) rather than needing a second, narrower
 * entry point split out just to avoid calling them before their
 * backing drivers exist.
 */

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
 * primitive board/STM32N6570-DK/snapshot.c's ring drain already uses for
 * raw binary frame bytes on this wire - see that file's top-of-file
 * comment for why tm_putchar (raw byte) and not tm_printf (text) is
 * the right call here.
 */
/*
 * Raw USART1 write. Board firmware overrides this (main_board.c) with the
 * BSP's tm_snd_dat(): the BSP's tm_putchar() sends a CR before every LF
 * byte (0x0A), which corrupted any binary frame containing the value 10 -
 * a team id, an equipment level, a checksum byte - so the laptop dropped
 * it on a checksum mismatch (found by review). This weak default, used
 * only by the desktop harnesses (whose tm_putchar stub is raw), keeps the
 * old byte-at-a-time path.
 */
extern void board_usart1_write(const uint8_t *bytes, uint32_t len);

__attribute__((weak)) void board_usart1_write(const uint8_t *bytes, uint32_t len)
{
    uint32_t i;

    for (i = 0u; i < len; i++)
    {
        (void)tm_putchar((INT)bytes[i]);
    }
}

static void mission_link_usart1_write(const uint8_t *bytes, uint32_t len)
{
    board_usart1_write(bytes, len);
}

/* ---------------------------------------------------------------
 * Channel 1: USART1 RX (board 0 only) - accumulate + resync/parse
 * ---------------------------------------------------------------
 * Small flat buffer, not a ring. Drained every main-loop pass, but also
 * filled (not parsed) through CBBA's reply wait - see
 * mission_link_task_usart1_pump() - which can last CBBA_ROUND_TIMEOUT_MS
 * while the laptop keeps sending, so it holds several frames. The laptop
 * sends at most 10 frames/s (telemetry_bridge.MissionFeed).
 *
 * RECALIBRATED alongside CBBA_ROUND_TIMEOUT_MS's own recalibration
 * (core/cbba.h, 530ms -> 1250ms): a 1250 ms wait now sees about 12.5
 * frames (was ~6 at 530ms); 1024 bytes is 25.6 of the largest
 * (40-byte map row) frames, preserving roughly the same ~2x margin
 * over the raw estimate the original 512-byte/530ms sizing had
 * (512 bytes / 40 = 12.8 frames vs. the ~6 raw estimate). It was 64
 * when it was only ever drained once per tick.
 */
#ifndef MISSION_LINK_RX_BUF_CAPACITY
#define MISSION_LINK_RX_BUF_CAPACITY 1024u
#endif

static uint8_t g_rx_buf[MISSION_LINK_RX_BUF_CAPACITY];
static uint32_t g_rx_buf_len;

/*
 * sim_tick()-vs-roster-arrival gap (flagged against main_board.c,
 * fixed here): mission_link_handshake_round_trip_complete() only
 * proves liveness (this board and its peer both up and able to
 * exchange a handshake frame) - it says nothing about whether any
 * MissionLinkRoster_t for a team THIS board hosts has actually been
 * applied yet. Before that happens, g_teams[] for this board's teams
 * is still whatever sim_runner_init()/generate_teams() left it as
 * (laptop-authoritative field values not yet overwritten), so running
 * sim_tick() against it is running the sim against not-yet-real data.
 *
 * Tracked here (not in core/mission_link.c, which is portable and not
 * this prompt's to touch) as a simple monotonic count of every
 * mission_link_apply_roster() call this file actually makes on this
 * board's own behalf - see both call sites below
 * (mission_link_route_or_apply_roster()'s local-apply branch, and
 * mission_link_comms_transport_rx_step()'s ROSTER case) - deliberately
 * NOT counting the relay branch, since relaying a roster onward to
 * another board is not this board's own team getting real data.
 * main_board.c gates entry to its tick loop on
 * mission_link_task_any_roster_applied() below being nonzero, the
 * same "wait, pumping mission_link_task_step(), until a condition
 * this file tracks becomes true" pattern it already uses for the
 * handshake gate.
 */
static uint32_t g_roster_applied_count;

/* One roster per team per boot. The laptop re-sends every roster
   periodically, because a roster relayed over LoRa can be lost and a
   board with no roster never leaves its boot wait. Re-applying one
   mid-mission would reset that team's position and stamina to the
   mission-start values, so only the first accepted roster for a team is
   applied; repeats are ignored. Cleared by mission_link_task_init() (a
   rebooted board needs its rosters again). */
static uint8_t g_roster_applied_for_team[NUM_TEAMS];

/* Applies a roster unless this team already has one, and counts it only
   if mission_link_apply_roster() accepted it - a rejected roster (bad
   team_id or location) used to count too, which let the boot wait end
   on data that was just thrown away. */
static void mission_link_apply_roster_once(const MissionLinkRoster_t *msg)
{
    uint32_t rejects_before;

    if (msg->team_id < NUM_TEAMS && g_roster_applied_for_team[msg->team_id])
    {
        return;
    }

    rejects_before = mission_link_roster_reject_count();
    mission_link_apply_roster(msg);
    if (mission_link_roster_reject_count() != rejects_before)
    {
        return;
    }

    g_roster_applied_for_team[msg->team_id] = 1u;
    g_roster_applied_count++;
}

/* Set on EVERY handshake Req this board receives (either channel), not
   just the first; cleared when mission_link_handshake_tx_step() below
   actually sends this board's own Reply. A repeated Req is the asker
   saying "I still have no Reply from you": board 0 only retries its
   relay Req while it has not recorded one (mission_link_handshake_
   should_send_relay_req()), and those retries exist precisely because
   LoRa loses frames. Without this, g_own_reply_sent latched after the
   first Reply and every retry was ignored, so ONE lost Reply was
   unrecoverable: board 0 retried until HANDSHAKE_NUCLEO_TIMEOUT_TICKS,
   reported this ready board to the laptop as timed out (ready=0), and
   this board then waited forever for a roster that never came. Bounded
   by the asker's own retry cadence, so this cannot flood the link. */
static uint8_t g_own_reply_resend_pending;

uint32_t mission_link_task_any_roster_applied(void)
{
    return g_roster_applied_count;
}

/* Board-topology helper: which team id to address to reach `board_id`
   at all, given comms_transport addressing is per-team, not per-board
   (see comms_transport_board_id_for_team()'s team_id/5 rule). The
   lowest team_id on that board is as good a representative as any -
   comms_transport only uses this to resolve which board's inbox to
   deliver to, not to pick a specific team within it. */
static uint8_t mission_link_representative_team(uint8_t board_id)
{
    /* Bug fix: this used to hardcode the board-assignment divisor as a
       bare literal 5u instead of comms_transport_representative_team_
       for_board(), which wraps the actual COMMS_TRANSPORT_TEAMS_PER_
       BOARD value (private to core/comms_transport.c, overridable via
       -D). A future change to that divisor without also updating this
       literal would have silently misrouted incident/handshake relays
       to the wrong board. */
    return comms_transport_representative_team_for_board(board_id);
}

/*
 * Shared readiness check for the three functions below — the actual
 * gate the earlier top-of-file design note referred to (moved here
 * after finding the original placement, at raw-byte-receive time,
 * would have blocked the handshake Req frame itself from ever being
 * parsed — see mission_link_usart1_rx_step()'s correction comment).
 * "Not ready" means drop the message outright — no apply, no relay,
 * nothing queued for later. mission_link_handshake_build_local_
 * reply() is the round-trip check (locally-ready AND request-seen),
 * not mission_link_handshake_local_is_ready() alone — see main_
 * board.c's mission_link_handshake_round_trip_complete() comment for
 * why the latter is NOT sufficient on its own.
 */
static int mission_link_handshake_ready_now(void)
{
    MissionLinkHandshakeReply_t scratch; /* discarded — only the
                                             return value is used as
                                             the gate here; the actual
                                             reply is built/sent by
                                             this file's own handshake
                                             TX step, not here. */
    return mission_link_handshake_build_local_reply(&scratch) != 0;
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
    uint8_t target_board;

    /* HANDSHAKE READINESS GATE — see mission_link_handshake_ready_
       now()'s own comment. Applies to BOTH branches below (apply and
       relay), not just the local-apply one: a board that hasn't
       completed its own handshake yet rejects mission-link data
       outright, it doesn't selectively forward what it can't yet
       apply to itself. */
    if (!mission_link_handshake_ready_now())
    {
        return;
    }

    target_board = comms_transport_board_id_for_team(msg->team_id);

    /* An out-of-range team_id goes through the local apply too, which
       rejects and counts it, instead of being relayed to a board id that
       doesn't exist. */
    if (msg->team_id >= NUM_TEAMS || target_board == g_comms_transport_this_board_id)
    {
        mission_link_apply_roster_once(msg);
        return;
    }

    {
        uint8_t relay_buf[MISSION_LINK_ROSTER_RELAY_SIZE];

        relay_buf[0] = MISSION_LINK_MSG_ROSTER;
        mission_link_pack_roster_payload(msg, &relay_buf[1]);

        (void)comms_transport_send(0u, msg->team_id, LORA_FRAME_TYPE_MISSION_LINK,
                                    COMMS_PRECEDENCE_ROUTINE,
                                    relay_buf, MISSION_LINK_ROSTER_RELAY_SIZE);
    }
}

static void mission_link_route_or_apply_fault(const MissionLinkFault_t *msg)
{
    uint8_t target_board;

    /* HANDSHAKE READINESS GATE — see mission_link_route_or_apply_
       roster()'s identical gate just above for the rationale. */
    if (!mission_link_handshake_ready_now())
    {
        return;
    }

    target_board = comms_transport_board_id_for_team(msg->team_id);

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
                                    mission_link_fault_precedence(msg),
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
 * board_count comes from comms_transport_board_count() (added
 * alongside mission_link_handshake.c's N-board generalization, in
 * the same change, as the one place this division happens - see that
 * function's own doc for why it's a function call here and not a
 * locally re-derived formula the way this loop used to compute it).
 */
static void mission_link_apply_and_relay_incident(const MissionLinkIncident_t *msg)
{
    uint8_t board_id;
    uint8_t board_count = comms_transport_board_count();

    /* HANDSHAKE READINESS GATE — see mission_link_route_or_apply_
       roster()'s identical gate for the rationale. Same "reject
       outright, apply nothing, relay nothing" scope. */
    if (!mission_link_handshake_ready_now())
    {
        return;
    }

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
                                    LORA_FRAME_TYPE_MISSION_LINK,
                                    mission_link_incident_precedence(msg),
                                    relay_buf, MISSION_LINK_INCIDENT_RELAY_SIZE);
    }
}

/*
 * Sends one already-packed relay payload (sub-type byte + payload) to
 * every other board - the fan-out for messages every board needs: the
 * mission clock, route map rows and sites (core/mission_link.h sections
 * 5b-5d). PRIORITY, like CBBA traffic: each is re-sent by the laptop, so
 * a late or lost one only delays a board.
 */
static void mission_link_relay_to_other_boards(const uint8_t *relay_buf, uint8_t relay_len)
{
    uint8_t board_id;
    uint8_t board_count = comms_transport_board_count();

    for (board_id = 0u; board_id < board_count; board_id++)
    {
        if (board_id == g_comms_transport_this_board_id)
        {
            continue;
        }

        (void)comms_transport_send(0u, mission_link_representative_team(board_id),
                                    LORA_FRAME_TYPE_MISSION_LINK,
                                    COMMS_PRECEDENCE_PRIORITY,
                                    relay_buf, relay_len);
    }
}

static void mission_link_apply_and_relay_clock(const MissionLinkClock_t *msg)
{
    uint8_t relay_buf[MISSION_LINK_CLOCK_RELAY_SIZE];

    if (!mission_link_handshake_ready_now())
    {
        return;
    }

    mission_link_apply_clock(msg);

    relay_buf[0] = MISSION_LINK_MSG_CLOCK;
    mission_link_pack_clock_payload(msg, &relay_buf[1]);
    mission_link_relay_to_other_boards(relay_buf, MISSION_LINK_CLOCK_RELAY_SIZE);
}

static void mission_link_apply_and_relay_map_row(const MissionLinkMapRow_t *msg)
{
    uint8_t relay_buf[MISSION_LINK_MAP_ROW_RELAY_SIZE];

    if (!mission_link_handshake_ready_now() || !mission_link_apply_map_row(msg))
    {
        return;
    }

    relay_buf[0] = MISSION_LINK_MSG_MAP_ROW;
    mission_link_pack_map_row_payload(msg, &relay_buf[1]);
    mission_link_relay_to_other_boards(relay_buf, MISSION_LINK_MAP_ROW_RELAY_SIZE);
}

static void mission_link_apply_and_relay_sites(const MissionLinkSites_t *msg)
{
    uint8_t relay_buf[MISSION_LINK_SITES_RELAY_SIZE];

    /* A rejected frame is not relayed either: every board would reject it. */
    if (!mission_link_handshake_ready_now() || !mission_link_apply_sites(msg))
    {
        return;
    }

    relay_buf[0] = MISSION_LINK_MSG_SITES;
    mission_link_pack_sites_payload(msg, &relay_buf[1]);
    mission_link_relay_to_other_boards(relay_buf, MISSION_LINK_SITES_RELAY_SIZE);
}

/*
 * Reports what happened to one override (core/mission_link_override.h
 * section 3). Board 0 writes the ack straight to USART1; any other board
 * sends it over LoRa, and board 0 forwards it in
 * mission_link_comms_transport_rx_step() below.
 */
static void mission_link_send_override_ack(const MissionLinkOverride_t *msg, uint8_t result)
{
    MissionLinkOverrideAck_t ack;

    ack.team_id = msg->team_id;
    ack.incident_id = msg->incident_id;
    ack.result = result;

    if (g_comms_transport_this_board_id == 0u)
    {
        uint8_t wire_buf[MISSION_LINK_OVERRIDE_ACK_WIRE_SIZE];

        (void)mission_link_override_pack_ack(&ack, wire_buf);
        mission_link_usart1_write(wire_buf, MISSION_LINK_OVERRIDE_ACK_WIRE_SIZE);
    }
    else
    {
        uint8_t payload[MISSION_LINK_OVERRIDE_ACK_PAYLOAD_SIZE];

        mission_link_override_pack_ack_payload(&ack, payload);
        /* Receiver team 0 addresses board 0 (the only board with USART1). */
        (void)comms_transport_send(msg->team_id, 0u, LORA_FRAME_TYPE_OVERRIDE_ACK,
                                    COMMS_PRECEDENCE_IMMEDIATE, payload,
                                    (uint8_t)MISSION_LINK_OVERRIDE_ACK_PAYLOAD_SIZE);
    }
}

/*
 * Dashboard override (laptop/tools/app.py's /api/board/override) arriving
 * on USART1. Same readiness gate as the roster/fault routers above, but
 * every outcome is acknowledged: applied here if this board hosts
 * msg->team_id, otherwise relayed as LORA_FRAME_TYPE_OVERRIDE, which the
 * hosting board applies and acknowledges in
 * mission_link_comms_transport_rx_step() below.
 */
static void mission_link_route_or_apply_override(const MissionLinkOverride_t *msg)
{
    uint8_t result;

    if (!mission_link_handshake_ready_now())
    {
        result = MISSION_LINK_OVERRIDE_ACK_NOT_READY;
    }
    else if (msg->team_id >= NUM_TEAMS)
    {
        result = MISSION_LINK_OVERRIDE_ACK_BAD_TEAM;
    }
    else if (comms_transport_board_id_for_team(msg->team_id) == g_comms_transport_this_board_id)
    {
        mission_link_override_apply(msg);
        result = MISSION_LINK_OVERRIDE_ACK_APPLIED;
    }
    else if (mission_link_override_route_or_apply(msg) == 0)
    {
        result = MISSION_LINK_OVERRIDE_ACK_RELAYED;
    }
    else
    {
        result = MISSION_LINK_OVERRIDE_ACK_RELAY_FAILED;
    }

    mission_link_send_override_ack(msg, result);
}

/*
 * Tries to parse exactly one frame (any of the three USART1-receivable
 * types) at the front of g_rx_buf. Returns 1 and compacts g_rx_buf if
 * a frame was consumed (successfully applied/relayed, or dropped as
 * unparseable), 0 if the buffer doesn't hold a complete frame yet and
 * nothing was consumed. Never consumes more than one frame (or one
 * resync byte) per call - callers loop this until it returns 0, same
 * shape as TelemetryFrameParser.feed()'s while-loop in
 * laptop/tools/telemetry_bridge.py.
 */
static int mission_link_rx_try_consume_one(void)
{
    MissionLinkRoster_t roster;
    MissionLinkIncident_t incident;
    MissionLinkFault_t fault;
    MissionLinkHandshakeReq_t handshake_req;
    MissionLinkOverride_t override_msg;
    MissionLinkClock_t clock_msg;
    MissionLinkMapRow_t map_row;
    MissionLinkSites_t sites;
    int rc;
    uint32_t consumed;

    /* Handshake Req is tried first: it's the smallest/most-likely-
       to-arrive-first frame at boot (the laptop's HandshakeCoordinator
       starts probing before it has any reason to send roster/
       incident/fault data — nothing downstream of the handshake is
       even safe to send yet, per the readiness gate above). Order
       otherwise doesn't matter for correctness (each parse_*() only
       matches its own magic bytes), this is purely "try the common
       case first" — see mission_link_parse_roster()'s own 1/0/-1
       contract, same one this follows. */
    rc = mission_link_handshake_parse_req(g_rx_buf, g_rx_buf_len, &handshake_req);
    if (rc == 1)
    {
        /* Only meaningful if handshake_req.board_id == this board's
           own id (0 — USART1 only exists on board 0, checked by this
           function's only caller). A request addressed to a
           different board_id arriving on THIS board's USART1 would
           mean the laptop is confused about topology; note it as
           "a request was seen" anyway rather than silently drop it,
           since mission_link_handshake_note_request_received() has
           no board_id parameter to filter on in the first place (see
           mission_link_handshake.h — it's a single per-board latch,
           not a table keyed by requester). */
        mission_link_handshake_note_request_received();
        g_own_reply_resend_pending = 1u;
        consumed = MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE;
        goto consume;
    }
    if (rc == 0)
    {
        return 0;
    }

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

    rc = mission_link_override_parse(g_rx_buf, g_rx_buf_len, &override_msg);
    if (rc == 1)
    {
        mission_link_route_or_apply_override(&override_msg);
        consumed = MISSION_LINK_OVERRIDE_WIRE_SIZE;
        goto consume;
    }
    if (rc == 0)
    {
        return 0;
    }

    rc = mission_link_parse_clock(g_rx_buf, g_rx_buf_len, &clock_msg);
    if (rc == 1)
    {
        mission_link_apply_and_relay_clock(&clock_msg);
        consumed = MISSION_LINK_CLOCK_WIRE_SIZE;
        goto consume;
    }
    if (rc == 0)
    {
        return 0;
    }

    rc = mission_link_parse_map_row(g_rx_buf, g_rx_buf_len, &map_row);
    if (rc == 1)
    {
        mission_link_apply_and_relay_map_row(&map_row);
        consumed = MISSION_LINK_MAP_ROW_WIRE_SIZE;
        goto consume;
    }
    if (rc == 0)
    {
        return 0;
    }

    rc = mission_link_parse_sites(g_rx_buf, g_rx_buf_len, &sites);
    if (rc == 1)
    {
        mission_link_apply_and_relay_sites(&sites);
        consumed = MISSION_LINK_SITES_WIRE_SIZE;
        goto consume;
    }
    if (rc == 0)
    {
        return 0;
    }

    /* None of the frame types matched at buf[0] (wrong magic bytes
       for all three, or a checksum mismatch on whichever one's magic
       did match) - a magic-byte collision inside free-running debug
       text, or a torn frame. Expected failure mode, not an error:
       drop exactly one byte and let the caller retry, same resync
       policy laptop/tools/telemetry_bridge.py already applies. */
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

/*
 * Moves waiting USART1 bytes into g_rx_buf without parsing them. Also
 * called from long waits outside this task's step (main_board.c's
 * cbba_wait_poll(), during CBBA's reply wait): the UART holds only a few
 * bytes, and a laptop frame arriving while the board sits in that wait
 * was otherwise overrun and lost. Parsing stays in
 * mission_link_task_step(), so nothing is applied mid-CBBA.
 */
void mission_link_task_usart1_pump(void)
{
    int byte;

    if (g_comms_transport_this_board_id != 0u)
    {
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
}

static void mission_link_usart1_rx_step(void)
{
    if (g_comms_transport_this_board_id != 0u)
    {
        /* Only the board holding id 0 has a laptop-facing USART1 —
           a ROLE, not a specific chip. (CHANGED: this comment used
           to name "the DK" here; it's rewritten because whichever
           physical board gets built/flashed with
           g_comms_transport_this_board_id == 0 is the one this
           applies to.) */
        return;
    }

    /*
     * CORRECTION vs. the first pass of this gate: it used to discard
     * every incoming byte outright whenever this board wasn't ready
     * yet. That's wrong — it would also discard the
     * MissionLinkHandshakeReq_t frame itself, which is the ONE frame
     * that has to get through before readiness, since it's what
     * makes readiness happen (mission_link_handshake_note_request_
     * received(), called from mission_link_rx_try_consume_one()
     * below). The readiness gate now lives at the point each
     * roster/incident/fault message would be APPLIED (see
     * mission_link_route_or_apply_roster()/_fault() and
     * mission_link_apply_and_relay_incident()'s own gates below) —
     * not here. This function always accumulates bytes and always
     * lets mission_link_rx_try_consume_one() try to parse a frame,
     * handshake included, regardless of readiness.
     */

    mission_link_task_usart1_pump();

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
    for (;;)
    {
        uint8_t sender_team_id;
        uint8_t frame_type;
        uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t payload_len;
        uint8_t sub_type;

        /* Bug fix: this used to be `while (comms_transport_poll_received(
         * ...))`, an unconditional, destructive poll on the shared per-
         * board inbox (core/comms_transport.h) with the frame_type check
         * only running AFTER the pop below. That silently consumed (and
         * then dropped, via `continue`) any LORA_FRAME_TYPE_CBBA/
         * FAULT_BROADCAST/FAULT_VOTE frame that happened to be queued
         * when this ran, before core/cbba.c's cbba_ingest_remote_mesh_
         * replies() or core/fault_comms.c's fault_comms_ingest_remote_
         * frames() - both driven from sim_tick() earlier in this same
         * board's per-tick loop, see main_board.c - ever got a chance to
         * see it on a later tick. Peek first instead, same pattern those
         * two functions already use: only poll (destructively) once the
         * peeked type is confirmed to be one this task actually owns,
         * otherwise leave it in the inbox for the next consumer. */
        /* Takes this task's frame types from anywhere in the inbox, so a
           CBBA/fault frame at the head (whose consumer may not be running
           yet - e.g. in main_board.c's boot wait loops) can no longer
           hide a Req or roster queued behind it. See
           comms_transport_poll_received_types(). */
        if (!comms_transport_poll_received_types(COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_HANDSHAKE) |
                                                  COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_MISSION_LINK) |
                                                  COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_OVERRIDE) |
                                                  COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_OVERRIDE_ACK),
                                                  &sender_team_id, &frame_type,
                                                  payload, &payload_len))
        {
            break; /* none of this task's frames waiting */
        }

        (void)sender_team_id;

        /*
         * CORRECTION vs. the first pass of this gate: it used to sit
         * here, before the frame_type check, which would have
         * dropped LORA_FRAME_TYPE_HANDSHAKE frames too — exactly the
         * frames that have to get through before this board is ready
         * (board 1's Req from the DK, and the DK's Reply from board
         * 1). Handshake frames are now handled in their own branch
         * below, ungated; the readiness gate moved down to sit only
         * in front of the MISSION_LINK sub-type switch, where
         * roster/incident/fault/status actually get applied/relayed.
         */

        if (frame_type == LORA_FRAME_TYPE_HANDSHAKE)
        {
            if (payload_len < 1u)
            {
                continue;
            }

            sub_type = payload[0];

            if (sub_type == MISSION_LINK_HANDSHAKE_MSG_REQ &&
                payload_len >= MISSION_LINK_HANDSHAKE_REQ_RELAY_SIZE)
            {
                /* Only meaningful addressed to THIS board — but as
                   with the USART1 Req case above, note_request_
                   received() has no board_id filter of its own (it's
                   a single per-board latch), so a misaddressed Req
                   is still just noted rather than silently dropped.
                   In practice this is board 1 receiving the DK's Req
                   (see mission_link_handshake_should_send_nucleo_
                   req()'s TX side, this file's handshake TX step
                   below) — board 0 sending its OWN Req doesn't loop
                   back to itself over the radio. */
                mission_link_handshake_note_request_received();
                g_own_reply_resend_pending = 1u;
            }
            else if (sub_type == MISSION_LINK_HANDSHAKE_MSG_REPLY &&
                     payload_len >= MISSION_LINK_HANDSHAKE_REPLY_RELAY_SIZE &&
                     g_comms_transport_this_board_id == 0u)
            {
                /* Only the DK (board 0) ever originates a Req to
                   relay to, so only it ever expects a Reply back —
                   see mission_link_handshake.h's "DK-side bookkeeping
                   for the NUCLEO leg" section, which is explicit that
                   these functions are board-0-only by caller
                   convention, not by an internal check of their own. */
                MissionLinkHandshakeReply_t remote_reply;
                mission_link_handshake_parse_reply_payload(&payload[1], &remote_reply);
                mission_link_handshake_note_remote_reply(&remote_reply);
            }

            continue;
        }

        if (frame_type == LORA_FRAME_TYPE_OVERRIDE)
        {
            MissionLinkOverride_t override_msg;

            /* Exact length, like fault_comms.c's receiver: anything else
               is protocol drift, not a truncated override to guess at. */
            if (payload_len != (uint8_t)MISSION_LINK_OVERRIDE_PAYLOAD_SIZE)
            {
                continue;
            }

            mission_link_override_parse_payload(payload, &override_msg);

            /* Only the board hosting the team handles (and acknowledges)
               it, and never relays again - board 0 is the only relayer.
               The team_id range check comes first so the board lookup
               stays meaningful. */
            if (override_msg.team_id >= NUM_TEAMS ||
                comms_transport_board_id_for_team(override_msg.team_id) !=
                    g_comms_transport_this_board_id)
            {
                continue;
            }

            /* Same readiness gate as the mission-link data types below. */
            if (!mission_link_handshake_ready_now())
            {
                mission_link_send_override_ack(&override_msg, MISSION_LINK_OVERRIDE_ACK_NOT_READY);
                continue;
            }

            mission_link_override_apply(&override_msg);
            mission_link_send_override_ack(&override_msg, MISSION_LINK_OVERRIDE_ACK_APPLIED);
            continue;
        }

        if (frame_type == LORA_FRAME_TYPE_OVERRIDE_ACK)
        {
            /* A hosting board's ack, on its way to the laptop. Only board
               0 has USART1; forwarded as-is, no readiness gate - it
               reports on the other board's state, not this one's. */
            if (g_comms_transport_this_board_id == 0u &&
                payload_len == (uint8_t)MISSION_LINK_OVERRIDE_ACK_PAYLOAD_SIZE)
            {
                MissionLinkOverrideAck_t ack;
                uint8_t wire_buf[MISSION_LINK_OVERRIDE_ACK_WIRE_SIZE];

                mission_link_override_parse_ack_payload(payload, &ack);
                (void)mission_link_override_pack_ack(&ack, wire_buf);
                mission_link_usart1_write(wire_buf, MISSION_LINK_OVERRIDE_ACK_WIRE_SIZE);
            }
            continue;
        }

        if (frame_type != LORA_FRAME_TYPE_MISSION_LINK)
        {
            /* Should be unreachable now that the peek above already
               filters to HANDSHAKE/MISSION_LINK only (peek and poll
               read the same head-of-queue entry with nothing else able
               to preempt in between - see comms_transport_inbox_push()'s
               concurrency-invariant comment). Kept as defense-in-depth
               rather than trusted blindly, since frame_type came off a
               separate call than peeked_type did. */
            continue;
        }

        if (payload_len < 1u)
        {
            continue;
        }

        /* HANDSHAKE READINESS GATE — now scoped to exactly the four
           mission-link data types below (roster/incident/fault/
           status), not the whole RX poll loop. Same round-trip check
           as mission_link_route_or_apply_roster()'s gate (see that
           function's comment) — reject outright, no apply, no
           relay/forward, if this board's own handshake hasn't
           completed yet. */
        if (!mission_link_handshake_ready_now())
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
                    mission_link_apply_roster_once(&roster);
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

            case MISSION_LINK_MSG_CLOCK:
                if (payload_len >= MISSION_LINK_CLOCK_RELAY_SIZE)
                {
                    MissionLinkClock_t clock_msg;
                    mission_link_parse_clock_payload(&payload[1], &clock_msg);
                    /* Apply only - board 0 is the one relayer. */
                    mission_link_apply_clock(&clock_msg);
                }
                break;

            case MISSION_LINK_MSG_MAP_ROW:
                if (payload_len >= MISSION_LINK_MAP_ROW_RELAY_SIZE)
                {
                    MissionLinkMapRow_t map_row;
                    mission_link_parse_map_row_payload(&payload[1], &map_row);
                    (void)mission_link_apply_map_row(&map_row);
                }
                break;

            case MISSION_LINK_MSG_SITES:
                if (payload_len >= MISSION_LINK_SITES_RELAY_SIZE)
                {
                    MissionLinkSites_t sites;
                    mission_link_parse_sites_payload(&payload[1], &sites);
                    (void)mission_link_apply_sites(&sites);
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
 * Sent on status/equipment_level/work_rate change, plus a periodic
 * heartbeat - see mission_link.h's MissionLinkStatus_t doc (schema v2)
 * for why: STATUS_ON_SCENE is typically held for many ticks in a row,
 * so a per-tick send would be mostly redundant airtime/UART traffic,
 * but a change-only trigger alone would let a silent board look
 * identical to a healthy one holding steady - the heartbeat is what
 * bounds that staleness and gives the laptop's watchdog a liveness
 * signal.
 *
 * last_sent_ms is a cbba_now_ms() wall-clock timestamp, NOT a sim-tick
 * count: ticks follow the laptop's mission clock, whose pace is a laptop
 * setting (36 s in a real-time mission, much faster in a rehearsal), so
 * ticks don't have a fixed real duration - see mission_link.h section
 * 5a and cbba.h's own
 * CBBA_ROUND_TIMEOUT_MS comment for the identical reasoning.
 */

typedef struct
{
    uint8_t  status;
    uint8_t  equipment_level;
    uint8_t  work_rate;
    uint8_t  known; /* 0 until the first status has been sent for this team */
    int16_t  loc_x;
    int16_t  loc_y;
    uint32_t last_sent_ms;
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
                                    mission_link_status_precedence(status),
                                    relay_buf, MISSION_LINK_STATUS_RELAY_SIZE);
    }
}

static void mission_link_status_tx_step(void)
{
    uint8_t team_id;
    uint32_t now_ms = cbba_now_ms();

    for (team_id = 0u; team_id < NUM_TEAMS; team_id++)
    {
        MissionLinkStatus_t status;
        MissionLinkLastSentStatus_t *last;
        int should_send;

        if (comms_transport_board_id_for_team(team_id) != g_comms_transport_this_board_id)
        {
            /* Not a local team - this board has no authority to
               report on it (and no up-to-date g_teams[] entry for it
               either, beyond whatever a fault/roster relay happened
               to populate). */
            continue;
        }

        /* No per-recommendation gate here: a team whose recovery
           recommendation is RECOVERY_ACTION_ABORT_MISSION reports like
           any other team. ABORT is precisely what pulls the team off its
           incident and sends it to rest (recovery_dispatch() ->
           abandon_task_for_recovery(), core/recovery_manager.c), and the
           recommendation stays sticky under hysteresis - so suppressing
           it here hid exactly the status/location change the laptop
           needs to see: laptop/mission_mirror.c's
           mission_mirror_team_left_task() never reopened the incident
           (left ASSIGNED to a team that had walked away) and the team's
           position/stamina froze on the dashboard. The change-detection
           triggers below plus the heartbeat already bound bandwidth for
           a team that isn't doing anything new, aborted or not. */

        mission_link_build_status(team_id, &status);
        last = &g_last_sent_status[team_id];

        if (!last->known)
        {
            /* Nothing sent yet for this team - always send, same as
               v1's behavior (there is no prior anchor to compare
               against). */
            should_send = 1;
        }
        else if (last->status != status.status ||
                 last->equipment_level != status.equipment_level)
        {
            /* v1 triggers, unchanged. */
            should_send = 1;
        }
        else if (last->work_rate != status.work_rate)
        {
            /* New trigger: work_rate changed - fault onset/clear on a
               resource-relevant class, or a new task assignment reset
               the anchor (mission_link_build_status() reflects the
               newly-assigned incident's work_remaining/incident_id
               immediately, so a reassignment shows up here as a
               work_rate-and/or-assigned_incident_id change even when
               status/equipment_level didn't move). */
            should_send = 1;
        }
        else if (last->loc_x != status.loc_x || last->loc_y != status.loc_y)
        {
            /* Schema v3: the team moved. The laptop mirrors positions
               from these reports (laptop/mission_mirror.h), and a team
               changes cell at most once per clock tick. */
            should_send = 1;
        }
        else if ((now_ms - last->last_sent_ms) >= MISSION_LINK_HEARTBEAT_INTERVAL_MS)
        {
            /* New trigger: periodic heartbeat. Re-anchors work_
               remaining_at_report/stamina_at_report/report_tick even
               though nothing else changed - see mission_link.h
               section 5a. Unsigned subtraction wraps correctly across
               a cbba_now_ms() rollover, same reasoning cbba.c's own
               round-timeout wait already relies on. */
            should_send = 1;
        }
        else
        {
            should_send = 0;
        }

        if (!should_send)
        {
            continue;
        }

        mission_link_send_status(team_id, &status);

        last->known = 1u;
        last->status = status.status;
        last->equipment_level = status.equipment_level;
        last->work_rate = status.work_rate;
        last->loc_x = status.loc_x;
        last->loc_y = status.loc_y;
        last->last_sent_ms = now_ms;
    }
}

/* ---------------------------------------------------------------
 * Channel 0 TX: handshake. Every board sends its own reply once
 * ready+requested (mission_link_handshake_build_local_reply()); board
 * 0 additionally drives EVERY OTHER board's relay leg — sending/
 * retrying each peer's own Req and forwarding whatever mission_link_
 * handshake_get_relay_reply() produces for that peer (real or
 * synthetic-timeout) to the laptop. GENERALIZED from a single fixed
 * "the NUCLEO leg" to a loop over comms_transport_board_count() peers
 * (see CHANGELOG.md) - with today's 2-board split that's still
 * exactly one peer in practice, but nothing here hardcodes that.
 * See core/mission_link_handshake.h's own doc for the state machine
 * this drives; this function only decides WHEN to call its pack/
 * send functions, never re-implements any of the retry/timeout
 * policy itself.
 * ---------------------------------------------------------------
 */
static uint8_t g_own_reply_sent;
static MissionLinkHandshakeReply_t g_own_reply_last_sent;

/* GENERALIZED to N peers (see CHANGELOG.md) - was a single pair of
   dedup-state variables for "the one relay peer, board 1". Indexed
   directly by peer board_id, same as core/mission_link_handshake.c's
   own arrays now are - see that file for why this is a fixed-cap
   array (HANDSHAKE_MAX_BOARDS) rather than sized off comms_
   transport_board_count() at runtime (no dynamic allocation in a
   portable/freestanding build). Slot 0 (this board, when this board
   IS board 0) is simply never touched by the loop below, same
   reasoning as the .c file's own doc on why no relabeling was needed
   to generalize. */
static uint8_t g_relay_reply_sent[HANDSHAKE_MAX_BOARDS];
static MissionLinkHandshakeReply_t g_relay_reply_last_sent[HANDSHAKE_MAX_BOARDS];

/* Last lora_crypto_peer_session_count() seen per peer, and how many peer
   reboots this board has acted on (mission_link_task_peer_reboot_count()).
   A count that rises past a nonzero value means the peer started a new
   LoRa session - it rebooted while this board stayed up. */
static uint32_t g_peer_session_count_seen[HANDSHAKE_MAX_BOARDS];
static uint32_t g_peer_reboot_count;

uint32_t mission_link_task_peer_reboot_count(void)
{
    return g_peer_reboot_count;
}

static void mission_link_handshake_tx_step(void)
{
    uint32_t now_ms = cbba_now_ms();

    /* This board's OWN reply, to whoever asked it. build_local_
       reply() is deliberately "not consumed" (see its own doc) so
       THIS function is responsible for not resending the same
       answer every single tick forever — sent once per distinct
       value. In practice the value should never change once
       ready+requested both latch true (both are one-way latches -
       see mark_local_ready()/note_request_received()'s own docs),
       so the change-check below is defensive, not expected to ever
       re-fire after the first send. The one deliberate exception is
       g_own_reply_resend_pending: a Req received after the Reply went
       out means that Reply was lost, so answer it again (see that
       flag's own doc near the top of this file). */
    {
        MissionLinkHandshakeReply_t reply;

        if (mission_link_handshake_build_local_reply(&reply) &&
            (!g_own_reply_sent ||
             g_own_reply_resend_pending ||
             reply.board_id != g_own_reply_last_sent.board_id ||
             reply.protocol_version != g_own_reply_last_sent.protocol_version ||
             reply.ready != g_own_reply_last_sent.ready))
        {
            if (g_comms_transport_this_board_id == 0u)
            {
                uint8_t wire_buf[MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE];
                (void)mission_link_handshake_pack_reply(&reply, wire_buf);
                mission_link_usart1_write(wire_buf, MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE);
            }
            else
            {
                uint8_t relay_buf[MISSION_LINK_HANDSHAKE_REPLY_RELAY_SIZE];

                relay_buf[0] = MISSION_LINK_HANDSHAKE_MSG_REPLY;
                mission_link_handshake_pack_reply_payload(&reply, &relay_buf[1]);

                /* Addressed to team 0 to resolve to board 0 via
                   comms_transport_board_id_for_team() (team 0 / 5 ==
                   0) — same convention mission_link_send_status()
                   already uses for its own non-DK-board uplink. */
                (void)comms_transport_send(0u, 0u, LORA_FRAME_TYPE_HANDSHAKE,
                                            COMMS_PRECEDENCE_IMMEDIATE,
                                            relay_buf, MISSION_LINK_HANDSHAKE_REPLY_RELAY_SIZE);
            }

            g_own_reply_sent = 1u;
            g_own_reply_resend_pending = 0u;
            g_own_reply_last_sent = reply;
        }
    }

    /* DK-only: drive every OTHER board's relay leg. Everything below
       is a no-op on any other board — mission_link_handshake_should_
       send_relay_req()/get_relay_reply() are documented board-0-only
       by caller convention (see mission_link_handshake.h), this `if`
       is what enforces that convention on this file's side. */
    if (g_comms_transport_this_board_id != 0u)
    {
        return;
    }

    /* GENERALIZED to N boards (see CHANGELOG.md) - was hardcoded to
       peer board_id=1 (the one NUCLEO). Loops peer_board_id over
       every board this build actually has (comms_transport_board_
       count(), the single source of board count - see that
       function's own doc), skipping board 0 itself (this board never
       relays to itself). Adding a board is a NUM_TEAMS/COMMS_
       TRANSPORT_TEAMS_PER_BOARD change, not a code change here. */
    {
        uint8_t board_count = comms_transport_board_count();
        uint8_t peer;

        /* Bug fix: g_relay_reply_sent[]/g_relay_reply_last_sent[] above
         * are fixed-size arrays capped at HANDSHAKE_MAX_BOARDS, but this
         * loop was bounded only by comms_transport_board_count() with no
         * clamp against that cap - the same class of out-of-bounds write
         * already fixed this session in core/mission_link_handshake.c's
         * own per-peer arrays (every accessor there now bounds-checks
         * peer_board_id). That fix never propagated to this file's own
         * local duplicate arrays. Unreachable today (NUM_TEAMS=10,
         * COMMS_TRANSPORT_TEAMS_PER_BOARD=5 => board_count=2, safely
         * under the cap of 8), but this file's own comment right above
         * explicitly anticipates board_count growing ("Adding a board is
         * a NUM_TEAMS/COMMS_TRANSPORT_TEAMS_PER_BOARD change, not a code
         * change here") without mentioning HANDSHAKE_MAX_BOARDS also
         * needs to grow to match - clamp defensively instead of trusting
         * that to always be remembered. */
        if (board_count > HANDSHAKE_MAX_BOARDS)
        {
            board_count = HANDSHAKE_MAX_BOARDS;
        }

        for (peer = 1u; peer < board_count; peer++)
        {
            /* Peer reboot: its old handshake state here is meaningless
               (it has forgotten having replied), so re-open the leg -
               Reqs resume - and tell the laptop right away that this
               peer is NOT ready until it answers again. Without this,
               board 0 never sent a rebooted peer another Req and the
               peer waited at its handshake gate forever. */
            {
                uint32_t sessions = lora_crypto_peer_session_count(peer);

                if (sessions != g_peer_session_count_seen[peer])
                {
                    if (g_peer_session_count_seen[peer] != 0u)
                    {
                        MissionLinkHandshakeReply_t not_ready;
                        uint8_t wire_buf[MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE];

                        mission_link_handshake_reset_relay_leg(peer);

                        not_ready.board_id = peer;
                        not_ready.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
                        not_ready.ready = 0u;
                        (void)mission_link_handshake_pack_reply(&not_ready, wire_buf);
                        mission_link_usart1_write(wire_buf, MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE);
                        g_relay_reply_sent[peer] = 1u;
                        g_relay_reply_last_sent[peer] = not_ready;

                        g_peer_reboot_count++;
                    }
                    g_peer_session_count_seen[peer] = sessions;
                }
            }

            if (mission_link_handshake_should_send_relay_req(peer, now_ms))
            {
                MissionLinkHandshakeReq_t req;
                uint8_t relay_buf[MISSION_LINK_HANDSHAKE_REQ_RELAY_SIZE];

                req.board_id = peer;
                req.protocol_version = MISSION_LINK_PROTOCOL_VERSION;

                relay_buf[0] = MISSION_LINK_HANDSHAKE_MSG_REQ;
                mission_link_handshake_pack_req_payload(&req, &relay_buf[1]);

                /* Addressed to peer's representative team (per
                   mission_link_representative_team()) — sent as team
                   0 the same "not really a team, local bookkeeping
                   only" sender convention channel 2's other sends
                   already use. */
                (void)comms_transport_send(0u, mission_link_representative_team(peer),
                                            LORA_FRAME_TYPE_HANDSHAKE, COMMS_PRECEDENCE_IMMEDIATE,
                                            relay_buf, MISSION_LINK_HANDSHAKE_REQ_RELAY_SIZE);
            }

            {
                MissionLinkHandshakeReply_t relay_reply;

                if (mission_link_handshake_get_relay_reply(peer, now_ms, &relay_reply) &&
                    (!g_relay_reply_sent[peer] ||
                     relay_reply.board_id != g_relay_reply_last_sent[peer].board_id ||
                     relay_reply.protocol_version != g_relay_reply_last_sent[peer].protocol_version ||
                     relay_reply.ready != g_relay_reply_last_sent[peer].ready))
                {
                    /* Each peer's reply is its own distinct USART1
                       frame - the wire format self-identifies which
                       board each reply is from (Reply_t.board_id), so
                       forwarding N peers' status to the laptop is N
                       separate sends, no format change needed (see
                       mission_link_handshake.h's own doc on this). */
                    uint8_t wire_buf[MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE];

                    (void)mission_link_handshake_pack_reply(&relay_reply, wire_buf);
                    mission_link_usart1_write(wire_buf, MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE);

                    g_relay_reply_sent[peer] = 1u;
                    g_relay_reply_last_sent[peer] = relay_reply;
                }
            }
        }
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
    g_roster_applied_count = 0u;
    mission_link_clock_reset();
    mission_link_map_reset();

    for (i = 0u; i < NUM_TEAMS; i++)
    {
        g_roster_applied_for_team[i] = 0u;
        g_last_sent_status[i].known = 0u;
        g_last_sent_status[i].status = 0u;
        g_last_sent_status[i].equipment_level = 0u;
        g_last_sent_status[i].work_rate = 0u;
        g_last_sent_status[i].loc_x = 0;
        g_last_sent_status[i].loc_y = 0;
        g_last_sent_status[i].last_sent_ms = 0u;
    }

    g_own_reply_sent = 0u;
    g_own_reply_resend_pending = 0u;
    {
        uint8_t i;
        for (i = 0u; i < HANDSHAKE_MAX_BOARDS; i++)
        {
            g_relay_reply_sent[i] = 0u;
            g_peer_session_count_seen[i] = 0u;
        }
        g_peer_reboot_count = 0u;
    }
}

void mission_link_task_step(void)
{
    mission_link_usart1_rx_step();
    mission_link_comms_transport_rx_step();
    mission_link_status_tx_step();
    mission_link_handshake_tx_step();
}

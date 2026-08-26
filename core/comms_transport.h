#ifndef COMMS_TRANSPORT_H
#define COMMS_TRANSPORT_H

#include <stdint.h>
#include "lora_frame.h"

/*
 * Single dispatch point for any outgoing message addressed to a team:
 * decides whether it can stay in-process (same board) or has to go out
 * over LoRa (different board). Both CBBA bidding and mission-link relay
 * call this same function - see the unification note this doc opens
 * with. Callers don't need to know or care which path a given message
 * actually took; that's exactly the point of this module.
 *
 * ---------------------------------------------------------------
 * Deviation from the sketch signature this was speced against
 * ---------------------------------------------------------------
 * The original sketch for comms_transport_send() didn't take a sender
 * id. That can't work: comms_transport_poll_received() promises to
 * hand back out_sender_team_id, and there is no way to produce a
 * truthful value there without being told who's sending somehow. This
 * module is deliberately payload-agnostic (it moves opaque
 * frame_type + payload bytes for both CBBA and mission-link without
 * knowing either one's internal layout), so it can't fall back on
 * "peek at a well-known offset inside the payload" the way one
 * specific caller (CBBA) could, since CBBAMessage_t happens to put
 * sender_id first. So: sender_team_id is added as an explicit first
 * parameter to comms_transport_send() below. Every other function
 * keeps the sketch's exact signature.
 *
 * ---------------------------------------------------------------
 * Open question for whoever does the SPI/LoRa RX work (flagged, not
 * silently resolved here - see handoff notes)
 * ---------------------------------------------------------------
 * sender_team_id above is tracked as local bookkeeping for the
 * same-board path (see comms_transport.c) - it is NOT currently
 * placed on the wire, because LoRaFrame_t (lora_frame.h) has no
 * sender field, and LORA_FRAME_MAX_PAYLOAD is sized exactly to
 * CBBAMessage_t's 144 bytes with zero spare bytes to smuggle one in
 * without either shrinking a payload type or growing that constant -
 * a decision that belongs to whoever owns lora_frame.h/cbba.c, not to
 * this file. Concretely: a genuinely separate receiving board cannot
 * recover sender identity from the raw over-the-air bytes alone
 * today. CBBA already sidesteps this at its own layer (sender_id is
 * the first field of CBBAMessage_t); mission-link (Prompt 3) will
 * need to do the same, or this constraint needs revisiting once real
 * LoRa RX exists to receive against.
 *
 * ---------------------------------------------------------------
 * Channel contention model
 * ---------------------------------------------------------------
 * A same-board send does not get to be free. Every send - in-process
 * or LoRa - advances one shared "channel busy until tick" value by a
 * cost proportional to the bytes it puts on the (real or simulated)
 * air, and queues behind whatever is already occupying the channel
 * rather than landing instantly. See comms_transport.c for the exact
 * formula and rationale; the point is that a board-0-only benchmark
 * (where every send happens to be same-board) must pay the same
 * aggregate channel cost a real spread-out N-board run would, or its
 * numbers don't transfer. comms_transport_channel_busy_until_tick()
 * exposes the current value read-only so tests (and, later,
 * benchmark/scoring code) can observe it without reaching into this
 * module's static state.
 *
 * ---------------------------------------------------------------
 * Handoff notes (main_board.c is intentionally not touched here)
 * ---------------------------------------------------------------
 * 1. This module needs to know which physical board it's running on
 *    before comms_transport_init() is called. It reads that from
 *    g_comms_transport_this_board_id (declared below, defaults to 0
 *    so laptop/desktop single-board builds and tests need zero setup).
 *    Whoever wires up main_board.c's usermain() needs to set that
 *    variable from this board's real identity (DIP switch read, a
 *    per-board build define, whatever the bring-up plan already uses
 *    to distinguish boards) BEFORE calling comms_transport_init() -
 *    one line, e.g.:
 *        g_comms_transport_this_board_id = <this board's real id>;
 *        comms_transport_init();
 * 2. comms_transport_poll_received() is non-blocking and does no
 *    waiting/sleeping of its own - if CBBA's receive loop or
 *    mission-link's RX task (Prompt 3) needs to run on a schedule,
 *    that's a µT-Kernel task/cyclic-handler concern for main_board.c
 *    to set up, not something this file creates.
 */

/* This board's own id (team_id / 5 - see comms_transport_board_id_for_team()
   below). Must be set before comms_transport_init() is called; see
   handoff note 1 above. Defaults to 0. */
extern uint8_t g_comms_transport_this_board_id;

void comms_transport_init(void);

/* team_id / 5 - this is the whole board-assignment rule, don't add a
   lookup table for something this cheap to compute. */
uint8_t comms_transport_board_id_for_team(uint8_t team_id);

/* Placeholder comms_transport_poll_received() fills out_sender_team_id
   with for anything that arrived over the physical RX path (see
   comms_transport_deliver_received() below) - see this header's own
   "Open question" note above: LoRaFrame_t has no sender field, so a
   genuinely separate receiving board cannot recover sender identity
   from the wire. Every real caller of comms_transport_poll_received()
   already ignores this output in favor of a payload-embedded
   identifier instead (board_id/team_id inside the actual
   MissionLink*_t/CBBAMessage_t struct itself) - confirmed by checking
   every dispatch site, not assumed. */
#define COMMS_TRANSPORT_UNKNOWN_SENDER ((uint8_t)0xFFu)

/*
 * The real physical-RX counterpart to comms_transport_send()'s
 * same-board loopback path. A real SPI/IRQ RX handler (still pending
 * bring-up - see lora_spi_transmit()'s own doc in comms_transport.c)
 * is expected to accumulate raw bytes off the radio, call lora_frame_
 * try_parse() itself once a complete frame is available (same
 * accumulate-then-try-parse pattern mission_link_task.c's own USART1/
 * LoRa RX steps already use, since byte-level stream framing is the
 * physical layer's job, not this module's), and call this function
 * once parsing succeeds - mirroring exactly what comms_transport_
 * send()'s same-board path already does internally (pack -> reparse
 * -> push), just with the parse step happening in the RX handler
 * instead of here.
 *
 * sender_team_id is always recorded as COMMS_TRANSPORT_UNKNOWN_SENDER
 * (see that macro's own doc) - not a limitation specific to this
 * function, the wire format itself has no sender field.
 *
 * Returns 0 on success, -1 if the inbox was full (dropped, same
 * "full inbox = report failure" policy comms_transport_inbox_push()
 * already has for the same-board path).
 */
int comms_transport_deliver_received(uint8_t frame_type, const uint8_t *payload,
                                      uint8_t payload_len);

/* Total number of boards this build is compiled for: ceil(NUM_TEAMS /
   COMMS_TRANSPORT_TEAMS_PER_BOARD) - see comms_transport.c's own doc
   on this function for why it's a function and not a header macro.
   THE single source of "how many boards exist" for anything that
   needs to loop over every board - mission_link_task.c's incident
   relay loop and mission_link_handshake.c's per-peer state (added
   alongside this function, in the same change, specifically so
   neither ever computes board count its own separate way again) both
   call this instead of re-deriving it. Change COMMS_TRANSPORT_TEAMS_
   PER_BOARD (a -D, see comms_transport.c) or NUM_TEAMS
   (core/mission_state.h) to change how many boards this project
   targets - nothing else needs editing to add or remove a board. */
uint8_t comms_transport_board_count(void);

/* Routes to receiver_team_id. If comms_transport_board_id_for_team(receiver_team_id)
   equals g_comms_transport_this_board_id, delivers via an in-process
   queue. Otherwise, calls out to lora_spi_transmit() (extern, NOT
   implemented here - that's the separate SX1262 SPI driver work,
   still pending bring-up; a weak default in comms_transport.c lets
   this file link and run standalone until that driver exists).
   payload_len must not exceed LORA_FRAME_MAX_PAYLOAD (lora_frame.h) -
   both paths pack through lora_frame_pack(), which enforces that same
   ceiling.

   sender_team_id: see "Deviation from the sketch signature" above.

   Returns 0 on queued/sent, non-zero on failure (e.g. payload_len too
   large, or the local inbox is full on the same-board path). */
int comms_transport_send(uint8_t sender_team_id, uint8_t receiver_team_id,
                          uint8_t frame_type, const uint8_t *payload,
                          uint8_t payload_len);

/* Non-blocking poll for anything addressed to a local team on this
   board. Returns 1 and fills out params if something's waiting, 0 if
   not. Used identically by CBBA's receive loop and by mission_link's
   RX task (Prompt 3) - same function, different callers. Drains one
   shared per-board inbox (this board may host up to 5 local teams;
   this function doesn't try to guess which one a given message was
   really for - that's the caller's job, same as it already is for
   CBBA today).

   out_payload must have room for at least LORA_FRAME_MAX_PAYLOAD bytes. */
int comms_transport_poll_received(uint8_t *out_sender_team_id,
                                   uint8_t *out_frame_type,
                                   uint8_t *out_payload,
                                   uint8_t *out_payload_len);

/* Read-only view of the shared channel-busy-until value (see "Channel
   contention model" above). Not part of the original sketch; added
   because the channel-busy requirement is only meaningful if it's
   observable - tests and future benchmark/scoring code need this to
   confirm every send actually pays a cost, not just same-board ones. */
uint32_t comms_transport_channel_busy_until_tick(void);

#endif /* COMMS_TRANSPORT_H */

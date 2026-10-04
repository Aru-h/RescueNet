#ifndef CBBA_H
#define CBBA_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "mission_state.h"
#include "allocator.h"
#include "scoring.h"
#include "team.h" 

/*
 * How long one per-tick CBBA bidding cycle actively waits for a mesh
 * reply from the other board's teams (see cbba_run()/cbba.c) before
 * finalizing with whatever consensus state has been ingested so far.
 *
 * RECALIBRATED against real numbers (was 530 ms, documented at the
 * time as "a starting SF7 round-trip budget - a placeholder until
 * whoever owns the SPI/radio driver settles on the real spreading
 * factor... at which point this number should be recalibrated" - that
 * SF/BW/CR/preamble/header/CRC choice is now locked in core/lora_
 * radio_params.h, so this is that recalibration, same spirit as
 * COMMS_TRANSPORT_CHANNEL_BYTES_PER_TICK's own calibration note in
 * comms_transport.c):
 *
 *   cbba_broadcast_local_teams() sends up to one frame per LOCALLY-
 *   hosted team (5, see comms_transport.c's COMMS_TRANSPORT_TEAMS_PER_
 *   BOARD) to the other board every round it has something to report.
 *   The other board's own worst-case burst is therefore up to 5
 *   frames, serialized one at a time over this single half-duplex
 *   channel. This 1250 ms was calibrated for the earlier dense, slot-
 *   indexed CBBA frame (101-byte payload, 129 bytes on air): by the
 *   SX126x time-on-air formula at SF7/BW125/CR4:5/preamble 8/explicit
 *   header/CRC on (core/lora_radio_params.h) one frame is ~215.3 ms,
 *   so 5 frames is ~1076.5 ms, plus ~15% margin and rounding.
 *
 *   KNOWN GAP since the wire format became incident-keyed (core/cbba.c
 *   CBBAWireHeader_t, review finding H2): the frame is now variable
 *   length, 42 + 5 bytes per ASSIGNED incident. Up to 11 assigned
 *   incidents it is no bigger than the old frame, so the budget above
 *   holds; the worst case (all 20 assigned: 142-byte payload, 170 on
 *   air, ~276.7 ms) makes a full 5-frame burst ~1383.5 ms, which this
 *   timeout does not cover. A reply that misses the window is not lost -
 *   it stays in the inbox and is ingested at the start of the next
 *   round - it just lands a tick late. The timeout was deliberately NOT
 *   raised to fit: MISSION_LINK_HEARTBEAT_INTERVAL_MS (mission_link.h)
 *   must stay a whole multiple of it (5000 = 4 x 1250, enforced by a
 *   _Static_assert in mission_link.c) and is mirrored by telemetry_
 *   bridge.py, so changing it is a link-timing decision of its own.
 *
 *   This still ONLY fits comfortably inside the 1x (36 s/tick) and 10x
 *   (3.6 s/tick) sim-speed tick budgets (1250 ms is ~3.5%/~35% of
 *   each). At 60x (0.6 s/tick), 1250 ms is itself already ~2x the
 *   ENTIRE tick budget - a real, disclosed limitation, not something
 *   silently patched by shrinking this back below what a genuine
 *   5-frame burst needs (that would reintroduce the exact bug this
 *   timeout exists to prevent - see docs/reports/AUDIT.md item #3's history in
 *   cbba_ingest_remote_mesh_replies()). 60x-speed CBBA over a real
 *   LoRa link is not achievable without either a faster PHY (shorter
 *   airtime per frame) or dropping full-fidelity board-to-board CBBA
 *   at that speed in favor of laptop-only simulation.
 *
 *   Note also that core/cbba.c's send-on-change gate (CBBALastBroadcast_t)
 *   means a converged/steady-state round often gets ZERO replies from
 *   the peer for a given team that tick (nothing changed, so nothing
 *   was sent) - this board cannot distinguish that from "still in
 *   flight" without an ack protocol neither side has, so it still
 *   waits out the full CBBA_ROUND_TIMEOUT_MS in that case, same as
 *   before send-on-change existed. That cost is CPU/tick-latency, not
 *   extra radio airtime (cbba_wait_poll() actively services the radio
 *   during the wait, so it isn't pure idle spin) - the airtime
 *   reduction from CBBALastBroadcast_t is real, but a converged round's
 *   wall-clock cost is not reduced by it, and won't be without a
 *   receive-side signal that a "no change" decision was made.
 *
 * A #define, not a static const, specifically so it stays easy to grep
 * for and swap in one place.
 *
 * This is the NOMINAL real-time bound. It is enforced as a count of wait
 * steps (CBBA_REPLY_WAIT_STEPS below), not against a clock: sim ticks
 * don't correspond to real elapsed time on hardware (BOARD_TICK_DELAY_MS
 * is 0 by default - flat out, unpaced - see board/STM32N6570-DK/
 * main_board.c), and a wall-clock read inside the simulation would make
 * results depend on timing.
 */
#define CBBA_ROUND_TIMEOUT_MS 1250u

/*
 * The reply wait is bounded by a COUNT of wait steps, not by reading a
 * clock: core/ makes no wall-clock read anywhere on the simulation path,
 * so the same inputs always run the same number of wait iterations
 * regardless of CPU speed, kernel tick resolution (10 ms, +1 tick on
 * every timeout) or task preemption.
 *
 * CBBA_REPLY_WAIT_STEP_MS is the NOMINAL platform time of one step. Each
 * call of cbba_wait_poll() below that finds nothing to ingest is one step,
 * and the platform decides what a step physically is: the board's hook
 * services the radio and then yields for about one step; the two-board
 * harness pumps its socket and sleeps one step; the weak default does
 * nothing, so the wait ends after CBBA_REPLY_WAIT_STEPS calls with no
 * delay at all. CBBA_REPLY_WAIT_STEPS * CBBA_REPLY_WAIT_STEP_MS equals
 * CBBA_ROUND_TIMEOUT_MS, so on a platform that honours the nominal step
 * the real-time bound is unchanged.
 */
#define CBBA_REPLY_WAIT_STEP_MS 10u
#define CBBA_REPLY_WAIT_STEPS   (CBBA_ROUND_TIMEOUT_MS / CBBA_REPLY_WAIT_STEP_MS)
_Static_assert((CBBA_ROUND_TIMEOUT_MS % CBBA_REPLY_WAIT_STEP_MS) == 0u,
               "CBBA_ROUND_TIMEOUT_MS must be a whole number of wait steps");

/*
 * Overrides the wait-step budget (default CBBA_REPLY_WAIT_STEPS) for a
 * platform whose real step takes longer than CBBA_REPLY_WAIT_STEP_MS. Call
 * once at boot, before cbba_run(); 0 is treated as 1. Never called on the
 * laptop build, so PC results do not depend on it.
 */
void cbba_set_reply_wait_steps(uint32_t steps);

/*
 * Elapsed-time source for link-side pacing (the status heartbeat interval,
 * handshake retry/timeout) - NOT used by core/ for CBBA's reply wait any
 * more (see CBBA_REPLY_WAIT_STEPS above), and never to be read inside
 * simulation code. core/ is freestanding (no <time.h>, no OS calls -
 * verified by this project's `make check-board`), so this can't call
 * tk_get_otm() directly. cbba.c provides a weak default (same pattern as
 * comms_transport.c's lora_spi_transmit weak stub) that advances a
 * simulated millisecond counter per call, suitable only for the laptop
 * build and standalone selftests; the board overrides it with a kernel
 * timer (board/STM32N6570-DK/main_board.c).
 */
uint32_t cbba_now_ms(void);

/*
 * One wait step of cbba_ingest_remote_mesh_replies()'s reply wait, called
 * whenever the inbox held no CBBA frame; the call counts as one step of
 * CBBA_REPLY_WAIT_STEPS (see above). The weak default does nothing
 * (laptop build, selftests). The two-board CBBA harness pumps its socket
 * and sleeps one step. Board firmware overrides it to poll the radio,
 * dispatch queued TX and yield for about one step: that wait runs inside
 * sim_tick(), where nothing else services the radio, so without it this
 * board's own bids stayed queued until the wait ended and no reply could
 * be received during it - two boards each waited out the full
 * CBBA_ROUND_TIMEOUT_MS for bids the other had not sent yet.
 */
void cbba_wait_poll(void);

void cbba_build_bundle(uint8_t team_id);
void cbba_run(void);
void cbba_improve_routes(void);
void cbba_sync_mission_execution_twin(void);
uint32_t cbba_get_two_opt_improvements(void);
uint32_t cbba_get_relocate_improvements(void);
void cbba_reset_improvement_counters(void);

/* Monotonic count of frames cbba_broadcast_local_teams() (core/cbba.c) has
   actually handed to comms_transport_send() - i.e. how many mesh sends the
   send-on-change gate let through, as opposed to skipping because neither
   winner[] nor winning_bid[] changed since the last send for that team (see
   CBBALastBroadcast_t in cbba.c). Reset by cbba_init(). Test-observable
   proxy for verifying the traffic-reduction gate is actually engaging,
   same "expose a counter rather than requiring the caller to instrument
   comms_transport itself" pattern as lora_frame_accept_count() (core/
   lora_frame.h). */
uint32_t cbba_broadcast_send_count(void);

/* Monotonic count of whole CBBA mesh frames cbba_ingest_remote_mesh_
   replies() dropped as malformed or untrusted: wrong length, sender_id
   >= NUM_TEAMS, or a sender_id hosted on this very board (see
   cbba_decode_wire_message() in cbba.c - review finding H5). Never reset. */
uint32_t cbba_wire_reject_count(void);

/* Monotonic count of individual claims dropped from otherwise-valid CBBA
   frames because their winner was neither a team id nor NO_ASSIGNMENT.
   Never reset. */
uint32_t cbba_wire_entry_reject_count(void);

#endif
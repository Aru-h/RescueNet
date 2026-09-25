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
 *   timeout exists to prevent - see AUDIT.md item #3's history in
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
 * On real hardware this must be enforced against a genuine
 * wall-clock timer - a µT-Kernel timer (tk_get_tim() or equivalent),
 * NOT a sim-tick counter (BOARD_TICK_DELAY_MS is 0 by default - flat
 * out, unpaced - see board/STM32N6570-DK/main_board.c - so sim ticks don't
 * correspond to real elapsed time on hardware). See cbba_now_ms()
 * below for where that hardware wiring plugs in.
 */
#define CBBA_ROUND_TIMEOUT_MS 1250u

/*
 * Elapsed-time source backing CBBA_ROUND_TIMEOUT_MS. core/ is
 * freestanding (no <time.h>, no OS calls - verified by this
 * project's `make check-board`), so this can't call tk_get_tim()
 * directly here. cbba.c provides a weak default (same pattern as
 * comms_transport.c's lora_spi_transmit weak stub) with no real
 * clock behind it, suitable only for the laptop/desktop harness and
 * standalone selftests. Whoever wires board/STM32N6570-DK/main_board.c's
 * usermain() should provide a strong (non-weak) override backed by a
 * µT-Kernel timer so CBBA_ROUND_TIMEOUT_MS is enforced in genuine
 * wall-clock ms on real hardware - do that BEFORE cbba_run() is first
 * called, same "set it before init" pattern as
 * g_comms_transport_this_board_id (see comms_transport.h).
 */
uint32_t cbba_now_ms(void);

/*
 * Called on every pass of cbba_ingest_remote_mesh_replies()'s reply wait.
 * The weak default does nothing (laptop build, selftests; the two-board
 * CBBA harness pumps its socket from its cbba_now_ms() instead). Board
 * firmware overrides it to poll the radio and dispatch queued TX: that
 * wait runs inside sim_tick(), where nothing else services the radio, so
 * without it this board's own bids stayed queued until the wait ended and
 * no reply could be received during it - two boards each waited out the
 * full CBBA_ROUND_TIMEOUT_MS for bids the other had not sent yet.
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

#endif
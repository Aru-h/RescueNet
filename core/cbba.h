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
 * 530 ms is a starting SF7 (LoRa spreading factor 7) round-trip
 * budget - a placeholder until whoever owns the SPI/radio driver
 * settles on the real spreading factor for this deployment, at which
 * point this number should be recalibrated against it (same spirit
 * as COMMS_TRANSPORT_CHANNEL_BYTES_PER_TICK's calibration note in
 * comms_transport.c). A #define, not a static const, specifically so
 * it stays easy to grep for and swap in one place.
 *
 * On real hardware this must be enforced against a genuine
 * wall-clock timer - a µT-Kernel timer (tk_get_tim() or equivalent),
 * NOT a sim-tick counter (BOARD_TICK_DELAY_MS is 0 by default - flat
 * out, unpaced - see platform/board/main_board.c - so sim ticks don't
 * correspond to real elapsed time on hardware). See cbba_now_ms()
 * below for where that hardware wiring plugs in.
 */
#define CBBA_ROUND_TIMEOUT_MS 530u

/*
 * Elapsed-time source backing CBBA_ROUND_TIMEOUT_MS. core/ is
 * freestanding (no <time.h>, no OS calls - verified by this
 * project's `make check-board`), so this can't call tk_get_tim()
 * directly here. cbba.c provides a weak default (same pattern as
 * comms_transport.c's lora_spi_transmit weak stub) with no real
 * clock behind it, suitable only for the laptop/desktop harness and
 * standalone selftests. Whoever wires platform/board/main_board.c's
 * usermain() should provide a strong (non-weak) override backed by a
 * µT-Kernel timer so CBBA_ROUND_TIMEOUT_MS is enforced in genuine
 * wall-clock ms on real hardware - do that BEFORE cbba_run() is first
 * called, same "set it before init" pattern as
 * g_comms_transport_this_board_id (see comms_transport.h).
 */
uint32_t cbba_now_ms(void);

void cbba_build_bundle(uint8_t team_id);
void cbba_run(void);
void cbba_improve_routes(void);
void cbba_sync_mission_execution_twin(void);
uint32_t cbba_get_two_opt_improvements(void);
uint32_t cbba_get_relocate_improvements(void);
void cbba_reset_improvement_counters(void);

#endif
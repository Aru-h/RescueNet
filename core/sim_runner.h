#ifndef SIM_RUNNER_H
#define SIM_RUNNER_H

#include <stdint.h>
#include "fault_config.h"

/*
 * Portable simulation lifecycle, extracted from what used to be
 * inline logic in the laptop main.c. Both platform/laptop/main.c and
 * platform/board/main_board.c call these in the same order, so the
 * tick sequence (fault_injector_step -> allocate_all_incidents ->
 * simulation_step) can never silently drift between platforms.
 *
 * This ordering is performance-sensitive: allocate_all_incidents()
 * runs the CBBA auction and is the dominant cost per tick, so any
 * future reordering here has to be made once, in one place, and
 * re-verified against both platforms rather than patched twice.
 */

/* One-time process/boot init: weight reset, equipment matrix
 * validation, baseline fault config. Call once before the first
 * sim_start_seed(). Returns 0 on success, nonzero if the equipment
 * requirement matrix failed validation (caller should treat this as
 * fatal — same as the laptop main.c's existing behavior). */
int sim_runner_init(void);

/* Begin a run for one seed: applies the fault profile, generates the
 * world, and opens the snapshot run. Call once per seed. */
void sim_start_seed(uint32_t seed, FaultProfile_t fault_profile);

/* Advance the simulation by exactly one tick: fault injection, CBBA
 * allocation, simulation step. Call once per tick after
 * sim_start_seed(). Does not touch snapshot/telemetry framing —
 * callers (laptop: --live JSON frame + nanosleep pacing; board: USB
 * telemetry frame) own that, since it's platform-specific presentation,
 * not simulation state. */
void sim_tick(void);

/* Close out the run for the current seed (snapshot_end_run). */
void sim_end_seed(void);

/* Process-level shutdown (snapshot_close_dataset). Call once after
 * all seeds are done. */
void sim_runner_shutdown(void);

#endif

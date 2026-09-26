#ifndef SIM_RUNNER_H
#define SIM_RUNNER_H

#include <stdint.h>
#include "fault_config.h"

/*
 * Portable simulation lifecycle, extracted from what used to be
 * inline logic in the laptop main.c. sim_tick()/sim_end_seed()/
 * sim_runner_shutdown() are still called identically by every
 * caller (laptop, board, laptop/tools/override_selftest.c), so the tick
 * sequence (fault_injector_step -> allocate_all_incidents ->
 * simulation_step) can never silently drift between them.
 *
 * sim_start_seed() is the ONE exception to "every caller does the
 * same thing" — see its own doc and sim_start_seed_board()'s doc
 * immediately below for why, and read both before touching either.
 *
 * This ordering is performance-sensitive: allocate_all_incidents()
 * runs the CBBA auction and is the dominant cost per tick, so any
 * future reordering here has to be made once, in one place, and
 * re-verified against every caller rather than patched twice.
 */

/* One-time process/boot init: weight reset, equipment matrix
 * validation, baseline fault config. Call once before the first
 * sim_start_seed()/sim_start_seed_board(). Returns 0 on success,
 * nonzero if the equipment requirement matrix failed validation
 * (caller should treat this as fatal — same as the laptop main.c's
 * existing behavior). */
int sim_runner_init(void);

/*
 * Begin a run for one seed on a WORLD-AUTHORITY caller: applies the
 * fault profile, generates the world (grid/environment/incidents
 * pools/teams — see generate_world()), and opens the snapshot run.
 * Call once per seed.
 *
 * Callers: laptop/main.c, and laptop/tools/override_selftest.c
 * (which deliberately depends on generate_world() running here —
 * see that file's own top-of-file comment on why it needs a REAL
 * generated world, not a mocked one, to test CBBA override logic
 * against a genuine adversarial bid).
 *
 * DOES NOT CALL generate_world() — READ THIS BEFORE CHOOSING WHICH
 * FUNCTION TO CALL:
 * The boards' main_board.c (DK and Pico) do NOT call this function. They call
 * sim_start_seed_board() instead, which is IDENTICAL to this
 * function except that in place of generate_world() it runs only the
 * board-safe subset of generate_world()'s per-run module resets (see
 * its doc below). This split exists
 * because per the laptop/board mission-link architecture, the
 * laptop is the sole world authority — a board must not generate
 * its own world/team roster locally, it receives them over the
 * mission link (see core/mission_link.h). If you're adding a new
 * caller: a caller that owns/authors world state (desktop tooling,
 * more selftests) belongs on sim_start_seed(); a caller that is
 * itself a board receiving world state from elsewhere belongs on
 * sim_start_seed_board(). Calling the wrong one either regenerates
 * a world a board shouldn't have, or leaves a world-authority
 * caller (like a future selftest) without one it expects.
 */
void sim_start_seed(uint32_t seed, FaultProfile_t fault_profile);

/*
 * Board-only counterpart to sim_start_seed() (see that function's
 * doc first — this one only documents the difference). Applies the
 * fault profile and opens the snapshot run, exactly like
 * sim_start_seed(), but deliberately does NOT call generate_world():
 * a board is not a world authority and must not fabricate its own
 * team/incident/environment state locally — that state arrives over
 * the mission link (MissionLinkRoster_t / MissionLinkIncident_t /
 * MissionLinkFault_t, core/mission_link.h) from the laptop instead.
 * It does run the module resets generate_world() performs that touch
 * only a module's own state (watchdog, mission-execution twin, CBBA
 * improvement counters, recovery manager, fault comms/fault buffer,
 * search-zone bookkeeping), so those start from their intended
 * defaults rather than zero-initialised statics. It must never call
 * anything that clears g_incidents[], the g_teams[] roster/location
 * fields, the map or the sites: all of that may already have arrived
 * from the laptop before the seed starts. See the list in
 * sim_runner.c for what each call touches.
 *
 * ONLY callers: board/STM32N6570-DK/main_board.c and
 * board/Raspberry_Pi_Pico/main_board.c. If you're tempted to
 * call this from anywhere else, stop and check: does that caller
 * receive its world state from a mission-link peer? If not, it
 * wants sim_start_seed(), not this.
 *
 * MAINTENANCE WARNING — this function's body is a deliberate,
 * partial duplicate of sim_start_seed()'s (same fault-profile /
 * snapshot-open lines) plus a hand-picked subset of generate_world()'s
 * module resets. If you change sim_start_seed()'s fault-profile or
 * snapshot-open logic, mirror that change here too (or factor the
 * shared lines into a static helper both functions call); and if you
 * add a module reset to generate_world() (core/simulation.c), decide
 * whether it is board-safe and add it here too if so — do NOT let the
 * two silently diverge on anything other than world generation, or a
 * board build and a laptop build will start a seed in
 * inconsistently-initialized states for no documented reason. There
 * is currently no compiler or test that catches this drift for
 * you; it is only caught by reading both functions side by side.
 *
 * Once per boot only: g_current_tick follows the laptop's clock and
 * is never reset here, so a second seed would start with its tick
 * loop already finished (main_board.c asserts BOARD_SEED_COUNT == 1).
 */
void sim_start_seed_board(uint32_t seed, FaultProfile_t fault_profile);

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

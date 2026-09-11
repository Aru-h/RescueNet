#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>

/*
 * Platform-provided trace sink. Gated at call sites by
 * g_simulation_trace_enabled so it costs nothing when trace is off.
 * Laptop implementation: vfprintf to stderr (matches every other
 * [DIAG ...] line in this codebase; app.py's batch endpoint parses
 * stderr specifically, not stdout - see debug.c for why).
 * Board implementation: routes to UART/USB log sink (TODO(usb)).
 * Signature intentionally mirrors printf() so existing call sites need
 * only a name change, not a rewrite.
 */
void debug_trace(const char *fmt, ...);

void print_world_state(void);
void initialize_test_scenario(void);
/*
 * Static environment layout - buildings, epicenter - as a one-shot
 * STATICENV JSON line on stdout. Unlike debug_emit_live_frame(), this
 * doesn't change tick-to-tick (buildings/roads/districts are fixed at
 * world-gen time), so it's meant to be called once per run, not every
 * tick, keeping the dashboard's per-tick LIVEFRAME payload smaller.
 */
void debug_emit_static_environment(void);
void debug_emit_live_frame(uint32_t tick);

/*
 * Priority 1 repro/verification harness: deterministically reproduces
 * "path lost mid-route" for a team on an active TASK_POOL_INCIDENT/
 * TASK_POOL_SYSTEM task (the move_team() branch that historically had
 * no `else` for find_next_path_step() returning "no path") and reports
 * whether the team recovers. Returns 0 if the team was un-stuck within
 * the tick budget, non-zero if it froze for the whole run (bug present).
 * Desktop test tooling only, invoked via `--repro-priority1`.
 */
int debug_run_priority1_repro(void);

/*
 * Large-scale natural-condition sweep for the Priority 1 fix: runs
 * `seed_count` seeds of `ticks_per_seed` ticks each (via the same
 * sim_start_seed()/sim_tick()/sim_end_seed() path a normal run uses -
 * no synthetic wall-off), and after every tick checks every team for
 * a "frozen" signature: STATUS_EN_ROUTE with no location change for
 * far longer than the highest possible single-cell travel cost. Any
 * such streak means a team is stuck exactly the way the pre-fix
 * move_team() task branch used to leave it. Prints a running progress
 * line, flags each frozen streak as it's found (seed, team, tick
 * range, length), and ends with a PASS/FAIL verdict plus totals for
 * g_probe_no_path_hits and g_diag_safezone_fallback_count. Returns 0
 * if no freeze was ever detected, non-zero (count of frozen streaks
 * found, capped at 255) otherwise. Desktop test tooling only, invoked
 * via `--sweep-priority1 [first_seed] [seed_count] [ticks_per_seed]
 * [--fault-profile=...]` (all positional args optional, defaulting to
 * 1 / 50 / 10000 / combined).
 */
int debug_run_priority1_sweep(uint32_t first_seed, uint32_t seed_count, uint32_t ticks_per_seed, int fault_profile);

void debug_print_generator_evaluation(uint32_t tick,
                                      uint8_t load,
                                      uint32_t probability,
                                      uint32_t roll,
                                      uint8_t spawned,
                                      uint8_t available_teams,
                                      uint8_t en_route_teams,
                                      uint8_t on_scene_teams,
                                      uint8_t resting_teams,
                                      uint8_t open_incidents,
                                      uint8_t assigned_incidents,
                                      uint8_t in_progress_incidents,
                                      uint8_t active_system_tasks,
                                      uint16_t degraded_cells,
                                      uint32_t next_evaluation_tick);

void debug_print_generator_selection(const uint32_t *weights,
                                     uint32_t weight_count,
                                     uint32_t total_weight,
                                     uint32_t type_roll,
                                     uint8_t selected_type);

#endif
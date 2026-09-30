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
 * Per-cell terrain (travel cost, navigable, flood, road class,
 * road_blocked, district id) as a one-shot STATICMAP JSON line on
 * stdout, compact rows of hex (see laptop/debug.c). Terrain is normally
 * static once generated, so this only actually re-prints when a cell's
 * value differs from what was last emitted (flood and road_blocked are
 * the fields that can change mid-run, same idea as debug_emit_static_
 * environment()'s aftershock re-emit). Desktop test tooling only (used
 * by the live dashboard) - not part of the firmware port surface, so no
 * platform.h wrapping. Board code never calls this. This printing path
 * only - the CSV terrain export is untouched.
 */
void debug_emit_static_map_if_changed(void);

/*
 * Part 4: sim-mode manual override, reachable ONLY via --live's own
 * stdin - never the board's USART1 path (laptop/tools/app.py's
 * /api/run-live/override route writes lines here, a completely
 * separate channel from /api/board/override's binary
 * MissionLinkOverride_t frame over a real serial link; the live board
 * path is untouched by this feature). Plain text lines, not the
 * binary wire format: this is a trusted local pipe between app.py and
 * this process, not a noisy serial link that needs magic-byte resync
 * and a checksum.
 *
 *   OVERRIDE <requested_tick> <team_id> <incident_id>\n
 *   OVERRIDE_CLEAR <requested_tick> <team_id>\n
 *
 * debug_live_override_init() resets the line-assembly buffer and must
 * be called once before the tick loop starts, live_mode only.
 * debug_live_override_poll(tick) does one non-blocking read of
 * whatever is currently available on stdin - it never blocks the tick
 * loop, and a closed/EOF stdin just means no further overrides are
 * possible, never an error (same policy laptop/main.c's existing
 * --mission-feed stdin handling already uses) - assembles complete
 * lines, validates and applies each one, and prints one OVERRIDEACK
 * JSON line per non-empty line read - every line gets exactly one
 * reply, applied or rejected with a reason (see laptop/debug.c:
 * bad_team, unknown_incident, system_task, incident_finished,
 * team_unavailable, stale_tick, future_tick, bad_format,
 * unknown_command). An empty line is the one exception - ignored
 * silently, not a request at all. bad_format is a recognized command
 * word (OVERRIDE/OVERRIDE_CLEAR) whose arguments don't parse cleanly -
 * wrong field count, a non-numeric field, a field too large for its
 * type, trailing junk after the last field, or a line longer than the
 * line-assembly buffer (discarded up to its '\n', one ack for the
 * whole line, no desync into the next one). unknown_command is
 * anything else. Both acks report team_id/incident_id as JSON null
 * (the line's own shape never established real ones) and
 * requested_tick as null too unless that much of the line did parse.
 * system_task means the id is real (it exists in g_system_tasks, and
 * shows up on the dashboard map like any other marker) but core/
 * cbba.c's own pin-validation (cbba_apply_override_pin(), called
 * every tick from cbba_build_bundle()) only ever looks in g_incidents
 * - pinning a team to a system task's id would validate here, ack
 * "applied", and then self-clear on the very next tick with no
 * further signal, exactly the "silently dropped" failure mode
 * overrides must never have, so it is rejected up front instead.
 * Rejection reasons
 * deliberately never reveal ground-truth fault state (core/fault_
 * config.h's TeamFaultState_t.active_mask) - only what the system
 * itself has already concluded (the recovery manager's degraded flag,
 * peer consensus ELEVATED tier) is used to reject a team as
 * unavailable, the same ground-truth-vs-observed distinction
 * docs/architecture/CONSENSUS_DESIGN.md draws everywhere else in this project.
 *
 * Lifecycle once applied - three states, visible live off LIVEFRAME's
 * per-team override/override_incident_id/has_dest/dest_id and the
 * matching incident's own status/assigned_team (no extra state added
 * for this - all of it already exists for other reasons):
 *
 *   1. pinned    - override=1. cbba_apply_override_pin() (core/cbba.c)
 *                  is force-injecting the incident into this team's
 *                  bundle every tick until CBBA converges on it.
 *   2. committed - override=0 again, but has_dest=1 with dest_id still
 *                  the pinned incident: CBBA has converged (the
 *                  incident left INC_OPEN, typically just a few ticks
 *                  after the pin - see cbba_apply_override_pin()'s own
 *                  doc, "no longer actionable"), and normal sim state
 *                  (team->assigned_task_pool/slot, incident->
 *                  assigned_team) is now carrying the assignment, not
 *                  the override. The pin's job is done here - it only
 *                  ever guarantees this initial commitment, nothing
 *                  past it.
 *   3. an ending, one of two - and the override has no say in which:
 *        - resolved: the incident finishes normally (status becomes
 *          INC_RESOLVED, and in practice its slot is freed around the
 *          same tick, so it simply stops appearing in the incidents
 *          array).
 *        - interrupted: the sim's OWN normal stamina/rest handling
 *          (core/incident.c, core/team.c - nothing override-specific,
 *          the exact same thing that can happen to any assignment,
 *          overridden or not) pulls the team off before it finishes -
 *          has_dest drops, the incident's status resets to INC_OPEN
 *          and assigned_team resets to NO_ASSIGNMENT for a future
 *          round to pick back up, possibly by a different team. This
 *          is not a bug and not an override failure: the override
 *          never promised anything past commitment. Once this
 *          happens the override is simply over - nothing re-pins the
 *          team automatically, so a fresh OVERRIDE is needed to force
 *          the same (or now-reopened) incident again.
 */
void debug_live_override_init(void);
void debug_live_override_poll(uint32_t tick);
/* --fast-forward support: load an override script (see debug.c) and
 * replay its entries for one tick, silently. */
int debug_live_override_load_script(const char *path);
void debug_live_override_replay_script(uint32_t tick);

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
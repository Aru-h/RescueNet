# Changelog

## Merge: tron_merged_step11 (Dashboard: Incident Journal, PAR Timer, Map Visualization)

Reconciled a sibling branch (11 separate merge/feature steps in another
session) that forked before the fog-of-war/search-zone merge and the
gateway-hang, CSV-sentinel, stale-read, fault-compounding, and
active-mask fixes documented below - confirmed via a file-by-file
`diff -u` against the true original, not assumed. Kept every one of
those fixes; layered in what was genuinely new on the other side.

### Merged (real, independent work - not present in this branch before)

- `incident.c` - two related `RETURN_TO_SAFE_ZONE` duplicate-task
  fixes: `create_return_to_safe_zone_task()` now checks for an
  existing pending task before creating a new one, and
  `abandon_task_for_recovery()` short-circuits if the team's current
  task is already a `RETURN_TO_SAFE_ZONE` task (previously it would
  clear the assignment and immediately recreate an equivalent task,
  destroying the evidence the dedup check needed).
- `g_diag_stage_completions[NUM_TEAMS]` - new counter tracking every
  stage handoff (not just final incident resolution), incremented in
  `release_main_task_team()`. Threaded through `main.c`'s DIAG
  THROUGHPUT summary and `debug_emit_live_frame()`'s LIVEFRAME output.
- `debug_emit_static_environment()` - new one-shot STATICENV JSON
  frame (buildings, epicenter) sent once per `--live` run rather than
  every tick, since that layout doesn't change after world generation.
- `hazard_cells` array in LIVEFRAME - streams only cells that are
  actually blocked/hazardous/damaged (not all 1296 grid cells) for
  live hazard-map rendering.
- `app.py` - `STATICENV` SSE line handling (dispatches to a
  `static_env` message type alongside the existing `frame` type).
  Also fixed a stale comment describing team-name indexing that no
  longer matched the actual `% len(TEAM_NAMES)` code beneath it.
- `templates/index.html` - taken as the new file wholesale after
  confirming (via the full diff) that all removed lines were
  superseded restructuring, not lost functionality unique to this
  branch. Adds: Incident Journal tab, PAR (Personnel Accountability
  Report) timer per team, static building/epicenter map rendering,
  hazard-cell density-tinted overlay, incident type icons, a
  preview-only drag-to-reassign UI (explicitly not wired to a live
  backend channel - documented as future work in the source), and a
  Chart.js update-in-place optimization replacing destroy/recreate
  every frame.

### Fixed as part of reconciling the port

- `debug_trace()` changed from writing to stdout to stderr. Necessary,
  not cosmetic: `app.py`'s batch (non-`--live`) endpoint captures
  stdout/stderr *separately* and only parses `result.stderr` for
  `[DIAG ...]` lines. The new rest-cycle tracing this merge adds uses
  `debug_trace()`, and a naive stdout implementation would have
  silently dropped that data for every non-live run while appearing to
  work fine in `--live` mode (which merges the streams). Verified
  against both code paths, not just live mode.

### Explicitly rejected (confirmed real regressions in the other
### branch, superseded by fixes already in this one)

- `recovery_manager_config.h` / `test_metrics.c` - reverted time
  constants (`RECOVERY_MANAGER_DEESCALATION_TICKS`,
  `STALL_THRESHOLD_TICKS`) to their pre-fix bare-integer values.
- `fault_injector.c` - reverted `active_mask` clearing, the
  wrong-incident-targeting fallback fix, and the fault
  reference-counting fix.
- `incident.c` / `snapshot.c` - lacked the gateway-hang fix and the
  CSV sentinel / stale-read fixes entirely.
- Fog-of-war/search-zone system absent entirely (`search_zones.c/h`,
  `discovered` flag, related `environment.c`/`incident.c` support) -
  confirmed via the merged session's own file timestamps that this
  branch forked before that work existed, not that it was removed.

### Verified

- Multi-seed sweep (1-15) clean, no hangs.
- CSV sentinel and stale-read fixes both still hold post-merge.
- `make check-board` passes against real BSP2 headers.
- Live SSE stream exercised end-to-end through the actual Flask
  server (not just the raw binary): confirmed `STATICENV` emits real
  building/epicenter data, `LIVEFRAME` carries both
  `stage_completions` and populated `hazard_cells`, and `[DIAG]`
  lines route correctly to `diag_line` SSE events.
- Merged `templates/index.html`'s script block is brace/paren-balanced
  (337/337, 515/515).

## Merge: Search-Zone / Fog-of-War System + Stale-Read CSV Fix

### Merged

- Merged a separately-developed session's work: `search_zones.c/h`
  (district-based fog-of-war and Search-team sweep dispatch), a
  fault-tracking refactor (`mission_fault_active_count` on
  `IncidentState_t` replacing per-fault-class `original_priority[]`/
  `original_victim_count[]` on `TeamFaultState_t`, fixing a
  compounding bug where a second mission fault landing on an
  already-faulted incident would capture the wrong "original" value to
  restore), and 3 new CSV telemetry columns (`incident_discovered`,
  `team_zone_id`, `team_zone_trust`).
- Kept `NUM_TEAMS=10` (the merged session's own baseline used 5); this
  is a real, verified difference in assumptions between the two
  branches, not a leftover default.
- Kept this branch's existing `--live` dashboard implementation as-is
  where the two branches independently built overlapping
  functionality; dashboard-adjacent discrepancies are expected and
  out of scope for this merge (a separately improved dashboard exists
  elsewhere).
- Verified the specific scenario the merged session's own notes
  flagged as never tested - two teams sharing the same specialization
  (Search) under `NUM_TEAMS=10` - actually works: both swept
  different, overlapping-but-not-identical zone sets over a 2000-tick
  run with no deadlock.

### Fixed

- **Stale team-state read in `EVENT_INCIDENT_RESOLVED` snapshot
  logging** (`snapshot.c`, `complete_task()` in `incident.c`): by the
  time a task's completion is detected and logged, the team it was
  assigned to can already have been dispatched to a new task earlier
  in the same tick (`allocate_all_incidents()` runs before
  `simulation_step()`'s completion detection). `snapshot_record()`
  derived incident-detail columns from the team's *live* state, so
  the logged row described the team's *next* assignment instead of
  the one that just resolved. Rare before this merge (2/68 resolved-task
  log rows affected in a 2000-tick run) but became the near-universal
  case afterward (88/88), almost certainly because the new search-zone
  system makes many more system tasks available for immediate
  same-tick redispatch. Fixed by adding `snapshot_record_resolved()`,
  which takes the resolving task's own state directly rather than
  re-deriving it from the team - `complete_task()` now uses this
  instead of the general `snapshot_record()`. Verified: post-fix, 78/88
  resolved-task rows show real incident IDs and the remaining 10
  correctly correspond to genuine system-task completions (which are
  supposed to show no incident), matching the diag counters exactly.

### Verified

- Multi-seed sweep (1-15) clean, no hangs.
- CSV sentinel fix still holds (zero `4294967295` occurrences).
- `make check-board` passes against real BSP2 headers with the merged
  and fixed sources.

## Bug Fixes: Evacuation Gateway Hang, CSV Sentinel Corruption

### Fixed

- **Multi-seed run hang** (`incident.c`, `initialize_evacuation_gateways()`):
  the deterministic gateway-site lookup (`environment_find_gateway_site()`)
  could return the same colliding cell forever on retry, since it has no
  randomness. Seed 2 (gateway index 1) hung indefinitely. Fixed by
  tracking whether the deterministic candidate was already rejected once
  and falling through to the randomized placement branch on retry.
  Verified: seed 2 now completes instantly; swept seeds 1-10 clean.
- **`cbba_selected_team` showing `4294967295` in the snapshot CSV**
  (`snapshot.c`, `snapshot_record()`): `selected_team` was a signed `int`
  defaulting to sentinel `-1`, but printed with `%u`, reinterpreting the
  bit pattern as `0xFFFFFFFF`. Fixed by using the codebase's existing
  `NO_ASSIGNMENT` (`0xFF`) sentinel convention instead of `-1`, matching
  how `assigned_team`/`assigned_task_slot` are already handled elsewhere.
  Verified: 261 corrupted rows in a 500-tick run before the fix, zero
  after.

## Laptop / Board Platform Split

### Added

- Split the codebase into `core/` (portable simulator, verified
  freestanding-clean for Cortex-M55), `platform/laptop/`, and
  `platform/board/` (STM32N6570-DK).
- Added `core/sim_runner.h/.c` as the single source of truth for the
  per-tick sequence (`fault_injector_step -> allocate_all_incidents ->
  simulation_step`), shared by both platforms.
- Added `platform/board/main_board.c`, `debug.c`, `snapshot.c` -
  compiled and verified against the real µT-Kernel 3.0 BSP2 headers
  (`github.com/tron-forum/mtk3_bsp2`, official STM32N6570-DK support).
- Added `make check-board`, which compiles `core/` + `platform/board/`
  for Cortex-M55 against real BSP headers when `vendor/mtk3_bsp2` is
  present (see `vendor/README.md`), catching libc/OS dependency
  regressions before they ever reach hardware.

### Removed

- Removed the `sweep`/weight-sensitivity-analysis CLI mode and
  `analysis.c`/`analysis.h` (unused dev tooling, not part of the
  production simulator).
- Removed committed run artifacts (logs, CSVs, stale binaries) and
  added `.gitignore` to keep them out going forward.

### Verified

- Rebuilt from a fresh extraction of the packaged tree; 200-tick run
  byte-identical to the pre-restructure original on both stdout and
  stderr.

## Operational Fault Injection and Watchdog Refinement

### Added

- Added a generic operational fault injection subsystem with deterministic fault profiles.
- Added fault-state tracking to teams so the simulator can model mobility, communication, resource, environment, and mission effects.
- Added optional `--fault-profile` CLI support.

### Changed

- The watchdog now emits recommendations from evidence instead of directly mutating CBBA state.
- Simulation applies watchdog recommendations and fault effects while preserving the existing snapshot schema.

### Verified

- Built the simulator and ran a short smoke test.

## Offline Snapshot Labeling Pipeline

### Added

- Added a standalone offline label generator for `tron_snapshot_dataset.csv` that writes a separate labeled CSV.
- Added a horizon-based recovery label that marks whether a team returns to `STATUS_AVAILABLE` within `N` ticks after a snapshot.
- Added recovery metadata columns to the labeled dataset without changing the original snapshot export schema.

### Documented

- Documented the snapshot-labeling methodology, including the run/team grouping rule and the tier-based ready-stamina threshold used as recovery metadata.

### Verified

- Built the simulator and labeler with `make`.

## Procedural Environment Milestone

### Added

- Added persistent damage state to generated buildings, road cells, and infrastructure cells.
- Added construction quality to generated building records.
- Added infrastructure registration for safe zones and evacuation gateways.
- Added a deterministic earthquake event with epicenter, intensity, district vulnerability, construction quality, and distance attenuation.
- Added road blockage state and high-cost blocked-road routing behavior.
- Added object-level spawned-incident masks so damaged objects do not emit the same incident type repeatedly.
- Added environment summary output for earthquake epicenter, intensity, damaged buildings, blocked roads, and damaged infrastructure.

### Changed

- Incident generation now originates from damaged environment objects instead of random coordinates.
- Collapse, confined-space, gas leak, chemical exposure, fire, casualty, survivor, and search incidents now select eligible damaged buildings, roads, or infrastructure sources.
- World setup now registers safe zones and gateways as infrastructure before applying earthquake damage, then initializes the safety grid from the damaged environment.
- Blocked roads now influence routing through high travel cost while remaining reachable for incident response.

### Preserved

- Existing simulator CLI remains `./tron [first_seed] [seed_count] [ticks_per_seed]`.
- Existing `TeamState_t`, `IncidentState_t`, `SafeZone_t`, CBBA bundle/path structures, and snapshot CSV schema remain compatible.
- Existing metrics and snapshot flows continue to observe the same state transitions.
- Teams still move one grid step per simulator tick, preserving current movement semantics.

### Verified

- Built the simulator with `make`.
- Ran the smoke simulation with `make smoke`.

## Initial Procedural Environment Milestone

### Added

- Added a procedural environment subsystem in `environment.c` and `environment.h`.
- Added deterministic district generation for rescue base, residential, industrial, medical, civic, and waterfront districts.
- Added a generated road network with arterial, collector, and local road classes.
- Added typed building records with district and risk metadata.
- Added per-cell environment metadata for base safety, travel cost, safe-zone suitability, and navigability.
- Added environment-aware safe-zone candidate selection.
- Added environment-aware evacuation gateway placement.
- Added environment-aware incident placement with type-specific district preferences.
- Added `find_path_cost(...)` wrapper for route-cost consumers that do not need the next step.
- Added a `Makefile` with `make`, `make smoke`, and `make clean`.
- Added `ARCHITECTURE.md` describing the new subsystem and compatibility boundaries.

### Changed

- `generate_world(...)` now creates the procedural environment before initializing the safety grid.
- `initialize_grid(...)` now initializes each cell from environment base safety instead of a flat value of `100`.
- `find_next_path_step(...)` now uses environment navigability and travel cost while preserving its existing signature.
- CBBA distance scoring now uses weighted route cost through the same path API.
- Return-to-safe-zone selection now uses environment-aware path cost instead of Manhattan distance.
- Generated incidents now land on environment-selected sites instead of uniformly random grid cells.
- Final debug output now includes an environment summary with district, road, navigable-cell, and building counts.

### Preserved

- Existing simulator CLI remains `./tron [first_seed] [seed_count] [ticks_per_seed]`.
- Existing `TeamState_t`, `IncidentState_t`, `SafeZone_t`, CBBA bundle/path structures, and snapshot CSV schema remain compatible.
- Existing metrics and snapshot flows continue to observe the same state transitions.
- Teams still move one grid step per simulator tick, preserving current movement semantics.

### Verified

- Built the simulator with `gcc -std=c11 -Wall -Wextra -Wpedantic`.
- Ran a seeded smoke simulation with `./tron 1 1 200`.

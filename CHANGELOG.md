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


20th August 2026
Merge summary
=============
You uploaded files-2.zip containing 9 files from a separate branch of work
(diagnostics/tracing flags, a fog-of-war "god/operator view" toggle, a
sweep-task anti-thrash cooldown, and a real capability-matrix bug fix).
I diffed every one of them against what I'd already built and merged as
follows:

TRUE MERGES (both sides had changes to reconcile)
--------------------------------------------------
tools__app.py
  - Kept: my handshake gating (/api/status, /api/board/launch,
    /api/board/continue-without-nucleo) and the override route
    (/api/board/override) from this chat.
  - Added: the "--trace" "--diag" CLI flags to both subprocess launch
    points (/api/run and /api/run-live), which is the only real content
    their branch's app.py had (everything else in their version was just
    a pre-handshake baseline, not an intentional removal).

tools_templates__index.html
  - Kept: my real confirmPendingOverride() fetch() implementation.
  - Added: their fog-of-war "god mode / operator view" toggle (button,
    toggleFogView(), and the two incident-filtering call sites) - this
    only works together with the "discovered" field debug.c now emits
    (see below), so both had to come over together.

ADOPTED AS-IS (files I never touched - no merge needed, just verified
they don't conflict with anything I built: no shared-struct changes to
CBBAAgentState_t/TeamState_t/IncidentState_t, no mission_link/comms_
transport touches)
-----------------------------------------------------------------------
platform_laptop__debug.c   -> platform/laptop/debug.c
  Adds "discovered" to the JSON incident/system-task output - required
  by index.html's fog-of-war toggle.

platform_laptop__main.c    -> platform/laptop/main.c
  New --trace / --diag CLI flags (gates g_simulation_trace_enabled and
  the end-of-run [DIAG ...] stderr summary, both off by default).

core__scoring.c            -> core/scoring.c
  Fixes a real bug: CAPABILITY_MATRIX's row/column order didn't match
  the actual Specialization_t enum (SEARCH=0, MEDICAL=1, ...) - Search
  and Medical had swapped data. Re-derived from the original intended
  semantics, not new numbers.

core__simulation.c         -> core/simulation.c
core__search_zones.h       -> core/search_zones.h
core__search_zones.c       -> core/search_zones.c
core__incident.c           -> core/incident.c
  Diagnostic counters (g_diag_discovered_by_type etc.) and a new
  ZONE_SWEEP_RECLAIM_COOLDOWN_TICKS / reclaimed_at_tick field on
  SearchZoneState_t to stop a create/evict thrash loop.

Verification performed after merging
-------------------------------------
- Full laptop build: clean, no errors.
- All core/board files that touch mission-link work re-checked with
  host gcc against the real BSP headers (-fsyntax-only): clean.
- All 5 standalone test suites re-run against the merged sources:
  handshake_selftest.c, mission_link_selftest.c, lora_frame_selftest.c,
  override_selftest.c (rebuilt against the merged core/scoring.c,
  core/incident.c etc.), handshake_selftest.py, telemetry_bridge_
  selftest.py - all pass.
- index.html's merged JS re-parsed for syntax errors: clean, both
  features (override fetch + fog toggle) confirmed present.
- A real ./tron run with --trace --diag confirmed the new flags actually
  produce output.

Filenames below use "__" in place of "/" - map back to the real path
using the arrow.

NEW FILES (from this chat, unaffected by the merge)
-----------------------------------------------------
core__mission_link_codec.h          -> core/mission_link_codec.h
core__mission_link.c                -> core/mission_link.c
platform_board__mission_link_task.c -> platform/board/mission_link_task.c
tools__mission_link_selftest.c      -> tools/mission_link_selftest.c
core__mission_link_handshake.h      -> core/mission_link_handshake.h
core__mission_link_handshake.c      -> core/mission_link_handshake.c
tools__handshake_selftest.c         -> tools/handshake_selftest.c
tools__handshake_selftest.py        -> tools/handshake_selftest.py
tools__override_selftest.c          -> tools/override_selftest.c
tools__telemetry_bridge.py          -> tools/telemetry_bridge.py

MERGED FILES (see above)
---------------------------
tools__app.py                       -> tools/app.py
tools_templates__index.html         -> tools/templates/index.html

EDITED (Makefile only, unrelated to this merge)
--------------------------------------------------
Makefile                            -> Makefile

ADOPTED AS-IS FROM files-2.zip (see above)
---------------------------------------------
platform_laptop__debug.c            -> platform/laptop/debug.c
platform_laptop__main.c             -> platform/laptop/main.c
core__scoring.c                     -> core/scoring.c
core__simulation.c                  -> core/simulation.c
core__search_zones.h                -> core/search_zones.h
core__search_zones.c                -> core/search_zones.c
core__incident.c                    -> core/incident.c

NOT included (already correct, never touched by me or files-2.zip):
core/mission_link.h, core/mission_link_override.h, core/mission_link_
override.c, core/mission_state.h, core/cbba.c,
core/WIRE_PROTOCOL_MAGIC_BYTES.md.


Second upload (files.zip) - genuine merge conflict
=====================================================
This one was the real thing: files.zip contained core/mission_state.h and
core/incident.c from a branch that ALSO started before the override work
landed on CBBAAgentState_t - exactly the collision the original override
prompt's own coordination note warned about ("if another change also adds
a field here around the same time, reconcile field order explicitly
rather than letting one silently clobber the other").

Their mission_state.h had two genuine, unrelated additions:
  - WORK_RATE_MIN/WORK_RATE_MAX (1-8, replacing a hardcoded 1-5) - a
    tuned constant with a detailed sensitivity-sweep justification
    comment (cuts total task volume 39%, safe-zone fallback failures 91%)
  - NUM_SYSTEM_TASKS bumped 10 -> 20

...but their version of the file had DROPPED the override_active /
override_incident_id fields and the _Static_assert entirely - not an
intentional removal, just an artifact of branching before those existed.
Taking their file as-is would have silently broken core/cbba.c's
cbba_apply_override_pin() and core/mission_link_override.c's apply
function (both reference those fields directly) - compile failure, not
a subtle bug, but still exactly the kind of clobber the coordination
note was trying to prevent.

Merge performed: took their file as the base (their tuning work is real
and correct), re-inserted the override_active/override_incident_id
fields and _Static_assert in the same place as before, added a "MERGE
NOTE" explaining why, and re-ran the sizeof() probe rather than assuming
the old 216-byte number still held (it does - neither of their changes
affects this struct's layout: WORK_RATE_MIN/MAX are plain #defines,
NUM_SYSTEM_TASKS only sizes an unrelated array).

incident.c's one change (hardcoded 1,5 -> WORK_RATE_MIN/WORK_RATE_MAX)
depends on mission_state.h's new constants and had no other conflicts -
adopted as-is.

Verified NUM_SYSTEM_TASKS=20 doesn't break anything: grepped every call
site across the codebase (debug.c, allocator.c, cbba.c, team.c,
incident.c, recovery_manager.c, mission_execution_twin.c,
test_metrics.c) - every one already uses the macro, not a hardcoded 10,
so the array simply got bigger with no code changes needed.

Re-ran the full verification pass after this merge, override_selftest.c
in particular (the test most directly dependent on the re-merged
fields): full build clean, board syntax checks clean, all 5 standalone
test suites pass, and a live smoke run confirms NUM_SYSTEM_TASKS=20 is
in effect (system task slot IDs now go past 10).

core__mission_state.h               -> core/mission_state.h  (merged)
core__incident.c                    -> core/incident.c        (updated,
                                        replaces the earlier core__incident.c)

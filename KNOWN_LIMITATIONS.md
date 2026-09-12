# Known Limitations — `tron_snapshot_dataset.csv`

For anyone training on the CSV export. Three known imperfections in the current data, plus one thing that's *not* in the CSV at all.

## 1. M37 — cross-district coverage misattribution (still unfixed)

`search_zone_pick_frontier_cell()` / the passive-coverage tracking in `core/incident.c`'s `search_sweep_step()` computes a team's district once from the team's own current cell, then credits its full sweep radius to that one district. Near a district boundary, some of the covered cells actually belong to a *different* district — coverage gets credited to the wrong `district_id`.

What was fixed this session: the coverage **denominator** (`search_zone_coverage_percent()`) now excludes cells that are genuinely unreachable (boxed-in pockets), so a district's coverage % is no longer artificially capped below 100% by cells nothing can ever reach. That's a real improvement, but it's a different bug from the cross-district credit issue above — the misattribution itself was not touched.

**Net effect:** coverage % values are more honest than before, but are still **not fully correct for districts near a boundary** — some of a district's reported coverage may actually reflect a neighboring district's cells. Don't treat per-district coverage % as ground truth for boundary-adjacent districts.

## 2. Evacuation gateway placement changed during this session

Evacuation gateway positions were fixed twice today: first a connectivity fix (the 3 gateways were landing on non-distinct/degenerate cells), then a separate reachability-model fix (gateway candidates could be chosen on cells that weren't actually road-connected). Before either fix, gateway placement was broken or degenerate in **up to ~95% of generated worlds** (47 of 50 sampled seeds).

**Any CSV data generated before today's fixes has unreliable gateway placement.** Evacuation-related rows/features (gateway location, evacuee routing, anything downstream of `g_evacuation_gateways[]`) from pre-fix runs should not be reused — only regenerate from a build that includes both fixes.

## 3. `EVENT_TICK` is ~97% of all rows, by design

`EVENT_TICK` was added this session to give unconditional per-team, per-tick visibility — previously the CSV only logged a row when one of 11 discrete event types fired (`EVENT_NEW_INCIDENT`, `EVENT_TEAM_ASSIGNED`, `EVENT_TEAM_ON_SCENE`, `EVENT_STAGE_COMPLETE`, `EVENT_TEAM_REST`, `EVENT_TEAM_AVAILABLE`, `EVENT_INCIDENT_RESOLVED`, `EVENT_FAULT_INJECTED`, `EVENT_FAULT_RECOVERED`, `EVENT_WATCHDOG_ANOMALY_CHANGED`, `EVENT_RECOVERY_ACTION_CHANGED`). `EVENT_TICK` fires every tick for every team regardless, so it now dominates the row count (~97% of rows in a measured 50-seed/3000-tick run).

It's meant for coverage/presence checks — e.g. "was this team logged every tick, with no gaps." **Filter `event_type == EVENT_TICK` (12) out before any analysis that looks at event sequences or transitions between meaningful events** — leaving it in will drown out the signal in anything sequence-based.

---

`fault_buffer`/`fault_comms` fields are not present in the CSV at all — that subsystem (`core/fault_buffer.c`, `core/fault_comms.c`) has no callers in any build (laptop sim, board firmware, or test harnesses), so there's no fault-broadcast-derived data to look for or expect.

---

# Known Limitations — `core/` (accepted findings from the AUDIT.md triage pass)

Different audience from the CSV section above: this is for anyone reading `core/` source, not consuming the dataset. These are LOW-severity items from `AUDIT.md` that were deliberately triaged as "leave as-is" — each is either protected by an invariant unlikely to break, purely theoretical given current constants, an explicit accepted tradeoff, or genuinely style-only. Not bugs to fix; documented here so nobody re-discovers and re-litigates them from scratch.

1. **`cbba_build_bundle`** (core/cbba.c) — `bundle_size` can advance past what `path_size` reflects when `path[]` fills up first; same underlying issue as `AUDIT.md` top-10 item #9's deliberately-unfixed "orphaned pin" behavior (needs a design decision on what happens when path caps out, not a mechanical fix). Tracked there, not duplicated here.
2. **`chacha20_rotl32`** (core/chacha20.c) — UB only on a rotation amount of 0 or ≥32; every call site uses fixed literals (16/12/8/7).
3. **`abs_i`** (core/environment.c) — UB only at `INT_MIN`; every caller passes small grid-coordinate deltas (max ~72).
4. **`district_infrastructure_density`** (core/environment.c) — computed into `DistrictRecord_t.critical_infrastructure_density`, which is dead data (never read elsewhere); cleanup opportunity, not a bug.
5. **`queue_hazard_event`** (core/environment.c) — silently drops a hazard notification when all `MAX_ENV_PENDING_HAZARDS` slots are full; an explicitly acknowledged bounded-queue tradeoff per its own comment.
6. **`next_road_id`** (core/environment.c) — 16-bit road-ID counter has no wraparound guard; world generation issues a few hundred IDs per run, nowhere near 65,535.
7. **`carve_repair_to_nearest_road`** (core/environment.c) — degenerate self-path fallback requires zero roads existing anywhere on the grid, which `stage_generate_roads()` always prevents by carving the arterial chain first.
8. **`apply_building_damage`** (core/environment.c) — footprint-window reconstruction is fragile against future changes to `place_building_rect()`, but already mitigated by a per-cell `building_id` mismatch check; no live bug today.
9. **`building_incident_score`** (core/environment.c) — no NULL check of its own, but both call sites only invoke it after `building_can_spawn_incident()` has already guaranteed non-NULL.
10. **`propagate_fire`** (core/environment.c) — its `building_id < MAX_ENV_BUILDINGS` guard is redundant (IDs are always in range by construction) but genuinely safe, not buggy.
11. **`lora_crypto_accept_counter`** (core/lora_crypto.c) — the actual floor-regression bug here was fixed via `AUDIT.md` top-10 item #10 (commit `7e733ab`); the residual "non-atomic, no real ISR context" note is the same class of finding as item #4 (documented future-risk, no live ISR anywhere in this codebase today).
12. **`sim_tick`** (core/sim_runner.c) — tick-counter wraparound requires ~4.29 billion ticks and self-heals within one tick regardless (`simulation_step()` re-syncs it).

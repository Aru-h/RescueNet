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

# TRON — Fix Plan

Three items, in priority order. Build with `make` after each, verify with
`./tron --sweep-priority1 1 50 3000 --fault-profile=combined` (0 flagged,
`VERDICT: PASS` expected) before moving to the next item.

## 1. Incident / safe-zone reachability validation (do this first)

**Problem:** `generate_safe_zones()` (core/safezone.c) and
`generate_incident_at()` (core/incident.c) can place a safe zone or incident
on a cell that's unreachable after `environment_apply_earthquake()` runs
(damage/road-blocking happens after placement in `generate_world()`,
core/simulation.c — this order is intentional, don't reorder it, an earlier
attempt to run the earthquake first caused a worse regression: buildings/
districts are only reachability-validated once, before the quake).

**Existing infrastructure to reuse** (core/environment.c,
`stage_validate_and_build_navigation`, already used for buildings/districts):
- `road_reachable_map(uint8_t reachable[GRID_WIDTH][GRID_HEIGHT])`
- `lot_reachable(const uint8_t reachable[...], int x, int y)`
- `carve_repair_to_nearest_road(int x, int y)` — actively fixes an unreachable
  cell rather than just rejecting it; prefer this over reject-and-retry.

**What to do:** after `environment_apply_earthquake()` runs (so damage is
already applied), add a validation pass for safe zones and incidents that
mirrors the existing building/district pass: build `reachable[][]` via
`road_reachable_map()`, and for each safe zone / incident location, if
`!lot_reachable(...)`, call `carve_repair_to_nearest_road(x, y)` then rebuild
the reachable map. Since safe zones/incidents are placed before the quake and
this check runs after, this is a straightforward reuse of the same pattern —
no reorder needed this time, which is what made it hard before.

**Verify:** run the sweep tool for a large seed/tick count and confirm the
`--sweep-priority1` no-path-hit count drops substantially vs. before this
change (it won't hit zero — genuinely new post-quake damage mid-run is still
possible and item 2 below is the safety net for that — but it should be much
lower for freshly-generated worlds).

## 2. Quarantine incidents that repeatedly fail to path (churn fix)

**Problem:** `move_team()`'s no-path branch (core/simulation.c) correctly
calls `abandon_task_for_recovery()` instead of freezing the team — but
`abandon_task_for_recovery()` (core/incident.c) always sets the incident back
to `INC_OPEN`, so a structurally-unreachable incident gets re-bid and
re-abandoned every cycle, forever. This is what caused the quadratic slowdown
observed when this fix was combined with the pre-existing placement bug.
Item 1 above reduces how often this triggers but doesn't eliminate it (new
mid-run damage can still cut a path), so this is still worth adding as a
safety net.

**core/mission_state.h** — add to `IncidentState_t`, near the other
`uint8_t` fault-tracking fields:
```c
uint8_t  unreachable_streak;   /* consecutive no-path abandons; quarantines a
                                   structurally unreachable incident instead of
                                   re-bidding it forever */
```

**core/incident.c**, `abandon_task_for_recovery()` — where it currently does:
```c
if(task->assigned_team == team->team_id)
{
    task->assigned_team = NO_ASSIGNMENT;
    task->status = INC_OPEN;
    ...
```
change to:
```c
if(task->assigned_team == team->team_id)
{
    task->assigned_team = NO_ASSIGNMENT;

    if(task->unreachable_streak < INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD)
        task->status = INC_OPEN;
    /* else: leave status alone — quarantined, stays out of INC_OPEN
       candidate pools without touching INC_RESOLVED (which scoring/
       metrics treat as a genuine success) */
    ...
```
Define `INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD` (try `3`) near the other
tunables in incident.h or fault_config.h.

**core/simulation.c**, `move_team()`:
- In the no-path `else` branch (right next to `g_probe_no_path_hits++`), add:
  `if(task->unreachable_streak < 255) task->unreachable_streak++;`
- In the arrival/success branch (where `team->loc_x == task->loc_x` triggers
  `STATUS_ON_SCENE`), add: `task->unreachable_streak = 0;`

**core/allocator.c** (~line 300, ~line 311) and **core/cbba.c** (~line 599,
~line 797) — these are the candidate-scanning loops (`status == INC_OPEN` /
`status != INC_OPEN`); add `task->unreachable_streak <
INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD &&` to each. First grep
`status.*INC_OPEN` across both files to catch any site missed here — some
occurrences (e.g. cbba.c ~line 549) are guards on an already-pinned slot, not
candidate scans, and don't need this clause.

## 3. M37 rework — district boundary bug in passive coverage loop

**Problem:** core/incident.c, `search_sweep_step()`, the non-search-team
passive coverage loop (~line 546 onward). `district_id` is computed once from
the team's own current cell, then reused for every cell in the sweep radius
circle — near a district boundary this credits coverage to the wrong
district (whichever district the team is standing in, not whichever district
each individual cell actually belongs to).

Current:
```c
for(int16_t dx = -(int16_t)radius; dx <= (int16_t)radius; dx++)
{
    for(int16_t dy = -(int16_t)radius; dy <= (int16_t)radius; dy++)
    {
        if(dx * dx + dy * dy > (int32_t)radius * radius)
            continue;
        search_zone_mark_cell_covered(district_id,(int16_t)(team->loc_x + dx),
                                       (int16_t)(team->loc_y + dy), current_tick);
    }
}
```
Replace with:
```c
for(int16_t dx = -(int16_t)radius; dx <= (int16_t)radius; dx++)
{
    for(int16_t dy = -(int16_t)radius; dy <= (int16_t)radius; dy++)
    {
        if(dx * dx + dy * dy > (int32_t)radius * radius)
            continue;

        int16_t cx = (int16_t)(team->loc_x + dx);
        int16_t cy = (int16_t)(team->loc_y + dy);

        /* Credit the CELL's own district, not the team's — a sweep
           radius crossing a district boundary must not attribute
           coverage to the wrong zone */
        search_zone_mark_cell_covered(zone_id_for_location(cx, cy), cx, cy, current_tick);
    }
}
```
Drop the now-unused `district_id` local *from this loop specifically* — check
whether the earlier search-team retarget block earlier in the same function
still needs its own (it does; don't touch that one). `zone_id_for_location()`
already clamps out-of-bounds coordinates, so this is a drop-in change.

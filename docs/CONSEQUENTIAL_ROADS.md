# Consequential roads (Step 3.1, causal profile only)

Before this step an environment fault in the causal profile only added a few
points of routing cost to one cell. Nothing slowed or stopped a team, so there
was almost nothing for `REROUTE` or `CLEAR_ROAD` to fix (see
`docs/RECOVERY_ACTIONS.md`, finding 1). Under `--fault-profile=causal` a
damaged road cell now costs teams real time.

* **Default: ON** under the causal profile (`--causal-roads=on`).
* `--causal-roads=off` restores the Step 3 causal profile exactly. This is
  checked: with it off, the causal dataset and the first 21 sidecar columns are
  byte-identical to the Step 3 build (actions off and on).
* Every other profile is untouched, and rejects `--causal-roads` / `--roads-*`.

## Mechanics

A damaged cell is one of two kinds, decided when the first fault lands on it by
a hash of `(seed, tick, team)` (never a shared RNG stream):

| Kind | Share | Effect on movement | What A* sees |
|---|---|---|---|
| **Partial debris** | 70% (assumed) | a team stalls 4 ticks before stepping onto the cell, once per crossing | the cell's `travel_cost` is raised by `delay x cost_per_tick` (4 x 3 = 12), capped at 255 |
| **Full blockage** | 30% (assumed) | the cell is impassable | `navigable = 0` |

Both live in `g_environment` itself (`travel_cost` and `navigable`), through
the same `environment_set_cell_operational_state()` that environment faults
already used, so the board's map frames and the dashboard's `STATICMAP` carry
them with no new wire field. `road_blocked` is set for both kinds, as before.
The existing road-degradation hazard keeps raising a flagged cell's cost over
time (clamped at 25), as it already did.

A* is **omniscient** about this: it sees the true cost. A discovery model
(a cell is known only once someone reaches it) is left to Step 3.5. The
sim's A* (`core/path.c`) is unchanged: same tie-breaking, same neighbour order.
With `--roads-astar=blind`, partial cells raise `travel_cost` by only the fault's
own severity (3) and a rerouting team gets the stall cost as a private overlay;
this is a what-if (see Results).

### Global connectivity guard

A full blockage is allowed only if every passable cell that could reach a
safe zone before can still reach one afterwards. The check is a breadth-first
search from the safe zones using the pathfinder's own move rules (8-way, no
corner cutting), run before the cell is blocked (`road_block_allowed()` in
`core/fault_injector.c`). The blocked cell is also refused if it holds a safe
zone, an evacuation gateway, or the location of an unresolved incident or
system task. A refused blockage becomes partial debris instead (counted as
`block_downgraded`).

**Audit.** Every causal tick the same search runs twice, once on the real map
and once with all road blocks lifted, and counts any cell reachable in the
second but not the first (cells that are themselves blocked excluded). The
count is printed as `audit_violations` in the `[DIAG ROADS]` line.
`make check-causal-roads-connectivity` runs 8 full causal runs, requires zero
violations, and requires that the guard was actually exercised (some blockage
created or refused), so a green result cannot mean "never blocked anything".

### Duration and clearing

* Unattended damage lasts a uniform 600-2400 ticks (6-24 h) from onset. The
  fault's own duration is that lifetime; the cell is restored when the last
  fault on it ends.
* `CLEAR_ROAD` shortens it. The task needs 120 work units (partial debris, about
  12 ticks for a team) or 240 (full blockage). A route destination must be a
  navigable cell, so for a fully blocked cell the task stands on a navigable
  neighbour and clears the blocked cell when the work completes.
* Damage is **per cell**: further faults landing on a damaged cell do not
  change its kind or cost.
* **The per-team environment-fault flag keeps its old meaning** (a team has an
  active environment fault for the fault's duration). A separate per-team
  "route touches a damaged cell" truth column is in the causal sidecar only
  (`route_touch`, see `docs/CAUSAL_FAULT_MODEL.md`).
* Because each team can hold only one environment fault at a time, long
  persistence means few damage events per run (about 11-18 damaged cells per
  3000-tick seed at stress 7).

## Grounding (1 tick = 36 s, so 1 day = 2400 ticks)

Primary pages for the Tohoku figures could not be fetched from this
environment (the network proxy blocks them); figures come from search-result
summaries. **Everything in the "source" column is to verify before it is cited.**

| Value used | Informed by | Status |
|---|---|---|
| Unattended persistence 600-2400 ticks (6-24 h) | Tohoku 2011 "Operation Teeth of a Comb" (Japan MLIT archive): clearing began 12 March, about 11 of 16 inland-to-coast routes secured within one to two days, 15 by 15 March, 97% of National Routes 45 and 6 passable by 18 March | to verify (summary of the MLIT archive, primary page not fetched; whether "cleared" meant a single lane is also unknown) |
| same | Kumamoto 2016: emergency restoration of all expressway damage within about 24 h except one slope failure; Kyushu Expressway closed about two weeks | to verify (JSCE / NEXCO summary via search) |
| same (long tail) | Haiti 2010: about 2% of 33 million cubic yards of debris cleared after about 8 months; Wenchuan 2008: all 2,384 damaged roads reopened after about 8 months | to verify (press and agency summaries) |
| Partial vs blocked split | debris-width studies of collapsed buildings: about 3 m of wreckage per collapsed house; a road under 4 m blocked by one collapse, 4-6 m blocked if both sides collapse, over 8 m rarely blocked (Applied Sciences 2020, 10(4), 1289) | to verify (summary via search); the **70/30 figure itself is an assumption** |

### Assumptions (no source)

* 70% partial / 30% blocked.
* 4-tick stall: a 100 m cell crossed at about 0.5 m/s through debris is about
  5.6 ticks against 1 tick normally, so about 4 extra. The 0.5 m/s is assumed.
* Clearing work 120 / 240 units (about 12 / 24 ticks per cell for one team).
* Persistence range 600-2400 ticks (the sources above give days-to-months for
  whole routes and hours for priority routes; the range is a judgement).
* `cost_per_tick` = 3 was **tuned on the development seeds 1001-1020 only**
  from {1, 2, 3, 5} (incidents resolved, roads on, actions off: 39.55, 39.25,
  40.10, 38.65). The differences among 1, 2 and 3 are small; the choice is weak.
* Blockages are refused at safe zones, gateways and incident/task cells.

## Configuration (`g_causal_roads`, `core/fault_config.c`)

| Field | Default | Flag |
|---|---|---|
| `enabled` | 1 | `--causal-roads=on\|off` |
| `partial_pct` | 70 | `--roads-partial-fraction=<0-100>` |
| `crossing_delay_ticks` | 4 | `--roads-delay=<1-20>` |
| `persist_min_ticks`, `persist_max_ticks` | 600, 2400 | `--roads-persist=<min>-<max>` |
| `clear_work_partial`, `clear_work_full` | 120, 240 | `--roads-clear-work=<partial>-<full>` |
| `cost_per_tick` | 3 | `--roads-cost-per-tick=<1-50>` |
| `route_cost_visible` | 1 | `--roads-astar=true\|blind` |

The Step 3 sensitivity flags `--causal-env-duration` and
`--causal-env-crossing-delay` still work, but only with `--causal-roads=off`
(with roads on they are rejected).

## Dashboard (`laptop/tools/static/dashboard/route.js` on `origin/feature/dashboard-2.5d`)

I read `route.js`, `map-render.js` and `replay.js` on that branch and the
`STATICMAP` emitter in its `laptop/debug.c`. I did not run the dashboard or its
end-to-end test. This sim branch contains none of that code.

**(a) Is the damage visible, and re-sent?** Yes, for the default
configuration. `STATICMAP` packs, per cell, a flags byte (navigable bit 0,
flood, road class, `road_blocked` bit 5), the `travel_cost` byte and the
district id. Partial debris raises the cell's `travel_cost`; a full blockage
clears the navigable bit. `debug_emit_static_map_if_changed()` compares every
cell with the last emission on each tick in `--live` mode and re-prints the
whole line when any flag, cost or district differs, so damage appearing and
clearing are both re-sent. `route.js` already clamps cost below 1 to 1 and
reads `navigable` and `travelCost` only, matching `core/path.c`.

**(b) What the dashboard would still need** (no dashboard edits made):

1. **Replay history.** `replay.js` keeps only the last `STATICMAP` of a
   recording ("only the LAST STATICMAP line matters for replay"). With damage
   appearing and clearing mid-run, every replayed frame would be routed on the
   final map. It needs to keep each `STATICMAP` with the tick it arrived and
   give `RoutePlanner.plan` the map in effect at the replayed frame.
2. **Stall ticks.** On a partial cell a team does not move for several ticks.
   Route geometry is unaffected, but any "next step matches" accounting would
   count a stalled tick as a mismatch. The dashboard needs either a per-team
   stalled flag in `LIVEFRAME` or to ignore ticks where the team did not move
   next to a high-cost cell.
3. **Per-team REROUTE overlay**, only with `--roads-astar=blind` and the reroute
   action on: the sim routes those teams on private extra costs that are in no
   frame. With the default (true cost visible) the overlay is 0 and nothing is
   needed.
4. **Relay repositioning** (`--causal-actions` including `relay`): a repositioning
   team heads to a relay cell instead of its destination. The dashboard needs a
   per-team relay target in `LIVEFRAME`; today only the sidecar has an
   `action_mask`. Actions are off by default, so this matters only when enabled.

## Integration items

* **State hash.** `core/state_hash.h` (on `origin/feature/rtos-multitask`) hashes
  teams, incidents, fault state and recovery state. The causal stall counter
  (`s_stall[]` in `core/causal_actions.c`) and the road-damage kind per cell
  (`s_road_kind[][]` in `core/fault_injector.c`) are **not** in that hash. They
  are deterministic functions of the inputs, so two runs agree, but the hash
  must cover them once the causal profile and the RTOS branch are merged.
* **Board and wire.** No wire field, enum, CSV schema or message changed. The new
  code is laptop-only (behind `ENVIRONMENT_HAS_WORLD_STORAGE`); the ARM build
  checks could not be run in this environment (no `arm-none-eabi-gcc`).

## Results

Causal profile, `--fault-stress=7`, 3000 ticks, AI predictor off unless stated.
Development seeds 1001-1020 (where `cost_per_tick` was tuned), held-out seeds
2001-2020, and the pooled 40. Every difference is paired by seed with a 95%
bootstrap CI (`laptop/tools/paired_compare.py`). A CI that includes 0 is not
evidence of an effect. **Victims lost is shown but no conclusion is drawn from
it on any single seed set** (all its intervals except one blind-A* row include 0).

Recorded mean incidents resolved per seed (actions off): roads off 44.45 (dev) /
43.75 (held-out), roads on 40.10 / 40.40. "Roads off" is the Step 3 causal
profile (byte-identical, checked).

### Cost of the roads to the baseline (actions off, roads on minus roads off)

| Seeds | Incidents resolved | Victims lost |
|---|---|---|
| Development | -4.35 [-6.80, -2.05] | +0.30 [-2.55, +3.20] |
| Held-out | -3.35 [-5.90, -1.20] | +1.75 [-0.65, +4.00] |
| Pooled (40) | -3.85 [-5.62, -2.25] | +1.02 [-0.80, +2.88] |

Roads cost about 3-4 incidents per seed on both sets. The roads now matter.

### Table 1: actions vs off (roads on, true-cost A*)

Each row is that action set minus actions off.

| Row | Dev incidents | Held-out incidents | Pooled incidents | Pooled victims lost |
|---|---|---|---|---|
| all four actions | -0.90 [-4.05, +2.10] | -1.15 [-3.55, +1.35] | -1.02 [-3.00, +0.93] | -0.12 [-2.38, +2.10] |
| **REROUTE only** | +0.00 [0, 0] | +0.00 [0, 0] | +0.00 [0, 0] | +0.00 [0, 0] |
| CLEAR_ROAD only | -0.90 [-4.15, +2.20] | -2.00 [-6.25, +1.40] | -1.45 [-4.08, +0.88] | +0.60 [-1.65, +2.83] |

Victims lost, all four actions: dev -0.70 [-4.35, +2.60], held-out +0.45
[-2.30, +3.25]. CLEAR_ROAD only: dev -0.55 [-3.95, +2.50], held-out +1.75
[-1.10, +4.85].

**REROUTE-only is exactly zero by construction.** With A* seeing the true cost
the REROUTE overlay is 0, so the action cannot change a route. It still starts
and ends about 250 times a seed, but it changes nothing. This is not a hidden
benefit.

**Blind-A\* what-if** (`--roads-astar=blind`: A* sees only the fault's own
severity, REROUTE carries the stall cost), reroute minus blind off:

| Seeds | Incidents resolved | Victims lost |
|---|---|---|
| Development | -1.95 [-4.70, +0.50] | +3.80 [+0.80, +6.60] |
| Held-out | -4.65 [-7.35, -2.15] | +6.10 [+3.20, +9.20] |
| Pooled | -3.30 [-5.25, -1.48] | +4.95 [+2.85, +7.05] |

Blind A* without actions is not distinguishable from true-cost A* (dev -1.35
[-3.90, +1.15], held-out +0.10 [-1.20, +1.50]). With the actions on, REROUTE
made things worse on both sets. I did not isolate the cause. A plausible one is
that detours cost more travel time than the one-off stall they avoid, but that
is untested.

### Table C13: oracle detectors, actions on and off (AI predictor on, roads on)

Differences are against the real AI with actions on (the first row).

| Condition | Dev incidents | vs AI/on | Held-out incidents | vs AI/on |
|---|---|---|---|---|
| AI, actions on | 38.85 | baseline | 37.45 | baseline |
| AI, actions off | 38.70 | -0.15 [-2.75, +2.45] | 39.50 | +2.05 [+0.10, +3.85] |
| oracle env, on | 36.00 | -2.85 [-5.45, -0.75] | 38.00 | +0.55 [-0.95, +2.05] |
| oracle env, off | 13.10 | -25.75 [-28.20, -23.60] | 13.50 | -23.95 [-26.50, -21.10] |
| oracle mission, on | 38.85 | +0.00 | 37.45 | +0.00 |
| oracle mission, off | 38.55 | -0.30 [-2.70, +2.10] | 39.10 | +1.65 [-0.55, +3.70] |
| oracle env+mission, on | 36.00 | -2.85 [-5.45, -0.75] | 38.00 | +0.55 [-0.95, +2.05] |
| oracle env+mission, off | 13.45 | -25.40 [-27.80, -23.30] | 13.55 | -23.90 [-26.20, -21.50] |

Victims lost (dev / held-out): AI on 13.80 / 14.70, AI off 14.95 / 14.35,
oracle env on 16.60 / 13.50, oracle env off 27.80 / 27.65.

* The real AI with actions on versus off has opposite signs on the two sets:
  dev +0.15, held-out -2.05 (excludes 0). There is no evidence the actions help
  and some that they cost incidents on held-out seeds.
* A perfect environment detector with **actions off** loses about 24-26
  incidents on both sets. The existing recovery then reacts to every flagged
  environment fault, which now last about 900 ticks. With actions on the loss
  falls to -2.85 (dev) and +0.55 (held-out). I did not isolate the mechanism.
* The mission oracle with actions on equals the AI baseline exactly: it never
  triggers an action here, because REINFORCE_INCIDENT starves (below).

### Per-action counts (mean per seed, all four actions on)

| Action | Dev started / resolved / abandoned | Held-out started / resolved / abandoned |
|---|---|---|
| reroute | 251.8 / 104.7 / 146.7 | 254.8 / 133.8 / 120.3 |
| clear_road | 13.2 / 7.9 / 3.3 | 10.9 / 5.9 / 3.2 |
| reinforce_incident | 0.0 / 0.0 / 4.2 | 0.0 / 0.0 / 5.5 |
| relay_reposition | 11.2 / - / - | 17.8 / 17.5 / 0.2 |

**CLEAR_ROAD starvation (reported, not fixed).** About 11-13 clearing tasks start
per seed against about 14-18 damaged cells, and about a third are abandoned.
**REINFORCE_INCIDENT starts no reinforcements at all**: its requests (4-6 per
seed) all lapse because a support team must be AVAILABLE, and teams are AVAILABLE
only about 3% of ticks. Step 3.5's planner needs to be able to pull a team off
lower-priority work.

### Should `--causal-actions` now default ON?

**No. It stays OFF.** The evidence: no all-actions or single-action interval
excludes 0 in the helpful direction on either set or pooled; REROUTE is a no-op
under true-cost A* and harmful under blind A*; CLEAR_ROAD is neutral to negative;
the real-AI on-vs-off comparison flips sign between the seed sets. Turning actions
on would change the headline numbers for no demonstrated gain. Re-examine after
Step 3.5.

### Checks run

| Check | Result |
|---|---|
| C1 md5 `./tron 1 3 1500 --fault-profile=combined` | `b248bea72ce2ceb81389762dccc0a749`, identical to the baseline |
| none, mobility, communication, resource, random, combined datasets | byte-identical to the baseline binary |
| `--diag` stderr (combined, none) | identical |
| `--causal-roads=off` vs Step 3 build, causal dataset, actions off and on | byte-identical |
| `check-evaluate-determinism`, `check-live-override-noop-regression`, `check-consensus-shadow-regression` | pass |
| `check-causal-profile` | pass |
| `check-causal-roads-connectivity` | pass: 0 cells cut off over 8 full runs, 27 blockages created or refused |
| `check-causal-fault-exposure` | pass (audit 0 violations) |
| `check-selftests` | pass (fourteen selftests) |
| Board (`check-board*`) | **not run**: no `arm-none-eabi-gcc` here |


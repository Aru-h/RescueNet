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

> Step 3.2 note: the tables below were measured with the Step 3.1 plumbing. The three
> fixes in `docs/CAUSAL_PLUMBING_FIXES.md` leave every AI-off row unchanged and remove
> the oracle-detector collapse and about two thirds of the blind-A* REROUTE loss;
> the investigations at the end of this file describe what they fix.

> **Note on stress 7 (added in Step 3.6).** The evaluation stress of 7 used since Step 3 was **never
> derived from data**. At stress 7 the measured mobility and resource MTBFs per team are about 1.7 h
> and 2.1 h; the field-robot literature quoted in `docs/CAUSAL_FAULT_MODEL.md` (MTBF 6-20 h) matches
> **stress 1-2** (stress 2: 4.4 h and 6.6 h). From Step 3.6 on, stress 2 is the realistic headline and
> stress 7 is reported as a harsh-conditions stress test. See `docs/PERSISTENT_FAULTS_PROPOSAL.md`.

> **Provenance caveat (added in Step 3.5).** The held-out range 2001-2020 used here, and in Steps
> 3 and 3.1 (`docs/CONSEQUENTIAL_ROADS.md`), overlaps the seeds 2001-2008 the AI's thresholds were
> tuned on (`ml/MODEL_REPORT.md` section 3). The held-out and pooled columns of every **AI-on**
> row are therefore not fully out-of-sample; **AI-off rows and the development seeds are not
> affected**. Step 3.5 moved held-out to the unused range 3001-3020; see
> `docs/RECOVERY_PLANNER_RESULTS.md`. These tables were not re-run on the new range.


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
made things worse on both sets. The cause is traced below ("Why blind-A* REROUTE
loses"): it does not change travel, it makes the legacy recovery fire more.

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
  incidents on both sets. Cause traced below ("Why the oracle environment
  detector collapses"): the legacy team-level reaction holds flagged teams at the
  safe zone for the whole, hours-long, damage lifetime.
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

## Why the oracle environment detector collapses (investigation, measurement only)

Hypothesis (from the Step 3.1 review): damage now lasts 6-24 h, the per-team
environment flag stays on for hours, and the legacy team-level reaction keeps those
teams idle. **Confirmed**, with the refinements below. Nothing in `core/` was
changed: counters were added to scratch copies of the sources
(`laptop/tools/investigations/trace_team_status.patch`, reports
`trace_status_report.py` and `trace_outofservice_report.py`), which log, per team
per tick, the flag, the status before and after dispatch, the recommended action
and its reason.

**Traced seeds** (causal, stress 7, roads on, actions off, AI-on build; "baseline"
is the shipped thresholds with the environment class off, "oracle" is the perfect
environment detector):

| | 1001 baseline | 1001 oracle | 1002 baseline | 1002 oracle |
|---|---|---|---|---|
| incidents resolved | 36 | 1 | 44 | 15 |
| environment flag on, % of team-ticks | 74.4 | 83.4 | 65.0 | 73.0 |
| flag episodes: count, mean length (ticks) | 18, 1240 | 19, 1317 | 19, 1026 | 17, 1289 |
| recommendation while flag on: RETURN_TO_SAFE_ZONE | 8.4% | **99.4%** | 7.5% | **99.4%** |
| team status while flag on: RESTING | 51.6% | **99.1%** | 48.7% | **98.8%** |
| all team-ticks: working (en route or on scene) | 34.3% | 8.0% | 35.4% | 10.6% |
| all team-ticks: resting | 50.5% | 86.5% | 49.0% | 82.6% |
| resting team-ticks with stamina and equipment >= 90 | 6.7% | **92.6%** | 7.3% | **86.7%** |
| healthy team-ticks held at a safe zone by a RETURN_TO_SAFE_ZONE recommendation, % of all team-ticks | 0.0 | **78.9** | 0.1 | **67.8** |
| RETURN_TO_SAFE_ZONE stretches: count, mean length (ticks) | 334, 8 | 110, 234 | 356, 6 | 185, 124 |
| status changes caused by a dispatch | 105 | 21 | 81 | 65 |

What this shows:

* **Which response fires:** almost only RETURN_TO_SAFE_ZONE (reason
  PREDICTED_FAULT), 99.4% of the flagged team-ticks. LOCAL_RECOVERY is 0.5% and
  reallocation 0.1% or less; abort never fires. The oracle's reaction is "go and
  rest", not "reallocate".
* **How long:** the flag lasts as long as the damage (mean 1000-1300 ticks, max
  about 2250), so each team is flagged for 65-83% of the run, in a few long
  episodes.
* **What it costs:** the teams are not interrupted (dispatch-caused status
  changes fall from 81-105 to 21-65); they are *held*. `recovery_dispatch()` leaves
  a RESTING team alone, so a team sits at the safe zone until the flag ends, even
  though 87-93% of those resting ticks are already fully rested (stamina and
  equipment >= 90). 68-79% of all team-ticks are healthy teams held idle.
* **The flag is a poor proxy for consequence:** in the baseline seed-1001 run the
  flag is on for 22,313 team-ticks, but the team's route touches a damaged cell on
  only 4.1% of them (sidecar `route_touch`). The flag keeps its Step 3 meaning (a
  team holds an environment fault for the fault's duration), as instructed.

**Dose-response** (20 seeds 1001-1020, AI on, actions off; only
`--roads-persist` varied, an existing flag; oracle environment minus shipped AI,
paired 95% CI):

| Unattended damage lasts | Incidents resolved | Victims lost |
|---|---|---|
| 20-60 ticks | -0.65 [-1.95, +0.65] | +0.25 [-1.60, +2.10] |
| 200-600 ticks | -11.90 [-14.35, -9.70] | +3.25 [+0.60, +6.00] |
| 600-2400 ticks (default) | -25.60 [-28.55, -22.50] | +12.85 [+9.65, +15.85] |

The collapse vanishes when damage is short and grows with duration, so the
duration of the flag, not the detector, is the driver.

Implication (not implemented): an environment detector must not drive a team-level
reaction. The causal `--causal-actions` path already routes it to map-level actions
instead (oracle environment with actions on: -2.85 on the development seeds and
+0.55 on the held-out seeds, against about -25 with them off). A team-level reaction
to the environment class would need a flag that reflects route exposure, not the
fault's lifetime.

## Why blind-A* REROUTE loses (investigation, measurement only)

Setup: `--roads-astar=blind`, AI off, seeds 1001-1020, REROUTE on vs off, counters
in a scratch copy (`reroute_probe.patch`, reports `reroute_probe_report.py` and
`reroute_recovery_report.py`). The instrumented build reproduces the recorded
per-seed incidents and victims exactly on all 20 seeds (reroute off 38.75, on 36.80
incidents; victims lost 13.95 and 17.75).

* **Detour length vs stalls avoided: both are tiny.** About 310 reroute episodes a
  seed. Measured at each episode's start (route without vs with the overlay), the
  overlay leaves the route unchanged in 85% of them. The average route is 10.1
  cells with 0.79 partial-debris cells, and 10.4 cells with 0.72 after rerouting:
  a detour of +0.26 cells against 0.08 cells of debris avoided (0.30 stall ticks at
  4 ticks per cell). Net about +0.04 ticks per episode, i.e. zero.
* **Realised movement agrees.** Step attempts 11,553 to 11,588 per seed (+35,
  CI [-290, +401]); stall ticks 842 to 912 (+70, CI [-352, +680]). Neither changes
  significantly.
* **Where the loss comes from: more legacy recovery.** Team-ticks per seed with a
  recovery recommendation, reroute off to on (paired diff, 95% CI):

| Legacy recommendation | Off | On | Diff |
|---|---|---|---|
| LOCAL_RECOVERY | 973.8 | 1450.5 | +476.8 [+184.0, +881.6] |
| RETURN_TO_SAFE_ZONE | 191.5 | 255.9 | +64.4 [+22.1, +120.3] |
| of which reason PROGRESS_DEVIATION (all actions) | 249.3 | 753.9 | +504.5 [+279.8, +785.1] |
| REQUEST_TASK_REALLOCATION, ABORT_MISSION | 0 | 0 | 0 |

The progress-deviation signal triples. The likely route: the overlay is added to
the cell cost in the pathfinder (`core/path.c:148`) and the team's
`cached_remaining_path_cost` comes from that same search (`core/simulation.c`,
the `causal_actions_path_begin()` calls), and `core/mission_execution_twin.c` reads
that cached value for its progress expectation. A reroute therefore inflates the
remaining-cost estimate by 12 per damaged cell and the team looks behind schedule.
The code path is verified; the link is not isolated by a counterfactual (that would
need a behaviour change, which this round excludes).

So blind REROUTE neither shortens nor lengthens trips; it costs incidents by making
the legacy recovery fire more. Under true-cost A* the overlay is 0, which is why
REROUTE-only is exactly zero there.

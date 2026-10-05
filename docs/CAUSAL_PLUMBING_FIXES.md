# Causal plumbing fixes (Step 3.2, causal profile only)

The Step 3.1 investigations (`docs/CONSEQUENTIAL_ROADS.md`, "Why the oracle
environment detector collapses" and "Why blind-A* REROUTE loses") found three
pieces of plumbing that made environment faults harmful for the wrong reason.
All three apply only under `--fault-profile=causal`; every other profile is
byte-identical (C1 md5 `b248bea72ce2ceb81389762dccc0a749`, all-profile dataset
diffs). `--causal-fixes=off` restores the Step 3.1 behaviour exactly (checked:
dataset and the first 23 sidecar columns byte-identical to the Step 3.1 build,
actions off and on).

`--causal-fixes=on|off|<list of label,rtb,cost,stall>`, default **on** (the `stall` fix is section 4, added
in Step 3.5; the results below were taken before it). The list form is
for ablations. Each fix is one bit (`CAUSAL_FIX_*`, `core/fault_config.h`).

## 1. The environment label is route exposure (`label`)

Before: the dataset's `fault_active_environment` (and severity, ticks-remaining
and the active-mask bit) was on while the team *held* an environment fault, which
lasts as long as the road damage it caused (hours). In the baseline it was on
for 65-78% of team-ticks, yet the team's route touched a damaged cell on only 4%
of those.

Now (needs consequential roads on): the label is on while **this team's current
route touches a damaged cell**, the `route_touch` signal of Step 3.1:

* bit 0: the next 8 steps of the route cross a partial-debris cell;
* bit 1: they pass within one cell of a fully blocked cell;
* a team with no destination (resting, available, on scene) has no route, so the
  label is 0.

It is computed once per tick, before allocation and movement, from the team's
position at the start of the tick (`causal_step`, `core/fault_injector.c`), so a
dataset row's label can lag the row's own position by one step. The row's
severity and ticks-remaining columns are the team's own fault's values while the
label is on (0 when it holds none); they are not recalibrated.

Not changed: the damage itself, its lifetime, the per-team fault record and every
injector rule that uses it (one environment fault per team, cleanup, CLEAR_ROAD).
Only what the dataset and the detectors see as "the environment label" changed
(`fault_injector_causal_env_label()`).

Sidecar: `route_touch` stays; a new last column `env_hold` carries the old
per-team flag (read at row time, like the dataset's), so old and new can be
compared (`docs/CAUSAL_FAULT_MODEL.md`). `make check-causal-fixes` asserts, for
every tick row, that the dataset label equals `route_touch != 0` (and equals
`env_hold` with `--causal-fixes=off`).

## 2. An environment detection never drives a team-level reaction (`rtb`)

Going home does not fix a road. Under the causal profile a flagged environment
class is never turned into RETURN_TO_SAFE_ZONE, LOCAL_RECOVERY, task
reallocation or abort, whatever the detector (`evaluate_fault_predictor()`,
`core/recovery_manager.c`). It maps to routing responses only:

| Environment detection (label on) | Response |
|---|---|
| REROUTE enabled | `REROUTE`: the team gets a private path-cost overlay (0 under true-cost A*, the stall cost under `--roads-astar=blind`) |
| CLEAR_ROAD enabled | a `CLEAR_ROAD` request for the damaged cell the route touches (`causal_actions_detector_flag()`) |
| neither enabled (`--causal-actions=off`, the default) | **wait**: no team-level action (CONTINUE_MONITORING); the team crosses or stalls at the debris under the movement rules |
| always | never RETURN_TO_SAFE_ZONE, LOCAL_RECOVERY, REQUEST_TASK_REALLOCATION or ABORT_MISSION |

The mission class is unchanged. `make check-causal-env-no-rtb` asserts the
consequence end to end: a perfect environment detector changes nothing with
actions off (per-team throughput identical to the shipped-thresholds run on three
seeds), while the same oracle with `--causal-fixes=off` does change the run
(control, so the check cannot pass on a detector that never fired). Note this
makes the oracle-environment, actions-off cell of C13 equal to the baseline by
construction; `--causal-fixes=label` is the ablation that keeps the old reaction.

## 3. The progress tracker ignores the REROUTE overlay (`cost`)

The overlay is added to cell costs in the pathfinder (`core/path.c`) and the
team's `cached_remaining_path_cost`, which `core/mission_execution_twin.c`
compares with its plan, came from that same search, so rerouting inflated the
remaining cost by the overlay (12 per damaged cell in blind mode) and the team
looked behind schedule. Now, when a team's search carried the overlay, the
cached remaining cost is recomputed from the team's new position at the true
terrain cost (`simulation_true_remaining_cost()`, `core/simulation.c`). The
overlay still steers the step. With no overlay (true-cost A*, or REROUTE off)
nothing changes.

## 4. Stall-aware evaluators (`stall`, added in Step 3.5)

Found while building the recovery planner: a team crossing a partial-debris cell is held for
four ticks (the crossing stall). The movement-stall evaluators read that as a mobility fault:
the watchdog's stall window (`core/watchdog.c`, `update_movement_stalls`), the peers' consensus
observer (`core/consensus.c`, `mobility_observe`), the AI's `moved_this_tick` feature
(`core/fault_predictor_features.c`) and the mission twin's expected-progress tracker
(`core/mission_execution_twin.c`). With the fix, a tick on which the team was held at a known
debris cell (`causal_actions_debris_held_at()`, a 16-tick ring per team in
`core/causal_actions.c`) is not sampled as a stall. A team stalled for any other reason is
unaffected. Probe before the fix (20 dev seeds): 4.4% of working team-ticks were near a stall,
but 30.4% of go-home events were (6.8x; 1028 of 3387).

`--causal-fixes` takes the token `stall`; `on` and the default now mean `label,rtb,cost,stall`
(`CAUSAL_FIXES_ALL = 0x0F`); it needs roads on and is a no-op without them. Causal profile only.
Checked: `--causal-fixes=label,rtb,cost` reproduces the Step 3.2 build byte for byte (dataset and
sidecar, causal profile, actions off and on, seeds 1001 and 3001); C1 md5, the other profiles'
datasets and their `--diag` stderr are unchanged.

Effect on go-home events and incidents (per seed; "near a stall" = within 12 ticks after a held
step; full table with the planner in `docs/RECOVERY_PLANNER_RESULTS.md`):

| Condition | Go-home events before | after | Near a stall before | after | Incidents before | after |
|---|---|---|---|---|---|---|
| Fixed gates, AI off, dev / held-out | 169.3 / 140.5 | 124.5 / 114.2 | 55.6 / 28.3 | 3.9 / 1.9 | 40.10 / 41.90 | 43.60 / 43.05 |
| Fixed gates, AI on, dev / held-out | 210.8 / 190.7 | 148.6 / 142.9 | 80.9 / 68.5 | 3.1 / 3.3 | 38.70 / 37.05 | 41.85 / 40.70 |
| Planner (no AI), dev / held-out | 170.8 / 160.0 | 124.6 / 116.5 | 56.5 / 54.0 | 3.1 / 2.7 | 41.25 / 41.35 | 43.45 / 42.35 |

Held-out here is seeds 3001-3020 (see the provenance note in
`docs/RECOVERY_PLANNER_RESULTS.md`); the Step 3.2 tables below used 2001-2020.

## Results

Causal profile, `--fault-stress=7`, roads on, 3000 ticks. Development seeds
1001-1020, held-out seeds 2001-2020, pooled 40; paired 95% bootstrap CIs
(`laptop/tools/paired_compare.py`). Scripts and run directories as in
`docs/CONSEQUENTIAL_ROADS.md`.

> **Provenance caveat (added in Step 3.5).** The held-out range 2001-2020 used here, and in Steps
> 3 and 3.1 (`docs/CONSEQUENTIAL_ROADS.md`), overlaps the seeds 2001-2008 the AI's thresholds were
> tuned on (`ml/MODEL_REPORT.md` section 3). The held-out and pooled columns of every **AI-on**
> row are therefore not fully out-of-sample; **AI-off rows and the development seeds are not
> affected**. Step 3.5 moved held-out to the unused range 3001-3020; see
> `docs/RECOVERY_PLANNER_RESULTS.md`. These tables were not re-run on the new range.

### Label exposure and episode length (AI off, actions off)

| | Development | Held-out |
|---|---|---|
| **New label** (route touches damage): exposure, % of team-ticks | **7.3** | **6.5** |
| episodes per seed | 228.8 | 190.6 |
| mean episode length, ticks (longest) | **9.6** (33) | **10.3** (33) |
| Old per-team flag: exposure | 72.7% | 72.3% |
| episodes per seed | 18.2 | 17.4 |
| mean episode length, ticks | 1202 | 1247 |
| any-fault exposure (dataset), old / new | 75.0% / 14.9% | 74.8% / 14.8% |

With all four actions on the label is on 9.1% (dev) and 7.5% (held-out) of
team-ticks. The environment class went from a few hours-long episodes per team to
many short ones, which is what route exposure should look like.

### Table 1: actions on / off (AI off, true-cost A*)

With the AI predictor off no detector fires, so none of the three fixes changes a
run: **every row below equals its Step 3.1 value exactly** (paired difference
+0.00 with a zero-width interval, on both seed sets). Fixed baseline minus roads-off
(the cost of the roads) is therefore unchanged: -3.85 [-5.62, -2.25] incidents
pooled.

| Row (minus actions off) | Dev incidents | Held-out incidents | Pooled incidents | Pooled victims lost |
|---|---|---|---|---|
| all four actions | -0.90 [-4.05, +2.10] | -1.15 [-3.55, +1.35] | -1.02 [-3.00, +0.93] | -0.12 [-2.38, +2.10] |
| **REROUTE only** | +0.00 [0, 0] | +0.00 [0, 0] | +0.00 [0, 0] | +0.00 [0, 0] |
| CLEAR_ROAD only | -0.90 [-4.15, +2.20] | -2.00 [-6.25, +1.40] | -1.45 [-4.08, +0.88] | +0.60 [-1.65, +2.83] |

**Blind-A\*** (`--roads-astar=blind`), REROUTE minus blind off. This is the row fix 3
targets:

| | Incidents resolved | Victims lost |
|---|---|---|
| Development, Step 3.1 | -1.95 [-4.70, +0.50] | +3.80 [+0.80, +6.60] |
| Development, fixed | -1.40 [-4.65, +1.60] | +2.85 [+0.00, +5.90] |
| Held-out, Step 3.1 | -4.65 [-7.35, -2.15] | +6.10 [+3.20, +9.20] |
| Held-out, fixed | -0.75 [-2.50, +1.00] | +3.15 [+0.85, +5.70] |
| Pooled, Step 3.1 | -3.30 [-5.25, -1.48] | +4.95 [+2.85, +7.05] |
| **Pooled, fixed** | **-1.07 [-2.92, +0.65]** | **+3.00 [+1.15, +5.00]** |

The incident loss shrinks by about two thirds and its interval now includes 0.
**A victims-lost penalty remains** (+3.00 pooled, interval excludes 0). I did not
explain it. Probe (20 dev seeds, `reroute_probe.patch`, blind A*, reroute off to
on): the added PROGRESS_DEVIATION recommendation team-ticks fall from +505
[+280, +785] (249 to 754) to **+177 [+6, +486]** (249 to 426), so fix 3 removes
about two thirds of that signal but not all of it. The remainder is not
bookkeeping: with true-cost A* the overlay is 0 and REROUTE-only is exactly zero,
so what is left comes from the route actually changing. One suspect is ruled out:
stall ticks wasted on a crossing that is then abandoned do not rise with REROUTE
(72 to 43 per seed). Completed debris crossings and stall ticks do rise (193 to
298 crossings, 842 to 1233 stall ticks per seed), but with intervals that include 0
([-51, +339] and [-282, +1353]).

### Table C13: oracle detectors (AI on, roads on, actions on and off)

Differences are against the real AI with actions on, as in
`docs/CONSEQUENTIAL_ROADS.md`.

| Condition | Dev incidents | vs AI/on | Held-out incidents | vs AI/on |
|---|---|---|---|---|
| AI, actions on | 38.85 | baseline | 37.45 | baseline |
| AI, actions off | 38.70 | -0.15 [-2.75, +2.45] | 39.50 | +2.05 [+0.10, +3.85] |
| oracle env, on | 38.30 | -0.55 [-1.45, +0.20] | 37.10 | -0.35 [-1.90, +0.90] |
| **oracle env, off** | **38.70** | **-0.15 [-2.75, +2.45]** | **39.50** | **+2.05 [+0.10, +3.85]** |
| oracle mission, on | 38.85 | +0.00 | 37.45 | +0.00 |
| oracle mission, off | 38.55 | -0.30 [-2.70, +2.10] | 39.10 | +1.65 [-0.55, +3.70] |
| oracle env+mission, on | 38.30 | -0.55 [-1.45, +0.20] | 37.10 | -0.35 [-1.90, +0.90] |
| oracle env+mission, off | 38.55 | -0.30 [-2.70, +2.10] | 39.10 | +1.65 [-0.55, +3.70] |

**The collapse is gone.** The perfect environment detector with actions off went
from -25.75 (dev) and -23.95 (held-out) incidents to exactly the real-AI value
(38.70 and 39.50): identical runs, because with the fix the detection changes no
team-level behaviour (see "by construction" below). With actions on it costs
-0.55 and -0.35 (was -2.85 and +0.55), intervals including 0. Victims lost
(dev / held-out): AI on 13.80 / 14.70, oracle env on 14.85 / 13.90, oracle env off
14.95 / 14.35 (was 27.80 / 27.65). The remaining sign flip between the seed sets
(AI actions on vs off: dev +0.15, held-out -2.05) is the Step 3 action effect, not
the plumbing.

### Dose-response (oracle env minus shipped AI, actions off, AI on)

Only `--roads-persist` (how long unattended damage lasts) varies; paired 95% CI.

| Damage lasts | Step 3.1 plumbing (dev) | Label fix only (dev) | All fixes: dev | held-out | pooled |
|---|---|---|---|---|---|
| 20-60 ticks | -0.65 [-1.95, +0.65] | +0.05 [-1.30, +1.40] | +0.00 | +0.00 | +0.00 |
| 200-600 ticks | -11.90 [-14.35, -9.70] | -1.95 [-3.55, -0.35] | +0.00 | +0.00 | +0.00 |
| 600-2400 (default) | -25.60 [-28.55, -22.50] | -5.05 [-8.65, -2.10] | +0.00 | +0.00 | +0.00 |

(Victims lost, step 3.1 / label only at the default persistence: +12.85
[+9.65, +15.85] / +2.20 [-0.15, +5.10]. With all fixes the difference is exactly
0 at every setting.) The two ablation columns separate the fixes:

* **Fix 1 alone** (the label, with the old team-level reaction left in) cuts the
  default-persistence loss from -25.60 to -5.05 and removes the dependence on
  damage duration at short persistence. The oracle then flags a team only while its
  route touches debris (about 7% of the time), so the legacy RETURN_TO_SAFE_ZONE
  holds teams far less. It still costs incidents, because going home from a road
  problem is still the wrong response.
* **Fix 2** removes the rest: with it the detection triggers nothing at all with
  actions off, so the difference is exactly zero at every persistence on every seed
  set. **This zero is by construction, not a finding**: a detector whose only
  consumers are map actions has no effect when those actions are off.

### Specialization load with the fixes on (dev seeds 1001-1020)

Default configuration (AI off, actions off): **identical** to the Step 3.1 table in
`docs/SPECIALIZATION_LOAD.md` (every count, queue, wait and utilisation figure
equal), because no detector acts there, so the environment flag never held teams
and there was nothing to recover. The teams-working recovery shows in the
scenario the fixes were written for, the perfect environment detector (AI on,
actions off, same 20 seeds):

| Team-ticks | Incidents | Victims lost | Working | Returning to rest | Resting | Available |
|---|---|---|---|---|---|---|
| AI baseline (env class off) | 38.70 | 14.95 | 35.4% | 13.5% | 49.1% | 1.9% |
| oracle env, old plumbing (`--causal-fixes=off`) | 13.10 | 27.80 | **10.8%** | 4.4% | 83.6% | 1.2% |
| oracle env, fixes on | 38.70 | 14.95 | **35.4%** | 13.5% | 49.1% | 1.9% |

Working time returns from 10.8% to the baseline's 35.4% in every specialization
(PERIMETER 9.7% to 33.5%, EXTRACTION 10.9% to 35.6%, SEARCH 11.6% to 43.0%).
Fixed-oracle equals baseline exactly, as fix 2 implies.

### Checks run

| Check | Result |
|---|---|
| C1 md5 `./tron 1 3 1500 --fault-profile=combined` | `b248bea72ce2ceb81389762dccc0a749`, unchanged |
| none, mobility, communication, resource, random, combined datasets vs the baseline binary | byte-identical |
| `--diag` stderr (combined, none) | identical |
| `--causal-fixes=off` vs the Step 3.1 build (seeds 1003, 1007, 1500 ticks, actions off and on) | dataset and sidecar columns 1-23 byte-identical |
| `check-evaluate-determinism`, `check-live-override-noop-regression`, `check-consensus-shadow-regression` | pass |
| `check-causal-profile` (now also rejects a bad `--causal-fixes`, and it without causal) | pass |
| `check-causal-fixes` (new): dataset label == `route_touch`; == `env_hold` with fixes off | pass (30,000 tick rows each; the label differs from the old flag on 22,849 of them) |
| `check-causal-env-no-rtb` (new): perfect environment detector changes nothing with actions off; control with fixes off does | pass, 3 seeds |
| `check-causal-roads-connectivity` | pass: 0 cells cut off over 8 runs, 27 blockages created or refused |
| `check-causal-fault-exposure` | pass (environment label exposure 9.9% with actions on, 295.6 episodes/seed, mean episode 10.0 ticks) |
| `check-selftests` | pass (fourteen selftests) |
| Board | `check-board*` **not run**: no `arm-none-eabi-gcc` here. As a partial substitute every `core/*.c` was syntax-checked on the host with `-DENVIRONMENT_HAS_WORLD_STORAGE=0` (the board configuration): no errors. That caught one real slip (a laptop-only array reset outside its guard), fixed before any result was produced. |

The stall-waste counter used in the blind-A* paragraph above was added to the scratch copy of
the reroute probe ad hoc and is not part of `reroute_probe.patch`.


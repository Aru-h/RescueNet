# Final evaluation of the v1 world (Step 4)

Pre-registration: `docs/FINAL_EVALUATION_PREREG.md`, committed as `33337b4` before any Step 4 run. Raw harness output (every
table below, unedited): `docs/FINAL_EVALUATION_harness_output.md`; numbers behind the figures:
`docs/figures/final_eval_results.json`. Produced by `laptop/tools/final_eval.sh` at commit `f63de4a` (clean tree) in about 95
minutes on 4 cores.

**World:** `--fault-profile=causal`, persistent faults (`--causal-persist=on`), consequential roads, the Step 3.6 causal
fixes, hold-repair off (not adopted, `docs/HOLD_REPAIR.md`), recovery actions off. 10 teams, 3000 ticks (30 h). Stress 1, 2,
3.5 and 7. **Stress 2 is the realistic headline** (mean time between faults per team 4.4 h mobility / 6.6 h resource, matching
the 6-20 h literature range); stress 7 (1.7 h / 2.1 h) is a harsh stress test that was never derived from data. Development
seeds 1001-1020, held-out 3001-3020, pooled 40, paired by seed, paired bootstrap 95% intervals. The AI rows use the **shipped
model (trained on the old fault model; context only)**. It is not retrained or recalibrated; the planner keeps its Step 3.5
design and tables.

## What we now know (plain language)

1. **A perfect fault detector is worth at most about 5% more incidents resolved, and at the realistic stress it is worth
   about nothing.** Giving the planner the true fault state adds +0.35 incidents per seed at stress 2 (95% interval -0.53 to
   +1.25, out of 42.3), -0.10 at stress 3.5, and +1.75 at stress 7 (+0.47 to +2.98, out of 33.3). The best case is therefore
   a few percent, and it does not grow steadily with stress.
2. **The gain from detection is not monotone in stress.** It is positive at both ends (+1.15 [+0.15, +2.17] at stress 1 and
   +1.75 at stress 7) and about zero in the middle. I predicted a steady rise and was wrong. The stress 1 result is borderline
   (the lower bound is +0.15; the held-out seeds alone give +0.70 [-0.65, +2.00]) and I have no tested explanation for it;
   with four comparisons a borderline interval can be chance.
3. **Most of what faults cost is not recoverable by detecting them better.** Against a no-fault reference (47.4 incidents
   per seed), faults cost the planner 2.95, 5.07, 7.17 and 14.15 incidents at stress 1, 2, 3.5 and 7. A perfect detector
   recovers 39%, 7%, -1% and 12% of that. The rest is the cost of the repair itself: a faulted team has to go home for about
   70 ticks (mobility) or 40 (resource), and about 91-95% of faults end that way at every stress (`fault_ending.png`).
4. **The planner is not distinguishable from the fixed gates at any stress** (all four pooled intervals include 0: +0.15,
   -0.20, +0.40, -0.97 incidents per seed).
5. **The shipped AI helps at stress 7 and not elsewhere**, on a world it was not trained on: +1.68 [+0.05, +3.12] on the fixed
   gates and +1.23 [+0.20, +2.25] on the planner at stress 7; every other pooled interval includes 0. Its stress 7 gain is
   about the size of the detector ceiling (+1.75), so at that stress a new model has little measurable room beyond it.
6. **Capacity falls steadily with stress, but half the fleet is resting in every world.** Mean teams in service (working or
   available) is 4.16, 3.77, 3.32 and 2.58 of 10 at stress 1, 2, 3.5 and 7 (planner), against 4.81 with no faults at all.
   About half of all team-ticks are RESTING in every condition (46.6% with no faults, 48-53% with them), which is the normal
   stamina rest cycle. Faults take away a further 14%, 22%, 31% and 46% of the in-service level.
7. **Headroom does not track effective capacity** the way I predicted: the rank correlation between headroom and teams in
   service across the four stress levels is -0.20, not -0.8 or lower. The stress 7 end fits ("more headroom when fewer
   teams are in service"); the stress 1 end does not.
8. **Everything at stress 2 and 7 reproduces Step 3.6 exactly** (40 of 40 seeds identical, all five systems, incidents and
   victims), so the harness and the earlier results agree.

## Results

Incidents resolved and victims lost per seed (mean). `*` = shipped model, context only.

| Stress | System | Incidents dev | held-out | pooled | Victims lost pooled | Teams in service (of 10) |
|---|---|---|---|---|---|---|
| 1 | F fixed gates | 44.25 | 44.35 | 44.30 | 11.78 | 4.19 |
| 1 | P planner | 44.25 | 44.65 | 44.45 | 12.07 | 4.16 |
| 1 | PD planner + detector oracle | 45.85 | 45.35 | 45.60 | 12.05 | 4.25 |
| 1 | FA fixed gates + AI * | 44.70 | 44.25 | 44.48 | 12.15 | 4.21 |
| 1 | PA planner + AI * | 44.45 | 43.70 | 44.08 | 11.53 | 4.16 |
| 2 | F | 42.10 | 42.95 | 42.52 | 12.07 | 3.76 |
| 2 | P | 42.40 | 42.25 | 42.33 | 12.30 | 3.77 |
| 2 | PD | 43.25 | 42.10 | 42.67 | 12.38 | 3.86 |
| 2 | FA * | 42.55 | 41.60 | 42.08 | 12.85 | 3.92 |
| 2 | PA * | 41.70 | 43.25 | 42.48 | 12.45 | 3.81 |
| 3.5 | F | 40.55 | 39.10 | 39.83 | 13.43 | 3.28 |
| 3.5 | P | 39.95 | 40.50 | 40.23 | 13.32 | 3.32 |
| 3.5 | PD | 40.45 | 39.80 | 40.12 | 13.03 | 3.44 |
| 3.5 | FA * | 40.25 | 40.30 | 40.27 | 13.85 | 3.52 |
| 3.5 | PA * | 40.80 | 39.75 | 40.27 | 14.25 | 3.34 |
| 7 | F | 34.00 | 34.45 | 34.23 | 14.95 | 2.54 |
| 7 | P | 33.30 | 33.20 | 33.25 | 15.40 | 2.58 |
| 7 | PD | 34.85 | 35.15 | 35.00 | 16.48 | 2.63 |
| 7 | FA * | 35.80 | 36.00 | 35.90 | 16.12 | 2.83 |
| 7 | PA * | 35.05 | 33.90 | 34.48 | 15.05 | 2.63 |
| none | F, `--fault-profile=none` (reference) | 47.40 pooled | | | | 4.81 |

Paired differences in incidents resolved per seed, pooled over 40 seeds (95% interval; dev and held-out in the raw output):

| Stress | Detector headroom PD minus P | Planner minus fixed gates P minus F | AI gain on fixed gates FA minus F * | AI gain on planner PA minus P * |
|---|---|---|---|---|
| 1 | **+1.15 [+0.15, +2.17]** | +0.15 [-0.97, +1.25] | +0.17 [-0.90, +1.25] | -0.38 [-1.10, +0.38] |
| 2 | +0.35 [-0.53, +1.25] | -0.20 [-1.23, +0.80] | -0.45 [-1.65, +0.70] | +0.15 [-0.72, +0.97] |
| 3.5 | -0.10 [-1.23, +1.02] | +0.40 [-0.53, +1.30] | +0.45 [-0.62, +1.57] | +0.05 [-0.93, +1.05] |
| 7 | **+1.75 [+0.47, +2.98]** | -0.97 [-2.30, +0.38] | **+1.68 [+0.05, +3.12]** | **+1.23 [+0.20, +2.25]** |

Victims lost: all 16 pooled paired intervals include 0 (see the raw output). No conclusion is drawn from victims lost.

Teams in service (share of team-ticks, planner P): working 38.3 / 34.7 / 30.5 / 23.5%, available 3.4 / 3.0 / 2.7 / 2.3%,
faulted (mobility or resource fault active) 2.2 / 3.6 / 5.0 / 7.1%, returning to rest 7.6 / 9.0 / 10.8 / 14.0%, resting
48.5 / 49.7 / 51.0 / 53.2% at stress 1 / 2 / 3.5 / 7; no-fault reference: 43.5 / 4.6 / 0.0 / 5.3 / 46.6%.

Fault exposure (fixed gates, share of team-ticks with the fault active): mobility 6.9 / 11.8 / 17.3 / 26.2%, resource 3.5 / 6.0 /
9.4 / 14.8%, communication about 1.0-1.3%, environment (route-touch label) 1.9 / 3.6 / 5.6 / 8.7%, mission about 0.

### Figures

* `docs/figures/headroom_vs_stress.png`: detector headroom and shipped-AI gain against stress (left) and against mean teams in
  service (right), with 95% intervals. Read as headroom against effective capacity.
* `docs/figures/incidents_vs_stress.png`: incidents resolved per system against stress, with the no-fault reference.
* `docs/figures/fault_ending.png`: how mobility and resource faults end at each stress (about 91-95% by a go-home repair).
* `docs/figures/teams_in_service.png`: where team-ticks go (working, available, faulted, returning, resting) per stress.

## Scorecard (pre-registered predictions)

| # | Prediction | Result | Held? |
|---|---|---|---|
| A | Mean teams in service falls with stress, every system | monotone for all five systems (planner 4.16, 3.77, 3.32, 2.58) | **yes** |
| A2 | Planner in-service share: at least 0.85 (stress 1); 0.72-0.88 (stress 2); 0.35-0.60 (stress 7) | 0.416, 0.377, 0.258 | **no (all three)**: I did not account for the ~50% of time teams spend resting in every world |
| B | Detector headroom ordered by stress (1 <= 2 <= 3.5 <= 7) | +1.15, +0.35, -0.10, +1.75 | **no** |
| B2 | Headroom interval includes 0 at stress 1 and 2, excludes 0 at stress 7 | stress 1 interval excludes 0 | **no** (stress 2 and 7 as predicted) |
| B3 | Stress 3.5 headroom between the stress 2 and stress 7 values | -0.10 (below both) | **no** |
| C | Rank correlation of headroom with teams in service -0.8 or lower | -0.20 | **no** |
| C2 | The rank correlation is -1 | -0.20 | **no** |
| D | AI gain includes 0 at stress 1 and 2, positive and excluding 0 at stress 7 | yes for both FA minus F and PA minus P | **yes** |
| D2 | AI-gain point estimates ordered with stress | FA minus F: +0.17, -0.45, +0.45, +1.68; PA minus P: -0.38, +0.15, +0.05, +1.23 | **no** |
| E | Planner minus fixed gates includes 0 at all four stress levels | all four include 0 | **yes** |
| F | Fixed-gate incidents fall monotonically with stress | 44.30, 42.52, 39.83, 34.23 | **yes** |
| G | Victims lost: most pooled intervals include 0 | all 16 include 0 | **yes** |
| H | Stress 2 and 7 rows equal Step 3.6 exactly | 40 of 40 seeds identical for all five systems at both stress levels | **yes** |

Six predictions held (A, D, E, F, G, H); seven did not (A2, B, B2, B3, C, C2, D2). The failures are about the shape of the
headroom curve and about my estimate of how much of the fleet is in service; none changes a conclusion about the planner or the
AI. The requested prediction, that headroom grows as teams in service falls, is **not supported** by these four points.

## What was not done and what is not claimed

* **A true team-count sweep was NOT done.** I could not vary the number of teams without changing core: `NUM_TEAMS` is a
  compile-time constant with a layout guard (`CBBAAgentState_t` is pinned to 176 or 216 bytes in `core/mission_state.h` and
  must be re-verified on the real ARM target), and a 14-team CBBA frame (158 bytes) would not fit the 144-byte LoRa payload
  (`core/cbba.c`). Bypassing the guard or changing core was ruled out. **Teams in service is a companion axis built from the
  fault and rest dynamics of 10 teams. Nothing here says anything about how results change with the number of teams.** A true
  team-count sweep is a possible v2 experiment (it needs the layout guard and the frame limit addressed on purpose, and an
  ARM-target re-verification).
* The AI rows measure the **shipped model** on a world it was not trained on, with its Step 3.5 thresholds and the planner's
  Step 3.5 tables. They say nothing about a new or retrained model; the harness exists so that one can be dropped in
  (`docs/V1_TRAINING_HANDOFF.md`).
* The no-fault reference is `--fault-profile=none`, the original world without the causal profile's roads and aftershocks,
  so part of the "loss to faults" is not operational faults. It was added after the pre-registration as a reference only.
* Detector headroom is measured with the planner as the consumer of the detector's probability. It says nothing about what a
  perfect detector would add to a differently designed recovery policy.
* No claim about real robots; the fault parameters are derived or assumed (`KNOWN_LIMITATIONS.md` items 15 and 16), and stress
  7 was never derived from data.
* Differences below roughly one incident per seed are not resolvable with 40 seeds.

## Reproduce

```
laptop/tools/final_eval.sh --tag shipped --ref-3.6 <Step 3.6 result dir> --figures docs/figures
```

Writes `build/final_eval/shipped/` (per-run JSON, `summary.md`, `results.json`, `provenance.txt`) and the figures; takes about
95 minutes on 4 cores. With `--model-header`, `--thresholds` and `--calib-header` it evaluates a candidate model (see
`docs/V1_TRAINING_HANDOFF.md`).

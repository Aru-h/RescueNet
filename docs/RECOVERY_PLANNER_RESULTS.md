# Cost-based recovery planner: results (Step 3.5)

`--recovery=utility`, causal profile only. Design, derivations and decisions D1-D7:
`docs/RECOVERY_PLANNER_PROPOSAL.md`. The fourth causal fix (stall-aware evaluators) is in
`docs/CAUSAL_PLUMBING_FIXES.md`. What ends a fault and the recovery-action bugs:
`docs/RECOVERY_ACTION_SEMANTICS.md`.

> **Note on stress 7 (added in Step 3.6).** The evaluation stress of 7 used since Step 3 was **never
> derived from data**. At stress 7 the measured mobility and resource MTBFs per team are about 1.7 h
> and 2.1 h; the field-robot literature quoted in `docs/CAUSAL_FAULT_MODEL.md` (MTBF 6-20 h) matches
> **stress 1-2** (stress 2: 4.4 h and 6.6 h). From Step 3.6 on, stress 2 is the realistic headline and
> stress 7 is reported as a harsh-conditions stress test. See `docs/PERSISTENT_FAULTS_PROPOSAL.md`.

Setup for every number below: causal profile, `--fault-stress=7`, roads on, true-cost A*,
all four causal fixes on, 3000 ticks, 20 seeds per set. **Development seeds 1001-1020**
(the only seeds used for tuning and calibration), **held-out seeds 3001-3020**, pooled = 40
seeds. Differences are paired by seed; intervals are 95% bootstrap (10,000 resamples). Victims
lost are reported next to incidents resolved; no conclusion is drawn from one set alone.

## Headline

1. **Planner without AI vs the fixed gates: no gain.** Incidents resolved per seed, planner
   minus fixed gates (AI off both): dev -0.15 [-1.25, +0.95], held-out -0.70 [-1.75, +0.35],
   pooled **-0.42 [-1.18, +0.33]**. Victims lost: pooled +0.03 [-1.10, +1.15]. Both the fixed
   gates and the planner now sit at about 43 incidents per seed (fixed 43.33, planner 42.90).
2. **Headroom from a perfect detector: about zero.** Planner with the oracle fault probability
   minus planner without AI: dev -0.45 [-1.95, +1.00], held-out +0.55 [-0.70, +1.60], pooled
   **+0.05 [-0.93, +0.95]**; victims lost pooled +0.40 [-1.20, +2.00]. In this model, and with
   the planner as the consumer, a better fault detector (the AI or any other) has no
   measurable incident or victim benefit to capture. The reason is structural, see
   `docs/RECOVERY_ACTION_SEMANTICS.md`: how faults end hardly depends on the detector
   (mobility 42-43% by cause for fixed gates, planner and oracle alike).
3. **Planner with the AI vs planner without it: no gain.** Pooled -0.38 [-1.30, +0.60]
   incidents, +0.10 [-1.07, +1.30] victims.
4. **The AI is worse than no AI under the fixed gates** (pooled -2.05 [-2.92, -1.20], with
   the stall fix on). Under the planner the AI is neutral (3.), which is the planner's doing:
   it only lets AI-only evidence move the choice within a cap, and it leaves the rule-based
   floor in place.

What I did **not** find: any configuration where the planner beats the fixed gates with an
interval excluding 0. The cost model reproduces the fixed gates' result at lower chatter
(below) but does not improve on it.

## Main table (mean per seed)

| Condition | Dev inc | Held inc | Pooled inc | Dev lost | Held lost | Pooled lost |
|---|---|---|---|---|---|---|
| b0 fixed gates, AI off | 43.60 | 43.05 | 43.33 | 13.80 | 13.35 | 13.57 |
| b1 fixed gates, AI on | 41.85 | 40.70 | 41.27 | 13.40 | 14.05 | 13.72 |
| b0b fixed, AI off, actions on | 41.35 | 40.35 | 40.85 | 13.60 | 14.15 | 13.88 |
| b1b fixed, AI on, actions on | 41.05 | 39.10 | 40.08 | 13.65 | 15.65 | 14.65 |
| p1 planner, no AI | 43.45 | 42.35 | 42.90 | 13.90 | 13.30 | 13.60 |
| p2 planner + AI | 43.15 | 41.90 | 42.52 | 13.15 | 14.25 | 13.70 |
| p3 planner + oracle P (laptop only) | 43.00 | 42.90 | 42.95 | 13.00 | 15.00 | 14.00 |

Paired differences (minus the baseline named), incidents resolved, pooled:

| Comparison | Pooled incidents | Pooled victims lost |
|---|---|---|
| b1 - b0 (AI on, fixed gates) | -2.05 [-2.92, -1.20] | +0.15 [-1.23, +1.65] |
| b0b - b0 (actions on, fixed gates) | -2.48 [-3.52, -1.45] | +0.30 [-1.25, +1.80] |
| b1b - b0 | -3.25 [-4.30, -2.23] | +1.07 [-0.55, +2.73] |
| p1 - b0 | -0.42 [-1.18, +0.33] | +0.03 [-1.10, +1.15] |
| p2 - p1 | -0.38 [-1.30, +0.60] | +0.10 [-1.07, +1.30] |
| p3 - p1 | +0.05 [-0.93, +0.95] | +0.40 [-1.20, +2.00] |
| p2 - b1 (planner + AI vs fixed + AI) | +1.25 [+0.17, +2.35] | -0.03 [-1.27, +1.23] |

The only positive interval excluding 0 is planner + AI vs fixed gates + AI (+1.25): the planner
makes the AI less harmful, it does not make the system better than the no-AI baseline.

## Layer ablation (planner, no AI; minus b0)

| Layers | Dev | Held-out | Pooled | Pooled victims lost |
|---|---|---|---|---|
| team only (Layer A) | +0.25 [0.00, +0.75] | -0.05 | +0.10 [-0.10, +0.40] | -0.10 [-0.23, 0.00] |
| tasks only (REROUTE/CLEAR/REINFORCE/RELAY by cost) | -1.00 | -0.85 | -0.93 [-1.68, -0.17] | -0.60 [-1.90, +0.65] |
| tasks + preemption | -0.90 | -0.55 | -0.72 [-1.52, +0.07] | +0.28 [-0.93, +1.50] |
| all (default) | -0.15 | -0.70 | -0.42 [-1.18, +0.33] | +0.03 [-1.10, +1.15] |

Team-level planning alone reproduces the fixed gates to within noise (pooled +0.10
[-0.10, +0.40]): it adds the rule-based floor and chooses a go-home on cost alone about once per
seed (0.7-0.9 of about 125 go-homes). The map actions chosen by cost cost a little under the current dynamics
(tasks only: pooled -0.93, interval excludes 0); adding preemption recovers about a third.

## Sensitivity (planner, no AI; minus p1; pooled incidents)

| Variation | Dev | Held-out | Pooled |
|---|---|---|---|
| V x0.5 | -0.05 | +0.05 | +0.00 [-0.72, +0.70] |
| V x2 | +0.60 | -0.35 | +0.12 [-0.72, +0.95] |
| margin 0.1 | +0.20 | -0.35 | -0.07 [-0.72, +0.55] |
| margin 0.5 | -1.50 [-2.70, -0.25] | +0.85 | -0.33 [-1.20, +0.55] |
| margin 1.0 | -1.85 [-3.55, -0.20] | +0.80 | -0.53 [-1.62, +0.55] |
| crossing cost x0.5 | -0.35 | +0.60 | +0.12 [-0.65, +0.82] |
| crossing cost x1.5 | -0.10 | -0.25 | -0.17 [-1.10, +0.68] |
| repair effectiveness x0.5 | -0.50 | +0.10 | -0.20 [-0.80, +0.30] |
| reassign delay 20 (x0.5) | -1.00 | -0.50 | -0.75 [-1.82, +0.23] |
| reassign delay 60 (x1.5) | -0.45 | +1.25 [+0.35, +2.15] | +0.40 [-0.42, +1.23] |
| reassign delay 1 (extreme) | -2.50 [-3.65, -1.40] | -0.65 | -1.57 [-2.42, -0.70] |

No +-50% change moves the pooled result outside noise. Two entries are exactly 0 and are
**no-ops, not robustness evidence**: `repair x1.5` (mobility and resource repair is already
1.0 and the factor is clamped at 1) and `cblind` (it enters only the communication harm of
Layer A, which never exceeds its margin here). The margin is the one sensitive parameter and
the dev and held-out effects have opposite signs (margin 0.5: -1.50 dev, +0.85 held-out).
Defaults were chosen on dev seeds only and kept (V x1, margin 0.25).

## Blind-A* (`--roads-astar=blind`) and the REROUTE victims penalty

Minus blind with no action:

| | Dev inc | Held inc | Pooled inc | Dev lost | Held lost | Pooled lost |
|---|---|---|---|---|---|---|
| REROUTE (fixed gates) | -1.45 [-2.90, -0.05] | -0.40 | -0.93 [-1.77, -0.10] | +3.30 [+1.30, +5.40] | -0.30 | +1.50 [+0.17, +2.90] |
| planner (no AI) | -1.80 [-2.65, -0.90] | -0.20 | -1.00 [-1.68, -0.28] | +1.30 | +0.50 | +0.90 [-0.55, +2.45] |

The Step 3.2 note (`docs/CAUSAL_PLUMBING_FIXES.md`) left a REROUTE victims penalty of +3.00
pooled unexplained. With the stall fix the dev-set penalty is the same size (+3.30; it was +2.85
before) but it does not appear on the new held-out seeds (-0.30), so the pooled figure is
+1.50 [+0.17, +2.90] against +3.00 before (the held-out seeds also changed, see below). The
planner's blind-A* run shows no victims penalty (pooled +0.90, interval includes 0) but the
same incident loss as REROUTE alone. I have not explained the penalty; I only note it is not
reproduced on held-out.

## Preemptions, actions, chattering (mean per seed, dev / held-out)

| | Preempted CLEAR | Preempted REINFORCE | CLEAR finished / abandoned | Approvals asked (denied in the simulator) | Recommendation changes | Reversals within 10 ticks |
|---|---|---|---|---|---|---|
| b0 fixed, AI off | - | - | - | - | 447 / 421 | 36 / 38 |
| b1 fixed, AI on | - | - | - | - | 1297 / 1295 | 315 / 336 |
| p1 planner, no AI | 9.6 / 9.5 | 0.5 / 0.6 | 7.0 / 8.6 finished, 10.7 / 10.1 failed | 44 / 65 | 258 / 243 | 129 / 121 |
| p2 planner + AI | 10.3 / 8.5 | 0.7 / 0.4 | 7.9 / 7.5, 10.9 / 8.8 | 47 / 63 | 262 / 250 | 131 / 125 |
| p3 oracle | 11.0 / 8.4 | 1.1 / 1.1 | 7.9 / 7.6, 10.1 / 8.8 | 53 / 78 | 258 / 252 | 129 / 126 |

(Under the simulator's deny-by-default approvals, every requested approval is denied, so the
last-specialist gate blocks that preemption; `--planner-approval=auto` is a laptop-only upper
bound and was not used for these numbers.) Chattering: the AI-on fixed gates change their
recommendation about three times as often as the AI-off gates (1297 vs 447) and half of the
changes are reversals within 10 ticks (315 vs 36); the planner is about 40% below the fixed
AI-off gates in changes (258 vs 447) but its reversals are higher (129 vs 36) because it
re-evaluates every tick. Preempted CLEAR jobs fail (the team leaves before it finishes) about
as often as they succeed (7.0 vs 10.7), so the preemption benefit is scaled by the observed
success rate.

## The stall-aware fix: go-home events before and after

A debris crossing stalls a team for four ticks. The progress, mobility and consensus evaluators
read that as a mobility or progress fault, so about a quarter to a third of go-home
recommendations were set off by a stall. Fix 4 (`--causal-fixes` token `stall`, default on under
the causal profile; needs roads on) excuses stalls in those evaluators. Same fix applied to both
baselines. Go-home events are recommendation changes into RETURN_TO_SAFE_ZONE or ABORT_MISSION
for a working team, per seed:

| Condition | Set | Before: go-home | near a stall | share | incidents | After: go-home | near a stall | share | incidents |
|---|---|---|---|---|---|---|---|---|---|
| Fixed gates, AI off | dev | 169.3 | 55.6 | 32.9% | 40.10 | 124.5 | 3.9 | 3.1% | 43.60 |
| Fixed gates, AI off | held-out | 140.5 | 28.3 | 20.1% | 41.90 | 114.2 | 1.9 | 1.7% | 43.05 |
| Fixed gates, AI on | dev | 210.8 | 80.9 | 38.4% | 38.70 | 148.6 | 3.1 | 2.1% | 41.85 |
| Fixed gates, AI on | held-out | 190.7 | 68.5 | 35.9% | 37.05 | 142.9 | 3.3 | 2.3% | 40.70 |
| Planner, no AI | dev | 170.8 | 56.5 | 33.1% | 41.25 | 124.6 | 3.1 | 2.5% | 43.45 |
| Planner, no AI | held-out | 160.0 | 54.0 | 33.8% | 41.35 | 116.5 | 2.7 | 2.3% | 42.35 |

"Near a stall" = within 12 ticks after the team was held at a debris cell. The "before" runs
use `--causal-fixes=label,rtb,cost`, which I checked reproduces the earlier per-seed results
exactly (all six sets). Held-out rows in the "before" columns are the seeds 3001-3020 re-run;
they are not the 2001-2020 numbers in Steps 3-3.2. The fixed gates improve by 3.5 incidents per
seed on dev and 1.1 on held-out from this fix alone, so every earlier conclusion that compared
against the pre-fix gates is conditional on it (the planner's +1.15 incidents over the
fixed gates on dev before the fix, 41.25 vs 40.10, is gone once both get the fix: 43.45 vs 43.60).

## Calibration (D4)

Calibration tables (`core/planner_calibration.h`) were fitted on dev seeds 1001-1020 only
(isotonic, 32 bins, per source and class, separately for the AI-on and AI-off builds), after the
stall fix. Reliability on held-out seeds 3001-3020 (ECE = weighted mean |predicted - observed|),
AI-on build:

| Source | Mobility | Communication | Resource |
|---|---|---|---|
| AI scores | 0.0014 | 0.0003 | 0.0017 |
| Watchdog | 0.0331 | 0.0157 | 0.0097 |
| Consensus | 0.0024 | 0.0002 | n/a |
| **Fused P (max of sources)** | **0.0280** | **0.0142** | 0.0019 |

(Dev in-sample ECE of each source is 0.) The individual sources are well calibrated out of
sample. The **fused P is not**: taking the maximum of calibrated sources over-states mobility
and communication in the bins where the watchdog and consensus fire together (for example the
0.47-0.50 mobility bin has mean predicted 0.48 against an observed frequency of 0.04). Fusion is
therefore a known weakness; its effect is limited because the rule-based floor, not P, decides
the common go-home. A proper fusion (a second calibration layer on the fused score) is the
obvious next step and was not done. The AI-off build has no AI input, so its AI-source ECE
equals the base rate and is ignored.

## Decision log sample (`--planner-log`, one row per Layer A decision)

```
world_seed,tick,layer,team,chosen,alt,p_mobility,p_communication,p_resource,p_ai_mobility,p_ai_communication,p_ai_resource,cost_fault_or_benefit,cost_action,cost_alt,margin,flags,target_x,target_y,aux,aux_cost
1001,78,A,0,0,2,0.0366,0.2101,0.0000,0.0000,0.0000,0.0000,0.3565,0.0000,0.3565,2.0000,0,-1,-1,-1,0.000
```

Team 0 at tick 78: STAY (0) chosen over go-home (2); expected fault cost of staying 0.36
incident-ticks, below the margin of 2.0, so no go-home. `laptop/tools/check_planner_log.py`
verifies floor, approval, cooldown and code consistency from this file.

## Board footprint and hash integration

* With `ENVIRONMENT_HAS_WORLD_STORAGE=0` (the board configuration, which has no world), the
  planner compiles to about 7.3 KB of text (host `-O2` x86-64, an upper bound; the ARM figure was
  not measured) and 512 bytes of static RAM (`s_team`, `s_stats`, `s_pre`, `s_ctx`); calibration
  tables and the hazard table are `const`. Layer B and preemption are stubs there. The
  6.9 KB calibration histogram and the log sink are laptop-only (`world=1`: 21 KB static).
  No heap, recursion or libm.
* The ARM `check-board*` targets could **not** be run here (no ARM toolchain in this
  environment). I compiled every changed file with the host compiler and
  `-DENVIRONMENT_HAS_WORLD_STORAGE=0 -Wall -Wextra -Wpedantic` instead (clean); that is a syntax
  and warning check, not a link or size check on the target.
* Stateful additions that feed the run hash (all causal-profile-only and zero elsewhere):
  per-team planner state, `s_pre`, preempt success/failure counters, the stall hold ring
  `s_hold_ring`, plus the Step 3.1-3.2 state (`s_stall`, `s_road_kind`, label cell).

## Why held-out moved from 2001-2020 to 3001-3020

I checked which seeds produced the AI's data (`ml/MODEL_REPORT.md`, `ai_threshold_sweep.py`):

| Seeds | Used for |
|---|---|
| 1-300 (random profile) | AI training |
| 2001-2008 | AI threshold tuning and closed-loop development (MODEL_REPORT section 3) |
| 1001-1020 | the AI's reported evaluation (not used for training or tuning); **my development seeds** |
| 3001-3020 | unused anywhere |

The held-out range 2001-2020 overlaps the AI's threshold-tuning seeds (2001-2008), so a result
on it that involves the AI is not fully out-of-sample. I switched to 3001-3020, which nothing
has touched.

**Earlier results that used the overlapping range:** Steps 3, 3.1 and 3.2 held-out tables
(`docs/CONSEQUENTIAL_ROADS.md`, `docs/CAUSAL_PLUMBING_FIXES.md`: Table 1, the C13 oracle table,
the blind-A* table, dose-response), held-out and pooled columns, on the **AI-on rows**. The
**AI-off rows are unaffected** (no AI tuning touches them). The dev seeds 1001-1020 are not
affected. I did not re-run those tables on 3001-3020; the new planner runs are the only
results on the clean range.

## What this does and does not show

* The planner is correct against its spec: floor never violated, approvals respected,
  cooldowns respected, byte-identical to the fixed gates when
  `--recovery=fixed`.
* It does not beat the fixed gates on incidents or victims, and a perfect detector adds nothing
  to it. That is a property of the current model (fault lengths are set by the injector, a
  go-home costs far more than the fault it removes), not evidence that the planner or the AI is
  wrong in general. It should be revisited if the model gives faults that a team action can end
  early at low cost.
* Seeds: 20 per set, 40 pooled; most intervals include 0, so differences below about 1
  incident per seed are not resolved.

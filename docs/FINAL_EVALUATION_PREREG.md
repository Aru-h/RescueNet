# Step 4: final evaluation, pre-registration

Written and committed **before any Step 4 run**. The results go in `docs/FINAL_EVALUATION.md`. Nothing here changes the
simulator, the planner, the AI, `ml/` or any threshold.

## 1. World and what is evaluated

* **World: the v1 world as frozen in Step 3.6**, `--fault-profile=causal` with `--causal-persist=on` (the default),
  roads on, all causal fixes on (`label,rtb,cost,stall,semantics`). The Step 3.7 `hold-repair` token is **off**: under its
  pre-registered adoption rule it was not adopted (`docs/HOLD_REPAIR.md`).
* **Stress levels: 1, 2, 3.5 and 7** (`--fault-stress`). Measured mean time between faults per team (Step 3.6, from the
  persistence runs): stress 1 gives 7.8 h (mobility) and 14.4 h (resource); stress 2 gives 4.4 h and 6.6 h; stress 3.5 gives
  2.8 h and 4.3 h; stress 7 gives 1.7 h and 2.1 h. **Stress 2 is the realistic headline** (it sits in the 6-20 h
  field-robot range); stress 7 is a harsh stress test that was never derived from data. Stress 1 and 3.5 fill the sweep.
* **Seeds: development 1001-1020, held-out 3001-3020, pooled 40**, 3000 ticks (30 h), 10 teams. Paired by seed; paired
  bootstrap 95% intervals (10000 resamples, seed 0).
* **Systems (rows), per stress level:**
  1. **F** fixed gates, no AI (`--recovery=fixed`, the default)
  2. **P** planner, no AI (`--recovery=utility --planner-p=watchdog`)
  3. **PD** planner plus the detector oracle (`--recovery=utility --planner-p=oracle`), the headroom ceiling: the planner is
     told the true fault state
  4. **FA** fixed gates plus the shipped AI
  5. **PA** planner plus the shipped AI (`--recovery=utility --planner-p=ai`)

  The AI rows are labelled **"shipped model (trained on the old fault model; context only)"** everywhere. The shipped
  model was trained on the random fault profile before persistence existed. It is not retrained, recalibrated or tuned
  here. The planner keeps its Step 3.5 V (1), margin (0.25) and calibration tables.
* **Total: 4 stress levels x 5 systems x 2 seed sets = 40 runs.** Stress 2 and 7 re-measure the same seeds as Step 3.6 with
  the same code paths, so those rows must reproduce the Step 3.6 numbers to the last digit (a reproducibility check, not a
  prediction; see H below). Stress 1 and 3.5 are new.

## 2. Measurements

Per system and stress level: incidents resolved and victims lost per seed (dev, held-out, pooled); the paired
differences below with intervals; and **teams in service**.

**Teams in service** (new, requested). Computed from the per-tick dataset rows of every run. For each team-tick, in this
priority order:

1. *resting*: status RESTING;
2. *returning*: heading to a rest zone and not yet resting;
3. *faulted*: not resting or returning, and a mobility or resource fault is active (the two classes that remove a team's
   capacity; communication, environment and mission faults do not remove capacity and are not counted here);
4. *in service*: everything else, split into *working* (en route or on scene) and *available*.

Mean teams in service = 10 x the in-service share of team-ticks. Reported per stress level and per system, with the four
shares. Because the faulted/resting/returning shares depend on the system, the companion panel uses the planner (P) for
the x axis of the headroom curve and the table gives all five systems.

Paired differences (all pooled over 40 seeds, with development and held-out shown):

* **Detector headroom** = PD minus P (what a perfect detector would add to the planner).
* **Shipped-AI gain** = FA minus F and PA minus P (context only).
* **Planner minus fixed gates** = P minus F.

## 3. Headline figure and what it is read as

Detector headroom (PD minus P) against stress, with intervals, plus the shipped-AI gain on the same axes. A companion panel
shows the same headroom against the mean number of teams in service. This reads as headroom against **effective
capacity**: how much a perfect detector is worth as faults take more of the fleet out of service. A second figure shows
incidents resolved per system against stress, and a third the fault-ending breakdown (base repair / field repair /
natural) per stress level.

## 4. Team-count sweep: not done

A true team-count sweep (6, 8, 10, 14 teams) was **not** done. `NUM_TEAMS` is a compile-time constant with a layout guard
(`CBBAAgentState_t` is pinned to 176/216 bytes in `core/mission_state.h`, to be re-verified on the real ARM target), and a
14-team CBBA frame (158 bytes) would not fit the 144-byte LoRa payload (`core/cbba.c`). Bypassing the guard or changing core
was ruled out. Teams in service above is a companion axis, **not** a team-count experiment, and nothing in Step 4 claims
anything about the number of teams. A true team-count sweep is listed as a possible v2 experiment.

## 5. Predictions (direction and confidence, fixed before any run)

| # | Prediction | Confidence |
|---|---|---|
| A | Mean teams in service falls monotonically with stress (1 > 2 > 3.5 > 7), for every system | 90% |
| A2 | Magnitudes, planner P: in-service share of at least 0.85 at stress 1; between 0.72 and 0.88 at stress 2; between 0.35 and 0.60 at stress 7 | 55% each |
| B | Detector headroom (PD minus P) grows with stress: the four point estimates are ordered stress 1 <= 2 <= 3.5 <= 7 | 50% |
| B2 | The pooled headroom interval includes 0 at stress 1 and at stress 2; it excludes 0 (positive) at stress 7 | 70% jointly |
| B3 | Headroom at stress 3.5 lies between the stress 2 and stress 7 values | 55% |
| C | **Headroom grows as teams in service falls** (the requested direction): the rank correlation between headroom and mean teams in service across the four stress levels is -0.8 or lower | 65% |
| C2 | The rank correlation is exactly -1 (all four ordered) | 50% |
| D | Shipped-AI gain (FA minus F and PA minus P): interval includes 0 at stress 1 and 2; positive and excluding 0 at stress 7; point estimates ordered with stress | 60% for the first two clauses together, 50% for the ordering |
| E | Planner minus fixed gates: pooled interval includes 0 at all four stress levels | 70% |
| F | Fixed-gate incidents resolved fall monotonically with stress (1 > 2 > 3.5 > 7) | 90% |
| G | Victims lost: no conclusion from any single comparison; I expect most pooled intervals to include 0 | 70% |
| H | The stress 2 and stress 7 rows equal the Step 3.6 values exactly (F v1, P, PD, F+AI, P+AI; same seeds, same code) | 95% |

Notes. With four stress levels a rank correlation is a descriptive summary, not a test; I will report the four points and
intervals themselves. B and C could both fail because the low-stress headroom is small and noisy. If the headroom is
non-monotonic I will say so plainly.

## 6. What the harness must do (model-agnostic)

`laptop/tools/final_eval.sh` runs the whole evaluation from one command and takes an optional `--model-header <path>`
(a weights header with the same symbols and the 75-input contract) and an optional `--thresholds a,b,c`. It builds a scratch
AI binary under `build/final_eval/` by force-including the header (the approach `laptop/tools/evaluate.py` already uses),
never touching repository sources. With no `--model-header` it uses the shipped model and labels the AI rows as above. The
planner's calibration tables can be refit for a new model with one documented command (written, **not run** for any new
model). The harness changes no simulator code. Its default output for the shipped model is the Step 4 result.

## 7. What is not claimed

* Nothing about the number of teams.
* Nothing about a new or retrained AI. The AI rows measure the shipped model on a world it was not trained on.
* No claim that the planner is better or worse than the fixed gates beyond what the intervals show; a result whose interval
  includes 0 is reported as not distinguishable at 40 seeds.
* No claim about real robots. Every number is for this simulator, and the fault model's parameters are derived or assumed
  (`KNOWN_LIMITATIONS.md` items 15 and 16).
* The Step 3.7 hold-repair result applies to the opt-in token only and is not part of this evaluation.

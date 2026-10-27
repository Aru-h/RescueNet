# Training a model for the v1 world: hand-off notes

> **Superseded in part by docs/HEADROOM_MAP.md (Step 5):** the stress-7 detector headroom (+1.75) and the shipped-AI planner gain (+1.23) did not replicate on 100 seeds (Step 5: +0.01 [-0.84, +0.85] and +0.14 [-0.47, +0.73]); the +1.68 (fixed gates + shipped AI) was not re-measured in Step 5 and is UNCONFIRMED. **Section 8 has been rewritten from `docs/HEADROOM_MAP.md`; the old Step 4 table is kept there, marked.**


For the owner of the fault-predictor model. **Nothing here has been run for a new model**: no training data was generated,
no model was trained, calibrated or exported, `ml/` and the shipped weights are untouched. These are the commands, seed
ranges, column rules, data caveats and evaluation commands you need to do it yourself and drop the result into the Step 4
harness.

Everything below refers to the **v1 world**: `--fault-profile=causal`, `--causal-persist=on` (the default under causal),
roads on, the Step 3.6 causal fixes on, hold-repair off, recovery actions off (the default). Stress 2 is the realistic
headline (it matches the 6-20 h field-robot mean time between faults); stress 7 is a harsh stress test that was never
derived from data; the sweep also uses stress 1 and 3.5.

## 1. What the shipped model was trained on, and why it is only context now

The shipped weights (`core/fault_predictor_weights.h`, checkpoint `ml/fault_predictor_checkpoint.pt`) were trained on
`--fault-profile=random` data from seeds 1-300, before faults persisted, before consequential roads and before the
environment label changed. The harness reports it as **"shipped model (trained on the old fault model; context only)"**.
Its thresholds were tuned on seeds 2001-2008. Do not reuse 2001-2008 or 1-300 as evaluation seeds for a new model's claims.

## 2. Seed ranges (disjoint)

| Use | Seeds | Notes |
|---|---|---|
| Training | **5001-5300** | one dataset, any mix of stress levels (section 3) |
| Validation (thresholds, early checks) | **5301-5340** | never used to train; tune thresholds here |
| Development (planner calibration, `final_eval.sh calibrate`) | 1001-1020 | already used for the planner's tables and the Step 3.x development results |
| Held-out (report numbers) | 3001-3020 | touched only by `final_eval.sh`; never tune on it |
| Old data, do not reuse | 1-300 (training of the shipped model), 2001-2008 (its threshold tuning) | |
| Reserved, avoid | 9001-9002 | the Step 3.7 mechanism check |

The ranges must not overlap: training and validation seeds 5001-5340 are disjoint from 1001-1020, 3001-3020, 1-300, 2001-2008.

## 3. Generating training data (exact commands, not run)

Training data must come from the **AI-off** build (`./tron`, `FAULT_MODEL_LOADED=0`), so the model does not influence the
labels or the dynamics it is learning from.

```
make tron
mkdir -p data_v1/s2 data_v1/s7          # one directory per configuration: the CSV name is fixed
(cd data_v1/s2 && ../../tron 5001 150 3000 --fault-profile=causal --fault-stress=2)
(cd data_v1/s7 && ../../tron 5151 150 3000 --fault-profile=causal --fault-stress=7)
```

`./tron <first seed> <number of seeds> <ticks> --fault-profile=causal --fault-stress=<x>` writes
`tron_snapshot_dataset.csv` (and, for the causal profile, the ground-truth sidecar `tron_causal_truth.csv`) in the current
directory, one row per team per tick (3000 ticks, 10 teams, so 30 000 rows per seed). Validation data the same way with
seeds 5301-5340.

Suggested mix (a suggestion, not a requirement): split 5001-5300 across the stress levels the model will be judged at, using
**disjoint seed blocks per stress level** (for example 75 seeds each at stress 1, 2, 3.5 and 7: 5001-5075, 5076-5150,
5151-5225, 5226-5300), because the same seed gives the same world at every stress level and duplicating worlds across
levels inflates the apparent sample size. `run_id` includes the stress level, so rows from different stress levels do not
collide, but it does **not** include `--causal-persist`, `--causal-fixes` or the planner flags: use one configuration per
dataset and do not concatenate files made with different flags and the same seeds.

Then, from the repository root, the existing pipeline:

```
python ml/data_prep.py <your concatenated csv>      # run-based split, scaling, leak guards
python ml/generate_feature_spec.py                  # regenerates core/feature_spec.h only if feature_spec.json changed
python ml/train.py
python ml/evaluate.py
python ml/export.py                                 # writes fault_predictor_weights.h; do not copy it into core/ to evaluate
```

`data_prep.py` splits by whole run (70/15/15) inside whatever file you give it. The validation seeds 5301-5340 are an
additional, outside check: score the exported model on them in closed loop (section 7) before looking at any other seeds.

## 4. Labels, features and ground-truth-only columns

* **Features: exactly the 75 inputs in `ml/feature_spec.json`.** The contract is unchanged by Steps 3.x and 4. A new model
  must keep the 75-input order and scaler; `make check-feature-spec-fresh`, `make check-feature-pack-parity` and
  `make check-feature-spec-leak-guard` enforce it. Any other column is not a feature.
* **Labels** (training targets only): `fault_active_mobility`, `fault_active_communication`, `fault_active_resource`,
  `fault_active_environment`, `fault_active_mission` in `tron_snapshot_dataset.csv`.
* **Ground truth, never features:** `fault_active_mask`, `fault_severity_*`, `fault_ticks_remaining_*`, every `gt_*`
  column, the consensus outputs (`consensus_*`, `peer_*` votes), and the entire sidecar `tron_causal_truth.csv`
  (`hidden_wear`, `link_margin_db`, `hazard_*`, `ticks_since_rest`, `onset_cause_*`, `clear_cause_*`, `action_mask`,
  `route_touch`, `env_hold`, `field_state`, `escalated_*`, `risk_*_h`). The sidecar is simulator-internal state the real
  system cannot see; `data_prep.py` already stops on name- and value-based leaks, and you should not weaken that guard.
* The `recovery_*` and `eval_*` columns are the rule gates' own outputs under the AI-off build, not part of the 75 inputs.

## 5. What changed in the world since the shipped model's data (read before you train)

* **The environment label changed.** Under the causal profile with the Step 3.2 fix on (the default),
  `fault_active_environment` is the **route-touch label**: it is 1 when the team's planned route passes through a damaged
  road cell (`route_touch` in the sidecar; `make check-causal-fixes` verifies they are equal). It is no longer a per-team
  fault flag injected on a timer. Environment faults therefore depend on the map state and the team's path, not on a dice
  roll, and the class base rate differs from the old data.
* **Faults now persist, which changes class balance and label persistence.** Mobility and resource faults last until a
  go-home repair (about 70 ticks for mobility, about 40 for resource), a field repair, or a long natural recovery tail
  (mean 1084 ticks), and an unremedied one can escalate a severity step. In the old data they ended on a 6-14 tick timer.
  Consequences: far more positive rows per class, long runs of nearly identical consecutive rows (the effective sample
  size is much smaller than the row count), labels that stay on while the team rests or returns to base, and strong
  dependence on the stress level. Class exposure per stress level is in section 8.
* **About half of all team-ticks are resting even with no faults.** At stress 2 a team spends roughly 45% of its time
  resting and about 40% in service (working or available). A model can look good by learning "resting" cues; evaluate
  closed-loop, not only by offline AUC.
* **Closed-loop shift.** Training rows come from the AI-off gates; once a model is on, it changes team behaviour (which
  teams rest, when), so the states it sees at deployment differ from its training rows. The Step 3.6 and Step 4 numbers
  for the shipped model show this effect (its gain at stress 7 exists although it was trained on the old world). ‡ *(Superseded in part by `docs/HEADROOM_MAP.md`, Step 5: the shipped model's stress-7 gain did not replicate on 100 seeds, +0.14 [-0.47, +0.73] on the planner; see the banner and section 8.)*
* **No team-count or hardware claims.** Nothing here concerns the number of teams; a true team-count sweep was not done.

## 6. What the model's output is used for

The planner (`--recovery=utility --planner-p=ai`) and the fixed gates (`FAULT_MODEL_LOADED=1`) read the five class scores
per team per tick. Per-class thresholds (`FAULT_PREDICTOR_CLASS_THRESHOLDS`, in `core/fault_predictor.h`) decide when the
fixed gates act on a score; the planner uses calibrated probabilities instead (tables in `core/planner_calibration.h`).
The environment and mission thresholds ship switched off (1.01).

## 7. Evaluating and calibrating a candidate through the harness (exact commands)

All of this builds scratch binaries under `build/final_eval/` by force-including your headers (the same mechanism as
`laptop/tools/evaluate.py --model-header`); it never modifies repository sources, and your header does not have to be copied
into `core/`. The header must define the same symbols as `core/fault_predictor_weights.h` (75 inputs, 128-64-5 layers;
`python ml/export.py` writes one).

1. **Pick thresholds on the validation seeds**, not on the development or held-out seeds. For each candidate threshold set:
   ```
   laptop/tools/final_eval.sh --tag val_a --model-header fault_predictor_weights.h --thresholds 0.57,0.96,0.70 \
       --stress "2 7" --dev-seeds 5301-5340 --sets D --no-reference
   ```
   (`--sets D` runs only the "development" role, here the validation seeds; read `build/final_eval/val_a/summary.md`.)
2. **Refit the planner's calibration tables for your model, on development seeds only** (one command; it writes
   `build/final_eval/<tag>/planner_calibration.h` and never touches `core/`):
   ```
   laptop/tools/final_eval.sh calibrate --tag mymodel --model-header fault_predictor_weights.h --thresholds 0.57,0.96,0.70
   ```
   It runs development seeds 1001-1020 with both an AI-on and an AI-off scratch build, dumps the planner's reliability
   histograms, and fits monotone (isotonic) tables with `laptop/tools/planner_calibration_fit.py fit`. The shipped tables were
   fitted at stress 7; add `--calib-stress "1 2 3.5 7"` to pool the four stress levels. Only the AI-on planner row (PA)
   changes with the tables; the no-AI rows do not depend on them.
3. **Run the full evaluation with your model and tables:**
   ```
   laptop/tools/final_eval.sh --tag mymodel --model-header fault_predictor_weights.h --thresholds 0.57,0.96,0.70 \
       --calib-header build/final_eval/mymodel/planner_calibration.h --ref-3.6 <optional Step 3.6 result dir>
   ```
   The report (`build/final_eval/mymodel/summary.md`, figures in `build/final_eval/mymodel/figures/`) labels the AI rows
   with your header's file name and md5 instead of the "shipped model" label. Held-out seeds 3001-3020 are read here and
   nowhere earlier in your workflow.
4. Also run the C-side contracts on your export before trusting any closed-loop number:
   `make check-feature-spec-fresh`, `make check-feature-pack-parity`, `python ml/export_parity_fixture.py --checkpoint <your checkpoint>`,
   `make check-model-parity`.

## 8. What a new model must beat

**Rewritten after Step 5 from `docs/HEADROOM_MAP.md`** (100 development seeds 1001-1100, confirmed on 100 held-out seeds
3001-3100, paired 95% intervals, v1 world, planner P as the consumer of the detector). The Step 4 version of this section
is kept at the end, marked as superseded.

**Plainly: a retrained detector cannot be expected to improve mission outcomes in the v1 world.**

* **The detector ceiling is indistinguishable from zero.** A perfect fault detector given to the planner (PD minus P) adds
  **+0.28 [-0.33, +0.89] incidents per seed at stress 2 and +0.01 [-0.84, +0.85] at stress 7**; the upper bound of either
  interval is about +0.9 incidents (about 2 percent of throughput). Detection delay (5 to 40 ticks), a 30 percent miss rate with
  1 percent false alarms, and removing 25 to 75 percent of the baseline's evidence all leave this at zero. The Step 4
  stress-7 figure (+1.75) was a small-sample result that did not replicate.
* **The shipped model on the planner is also zero:** PA minus P is **-0.05 [-0.60, +0.50] at stress 2 and +0.14
  [-0.47, +0.73] at stress 7**. The Step 4 "+1.23 at stress 7" did not replicate. The +1.68 on the fixed gates (FA minus F) was
  not re-measured in Step 5 and is **unconfirmed**; do not use it as a target. Fixed gates and planner are also
  indistinguishable (F minus P -0.26 and -0.28).
* **Where the headroom is.** Not in detection. The confirmed headroom in the v1 world is in work speed, travel, rest and
  stamina, team mix and incident discovery (`docs/HEADROOM_MAP.md`, sections D to G); a fault model cannot supply any of it.
  Cheaper responses (relief, support repair, lane opening, degraded mode, autonomy) do not make a better detector more valuable:
  with all five on, the detector-oracle gain is +0.19 and -0.27 incidents.
* **Success criteria for a new model** (replace the old "match or exceed the shipped model's gain"):
  1. **Host/board parity:** the exported weights reproduce the training framework's outputs on the host and on the board
     (`make check-feature-spec-fresh`, `make check-feature-pack-parity`, `python ml/export_parity_fixture.py --checkpoint <your checkpoint>`,
     `make check-model-parity`), with the board build still compiling and linking (`make check-board-single`).
  2. **Calibration quality:** scores are well calibrated on the validation seeds 5301-5340 (the reliability histograms and the
     isotonic tables of `final_eval.sh calibrate`, section 7), because the planner reads the score as a probability.
  3. **No harm** against the baselines in the same rows: PA minus P and FA minus F with intervals that do not exclude 0 on the
     negative side at stress 1, 2, 3.5 and 7, and no worse victims lost. The shipped model (-0.05 and +0.14 on the planner) is the
     reference, not a target to beat.
  4. **Not** an incidents gain: no incidents gain is expected, and one should not be claimed unless it is confirmed on held-out
     seeds with the multiple-testing correction used in Step 5.
* **Noise floor.** With 100 seeds, a paired 95% interval on incidents is about +-0.6 to +-0.9 incidents per seed; differences below
  that are not resolvable. With the 40 seeds of the Step 4 harness the interval is wider (about +-0.9 to +-1.3). A candidate whose interval
  overlaps the shipped model's has not shown an improvement.
* **Victims lost** shows no reliable change in any detector comparison; do not claim a victims-lost gain from one comparison.
* **Fault load** to expect in your training data (fixed gates, share of team-ticks with the fault active): mobility 6.9 /
  11.8 / 17.3 / 26.2%, resource 3.5 / 6.0 / 9.4 / 14.8%, communication about 1.0-1.3%, environment (route-touch label) 1.9 /
  3.6 / 5.6 / 8.7%, mission about 0%, at stress 1 / 2 / 3.5 / 7. Mission is essentially never active; do not expect to learn it.
* The gates catch mobility faults fast (median 2 ticks) but miss 38 to 43 percent of mobility, 85 to 86 percent of communication
  and 63 to 67 percent of resource episodes before they end (`docs/HEADROOM_MAP.md`, B0.2); the missed episodes are where any
  headroom would have to come from, and the perfect-detector result says catching them changes no outcome.
* Report your own candidate with `final_eval.sh` (section 7) and the held-out seeds only once, after thresholds and
  calibration are fixed on validation and development seeds. (The seed ranges and commands in sections 2, 3 and 7 are unchanged.
  Step 5 read held-out seeds 3001-3100 for the world measurements; no model was trained or tuned on them.)

### Step 4 table and ceiling (superseded in part; kept for reference)

From `docs/FINAL_EVALUATION.md` (v1 world, 40 seeds pooled, 95% paired intervals). The shipped model is the **context** row, not a
fair baseline: it was trained on the old fault model. Incidents resolved per seed. ‡ *The stress-7 PD, FA and PA rows and the
differences derived from them (+1.75, +1.68, +1.23) did not replicate (the first and third) or were not re-measured (the second)
in Step 5; see the banner.*

| Stress | Fixed gates F | Planner P | Planner + perfect detector PD (ceiling) | Shipped model on fixed gates FA | Shipped model on planner PA |
|---|---|---|---|---|---|
| 1 | 44.30 | 44.45 | 45.60 | 44.48 | 44.08 |
| 2 (realistic headline) | 42.52 | 42.33 | 42.67 | 42.08 | 42.48 |
| 3.5 | 39.83 | 40.23 | 40.12 | 40.27 | 40.27 |
| 7 (harsh stress test) ‡ | 34.23 | 33.25 | 35.00 | 35.90 | 34.48 |

* **Ceiling (Step 4).** The detector-oracle headroom (PD minus P) was +1.15 [+0.15, +2.17] at stress 1, +0.35 [-0.53, +1.25] at stress 2,
  -0.10 [-1.23, +1.02] at stress 3.5 and +1.75 [+0.47, +2.98] ‡ at stress 7 incidents per seed. ‡ *(Superseded: stress 7 is +0.01 [-0.84, +0.85] in
  Step 5; the stress 1 and 3.5 values were not re-measured.)*
* **The shipped model's gain (Step 4).** At stress 7, +1.68 [+0.05, +3.12] ‡ on the fixed gates (FA minus F) and +1.23 [+0.20, +2.25] ‡
  on the planner (PA minus P); at stress 1, 2 and 3.5 indistinguishable from 0. ‡ *(Superseded: PA minus P is +0.14 [-0.47, +0.73]
  at stress 7 in Step 5; the +1.68 is unconfirmed. The old instruction to "match or exceed" these no longer applies.)*

## 9. Things that are easy to get wrong

* Do not tune thresholds on 1001-1020 or 3001-3020; use 5301-5340.
* Do not train on rows made with the AI on, or with `--causal-persist` other than `on`, unless you mean to.
* Do not split rows randomly: split by run (`run_id`), as `data_prep.py` does, because consecutive rows of a run are nearly
  duplicates.
* Report held-out results from `final_eval.sh` only, and say that the shipped model's numbers are context, not a baseline
  it was trained against.

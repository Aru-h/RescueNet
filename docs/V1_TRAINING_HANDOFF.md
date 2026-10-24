# Training a model for the v1 world: hand-off notes

> **DRAFT (committed mid-run).** Section 8 ("what a new model must beat") is filled in from the Step 4 sweep when it finishes, and the `--sets` option used in section 7 is added to `final_eval.sh` after the sweep (a running bash script must not be edited). Everything else is final.

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
  dependence on the stress level. Class exposure and base rates per stress level are in section 8.
* **About half of all team-ticks are resting even with no faults.** At stress 2 a team spends roughly 45% of its time
  resting and about 40% in service (working or available). A model can look good by learning "resting" cues; evaluate
  closed-loop, not only by offline AUC.
* **Closed-loop shift.** Training rows come from the AI-off gates; once a model is on, it changes team behaviour (which
  teams rest, when), so the states it sees at deployment differ from its training rows. The Step 3.6 and Step 4 numbers
  for the shipped model show this effect (its gain at stress 7 exists although it was trained on the old world).
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

*(filled in from the Step 4 results, `docs/FINAL_EVALUATION.md`; see that file for intervals and limits)*

## 9. Things that are easy to get wrong

* Do not tune thresholds on 1001-1020 or 3001-3020; use 5301-5340.
* Do not train on rows made with the AI on, or with `--causal-persist` other than `on`, unless you mean to.
* Do not split rows randomly: split by run (`run_id`), as `data_prep.py` does, because consecutive rows of a run are nearly
  duplicates.
* Report held-out results from `final_eval.sh` only, and say that the shipped model's numbers are context, not a baseline
  it was trained against.

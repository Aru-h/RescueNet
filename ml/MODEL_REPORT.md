# Fault Predictor — Model Report

## 1. Summary

The rescue-coordination system detects faults with a rule-based watchdog that
compares each team's expected state (from a digital twin) with its actual
state, backed by peer consensus. This report covers the AI layer added beside
them: a small neural network that reads one team's state every tick and
outputs five independent fault scores (mobility, communication, resource,
environment, mission).

- **Size:** 75 inputs, two hidden layers (128 and 64), 5 outputs. 18,309
  weights and biases after export, about 73 KB as float32. The forward pass is
  plain C in `core/fault_predictor.c`, with no ML runtime on the board.
- **Build:** off by default (`FAULT_MODEL_LOADED=0`); on in `make tron-ai` and
  in the board's STM32CubeIDE `DevRam` configuration (§8).
- **Safety design:** the AI never acts alone on serious actions. On its own it
  can only trigger local recovery; anything stronger needs corroboration (§8).
- **Offline result:** the model ranks faults better than the watchdog's single
  confidence score (higher AUC) in all five classes; strong on communication
  and resource, moderate on mobility and mission, weakest on environment (§6).
- **Closed-loop result:** acting on mobility, communication and resource, the
  AI raises incidents resolved from 35.9 to 38.95 per run (+3.05, 95% CI
  [1.30, 4.80]) on held-out seeds. Environment and mission are switched off
  because acting on them makes the mission worse **even with a perfect
  detector**: those faults hit the map or the incident, while every recovery
  action acts on the team (§7).
- **The limits are in the recovery layer, not the model.** A perfect detector
  on the shipped classes reaches only 39.7 incidents vs the model's 38.95,
  because faults in this simulator clear on a fixed timer that no recovery
  action can shorten (§9).
- **Everything here was measured on simulator data**, not on real hardware.

## 2. Problem and labels

Each row is one team at one tick. The task is present-tick detection: is a
fault of class X active right now?

- Five independent binary targets. Several can be active at once, so the model
  uses five independent sigmoids, not a softmax.
- Labels come only from the fault injector's own `fault_active_*` columns,
  never from the watchdog's or consensus's output, so the model is not trained
  to copy them.
- Fault onset in the simulator is random, so forecasting is not attempted. The
  model detects; the lead-time analysis (§6.2) measures how early compared with
  the watchdog.

## 3. Data and split

- **Source:** `tron_snapshot_dataset.csv`, written by the laptop simulator
  (AI off) with `--fault-profile=random`, training seeds 1–300.
- **Size:** several million rows; after the split the validation set has
  1,600,000 rows and the test set 1,500,000.
- **Split:** by whole simulation run, 70% train, 15% validation, 15% test. No
  run appears in more than one split. The split is re-drawn if the fault rate
  drifts too far between splits.
- **Scaling:** fitted on the training split only. The same constants are baked
  into `core/feature_spec.h`, so the board scales inputs exactly as training
  did.
- **Leak guards** (stop the pipeline if triggered): no feature may be a
  ground-truth, `gt_*` or consensus column; no feature may agree with a label on
  99.5% of rows or correlate with it at 0.99 or above.
- **Seed use across the project:** training 1–300; closed-loop threshold tuning
  (dev) 2001–2008; every closed-loop result reported here 1001–1020.

## 4. Features (75 inputs)

The exact ordered list is `ml/feature_spec.json`. In groups:

| Group | Examples |
|---|---|
| Team and task state | stamina, hours worked, equipment level, incident priority, victim count |
| Digital-twin comparison | expected vs actual progress, progress error, expected vs actual stamina, stamina error, remaining path cost, mission stage, communication health |
| Watchdog evidence | seven evidence channels (progress, stamina, communication, operating state, path, assignment, timeout) and a confirmed-mobility-impairment flag |
| Movement history | fraction of the last 5 and 10 ticks spent stalled, change in priority and victim count |
| Map signals | travel cost at the team's own cell, zone id (one-hot), zone trust, path-unreachable flag |
| Categorical one-hots | specialization, expected operating state, status, fitness tier, required capability (6 values: -1 none, 0–4; 4 is `SPEC_SUPPORT`) |
| Flags | has support, 8 equipment flags, 4 hazard flags |

`core/feature_pack.c` builds this vector each tick from live state, before the
recovery step of that tick, which is exactly where the training data was
captured. `make check-feature-pack-parity` confirms the C and Python vectors
match (worst difference about 1e-7 on scaled values, tolerance 1e-5).

## 5. Model and training

- **Architecture:** 75 → 128 → 64 → 5. Each hidden layer is Linear, BatchNorm,
  ReLU, Dropout (0.2). The output layer is linear (raw logits).
- **Loss:** binary cross-entropy with logits, with a per-class positive weight
  (mobility 3.21, communication 3.21, resource 3.22, environment 13.38,
  mission 11.15) to offset class imbalance.
- **Optimizer:** Adam, learning rate 0.001, batch size 1,024.
- **Stopping:** up to 25 epochs, stopping when validation loss fails to improve
  for 2 epochs in a row. The shipped weights come from the epoch-8 checkpoint,
  validation loss 0.435948.
- **Export:** BatchNorm is folded into the linear layers, so the C code needs
  only matrix multiplies, ReLU and a sigmoid. `export.py` checks the feature
  order and scaler against `feature_spec.json` before writing the header.
- **C-vs-PyTorch parity:** `make check-model-parity` feeds 19 vectors (12 real
  simulator vectors, 7 edge cases) through the C forward pass; the worst
  difference from PyTorch is 3.6e-7 (tolerance 1e-5). With the model off, the
  same test requires all scores to be exactly 0.

## 6. Offline results

Numbers in this section are on the held-out test split of the training
dataset (AI off, so the model is only observing). The baseline is the
watchdog's own single confidence score, scanned over every threshold and taken
at its best operating point.

### 6.1 Ranking quality and precision/recall (each class's best-F1 threshold)

| Class | Model AUC | Watchdog AUC | Model precision | Model recall | Model F1 |
|---|---|---|---|---|---|
| mobility | 0.872 | 0.515 | 0.617 | 0.617 | 0.617 |
| communication | 0.997 | 0.974 | 0.966 | 1.000 | 0.983 |
| resource | 0.986 | 0.544 | 0.839 | 0.904 | 0.870 |
| environment | 0.854 | 0.498 | 0.260 | 0.663 | 0.373 |
| mission | 0.959 | 0.602 | 0.715 | 0.654 | 0.683 |

The model exceeds the watchdog's precision in every class and its recall in
four of five. The exception is mission: the watchdog's best point reaches
recall 0.92 at precision 0.16; the model's strength there is precision. The
model has richer inputs than the watchdog's single score (§4), so this shows
what the extra inputs add, not a like-for-like contest between two algorithms.

### 6.2 Lead time against the watchdog

For each fault episode in the test set: did the model flag it before the
watchdog did? Positive lead means the model was earlier. Episodes cut off by
the start or end of a run are excluded; a model flag before the true onset
never counts.

| Class | Episodes | Mean lead (ticks) | Median lead (ticks) | Caught only by model | Missed by model |
|---|---|---|---|---|---|
| mobility | 8117 | -16.2 | 2 | 6 | 17 |
| communication | 8151 | 1.7 | 2 | 0 | 0 |
| resource | 8141 | 36.2 | 18 | 5 | 1 |
| environment | 9904 | 27.9 | 21 | 25 | 11 |
| mission | 2695 | 57.2 | 54 | 3 | 0 |

"Caught only by model" means the watchdog never flagged that episode. Mobility
is the exception on timing: the mean is negative, though the median is
slightly positive.

## 7. Decision thresholds

A threshold turns a score into yes/no: a class is flagged when
`score >= threshold`. The watchdog does the same with one number
(`anomaly_detected = anomaly_confidence >= 0.10`, `WATCHDOG_ANOMALY_THRESHOLD`
in `core/watchdog.c`).

**In use** (`FAULT_PREDICTOR_CLASS_THRESHOLDS` in `core/fault_predictor.h`):

| Class | Threshold | Origin |
|---|---|---|
| mobility | 0.573 | validation best-F1 point |
| communication | 0.965 | validation best-F1 point |
| resource | 0.705 | validation best-F1 point |
| environment | off (1.01) | switched off after closed-loop testing |
| mission | off (1.01) | switched off after closed-loop testing |

**How these were chosen.** The offline analysis first proposed recall-leaning
points (mobility 0.532, environment 0.706, mission 0.731; communication and
resource at best F1), reasoning that a false alarm on a cheap action is
absorbed while a missed fault has no safety net. That set was then tested in
closed loop, where the AI's flags actually move teams, and it hurt the
mission: incidents resolved fell from 36.6 (no AI) to 23.2 per run. An
ablation on dev seeds 2001–2008 (no-AI reference 37.0) isolated the cause:

| Classes acting | Incidents resolved |
|---|---|
| mobility only | 35.6 |
| communication only | 36.6 |
| resource only | 38.1 |
| environment only | 32.9 |
| mission only | 25.2 |
| mobility + communication + resource | 38.8 |
| those three + environment and mission at 0.95 | 29.8 |

Acting on environment or mission lowers incidents resolved even at threshold
0.95.

**Why environment and mission are off: the recovery layer, not the model.**
Neither fault damages the team. An environment fault raises the travel cost of
one road cell on the team's path; a mission fault makes the *incident* worse
(more victims, priority raised toward critical). Every recovery action,
however, acts on the team: local recovery, reallocating its task, returning it
to the safe zone, or aborting. So when these classes trigger recovery, the
system pulls a healthy team off its work, in the mission case off an incident
that has just become more urgent, and fixes nothing, because the fault is in
the map or the incident.

This was tested directly by replacing the model's scores with perfect ground
truth, a detector that is never wrong, and letting only these classes act
(held-out seeds 1001–1020, random faults, paired against no AI):

| What acts | Incidents resolved | vs no AI (95% CI) |
|---|---|---|
| No AI | 36.65 | — |
| Perfect detector, environment only | 34.40 | −2.25 [−3.50, −1.00] |
| Perfect detector, mission only | 29.30 | −7.35 [−9.20, −5.50] |
| Perfect detector, environment + mission | 27.85 | −8.80 [−10.40, −7.25] |
| Real model, environment + mission | 22.15 | −14.50 [−18.80, −11.20] |

Even with perfect detection, acting on these classes costs up to 8.8 incidents
per run, so they would have to be off with any model. The real model loses
more (−14.5) because it also raises false alarms, but that is not what forces
the decision. Turning these classes back on needs recovery actions that act on
the map or the incident (for example rerouting around a blocked road, or
sending reinforcements to a worsening incident), not a better model. The
experiment reproduces with `bash laptop/tools/ai_env_mission_oracle.sh`; its
control rows reproduce the shipped results exactly (no AI 36.65, shipped AI
38.95, perfect detector on the three shipped classes 39.70).

Among the three-class variants the differences were within noise, so mobility
uses its validated best-F1 point (0.573). The shipped set was then verified on
held-out seeds 1001–1020 (§9). After any retrain, re-run
`laptop/tools/ai_threshold_sweep.py` the same way.

## 8. How it runs on the board

The model is compiled in only with `FAULT_MODEL_LOADED=1`: the board's
STM32CubeIDE `DevRam` configuration and `make tron-ai` set it; the default
laptop build does not, so the dataset and regression tests are unaffected.

Every tick, for each of the 10 teams:

1. `core/feature_pack.c` builds the 75-input vector from live state and scales
   it with the constants in `core/feature_spec.h`.
2. `core/fault_predictor.c` runs the network: 18,112 multiply-adds per team,
   about 181 thousand per tick for all 10 teams, then a sigmoid per class.
3. `evaluate_fault_predictor()` in `core/recovery_manager.c` keeps only the
   classes whose score passes that class's threshold (§7) and takes the highest
   passing score as the model's confidence. If no class passes, confidence is
   0 and the team is just monitored.
4. The confidence becomes a recovery recommendation:
   - on its own, it can trigger only **local recovery**;
   - **task reallocation** (confidence ≥ 0.50) and **return to safe zone /
     abort** (≥ 0.80) also require corroboration from at least one of: the
     watchdog's anomaly flag; the model's confidence staying at or above 0.20
     for 8 consecutive ticks; peer consensus at its ELEVATED tier.

This is the simplex pattern used in safety-critical systems: a well-understood
rule-based controller stays in charge, and the learned component can only add
to it.

**Map gate.** On the board the model is skipped until the route and district
maps have fully arrived over the mission link, because two of its inputs are
meaningless before then. In the laptop simulator the world is stored locally,
so there is nothing to wait for.

**Footprint.** About 73 KB of weights as float32, in the board image. Inference
time on the STM32N6570-DK has not been measured.

## 9. Closed-loop evaluation

Offline scores do not show what happens when the AI's recommendations change
what teams do. These results come from the full simulator with the AI acting,
held-out seeds 1001–1020, 3000 ticks, paired per seed with 95% bootstrap
confidence intervals. The full test-by-test record, commands and raw numbers
are in [`docs/evaluation/AI_EVALUATION_BRIEF.md`](../docs/evaluation/AI_EVALUATION_BRIEF.md).

**Mission outcome** (incidents resolved per run, random fault profile):

| Rules only | Rules + consensus | Rules + consensus + AI | AI vs rules |
|---|---|---|---|
| 35.9 | 36.65 | 38.95 | **+3.05 [1.30, 4.80]** |

With no faults injected, the AI build resolves 47.5 vs 47.85 (difference
within noise), so it does not harm fault-free runs.

**No closed-loop drift.** The model was trained on AI-off trajectories. Scored
inside the AI-on runs it degrades by less than measurement noise: macro F1
0.708 open loop vs 0.706 closed loop (5 classes), 0.799 vs 0.798 (4 classes
excluding environment).

**Why the gain is +3 and not more: not the model.** Faults cost about 12
incidents per run (47.85 with no faults vs 35.9). A diagnostic that replaced
the model's scores with perfect ground truth reached only 39.7, within noise of
the real AI (38.95), so a better model could add at most about 0.75 incidents.
The cause is how faults and recovery work in this simulator: a fault clears on
a fixed timer in `core/fault_injector.c` (its duration plus one repair
interval), and nothing outside the injector can end it early, so no recovery
action shortens a fault, however early it is detected.
Per single-class profile, even a perfect detector recovers:

| Fault class | Incidents lost to faults | Recovered by perfect detection |
|---|---|---|
| mobility | 5.10 | +0.05 (not significant) |
| communication | 3.50 | +0.20 (not significant) |
| resource | 9.70 | +3.20 [+1.80, +4.70] |

So the whole recoverable gain sits in resource faults, and the remaining loss
is the cost of the fault itself for its fixed duration. This shows the current
recovery actions cannot recover mobility and communication losses; whether a
different recovery action could is untested.

## 10. Limitations

- **Environment and mission are switched off, for a recovery-layer reason.**
  Acting on them lowers the mission outcome even with a perfect detector
  (−8.8 incidents per run), because those faults are in the map or the
  incident while every recovery action acts on the team (§7). Using them needs
  new recovery actions that act on the map or the incident, not a better
  model.
- **The gain is capped by the simulator's fault model, not the detector.**
  Faults clear on a fixed timer that no recovery action can shorten; a perfect
  detector gains only about 0.75 incidents over the model (§9).
- **Environment is the weakest class** (test F1 0.37, precision about 0.26 at
  best F1). An environment fault only raises the travel cost of one cell,
  leaving almost no trace in a team's own telemetry; what the model detects
  comes largely from the travel-cost and zone map inputs, which on the board
  arrive over the mission link.
- **Mission recall does not exceed the watchdog's best achievable recall**;
  the model's advantage there is precision.
- **Mobility timing:** mean lead time against the watchdog is negative.
- **The baseline comparison is not like-for-like** (§6.1).
- **`make evaluate`'s detection table undercounts the AI.** The AI's recovery
  reason (`PREDICTED_FAULT`) is not attributed to any fault class by
  `laptop/tools/eval_metrics.py`, so its per-class detection rates exclude the
  AI. The mission outcome (incidents resolved) is the reliable measure.
- **Fault rate is high.** Faults are active on about 58% of team-ticks, and
  most fault ticks occur while teams rest, when rule evaluators are blind. This
  inflates the AI's apparent detection advantage in per-tick numbers.
- **Simulator data only.** Training, tuning and testing all use the same
  simulator. Nothing has been validated on real hardware or real incidents.
  Inference time on the board is unmeasured.
- **Platform differences.** Absolute numbers differ slightly between Linux and
  macOS for the same code (floating-point differences change trajectories);
  compare within one platform.

## 11. Reproducing

See [`README.md`](README.md) in this folder for the commands.

## 12. Third-party software (ML pipeline)

Used only to train and evaluate the model on a development machine. None runs
on the board, where the forward pass is the project's own C code.

| Software | Rights holder | Source | Used for | License |
|---|---|---|---|---|
| PyTorch | PyTorch project | `pip install torch` | Model definition, training, checkpoint | BSD-3-Clause |
| NumPy | NumPy developers | `pip install numpy` | Array storage and math | BSD-3-Clause |
| pandas | pandas developers | `pip install pandas` | Reading the dataset | BSD-3-Clause |
| scikit-learn | scikit-learn developers | `pip install scikit-learn` | Run-based splitting, metrics, scaler base | BSD-3-Clause |

The model architecture, feature contract, training script, thresholds, export
tool and C forward pass are original to this project.

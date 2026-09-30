# AI fault predictor: evaluation brief (verification edition)

Purpose: let a second person check, step by step, what was run, what came out, and why each conclusion follows (or does not). Every claim below lists the exact test, the command, the raw result, and its limits. Claims are labelled **PROVEN** (directly measured), **SUPPORTED** (measured, but with an inference step) or **INFERENCE** (not tested).

## 0. Setup and provenance

- Measured on the `ai-integration` branch (thresholds commit `3bea8f6`, analysis tools `443828d`), since merged into `submission/single-board-dk` as `c578f5d` with the same thresholds and model. Paths below are as of that merge; the ML scripts formerly at the repository root (`data_prep.py`, `feature_spec.*`, `evidence_channels.py`) now live in `ml/`, and the checkpoint is `ml/fault_predictor_checkpoint.pt`.
- Simulator: `./tron` (AI off, default build) and `./tron_ai` (`make tron-ai`, `-DFAULT_MODEL_LOADED=1`). Both use `-DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1`.
- Model: 75 -> 128 -> 64 -> 5 float32 MLP, sigmoid outputs; checkpoint epoch 8, val_loss 0.435948 (`core/fault_predictor_weights.h` is its BatchNorm-folded export).
- Seeds: **held-out 1001-1020** for every reported number, except threshold selection which used **dev seeds 2001-2008** (training seeds are 1-300). 3000 ticks per run, fault profile `random` unless stated (`none` = no faults).
- Ground truth = the simulator's `fault_active_<class>` CSV columns (per team per tick).
- Environment: results were produced in a Linux container; the owner's Mac runs gave slightly different absolute numbers for the same code (float/libm differences change trajectories: e.g. AI-off "combined" actions 646 on Mac vs 640 on Linux). Compare within one platform. Tools: Python 3, torch 2.14, scikit-learn, pandas.
- Terms: **tick-level** = one row per (team, tick). **Episode** = a contiguous stretch where one team has one fault class active. **Flag** = a class's AI score >= its threshold. **Resting** = `status == 3`.

## 1. What a threshold is (needed to read everything else)

The AI outputs 5 scores in [0,1] per team per tick (mobility, communication, resource, environment, mission). A threshold turns one score into yes/no: flagged = `score >= threshold`. Higher threshold: fewer false alarms, more misses.

The watchdog does the same with one number: it sums seven evidence accumulators into `anomaly_confidence`, and `watchdog_anomaly_detected = anomaly_confidence >= 0.10` (`WATCHDOG_ANOMALY_THRESHOLD`, `core/watchdog.c:684`). Its trade-off, measured against "any fault active" on 600,000 team-ticks (AI-off run, seeds 1001-1020):

| T | flagged share | precision | recall | F1 |
|---|---|---|---|---|
| 0.05 | 36.0% | 0.889 | 0.551 | 0.681 |
| **0.10 (in use)** | 27.1% | 0.953 | 0.445 | 0.607 |
| 0.18 | 22.8% | 0.982 | 0.385 | 0.554 |
| 0.30 | 0.5% | 0.998 | 0.008 | 0.016 |

Thresholds are **live settings, not analysis-only**: `FAULT_PREDICTOR_CLASS_THRESHOLDS` in `core/fault_predictor.h` decides which AI scores count in `evaluate_fault_predictor()` (`core/recovery_manager.c`). In use: mobility 0.573, communication 0.965, resource 0.705, environment OFF (1.01), mission OFF (1.01). Origin: the first three are the validation-set best-F1 points from the ML team's `ml/evaluate.py` sweep (mobility 0.573, communication 0.965, resource 0.705); environment and mission were switched off because of claim C4.

A second layer sits above the flag: an AI flag only produces an action stronger than LOCAL_RECOVERY when corroborated by the watchdog flag, a streak of 8 consecutive high scores, or consensus ELEVATED (`corroborated` in `evaluate_fault_predictor()`).

## 2. Conclusions at a glance

| # | Conclusion | Status |
|---|---|---|
| C1 | Default build and dataset unchanged (AI opt-in) | PROVEN |
| C2 | C forward pass equals PyTorch (worst 3.9e-07) and equals the supplied checkpoint | PROVEN |
| C3 | With all 5 classes acting on recall-leaning thresholds the AI badly hurts the mission | PROVEN |
| C4 | Environment and mission acting is what does the harm | PROVEN (ablation) |
| C5 | With mobility/communication/resource only, the AI gives +3.05 incidents (real) | PROVEN |
| C6 | The model does not degrade when it is the one acting (no closed-loop drift) | PROVEN |
| C7 | `make evaluate`'s detection table undercounts the AI | PROVEN (mechanism SUPPORTED) |
| C8 | Rule evaluators are blind during rest; most fault ticks occur during rest | PROVEN |
| C9 | Faults cost ~12 incidents; the AI recovers ~3 | PROVEN |
| C10 | A perfect detector does about as well as the real AI; detection is not the limit | PROVEN for the tested logic (single diagnostic) |
| C11 | The remaining loss happens after detection, because recovery actions cannot recover it | INFERENCE |
| C12 | The evaluation has biases (list in section 4) | PROVEN / SUPPORTED per item |
| C13 | Environment and mission must stay off even with a perfect detector: the cause is the recovery layer, not the model | PROVEN |

## 3. Claims and proofs

### C1. Default build unchanged
- Test: build `./tron` from the base branch (`origin/submission/single-board-dk`, `113bc79`) and from `ai-integration`; run `./tron 1 3 1500 --fault-profile=combined` in an empty directory each; compare `md5sum tron_snapshot_dataset.csv`.
- Result: both `b248bea72ce2ceb81389762dccc0a749` (also re-checked after each later change to the thresholds).
- Also (owner's Mac): `make check-live-override-noop-regression` ("byte-identical") and `make check-consensus-shadow-regression` (3 profiles) passed on the merged branch.
- Limit: one config compared by md5; the two regression targets cover more.

### C2. C model equals PyTorch and the checkpoint
- Test: `python ml/export_parity_fixture.py --checkpoint <ckpt> --real-rows <features.csv>` wrote `laptop/tools/model_parity_fixture.csv` (19 vectors: 12 real simulator feature vectors from seeds 1001-1005, 7 edge cases such as all-zeros, +/-5, +/-50, data min/max) with the PyTorch (eval mode, float32) sigmoid outputs. `make check-model-parity` feeds each through `fault_predictor_run()` (built with `-DFAULT_MODEL_LOADED=1`) and requires every output within 1e-5; a second build (default) must return exact zeros.
- Result: worst |C - PyTorch| = 3.58e-07 (Linux), 3.87e-07 (owner's Mac); default build all zeros.
- What it proves: the C weights header is the supplied checkpoint and the C math is right. Limit: 19 vectors, not the full data set.

### C3. All 5 classes acting: harmful
- Test: `make evaluate` (20 seeds x 3 variants) with thresholds `{0.532, 0.965, 0.705, 0.706, 0.731}` (recall-leaning set from the ML team's table) and, earlier, with a shared 0.24 floor on the highest score.
- Result (incidents resolved, mean per run): no AI 36.6 | AI, shared 0.24 floor 5.3 | AI, per-class recall-leaning thresholds 23.2. Clean run (no faults, `ai_fp_report.py`): false-positive actions 155 (AI off) vs 360-639 (AI on).
- Also: with the shared floor the AI was in LOCAL_RECOVERY on 87,728 of 90,000 clean team-ticks (97%).

### C4. Environment and mission cause the harm (ablation)
- Test: `laptop/tools/ai_threshold_sweep.py` builds one AI binary per threshold set (`-DFAULT_PREDICTOR_CLASS_THRESHOLDS={...}`, 1.01 = class off) and runs it on **dev seeds 2001-2008** (random) plus 4 no-fault seeds. No-AI reference on the same seeds: 37.0 incidents.
- Result (incidents resolved): mobility only 35.6 | communication only 36.6 | resource only 38.1 | **environment only 32.9** | **mission only 25.2** | mobility+communication+resource 38.8. Environment+mission at 0.95 on top of the three: 29.8.
- Conclusion: acting on environment or mission lowers incidents even at 0.95. Reason: PROVEN in C13 to be the recovery layer, not the model (a perfect detector acting on these classes also lowers incidents). Noise: dev-seed differences of about 1 incident are within noise (three-class variants 37.8-39.6).

### C5. Tuned thresholds: modest, real gain
- Test: `python3 laptop/tools/evaluate.py --variants rules,rules+consensus,rules+consensus+ai --baseline rules --seeds-start 1001 --seeds-count 20 --ticks 3000 --profiles random,none` (paired per-seed differences, 95% bootstrap CI). Thresholds in the code: 0.573 / 0.965 / 0.705 / off / off.
- Result:

| | rules | rules+consensus | +AI | AI vs rules |
|---|---|---|---|---|
| Incidents resolved, random faults | 35.9 | 36.65 | 38.95 | **+3.05 [1.30, 4.80]** real |
| Stage completions, random faults | 46.65 | 46.1 | 47.95 | +1.3 [-0.75, 3.40] noise |
| Incidents resolved, no faults | 47.85 | 47.85 | 47.5 | -0.35 [-1.25, 0.55] noise |
| Stage completions, no faults | 60.2 | 60.2 | 58.65 | -1.55 [-3.55, 0.35] noise |

- Action counts (`ai_fp_report.py`, seeds 1001-1003): false-positive actions AI on/off: 189/155 (no faults), 34/29 (combined); true-positive actions 1,687 vs 570 (combined).
- Limit: threshold choice among the three-class variants is within noise; 0.573 was kept as the validated best-F1 point.

### C6. No closed-loop drift
- Question: the model was trained on AI-off trajectories; does it get worse when it acts and changes those trajectories?
- Test: `laptop/tools/ai_score_dump.c` logs the AI's 5 scores per team-tick from inside the acting AI build (fidelity check: its `tron_snapshot_dataset.csv` md5 equals `./tron_ai`'s for the same seed: `291fd739eae69ea88ef2c9c08c7d77d9`). For the AI-off run, the model is scored offline on the AI-off feature vectors (`feature_pack_dump.c` with the same flags). `ai_closed_loop_analysis.py` compares both against ground truth (600,000 team-ticks each, seeds 1001-1020, one process per seed).
- Result (F1 open loop vs closed loop): mobility 0.660 / 0.649; communication 0.983 / 0.983; resource 0.891 / 0.898; environment 0.345 / 0.340; mission 0.660 / 0.661. Macro F1 **0.708 vs 0.706** (5 classes), **0.799 vs 0.798** (4 excl. environment).
- Conclusion: no measurable drift. Per-class closed-loop precision/recall: mobility 0.648/0.650, communication 0.966/1.000, resource 0.893/0.903, environment 0.240/0.583, mission 0.554/0.821.

### C7. `make evaluate` undercounts the AI
- Test: same closed-loop run, per episode (3,364 / 3,373 / 3,369 episodes): was there any tick where a rule evaluator flagged (what `evaluate` counts), where the AI flagged, or either?
- Result:

| class | rules only | AI only (any tick in episode) | rules OR AI | AI-off run, rules only |
|---|---|---|---|---|
| mobility | 33.0% | 90.9% | 90.9% | 37.5% |
| communication | 32.4% | 100% | 100% | 53.1% |
| resource | 5.5% | 100% | 100% | 13.9% |

- Why `evaluate` misses it (PROVEN by reading `laptop/tools/eval_metrics.py`): the AI's reason `PREDICTED_FAULT` is not attributed to any class in either detection mode.
- Why the rules' own credit also falls in the AI-on run (SUPPORTED, not isolated): in 70.2-94.8% of episodes an AI action (fused reason `PREDICTED_FAULT`) occurs at or before the first rule flag.
- Limit: the "AI only" column counts a single tick, so it is a contrast, not a fair detection rate. The fair per-tick numbers are in C6.

### C8. Rule evaluators are blind during rest
- Test (AI-off run, seeds 1001-1020): among fault ticks, flag rate when the team is resting vs active (`ai_bias_checks.py`).
- Result: teams rest 59.5% of all team-ticks. Share of fault ticks that occur while resting: mobility 67%, communication 72%, resource 68%. Rule evaluator flag rate on fault ticks: resting **0.0%** for all three; active 15.8% / 32.0% / 3.2%. AI flag rate: resting 59.2% / 100% / 95.7%; active 93.2% / 99.9% / 83.2%.
- Tick-level rule-evaluator P/R/F1 vs the AI (same ticks): mobility 0.962/0.052/0.099 vs 0.622/0.704/0.660; communication 0.959/0.088/0.161 vs 0.967/1.000/0.983; resource 0.308/0.010/0.020 vs 0.867/0.917/0.891.
- Consequence: 86.0% of the AI-on run's fused actions >= RETURN_TO_SAFE_ZONE occur on teams already resting; 40.2% of all team-ticks carry such a recommendation (0.5% with the AI off).
- Limit: I did not measure whether a fault during rest costs any incidents. My reading is that it costs little (a resting team is not working), but that is INFERENCE.

### C9. Faults cost ~12 incidents; the AI recovers ~3
- Test/result: C5's `none` vs `random` profile with the same rules-only build: 47.85 vs 35.9. AI recovers +3.05 of the 11.95 gap (about a quarter).

### C10. A perfect detector does about as well as the real AI
- Test (diagnostic, scratch copy, not committed): in `evaluate_fault_predictor()`, right after `scores = fault_predictor_run(feature_vector);`, overwrite every class score with the simulator's ground truth: `scores.score[k] = team->fault_state.active[k] ? 1.0f : 0.0f;` Same thresholds as the code (so only mobility/communication/resource act). Second variant also replaces `corroborated = watchdog->anomaly_detected || sustained;` with `corroborated = 1u;`. Run with `ai_threshold_sweep.py` on seeds 1001-1020 (+4 no-fault seeds), model on.
- Note: my patch was placed outside the `#if`, so the sweep's "no-AI reference" row was also an oracle run (identical to the first oracle row: that is why both rows show the same numbers). Ignore that row.
- Result (incidents resolved, random faults): oracle with corroboration rule **39.7**; oracle with corroboration removed **39.1**; real AI 38.95; no AI 35.9; no faults 47.85.
- Conclusion: even perfect detection stays near 39-40, far below 47.85, so detection accuracy is not what limits the gain in this simulator with the current recovery logic. Limits: single experiment; the oracle only flags the three enabled classes; it also flags resting teams (no-ops); difference oracle vs real AI (39.7 vs 38.95) is within noise.

### C11. Where the rest is lost (MEASURED, investigation `ai_headroom_investigation.sh`)
- Code fact (verify with `grep -rn "recovery_tick\|pending_repair" core`): a fault clears on a timer in `fault_injector_step()` (duration 5-16 ticks plus `FAULT_REPAIR_CONFIRM_DELAY_TICKS` = one PAR interval) and nothing outside `fault_injector.c` writes those fields. No recovery action can shorten a fault.
- Test: seeds 1001-1020, 3000 ticks, one process per seed; conditions: no faults (`none`), faults with the default AI-off build, faults with the oracle detector (scores overwritten with ground truth, thresholds as shipped so only mobility/communication/resource act, corroboration rule kept). Profiles `random` plus single-class `mobility`, `communication`, `resource`. Script: `bash laptop/tools/ai_headroom_investigation.sh` (builds the oracle in `build/headroom/`, repo sources untouched); tables from `laptop/tools/ai_headroom_analysis.py`. Note: the AI-off build here is `./tron` (rules + consensus): 36.65 incidents, whereas C5's 35.9 is the rules-only row.
- Result, exposure (random): faults active on 58.1% of team-ticks with AI off, 59.1% with the oracle; 324 vs 337 episodes of ~53 ticks. Exposure is the same with and without detection.
- Result, incidents (paired seeds, bootstrap 95% CI on oracle minus no-AI):

| profile | no faults | no AI | oracle | lost | oracle gain | recovered |
|---|---|---|---|---|---|---|
| mobility | 47.85 | 42.75 | 42.80 | 5.10 | +0.05 [-1.45, +1.55] | 1% |
| communication | 47.85 | 44.35 | 44.55 | 3.50 | +0.20 [-0.95, +1.35] | 6% |
| resource | 47.85 | 38.15 | 41.35 | 9.70 | +3.20 [+1.80, +4.70] | 33% |
| random (all 5 classes) | 47.85 | 36.65 | 39.70 | 11.20 | +3.05 [+1.80, +4.45] | 27% |

- Conclusion: the whole recoverable gain sits in resource faults; for mobility and communication even a perfect detector gains nothing measurable, and the remaining loss is the cost of the fault itself for its fixed duration. Environment and mission faults are not isolated by any profile, so their share of the random-profile loss is not separately measured.
- Limits: the oracle also flags resting teams (no-ops); it is the same recovery logic, so this shows the *current recovery actions* cannot recover mobility/communication loss, not that no possible action could. I did not test alternative recovery actions, so whether a better response exists is open. Working ticks in the random runs: 15,178 (no faults) vs 12,061 (no AI) vs 11,889 (oracle) per run; resting ticks rise correspondingly.

### C13. Environment and mission: off because of the recovery layer, not the model (PROVEN)
- Code fact (`core/fault_injector.c`): an environment fault raises the travel cost of one road cell on the team's next path step and marks it `road_blocked`; a mission fault makes the team's *incident* worse (victim count up, priority raised one level toward critical). Neither changes the team itself. Every recovery action in `core/recovery_manager.c` targets the team (local recovery, task reallocation, return to safe zone, abort).
- Test: same oracle patch as C10/C11 (scores overwritten with ground truth, corroboration rule kept), built with `-DFAULT_PREDICTOR_CLASS_THRESHOLDS` so that only environment and/or mission act (0.5 on a 0/1 oracle score = act on every true fault). Seeds 1001-1020, 3000 ticks, profile `random`, paired against `./tron` (rules + consensus). Script: `bash laptop/tools/ai_env_mission_oracle.sh` (builds in `build/env-mission-oracle/`, repo sources untouched).
- Result (incidents resolved, mean; paired difference vs no AI, bootstrap 95% CI):

| condition | incidents | vs no AI |
|---|---|---|
| no AI (`./tron`) | 36.65 | — |
| real AI, shipped thresholds (control) | 38.95 | +2.30 [+0.65, +4.05] |
| oracle, shipped thresholds (control) | 39.70 | +3.05 [+1.75, +4.45] |
| oracle, environment only | 34.40 | -2.25 [-3.50, -1.00] |
| oracle, mission only | 29.30 | -7.35 [-9.20, -5.50] |
| oracle, environment + mission | 27.85 | -8.80 [-10.40, -7.25] |
| real AI, environment + mission only (0.706 / 0.731) | 22.15 | -14.50 [-18.80, -11.20] |

- The three control rows reproduce C5/C11 exactly (36.65, 38.95, 39.70), so the setup matches.
- Conclusion: a detector that is never wrong still lowers incidents when environment or mission act, so these classes must stay off with any model; the cause is that the recovery actions target the team while these faults sit in the map or the incident. The real model's larger loss (-14.50 vs -8.80) adds its false alarms on top, but the decision does not depend on them. Enabling these classes requires recovery actions that act on the map or the incident (e.g. rerouting around a blocked cell, reinforcing a worsening incident).
- Limits: `random` profile only (no single-class environment/mission profile exists); one set of 20 seeds.

## 4. Are the tests biased?

| Bias | Direction | Evidence |
|---|---|---|
| Detection credit ignores AI reason; AI acting silences rule evaluators | against the AI | C7 |
| Ground truth counts faults during rest, when rules are blind and actions are no-ops | inflates AI advantage in detection numbers | C8 |
| "Any tick in episode" credit | favours frequent flaggers (used only as a contrast in C7) | C7 |
| 58.1% of team-ticks have a fault active (measured on the AI-off run) | inflates value of detection, undercounts false-alarm cost | direct count |
| Same synthetic simulator for training, tuning, testing; watchdog 0.10 also tuned on it | favours both | stated in code comments; no real-world data exists |
| My threshold search | no leakage: dev seeds 2001-2008, verified on 1001-1020 | C4, C5 |
| Mission outcome (incidents resolved) needs no labels; paired per-seed CI | least biased metric; used for the headline | C5 |

## 5. Not proven / open

1. Whether a different recovery action could recover mobility/communication losses (C11 shows the current ones do not). For environment/mission, C13 shows the current actions make things worse even with perfect detection.
2. Whether faults during rest cost anything.
3. Real-world performance (all data is synthetic; fault rate unrealistic).
4. Environment and mission are OFF; using them needs recovery actions that act on the map or the incident (C13). Whether retraining would then also help is untested.
5. `make evaluate` still shows the AI's detection as worse than no-AI (communication 32.4% vs 53.2%); it needs per-class AI scores in its CSV to fix (AI build only), not yet implemented.
6. (Resolved.) `make check-dashboard-e2e` has since been run on the merged code on the owner's Mac and passed in full, including replay take-over.

## 6. How to reproduce every result

```
make clean && make && make tron-ai                      # default + AI builds
# C1
./tron 1 3 1500 --fault-profile=combined                # in an empty dir; md5sum tron_snapshot_dataset.csv
# C2
python ml/export_parity_fixture.py --checkpoint fault_predictor_checkpoint.pt --real-rows <features_c.csv>
make check-model-parity
# C3-C5, C9  (thresholds are in core/fault_predictor.h)
make evaluate EVAL_PROFILES=random,none
python3 laptop/tools/ai_fp_report.py                    # false-positive / true-positive actions
python3 laptop/tools/ai_threshold_sweep.py --configs cfg.json --seeds-start 2001 --seeds-count 8     # dev; then 1001/20 to verify
#   cfg.json: {"name": [mob,comm,res,env,mis], ...}   (1.01 = class off)
# C6-C8: closed-loop scores and analysis, ONE PROCESS PER SEED
cc -std=gnu11 -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1 -DFAULT_MODEL_LOADED=1 -Icore -Ilaptop \
   -o ai_score_dump laptop/tools/ai_score_dump.c core/*.c laptop/debug.c laptop/snapshot.c -lm
cc ... (same without -DFAULT_MODEL_LOADED=1) -o feature_dump_ol laptop/tools/feature_pack_dump.c core/*.c laptop/debug.c laptop/snapshot.c -lm
#   for seed in 1001..1020: (cd closed/s$seed && ai_score_dump 1 3000 $seed scores.csv) ; (cd open/s$seed && feature_dump_ol 1 3000 $seed features.csv)
python3 laptop/tools/ai_closed_loop_analysis.py closed open --checkpoint <ckpt>   # C6, C7
python3 laptop/tools/ai_bias_checks.py closed open --checkpoint <ckpt>            # C8, watchdog table, AUCs
# C10: scratch copy of core/ + laptop/, apply the two-line oracle patch described above, then run ai_threshold_sweep.py there
```

Other numbers quoted above from `ai_bias_checks.py`: ROC AUC vs ground truth, AI score vs watchdog `anomaly_confidence`: mobility 0.895 / 0.661, communication 0.997 / 0.984, resource 0.990 / 0.646, environment 0.837 / 0.497, mission 0.950 / 0.603; any fault 0.848 (max of the three AI scores) / 0.764.

A single process running many seeds did not finish reliably in the analysis environment (only 5-6 of 20 seeds completed, silently); use one process per seed and check row counts (600,000 tick rows per run set).

## 7. File index
`core/fault_predictor.h` (thresholds) | `core/recovery_manager.c` (`evaluate_fault_predictor`) | `laptop/tools/ai_fp_report.py` | `ai_threshold_sweep.py` | `ai_score_dump.c` | `ai_closed_loop_analysis.py` | `ai_bias_checks.py` | `ml/export_parity_fixture.py` | `laptop/tools/model_parity_fixture.csv` | `docs/ml/FAULT_PREDICTOR_THRESHOLDS.md`.

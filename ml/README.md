# ml/ — AI fault predictor: data, training, evaluation and export

This folder holds everything that produced the fault predictor running in
`core/fault_predictor.c`: a small neural network that reads one team's state
each tick and scores how likely it is that a fault is active in each of five
classes (mobility, communication, resource, environment, mission).

For the results, the design reasoning and the limitations, read
[`MODEL_REPORT.md`](MODEL_REPORT.md). This file covers what is here and how to
run it.

## What is where

| Path | Purpose |
|---|---|
| `data_prep.py` | Builds the training arrays from the simulator CSV: run-based 70/15/15 split, scaling, leak guards. Writes `data_prep_output/` and the feature contract `ml/feature_spec.json`. |
| `feature_spec.json` | The feature contract: the 75 inputs in order, plus the scaler. Single source of truth. |
| `generate_feature_spec.py` | Generates `core/feature_spec.h` (C) and `ml/feature_spec.py` (Python) from `feature_spec.json`. Never hand-edit either output. |
| `feature_spec.py` | Generated Python constants (input order, scaler). |
| `evidence_channels.py` | Python reconstruction of the watchdog's evidence channels, used by analysis scripts and the baseline cross-check. |
| `model.py` | Network definition: 75 → 128 → 64 → 5, BatchNorm + ReLU + Dropout(0.2), raw logits out. `python ml/model.py` runs a smoke test. |
| `train.py` | Trains on `data_prep_output/`; early stopping on validation loss; saves `fault_predictor_checkpoint.pt`. |
| `evaluate.py` | Loads the checkpoint; per-class thresholds on validation; precision/recall/F1/AUC on the held-out test set against the watchdog baseline; lead-time analysis. |
| `naive_baseline.py` | Baseline: how well the watchdog's single confidence score detects each class. |
| `export.py` | Folds BatchNorm into the linear layers and writes `fault_predictor_weights.h`. Refuses to write if the checkpoint's feature order or scaler disagrees with `feature_spec.json`. |
| `export_parity_fixture.py` | Writes the PyTorch reference outputs used by `make check-model-parity`. |
| `fault_predictor_checkpoint.pt` | The trained model the shipped weights were exported from. |
| `MODEL_REPORT.md` | Model report: data, features, results, thresholds, closed-loop evaluation, limitations. |

Shipped files this pipeline feeds, outside this folder:

| Path | What it is |
|---|---|
| `core/feature_spec.h` | Generated C feature contract (input order, scaler constants). |
| `core/feature_pack.c` | Builds and scales the 75-input vector from live state each tick. |
| `core/fault_predictor_weights.h` | The exported weights: 18,309 float32 weights and biases (about 73 KB). |
| `core/fault_predictor.c` / `.h` | Forward pass in plain C, and the per-class decision thresholds. |
| `laptop/tools/model_parity_fixture.csv` | 19 input vectors with their PyTorch outputs, for the C-vs-PyTorch parity test. |

**The trained checkpoint is `ml/fault_predictor_checkpoint.pt`** (epoch 8,
validation loss 0.435948, 85 KB). The committed `core/fault_predictor_weights.h`
is its export: on the 19 parity-fixture vectors the checkpoint reproduces the
fixture's reference outputs within 2.1e-7, and the C forward pass reproduces
them within 3.6e-7 (`make check-model-parity`). `train.py` writes a new
checkpoint to the repository root, and `export.py` reads it from there; to
re-export the shipped model, copy it there first:
`cp ml/fault_predictor_checkpoint.pt .`

## Install

From the repository root:

```
pip install -r requirements.txt
```

This includes PyTorch, NumPy, pandas and scikit-learn. Training runs on CPU; a
GPU is used automatically if PyTorch can see one.

## Run everything from the repository root

`train.py`, `evaluate.py`, `naive_baseline.py` and `export.py` read and write
`data_prep_output/`, the checkpoint and the weights header relative to the
directory you run them from. Run them from the repository root, not from
inside `ml/`:

```
python ml/train.py
```

## Reproducing the model, step by step

1. **Build the simulator and generate data** with the random fault profile,
   using the training seeds (1–300; seeds 1001+ and 2001+ are kept for
   evaluation and threshold tuning):
   ```
   make
   ./tron 1 300 3000 --fault-profile=random
   ```
   This writes `tron_snapshot_dataset.csv`. The default build has the AI off
   (`FAULT_MODEL_LOADED=0`), which is what training data must come from.
2. **Prepare the data.**
   ```
   python ml/data_prep.py tron_snapshot_dataset.csv
   ```
   Splits by whole run (70/15/15), scales the features, stops if any feature
   leaks a label, and writes `data_prep_output/` and `ml/feature_spec.json`.
3. **Regenerate the feature contract files.**
   ```
   python ml/generate_feature_spec.py
   ```
4. **Baseline.** `python ml/naive_baseline.py` prints the watchdog-only numbers.
   `evaluate.py` compares against its `NAIVE_BASELINE` table; update that table
   if you change the dataset.
5. **Train.** `python ml/train.py` (writes `fault_predictor_checkpoint.pt`).
6. **Evaluate offline.** `python ml/evaluate.py`.
7. **Export.**
   ```
   python ml/export.py
   cp fault_predictor_weights.h core/
   ```
8. **Verify on the C side.**
   ```
   make check-feature-spec-fresh     # generated files match feature_spec.json
   make check-feature-pack-parity    # C feature vector == Python feature vector
   python ml/export_parity_fixture.py --checkpoint fault_predictor_checkpoint.pt
   make check-model-parity           # C forward pass == PyTorch (within 1e-5)
   ```
9. **Re-tune the decision thresholds in closed loop.** Offline thresholds are
   not enough (see `MODEL_REPORT.md` §7): re-run
   `laptop/tools/ai_threshold_sweep.py` on dev seeds 2001–2008, verify on
   held-out seeds 1001–1020, and update `FAULT_PREDICTOR_CLASS_THRESHOLDS` in
   `core/fault_predictor.h`.
10. **Measure the effect on the mission.**
    ```
    make tron-ai
    make evaluate
    ```

Re-running training gives different weights, so the numbers in
`MODEL_REPORT.md` describe the committed weights, not a fresh run.

## Building with the model on

The model is compiled in only when `FAULT_MODEL_LOADED=1`:

| Build | Model |
|---|---|
| `make` → `./tron` | off (default; dataset generation and regression tests) |
| `make tron-ai` → `./tron_ai` | on |
| STM32CubeIDE `DevRam` configuration (board) | on |

With the model off, `fault_predictor_run()` returns all-zero scores and the
simulator's output is byte-identical to a build without the AI. To run the
dashboard with the model on:

```
make tron-ai
TRON_BINARY=./tron_ai SIM_ONLY=1 python laptop/tools/app.py
```

## Rules this pipeline follows

- **Labels come only from the injected faults** (`fault_active_*` columns),
  never from watchdog or consensus output.
- **No label leakage.** `data_prep.py` stops if any feature is a ground-truth,
  `gt_*` or consensus column, or is a near-copy of a label in the data.
- **Held-out evaluation.** Splits are by whole run, the scaler is fitted on the
  training split only, offline thresholds are chosen on validation, and
  closed-loop thresholds are chosen on dev seeds and verified on separate
  held-out seeds.
- **Fixed feature order.** Changing a feature means regenerating the C files,
  retraining and re-exporting.

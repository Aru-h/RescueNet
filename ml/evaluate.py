"""
evaluate.py -- loads the trained checkpoint (train.py's output) and reports:
  1. Per-class precision / recall / F1 / AUC on the held-out TEST set
     (never touched during training or threshold selection).
  2. Per-class decision thresholds, chosen by sweeping on the VAL set
     (not test -- avoids tuning on the same data you report results on).
  3. A direct comparison against naive_baseline.py's real numbers (the bar
     the watchdog's own single confidence scalar could achieve).
  4. The lead-time metric -- does the model flag a real fault episode
     before the watchdog does -- using three explicit edge-case rules:
       a) episodes truncated at the start/end of a run are excluded
          (true onset/resolution tick is unknown outside the window)
       b) episodes the watchdog never flags at all are counted in their
          own "watchdog missed entirely" bucket, not dropped or scored
          as a 0-tick gap
       c) a model flag BEFORE the episode's true start tick never counts
          as a detection of that episode (avoids crediting noise as an
          early catch)

Requires data_prep.py to have been (re)run with the test-metadata block
added (test_run_id.npy / test_team_id.npy / test_cycle.npy /
test_watchdog_anomaly_detected.npy) -- these are separate from
feature_columns/X and are never seen by the model itself.
"""

import pickle
import numpy as np
import pandas as pd
import torch
from sklearn.metrics import precision_recall_curve, precision_score, recall_score, f1_score, roc_auc_score

from model import FaultPredictorNet, FAULT_CLASS_NAMES

DATA_DIR = "data_prep_output"
CHECKPOINT_PATH = "fault_predictor_checkpoint.pt"
BATCH_SIZE = 4096  # inference-only batching, no gradients -- can be larger than training's

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

# Numbers from naive_baseline.py's run on the training dataset -- the bar the
# trained model needs to clear.
# AUC / best_MCC are threshold-independent / best-achievable, per naive_baseline.py's
# own reasoning -- read your model's numbers against these, not against recall@0.10.
NAIVE_BASELINE = {
    "mobility":      {"auc": 0.515, "best_mcc": 0.076, "mcc_precision": 0.360, "mcc_recall": 0.118},
    "communication": {"auc": 0.974, "best_mcc": 0.897, "mcc_precision": 0.951, "mcc_recall": 0.894},
    "resource":      {"auc": 0.544, "best_mcc": 0.092, "mcc_precision": 0.358, "mcc_recall": 0.164},
    "environment":   {"auc": 0.498, "best_mcc": 0.002, "mcc_precision": 0.081, "mcc_recall": 0.514},
    "mission":       {"auc": 0.602, "best_mcc": 0.191, "mcc_precision": 0.162, "mcc_recall": 0.922},
}


# ── Load checkpoint + rebuild model ─────────────────────────────────
def load_model():
    # weights_only=False is required here: the checkpoint holds a
    # FaultPredictorConfig dataclass and plain Python lists (class_names,
    # feature_columns) alongside the tensors, not just tensors -- newer
    # PyTorch defaults weights_only=True, which would refuse to unpickle
    # those. Safe here since this is a checkpoint train.py just wrote on
    # your own machine, not an untrusted download.
    checkpoint = torch.load(CHECKPOINT_PATH, map_location=DEVICE, weights_only=False)

    config = checkpoint["config"]
    model = FaultPredictorNet(config).to(DEVICE)
    model.load_state_dict(checkpoint["model_state_dict"])
    model.eval()

    print(f"Loaded checkpoint from epoch {checkpoint['epoch']}, "
          f"val_loss={checkpoint['val_loss']:.5f}")

    return model, checkpoint["class_names"], checkpoint["feature_columns"]


# ── Batched inference -- returns sigmoid probabilities as a numpy array ──
@torch.no_grad()
def predict_proba_batched(model, X):
    probs = []
    for start in range(0, len(X), BATCH_SIZE):
        batch = torch.from_numpy(X[start:start + BATCH_SIZE]).to(DEVICE)
        probs.append(torch.sigmoid(model(batch)).cpu().numpy())
    return np.concatenate(probs, axis=0)


# ── Per-class threshold sweep on VAL, picking the best-F1 threshold ────
def sweep_thresholds(y_val, probs_val, class_names):
    thresholds = {}
    print("\n=== Threshold sweep (on VAL, best-F1 per class) ===")
    for i, name in enumerate(class_names):
        precision, recall, thresh = precision_recall_curve(y_val[:, i], probs_val[:, i])
        f1 = np.where(precision + recall > 0, 2 * precision * recall / (precision + recall), 0.0)
        # precision_recall_curve returns len(thresh) == len(precision) - 1;
        # drop the last precision/recall point (threshold = +inf, not real).
        f1 = f1[:-1]
        best_i = int(np.argmax(f1))
        best_t = float(thresh[best_i]) if len(thresh) > 0 else 0.5

        # Degenerate guard, same spirit as naive_baseline.py: if the chosen
        # threshold sits at (or near) the minimum score seen, the "optimum"
        # is close to "flag everything" -- flag it rather than trust it silently.
        degenerate = best_i <= 1
        flag = "  [near-degenerate -- check manually]" if degenerate else ""

        thresholds[name] = best_t
        print(f"  {name:<15} threshold={best_t:.3f}  F1={f1[best_i]:.4f}{flag}")

    return thresholds

# ── Explore the full precision/recall tradeoff for specific classes ────
def explore_thresholds(y_val, probs_val, class_names, target_classes,
                        recall_targets=(0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90)):
    print(f"\n=== Threshold exploration (VAL, at fixed recall targets) ===")
    for name in target_classes:
        i = class_names.index(name)
        precision, recall, thresh = precision_recall_curve(y_val[:, i], probs_val[:, i])
        precision, recall = precision[:-1], recall[:-1]
        base_rate = y_val[:, i].mean()  # true base rate on THIS val split -- what
                                         # "flag everything" precision would be for free

        print(f"\n  -- {name} --  (max achievable recall on VAL: {recall.max():.3f}, "
              f"base_rate={base_rate:.3f})")
        for target in recall_targets:
            if target > recall.max():
                print(f"    target_recall={target:.2f}  -- unreachable, skipping")
                continue
            idx = np.argmin(np.abs(recall - target))
            beats_base = "YES" if precision[idx] > base_rate else "no"
            print(f"    target_recall={target:.2f}  threshold={thresh[idx]:.3f}  "
                  f"precision={precision[idx]:.3f}  recall={recall[idx]:.3f}  "
                  f"above_base_rate={beats_base}")


# ── Per-class test-set metrics at the chosen thresholds ────────────────
def evaluate_test_set(y_test, probs_test, thresholds, class_names):
    print("\n=== TEST SET results (per class) — vs. naive_baseline.py ===")
    print(f"{'class':<15}{'precision':>10}{'recall':>8}{'F1':>7}{'AUC':>7}"
          f"{'base_AUC':>10}{'base_MCC':>10}{'base_P':>8}{'base_R':>8}"
          f"{'beats_P':>9}{'beats_R':>9}")

    results = {}
    for i, name in enumerate(class_names):
        y_true = y_test[:, i]
        y_score = probs_test[:, i]
        y_pred = (y_score >= thresholds[name]).astype(int)

        precision = precision_score(y_true, y_pred, zero_division=0)
        recall = recall_score(y_true, y_pred, zero_division=0)
        f1 = f1_score(y_true, y_pred, zero_division=0)
        auc = roc_auc_score(y_true, y_score) if len(np.unique(y_true)) > 1 else float("nan")

        base = NAIVE_BASELINE[name]
        results[name] = {"precision": precision, "recall": recall, "f1": f1, "auc": auc}

        beats_p = "YES" if precision > base["mcc_precision"] else "no"
        beats_r = "YES" if recall > base["mcc_recall"] else "no"

        print(f"{name:<15}{precision:>10.4f}{recall:>8.4f}{f1:>7.4f}{auc:>7.4f}"
              f"{base['auc']:>10.4f}{base['best_mcc']:>10.4f}"
              f"{base['mcc_precision']:>8.4f}{base['mcc_recall']:>8.4f}"
              f"{beats_p:>9}{beats_r:>9}")

    print("\nbase_P/base_R = watchdog's own precision/recall at ITS best-MCC "
          "operating point (the honest achievable ceiling -- best-F1 degenerates "
          "to 'flag everything' for mobility/resource/environment, so best-MCC "
          "is used here instead, per naive_baseline.py's own guidance). "
          "beats_P/beats_R: does this model's chosen threshold beat that ceiling.")

    print("\nRead 'AUC' against 'base_AUC' — beating it means the richer "
          "raw+evidence feature set is doing real work over the watchdog's "
          "single confidence scalar. base_MCC is the watchdog's best-achievable "
          "per-class ceiling at its ONE global threshold — not directly "
          "comparable to this model's precision/recall (5 independent "
          "thresholds vs. 1 shared one), but useful context.")


    return results


# ── Lead-time metric (the "slightly more detailed" version) ────────────
def find_episodes(active):
    """Run-length-encode a 0/1 array into (start_idx, end_idx) episodes,
    both inclusive. Pure numpy, no Python-level row loop."""
    if len(active) == 0 or not active.any():
        return []
    changes = np.diff(active.astype(int))
    starts = np.where(changes == 1)[0] + 1
    if active[0] == 1:
        starts = np.insert(starts, 0, 0)
    ends = np.where(changes == -1)[0]
    if active[-1] == 1:
        ends = np.append(ends, len(active) - 1)
    return list(zip(starts.tolist(), ends.tolist()))


def lead_time_for_group(group_df, thresholds, class_names):
    """One (run_id, team_id)'s full tick history, already sorted by cycle.
    Returns per-class lists of results for this group only -- caller
    aggregates across all groups."""
    n = len(group_df)
    cycle = group_df["cycle"].to_numpy()
    watchdog_flag = group_df["watchdog_anomaly_detected"].to_numpy()

    out = {name: {"lead_times": [], "watchdog_missed": 0,
                   "model_missed": 0, "excluded_boundary": 0} for name in class_names}

    for name in class_names:
        active = group_df[f"y_{name}"].to_numpy()
        probs = group_df[f"prob_{name}"].to_numpy()
        pred_active = probs >= thresholds[name]

        for start, end in find_episodes(active):
            # Rule (a): exclude episodes touching this group's own boundary --
            # a whole run_id is always kept together in one split (group_split
            # splits on run_id), so index 0 / n-1 here means real tick 0 or the
            # run's real last tick, not an artificial chunk edge.
            if start == 0 or end == n - 1:
                out[name]["excluded_boundary"] += 1
                continue

            # Rule (c): only search from the episode's true start onward --
            # a model flag before `start` structurally cannot be counted here.
            model_hits = np.where(pred_active[start:])[0]
            model_detect_idx = start + model_hits[0] if len(model_hits) > 0 else None

            watchdog_hits = np.where(watchdog_flag[start:] == 1)[0]
            watchdog_detect_idx = start + watchdog_hits[0] if len(watchdog_hits) > 0 else None

            if model_detect_idx is None:
                out[name]["model_missed"] += 1
                continue

            if watchdog_detect_idx is None:
                # Rule (b): don't drop, don't score as 0 -- its own bucket.
                out[name]["watchdog_missed"] += 1
                continue

            lead_ticks = cycle[watchdog_detect_idx] - cycle[model_detect_idx]
            out[name]["lead_times"].append(lead_ticks)

    return out


def run_lead_time_analysis(class_names, thresholds, probs_test):
    print("\n=== Lead-time analysis (TEST set, per fault-class episode) ===")

    run_id = np.load(f"{DATA_DIR}/test_run_id.npy")
    team_id = np.load(f"{DATA_DIR}/test_team_id.npy")
    cycle = np.load(f"{DATA_DIR}/test_cycle.npy")
    watchdog_flag = np.load(f"{DATA_DIR}/test_watchdog_anomaly_detected.npy")
    y_test = np.load(f"{DATA_DIR}/y_test.npy")

    df = pd.DataFrame({"run_id": run_id, "team_id": team_id, "cycle": cycle,
                        "watchdog_anomaly_detected": watchdog_flag})
    for i, name in enumerate(class_names):
        df[f"y_{name}"] = y_test[:, i]
        df[f"prob_{name}"] = probs_test[:, i]

    df = df.sort_values(["run_id", "team_id", "cycle"])

    totals = {name: {"lead_times": [], "watchdog_missed": 0,
                      "model_missed": 0, "excluded_boundary": 0} for name in class_names}

    n_groups = 0
    for (_, _), group_df in df.groupby(["run_id", "team_id"], sort=False):
        n_groups += 1
        group_result = lead_time_for_group(group_df, thresholds, class_names)
        for name in class_names:
            totals[name]["lead_times"].extend(group_result[name]["lead_times"])
            totals[name]["watchdog_missed"] += group_result[name]["watchdog_missed"]
            totals[name]["model_missed"] += group_result[name]["model_missed"]
            totals[name]["excluded_boundary"] += group_result[name]["excluded_boundary"]

    print(f"(scanned {n_groups} (run_id, team_id) sequences in the test set)\n")
    print(f"{'class':<15}{'n_valid':>8}{'mean_lead':>11}{'median_lead':>13}"
          f"{'watchdog_missed':>17}{'model_missed':>14}{'excluded':>10}")
    for name in class_names:
        t = totals[name]
        lt = np.array(t["lead_times"])
        n_valid = len(lt)
        mean_lead = lt.mean() if n_valid > 0 else float("nan")
        median_lead = np.median(lt) if n_valid > 0 else float("nan")
        print(f"{name:<15}{n_valid:>8}{mean_lead:>11.2f}{median_lead:>13.2f}"
              f"{t['watchdog_missed']:>17}{t['model_missed']:>14}{t['excluded_boundary']:>10}")

    print("\nmean_lead/median_lead (ticks): positive = model detected before the "
          "watchdog. watchdog_missed = episodes the model caught that the "
          "watchdog never flagged at all during the whole episode -- a "
          "separate, arguably more important number than the average. "
          "model_missed = episodes the model never flagged after the true "
          "onset (excluding pre-onset noise, per rule c). excluded = "
          "episodes touching a run's real start/end tick, where the true "
          "onset/resolution isn't observable.")

    return totals


# ── Main ─────────────────────────────────────────────────────────────
def main():
    print(f"Device: {DEVICE}")

    model, class_names, feature_columns = load_model()
    assert class_names == FAULT_CLASS_NAMES, (
        f"Class order mismatch: checkpoint has {class_names}, model.py has "
        f"{FAULT_CLASS_NAMES} -- stop and reconcile before trusting any number below."
    )

    with open(f"{DATA_DIR}/feature_columns.pkl", "rb") as f:
        saved_feature_columns = pickle.load(f)
    assert feature_columns == saved_feature_columns, (
        "feature_columns in the checkpoint don't match the current "
        "data_prep_output/feature_columns.pkl -- did data_prep.py get "
        "rerun with a different feature set since this checkpoint was "
        "trained? Stop and reconcile before trusting any number below."
    )

    X_val = np.load(f"{DATA_DIR}/X_val_scaled.npy").astype(np.float32)
    y_val = np.load(f"{DATA_DIR}/y_val.npy")
    X_test = np.load(f"{DATA_DIR}/X_test_scaled.npy").astype(np.float32)
    y_test = np.load(f"{DATA_DIR}/y_test.npy")

    print(f"Val rows: {len(X_val):,}  Test rows: {len(X_test):,}")

    print("\nRunning inference on VAL (for threshold selection)...")
    probs_val = predict_proba_batched(model, X_val)

    print("Running inference on TEST (held out, untouched until now)...")
    probs_test = predict_proba_batched(model, X_test)

    thresholds = sweep_thresholds(y_val, probs_val, class_names)
    explore_thresholds(y_val, probs_val, class_names,
                        target_classes=["mobility", "mission"])
    # environment: the coarse 0.60-0.90 sweep above showed EVERY point below
    # the 0.080 base rate (naive_baseline.py) -- worse than a do-nothing
    # classifier. That range skips right over the default threshold's
    # operating point (recall=0.222, precision=0.102, which IS above base
    # rate) without showing where precision actually crosses 0.080. Finer
    # granularity here finds that real crossover point instead of guessing.
    explore_thresholds(y_val, probs_val, class_names,
                        target_classes=["environment"],
                        recall_targets=(0.25, 0.30, 0.35, 0.40, 0.45, 0.50, 0.55, 0.60))
    evaluate_test_set(y_test, probs_test, thresholds, class_names)
    run_lead_time_analysis(class_names, thresholds, probs_test)

    print("\nDone.")


if __name__ == "__main__":
    main()
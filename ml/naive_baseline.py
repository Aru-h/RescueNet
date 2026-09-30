"""
naive_baseline.py -- naive-threshold baseline.

Sweeps a threshold on the REAL watchdog's own `watchdog_anomaly_confidence`
scalar against ground truth (`fault_active_*`), both for "any fault active"
and per class. This is the bar the trained multi-label model has to clear —
per class especially, since the watchdog only ever produces ONE confidence
number per team per tick, so its best-possible per-class recall/precision
(scanning every threshold) is an upper bound on what a single global anomaly
flag could ever achieve for that class specifically, not something the real
system can actually realize simultaneously across all 5 classes.

Memory approach: chunked -- never holds every row's
score in memory. Instead accumulates a histogram of confidence
values split by label (active / inactive), per target. Cumulative sum from
the top bin down gives TP/FP counts at arbitrary threshold resolution without
sorting the full column. This is the same chunked-and-vectorized pattern
data_prep.py uses, just applied to a histogram instead of a running sum.

Assumes real watchdog_anomaly_confidence is already in [0, 1] — if you want
to cross-check the recomputed value from evidence_channels.py instead
(it should track the logged value very closely), swap the
CONFIDENCE_COL constant to "recomputed_anomaly_confidence" after running that
module's reconstruct_full_timeline() to produce it, and point CSV at that
output instead.
"""

import numpy as np
import pandas as pd

CSV = "tron_snapshot_dataset.csv"
CHUNK = 2_000_000

CONFIDENCE_COL = "watchdog_anomaly_confidence"
FAULT_CLASSES = ["mobility", "communication", "resource", "environment", "mission"]
FAULT_LABEL_COLS = [f"fault_active_{c}" for c in FAULT_CLASSES]

N_BINS = 1000  # threshold resolution — 1000 bins over [0,1] = 0.001 granularity
BIN_EDGES = np.linspace(0.0, 1.0, N_BINS + 1)

# Reference operating points worth reporting explicitly, not just the sweep.
REFERENCE_THRESHOLDS = {"current_anomaly_threshold": 0.10, "current_investigate_threshold": 0.18}


def new_hist_pair():
    # [0] = histogram of confidence for label==0 rows, [1] = label==1 rows
    return {0: np.zeros(N_BINS, dtype=np.int64), 1: np.zeros(N_BINS, dtype=np.int64)}


def accumulate(hist_pair, confidence, label_mask):
    """Add one chunk's worth of (confidence, binary label) into hist_pair."""
    for label_val in (0, 1):
        mask = label_mask if label_val == 1 else ~label_mask
        if mask.any():
            counts, _ = np.histogram(confidence[mask], bins=BIN_EDGES)
            hist_pair[label_val] += counts


def sweep_from_hist(hist_pair):
    """
    hist_pair[1][i] = count of active rows with confidence in bin i
    hist_pair[0][i] = count of inactive rows with confidence in bin i

    Threshold t = BIN_EDGES[i] (>= t counts as "predicted active").
    TP(t) = sum of hist_pair[1][i:]   (active rows scoring >= t)
    FP(t) = sum of hist_pair[0][i:]   (inactive rows scoring >= t)
    Computed via reverse cumulative sum — no per-row work, no sort.
    """
    P = hist_pair[1].sum()   # total actually-active rows
    N = hist_pair[0].sum()   # total actually-inactive rows

    tp = np.cumsum(hist_pair[1][::-1])[::-1]  # TP(t) for t = each bin edge (excl. last)
    fp = np.cumsum(hist_pair[0][::-1])[::-1]

    thresholds = BIN_EDGES[:-1]
    fn = P - tp
    tn = N - fp

    with np.errstate(divide="ignore", invalid="ignore"):
        precision = np.where(tp + fp > 0, tp / (tp + fp), np.nan)
        recall = np.where(P > 0, tp / P, np.nan)
        fpr = np.where(N > 0, fp / N, np.nan)
        f1 = np.where(precision + recall > 0,
                      2 * precision * recall / (precision + recall), np.nan)

        # MCC -- unlike F1, doesn't get gamed by a skewed base rate. F1 can
        # reward "flag everything" whenever positives are the majority class
        # (precision floors at the base rate, recall maxes at 1.0, and F1
        # rewards that combination) -- MCC uses all four confusion-matrix
        # cells (TP, FP, FN, TN) and sits at ~0 for both "flag everything"
        # and "flag nothing", so it can't be fooled the same way.
        #
        # Cast to float64 BEFORE multiplying: tp/fp/fn/tn are int64 counts
        # (tens of millions at full-dataset scale), and multiplying four of
        # them together overflows int64's range even at test-data scale --
        # numpy wraps silently on overflow (no exception), producing garbage
        # MCC values outside [-1, 1] with no warning. Confirmed by testing:
        # uncaught, this gave MCC=104 on a 200k-row synthetic check.
        tp_f, fp_f, fn_f, tn_f = tp.astype(np.float64), fp.astype(np.float64), fn.astype(np.float64), tn.astype(np.float64)
        mcc_denom = np.sqrt((tp_f + fp_f) * (tp_f + fn_f) * (tn_f + fp_f) * (tn_f + fn_f))
        mcc = np.where(mcc_denom > 0, (tp_f * tn_f - fp_f * fn_f) / mcc_denom, np.nan)

    # AUC via trapezoid over (FPR, TPR). thresholds increase with index (0 -> 1),
    # so recall/fpr both DECREASE with index (higher cutoff = fewer rows counted
    # positive) -- i.e. fpr runs high-to-low left-to-right in this array. Feeding
    # that straight into the integrator integrates from FPR=1 down to FPR=0,
    # which is the negative of the standard AUC -- reverse both arrays first so
    # fpr is increasing, matching the normal ROC-AUC convention.
    valid = ~np.isnan(recall) & ~np.isnan(fpr)
    r_valid, f_valid = recall[valid][::-1], fpr[valid][::-1]
    auc = np.trapezoid(r_valid, f_valid) if P > 0 and N > 0 else float("nan")

    best_f1_i = np.nanargmax(f1)
    best_mcc_i = np.nanargmax(mcc)

    # Degenerate-threshold flag: if the best cutoff found is the very first
    # bin (or within one bin of it), the "optimum" is just "flag every row
    # active" -- precision there equals the raw base rate (P/(P+N)) and
    # recall is trivially 1.0, which can beat every real cutoff on F1 alone
    # whenever positives are the majority class. Not a real finding -- flag
    # it instead of silently reporting it as though it were one.
    base_rate = P / (P + N) if (P + N) > 0 else float("nan")
    f1_degenerate = best_f1_i <= 1
    mcc_degenerate = best_mcc_i <= 1  # MCC is far less prone to this, checked anyway

    return {
        "P": int(P), "N": int(N), "base_rate": base_rate,
        "thresholds": thresholds, "precision": precision, "recall": recall,
        "fpr": fpr, "f1": f1, "mcc": mcc, "auc": auc,
        "best_threshold": float(thresholds[best_f1_i]),
        "best_f1": float(f1[best_f1_i]),
        "best_precision": float(precision[best_f1_i]),
        "best_recall": float(recall[best_f1_i]),
        "f1_degenerate": f1_degenerate,
        "best_mcc_threshold": float(thresholds[best_mcc_i]),
        "best_mcc": float(mcc[best_mcc_i]),
        "mcc_precision": float(precision[best_mcc_i]),
        "mcc_recall": float(recall[best_mcc_i]),
        "mcc_degenerate": mcc_degenerate,
    }


def metrics_at_threshold(hist_pair, threshold):
    bin_idx = int(np.searchsorted(BIN_EDGES, threshold, side="right") - 1)
    bin_idx = max(0, min(bin_idx, N_BINS - 1))
    tp = hist_pair[1][bin_idx:].sum()
    fp = hist_pair[0][bin_idx:].sum()
    P = hist_pair[1].sum()
    N = hist_pair[0].sum()
    recall = tp / P if P > 0 else float("nan")
    precision = tp / (tp + fp) if (tp + fp) > 0 else float("nan")
    fpr = fp / N if N > 0 else float("nan")
    return {"threshold": threshold, "precision": float(precision),
            "recall": float(recall), "fpr": float(fpr)}


def main():
    targets = {"any_fault_active": new_hist_pair()}
    for c in FAULT_CLASSES:
        targets[c] = new_hist_pair()

    total_rows = 0
    usecols = [CONFIDENCE_COL] + FAULT_LABEL_COLS

    print(f"Scanning {CSV} in {CHUNK:,}-row chunks...")
    for chunk in pd.read_csv(CSV, usecols=usecols, chunksize=CHUNK):
        total_rows += len(chunk)
        confidence = chunk[CONFIDENCE_COL].to_numpy(dtype=np.float64)
        confidence = np.clip(confidence, 0.0, 1.0)  # guard against any stray >1/<0

        any_active = (chunk[FAULT_LABEL_COLS].sum(axis=1) > 0).to_numpy()
        accumulate(targets["any_fault_active"], confidence, any_active)

        for c in FAULT_CLASSES:
            active = (chunk[f"fault_active_{c}"] == 1).to_numpy()
            accumulate(targets[c], confidence, active)

    print(f"Total rows scanned: {total_rows:,}\n")

    print("=== ANY FAULT ACTIVE (overall) ===")
    result = sweep_from_hist(targets["any_fault_active"])
    print(f"  P={result['P']:,}  N={result['N']:,}  base_rate={result['base_rate']:.4f}  AUC={result['auc']:.4f}")
    print(f"  Best-F1 threshold: {result['best_threshold']:.3f}  "
          f"(F1={result['best_f1']:.4f}, P={result['best_precision']:.4f}, R={result['best_recall']:.4f})"
          + ("  [DEGENERATE -- base-rate artifact, not a real cutoff]" if result["f1_degenerate"] else ""))
    print(f"  Best-MCC threshold: {result['best_mcc_threshold']:.3f}  "
          f"(MCC={result['best_mcc']:.4f}, P={result['mcc_precision']:.4f}, R={result['mcc_recall']:.4f})"
          + ("  [DEGENERATE]" if result["mcc_degenerate"] else ""))
    for name, t in REFERENCE_THRESHOLDS.items():
        m = metrics_at_threshold(targets["any_fault_active"], t)
        print(f"  At {name} ({t}): precision={m['precision']:.4f} "
              f"recall={m['recall']:.4f} FPR={m['fpr']:.4f}")

    print("\n=== PER-CLASS (same single scalar, best-achievable threshold per class) ===")
    print(f"{'class':<15}{'base_rate':>10}{'AUC':>7}{'best_F1':>9}{'F1_thr':>8}"
          f"{'best_MCC':>10}{'MCC_thr':>9}{'recall@0.10':>13}")
    any_f1_degenerate = False
    for c in FAULT_CLASSES:
        r = sweep_from_hist(targets[c])
        at_010 = metrics_at_threshold(targets[c], 0.10)
        f1_flag = "*" if r["f1_degenerate"] else " "
        any_f1_degenerate = any_f1_degenerate or r["f1_degenerate"]
        print(f"{c:<15}{r['base_rate']:>10.3f}{r['auc']:>7.3f}"
              f"{r['best_f1']:>8.3f}{f1_flag}{r['best_threshold']:>7.3f}"
              f"{r['best_mcc']:>10.3f}{r['best_mcc_threshold']:>9.3f}"
              f"{at_010['recall']:>13.3f}")
        print(f"    -> at best-F1 threshold: precision={r['best_precision']:.3f}  recall={r['best_recall']:.3f}")
        print(f"    -> at best-MCC threshold: precision={r['mcc_precision']:.3f}  recall={r['mcc_recall']:.3f}")

    print("\nAUC is the trustworthy summary number here -- threshold-independent,\n"
          "unaffected by class base rate. best_MCC is the trustworthy PER-\n"
          "THRESHOLD pick (won't degenerate to 'flag everything' the way F1 can\n"
          "when positives are the majority class). A '*' next to best_F1 means\n"
          "F1's own optimum landed on 'flag every row active' -- base_rate ==\n"
          "best_P in that row confirms it -- treat that F1/threshold pair as a\n"
          "base-rate artifact, not a real finding; read best_MCC/AUC instead.\n\n"
          "best_MCC/best_R per class is still an UPPER BOUND on what a single\n"
          "global confidence knob could ever achieve for that class alone -- the\n"
          "real watchdog can't run 5 different thresholds off one scalar at once,\n"
          "so this is a ceiling to beat, not something the current system\n"
          "achieves in practice. Compare your trained model's per-class\n"
          "recall/precision (evaluate.py) against AUC/best_MCC, not recall@0.10.")

    return targets  # kept in scope if run interactively / imported for plotting


if __name__ == "__main__":
    main()
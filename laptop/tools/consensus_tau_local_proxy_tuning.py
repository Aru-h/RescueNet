"""
Item 10: CONSENSUS_TAU_LOCAL proxy tuning (core/consensus_weighting.h).

Confirmed first (per the task's own precondition): consensus_weighting.c
has ZERO live callers anywhere in core/, board/, or laptop/ outside its
own file and its own selftest - `grep -rln "consensus_gate_" core/*.c
board/*/*.c laptop/*.c` (excluding consensus_weighting.[ch] itself)
returns nothing. This tuning is therefore explicitly provisional and
academic until the module is wired in - stated in the code comment
this script's output feeds, not just here.

What CONSENSUS_TAU_LOCAL actually gates: consensus_gate_tier()'s x==1
branch - "is this ONE board's own vote, by itself, confident enough to
justify CONSENSUS_TIER_LOCAL (same ceiling as AI-alone) trust". With
x==1 (exactly one board voting), Brooks-Iyengar's max-overlap y reduces
to that single vote's own confidence (no second interval to overlap
against) - so this reduces to an ordinary single-classifier confidence-
threshold tuning problem: how confident does a real model's own
prediction need to be before its confidence is trustworthy on its own.

PROXY, not a real model: reuses the same Config A sklearn pipeline
(HistGradientBoostingClassifier) already built and used throughout
this session's other analyses - genuinely trained, genuinely real
predict_proba output, but NOT core/fault_predictor.c (FAULT_MODEL_
LOADED is still 0, untrained) and NOT train.py (does not exist in this
repo). This is explicitly a stand-in, same epistemic status as the
provisional constants it's informing - re-tune once a real trained
model exists.
"""
import sys, os
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO_ROOT)

import gc
import numpy as np
import pandas as pd
from sklearn.model_selection import GroupShuffleSplit
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import f1_score, precision_score, recall_score

from evidence_channels import reconstruct_full_timeline, recompute_team_timeline, WEIGHTS

CSV_PATH = os.path.join(REPO_ROOT, "tron_snapshot_dataset.csv")
FAULT_CLASS_NAMES = ["mobility", "communication", "resource", "environment", "mission"]
FAULT_LABEL_COLUMNS = [f"fault_active_{n}" for n in FAULT_CLASS_NAMES]
EVIDENCE_CHANNEL_NAMES = list(WEIGHTS.keys())
EVIDENCE_FEATURE_COLUMNS = [f"evidence_{ch}" for ch in EVIDENCE_CHANNEL_NAMES] + ["confirmed_mobility_impairment"]


def downcast(frame):
    for col in frame.columns:
        if frame[col].dtype == "int64":
            frame[col] = pd.to_numeric(frame[col], downcast="integer")
        elif frame[col].dtype == "float64":
            frame[col] = pd.to_numeric(frame[col], downcast="float")
    return frame


print("Loading + dedup...")
df = pd.read_csv(CSV_PATH)
df = downcast(df)
df["_row_seq"] = np.arange(len(df), dtype=np.int32)
df = df.sort_values(["run_id", "team_id", "cycle", "_row_seq"])
df = df.drop_duplicates(subset=["run_id", "team_id", "cycle"], keep="last")

print("Evidence-channel reconstruction...")
run_max_cycle = df.groupby("run_id")["cycle"].max().to_dict()
pieces = []
for (run_id, team_id), team_rows in df.groupby(["run_id", "team_id"]):
    full_timeline, _ = reconstruct_full_timeline(team_rows, run_max_cycle[run_id])
    full_timeline = recompute_team_timeline(full_timeline)
    full_timeline["run_id"] = run_id
    full_timeline["team_id"] = team_id
    pieces.append(full_timeline[["run_id", "team_id", "cycle"] + EVIDENCE_FEATURE_COLUMNS])
evidence_df = pd.concat(pieces, ignore_index=True)
del pieces
df = df.merge(evidence_df, on=["run_id", "team_id", "cycle"], how="left")
del evidence_df
gc.collect()
df = df.drop(columns=["_row_seq"])

equipment_names = ["rope", "spreader", "medical_kit", "boat", "k9", "hazmat_suit", "structural", "generator"]
for bit, name in enumerate(equipment_names):
    df[f"equipment_{name}"] = ((df["equipment_flags"] & (1 << bit)) != 0).astype(np.uint8)
hazard_names = ["gas", "flood", "structural", "cbrn"]
for bit, name in enumerate(hazard_names):
    df[f"hazard_{name}"] = ((df["hazard_flags"] & (1 << bit)) != 0).astype(np.uint8)
df = pd.get_dummies(
    df,
    columns=["specialization", "twin_expected_operating_state", "status",
             "fitness_tier", "incident_req_capability", "support_team", "team_zone_id"],
    dtype=np.uint8,
)
df["path_is_unreachable"] = (df["twin_remaining_path_cost"] == 65535).astype(int)

numerical_features = [
    "incident_priority", "victim_count", "hours_worked", "stamina",
    "twin_expected_progress", "twin_actual_progress", "twin_progress_error",
    "twin_expected_stamina", "twin_actual_stamina", "twin_stamina_error",
    "twin_remaining_path_cost", "twin_expected_work_remaining",
    "twin_mission_stage", "twin_communication_health", "equipment_level", "team_zone_trust",
]
one_hot_features = [c for c in df.columns if c.startswith((
    "specialization_", "status_", "fitness_tier_", "incident_req_capability_",
    "twin_expected_operating_state_", "support_team_", "team_zone_id_"))]
bitmask_features = [
    "equipment_rope", "equipment_spreader", "equipment_medical_kit", "equipment_boat",
    "equipment_k9", "equipment_hazmat_suit", "equipment_structural", "equipment_generator",
    "hazard_gas", "hazard_flood", "hazard_structural", "hazard_cbrn",
]
feature_columns = numerical_features + one_hot_features + bitmask_features + EVIDENCE_FEATURE_COLUMNS + ["path_is_unreachable"]

real_nonzero_mask = (df["twin_remaining_path_cost"] != 65535) & (df["twin_remaining_path_cost"] != 0)
mean_nonzero_real_cost = df.loc[real_nonzero_mask, "twin_remaining_path_cost"].mean()
df["twin_remaining_path_cost"] = df["twin_remaining_path_cost"].astype(float)
df.loc[df["twin_remaining_path_cost"] == 65535, "twin_remaining_path_cost"] = mean_nonzero_real_cost

y_multi = df[FAULT_LABEL_COLUMNS].astype(np.uint8)
y_multi.columns = FAULT_CLASS_NAMES


def group_split(frame, y_balance, group_col="run_id", test_size=0.15, val_size=0.15, seed=42, max_tries=40):
    overall_rate = y_balance.mean()
    groups = frame[group_col].values
    for attempt in range(max_tries):
        gss1 = GroupShuffleSplit(n_splits=1, test_size=test_size, random_state=seed + attempt)
        trainval_idx, test_idx = next(gss1.split(frame, y_balance, groups=groups))
        val_frac = val_size / (1 - test_size)
        gss2 = GroupShuffleSplit(n_splits=1, test_size=val_frac, random_state=seed + attempt + 1000)
        train_rel, val_rel = next(gss2.split(trainval_idx, y_balance.iloc[trainval_idx], groups=groups[trainval_idx]))
        train_idx = trainval_idx[train_rel]
        val_idx = trainval_idx[val_rel]
        rates = [y_balance.iloc[idx].mean() for idx in (train_idx, val_idx, test_idx)]
        if all(abs(r - overall_rate) / overall_rate < 0.30 for r in rates):
            return train_idx, val_idx, test_idx
    raise RuntimeError("no balanced split found")


y_balance = y_multi.max(axis=1)
train_idx, val_idx, test_idx = group_split(df, y_balance)

X = df[feature_columns].astype(np.float32)

pooled_proba = []
pooled_truth = []
per_class_results = {}

for cls in FAULT_CLASS_NAMES:
    clf = HistGradientBoostingClassifier(max_iter=150, max_depth=6, learning_rate=0.1,
                                           random_state=42, class_weight="balanced")
    clf.fit(X.iloc[train_idx], y_multi[cls].iloc[train_idx])
    proba = clf.predict_proba(X.iloc[test_idx])[:, 1]
    truth = y_multi[cls].iloc[test_idx].to_numpy()
    pooled_proba.append(proba)
    pooled_truth.append(truth)
    per_class_results[cls] = (proba, truth)
    print(f"Trained + scored {cls}")

pooled_proba = np.concatenate(pooled_proba)
pooled_truth = np.concatenate(pooled_truth)

print("\n" + "=" * 70)
print("Sweeping candidate CONSENSUS_TAU_LOCAL thresholds against pooled "
      "(all 5 classes) held-out predicted confidence vs. real ground truth")
print("=" * 70)
best_f1 = -1.0
best_tau = None
for tau in np.arange(0.05, 0.96, 0.05):
    pred = (pooled_proba >= tau).astype(int)
    p = precision_score(pooled_truth, pred, zero_division=0)
    r = recall_score(pooled_truth, pred, zero_division=0)
    f1 = f1_score(pooled_truth, pred, zero_division=0)
    marker = ""
    if f1 > best_f1:
        best_f1 = f1
        best_tau = tau
        marker = "  <-- best F1 so far"
    print(f"  tau={tau:.2f}  precision={p:.4f}  recall={r:.4f}  F1={f1:.4f}{marker}")

print(f"\nF1-maximizing pooled threshold: tau={best_tau:.2f} (F1={best_f1:.4f})")

# Also report at a couple of fixed reference points for context.
for ref_tau in (0.5, best_tau):
    pred = (pooled_proba >= ref_tau).astype(int)
    p = precision_score(pooled_truth, pred, zero_division=0)
    r = recall_score(pooled_truth, pred, zero_division=0)
    print(f"  reference: tau={ref_tau:.2f}  precision={p:.4f}  recall={r:.4f}")

print("\n" + "=" * 70)
print("RECOMMENDATION (explicitly provisional/proxy-derived)")
print("=" * 70)
print(f"CONSENSUS_TAU_LOCAL candidate: {best_tau:.2f} (currently 0.5f, an unvalidated round number)")
print("This is a PROXY value from a real-but-not-production classifier, pooled across all "
      "5 fault classes (CONSENSUS_TAU_LOCAL is one constant applied identically regardless "
      "of class) - re-tune once core/fault_predictor.c has real trained weights and "
      "FAULT_MODEL_LOADED=1, using its actual confidence distribution instead of this proxy.")

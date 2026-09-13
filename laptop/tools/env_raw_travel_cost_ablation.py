"""
Item 9: does the new env_raw_travel_cost signal (core/environment.h's
environment_cell_travel_cost() at the team's own live position, plumbed
through laptop/snapshot.c into tron_snapshot_dataset.csv) actually
improve environment-class detectability? Real before/after F1 on the
same Config A feature pipeline + HistGradientBoostingClassifier
methodology already used this session, not just "it was added."
"""
import sys, os
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO_ROOT)

import gc
import numpy as np
import pandas as pd
from sklearn.model_selection import GroupShuffleSplit
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import precision_score, recall_score, f1_score

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

if "env_raw_travel_cost" not in df.columns:
    raise RuntimeError("env_raw_travel_cost column missing - regenerate the dataset with the "
                        "updated laptop/snapshot.c before running this ablation.")

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
base_feature_columns = numerical_features + one_hot_features + bitmask_features + EVIDENCE_FEATURE_COLUMNS + ["path_is_unreachable"]

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
            print(f"Accepted split on attempt {attempt}.")
            return train_idx, val_idx, test_idx
    raise RuntimeError("no balanced split found")


y_balance = y_multi.max(axis=1)
train_idx, val_idx, test_idx = group_split(df, y_balance)

results = {}
for label, feature_columns in [
    ("BEFORE (without env_raw_travel_cost)", base_feature_columns),
    ("AFTER (with env_raw_travel_cost)", base_feature_columns + ["env_raw_travel_cost"]),
]:
    X = df[feature_columns].astype(np.float32)
    X_train, X_test = X.iloc[train_idx], X.iloc[test_idx]
    y_train, y_test = y_multi["environment"].iloc[train_idx], y_multi["environment"].iloc[test_idx]

    clf = HistGradientBoostingClassifier(max_iter=150, max_depth=6, learning_rate=0.1,
                                           random_state=42, class_weight="balanced")
    clf.fit(X_train, y_train)
    pred = clf.predict(X_test)
    precision = precision_score(y_test, pred, zero_division=0)
    recall = recall_score(y_test, pred, zero_division=0)
    f1 = f1_score(y_test, pred, zero_division=0)
    results[label] = dict(precision=precision, recall=recall, f1=f1)
    print(f"\n{label} - {len(feature_columns)} features:")
    print(f"  environment class: P={precision:.4f} R={recall:.4f} F1={f1:.4f}")

print("\n" + "=" * 70)
print("VERDICT")
print("=" * 70)
before_f1 = results["BEFORE (without env_raw_travel_cost)"]["f1"]
after_f1 = results["AFTER (with env_raw_travel_cost)"]["f1"]
delta = after_f1 - before_f1
print(f"environment-class F1: {before_f1:.4f} -> {after_f1:.4f}  (delta {delta:+.4f})")
if delta > 0.02:
    print("Real, meaningful improvement from the new raw signal.")
elif delta > 0.0:
    print("Small positive delta - a real but modest improvement, not a fix for the "
          "structural observability gap on its own.")
else:
    print("No improvement (or a regression) from this signal alone on this dataset - "
          "stating this plainly rather than overstating the fix's impact.")

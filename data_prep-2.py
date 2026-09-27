"""
Data preparation pipeline for the v4 fault predictor.

Loads the simulator's raw CSV, deduplicates per-tick rows, constructs the
5-class ground-truth labels, merges in the 7 recomputed evidence-channel
features plus confirmed_mobility_impairment, encodes categorical/bitmask
fields, splits by run_id into train/val/test, and scales the final feature
matrix.
"""

import gc
import pandas as pd
import numpy as np
import pickle
import os
from sklearn.model_selection import GroupShuffleSplit
from evidence_channels import reconstruct_full_timeline, recompute_team_timeline, WEIGHTS

pd.set_option("display.max_columns", None)

OUTPUT_DIR = "data_prep_output"
os.makedirs(OUTPUT_DIR, exist_ok=True)

# ── Ground-truth label columns ──────────────────────────────────────────
# Order matches core/fault_config.h's OperationalFaultClass_t enum
# (MOBILITY, COMMUNICATION, RESOURCE, ENVIRONMENT, MISSION), so a saved
# column index always maps back to the same fault class the simulator uses.
FAULT_CLASS_NAMES = ["mobility", "communication", "resource", "environment", "mission"]
FAULT_LABEL_COLUMNS = [f"fault_active_{name}" for name in FAULT_CLASS_NAMES]

# ── Evidence-channel feature names ──────────────────────────────────────
# Pulled directly from evidence_channels.py's own WEIGHTS dict so the two
# files can never silently drift out of sync.
EVIDENCE_CHANNEL_NAMES = list(WEIGHTS.keys())
EVIDENCE_FEATURE_COLUMNS = [f"evidence_{ch}" for ch in EVIDENCE_CHANNEL_NAMES] + [
    "confirmed_mobility_impairment"
]


def downcast_numeric_columns(frame):
    """Shrinks every int/float column to the smallest type that still
    exactly holds every value already present in it."""
    for col in frame.columns:
        if frame[col].dtype == "int64":
            frame[col] = pd.to_numeric(frame[col], downcast="integer")
        elif frame[col].dtype == "float64":
            frame[col] = pd.to_numeric(frame[col], downcast="float")
    return frame


# ── Load ──────────────────────────────────────────────────────────────
CSV_PATH = "simulation_output_final_v4.csv"

df = pd.read_csv(CSV_PATH)
df = downcast_numeric_columns(df)

# Deduplicate: the per-tick logging fix means a tick that already had a
# real event also gets a second EVENT_TICK row for the same tick. Keep the
# last row per (run_id, team_id, cycle) using an explicit row-order
# tiebreak (EVENT_TICK is always logged strictly after any real event row
# for the same tick, so "last" reliably means "the more final snapshot").
df["_row_seq"] = np.arange(len(df), dtype=np.int32)
df = df.sort_values(["run_id", "team_id", "cycle", "_row_seq"])
df = df.drop_duplicates(subset=["run_id", "team_id", "cycle"], keep="last")
# _row_seq is kept for now — reconstruct_full_timeline() below expects it
# on the team-level slices it's handed, and it's dropped once that's done.

seeds = sorted(df["world_seed"].unique())
print("Number of distinct seeds:", len(seeds))
print("Min seed:", seeds[0], "Max seed:", seeds[-1])
print("Contiguous (max-min+1 == count)?", seeds[-1] - seeds[0] + 1 == len(seeds))

max_cycle_per_run = df.groupby("run_id")["cycle"].max()
print("\nticks_per_seed candidates (max cycle per run):")
print(max_cycle_per_run.value_counts().head(10))

print(list(df.columns))

# Reference only, never used as the training target — the watchdog's own
# live output, kept for later comparison in evaluate.py.
anomaly_rows = df[df["watchdog_anomaly_detected"] == 1]
print(anomaly_rows["watchdog_failure_mode"].value_counts())
print("Shape:", df.shape)
print("\nwatchdog_anomaly_detected (reference only, not the label):")
print(df["watchdog_anomaly_detected"].value_counts())
print("watchdog_failure_mode" in df.columns)
print(df["watchdog_failure_mode"].value_counts() if "watchdog_failure_mode" in df.columns else "not present")

# ── Label construction — ground truth, multi-label ──────────────────
# Read directly from fault_injector.c's ground truth, one binary column per
# fault class. Deliberately never derived from watchdog_anomaly_detected or
# watchdog_failure_mode: ground truth is information the watchdog cannot
# access at all, which is the entire premise behind building this model.
missing_label_cols = [c for c in FAULT_LABEL_COLUMNS if c not in df.columns]
if missing_label_cols:
    raise RuntimeError(
        f"Expected ground-truth columns missing from CSV: {missing_label_cols}."
    )

y_multi = df[FAULT_LABEL_COLUMNS].astype(np.uint8)
y_multi.columns = FAULT_CLASS_NAMES

bad_values = ~y_multi.isin([0, 1])
if bad_values.any().any():
    raise RuntimeError(
        "Non-binary values found in fault_active_* columns — label columns "
        "are supposed to be strictly 0/1. Inspect the CSV before proceeding."
    )

print("\nGround-truth label positive rates (per class, whole dataset):")
print(y_multi.mean().sort_values(ascending=False))
print("Any-fault-active rate (row has >=1 of the 5 classes active):",
      y_multi.max(axis=1).mean())

# ── Evidence-channel + confirmed_mobility_impairment merge ───────────
# Wired directly against evidence_channels.py's own reconstruct_full_timeline()/
# recompute_team_timeline() functions rather than re-deriving the rise/decay
# math a second time here.
#
# Must run before the one-hot encoding block below: recompute_team_timeline()
# reads status/twin_expected_operating_state as their original numeric codes,
# and one-hot encoding would replace those columns with dummy columns.
run_max_cycle_dict = max_cycle_per_run.to_dict()

evidence_pieces = []
for (run_id, team_id), team_rows in df.groupby(["run_id", "team_id"]):
    full_timeline, n_gap_ticks = reconstruct_full_timeline(team_rows, run_max_cycle_dict[run_id])
    full_timeline = recompute_team_timeline(full_timeline)
    full_timeline["run_id"] = run_id
    full_timeline["team_id"] = team_id
    evidence_pieces.append(full_timeline[["run_id", "team_id", "cycle"] + EVIDENCE_FEATURE_COLUMNS])

evidence_df = pd.concat(evidence_pieces, ignore_index=True)
del evidence_pieces
df = df.merge(evidence_df, on=["run_id", "team_id", "cycle"], how="left")
del evidence_df
gc.collect()

# Defensive check — a merge mismatch would silently show up as NaNs in
# exactly these columns.
evidence_nan_count = df[EVIDENCE_FEATURE_COLUMNS].isna().sum().sum()
if evidence_nan_count > 0:
    raise RuntimeError(
        f"Evidence-channel merge produced {evidence_nan_count} NaN values across "
        f"{EVIDENCE_FEATURE_COLUMNS} — inspect before proceeding."
    )

df = df.drop(columns=["_row_seq"])

print("\nEvidence-channel features merged in:", EVIDENCE_FEATURE_COLUMNS)
print(df[EVIDENCE_FEATURE_COLUMNS].describe())

# ── Bitmask expansion ────────────────────────────────────────────────
equipment_names = [
    "rope", "spreader", "medical_kit", "boat",
    "k9", "hazmat_suit", "structural", "generator",
]
for bit, name in enumerate(equipment_names):
    df[f"equipment_{name}"] = ((df["equipment_flags"] & (1 << bit)) != 0).astype(np.uint8)

hazard_names = ["gas", "flood", "structural", "cbrn"]
for bit, name in enumerate(hazard_names):
    df[f"hazard_{name}"] = ((df["hazard_flags"] & (1 << bit)) != 0).astype(np.uint8)

# ── One-hot encoding ─────────────────────────────────────────────────
# assigned_incident is deliberately excluded — it's a plain incident-ID
# number, not a meaningful predictive feature.
#
# Must run after the evidence-channel merge above.
df = pd.get_dummies(
    df,
    columns=[
        "specialization",
        "twin_expected_operating_state",
        "status",
        "fitness_tier",
        "incident_req_capability",
        "support_team", "team_zone_id",
    ],
    dtype=np.uint8,
)

print([c for c in df.columns if c.startswith("team_zone_id_")])

# ── twin_remaining_path_cost: sentinel flag ──────────────────────────
df["path_is_unreachable"] = (df["twin_remaining_path_cost"] == 65535).astype(int)

# ── Final feature selection ─────────────────────────────────────────
# Raw telemetry combined with the 7 evidence-channel scores +
# confirmed_mobility_impairment. Raw features react immediately to a
# fault's onset; the evidence channels are deliberately smoothed the same
# way the watchdog computes them internally. Combining both gives the
# model fast-reacting signal plus multi-tick corroboration, without
# inheriting the watchdog's own reaction lag.
numerical_features = [
    "incident_priority", "victim_count", "hours_worked", "stamina",
    "twin_expected_progress", "twin_actual_progress", "twin_progress_error",
    "twin_expected_stamina", "twin_actual_stamina", "twin_stamina_error",
    "twin_remaining_path_cost", "twin_expected_work_remaining",
    "twin_mission_stage", "twin_communication_health", "equipment_level", "team_zone_trust",
    "env_raw_travel_cost",
]

one_hot_features = [
    col for col in df.columns
    if col.startswith((
        "specialization_", "status_", "fitness_tier_",
        "incident_req_capability_", "twin_expected_operating_state_",
        "support_team_", "team_zone_id_",
    ))
]

bitmask_features = [
    "equipment_rope", "equipment_spreader", "equipment_medical_kit", "equipment_boat",
    "equipment_k9", "equipment_hazmat_suit", "equipment_structural", "equipment_generator",
    "hazard_gas", "hazard_flood", "hazard_structural", "hazard_cbrn",
]

evidence_features = EVIDENCE_FEATURE_COLUMNS

feature_columns = numerical_features + one_hot_features + bitmask_features + evidence_features

# Defensive check — ground truth must never leak into the model's inputs.
# By name: the labels themselves plus every other injector-only column the
# snapshot writer emits alongside them (severity, ticks remaining, mask).
GROUND_TRUTH_COLUMNS = (
    FAULT_LABEL_COLUMNS
    + [f"fault_severity_{name}" for name in FAULT_CLASS_NAMES]
    + [f"fault_ticks_remaining_{name}" for name in FAULT_CLASS_NAMES]
    + ["fault_active_mask"]
    # Label-only columns of tron_peer_obs.csv (--peer-log): the bus's own
    # record of what it dropped. Never a feature; the gt_ prefix marks them.
    + ["gt_link_dropped", "gt_loss_model"]
)
leaked = set(feature_columns) & set(GROUND_TRUTH_COLUMNS)
leaked |= {c for c in feature_columns if c.startswith("gt_")}
assert not leaked, f"Ground-truth column(s) leaked into features: {leaked}"

# By value: a name check alone can't catch a feature that is the label
# under another name. twin_communication_health used to be exactly
# fault_active_communication * severity on every row and passed the name
# check above. Flag any feature whose "non-zero" pattern matches a label
# on (almost) every row, or that correlates with it almost perfectly.
# An honest observable of a fault tracks it closely but not exactly (it
# lags onset and trails recovery), so it stays under these limits.
LEAK_AGREEMENT_LIMIT = 0.995
LEAK_CORRELATION_LIMIT = 0.99
value_leaks = []
for label in FAULT_LABEL_COLUMNS:
    truth = df[label].astype(bool)
    if truth.nunique() < 2:
        continue
    for col in feature_columns:
        values = pd.to_numeric(df[col], errors="coerce").fillna(0)
        if values.nunique() < 2:
            continue
        agreement = ((values != 0) == truth).mean()
        correlation = abs(np.corrcoef(values, truth.astype(float))[0, 1])
        if agreement >= LEAK_AGREEMENT_LIMIT or correlation >= LEAK_CORRELATION_LIMIT:
            value_leaks.append(f"{col}~{label} (agree={agreement:.4f}, |r|={correlation:.4f})")
assert not value_leaks, (
    "Feature(s) are near-exact copies of a ground-truth label: "
    + "; ".join(value_leaks))

X = df[feature_columns]

print("\nNumber of features (before path_is_unreachable):", X.shape[1])
print("Missing values:\n", X.isna().sum().sum(), "total NaNs")


# ── Group-based split ────────────────────────────────────────────────
def group_split(df, y_balance, group_col="run_id", test_size=0.15, val_size=0.15, seed=42, max_tries=20):
    """
    Splits by whole run_id groups so no run appears in more than one split.
    Retries with new seeds if the resulting positive-rate per split drifts
    too far from the overall rate.
    """
    overall_rate = y_balance.mean()
    groups = df[group_col].values

    for attempt in range(max_tries):
        gss1 = GroupShuffleSplit(n_splits=1, test_size=test_size, random_state=seed + attempt)
        trainval_idx, test_idx = next(gss1.split(df, y_balance, groups=groups))

        val_fraction_of_trainval = val_size / (1 - test_size)
        gss2 = GroupShuffleSplit(n_splits=1, test_size=val_fraction_of_trainval,
                                  random_state=seed + attempt + 1000)
        train_idx_rel, val_idx_rel = next(gss2.split(
            trainval_idx, y_balance.iloc[trainval_idx], groups=groups[trainval_idx]
        ))
        train_idx = trainval_idx[train_idx_rel]
        val_idx = trainval_idx[val_idx_rel]

        rates = [y_balance.iloc[idx].mean() for idx in (train_idx, val_idx, test_idx)]
        if all(abs(r - overall_rate) / overall_rate < 0.25 for r in rates):
            print(f"Accepted split on attempt {attempt}. Any-fault-active rates — "
                  f"train: {rates[0]:.4f}, val: {rates[1]:.4f}, test: {rates[2]:.4f} "
                  f"(overall: {overall_rate:.4f})")
            return train_idx, val_idx, test_idx

    raise RuntimeError("Could not find a balanced group split after max_tries — "
                        "inspect anomaly distribution across run_ids manually.")


y_balance = y_multi.max(axis=1)  # "any fault active"
train_idx, val_idx, test_idx = group_split(df, y_balance, group_col="run_id")

print("\nPer-class positive rate by split:")
for split_name, idx in [("train", train_idx), ("val", val_idx), ("test", test_idx)]:
    print(f"  {split_name}:")
    print(y_multi.iloc[idx].mean().to_string())

# ── twin_remaining_path_cost: sentinel replacement ───────────────────
# Must happen after the split — uses only training rows to avoid leakage.
# The column is heavily zero-inflated (a team that has already arrived
# shows 0 remaining cost for every subsequent tick), so the sentinel is
# replaced using the mean of actively-traveling rows only (nonzero,
# non-sentinel) rather than a plain mean over all real rows — a closer
# conceptual match, since an unreachable path resembles "still traveling"
# far more than "already arrived."
real_nonzero_train_mask = (
    (df.loc[train_idx, "twin_remaining_path_cost"] != 65535)
    & (df.loc[train_idx, "twin_remaining_path_cost"] != 0)
)

mean_nonzero_real_cost = df.loc[train_idx, "twin_remaining_path_cost"][real_nonzero_train_mask].mean()

print(f"\ntwin_remaining_path_cost training-set mean (actively-traveling rows only): "
      f"{mean_nonzero_real_cost:.2f}")
df["twin_remaining_path_cost"] = df["twin_remaining_path_cost"].astype(float)
sentinel_mask = df["twin_remaining_path_cost"] == 65535
df.loc[sentinel_mask, "twin_remaining_path_cost"] = mean_nonzero_real_cost


# Add path_is_unreachable to the feature set and rebuild X
feature_columns = feature_columns + ["path_is_unreachable"]
X = df[feature_columns]
X = downcast_numeric_columns(X)
print("Number of features (with path_is_unreachable):", X.shape[1])
del df
gc.collect()

# ── Scaling ───────────────────────────────────────────────────────────
class SafeStandardScaler:
    """Standardizes each column. Zero-variance columns are left unscaled
    instead of producing NaNs from division by zero."""

    def fit(self, X):
        X = np.asarray(X, dtype=np.float32)
        self.mean_ = X.mean(axis=0)
        std = X.std(axis=0)
        self.zero_var_mask_ = std == 0
        std[self.zero_var_mask_] = 1.0
        self.std_ = std
        return self

    def transform(self, X):
        X = np.asarray(X, dtype=np.float32)
        return (X - self.mean_) / self.std_

    def fit_transform(self, X):
        return self.fit(X).transform(X)


scaler = SafeStandardScaler()
nan_flags = {}

X_train, y_train = X.iloc[train_idx], y_multi.iloc[train_idx]
X_train_scaled = scaler.fit_transform(X_train)
nan_flags["train"] = np.isnan(X_train_scaled).any()
np.save(f"{OUTPUT_DIR}/X_train_scaled.npy", X_train_scaled)
np.save(f"{OUTPUT_DIR}/y_train.npy", y_train.values)
del X_train, X_train_scaled, y_train
gc.collect()

X_val, y_val = X.iloc[val_idx], y_multi.iloc[val_idx]
X_val_scaled = scaler.transform(X_val)
nan_flags["val"] = np.isnan(X_val_scaled).any()
np.save(f"{OUTPUT_DIR}/X_val_scaled.npy", X_val_scaled)
np.save(f"{OUTPUT_DIR}/y_val.npy", y_val.values)
del X_val, X_val_scaled, y_val
gc.collect()

X_test, y_test = X.iloc[test_idx], y_multi.iloc[test_idx]
X_test_scaled = scaler.transform(X_test)
nan_flags["test"] = np.isnan(X_test_scaled).any()
np.save(f"{OUTPUT_DIR}/X_test_scaled.npy", X_test_scaled)
np.save(f"{OUTPUT_DIR}/y_test.npy", y_test.values)
del X_test, X_test_scaled, y_test
gc.collect()

print("\nAny NaNs after scaling? — train:", nan_flags["train"],
      "val:", nan_flags["val"], "test:", nan_flags["test"])

with open(f"{OUTPUT_DIR}/scaler.pkl", "wb") as f:
    pickle.dump(scaler, f)

with open(f"{OUTPUT_DIR}/feature_columns.pkl", "wb") as f:
    pickle.dump(feature_columns, f)

with open(f"{OUTPUT_DIR}/fault_class_names.pkl", "wb") as f:
    pickle.dump(FAULT_CLASS_NAMES, f)

print(f"\nSaved all preprocessed arrays + scaler + feature_columns + "
      f"fault_class_names to ./{OUTPUT_DIR}/")
print("train.py can now load these directly instead of re-running this script.")

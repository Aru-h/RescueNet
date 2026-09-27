"""
Data preparation pipeline for the fault-detection model's 74-input feature
contract (FEATURE_CONTRACT_DESIGN.md; decisions D1-D10 finalized in review).
Supersedes data_prep-2.py and data_prep-3.py - neither is imported here, and
this is the only place the 74 inputs are defined; core/feature_spec.h and
feature_spec.py are generated from this file's feature_spec.json output
(generate_feature_spec.py), never hand-edited to match.

Loads the simulator's raw CSV, deduplicates per (run_id, cycle, team_id),
constructs the 5-class ground-truth labels, reads the 7 evidence-channel
features + confirmed_mobility_impairment straight from the CSV, encodes
categorical/bitmask fields using FIXED one-hot vocabularies, adds the
stall-fraction and incident-delta history features, splits by run_id into
train/val/test, and scales the final 74-column feature matrix.

Combines data_prep-2.py's ground-truth guard rails (GROUND_TRUTH_COLUMNS
name guard, the gt_* prefix guard, the value-based leak check) with
data_prep-3.py's stall_frac_last_5/_10 and delta_priority/delta_victim_count
history features (core/fault_predictor_features.c mirrors these exactly on
the board - see that file for why on-scene teams counting as "stalled" is
intentional and not something to silently change here), on top of:

- Decision 1 (keep): team_zone_id, team_zone_trust and env_raw_travel_cost
  stay in - they are board-computable via the mission-link zone/map frames
  (core/mission_link.h sections 5c/5e/5f), so the earlier research doc's
  recommendation to drop them was based on a wrong claim about what the
  board can compute (corrected in that doc's own changelog).
- Decision 2 (evidence source): evidence_progress..evidence_timeout and
  confirmed_mobility_impairment are read straight from the CSV - the C
  watchdog's own values (laptop/snapshot.c's write_evidence_columns()),
  not evidence_channels.py's Python reconstruction. That module stays
  importable as an OPTIONAL cross-check only (--evidence-parity-check
  below, never merged into the training features): merging its output
  back into a dataframe that already has same-named columns from the CSV
  would collide silently (pandas suffixes both _x/_y on merge, and neither
  plain name survives) - see environment_class_e2e_stress.py and env_raw_
  travel_cost_ablation.py, which hit exactly this and were fixed the same
  way.
- Decision 4 (no-task encoding): incident_priority/victim_count already
  read 0, not -1, for a team with no incident assigned - laptop/snapshot.c
  writes it that way now, so there is nothing left to do here beyond not
  re-deriving a -1 sentinel from something else. incident_req_capability
  is untouched: it keeps -1 as its own one-hot "none" category, which
  can never collide with a real capability value of 0.
- Decision 5 (run_id): laptop/snapshot.c now computes it as a deterministic
  hash of (world_seed, fault profile, CONSENSUS_ENFORCE, vbus link loss),
  not process-start-time + seed - grouped/split on exactly as before, but
  now asserted unique per run rather than merely assumed (a run_id used to
  collide across two dataset-generation processes started in the same
  wall-clock second - docs/research/feature_contract_research.md Q7).
- Decision 6 (fixed vocabularies): one-hot columns come from FIXED_
  VOCABULARIES below (the C enums' own value ranges), not from whichever
  values happen to appear in a partial seed sample - team_zone_id and
  incident_req_capability used to be data-dependent. support_team collapses
  to a single has_support (0/1) flag instead of a one-hot block.
"""
import argparse
import json
import os
import pickle
import sys

import numpy as np
import pandas as pd
from sklearn.model_selection import GroupShuffleSplit

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
# Order matches core/watchdog.h's WatchdogEvidenceSlot_t enum, which is
# also evidence_channels.py's WEIGHTS dict order - kept as a literal list
# here (Decision 2: this file no longer imports evidence_channels.py for
# the main pipeline, only for the optional parity check below).
EVIDENCE_CHANNEL_NAMES = [
    "progress", "stamina", "communication", "operating_state",
    "path", "assignment", "timeout",
]
EVIDENCE_FEATURE_COLUMNS = [f"evidence_{ch}" for ch in EVIDENCE_CHANNEL_NAMES] + [
    "confirmed_mobility_impairment"
]

# ── Fixed one-hot vocabularies (Decision 6) ──────────────────────────────
# From the C enums' own value ranges, not from whatever a partial sample
# happens to contain. pd.get_dummies() with these as pd.Categorical
# categories always emits every column below, even ones with zero rows in
# a small sample (e.g. the 5-seed sanity run) - required for the board's
# fixed-width input vector (core/feature_spec.h) to line up with training.
FIXED_VOCABULARIES = {
    "specialization": list(range(5)),                     # TeamSpecialization_t
    "twin_expected_operating_state": list(range(4)),
    "status": list(range(4)),                              # TeamStatus_t
    "fitness_tier": list(range(3)),                         # Fitness_Tier
    "incident_req_capability": [-1, 0, 1, 2, 3],           # -1 = "none"
    "team_zone_id": list(range(10)),                        # NUM_SEARCH_ZONES
}

# GROUND_TRUTH_COLUMNS (data_prep-2.py's guard, kept verbatim in spirit):
# the labels themselves, plus every other injector-only column the
# snapshot writer emits alongside them, plus the gt_ prefixed label-only
# columns tron_peer_obs.csv carries (not loaded by this file, but the
# guard stays so a future consolidated loader can't silently leak them).
# consensus_* columns are separately guarded below (Decision 8: consensus
# outputs are not model inputs) since they aren't "ground truth" in the
# same sense, just out of scope for this model.
GROUND_TRUTH_COLUMNS = (
    FAULT_LABEL_COLUMNS
    + [f"fault_severity_{name}" for name in FAULT_CLASS_NAMES]
    + [f"fault_ticks_remaining_{name}" for name in FAULT_CLASS_NAMES]
    + ["fault_active_mask"]
    + ["gt_link_dropped", "gt_loss_model"]
)
LEAK_AGREEMENT_LIMIT = 0.995
LEAK_CORRELATION_LIMIT = 0.99


def downcast_numeric_columns(frame):
    """Shrinks every int/float column to the smallest type that still
    exactly holds every value already present in it."""
    for col in frame.columns:
        if frame[col].dtype == "int64":
            frame[col] = pd.to_numeric(frame[col], downcast="integer")
        elif frame[col].dtype == "float64":
            frame[col] = pd.to_numeric(frame[col], downcast="float")
    return frame


def parse_args():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("csv_path", nargs="?", default="tron_snapshot_dataset.csv",
                     help="Path to the snapshot CSV (default: tron_snapshot_dataset.csv)")
    ap.add_argument("--evidence-parity-check", action="store_true",
                     help="Also recompute evidence via evidence_channels.py's Python "
                          "mirror and report agreement against the CSV's own evidence "
                          "columns. Optional cross-check only (Decision 2) - the result "
                          "is never merged into the training features.")
    return ap.parse_args()


def run_evidence_parity_check(df):
    """Optional cross-check (Decision 2): compares the CSV's own evidence_*/
    confirmed_mobility_impairment columns (written directly by the C
    watchdog, laptop/snapshot.c) against evidence_channels.py's independent
    Python reconstruction, on a throwaway copy of the relevant columns -
    never merged into df or the training features."""
    from evidence_channels import reconstruct_full_timeline, recompute_team_timeline

    run_max_cycle_dict = df.groupby("run_id")["cycle"].max().to_dict()
    pieces = []
    for (run_id, team_id), team_rows in df.groupby(["run_id", "team_id"]):
        full_timeline, _ = reconstruct_full_timeline(team_rows.copy(), run_max_cycle_dict[run_id])
        full_timeline = recompute_team_timeline(full_timeline)
        full_timeline["run_id"] = run_id
        full_timeline["team_id"] = team_id
        pieces.append(full_timeline[["run_id", "team_id", "cycle"] + EVIDENCE_FEATURE_COLUMNS])
    mirror_df = pd.concat(pieces, ignore_index=True)
    mirror_df = mirror_df.rename(columns={c: f"{c}_mirror" for c in EVIDENCE_FEATURE_COLUMNS})
    merged = df[["run_id", "team_id", "cycle"] + EVIDENCE_FEATURE_COLUMNS].merge(
        mirror_df, on=["run_id", "team_id", "cycle"], how="left")

    print("\nEvidence parity check (C watchdog CSV columns vs. Python mirror):")
    for col in EVIDENCE_FEATURE_COLUMNS:
        real = merged[col].astype(float)
        mirror = merged[f"{col}_mirror"].astype(float)
        diff = (real - mirror).abs()
        agree = (real.round(4) == mirror.round(4)).mean()
        print(f"  {col}: mean|diff|={diff.mean():.6f} max|diff|={diff.max():.6f} "
              f"exact-agreement={agree:.4%}")


args = parse_args()
CSV_PATH = args.csv_path

# ── Load ──────────────────────────────────────────────────────────────
df = pd.read_csv(CSV_PATH)
df = downcast_numeric_columns(df)

rows_before_dedup = len(df)

# Deduplicate: keep the last row per (run_id, cycle, team_id) using an
# explicit row-order tiebreak (the more final snapshot for a tick that
# logged more than one event row).
df["_row_seq"] = np.arange(len(df), dtype=np.int32)
df = df.sort_values(["run_id", "team_id", "cycle", "_row_seq"])
df = df.drop_duplicates(subset=["run_id", "cycle", "team_id"], keep="last")
df = df.drop(columns=["_row_seq"])

rows_after_dedup = len(df)
print(f"Rows before dedup: {rows_before_dedup}")
print(f"Rows after dedup:  {rows_after_dedup} "
      f"({100 * (1 - rows_after_dedup / rows_before_dedup):.1f}% dropped)")

seeds = sorted(df["world_seed"].unique())
print("Number of distinct seeds:", len(seeds))
print("Min seed:", seeds[0], "Max seed:", seeds[-1])
print("Contiguous (max-min+1 == count)?", seeds[-1] - seeds[0] + 1 == len(seeds))

max_cycle_per_run = df.groupby("run_id")["cycle"].max()
print("\nticks_per_seed candidates (max cycle per run):")
print(max_cycle_per_run.value_counts().head(10))

# ── Decision 5: composite run_id, asserted unique per run ──────────────
# run_id (laptop/snapshot.c's compute_composite_run_id()) is a deterministic
# function of (world_seed, fault profile, CONSENSUS_ENFORCE, vbus link
# loss). Within one dataset-generation session (fixed profile/enforce/
# link-loss - CONSENSUS_DESIGN.md 5.1's provenance rule: never mix 0/1
# enforce runs in one training set), this must be a bijection with
# world_seed - a collision here means either a genuine hash collision
# (astronomically unlikely at these row counts) or the dataset mixes run
# configurations and needs regenerating consistently.
run_to_seed_count = df.groupby("run_id")["world_seed"].nunique()
seed_to_run_count = df.groupby("world_seed")["run_id"].nunique()
assert run_to_seed_count.max() == 1, (
    "run_id maps to more than one world_seed - a run_id collision or a "
    "corrupted CSV; inspect before proceeding.")
assert seed_to_run_count.max() == 1, (
    "world_seed maps to more than one run_id - this dataset mixes fault "
    "profile/CONSENSUS_ENFORCE/vbus-link-loss configurations across runs "
    "of the same seed; regenerate with a single, consistent configuration.")
print(f"run_id uniqueness OK: {df['run_id'].nunique()} distinct run_ids "
      f"for {len(seeds)} distinct world_seeds.")

# Reference only, never used as the training target - the watchdog's own
# live output, kept for later comparison in evaluate.py.
if "watchdog_anomaly_detected" in df.columns:
    anomaly_rows = df[df["watchdog_anomaly_detected"] == 1]
    if "watchdog_failure_mode" in df.columns:
        print(anomaly_rows["watchdog_failure_mode"].value_counts())
print("Shape:", df.shape)

# ── Label construction - ground truth, multi-label ──────────────────
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
        "Non-binary values found in fault_active_* columns - label columns "
        "are supposed to be strictly 0/1. Inspect the CSV before proceeding."
    )

print("\nGround-truth label positive rates (per class, whole dataset):")
print(y_multi.mean().sort_values(ascending=False))
print("Any-fault-active rate (row has >=1 of the 5 classes active):",
      y_multi.max(axis=1).mean())

# ── Evidence-channel + confirmed_mobility_impairment (Decision 2) ─────
# Read straight off the CSV - the C watchdog's own values
# (watchdog_get_evidence_vector(), emitted by laptop/snapshot.c's
# write_evidence_columns()). No Python reconstruction, no merge: train ==
# serve, exactly, with no mirror-drift residual (docs/research/feature_
# contract_research.md Q2 measured the old Python mirror at ~99.977%
# agreement with the real watchdog - close, but not the single source of
# truth this contract requires).
missing_evidence_cols = [c for c in EVIDENCE_FEATURE_COLUMNS if c not in df.columns]
if missing_evidence_cols:
    raise RuntimeError(
        f"Expected evidence columns missing from CSV: {missing_evidence_cols}. "
        "Regenerate with a build that includes feature-contract Decision 2 "
        "(laptop/snapshot.c's write_evidence_columns())."
    )
print("\nEvidence-channel features (read directly from CSV):", EVIDENCE_FEATURE_COLUMNS)
print(df[EVIDENCE_FEATURE_COLUMNS].describe())

if args.evidence_parity_check:
    run_evidence_parity_check(df)

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

# ── has_support (Decision 6) ─────────────────────────────────────────
# support_team collapses to a single 0/1 flag instead of a data-dependent
# one-hot block ({-1, 4, 9, ...} depending on what a sample happened to
# contain). -1 means "no support team assigned" (NO_ASSIGNMENT on the
# board - see laptop/snapshot.c's snapshot_record()).
df["has_support"] = (df["support_team"] != -1).astype(np.uint8)

# ── Fixed one-hot encoding (Decision 6) ──────────────────────────────
# Cast each column to a Categorical with the FULL fixed category list
# before pd.get_dummies(), so every vocabulary column always exists, even
# with zero rows in a small sample - required for the board's fixed-width
# input vector (core/feature_spec.h) to line up with training regardless
# of sample size. Must run after the bitmask expansion above and before
# path_is_unreachable below (order doesn't matter for those two, but must
# run before final feature-column assembly).
for col, categories in FIXED_VOCABULARIES.items():
    df[col] = pd.Categorical(df[col], categories=categories)

df = pd.get_dummies(
    df,
    columns=list(FIXED_VOCABULARIES.keys()),
    dtype=np.uint8,
)

# ── twin_remaining_path_cost: sentinel flag ──────────────────────────
df["path_is_unreachable"] = (df["twin_remaining_path_cost"] == 65535).astype(int)

# ── Movement-stall and mission-fault-delta history features ──────────
# From data_prep-3.py, unchanged in definition - core/fault_predictor_
# features.c mirrors this exactly (compute_moved_this_tick()'s idle-at-
# base guard, the ring-buffer stall fraction, the per-incident baseline
# capture), and changing either side needs a coordinated retrain, not a
# one-sided tweak here. On-scene teams counting as "stalled" whenever
# they haven't moved is intentional (see that file's own header comment)
# - it is measuring lack of movement, not judging whether movement was
# expected at that instant.
#
# Corrected from data_prep-3.py's original comment: incidents DO spawn
# mid-run - generate_incidents_if_needed() runs every tick from core/
# simulation.c - so the "earliest logged row for a given (run_id,
# assigned_incident) is that incident's pre-fault baseline" reasoning
# below is only valid because a fault can't have landed on an incident
# before its own first tick, not because incidents are placed once at
# world-gen and never spawn again.
df["moved"] = (
    df.groupby(["run_id", "team_id"])[["loc_x", "loc_y"]].diff().abs().sum(axis=1) > 0
)
df["moved"] = df["moved"] | ((df["assigned_incident"] == -1) & (df["twin_remaining_path_cost"] == 0))
df["stall_frac_last_10"] = (
    (~df["moved"]).astype(int)
    .groupby([df["run_id"], df["team_id"]])
    .transform(lambda s: s.rolling(10, min_periods=1).mean())
)
df["stall_frac_last_5"] = (
    (~df["moved"]).astype(int)
    .groupby([df["run_id"], df["team_id"]])
    .transform(lambda s: s.rolling(5, min_periods=1).mean())
)

incident_mask = df["assigned_incident"] != -1
baseline_lookup = (
    df.loc[incident_mask, ["run_id", "assigned_incident", "cycle",
                            "incident_priority", "victim_count"]]
    .sort_values("cycle")
    .groupby(["run_id", "assigned_incident"], as_index=False)
    .first()
    .rename(columns={
        "incident_priority": "baseline_incident_priority",
        "victim_count": "baseline_victim_count",
    })
    .drop(columns=["cycle"])
)
df = df.merge(baseline_lookup, on=["run_id", "assigned_incident"], how="left")

df["delta_priority"] = (
    df["incident_priority"].astype(np.int16) - df["baseline_incident_priority"]
)
df["delta_victim_count"] = (
    df["victim_count"].astype(np.int16) - df["baseline_victim_count"]
)
df["delta_priority"] = df["delta_priority"].fillna(0)
df["delta_victim_count"] = df["delta_victim_count"].fillna(0)
df = df.drop(columns=["baseline_incident_priority", "baseline_victim_count", "moved"])

# ── Final feature selection: the 74-input contract ───────────────────
# Order here IS the order feature_spec.json/core/feature_spec.h/feature_
# spec.py fix for good - the single source of truth, not any one of the
# files this was merged from.
numerical_features = [
    "incident_priority", "victim_count", "hours_worked", "stamina",
    "twin_expected_progress", "twin_actual_progress", "twin_progress_error",
    "twin_expected_stamina", "twin_actual_stamina", "twin_stamina_error",
    "twin_remaining_path_cost", "twin_expected_work_remaining",
    "twin_mission_stage", "twin_communication_health", "equipment_level",
    "team_zone_trust", "env_raw_travel_cost",
    "stall_frac_last_10", "stall_frac_last_5", "delta_priority", "delta_victim_count",
]  # 21

one_hot_features = [
    col for col in df.columns
    if col.startswith((
        "specialization_", "status_", "fitness_tier_",
        "incident_req_capability_", "twin_expected_operating_state_",
        "team_zone_id_",
    ))
]  # 5 + 4 + 3 + 5 + 4 + 10 = 31

has_support_feature = ["has_support"]  # 1

bitmask_features = [
    "equipment_rope", "equipment_spreader", "equipment_medical_kit", "equipment_boat",
    "equipment_k9", "equipment_hazmat_suit", "equipment_structural", "equipment_generator",
    "hazard_gas", "hazard_flood", "hazard_structural", "hazard_cbrn",
]  # 12

evidence_features = EVIDENCE_FEATURE_COLUMNS  # 8

feature_columns = (
    numerical_features + one_hot_features + has_support_feature
    + bitmask_features + evidence_features
)

print("\nNumber of features (before path_is_unreachable):", len(feature_columns))

# ── Ground-truth / consensus leak guard ──────────────────────────────
# By name: the labels themselves plus every other injector-only column,
# the gt_ prefixed peer-obs label columns, and (Decision 8) every
# consensus_*/peer_*/beacon_emitted column - consensus outputs are not
# model inputs, by design, not by omission.
CONSENSUS_OUTPUT_PREFIXES = ("consensus_", "peer_", "beacon_emitted")
leaked = set(feature_columns) & set(GROUND_TRUTH_COLUMNS)
leaked |= {c for c in feature_columns if c.startswith("gt_")}
leaked |= {c for c in feature_columns if c.startswith(CONSENSUS_OUTPUT_PREFIXES)}
assert not leaked, f"Ground-truth/consensus column(s) leaked into features: {leaked}"

# By value: a name check alone can't catch a feature that is the label
# under another name (twin_communication_health once matched fault_active_
# communication * severity on almost every row and passed the name check).
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
print("Leak guard: OK (no name-based or value-based leak found)")

X = df[feature_columns]
print("Missing values:\n", X.isna().sum().sum(), "total NaNs")

# ── Group-based split ────────────────────────────────────────────────
def group_split(frame, y_balance, group_col="run_id", test_size=0.15, val_size=0.15, seed=42, max_tries=20):
    """Splits by whole run_id groups so no run appears in more than one
    split. Retries with new seeds if the resulting positive-rate per split
    drifts too far from the overall rate."""
    overall_rate = y_balance.mean()
    groups = frame[group_col].values

    for attempt in range(max_tries):
        gss1 = GroupShuffleSplit(n_splits=1, test_size=test_size, random_state=seed + attempt)
        trainval_idx, test_idx = next(gss1.split(frame, y_balance, groups=groups))

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
            print(f"Accepted split on attempt {attempt}. Any-fault-active rates - "
                  f"train: {rates[0]:.4f}, val: {rates[1]:.4f}, test: {rates[2]:.4f} "
                  f"(overall: {overall_rate:.4f})")
            return train_idx, val_idx, test_idx

    raise RuntimeError("Could not find a balanced group split after max_tries - "
                        "inspect anomaly distribution across run_ids manually.")


y_balance = y_multi.max(axis=1)  # "any fault active"
train_idx, val_idx, test_idx = group_split(df, y_balance, group_col="run_id")

print("\nPer-class positive rate by split:")
for split_name, idx in [("train", train_idx), ("val", val_idx), ("test", test_idx)]:
    print(f"  {split_name}:")
    print(y_multi.iloc[idx].mean().to_string())

# ── twin_remaining_path_cost: sentinel replacement ───────────────────
# Uses only training rows to avoid leakage. Replaced with the mean of
# actively-traveling rows only (nonzero, non-sentinel), not a plain mean
# over all real rows - the column is heavily zero-inflated by teams that
# have already arrived, and an unreachable path resembles "still
# traveling" far more than "already arrived."
real_nonzero_train_mask = (
    (df.loc[train_idx, "twin_remaining_path_cost"] != 65535)
    & (df.loc[train_idx, "twin_remaining_path_cost"] != 0)
)
mean_nonzero_real_cost = float(
    df.loc[train_idx, "twin_remaining_path_cost"][real_nonzero_train_mask].mean()
)

print(f"\ntwin_remaining_path_cost training-set mean (actively-traveling rows only): "
      f"{mean_nonzero_real_cost:.2f}")
df["twin_remaining_path_cost"] = df["twin_remaining_path_cost"].astype(float)
sentinel_mask = df["twin_remaining_path_cost"] == 65535
df.loc[sentinel_mask, "twin_remaining_path_cost"] = mean_nonzero_real_cost

feature_columns = feature_columns + ["path_is_unreachable"]
X = df[feature_columns]
X = downcast_numeric_columns(X)
print("Number of features (with path_is_unreachable):", X.shape[1])
assert X.shape[1] == 74, (
    f"Expected exactly 74 inputs, got {X.shape[1]}. Stop and report why "
    "rather than forcing 74 - see FEATURE_CONTRACT_DESIGN.md."
)
del df

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

X_val, y_val = X.iloc[val_idx], y_multi.iloc[val_idx]
X_val_scaled = scaler.transform(X_val)
nan_flags["val"] = np.isnan(X_val_scaled).any()
np.save(f"{OUTPUT_DIR}/X_val_scaled.npy", X_val_scaled)
np.save(f"{OUTPUT_DIR}/y_val.npy", y_val.values)
del X_val, X_val_scaled, y_val

X_test, y_test = X.iloc[test_idx], y_multi.iloc[test_idx]
X_test_scaled = scaler.transform(X_test)
nan_flags["test"] = np.isnan(X_test_scaled).any()
np.save(f"{OUTPUT_DIR}/X_test_scaled.npy", X_test_scaled)
np.save(f"{OUTPUT_DIR}/y_test.npy", y_test.values)
del X_test, X_test_scaled, y_test

print("\nAny NaNs after scaling? - train:", nan_flags["train"],
      "val:", nan_flags["val"], "test:", nan_flags["test"])

with open(f"{OUTPUT_DIR}/scaler.pkl", "wb") as f:
    pickle.dump(scaler, f)

with open(f"{OUTPUT_DIR}/feature_columns.pkl", "wb") as f:
    pickle.dump(feature_columns, f)

with open(f"{OUTPUT_DIR}/fault_class_names.pkl", "wb") as f:
    pickle.dump(FAULT_CLASS_NAMES, f)

# ── feature_spec.json: the single source of truth ────────────────────
# core/feature_spec.h and feature_spec.py are GENERATED from this file by
# generate_feature_spec.py - never hand-edited to match. mean_/std_ are
# exported as plain float lists (never recomputed downstream - loading a
# pickled scaler and recomputing from float32 differ by ~0.029 sigma,
# per docs/research/feature_contract_research.md Q7).
feature_spec = {
    "inputs": feature_columns,
    "vocabularies": FIXED_VOCABULARIES,
    "scaler_mean": [float(v) for v in scaler.mean_],
    "scaler_std": [float(v) for v in scaler.std_],
    "path_cost_sentinel_replacement": mean_nonzero_real_cost,
}
with open(f"{OUTPUT_DIR}/feature_spec.json", "w") as f:
    json.dump(feature_spec, f, indent=2)
# Also written at the repo root: generate_feature_spec.py's default input,
# and the file this project commits as the frozen contract.
with open("feature_spec.json", "w") as f:
    json.dump(feature_spec, f, indent=2)

print(f"\nSaved all preprocessed arrays + scaler + feature_columns + "
      f"fault_class_names + feature_spec.json to ./{OUTPUT_DIR}/ "
      f"(feature_spec.json also written to the repo root).")
print("The 74 inputs, in order:")
for i, name in enumerate(feature_columns):
    print(f"  [{i:2d}] {name}")

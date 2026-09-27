"""
Section 1 + 2 + 3: load CSV, dedup, ground-truth label construction (v4),
evidence-channel + confirmed_mobility_impairment feature merge, bitmask/one-hot
expansion, twin_remaining_path_cost sentinel handling, group-based
train/val/test split, scaling.
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

# ── v4 ground-truth label columns (Section 6.2) ────────────────────────
# Order matches core/fault_config.h's OperationalFaultClass_t enum
# (MOBILITY=0, COMMUNICATION, RESOURCE, ENVIRONMENT, MISSION) — kept in
# this order so a saved column index always maps back to the same fault
# class the simulator itself uses, which matters once model.py/train.py
# need to report or threshold per-class.
FAULT_CLASS_NAMES = ["mobility", "communication", "resource", "environment", "mission"]
FAULT_LABEL_COLUMNS = [f"fault_active_{name}" for name in FAULT_CLASS_NAMES]

# ── Evidence-channel feature names (Section 6.3) ────────────────────────
# EVIDENCE_CHANNEL_NAMES comes straight from evidence_channels.py's own
# WEIGHTS dict, not retyped here — if that file's channel set ever changes
# (it shouldn't; it's tied to watchdog.c's own evidence math), this follows
# automatically instead of silently drifting out of sync.
EVIDENCE_CHANNEL_NAMES = list(WEIGHTS.keys())
EVIDENCE_FEATURE_COLUMNS = [f"evidence_{ch}" for ch in EVIDENCE_CHANNEL_NAMES] + [
    "confirmed_mobility_impairment"
]


def downcast_numeric_columns(frame):
    """Shrinks every int/float column to the smallest type that still
    exactly holds every value already present in it. Pure memory
    optimization -- never changes any value, since pandas checks the
    real min/max of each column before choosing a smaller type."""
    for col in frame.columns:
        if frame[col].dtype == "int64":
            frame[col] = pd.to_numeric(frame[col], downcast="integer")
        elif frame[col].dtype == "float64":
            frame[col] = pd.to_numeric(frame[col], downcast="float")
    return frame


# ── Load ──────────────────────────────────────────────────────────────
# CSV_PATH is a placeholder, not yet pointed at a real generated dataset.
# All simulator-side bugs that previously invalidated every prior full
# regeneration (Priority 1's lost-path freeze, the stamina stuck-loop, the
# team_set_available() row-inflation bug) are confirmed fixed as of the
# current codebase — see final_codebase_status_and_next_actions.md. Point
# this at the actual generated CSV once it exists; every printed number
# below is only meaningful once that swap happens.
CSV_PATH = "simulation_output_300_seeds.csv"  # PLACEHOLDER — update once the real dataset is generated

df = pd.read_csv(CSV_PATH)
df = downcast_numeric_columns(df)
df["_row_seq"] = np.arange(len(df), dtype=np.int32)
df = df.sort_values(["run_id", "team_id", "cycle", "_row_seq"])
df = df.drop_duplicates(subset=["run_id", "team_id", "cycle"], keep="last")
gc.collect()  # MEMORY FIX: dedup drops ~36% of rows; free the now-garbage
              # pre-dedup allocation before the (heavier) evidence-channel
              # merge below runs. No change to the dedup logic itself.
# NOTE: _row_seq is NOT dropped here (it used to be, right after this dedup).
# reconstruct_full_timeline() (imported from evidence_channels.py, used in
# the evidence-channel merge below) expects a "_row_seq" column to exist on
# whatever team-level slice it's handed, since it does its own internal
# sort/dedup safety net. It's dropped further down, once the evidence merge
# is done and nothing needs it anymore.
seeds = sorted(df["world_seed"].unique())
print("Number of distinct seeds:", len(seeds))
print("Min seed:", seeds[0], "Max seed:", seeds[-1])
print("Contiguous (max-min+1 == count)?", seeds[-1] - seeds[0] + 1 == len(seeds))

max_cycle_per_run = df.groupby("run_id")["cycle"].max()
print("\nticks_per_seed candidates (max cycle per run):")
print(max_cycle_per_run.value_counts().head(10))

print(list(df.columns))

# NOTE — diagnostic only, not the training target. watchdog_anomaly_detected
# and watchdog_failure_mode are the watchdog's OWN live output. Printed here
# purely for exploratory context (and because evaluate.py will eventually
# want the watchdog's own detections as a comparison baseline / lead-time
# reference) — never used to build y. See the label-construction block below
# for why.
anomaly_rows = df[df["watchdog_anomaly_detected"] == 1]
print(anomaly_rows["watchdog_failure_mode"].value_counts())
del anomaly_rows  # MEMORY FIX: this was a second multi-million-row frame that
                  # previously lived, unused, for the rest of the entire script
                  # after this one diagnostic print — no functional change,
                  # just freeing something that was never needed again.
gc.collect()
print("Shape:", df.shape)
print("\nwatchdog_anomaly_detected (reference only, not the label):")
print(df["watchdog_anomaly_detected"].value_counts())
print("watchdog_failure_mode" in df.columns)
print(df["watchdog_failure_mode"].value_counts() if "watchdog_failure_mode" in df.columns else "not present")

# ── Label construction (Section 6.2, v4 — ground truth, multi-label) ──
# Read straight from fault_injector.c's ground truth, one binary column
# per fault class. Deliberately NOT derived from watchdog_anomaly_detected
# or watchdog_failure_mode anywhere in this pipeline: training toward the
# watchdog's own live output would just reproduce the "present-tick
# detector" design that was already ruled out (the watchdog already
# computes that for free, on the same hardware — a model copying it can't
# structurally beat it). Ground truth is information the watchdog cannot
# see at all, which is the entire point of this pivot. Don't change this
# without re-reading Section 4 of the mega-context first.
missing_label_cols = [c for c in FAULT_LABEL_COLUMNS if c not in df.columns]
if missing_label_cols:
    raise RuntimeError(
        f"Expected ground-truth columns missing from CSV: {missing_label_cols}. "
        "Label construction assumes the v3+ (post-dedup) schema — check the CSV "
        "actually being loaded matches that."
    )

y_multi = df[FAULT_LABEL_COLUMNS].astype(np.uint8)
y_multi.columns = FAULT_CLASS_NAMES  # short names for readability downstream

# Sanity check — these should already be clean 0/1, but this is cheap
# insurance against silently training on garbage if a future CSV regen
# ever changes that.
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

# ── Evidence-channel + confirmed_mobility_impairment merge (Section 6.3) ──
# Wired in using evidence_channels.py's own reconstruct_full_timeline()/
# recompute_team_timeline() functions directly (imported above) rather than
# re-deriving the rise/decay math a second time in this file — the two
# files would drift out of sync otherwise.
#
# MUST run before the one-hot encoding block further down: recompute_team_timeline()
# reads status/twin_expected_operating_state as their original numeric codes
# (STATUS_AVAILABLE=0, etc.), and one-hot encoding replaces those columns
# with dummy columns, which would break this.
#
# Implementation note, worth understanding before trusting this at full
# scale: recompute_team_timeline() iterates its input row-by-row in Python
# (`for i in range(n): row = full_timeline.iloc[i]`) — fine at a 5-seed
# smoke-test scale (~500K rows), unconfirmed at the full 733-seed row count
# (114M+). If this turns out too slow on the full dataset, the fix is
# vectorizing recompute_team_timeline() itself — it's a straightforward
# rise/decay accumulator, not inherently sequential-per-row in a way that
# resists vectorization — not something to solve here speculatively now.
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

# Defensive check — a left-merge mismatch (e.g. a gap-filled tick in the
# reconstructed timeline that doesn't correspond to a real row in df, or a
# key mismatch) would silently show up as NaNs in exactly these columns.
# Catch it here rather than downstream in scaling, where a NaN's origin is
# much harder to trace back to this block.
evidence_nan_count = df[EVIDENCE_FEATURE_COLUMNS].isna().sum().sum()
if evidence_nan_count > 0:
    raise RuntimeError(
        f"Evidence-channel merge produced {evidence_nan_count} NaN values across "
        f"{EVIDENCE_FEATURE_COLUMNS} — likely a (run_id, team_id, cycle) mismatch "
        "between df and the reconstructed timelines. Inspect before proceeding."
    )

df = df.drop(columns=["_row_seq"])  # no longer needed past this point

print("\nEvidence-channel features merged in:", EVIDENCE_FEATURE_COLUMNS)
print(df[EVIDENCE_FEATURE_COLUMNS].describe())

# ── Bitmask expansion ────────────────────────────────────────────────
# Bit order confirmed directly against core/ (equipment_flags/hazard_flags
# bit assignments) — not an assumed ordering.
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
# "assigned_incident" is deliberately NOT encoded. Confirmed directly
# against core/mission_link.h: this column carries assigned_incident_id,
# a plain incident-identifier number (MISSION_LINK_NO_INCIDENT is its idle
# sentinel) — an arbitrary ID with no ordinal or categorical meaning, not a
# predictive feature. Left out entirely rather than encoded-and-unused.
#
# This must run AFTER the evidence-channel merge above (it consumes status/
# twin_expected_operating_state in their original numeric form).
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
gc.collect()  # MEMORY FIX: get_dummies just replaced df with a wider frame;
              # free the old one now rather than waiting for the next gc pass.

# ── twin_remaining_path_cost: sentinel flag (Section 1, part 1) ────────
df["path_is_unreachable"] = (df["twin_remaining_path_cost"] == 65535).astype(int)

# ── movement-stall feature: catches short mobility/environment faults ──
# that whole-mission-ratio features (twin_progress_error) dilute away.
# See project notes on the mobility-detection investigation.
df["moved"] = (
    df.groupby(["run_id", "team_id"])[["loc_x", "loc_y"]].diff().abs().sum(axis=1) > 0
)
df["moved"] = df["moved"] | ((df["assigned_incident"] == -1) & (df["twin_remaining_path_cost"] == 0))
df["stall_frac_last_10"] = (
    (~df["moved"]).astype(int)
    .groupby([df["run_id"], df["team_id"]])
    .transform(lambda s: s.rolling(10, min_periods=1).mean())
)

# Shorter-window twin of stall_frac_last_10 — same rolling-stall mechanism,
# narrower window (5 ticks instead of 10). Added specifically to catch
# short-duration mobility faults that the 10-tick window dilutes toward
# invisibility (a fault lasting only a few ticks barely moves a 10-tick
# rolling mean, but moves a 5-tick one much more). Uses the same "moved"
# column already computed above — no new leakage/contamination risk beyond
# what stall_frac_last_10 already carries.
df["stall_frac_last_5"] = (
    (~df["moved"]).astype(int)
    .groupby([df["run_id"], df["team_id"]])
    .transform(lambda s: s.rolling(5, min_periods=1).mean())
)

#CHECK
mask_active = (df["fault_active_mobility"] == 1) & (df["status_1"] == 1)   # status_1 = EN_ROUTE
mask_inactive = (df["fault_active_mobility"] == 0) & (df["status_1"] == 1)
print("active:", df.loc[mask_active, "stall_frac_last_10"].mean())
print("inactive:", df.loc[mask_inactive, "stall_frac_last_10"].mean())
print("stall_frac_last_5 active:", df.loc[mask_active, "stall_frac_last_5"].mean())
print("stall_frac_last_5 inactive:", df.loc[mask_inactive, "stall_frac_last_5"].mean())

# ── Mission-fault delta features ────────────────────────────────────
# fault_injector.c stores a true pre-fault baseline per incident
# (task->mission_fault_original_priority / _victim_count) at the moment a
# mission fault first lands, and restores from it on recovery — but these
# two fields are never written to the CSV by laptop/snapshot.c (confirmed
# directly against that file), so they can't be read back directly.
#
# Reconstructed here instead, from data already in the CSV: incidents are
# placed once at world-gen and never spawn mid-run (EVENT_NEW_INCIDENT
# never fires — confirmed against the C source), so the earliest logged
# row for a given (run_id, assigned_incident) is that incident's true,
# pre-fault state — the same value the C-side field would hold. This lets
# us compute the fault-caused *change* in priority/victim_count instead of
# their noisy absolute values, which vary a lot incident-to-incident for
# reasons unrelated to any fault.
#
# assigned_incident == -1 (team not currently working any incident) is
# excluded from the baseline lookup — there's no incident to compute a
# baseline for. Delta is filled to 0 for those rows: no incident, no delta.
#
# Known, accepted limitation (not fixed here): incident priority can also
# be raised by escalate_starved_incidents(), a mechanism unrelated to
# mission faults — so this delta isn't a perfectly clean causal signal,
# just a cleaner one than the raw absolute values.
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
del baseline_lookup
gc.collect()

# Signed cast before subtracting — incident_priority/victim_count were
# downcast to small unsigned types earlier (downcast_numeric_columns()),
# and an unsigned subtraction underflows/wraps instead of going negative.
# In principle these deltas should stay >= 0 given how the fault mechanism
# works (priority/victim_count only ever move toward worse, then get
# restored on recovery), but casting signed here costs nothing and removes
# any risk of a silent wraparound bug if that assumption is ever wrong.
df["delta_priority"] = (
    df["incident_priority"].astype(np.int16) - df["baseline_incident_priority"]
)
df["delta_victim_count"] = (
    df["victim_count"].astype(np.int16) - df["baseline_victim_count"]
)
df["delta_priority"] = df["delta_priority"].fillna(0)
df["delta_victim_count"] = df["delta_victim_count"].fillna(0)
df = df.drop(columns=["baseline_incident_priority", "baseline_victim_count"])

#CHECK — delta features vs. the raw values they're derived from, printed
# side by side so the active-vs-inactive separation can be compared
# directly without needing to re-run anything.
mask_active_mission = df["fault_active_mission"] == 1
mask_inactive_mission = df["fault_active_mission"] == 0
print("delta_priority active:", df.loc[mask_active_mission, "delta_priority"].mean())
print("delta_priority inactive:", df.loc[mask_inactive_mission, "delta_priority"].mean())
print("delta_victim_count active:", df.loc[mask_active_mission, "delta_victim_count"].mean())
print("delta_victim_count inactive:", df.loc[mask_inactive_mission, "delta_victim_count"].mean())
print("raw incident_priority active:", df.loc[mask_active_mission, "incident_priority"].mean())
print("raw incident_priority inactive:", df.loc[mask_inactive_mission, "incident_priority"].mean())
print("raw victim_count active:", df.loc[mask_active_mission, "victim_count"].mean())
print("raw victim_count inactive:", df.loc[mask_inactive_mission, "victim_count"].mean())

# ── Final feature selection ─────────────────────────────────────────
# Raw telemetry + the 7 evidence-channel features + confirmed_mobility_impairment,
# combined (Section 6.3) — evidence_features is included below. This was the
# one item the original roadmap listed as "not yet done"; it's done here,
# wired against evidence_channels.py's own recomputation logic rather than
# duplicated.
numerical_features = [
    "incident_priority", "victim_count", "hours_worked", "stamina",
    "twin_expected_progress", "twin_actual_progress", "twin_progress_error",
    "twin_expected_stamina", "twin_actual_stamina", "twin_stamina_error",
    "twin_remaining_path_cost", "twin_expected_work_remaining",
    "twin_mission_stage", "twin_communication_health", "equipment_level", "team_zone_trust","stall_frac_last_10",
    "stall_frac_last_5", "delta_priority", "delta_victim_count",
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

# The 7 continuous evidence-channel scores + confirmed_mobility_impairment,
# computed by the merge block above.
evidence_features = EVIDENCE_FEATURE_COLUMNS

# path_is_unreachable is intentionally NOT added here yet — added further down,
# after the group split, once the sentinel mean-replacement is done.
feature_columns = numerical_features + one_hot_features + bitmask_features + evidence_features

# Defensive check — the single most load-bearing constraint in the v4 design
# (Section 6.2) is that ground truth never leaks into the model's inputs.
# feature_columns is hand-built above, so this should be impossible by
# construction, but asserting it costs nothing and catches a future
# copy-paste mistake immediately instead of silently training on leaked labels.
leaked = set(feature_columns) & set(FAULT_LABEL_COLUMNS)
assert not leaked, f"Ground-truth label column(s) leaked into features: {leaked}"

X = df[feature_columns]

# Feature count (before path_is_unreachable) is not hardcoded anywhere
# downstream (e.g. model.py's input_dim) — read off X.shape[1] at build
# time instead, since support_team/team_zone_id's one-hot cardinality is an
# ordinary function of what actually shows up in whatever sample is loaded
# (status/specialization/fitness_tier are small, fixed C enums and will
# always appear regardless of sample; support_team/team_zone_id are not
# guaranteed to hit every possible value in a partial-seed sample).
print("\nNumber of features (before path_is_unreachable):", X.shape[1])
print("Missing values:\n", X.isna().sum().sum(), "total NaNs")


# ── Group-based split (Section 2) ───────────────────────────────────
def group_split(df, y_balance, group_col="run_id", test_size=0.15, val_size=0.15, seed=42, max_tries=20):
    """
    Splits by whole run_id groups so no run appears in more than one split.
    Retries with new seeds if the resulting positive-rate per split drifts
    too far from the overall rate (checked via a simple tolerance).

    y_balance: a single scalar-per-row Series used only to judge whether a
    candidate split is acceptably balanced. For v4 this is the "any fault
    active" indicator (max across the 5 ground-truth classes). NOTE: this
    only guarantees the *overall* any-fault rate stays balanced across
    splits — it does not separately guarantee each of the 5 individual
    fault classes lands evenly (rarer classes, e.g. MISSION, could still
    drift more per split). Per-class rates are printed after the split so
    you can eyeball this; tightening the check to require all 5 classes
    within tolerance simultaneously was not done here since it risks
    exhausting max_tries given how different the 5 classes' base rates are.

    test_size/val_size/tolerance (0.25, below)/max_tries are chosen
    defaults, not empirically derived from this project's actual data —
    reasonable starting points, worth revisiting once real per-class rates
    are known from a clean full-scale CSV.
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


y_balance = y_multi.max(axis=1)  # "any fault active" — see group_split docstring
train_idx, val_idx, test_idx = group_split(df, y_balance, group_col="run_id")

# ── Save test-set identifiers for the lead-time metric (evaluate.py) ────
# NOT part of feature_columns / X -- a parallel lookup table only, so
# evaluate.py can trace a test row back to (run_id, team_id, cycle) and
# the real watchdog's own detection flag, after the model has already
# made its prediction. Same test_idx as X_test/y_test below, so row
# order is guaranteed aligned -- nothing here ever touches feature_columns.
test_metadata = df.loc[test_idx, ["run_id", "team_id", "cycle", "watchdog_anomaly_detected"]].reset_index(drop=True)
np.save(f"{OUTPUT_DIR}/test_run_id.npy", test_metadata["run_id"].to_numpy())
np.save(f"{OUTPUT_DIR}/test_team_id.npy", test_metadata["team_id"].to_numpy())
np.save(f"{OUTPUT_DIR}/test_cycle.npy", test_metadata["cycle"].to_numpy())
np.save(f"{OUTPUT_DIR}/test_watchdog_anomaly_detected.npy", test_metadata["watchdog_anomaly_detected"].to_numpy())

print("\nPer-class positive rate by split (informational — not enforced):")
for split_name, idx in [("train", train_idx), ("val", val_idx), ("test", test_idx)]:
    print(f"  {split_name}:")
    print(y_multi.iloc[idx].mean().to_string())

# ── twin_remaining_path_cost: sentinel replacement (Section 1, part 2) ─
# Must happen AFTER the split — uses only training rows to avoid leakage.
#
# The column is heavily zero-inflated: median cost among real (non-sentinel)
# training rows is 0.00, because a team that has already arrived shows 0
# remaining cost for every subsequent tick. Imputing the sentinel with a
# plain mean/median over all real rows would therefore be dominated by the
# zero-inflated "arrived" cluster and badly understate what "unreachable"
# should look like. Fix: compute the replacement value from
# actively-traveling rows only (nonzero, non-sentinel) — a closer
# conceptual match, since an unreachable path is a more extreme version of
# "still traveling / something's going on" than of "already arrived." This
# reasoning is a property of the raw feature's own distribution and doesn't
# depend on which target is in use.
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


# Now add path_is_unreachable to the feature set and rebuild X
feature_columns = feature_columns + ["path_is_unreachable"]
X = df[feature_columns]
X = downcast_numeric_columns(X)  # covers bitmask/one-hot/path_is_unreachable/evidence cols
print("Number of features (with path_is_unreachable):", X.shape[1])
del df
gc.collect()

# ── Scaling (Section 3) ──────────────────────────────────────────────
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

# Train — fit_transform must go first, since this is what defines the
# scaler's mean_/std_. Same order as the original script.
X_train, y_train = X.iloc[train_idx], y_multi.iloc[train_idx]
X_train_scaled = scaler.fit_transform(X_train)
nan_flags["train"] = np.isnan(X_train_scaled).any()
np.save(f"{OUTPUT_DIR}/X_train_scaled.npy", X_train_scaled)
np.save(f"{OUTPUT_DIR}/y_train.npy", y_train.values)  # shape (n, 5)
del X_train, X_train_scaled, y_train
gc.collect()

# Val — transform only (reuses the scaler already fit on train)
X_val, y_val = X.iloc[val_idx], y_multi.iloc[val_idx]
X_val_scaled = scaler.transform(X_val)
nan_flags["val"] = np.isnan(X_val_scaled).any()
np.save(f"{OUTPUT_DIR}/X_val_scaled.npy", X_val_scaled)
np.save(f"{OUTPUT_DIR}/y_val.npy", y_val.values)
del X_val, X_val_scaled, y_val
gc.collect()

# Test — transform only (same scaler, still untouched since fit)
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

# y_train/y_val/y_test.npy are (n, 5) matrices with no column headers of
# their own — save the class-name order alongside so model.py/train.py/
# evaluate.py can always map output neuron index -> fault class name
# without re-deriving or guessing the order.
with open(f"{OUTPUT_DIR}/fault_class_names.pkl", "wb") as f:
    pickle.dump(FAULT_CLASS_NAMES, f)

print(f"\nSaved all preprocessed arrays + scaler + feature_columns + "
      f"fault_class_names to ./{OUTPUT_DIR}/")
print("train.py can now load these directly instead of re-running this script.")
"""
Scenario 3: environment-class faults, end-to-end through the REAL
recovery_manager.c escalation logic (faithfully ported to Python here),
using a REAL trained classifier (same Config A pipeline as the earlier
comparative experiment) and REAL onset episodes from
tron_snapshot_dataset.csv - not synthetic confidence sequences.

Question: not just F1, but does ANY recovery action ever fire during a
real environment-fault episode, and does it ever ESCALATE past
LOCAL_RECOVERY (which needs corroboration - watchdog agreement or an
8-tick persistence streak, per core/recovery_manager.c:1279)?

Persistence/hysteresis constants and stage tables are copied exactly
from core/recovery_manager_config.h and core/recovery_manager.c's
recovery_action_persistence()/recovery_stage_action_floor() - see
those files for the authoritative source; this is a port for offline
analysis, not a claim that this file is itself part of the build.
"""
import os
import sys
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO_ROOT)

import numpy as np
import pandas as pd
from sklearn.model_selection import GroupShuffleSplit
from sklearn.ensemble import HistGradientBoostingClassifier

from evidence_channels import WEIGHTS

CSV_PATH = os.path.join(REPO_ROOT, "tron_snapshot_dataset.csv")

FAULT_CLASS_NAMES = ["mobility", "communication", "resource", "environment", "mission"]
FAULT_LABEL_COLUMNS = [f"fault_active_{name}" for name in FAULT_CLASS_NAMES]
EVIDENCE_CHANNEL_NAMES = list(WEIGHTS.keys())
EVIDENCE_FEATURE_COLUMNS = [f"evidence_{ch}" for ch in EVIDENCE_CHANNEL_NAMES] + ["confirmed_mobility_impairment"]

# ── Escalation-logic constants, copied exactly from core/recovery_manager_config.h ──
FAULT_PERSISTENCE_FLOOR = 0.20
FAULT_PERSISTENCE_TRIP_TICKS = 8
RECOVERY_MANAGER_PREDICTOR_REALLOCATE_CONFIDENCE = 0.50
RECOVERY_MANAGER_PREDICTOR_ESCALATE_CONFIDENCE = 0.80
RECOVERY_MANAGER_ACTION_FLOOR_LOCAL = 0.24
RECOVERY_MANAGER_ACTION_FLOOR_RETURN = 0.36
RECOVERY_MANAGER_ACTION_FLOOR_REALLOC = 0.48
RECOVERY_MANAGER_ACTION_FLOOR_ABORT = 0.72

ACTION_NONE, ACTION_LOCAL, ACTION_RETURN, ACTION_REALLOC, ACTION_ABORT = 0, 1, 2, 3, 4
ACTION_NAMES = ["CONTINUE_MONITORING", "LOCAL_RECOVERY", "RETURN_TO_SAFE_ZONE",
                "REQUEST_TASK_REALLOCATION", "ABORT_MISSION"]

MISSION_STAGE_IDLE, MISSION_STAGE_TRAVEL, MISSION_STAGE_WORK, MISSION_STAGE_RECOVERY = 0, 1, 2, 3

# persistence[stage][action], cooldown[stage][action] - copied from
# core/recovery_manager_config.h's per-stage *_PERSISTENCE / *_COOLDOWN macros.
PERSISTENCE = {
    MISSION_STAGE_TRAVEL:   {ACTION_LOCAL: 1, ACTION_RETURN: 2, ACTION_REALLOC: 3, ACTION_ABORT: 4},
    MISSION_STAGE_WORK:     {ACTION_LOCAL: 1, ACTION_RETURN: 2, ACTION_REALLOC: 2, ACTION_ABORT: 4},
    MISSION_STAGE_RECOVERY: {ACTION_LOCAL: 1, ACTION_RETURN: 1, ACTION_REALLOC: 2, ACTION_ABORT: 3},
    MISSION_STAGE_IDLE:     {ACTION_LOCAL: 1, ACTION_RETURN: 1, ACTION_REALLOC: 2, ACTION_ABORT: 3},
}


def action_floor(action):
    return {ACTION_LOCAL: RECOVERY_MANAGER_ACTION_FLOOR_LOCAL,
            ACTION_RETURN: RECOVERY_MANAGER_ACTION_FLOOR_RETURN,
            ACTION_REALLOC: RECOVERY_MANAGER_ACTION_FLOOR_REALLOC,
            ACTION_ABORT: RECOVERY_MANAGER_ACTION_FLOOR_ABORT}.get(action, 0.0)


def downgrade_to_achievable(desired_action, confidence):
    action = desired_action
    while action > ACTION_NONE and confidence < action_floor(action):
        action -= 1
    return action


def evaluate_fault_predictor_candidate(confidence, watchdog_anomaly_detected, sustained):
    """Ports core/recovery_manager.c:1269-1311 exactly (evaluate_fault_predictor's
    candidate-signal computation) - degraded-team ABORT branch omitted since this
    analysis treats every episode as an ordinary (non-degraded) EN_ROUTE/ON_SCENE
    team, matching this dataset's typical onset context."""
    if confidence <= 0.0:
        return ACTION_NONE
    if confidence < RECOVERY_MANAGER_ACTION_FLOOR_LOCAL:
        return ACTION_NONE
    corroborated = watchdog_anomaly_detected or sustained
    if corroborated and confidence >= RECOVERY_MANAGER_PREDICTOR_ESCALATE_CONFIDENCE:
        return downgrade_to_achievable(ACTION_RETURN, confidence)
    if corroborated and confidence >= RECOVERY_MANAGER_PREDICTOR_REALLOCATE_CONFIDENCE:
        return downgrade_to_achievable(ACTION_REALLOC, confidence)
    return ACTION_LOCAL


def run_episode_through_hysteresis(confidences, watchdog_flags, stages):
    """Faithful port of predictor_persistence_streak_update() +
    recovery_apply_hysteresis()'s escalation-only path (core/recovery_manager.c
    :1163-1176, :1347-1435), isolated exactly as the C stress-test harness
    isolated it (fault-predictor signal only, other evaluators quiescent).
    Returns (highest_action_reached, active_action_trajectory)."""
    streak = 0
    active_action = ACTION_NONE
    pending_action = ACTION_NONE
    pending_streak = 0
    trajectory = []
    highest = ACTION_NONE

    for confidence, wd, stage in zip(confidences, watchdog_flags, stages):
        streak = streak + 1 if confidence >= FAULT_PERSISTENCE_FLOOR else 0
        sustained = streak >= FAULT_PERSISTENCE_TRIP_TICKS
        candidate = evaluate_fault_predictor_candidate(confidence, wd, sustained)

        if candidate == active_action:
            pending_action = candidate
            pending_streak = 0
        elif candidate > active_action:
            if pending_action != candidate:
                pending_action = candidate
                pending_streak = 1
            else:
                pending_streak += 1
            needed = PERSISTENCE.get(stage, PERSISTENCE[MISSION_STAGE_IDLE]).get(candidate, 1)
            if pending_streak >= needed:
                active_action = active_action + 1  # one level per trip, matching the C code exactly
                pending_streak = 0
        # de-escalation path omitted: within one short onset episode analyzed
        # in isolation, de-escalation only matters after the fault clears,
        # which is out of scope for "does escalation ever fire during this
        # episode" - the highest-water-mark below is unaffected by it.

        trajectory.append(active_action)
        highest = max(highest, active_action)

    return highest, trajectory


def downcast_numeric_columns(frame):
    for col in frame.columns:
        if frame[col].dtype == "int64":
            frame[col] = pd.to_numeric(frame[col], downcast="integer")
        elif frame[col].dtype == "float64":
            frame[col] = pd.to_numeric(frame[col], downcast="float")
    return frame


print("Loading + dedup...")
df = pd.read_csv(CSV_PATH)
df = downcast_numeric_columns(df)
df["_row_seq"] = np.arange(len(df), dtype=np.int32)
df = df.sort_values(["run_id", "team_id", "cycle", "_row_seq"])
df = df.drop_duplicates(subset=["run_id", "team_id", "cycle"], keep="last")

# Dataset precondition, checked before the slow reconstruction/training
# below: group_split() needs at least 3 distinct runs to form non-empty
# train/val/test groups, and the analysis needs real environment-fault
# rows. A plain `make smoke` dataset (1 run, no faults) used to surface
# here only as an opaque sklearn ValueError from GroupShuffleSplit.
_missing_labels = [c for c in FAULT_LABEL_COLUMNS if c not in df.columns]
_n_runs = df["run_id"].nunique()
_n_env_fault_rows = 0 if _missing_labels else int(df["fault_active_environment"].sum())
if _missing_labels or _n_runs < 3 or _n_env_fault_rows == 0:
    sys.stdout.flush()
    print(f"ERROR: {CSV_PATH} does not meet this analysis's precondition "
          f"(runs={_n_runs}, need >= 3; environment-fault rows={_n_env_fault_rows}, need > 0"
          + (f"; missing columns {_missing_labels}" if _missing_labels else "") + ").\n"
          "Regenerate it with fault injection, e.g.\n"
          "  ./tron 1 30 400 --fault-profile=combined",
          file=sys.stderr)
    sys.exit(1)

y_multi_full = df[FAULT_LABEL_COLUMNS].astype(np.uint8)
y_multi_full.columns = FAULT_CLASS_NAMES

# Feature-contract Decision 2: the evidence-channel columns come straight
# from the C watchdog now (laptop/snapshot.c emits watchdog_get_evidence_
# vector() directly), not from evidence_channels.py's Python reconstruction
# - train == serve, no mirror-drift residual. Re-deriving them here via
# reconstruct_full_timeline()/recompute_team_timeline() and merging back in,
# as this script used to, would silently collide with the CSV's own
# evidence_progress/etc. columns (pandas suffixes both _x/_y on merge,
# and neither name survives), so that merge is gone; the columns are simply
# already in df, read straight off the CSV like everything else.
_missing_evidence = [c for c in EVIDENCE_FEATURE_COLUMNS if c not in df.columns]
if _missing_evidence:
    sys.stdout.flush()
    print(f"ERROR: {CSV_PATH} is missing evidence columns {_missing_evidence} - "
          "regenerate with a build that includes feature-contract Decision 2 "
          "(laptop/snapshot.c's write_evidence_columns()).", file=sys.stderr)
    sys.exit(1)
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
heldout_idx = np.concatenate([val_idx, test_idx])  # more onset episodes for this analysis

X = df[feature_columns].astype(np.float32)
print("Training environment-class classifier...")
clf = HistGradientBoostingClassifier(max_iter=150, max_depth=6, learning_rate=0.1,
                                       random_state=42, class_weight="balanced")
clf.fit(X.iloc[train_idx], y_multi["environment"].iloc[train_idx])
proba = clf.predict_proba(X.iloc[heldout_idx])[:, 1]

heldout = df.iloc[heldout_idx].copy()
heldout["pred_proba_environment"] = proba
heldout["watchdog_anomaly_detected"] = heldout["watchdog_anomaly_detected"].astype(int)
heldout = heldout.sort_values(["run_id", "team_id", "cycle"])

print(f"\nHeld-out rows: {len(heldout)}  (val+test runs, never seen in training)")
print(f"Positive (environment-fault-active) rows in held-out set: {int(heldout['fault_active_environment'].sum())}")

print("\nIdentifying contiguous environment-fault onset episodes...")
episodes = []
for (run_id, team_id), g in heldout.groupby(["run_id", "team_id"]):
    g = g.sort_values("cycle")
    active = g["fault_active_environment"].to_numpy()
    proba_seq = g["pred_proba_environment"].to_numpy()
    wd_seq = g["watchdog_anomaly_detected"].to_numpy()
    stage_seq = g["twin_mission_stage"].to_numpy().astype(int)
    # contiguous runs of active==1
    change = np.diff(np.concatenate([[0], active, [0]]))
    starts = np.where(change == 1)[0]
    ends = np.where(change == -1)[0]  # exclusive
    for s, e in zip(starts, ends):
        episodes.append(dict(
            run_id=run_id, team_id=team_id, start=s, end=e, length=e - s,
            proba=proba_seq[s:e], wd=wd_seq[s:e], stage=stage_seq[s:e],
        ))

print(f"Total environment-fault onset episodes in held-out set: {len(episodes)}")
lengths = [ep["length"] for ep in episodes]
print(f"Episode length distribution: min={min(lengths)} median={int(np.median(lengths))} "
      f"mean={np.mean(lengths):.1f} max={max(lengths)}")

print("\nRunning each episode through the ported escalation state machine "
      "(cold-start persistence/hysteresis state at episode onset)...")
never_any_action = 0
capped_at_local = 0
reached_return_plus = 0
reached_realloc_plus = 0
reached_abort = 0

for ep in episodes:
    highest, traj = run_episode_through_hysteresis(ep["proba"], ep["wd"], ep["stage"])
    if highest == ACTION_NONE:
        never_any_action += 1
    elif highest == ACTION_LOCAL:
        capped_at_local += 1
    else:
        reached_return_plus += 1
        if highest >= ACTION_REALLOC:
            reached_realloc_plus += 1
        if highest == ACTION_ABORT:
            reached_abort += 1

n = len(episodes)
print(f"\n=== Scenario 3 results: {n} real environment-fault onset episodes, held-out data ===")
print(f"  Never triggers ANY action (stuck at CONTINUE_MONITORING the whole episode): "
      f"{never_any_action} ({never_any_action/n:.1%})")
print(f"  Triggers LOCAL_RECOVERY but never escalates further:                        "
      f"{capped_at_local} ({capped_at_local/n:.1%})")
print(f"  Escalates to RETURN_TO_SAFE_ZONE or above (genuine corroborated escalation): "
      f"{reached_return_plus} ({reached_return_plus/n:.1%})")
print(f"    of which reach REQUEST_TASK_REALLOCATION or above: {reached_realloc_plus} ({reached_realloc_plus/n:.1%})")
print(f"    of which reach ABORT_MISSION:                       {reached_abort} ({reached_abort/n:.1%})")
print(f"\n  Combined 'no meaningful safety action ever' (never_any_action + capped_at_local): "
      f"{never_any_action + capped_at_local} ({(never_any_action + capped_at_local)/n:.1%})")

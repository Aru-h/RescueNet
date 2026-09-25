"""
evidence_channels.py — recomputes the watchdog's 7 evidence channels from raw
per-tick simulator telemetry.

The real watchdog (core/watchdog.c) maintains 7 independent evidence channels
(progress, stamina, communication, operating_state, path, assignment, timeout),
each a smoothed 0-1 accumulator built from a per-tick instantaneous signal via
a shared rise/decay rule, and combines them into a single weighted anomaly
confidence score. This module reconstructs that same computation in Python
from the logged CSV, so that:

  1. The reconstruction can be validated against the watchdog's own real,
     logged output (an accuracy/agreement check), and
  2. The 7 recomputed channels, plus a derived confirmed_mobility_impairment
     flag, can be used as model input features (see data_prep.py).

All constants below are copied exactly from core/watchdog.c and must be kept
in sync with it. In particular, ANOMALY_THRESHOLD must match
WATCHDOG_ANOMALY_THRESHOLD (core/watchdog.c) exactly - re-check it any time
that constant changes there.
"""

import pandas as pd
import numpy as np

# ── Constants, mirrored exactly from core/watchdog.c ────────────────────
RISE = 0.35
DECAY = 0.80

PROGRESS_FULL_SCALE = 0.35
STAMINA_FULL_SCALE = 18.0
LIGHT_MIN_STAMINA = 40.0

WEIGHTS = {
    "progress": 0.2222,
    "stamina": 0.1667,
    "communication": 0.2037,
    "operating_state": 0.1296,
    "path": 0.1481,
    "assignment": 0.0556,
    "timeout": 0.0741,
}

ANOMALY_THRESHOLD = 0.10
STAGNATION_STREAK_MAX = 5
TIMEOUT_STREAK_MAX = 6
MOBILITY_CONFIRM_TICKS = 3
MOBILITY_PROGRESS_MARGIN = 0.02
# Stall-based mobility detection (update_movement_stalls() in watchdog.c).
STALL_WINDOW_TICKS = 4
STALL_CONFIRM_COUNT = 2

STATUS_AVAILABLE = 0
STATUS_EN_ROUTE = 1
STATUS_ON_SCENE = 2
STATUS_RESTING = 3

MISSION_STAGE_IDLE = 0
MISSION_STAGE_TRAVEL = 1
MISSION_STAGE_WORK = 2
MISSION_STAGE_RECOVERY = 3

TASK_POOL_NONE = 0
TASK_POOL_INCIDENT = 1
TASK_POOL_SYSTEM = 2
NO_ASSIGNMENT_SLOT = 255

RAW_COLUMNS = [
    "twin_progress_error", "twin_stamina_error", "stamina",
    "twin_communication_health", "status", "twin_expected_operating_state",
    "twin_mission_stage", "twin_assigned_task_pool", "twin_assigned_task_slot",
    "twin_remaining_path_cost", "twin_actual_progress", "twin_expected_progress",
    "twin_expected_arrival_tick", "loc_x", "loc_y",
]

DEFAULTS = {
    "twin_progress_error": 0.0,
    "twin_stamina_error": 0.0,
    "stamina": 100.0,
    "twin_communication_health": 3.0,
    "status": STATUS_AVAILABLE,
    "twin_expected_operating_state": STATUS_AVAILABLE,
    "twin_mission_stage": MISSION_STAGE_IDLE,
    "twin_assigned_task_pool": TASK_POOL_NONE,
    "twin_assigned_task_slot": NO_ASSIGNMENT_SLOT,
    "twin_remaining_path_cost": 0,
    "twin_actual_progress": 0.0,
    "twin_expected_progress": 0.0,
    "twin_expected_arrival_tick": 0,
    "loc_x": 0,
    "loc_y": 0,
}

FLAG_BITS = {
    "progress": 1, "stamina": 2, "communication": 4,
    "operating_state": 8, "path": 16, "assignment": 32, "timeout": 64,
}


def clamp01(x):
    return max(0.0, min(1.0, x))


def accumulate(previous, instantaneous):
    """The shared rise/decay accumulator rule every channel uses: rises a
    fraction of the remaining gap toward a high instantaneous reading, and
    decays multiplicatively otherwise."""
    instantaneous = clamp01(instantaneous)
    if instantaneous > previous:
        return previous + RISE * (instantaneous - previous)
    return previous * DECAY


def reconstruct_full_timeline(team_rows, run_max_cycle):
    """Builds the full 0..run_max_cycle per-tick timeline for one (run_id,
    team_id). Deduplicates any tick with more than one logged row using true
    file order (the _row_seq column, expected to be present on the input
    frame), keeping the last-written row for that tick. Any tick with no
    logged row at all is forward-filled from the most recent prior row.

    Returns (full_timeline, n_gap_ticks), where n_gap_ticks is the number of
    ticks that had no logged row and had to be forward-filled.
    """
    team_rows = team_rows.sort_values(["cycle", "_row_seq"]).drop_duplicates(subset="cycle", keep="last")

    team_rows = team_rows.set_index("cycle")
    full_index = pd.RangeIndex(0, run_max_cycle + 1, name="cycle")

    was_logged = pd.Series(False, index=full_index)
    was_logged.loc[team_rows.index] = True
    n_gap_ticks = int((~was_logged).sum())

    reindexed = team_rows.reindex(full_index)
    reindexed[RAW_COLUMNS] = reindexed[RAW_COLUMNS].ffill()

    for col, default in DEFAULTS.items():
        reindexed[col] = reindexed[col].fillna(default)

    reindexed["was_logged"] = was_logged
    reindexed = reindexed.reset_index()
    return reindexed, n_gap_ticks


def recompute_team_timeline(full_timeline):
    """Computes all 7 evidence channels, the combined anomaly confidence, and
    confirmed_mobility_impairment, tick by tick, for one team's full
    timeline. Mirrors core/watchdog.c's per-tick evidence update exactly.

    Adds these columns to full_timeline and returns it:
      - evidence_{channel}: the smoothed 0-1 accumulator value per channel
        (used as a model input feature)
      - recomputed_{channel}_flag: the same value, thresholded to 0/1 (used
        for validation against the watchdog's own logged flag bits)
      - recomputed_anomaly_confidence / recomputed_anomaly_detected: the
        combined weighted score and its threshold-based detection flag
      - confirmed_mobility_impairment: 0/1, disambiguates a genuine mobility
        fault from an ENVIRONMENT-class obstruction: set by movement stalls
        while travelling (update_movement_stalls()) or by travel-time
        progress degradation that persists after arrival on scene
        (update_mobility_confirmation())
    """
    n = len(full_timeline)
    acc = {ch: 0.0 for ch in WEIGHTS}
    out_confidence = np.zeros(n)
    out_channel_values = {ch: np.zeros(n) for ch in WEIGHTS}
    out_confirmed_mobility = np.zeros(n, dtype=np.uint8)

    prev_status = None
    travel_degradation_seen = False
    mobility_watch_active = False
    mobility_watch_ticks = 0
    mobility_watch_baseline_error = 0.0
    confirmed_mobility_impairment = False

    # update_movement_stalls() state: newest EN_ROUTE tick in bit 0.
    stall_mask = (1 << STALL_WINDOW_TICKS) - 1
    stall_history = 0
    stall_samples = 0
    last_loc = None

    path_stagnation_streak = 0
    last_remaining_cost = None
    has_last_sample = False

    last_task_pool = None
    last_task_slot = None

    timeout_streak = 0

    for i in range(n):
        row = full_timeline.iloc[i]

        inst_progress = clamp01(abs(row["twin_progress_error"]) / PROGRESS_FULL_SCALE)

        inst_stamina = clamp01(abs(row["twin_stamina_error"]) / STAMINA_FULL_SCALE)
        if row["stamina"] <= LIGHT_MIN_STAMINA:
            inst_stamina = clamp01(inst_stamina + 0.20)

        inst_communication = clamp01(row["twin_communication_health"] / 3.0)

        inst_operating_state = 0.0 if row["status"] == row["twin_expected_operating_state"] else 1.0

        now_en_route = row["status"] == STATUS_EN_ROUTE

        # update_movement_stalls() - runs before the mobility confirmation
        # below, which is what advances prev_status.
        loc = (row["loc_x"], row["loc_y"])
        if now_en_route and prev_status == STATUS_EN_ROUTE and has_last_sample:
            stalled = 1 if loc == last_loc else 0
            stall_history = ((stall_history << 1) | stalled) & stall_mask
            if stall_samples < STALL_WINDOW_TICKS:
                stall_samples += 1
            if bin(stall_history).count("1") >= STALL_CONFIRM_COUNT:
                confirmed_mobility_impairment = True
                travel_degradation_seen = True
            elif stall_samples >= STALL_WINDOW_TICKS and stall_history == 0:
                confirmed_mobility_impairment = False
        elif not now_en_route:
            stall_history = 0
            stall_samples = 0
        last_loc = loc

        # update_mobility_confirmation()
        now_on_scene = row["status"] == STATUS_ON_SCENE
        was_en_route = prev_status == STATUS_EN_ROUTE

        if now_en_route and prev_status != STATUS_EN_ROUTE:
            travel_degradation_seen = False
        if now_en_route and row["twin_progress_error"] > MOBILITY_PROGRESS_MARGIN:
            travel_degradation_seen = True

        if was_en_route and now_on_scene:
            if travel_degradation_seen:
                mobility_watch_active = True
                mobility_watch_ticks = 0
                mobility_watch_baseline_error = row["twin_progress_error"]
            else:
                confirmed_mobility_impairment = False
                mobility_watch_active = False
            travel_degradation_seen = False
        elif mobility_watch_active and now_on_scene:
            mobility_watch_ticks += 1
            if row["twin_progress_error"] > mobility_watch_baseline_error + MOBILITY_PROGRESS_MARGIN:
                confirmed_mobility_impairment = True
                mobility_watch_active = False
            elif mobility_watch_ticks >= MOBILITY_CONFIRM_TICKS:
                confirmed_mobility_impairment = False
                mobility_watch_active = False
        elif not now_on_scene and not now_en_route:
            mobility_watch_active = False

        prev_status = row["status"]

        in_travel = row["twin_mission_stage"] == MISSION_STAGE_TRAVEL
        has_assignment = row["twin_assigned_task_pool"] != TASK_POOL_NONE and \
                          row["twin_assigned_task_slot"] != NO_ASSIGNMENT_SLOT

        # path_evidence() gates on mission_stage alone: a team heading to a
        # safe zone is in TRAVEL with no task slot and still accrues evidence.
        if not in_travel:
            path_stagnation_streak = 0
            inst_path = 0.0
        elif not has_last_sample:
            inst_path = 0.0
        elif row["twin_remaining_path_cost"] < last_remaining_cost:
            path_stagnation_streak = 0
            inst_path = 0.0
        elif row["twin_progress_error"] <= 0.0 and not confirmed_mobility_impairment:
            path_stagnation_streak = 0
            inst_path = 0.0
        else:
            if confirmed_mobility_impairment:
                path_stagnation_streak = min(path_stagnation_streak + 2, STAGNATION_STREAK_MAX)
            else:
                path_stagnation_streak = min(path_stagnation_streak + 1, STAGNATION_STREAK_MAX)
            inst_path = clamp01(path_stagnation_streak / STAGNATION_STREAK_MAX)
            if confirmed_mobility_impairment:
                inst_path = clamp01(0.35 + 0.65 * inst_path)

        last_remaining_cost = row["twin_remaining_path_cost"]

        cur_task_pool = row["twin_assigned_task_pool"]
        cur_task_slot = row["twin_assigned_task_slot"]
        if not has_last_sample:
            inst_assignment = 0.0
        elif last_task_pool == TASK_POOL_NONE or last_task_slot == NO_ASSIGNMENT_SLOT:
            inst_assignment = 0.0
        elif cur_task_pool == last_task_pool and cur_task_slot == last_task_slot:
            inst_assignment = 0.0
        elif row["twin_actual_progress"] < 0.90:
            inst_assignment = 1.0
        else:
            inst_assignment = 0.25
        last_task_pool, last_task_slot = cur_task_pool, cur_task_slot

        if not has_assignment:
            timeout_streak = 0
            inst_timeout = 0.0
        else:
            late_arrival = (row["twin_expected_operating_state"] == STATUS_ON_SCENE and
                             row["status"] != STATUS_ON_SCENE and
                             row["cycle"] > row["twin_expected_arrival_tick"])
            late_finish = (row["twin_expected_progress"] >= 0.999 and
                           row["twin_actual_progress"] + 0.05 < row["twin_expected_progress"])
            if late_arrival or late_finish:
                timeout_streak = min(timeout_streak + 1, TIMEOUT_STREAK_MAX)
            else:
                timeout_streak = 0
            inst_timeout = clamp01(timeout_streak / TIMEOUT_STREAK_MAX)

        has_last_sample = True

        instantaneous = {
            "progress": inst_progress, "stamina": inst_stamina,
            "communication": inst_communication, "operating_state": inst_operating_state,
            "path": inst_path, "assignment": inst_assignment, "timeout": inst_timeout,
        }
        for ch in WEIGHTS:
            acc[ch] = accumulate(acc[ch], instantaneous[ch])
            out_channel_values[ch][i] = acc[ch]

        out_confidence[i] = sum(WEIGHTS[ch] * acc[ch] for ch in WEIGHTS)
        out_confirmed_mobility[i] = confirmed_mobility_impairment

    full_timeline["recomputed_anomaly_confidence"] = out_confidence
    full_timeline["recomputed_anomaly_detected"] = (out_confidence >= ANOMALY_THRESHOLD).astype(int)
    for ch in WEIGHTS:
        full_timeline[f"recomputed_{ch}_flag"] = (out_channel_values[ch] >= 0.20).astype(int)
        full_timeline[f"evidence_{ch}"] = out_channel_values[ch]
    full_timeline["confirmed_mobility_impairment"] = out_confirmed_mobility
    return full_timeline


def compute_evidence_features(df, sample_seeds=None, seed_column="world_seed"):
    """Convenience entry point for data_prep.py / other callers: given a raw
    (already deduplicated, _row_seq-tagged) simulator dataframe, returns it
    with the evidence-channel and confirmed_mobility_impairment columns
    added for every (run_id, team_id) timeline present.

    If sample_seeds is given, restricts to those world_seed values first
    (useful for validation runs against a smaller sample).
    """
    if sample_seeds is not None:
        df = df[df[seed_column].isin(sample_seeds)]

    run_max_cycle = df.groupby("run_id")["cycle"].max().to_dict()

    results = []
    for (run_id, team_id), team_rows in df.groupby(["run_id", "team_id"]):
        full_timeline, _ = reconstruct_full_timeline(team_rows, run_max_cycle[run_id])
        results.append(recompute_team_timeline(full_timeline))

    return pd.concat(results, ignore_index=True)


def validate_against_watchdog(result_df):
    """Compares the recomputed evidence channels and anomaly detection
    against the watchdog's own real, logged output columns
    (watchdog_evidence_flags, watchdog_anomaly_detected,
    watchdog_anomaly_confidence). Returns a dict of per-channel and overall
    agreement rates. Only rows with a real logged value (was_logged) are
    compared."""
    logged_only = result_df[result_df["was_logged"]]

    overall_agreement = (
        logged_only["recomputed_anomaly_detected"] == logged_only["watchdog_anomaly_detected"]
    ).mean()

    channel_agreement = {}
    for ch, bit in FLAG_BITS.items():
        real_flag = (logged_only["watchdog_evidence_flags"].astype(int) & bit) > 0
        recomputed_flag = logged_only[f"recomputed_{ch}_flag"].astype(bool)
        channel_agreement[ch] = (real_flag == recomputed_flag).mean()

    return {
        "overall_anomaly_detected_agreement": overall_agreement,
        "channels": channel_agreement,
        "rows_compared": len(logged_only),
    }


if __name__ == "__main__":
    CSV_PATH = "simulation_output.csv"

    df = pd.read_csv(CSV_PATH)
    df["_row_seq"] = np.arange(len(df), dtype=np.int32)

    result_df = compute_evidence_features(df)
    report = validate_against_watchdog(result_df)

    print("Rows compared:", report["rows_compared"])
    print("Overall anomaly_detected agreement:", f"{report['overall_anomaly_detected_agreement']:.4f}")
    print("\nPer-channel agreement:")
    for ch, agreement in report["channels"].items():
        print(f"  {ch:16s} {agreement:.4f}")

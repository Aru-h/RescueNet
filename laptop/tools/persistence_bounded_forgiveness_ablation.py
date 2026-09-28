"""
Item 8: bounded-forgiveness persistence-streak variant vs. the shipped
hard-consecutive-streak (H=8) mechanism (core/recovery_manager.c's
predictor_persistence_streak_update()/FAULT_PERSISTENCE_FLOOR=0.20).

Bounded-forgiveness rule (explicitly narrower than the already-rejected
proportional up/down decay counter in recovery_manager_config.h:154-170):
tolerate exactly ONE isolated miss without resetting the streak, but
hard-reset on two CONSECUTIVE misses. State: streak (uint) + was_last_
miss (bool). On a miss: if was_last_miss, reset streak to 0; otherwise
leave streak unchanged (paused, not reset). On a hit: streak += 1,
was_last_miss = False.

Validated against the SAME real methodology already used to tune H=8
(recovery_manager_config.h's own doc): recomputed_anomaly_confidence
(evidence_channels.py, real math, no trained model) against real
simulator ground truth (fault_active_*), false-reject/false-accept
rates - not synthetic sequences alone.
"""
import sys, os
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO_ROOT)

import numpy as np
import pandas as pd
from evidence_channels import reconstruct_full_timeline, recompute_team_timeline

CSV_PATH = os.environ.get(
    "TRON_SNAPSHOT_CSV", os.path.join(REPO_ROOT, "tron_snapshot_dataset.csv")
)  # override so concurrent Makefile checks never share one CSV path
  # (a race corrupted it for another check mid-sweep once - see the
  # Makefile targets that set this env var to a private scratch path)
FAULT_LABEL_COLUMNS = [f"fault_active_{c}" for c in
                       ["mobility", "communication", "resource", "environment", "mission"]]

FLOOR = 0.20
H = 8


def hard_reset_streak(confidence):
    """Baseline mechanism - exact port of predictor_persistence_streak_update()."""
    streak = np.zeros(len(confidence), dtype=np.int32)
    s = 0
    for i, c in enumerate(confidence):
        s = s + 1 if c >= FLOOR else 0
        streak[i] = s
    return streak


def bounded_forgiveness_streak(confidence):
    """New mechanism - tolerates exactly one isolated miss."""
    streak = np.zeros(len(confidence), dtype=np.int32)
    s = 0
    was_last_miss = False
    for i, c in enumerate(confidence):
        if c >= FLOOR:
            s += 1
            was_last_miss = False
        else:
            if was_last_miss:
                s = 0  # second consecutive miss - hard reset
            # else: first isolated miss - forgiven, s unchanged
            was_last_miss = True
        streak[i] = s
    return streak


def dip_every_nth_tick_confidence(n, high=0.85, low=0.0, length=200):
    return np.array([low if (t + 1) % n == 0 else high for t in range(length)])


print("=" * 70)
print("PART 1: direct re-test of the dip-every-7th-tick exploit found earlier this session")
print("=" * 70)
seq = dip_every_nth_tick_confidence(7)
baseline = hard_reset_streak(seq)
new = bounded_forgiveness_streak(seq)
print(f"Hard-reset (H={H}):        max streak ever reached = {baseline.max()}  "
      f"(sustained escalation past LOCAL_RECOVERY requires reaching {H}: "
      f"{'NEVER' if baseline.max() < H else 'reached at tick ' + str(int(np.argmax(baseline >= H)) + 1)})")
print(f"Bounded-forgiveness (H={H}): max streak ever reached = {new.max()}  "
      f"(reached H at tick {int(np.argmax(new >= H)) + 1 if new.max() >= H else 'NEVER'})")
print(f"\n  -> The dip-every-7th-tick exploit is {'CLOSED' if new.max() >= H else 'NOT closed'} "
      f"by the bounded-forgiveness variant (confirmed directly, not inferred).")

print("\n" + "=" * 70)
print("PART 2: real false-reject/false-accept methodology on the actual simulator dataset")
print("=" * 70)
print("Loading + dedup...")
df = pd.read_csv(CSV_PATH)
for col in df.columns:
    if df[col].dtype == "int64":
        df[col] = pd.to_numeric(df[col], downcast="integer")
    elif df[col].dtype == "float64":
        df[col] = pd.to_numeric(df[col], downcast="float")
df["_row_seq"] = np.arange(len(df), dtype=np.int32)
df = df.sort_values(["run_id", "team_id", "cycle", "_row_seq"])
df = df.drop_duplicates(subset=["run_id", "team_id", "cycle"], keep="last")

run_max_cycle = df.groupby("run_id")["cycle"].max().to_dict()

onset_runs_baseline_len = []
onset_runs_new_len = []
noise_runs_baseline_len = []
noise_runs_new_len = []

print("Reconstructing per-(run_id,team_id) timelines and running both streak mechanisms...")
n_timelines = 0
for (run_id, team_id), team_rows in df.groupby(["run_id", "team_id"]):
    full_timeline, _ = reconstruct_full_timeline(team_rows, run_max_cycle[run_id])
    full_timeline = recompute_team_timeline(full_timeline)
    n_timelines += 1

    confidence = full_timeline["recomputed_anomaly_confidence"].to_numpy()
    # fault_active_* columns DO survive reconstruct_full_timeline()'s reindex, but
    # are NOT in its RAW_COLUMNS/DEFAULTS ffill list, so gap ticks hold NaN, not a
    # forward-filled real value - must ffill them explicitly here, same as every
    # other column reconstruct_full_timeline() actually ffills, rather than casting
    # NaN straight to int (undefined/garbage, not just a cosmetic warning).
    gt = team_rows.set_index("cycle")[FAULT_LABEL_COLUMNS].reindex(
        range(0, run_max_cycle[run_id] + 1)).ffill().fillna(0).to_numpy()
    any_fault_active = gt.max(axis=1).astype(int)

    baseline_streak = hard_reset_streak(confidence)
    new_streak = bounded_forgiveness_streak(confidence)

    # Contiguous above-floor runs (baseline definition - a run IS the hard-reset
    # streak's own contiguous nonzero stretch, so run-end length == streak value
    # at that index).
    above = confidence >= FLOOR
    change = np.diff(np.concatenate([[0], above.astype(int), [0]]))
    starts = np.where(change == 1)[0]
    ends = np.where(change == -1)[0]  # exclusive

    for s, e in zip(starts, ends):
        run_len_baseline = e - s
        # bounded-forgiveness run length at the same run: the streak value the
        # NEW mechanism holds at the end of this same above-floor stretch (it
        # may be larger, since it wasn't reset by an earlier isolated miss).
        run_len_new = int(new_streak[e - 1])
        is_onset = bool(any_fault_active[s:e].max()) if e > s else False
        if is_onset:
            onset_runs_baseline_len.append(run_len_baseline)
            onset_runs_new_len.append(run_len_new)
        else:
            noise_runs_baseline_len.append(run_len_baseline)
            noise_runs_new_len.append(run_len_new)

print(f"Timelines processed: {n_timelines}")
print(f"Onset (real fault active) above-floor runs: {len(onset_runs_baseline_len)}")
print(f"Noise (no fault active) above-floor runs:   {len(noise_runs_baseline_len)}")

# Both rates below are means over these lists. An empty list makes a rate
# NaN, every NaN comparison in the VERDICT chain is False, and the chain
# used to fall through to a confident "does NOT meaningfully improve"
# verdict with exit 0 - a verdict from no data at all. Refuse instead.
if not onset_runs_baseline_len or not noise_runs_baseline_len:
    sys.stdout.flush()
    print(f"\nERROR: {CSV_PATH} has no {'fault-onset' if not onset_runs_baseline_len else 'no-fault'} "
          "above-floor runs, so false-reject/false-accept cannot be measured.\n"
          "This needs a dataset generated with fault injection, e.g.\n"
          "  ./tron 1 30 400 --fault-profile=combined\n"
          "(a plain `make smoke` / `./tron 1 1 200` dataset has no faults).",
          file=sys.stderr)
    sys.exit(1)

onset_baseline = np.array(onset_runs_baseline_len)
onset_new = np.array(onset_runs_new_len)
noise_baseline = np.array(noise_runs_baseline_len)
noise_new = np.array(noise_runs_new_len)

fr_baseline = (onset_baseline < H).mean()
fa_baseline = (noise_baseline >= H).mean()
fr_new = (onset_new < H).mean()
fa_new = (noise_new >= H).mean()

print(f"\nAt H={H}:")
print(f"  Hard-reset (shipped):        false-reject={fr_baseline:.1%}  false-accept={fa_baseline:.1%}")
print(f"  Bounded-forgiveness (new):   false-reject={fr_new:.1%}  false-accept={fa_new:.1%}")

print(f"\nDelta: false-reject {'-' if fr_new < fr_baseline else '+'}{abs(fr_new - fr_baseline):.1%}, "
      f"false-accept {'-' if fa_new < fa_baseline else '+'}{abs(fa_new - fa_baseline):.1%}")

print("\n" + "=" * 70)
print("VERDICT")
print("=" * 70)
if fr_new < fr_baseline and fa_new <= fa_baseline + 0.02:
    print("Bounded-forgiveness improves false-reject without meaningfully worsening "
          "false-accept - a real improvement over H=8 hard-reset.")
elif fr_new < fr_baseline and fa_new > fa_baseline + 0.02:
    print("Bounded-forgiveness reduces false-reject but at a real false-accept cost - "
          "the same class of tradeoff the up/down variant was rejected for. Whether "
          "this net trade is worth it depends on which error this system should prefer, "
          "but it does reopen some of the noise-forgiveness problem the up/down variant "
          "had, just to a lesser degree (bounded to one isolated tick, not unlimited decay).")
else:
    print("Bounded-forgiveness does NOT meaningfully improve on H=8 hard-reset on this "
          "real dataset - stating this plainly rather than shipping it anyway.")

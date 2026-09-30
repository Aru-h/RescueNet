#!/usr/bin/env python3
"""
laptop/tools/ai_fp_report.py

False-positive recovery actions, AI off vs AI on. Runs ./tron (AI off) and
./tron_ai (`make tron-ai`, AI on) on the same held-out seeds and reads the
snapshot CSV each writes (EVENT_TICK rows, one per team per tick).

Why: an AI score only becomes a recovery action when backed up by the
watchdog anomaly flag, a sustained streak or consensus ELEVATED. The
watchdog threshold (core/watchdog.c WATCHDOG_ANOMALY_THRESHOLD) was tuned
while the AI always returned zero, so its false-positive rate never had a
live effect; with the AI on, a false watchdog alarm can back up an AI score
and pull a healthy team off its task. This script only measures - it does
not retune anything.

Metrics per (binary, profile), summed over the seeds:
  (a) watchdog anomaly false-positive rate: fraction of team-ticks with NO
      active fault (fault_active_mask == 0) where watchdog_anomaly_detected
      is 1.
  (b) false-positive actions: recovery actions on a team with no injected
      fault. An "action" is the rising edge of recovery_recommendation
      >= RETURN_TO_SAFE_ZONE (2) for a team (the fused decision that
      recovery_dispatch() acts on: return to safe zone / reallocate /
      abort). Counted as false-positive when the team had no active fault
      on that tick or in the previous GRACE ticks (so the tail of a fault
      that just ended is not counted against the detector).
  (c) true-positive actions: the same rising edges on a tick where the team
      has an active fault.
  Also reported: actions whose fused reason is PREDICTED_FAULT (the AI
  evaluator won the slot), actions in the post-fault grace window
  (neither FP nor TP), and LOCAL_RECOVERY ticks (the mild AI-alone tier).

usage (from the repo root, after `make && make tron-ai`):
    python3 laptop/tools/ai_fp_report.py [--seeds-start 1001] [--seeds-count 3]
        [--ticks 3000] [--profiles none,combined]
"""
import argparse
import csv
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EVENT_TICK = 12
ACTION_RTSZ = 2
REASON_PREDICTED_FAULT = 9
GRACE = 10


def run_one(binary: Path, seed: int, ticks: int, profile: str) -> Path:
    workdir = Path(tempfile.mkdtemp(prefix="aifp_"))
    subprocess.run([str(binary), str(seed), "1", str(ticks), f"--fault-profile={profile}"],
                   cwd=workdir, check=True, capture_output=True)
    return workdir / "tron_snapshot_dataset.csv"


def measure(csv_path: Path, acc: dict) -> None:
    last_fault = {}     # team -> last cycle with an active fault
    prev_action = {}    # team -> previous tick's action-ness
    with open(csv_path, newline="") as fh:
        rows = [r for r in csv.DictReader(fh) if int(r["event_type"]) == EVENT_TICK]
    rows.sort(key=lambda r: (int(r["team_id"]), int(r["cycle"])))
    for r in rows:
        team, cycle = int(r["team_id"]), int(r["cycle"])
        faulted = int(r["fault_active_mask"]) != 0
        anomaly = int(r["watchdog_anomaly_detected"]) != 0
        rec = int(r["recovery_recommendation"])
        reason = int(r["recovery_reason"])
        acc["team_ticks"] += 1
        if faulted:
            acc["fault_ticks"] += 1
            last_fault[team] = cycle
        else:
            acc["clean_ticks"] += 1
            if anomaly:
                acc["clean_anomaly_ticks"] += 1
        if rec == 1:
            acc["local_recovery_ticks"] += 1
        is_action = rec >= ACTION_RTSZ
        if is_action and not prev_action.get(team, False):
            acc["actions"] += 1
            if reason == REASON_PREDICTED_FAULT:
                acc["ai_reason_actions"] += 1
            if faulted:
                acc["tp_actions"] += 1
            elif cycle - last_fault.get(team, -10**9) <= GRACE:
                acc["grace_actions"] += 1
            else:
                acc["fp_actions"] += 1
                if reason == REASON_PREDICTED_FAULT:
                    acc["fp_ai_reason_actions"] += 1
        prev_action[team] = is_action


def new_acc() -> dict:
    keys = ["team_ticks", "fault_ticks", "clean_ticks", "clean_anomaly_ticks", "local_recovery_ticks",
            "actions", "ai_reason_actions", "tp_actions", "fp_actions", "grace_actions", "fp_ai_reason_actions"]
    return {k: 0 for k in keys}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--seeds-start", type=int, default=1001)
    ap.add_argument("--seeds-count", type=int, default=3)
    ap.add_argument("--ticks", type=int, default=3000)
    ap.add_argument("--profiles", default="none,combined")
    ap.add_argument("--bin", default=str(REPO / "tron"))
    ap.add_argument("--bin-ai", default=str(REPO / "tron_ai"))
    args = ap.parse_args()
    if args.seeds_start <= 300:
        print("refusing training seeds (<=300); use held-out seeds 1001+", file=sys.stderr)
        return 2
    for b in (args.bin, args.bin_ai):
        if not os.path.exists(b):
            print(f"missing {b}: run `make && make tron-ai` first", file=sys.stderr)
            return 2

    results = {}
    for label, binary in (("AI off (./tron)", args.bin), ("AI on (./tron_ai)", args.bin_ai)):
        for profile in args.profiles.split(","):
            acc = new_acc()
            for seed in range(args.seeds_start, args.seeds_start + args.seeds_count):
                measure(run_one(Path(binary), seed, args.ticks, profile), acc)
            results[(label, profile)] = acc

    print(f"seeds {args.seeds_start}..{args.seeds_start + args.seeds_count - 1}, {args.ticks} ticks each\n")
    print("| build | profile | (a) watchdog FPR (clean team-ticks) | (b) FP actions | (c) TP actions "
          "| actions total | of which AI reason | FP w/ AI reason | grace-window actions | LOCAL_RECOVERY ticks |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for (label, profile), a in results.items():
        fpr = a["clean_anomaly_ticks"] / a["clean_ticks"] if a["clean_ticks"] else float("nan")
        print(f"| {label} | {profile} | {100 * fpr:.2f}% ({a['clean_anomaly_ticks']}/{a['clean_ticks']}) "
              f"| {a['fp_actions']} | {a['tp_actions']} | {a['actions']} | {a['ai_reason_actions']} "
              f"| {a['fp_ai_reason_actions']} | {a['grace_actions']} | {a['local_recovery_ticks']} |")
    return 0


if __name__ == "__main__":
    sys.exit(main())

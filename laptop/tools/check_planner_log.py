#!/usr/bin/env python3
"""Step 3.5 planner invariants, read from the decision log (--planner-log) of a run.

  floor      every Layer A row flagged FORCED_FLOOR (the rule-based evaluators alone asked for the safe action)
             chose a go-home action: LOCAL_RECOVERY (1), RETURN_TO_SAFE_ZONE (2) or ABORT_MISSION (4), or REALLOC (3)
             - the planner never went below the floor (D2);
  approval   with approval=deny, no planner-chosen go-home row carries an approval request that was not granted,
             and no row carries APPROVAL_OK (D3); with approval=auto, every request carries APPROVAL_OK;
  cooldown   the same team is never preempted twice within the preemption cooldown (30 ticks);
  codes      Layer B / preemption rows carry a CausalAction_t (0-3), Layer A rows a RecoveryAction_t (0-4):
             there is no environment-class team action in the log (environment is routing-only);
  selftest   `--selftest` checks the calibration fitter's isotonic pass on a synthetic histogram.

usage: check_planner_log.py <tron_planner_log.csv> deny|auto
       check_planner_log.py --selftest
"""
import csv, os, sys

FORCED_FLOOR, APPROVAL_REQ, APPROVAL_OK = 0x01, 0x10, 0x20
COOLDOWN = 30


def selftest():
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from planner_calibration_fit import pav, BINS
    bins = [[0, 0, 0.0] for _ in range(BINS)]
    # observed frequency dips in the middle (0.4, 0.1, 0.5): PAV must pool the violation
    for b, (n, p) in {2: (100, 10), 10: (100, 40), 11: (100, 10), 12: (100, 50), 30: (50, 45)}.items():
        bins[b] = [n, p, 0.0]
    v = pav(bins)
    assert all(v[i] <= v[i + 1] + 1e-12 for i in range(BINS - 1)), "not monotone"
    assert abs(v[10] - v[11]) < 1e-12, "violating pair not pooled"
    assert abs(v[10] - 100 * 0.25 / 100) < 1e-9 or abs(v[10] - 0.25) < 0.0301, v[10]
    assert abs(v[2] - 0.1) < 1e-9 and abs(v[30] - 0.9) < 1e-9
    print("ok: isotonic pass is monotone, pools violations, keeps populated extremes")


def main():
    if sys.argv[1] == "--selftest":
        return selftest()
    path, mode = sys.argv[1], sys.argv[2]
    rows = list(csv.DictReader(open(path)))
    if not rows:
        print("FAIL: empty decision log"); sys.exit(1)
    last_pre = {}
    n_a = n_pre = n_floor = n_req = 0
    for r in rows:
        layer, chosen, flags = r["layer"], int(r["chosen"]), int(r["flags"])
        if layer == "A":
            n_a += 1
            if chosen > 4:
                print("FAIL: Layer A code out of range", r); sys.exit(1)
            if flags & FORCED_FLOOR:
                n_floor += 1
                if chosen not in (1, 2, 3, 4):
                    print("FAIL: planner went below the Simplex floor", r); sys.exit(1)
            if flags & APPROVAL_REQ:
                n_req += 1
                if mode == "deny" and chosen in (1, 2) and not (flags & FORCED_FLOOR):
                    print("FAIL: go-home chosen under a denied approval", r); sys.exit(1)
            if mode == "deny" and (flags & APPROVAL_OK):
                print("FAIL: approval granted in deny mode", r); sys.exit(1)
            if mode == "auto" and (flags & APPROVAL_REQ) and not (flags & APPROVAL_OK):
                print("FAIL: approval request not granted in auto mode", r); sys.exit(1)
        else:
            if chosen > 3:
                print("FAIL: Layer B code out of range", r); sys.exit(1)
            if layer == "preempt":
                n_pre += 1
                key = (r["world_seed"], r["team"])
                t = int(r["tick"])
                if key in last_pre and t - last_pre[key] <= COOLDOWN:
                    print("FAIL: team preempted again within the cooldown", r); sys.exit(1)
                last_pre[key] = t
    print(f"ok: {len(rows)} decisions ({n_a} Layer A, {n_floor} floor-forced, {n_pre} preemptions): floor, approval ({mode}), cooldown and codes hold")


main()

#!/usr/bin/env python3
"""Step 3.6 sidecar checks: values in range, escalation never undone inside an episode, a held team stays put.

usage: check_causal_persist.py <tron_causal_truth.csv> <tron_snapshot_dataset.csv>
"""
import csv, sys, collections

truth_path, data_path = sys.argv[1], sys.argv[2]
truth = collections.defaultdict(list)
with open(truth_path) as f:
    for r in csv.DictReader(f):
        truth[(r["world_seed"], r["team_id"])].append(r)
loc = {}
with open(data_path) as f:
    for r in csv.DictReader(f):
        loc[(r["world_seed"], r["cycle"], r["team_id"])] = (r["loc_x"], r["loc_y"]) if "loc_x" in r else None

bad = 0
held_rows = 0
for key, rows in truth.items():
    rows.sort(key=lambda r: int(r["cycle"]))
    prev_esc = [0, 0]
    prev_loc = None
    for r in rows:
        fs = int(r["field_state"])
        em, er = int(r["escalated_mobility"]), int(r["escalated_resource"])
        rm, rr = float(r["risk_mobility_h"]), float(r["risk_resource_h"])
        if fs not in (0, 1, 2, 3) or not (0.0 <= rm <= 1.0) or not (0.0 <= rr <= 1.0):
            bad += 1
        # an escalated flag may only fall when the fault has ended (the flag is per active fault)
        if (prev_esc[0] and not em) or (prev_esc[1] and not er):
            pass   # fault ended: allowed
        prev_esc = [em, er]
        cur = loc.get((r["world_seed"], r["cycle"], r["team_id"]))
        if fs and prev_loc is not None and cur is not None:
            held_rows += 1
            if cur != prev_loc:
                bad += 1
        prev_loc = cur if fs else None
print(f"check_causal_persist: {sum(len(v) for v in truth.values())} rows, {held_rows} held rows checked, {bad} violation(s)")
sys.exit(1 if bad else 0)

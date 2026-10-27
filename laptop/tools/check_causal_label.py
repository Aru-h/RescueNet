#!/usr/bin/env python3
"""Step 3.2 label check. Joins the dataset's EVENT_TICK rows to the causal
sidecar on (world_seed, cycle, team_id) and verifies the environment label:

  label  : fault_active_environment == (route_touch != 0), the active-mask bit
           agrees, and severity/remaining are 0 while it is off;
  hold   : fault_active_environment == env_hold (--causal-fixes=off).

Also requires that the two signals actually differ somewhere in `label` mode,
so the check cannot pass on an empty or degenerate run.
usage: check_causal_label.py <dataset.csv> <sidecar.csv> label|hold
"""
import csv, sys

EVENT_TICK = "12"
ENV = 3
ds, sc, mode = sys.argv[1], sys.argv[2], sys.argv[3]
side = {}
for r in csv.DictReader(open(sc)):
    side[(r["world_seed"], r["cycle"], r["team_id"])] = r
rows = bad = differ = n = 0
for r in csv.DictReader(open(ds)):
    if r["event_type"] != EVENT_TICK:
        continue
    s = side.get((r["world_seed"], r["cycle"], r["team_id"]))
    if s is None:
        print("FAIL: dataset tick row without a sidecar row", r["cycle"], r["team_id"]); sys.exit(1)
    n += 1
    flag = int(r["fault_active_environment"])
    want = (int(s["route_touch"]) != 0) if mode == "label" else (int(s["env_hold"]) != 0)
    if flag != int(want):
        bad += 1
    if mode == "label":
        mask_bit = (int(r["fault_active_mask"]) >> ENV) & 1
        if mask_bit != flag:
            bad += 1
        if not flag and (int(r["fault_severity_environment"]) != 0 or int(r["fault_ticks_remaining_environment"]) != 0):
            bad += 1
        if (int(s["route_touch"]) != 0) != (int(s["env_hold"]) != 0):
            differ += 1
if n == 0:
    print("FAIL: no tick rows"); sys.exit(1)
if bad:
    print(f"FAIL: {bad} rows disagree ({mode} mode, {n} tick rows)"); sys.exit(1)
if mode == "label" and differ == 0:
    print("FAIL: label and hold never differ, the check is not exercising the label"); sys.exit(1)
print(f"ok: {n} tick rows, environment label == {'route_touch' if mode == 'label' else 'env_hold'}" + (f" ({differ} rows where it differs from the old flag)" if mode == "label" else ""))

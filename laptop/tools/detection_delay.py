#!/usr/bin/env python3
"""
laptop/tools/detection_delay.py -- Step 5 D1: detection delay and miss rate of the fixed gates vs ground truth.

Reads the per-seed dataset kept by `headroom_run.py run --configs F --keep` (fixed gates: no planner, no AI) and, per
fault class, finds every fault episode (consecutive ticks with fault_active_<class> = 1 for one team). An episode is
"detected" at the first tick of the episode at which the gates' logged recovery_recommendation is at least
LOCAL_RECOVERY (1); a second reading uses RETURN_TO_SAFE_ZONE (2). Delay = detection tick minus onset tick. Missed =
never recommended before the episode ended. The recommendation may come from another evaluator (stamina, progress),
so the table also shows how many episodes were already recommended at the onset tick ("coincident").

  detection_delay.py --res build/headroom/res_d1 --stress 2,7
"""
import argparse
import csv
import glob
import os

import numpy as np

CLASSES = ["mobility", "communication", "resource"]


def episodes(path):
    out = {c: [] for c in CLASSES}
    open_ep = {}
    last = {}
    with open(path, newline="") as fh:
        for row in csv.DictReader(fh):
            if int(row["event_type"]) != 12:
                continue
            team, cyc = int(row["team_id"]), int(row["cycle"])
            rec = int(row["recovery_recommendation"])
            for c in CLASSES:
                f = int(row[f"fault_active_{c}"])
                key = (team, c)
                if f and key not in open_ep:
                    open_ep[key] = [cyc, None, None]       # onset, first rec>=1, first rec>=2
                if f:
                    ep = open_ep[key]
                    if rec >= 1 and ep[1] is None:
                        ep[1] = cyc
                    if rec >= 2 and ep[2] is None:
                        ep[2] = cyc
                elif key in open_ep:
                    ep = open_ep.pop(key)
                    out[c].append((ep[0], ep[1], ep[2], cyc))
            last[team] = cyc
    for (team, c), ep in open_ep.items():
        out[c].append((ep[0], ep[1], ep[2], last[team] + 1))
    return out


def summarise(eps, level):
    n = len(eps)
    if n == 0:
        return None
    idx = 1 if level == 1 else 2
    det = [e[idx] - e[0] for e in eps if e[idx] is not None]
    coincident = sum(1 for e in eps if e[idx] is not None and e[idx] == e[0])
    d = np.array(det) if det else np.array([0])
    return dict(n=n, missed=n - len(det), miss=(n - len(det)) / n, median=float(np.median(d)), p10=float(np.percentile(d, 10)),
                p90=float(np.percentile(d, 90)), le5=float(np.mean(d <= 5)), le10=float(np.mean(d <= 10)),
                le40=float(np.mean(d <= 40)), coincident=coincident / n, ep_len=float(np.mean([e[3] - e[0] for e in eps])))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--res", required=True)
    ap.add_argument("--stress", default="2,7")
    ap.add_argument("--out")
    a = ap.parse_args()
    lines = ["| Stress | Class | Episodes | Mean length (ticks) | Level | Missed | Delay median [p10, p90] (ticks) | Detected within 5 / 10 / 40 ticks | Already recommended at onset |",
             "|---|---|---|---|---|---|---|---|---|"]
    for st in a.stress.split(","):
        files = sorted(glob.glob(os.path.join(a.res, f"s{float(st):g}", "F", "raw", "*", "tron_snapshot_dataset.csv")))
        allep = {c: [] for c in CLASSES}
        for f in files:
            e = episodes(f)
            for c in CLASSES:
                allep[c] += e[c]
        for c in CLASSES:
            for level, lname in ((1, ">= LOCAL"), (2, ">= RETURN")):
                s = summarise(allep[c], level)
                if s is None:
                    continue
                lines.append(f"| {st} | {c} ({len(files)} seeds) | {s['n']} | {s['ep_len']:.0f} | {lname} | {100 * s['miss']:.0f}% | "
                             f"{s['median']:.0f} [{s['p10']:.0f}, {s['p90']:.0f}] | {100 * s['le5']:.0f}% / {100 * s['le10']:.0f}% / {100 * s['le40']:.0f}% | "
                             f"{100 * s['coincident']:.0f}% |")
    text = "\n".join(lines)
    print(text)
    if a.out:
        open(a.out, "w").write(text + "\n")


if __name__ == "__main__":
    main()

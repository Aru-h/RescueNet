#!/usr/bin/env python3
"""
laptop/tools/headroom_export.py -- one compact CSV of per-run results, so the Step 5 analysis is reproducible without
the (gitignored) raw result tree.

  headroom_export.py --res build/headroom/res --configs P,F,NOFAULT --out docs/data/headroom/tier0.csv [--stress 2,7]

One row per (stress, config, seed): incidents resolved, victims lost and seen, incidents spawned, priority-1 resolved
and victims lost, mean teams in service, key probe counters, wall seconds, the binary hash that produced the run.
Results written before the runner recorded the binary hash are tagged with the hash of the binary that was current
when the file was written (--legacy-bin NAME=HASH:MTIME, by default the frozen B1 binary).
"""
import argparse
import csv
import glob
import json
import os

COUNTERS = [("PROBE", k) for k in ("oracle_misses", "oracle_false_alarms", "dropout_team_ticks", "event_mismatch", "rr_relief_assign",
                                   "rr_relief_swap", "rr_repair_try", "rr_repair_commit", "rr_preempt_considered", "rr_preempt_done",
                                   "rr_forgone_work", "rr_lanes")] + \
           [("ROADKNOW", k) for k in ("damage_episodes", "known_episodes", "known_within_2", "known_delay_sum", "known_delay_max",
                                      "reports", "lost", "unknown_partial_enter", "blocked_stops", "stale_cell_ticks")] + \
           [("ROADUSE", k) for k in ("no_path_hits", "stall_ticks")]


def insvc(r):
    return sum(sum(v.get(k, 0) for k in ("on_scene", "to_task", "available")) for v in r.get("tb", {}).values()) / 3000.0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--res", required=True)
    ap.add_argument("--configs", required=True)
    ap.add_argument("--stress", default="2,7")
    ap.add_argument("--out", required=True)
    ap.add_argument("--legacy-bin", default="63a3f3bfb95f")
    ap.add_argument("--legacy-mtime", type=float, default=0.0, help="results older than this have no known binary (unknown)")
    ap.add_argument("--tier", default="")
    a = ap.parse_args()
    cols = ["tier", "stress", "config", "seed", "incidents", "victims_lost", "victims_seen", "spawned", "p1_resolved",
            "p1_victims_lost", "teams_in_service", "wall_s", "bin"] + [f"{g.lower()}_{k}" for g, k in COUNTERS]
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    n = 0
    with open(a.out, "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(cols)
        for st in a.stress.split(","):
            for cfg in a.configs.split(","):
                for f in sorted(glob.glob(os.path.join(a.res, f"s{float(st):g}", cfg, "*.json"))):
                    r = json.load(open(f))
                    binh = r.get("bin") or (a.legacy_bin if os.path.getmtime(f) >= a.legacy_mtime else "unknown")
                    row = [a.tier, st, cfg, r["seed"], r["incidents"], r.get("victims_lost", ""), r.get("victims_seen", ""),
                           r.get("spawned", ""), r.get("resolved_by_prio", {}).get("1", 0),
                           (r.get("prio_lost") or [0])[0], f"{insvc(r):.3f}", r.get("wall_s", ""), binh]
                    for g, k in COUNTERS:
                        row.append(r.get("diag", {}).get(g, {}).get(k, ""))
                    w.writerow(row)
                    n += 1
    print(f"wrote {n} rows to {a.out}")


if __name__ == "__main__":
    main()

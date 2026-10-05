#!/usr/bin/env python3
"""What ends an active fault under the causal profile (measurement only).

Reads tron_snapshot_dataset.csv + tron_causal_truth.csv from each seed directory
(<root>/<seed>/), finds fault episodes per team and class, and reports how each
ended (injector backstop timer vs the cause being removed), episode length, the
backstop length drawn at onset, and what the team was doing when it ended.
"""
import csv, sys, os, collections

CLASSES = ["mobility", "communication", "resource", "environment", "mission"]
CAUSE = {0: "none", 1: "removed", 2: "backstop"}   # CausalClearCause_t order (checked below)

def main(root):
    seeds = sorted(d for d in os.listdir(root) if d.isdigit())
    stats = collections.defaultdict(lambda: collections.defaultdict(list))
    for sd in seeds:
        dpath = os.path.join(root, sd, "tron_snapshot_dataset.csv")
        tpath = os.path.join(root, sd, "tron_causal_truth.csv")
        if not (os.path.exists(dpath) and os.path.exists(tpath)):
            continue
        truth = {}
        with open(tpath) as f:
            for r in csv.DictReader(f):
                truth[(r["cycle"], r["team_id"])] = r
        rows = collections.defaultdict(list)
        with open(dpath) as f:
            for r in csv.DictReader(f):
                rows[r["team_id"]].append(r)
        for team, rs in rows.items():
            rs.sort(key=lambda r: int(r["cycle"]))
            for ci, c in enumerate(CLASSES):
                start = None
                for i, r in enumerate(rs):
                    act = r["fault_active_" + c] == "1"
                    if act and start is None:
                        start = i
                    elif not act and start is not None:
                        ep = rs[start:i]
                        dur = i - start
                        back = int(ep[0]["fault_ticks_remaining_" + c] or 0) + 1
                        t = truth.get((r["cycle"], team), {})
                        cc = int(t.get("clear_cause_" + c, 0) or 0)
                        last = ep[-1]
                        went_home = any(e["recovery_recommendation"] in ("1", "RETURN_TO_SAFE_ZONE") for e in ep)
                        st = stats[c]
                        st["dur"].append(dur)
                        st["back"].append(back)
                        st["cause"].append(CAUSE.get(cc, str(cc)))
                        st["resting_at_end"].append(last["status"])
                        st["short"].append(back - dur if CAUSE.get(cc) == "removed" else 0)
                        st["home"].append(went_home)
                        start = None
    print("class         n   by-cause%  mean-dur  mean-backstop  mean-shortening(cause-ended)  mean-dur(cause)  mean-dur(backstop)")
    for c in CLASSES:
        st = stats[c]
        n = len(st["dur"])
        if not n:
            print(f"{c:12s}  0"); continue
        cs = [i for i, x in enumerate(st["cause"]) if x == "removed"]
        bs = [i for i, x in enumerate(st["cause"]) if x == "backstop"]
        md = lambda idx, k: sum(st[k][i] for i in idx) / len(idx) if idx else float("nan")
        print(f"{c:12s} {n:5d}  {100*len(cs)/n:7.1f}  {sum(st['dur'])/n:8.2f}  {sum(st['back'])/n:12.2f}  {md(cs,'short'):26.2f}  {md(cs,'dur'):14.2f}  {md(bs,'dur'):16.2f}")
    return stats

if __name__ == "__main__":
    main(sys.argv[1])

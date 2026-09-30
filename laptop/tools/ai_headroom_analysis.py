#!/usr/bin/env python3
"""Tables for ai_headroom_investigation.sh: incidents per condition, fault
exposure, and the share of the fault-induced incident loss that a perfect
detector recovers, per fault class.  usage: ai_headroom_analysis.py <dir with runs/>"""
import glob, os, re, sys
import numpy as np, pandas as pd

W = sys.argv[1]
CLS = ["mobility", "communication", "resource", "environment", "mission"]
TICK = 12  # eval_metrics.EVENT_TICK


def incidents(d):
    return sum(int(x) for x in re.findall(r"incidents_resolved=(\d+)", open(d + "/diag.log").read()))


def runs(cond):
    return {int(os.path.basename(d)): d for d in glob.glob(f"{W}/runs/{cond}/*") if os.path.exists(d + "/ok")}


def exposure(d):
    df = pd.read_csv(d + "/tron_snapshot_dataset.csv", low_memory=False,
                     usecols=["event_type", "cycle", "team_id", "fault_active_mask"])
    df = df[df.event_type == TICK]
    n = 0
    for _, g in df.sort_values(["team_id", "cycle"]).groupby("team_id"):
        v = (g.fault_active_mask.to_numpy() > 0).astype(int)
        n += int(((v[1:] == 1) & (v[:-1] == 0)).sum() + v[0])
    return int((df.fault_active_mask > 0).sum()), n, len(df) // 1


print("## Fault exposure (mean per run, 10 teams x 3000 ticks = 30000 team-ticks), profile random")
for c in ["free", "noai", "oracle"]:
    rs = runs(c)
    if not rs: continue
    e = np.array([exposure(d) for d in rs.values()])
    inc = np.mean([incidents(d) for d in rs.values()])
    ft, ep = e[:, 0].mean(), e[:, 1].mean()
    print(f"{c:8s} incidents {inc:6.2f}  fault team-ticks {ft:9.1f} ({ft/300:4.1f}% of team-ticks)  episodes {ep:6.1f}  ticks/episode {ft/ep if ep else 0:5.1f}")


def boot(x, n=4000):
    r = np.random.default_rng(0); x = np.asarray(x, float)
    return np.percentile([r.choice(x, len(x)).mean() for _ in range(n)], [2.5, 97.5])


free = {s: incidents(d) for s, d in runs("free").items()}
print("\n## Incidents lost to faults, and share a perfect detector recovers (paired seeds)")
print(f"{'profile':14s} {'free':>6s} {'no-AI':>6s} {'oracle':>7s} {'loss':>6s} {'oracle gain [95% CI]':>24s} {'recovered':>9s}")
for p in ["mobility", "communication", "resource", "random"]:
    n = {s: incidents(d) for s, d in runs("noai" if p == "random" else "noai_" + p).items()}
    o = {s: incidents(d) for s, d in runs("oracle" if p == "random" else "oracle_" + p).items()}
    s = sorted(set(n) & set(o) & set(free))
    if not s: continue
    f, nn, oo = (np.mean([x[i] for i in s]) for x in (free, n, o))
    lo, hi = boot([o[i] - n[i] for i in s])
    print(f"{p:14s} {f:6.2f} {nn:6.2f} {oo:7.2f} {f-nn:6.2f} {oo-nn:+8.2f} [{lo:+.2f},{hi:+.2f}] {100*(oo-nn)/(f-nn):8.0f}%")

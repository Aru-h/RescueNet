#!/usr/bin/env python3
"""Sum [DIAG FAULT] / [DIAG FAULTEND] over seed directories: what ends each fault class.

usage: fault_end_summary.py <dir> [<dir> ...]    (each holds <seed>/diag.log)
"""
import re, sys, os, glob

NAMES = ["mobility", "communication", "resource", "environment", "mission"]

def collect(d):
    t = {c: dict(on=0, cause=0, back=0, ticks=0, cticks=0, cplan=0) for c in range(5)}
    n = 0
    for p in sorted(glob.glob(os.path.join(d, "*", "diag.log"))):
        n += 1
        for line in open(p):
            m = re.match(r"\[DIAG FAULT\] class=(\d) onsets=(\d+) cleared_by_cause=(\d+) cleared_by_backstop=(\d+) exposure_ticks=(\d+) episode_ticks=(\d+)", line)
            if m:
                c = int(m[1]); t[c]["on"] += int(m[2]); t[c]["cause"] += int(m[3]); t[c]["back"] += int(m[4]); t[c]["ticks"] += int(m[6])
            m = re.match(r"\[DIAG FAULTEND\] class=(\d) cause_ticks=(\d+) cause_planned_ticks=(\d+)", line)
            if m:
                c = int(m[1]); t[c]["cticks"] += int(m[2]); t[c]["cplan"] += int(m[3])
    return n, t

for d in sys.argv[1:]:
    n, t = collect(d)
    print(f"== {os.path.basename(d.rstrip('/'))}: {n} seeds (per-seed means)")
    print("class          onsets  by-cause  by-backstop  mean-len  len(cause)  planned(cause)  shortened-by  len(backstop)")
    for c in range(5):
        x = t[c]; ended = x["cause"] + x["back"]
        if not ended:
            continue
        lc = x["cticks"] / x["cause"] if x["cause"] else float("nan")
        pc = x["cplan"] / x["cause"] if x["cause"] else float("nan")
        lb = (x["ticks"] - x["cticks"]) / x["back"] if x["back"] else float("nan")
        print(f"{NAMES[c]:13s} {x['on']/n:7.1f} {x['cause']/n:9.1f} {x['back']/n:12.1f} {x['ticks']/ended:9.2f} {lc:11.2f} {pc:15.2f} {pc-lc:13.2f} {lb:14.2f}")

#!/usr/bin/env python3
"""
laptop/tools/fault_exposure.py

Runs ./tron once per seed (AI off - the default build) and reports, per
fault class, how much of the run teams spend faulted, plus the mission
outcome (incidents resolved, from --diag). With --check it also enforces
sanity bounds on that exposure, which catches runaway fault -> drain ->
fault loops and a broken maximum-duration backstop.

Standard library only (no pandas), so it runs wherever `make` does.

usage:
  fault_exposure.py [--tron ./tron] [--profile causal] [--seeds 1001-1020]
                    [--ticks 3000] [--stress X] [--jobs 4] [--extra-arg ARG ...]
                    [--check] [--json OUT.json]
"""
import argparse
import csv
import json
import os
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

CLASSES = ["mobility", "communication", "resource", "environment", "mission"]
EVENT_TICK = 12
THROUGHPUT_RE = re.compile(r"\[DIAG THROUGHPUT\] team=(\d+) incidents_resolved=(\d+)")
CAUSAL_RE = re.compile(
    r"\[DIAG CAUSAL\] fault_stress=(\S+) victims_seen=(\d+) victims_lost=(\d+) escalations=(\d+) "
    r"aftershock_events=(\d+) aftershock_team_hits=(\d+)")
ACTION_RE = re.compile(
    r"\[DIAG ACTION\] name=(\S+) dispatched=(\d+) resolved=(\d+) abandoned=(\d+) active_ticks=(\d+) from_detector=(\d+)")
FAULT_RE = re.compile(
    r"\[DIAG FAULT\] class=(\d+) onsets=(\d+) cleared_by_cause=(\d+) "
    r"cleared_by_backstop=(\d+) exposure_ticks=(\d+) episode_ticks=(\d+)")

# Sanity bounds for --check. These are deliberately loose guard rails
# (assumed, not calibrated): they exist to catch a loop that pins teams in
# a faulted state, not to define the "right" fault rate.
MAX_CLASS_EXPOSURE = 0.30        # fraction of team-ticks one class may be active
MAX_ANY_EXPOSURE = 0.60          # fraction of team-ticks with >= 1 active fault
MAX_TEAM_ANY_EXPOSURE = 0.80     # worst single team, any fault
MAX_ONSETS_PER_TEAM_KTICK = 60.0 # chattering guard (episodes / team / 1000 ticks)
# Longest episode a class may show = configured backstop + slack. Under the
# causal profile communication has no backstop (it clears only when the link
# margin is positive), so it is bounded by exposure and chattering instead.
MAX_EPISODE_TICKS = {"mobility": 14 + 2, "resource": 14 + 2,
                     "environment": 12 + 2, "mission": 12 + 2}


ONSET_NAMES = ["none", "baseline", "wear", "roughness", "battery", "since_rest",
               "link_margin", "aftershock", "deterioration"]


def parse_seeds(text):
    if "-" in text:
        lo, hi = text.split("-", 1)
        return list(range(int(lo), int(hi) + 1))
    return [int(x) for x in text.split(",")]


def run_seed(tron, profile, seed, ticks, extra):
    tron = os.path.abspath(tron)
    with tempfile.TemporaryDirectory(prefix=f"fexp{seed}_") as work:
        cmd = [tron, str(seed), "1", str(ticks), f"--fault-profile={profile}", "--diag"] + extra
        proc = subprocess.run(cmd, cwd=work, capture_output=True, text=True)
        if proc.returncode != 0:
            raise RuntimeError(f"seed {seed}: {' '.join(cmd)} exited {proc.returncode}\n{proc.stderr[-400:]}")
        resolved = sum(int(m.group(2)) for m in THROUGHPUT_RE.finditer(proc.stderr))
        causal = {int(m.group(1)): tuple(int(g) for g in m.groups()[1:]) for m in FAULT_RE.finditer(proc.stderr)}
        actions = {m.group(1): [int(g) for g in m.groups()[1:]] for m in ACTION_RE.finditer(proc.stderr)}
        world = CAUSAL_RE.search(proc.stderr)
        world = [float(world.group(1))] + [int(g) for g in world.groups()[1:]] if world else None

        team_ticks = 0
        active = {c: 0 for c in CLASSES}
        any_active = 0
        per_team_any = {}
        per_team_ticks = {}
        episodes = {c: [] for c in CLASSES}
        open_start = {}
        last_cycle = {}
        with open(os.path.join(work, "tron_snapshot_dataset.csv"), newline="") as fh:
            for row in csv.DictReader(fh):
                if int(row["event_type"]) != EVENT_TICK:
                    continue
                team = int(row["team_id"])
                cycle = int(row["cycle"])
                flags = [int(row[f"fault_active_{c}"]) for c in CLASSES]
                team_ticks += 1
                per_team_ticks[team] = per_team_ticks.get(team, 0) + 1
                if any(flags):
                    any_active += 1
                    per_team_any[team] = per_team_any.get(team, 0) + 1
                for c, f in zip(CLASSES, flags):
                    key = (team, c)
                    if f:
                        active[c] += 1
                        open_start.setdefault(key, cycle)
                    elif key in open_start:
                        episodes[c].append(cycle - open_start.pop(key))
                last_cycle[team] = cycle
        for (team, c), start in open_start.items():
            episodes[c].append(last_cycle[team] + 1 - start)

        attribution = {c: {} for c in CLASSES}
        sidecar = os.path.join(work, "tron_causal_truth.csv")
        if os.path.exists(sidecar):
            with open(sidecar, newline="") as fh:
                for row in csv.DictReader(fh):
                    for c in CLASSES:
                        code = int(row[f"onset_cause_{c}"])
                        if code:
                            name = ONSET_NAMES[code]
                            attribution[c][name] = attribution[c].get(name, 0) + 1

        return {
            "seed": seed, "attribution": attribution, "incidents_resolved": resolved, "team_ticks": team_ticks,
            "active": active, "any_active": any_active,
            "worst_team_any": max((per_team_any.get(t, 0) / n for t, n in per_team_ticks.items()), default=0.0),
            "episodes": episodes, "causal_diag": causal, "world": world, "actions": actions,
        }


def summarise(results):
    n = len(results)
    team_ticks = sum(r["team_ticks"] for r in results)
    out = {
        "seeds": n,
        "incidents_resolved_mean": sum(r["incidents_resolved"] for r in results) / n,
        "any_exposure": sum(r["any_active"] for r in results) / team_ticks,
        "worst_team_any_exposure": max(r["worst_team_any"] for r in results),
        "classes": {},
    }
    ticks_per_team_seed = team_ticks / n / 10.0  # NUM_TEAMS == 10
    for c in CLASSES:
        eps = [e for r in results for e in r["episodes"][c]]
        out["classes"][c] = {
            "exposure": sum(r["active"][c] for r in results) / team_ticks,
            "episodes": len(eps),
            "onsets_per_team_ktick": len(eps) / (n * 10.0) / (ticks_per_team_seed / 1000.0),
            "mean_duration": (sum(eps) / len(eps)) if eps else 0.0,
            "max_duration": max(eps) if eps else 0,
            "short_share": (sum(1 for e in eps if e <= 2) / len(eps)) if eps else 0.0,
        }
    attr = {c: {} for c in CLASSES}
    for r in results:
        for c in CLASSES:
            for name, v in r["attribution"][c].items():
                attr[c][name] = attr[c].get(name, 0) + v
    out["onset_attribution"] = attr
    cause = {c: [0, 0] for c in range(len(CLASSES))}
    for r in results:
        for k, v in r["causal_diag"].items():
            cause[k][0] += v[1]
            cause[k][1] += v[2]
    out["cleared_by_cause_backstop"] = {CLASSES[k]: v for k, v in cause.items()}
    acts = {}
    for r in results:
        for name, v in r["actions"].items():
            acc = acts.setdefault(name, [0, 0, 0, 0, 0])
            for i, x in enumerate(v):
                acc[i] += x
    if acts:
        out["actions"] = {name: dict(zip(["dispatched", "resolved", "abandoned", "active_ticks", "from_detector"],
                                         [x / n for x in v])) for name, v in acts.items()}
    out["per_seed"] = {str(r["seed"]): {"incidents_resolved": r["incidents_resolved"],
                                        "victims_lost": (r["world"][2] if r["world"] else None)} for r in results}
    worlds = [r["world"] for r in results if r["world"]]
    if worlds:
        seen = sum(w[1] for w in worlds)
        out["world"] = {
            "fault_stress": worlds[0][0],
            "victims_seen": seen / n, "victims_lost": sum(w[2] for w in worlds) / n,
            "victim_loss_fraction": (sum(w[2] for w in worlds) / seen) if seen else 0.0,
            "escalations": sum(w[3] for w in worlds) / n,
            "aftershock_events": sum(w[4] for w in worlds) / n,
            "aftershock_team_hits": sum(w[5] for w in worlds) / n,
        }
    return out


def check(summary):
    problems = []
    for c, s in summary["classes"].items():
        if s["exposure"] > MAX_CLASS_EXPOSURE:
            problems.append(f"{c}: exposure {s['exposure']:.3f} > {MAX_CLASS_EXPOSURE}")
        if s["onsets_per_team_ktick"] > MAX_ONSETS_PER_TEAM_KTICK:
            problems.append(f"{c}: {s['onsets_per_team_ktick']:.1f} episodes/team/1000 ticks > {MAX_ONSETS_PER_TEAM_KTICK}")
        if c in MAX_EPISODE_TICKS and s["max_duration"] > MAX_EPISODE_TICKS[c]:
            problems.append(f"{c}: longest episode {s['max_duration']} ticks > backstop {MAX_EPISODE_TICKS[c]}")
    if summary["any_exposure"] > MAX_ANY_EXPOSURE:
        problems.append(f"any-fault exposure {summary['any_exposure']:.3f} > {MAX_ANY_EXPOSURE}")
    if summary["worst_team_any_exposure"] > MAX_TEAM_ANY_EXPOSURE:
        problems.append(f"worst team any-fault exposure {summary['worst_team_any_exposure']:.3f} > {MAX_TEAM_ANY_EXPOSURE}")
    total_onsets = sum(s["episodes"] for s in summary["classes"].values())
    if total_onsets == 0:
        problems.append("no fault episodes at all - the profile injected nothing")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tron", default="./tron")
    ap.add_argument("--profile", default="causal")
    ap.add_argument("--seeds", default="1001-1020")
    ap.add_argument("--ticks", type=int, default=3000)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--stress", type=float, help="causal --fault-stress multiplier (recorded in the output)")
    ap.add_argument("--extra-arg", action="append", default=[])
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--json")
    args = ap.parse_args()

    seeds = parse_seeds(args.seeds)
    if args.stress is not None:
        args.extra_arg = args.extra_arg + [f"--fault-stress={args.stress:g}"]
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = list(pool.map(lambda s: run_seed(args.tron, args.profile, s, args.ticks, args.extra_arg), seeds))
    summary = summarise(results)
    summary.update(profile=args.profile, ticks=args.ticks, extra_args=args.extra_arg,
                   fault_stress=args.stress if args.stress is not None else (1.0 if args.profile == "causal" else None))

    print(f"profile={args.profile} seeds={args.seeds} ticks={args.ticks} extra={args.extra_arg}")
    if args.profile == "causal":
        print(f"fault_stress used: {summary['fault_stress']:g}")
    print(f"incidents resolved / seed (mean): {summary['incidents_resolved_mean']:.2f}")
    print(f"any-fault exposure: {summary['any_exposure']:.3f}   worst single team: {summary['worst_team_any_exposure']:.3f}")
    print(f"{'class':<14}{'exposure':>9}{'episodes':>9}{'ep/team/kt':>11}{'mean dur':>9}{'max dur':>8}{'<=2tick':>8}")
    for c in CLASSES:
        s = summary["classes"][c]
        print(f"{c:<14}{s['exposure']:>9.3f}{s['episodes']:>9d}{s['onsets_per_team_ktick']:>11.1f}"
              f"{s['mean_duration']:>9.1f}{s['max_duration']:>8d}{100*s['short_share']:>7.0f}%")
    for c in CLASSES:
        a = summary["onset_attribution"][c]
        tot = sum(a.values())
        if tot:
            print(f"onset cause {c}: " + ", ".join(f"{k} {100*v/tot:.0f}%" for k, v in sorted(a.items(), key=lambda kv: -kv[1])))
    if "world" in summary:
        w = summary["world"]
        print(f"victims lost / seed: {w['victims_lost']:.1f} of {w['victims_seen']:.1f} ({100*w['victim_loss_fraction']:.1f}%)  "
              f"escalations: {w['escalations']:.1f}  aftershocks: {w['aftershock_events']:.1f} (team hits {w['aftershock_team_hits']:.1f})")
    if "actions" in summary:
        print("actions per seed (mean): " + "; ".join(
            f"{k}: started {v['dispatched']:.1f}, resolved {v['resolved']:.1f}, abandoned {v['abandoned']:.1f}, "
            f"active ticks {v['active_ticks']:.0f}" for k, v in summary["actions"].items()))
    if any(v != [0, 0] for v in summary["cleared_by_cause_backstop"].values()):
        print("cleared by [cause, backstop]: " +
              ", ".join(f"{c}={v}" for c, v in summary["cleared_by_cause_backstop"].items()))
    if args.json:
        with open(args.json, "w") as fh:
            json.dump(summary, fh, indent=2)

    if args.check:
        problems = check(summary)
        if problems:
            print("FAIL fault exposure sanity:")
            for p in problems:
                print("  - " + p)
            return 1
        print("OK fault exposure within sanity bounds")
    return 0


if __name__ == "__main__":
    sys.exit(main())

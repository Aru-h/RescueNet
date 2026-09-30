#!/usr/bin/env python3
"""
laptop/tools/ai_threshold_sweep.py

Closed-loop threshold sweep for the AI fault predictor. For each candidate
threshold set (mobility, communication, resource, environment, mission) it
builds an AI binary with -DFAULT_PREDICTOR_CLASS_THRESHOLDS=... and runs it
(plus a no-AI reference) on DEV seeds, reporting mission outcome, per-class
detection and false-positive actions. A threshold of 1.01 disables a class.

Pick thresholds on dev seeds (default 2001-2008), never on the seeds you
report on (1001-1020), then verify the chosen set there with --seeds-start
1001 --seeds-count 20.

usage: python3 laptop/tools/ai_threshold_sweep.py --configs configs.json
  configs.json: {"name": [mob, comm, res, env, mis], ...}
"""
import argparse, glob, json, os, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))
import eval_metrics as em  # noqa: E402

CFLAGS = ["-std=gnu11", "-O0", "-DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS", "-DVIRTUAL_BUS_ENABLED=1",
          "-DCONSENSUS_ENFORCE=1", "-Icore", "-Ilaptop"]
TAIL = ["laptop/main.c", "laptop/debug.c", "laptop/snapshot.c", "laptop/mission_feed.c", "laptop/mission_mirror.c"]
CLASSES = ["mobility", "communication", "resource"]


def build(name, thresholds, outdir):
    binp = Path(outdir) / f"tron_{name}"
    defs = []
    if thresholds is not None:
        arr = "{" + ",".join(f"{t}f" for t in thresholds) + "}"
        defs = ["-DFAULT_MODEL_LOADED=1", f"-DFAULT_PREDICTOR_CLASS_THRESHOLDS={arr}"]
    src = sorted(glob.glob(str(REPO / "core" / "*.c"))) + [str(REPO / t) for t in TAIL]
    subprocess.run(["cc"] + CFLAGS + defs + ["-o", str(binp)] + src + ["-lm"], cwd=REPO, check=True)
    return str(binp)


def run(task):
    binp, seed, ticks, profile = task
    with tempfile.TemporaryDirectory() as wd:
        r = subprocess.run([binp, str(seed), "1", str(ticks), f"--fault-profile={profile}", "--diag"],
                           cwd=wd, capture_output=True, text=True, check=True)
        rows = em.parse_tick_rows(os.path.join(wd, "tron_snapshot_dataset.csv"))
        cm, _ = em.compute_run_metrics(rows)
        out = {"mission": em.parse_mission_outcome(r.stderr), "det": {}, "fa": {}, "fp_actions": 0, "tp_actions": 0}
        for c in CLASSES:
            for mode in ("any", "system"):
                m = cm[(c, mode)]
                out["det"][f"{c}|{mode}"] = m.detected_at_all_rate()
                out["fa"][f"{c}|{mode}"] = m.false_alarms_per_1000_team_ticks()
        # action edges on teams with / without an active fault (grace 10 ticks)
        last, prev = {}, {}
        for x in sorted(rows, key=lambda x: (x.team_id, x.cycle)):
            faulted = any(x.fault_active.values())
            if faulted:
                last[x.team_id] = x.cycle
            act = x.recommendation >= 2
            if act and not prev.get(x.team_id, False):
                if faulted:
                    out["tp_actions"] += 1
                elif x.cycle - last.get(x.team_id, -10**9) > 10:
                    out["fp_actions"] += 1
            prev[x.team_id] = act
        return out


def mean(v):
    v = [x for x in v if x is not None]
    return sum(v) / len(v) if v else float("nan")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--configs", required=True)
    ap.add_argument("--seeds-start", type=int, default=2001)
    ap.add_argument("--seeds-count", type=int, default=8)
    ap.add_argument("--clean-seeds", type=int, default=4, help="seeds for the no-fault (profile none) run")
    ap.add_argument("--ticks", type=int, default=3000)
    ap.add_argument("--workers", type=int, default=os.cpu_count())
    ap.add_argument("--no-reference", action="store_true")
    a = ap.parse_args()
    cfgs = json.load(open(a.configs))
    if not a.no_reference:
        cfgs = {"NO_AI(rules+consensus)": None, **cfgs}
    seeds = list(range(a.seeds_start, a.seeds_start + a.seeds_count))
    clean = list(range(a.seeds_start, a.seeds_start + a.clean_seeds))
    tmp = tempfile.mkdtemp(prefix="sweep_")
    bins = {n: build(n.replace("/", "_").replace("(", "").replace(")", "").replace("+", "_"), t, tmp) for n, t in cfgs.items()}
    tasks, keys = [], []
    for n, b in bins.items():
        for s in seeds:
            tasks.append((b, s, a.ticks, "random")); keys.append((n, "random"))
        for s in clean:
            tasks.append((b, s, a.ticks, "none")); keys.append((n, "none"))
    with ProcessPoolExecutor(a.workers) as ex:
        res = list(ex.map(run, tasks))
    agg = {}
    for k, r in zip(keys, res):
        agg.setdefault(k, []).append(r)
    print(f"dev seeds {seeds[0]}..{seeds[-1]} ({len(seeds)}), clean seeds {len(clean)}, {a.ticks} ticks\n")
    print("| config | thresholds (mob,comm,res,env,mis) | incidents resolved | stage compl. | mob det (any/sys) | comm det (any/sys) | res det (any/sys) | FP actions (no faults) | FP actions (random) | TP actions (random) |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for n, t in cfgs.items():
        R = agg[(n, "random")]; C = agg[(n, "none")]
        inc = mean([r["mission"]["incidents_resolved"] for r in R]); st = mean([r["mission"]["stage_completions"] for r in R])
        d = lambda c: f"{100*mean([r['det'][c+'|any'] for r in R]):.1f}/{100*mean([r['det'][c+'|system'] for r in R]):.1f}"
        print(f"| {n} | {t if t else '-'} | {inc:.1f} | {st:.1f} | {d('mobility')} | {d('communication')} | {d('resource')} "
              f"| {mean([r['fp_actions'] for r in C]):.0f} | {mean([r['fp_actions'] for r in R]):.0f} | {mean([r['tp_actions'] for r in R]):.0f} |")


if __name__ == "__main__":
    main()

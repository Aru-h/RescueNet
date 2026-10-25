#!/usr/bin/env python3
"""
laptop/tools/headroom_run.py -- Step 5 headroom map runner (docs/HEADROOM_MAP_PREREG.md).

  headroom_run.py build                          build build/headroom/bin/{noai,ai} from the repository sources (-O2)
  headroom_run.py run --configs P,PD --stress 2,7 --set D [--seeds 1001-1100] [--jobs 4] [--out build/headroom/res]
  headroom_run.py list                           print the configuration registry

One simulator process per (configuration, stress, seed). Per-seed results are cached in
<out>/s<stress>/<config>/<seed>.json, so an interrupted run resumes. The v1 world is always
  --fault-profile=causal --fault-stress=S --causal-persist=on --causal-fixes=label,rtb,cost,stall,semantics
and every run passes --diag --probe-diag (output only). The baseline P also records the hazard events
(--probe-record) that the foresight probes replay (--probe-events).
"""
import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BIN = os.path.join(ROOT, "build", "headroom", "bin")
V1 = ["--fault-profile=causal", "--causal-persist=on", "--causal-fixes=label,rtb,cost,stall,semantics",
      "--diag", "--probe-diag"]
PL = ["--recovery=utility"]
P_WD = PL + ["--planner-p=watchdog"]
SPEC = {"search": 0, "medical": 1, "extraction": 2, "perimeter": 3, "support": 4}

# name -> (binary, args). Strings starting with '@' are expanded by run_one (per-seed paths).
CONFIGS = {
    "F": ("noai", []),
    "P": ("noai", P_WD + ["--probe-record=@rec"]),
    "PD": ("noai", PL + ["--planner-p=oracle"]),
    "FA": ("ai", []),
    "PA": ("ai", PL + ["--planner-p=ai"]),
    "NOFAULT": ("noai", None),   # special: --fault-profile=none, fixed gates
}
# D2
for k in (5, 10, 20, 40):
    CONFIGS[f"D2_{k}"] = ("noai", PL + ["--planner-p=oracle", f"--probe-oracle-delay={k}"])
CONFIGS["D3a"] = ("noai", PL + ["--planner-p=oracle", "--probe-oracle-fn=0.30", "--probe-oracle-fp=0.01"])
for q in (25, 50, 75):
    d = f"--probe-evidence-dropout={q / 100:g}"
    CONFIGS[f"D3b_P_{q}"] = ("noai", P_WD + [d])
    CONFIGS[f"D3b_PD_{q}"] = ("noai", PL + ["--planner-p=oracle", d])
    CONFIGS[f"D3b_PA_{q}"] = ("ai", PL + ["--planner-p=ai", d])
# S1: level ii (hazard) and level i (draw), maintenance cost multiplier
for m in (1, 0.5, 0.25):
    CONFIGS[f"S1_ii_{m:g}"] = ("noai", PL + ["--planner-p=hazard", "--planner-prevent=both", f"--probe-prevent-cost={m:g}"])
    CONFIGS[f"S1_i_{m:g}"] = ("noai", PL + ["--planner-p=hazard", "--planner-prevent=both", f"--probe-prevent-cost={m:g}",
                                              "--probe-hazard-foresight=draw"])
CONFIGS["M1_ii"] = ("noai", P_WD + ["--probe-urgency=survival"])
CONFIGS["M1_i"] = ("noai", P_WD + ["--probe-urgency=foresight"])
CONFIGS["M2"] = ("noai", P_WD + ["--probe-foresight=fire", "--probe-lead=50", "--probe-events=@rec"])
CONFIGS["M3"] = ("noai", P_WD + ["--probe-foresight=collapse", "--probe-lead=50", "--probe-events=@rec"])
for t in (10, 50, 200):
    CONFIGS[f"M4_{t}"] = ("noai", P_WD + ["--probe-foresight=hazard", f"--probe-lead={t}", "--probe-events=@rec"])
CONFIGS["M5"] = ("noai", P_WD + ["--probe-reveal-all"])
CONFIGS["DQ2"] = ("noai", P_WD + ["--probe-no-comm-faults"])
CONFIGS["DQ1"] = ("noai", P_WD + ["--probe-central-alloc"])
for x in (0.5, 0.75, 1.5, 2):
    CONFIGS[f"K1_rest_{x:g}"] = ("noai", P_WD + [f"--probe-rest-mult={x:g}"])
    CONFIGS[f"K1_drain_{x:g}"] = ("noai", P_WD + [f"--probe-drain-mult={x:g}"])
for k in (1, 2, 3):
    CONFIGS[f"K2_{k}"] = ("noai", P_WD + [f"--probe-offline={k}"])
# Tier 2
for x in (1.25, 1.5, 2, 3, 4):
    CONFIGS[f"W1a_{x:g}"] = ("noai", P_WD + [f"--probe-work-mult={x:g}", "--probe-work-fatigue=a"])
    CONFIGS[f"W1b_{x:g}"] = ("noai", P_WD + [f"--probe-work-mult={x:g}", "--probe-work-fatigue=b"])
for s in SPEC:
    for x in (2, 3):
        CONFIGS[f"W2_{s}_{x}"] = ("noai", P_WD + [f"--probe-work-spec={s}:{x}"])
for st in ("early", "late"):
    for x in (2, 4):
        CONFIGS[f"W3_{st}_{x}"] = ("noai", P_WD + [f"--probe-work-stage={st}:{x}"])
CONFIGS["W0"] = ("noai", P_WD + ["--probe-instant-work"])
CONFIGS["V0_task"] = ("noai", P_WD + ["--probe-instant-travel=task"])
CONFIGS["V0_all"] = ("noai", P_WD + ["--probe-instant-travel=all"])
for x in (2, 4):
    CONFIGS[f"V1_{x}"] = ("noai", P_WD + [f"--probe-speed=search:{x},extraction:{x},perimeter:0.5"])
    CONFIGS[f"V1_{x}_act"] = ("noai", P_WD + [f"--probe-speed=search:{x},extraction:{x},perimeter:0.5",
                                                "--causal-actions=reroute,clear_road"])
for m in "abc":
    CONFIGS[f"TM1_{m}"] = ("noai", P_WD + [f"--probe-team-mix={m}"])
# Tier 3
CONFIGS["R1"] = ("noai", P_WD + ["--probe-roadknow=obs"])
CONFIGS["R1s"] = ("noai", P_WD + ["--probe-roadknow=obs", "--probe-road-radius=0"])
CONFIGS["R2"] = ("noai", P_WD + ["--probe-roadknow=prior"])
CONFIGS["R3"] = ("noai", P_WD + ["--probe-roadknow=obs", "--probe-road-share=instant"])
CONFIGS["ACT"] = ("noai", P_WD + ["--causal-actions=reroute,clear_road"])
CONFIGS["R1_act"] = ("noai", P_WD + ["--probe-roadknow=obs", "--causal-actions=reroute,clear_road"])
CONFIGS["R2_act"] = ("noai", P_WD + ["--probe-roadknow=prior", "--causal-actions=reroute,clear_road"])

KV = re.compile(r"(\w+)=(-?[\d.]+)")
LINE = re.compile(r"\[DIAG (\w+)\] ?(.*)")
THROUGHPUT = re.compile(r"\[DIAG THROUGHPUT\] team=(\d+) incidents_resolved=(\d+)")


def percentile(v, q):
    if not v:
        return None
    s = sorted(v)
    return s[min(len(s) - 1, int(q * len(s)))]


def parse(stderr, keep_inc=False):
    out = {"incidents": 0, "tb": {}, "diag": {}}
    inc = []
    fog_sp = fog_re = 0
    for raw in stderr.splitlines():
        m = THROUGHPUT.match(raw)
        if m:
            out["incidents"] += int(m.group(2))
            continue
        m = LINE.match(raw)
        if not m:
            continue
        name, body = m.group(1), m.group(2)
        kv = {k: float(v) for k, v in KV.findall(body)}
        if name == "CAUSAL":
            out["victims_seen"] = kv.get("victims_seen")
            out["victims_lost"] = kv.get("victims_lost")
            out["escalations"] = kv.get("escalations")
        elif name == "FOG":
            fog_sp += kv.get("spawned", 0)
            fog_re += kv.get("resolved", 0)
        elif name == "TB":
            sp = int(kv.pop("spec"))
            out["tb"][sp] = {k: int(v) for k, v in kv.items()}
        elif name == "PRIO":
            nums = re.findall(r"(seen|lost)=([\d,]+)", body)
            for k, v in nums:
                out["prio_" + k] = [int(x) for x in v.split(",")]
        elif name == "INC":
            inc.append((int(kv["type"]), int(kv["prio"]), int(kv["reported"]), int(kv["arrived"]), int(kv["resolved"])))
        else:
            out["diag"].setdefault(name, {}).update(kv)
    out["spawned"] = fog_sp
    out["resolved_fog"] = fog_re
    if inc:
        rr = [r - p for (_, _, p, a, r) in inc]
        ra = [a - p for (_, _, p, a, r) in inc if a]
        out["resp_resolve"] = {"p50": percentile(rr, .5), "p90": percentile(rr, .9), "mean": sum(rr) / len(rr)}
        out["resp_arrive"] = {"p50": percentile(ra, .5), "p90": percentile(ra, .9), "mean": (sum(ra) / len(ra)) if ra else None}
        by_prio = {}
        by_type = {}
        for (ty, pr, *_rest) in inc:
            by_prio[pr] = by_prio.get(pr, 0) + 1
            by_type[ty] = by_type.get(ty, 0) + 1
        out["resolved_by_prio"] = by_prio
        out["resolved_by_type"] = by_type
    return out


def build(args=None):
    os.makedirs(BIN, exist_ok=True)
    cf = ["cc", "-std=gnu11", "-O2", "-w", "-DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS", "-DVIRTUAL_BUS_ENABLED=1",
          "-DCONSENSUS_ENFORCE=1", "-Icore", "-Ilaptop"]
    srcs = [os.path.join("core", f) for f in sorted(os.listdir(os.path.join(ROOT, "core"))) if f.endswith(".c")]
    srcs += ["laptop/main.c", "laptop/debug.c", "laptop/snapshot.c", "laptop/mission_feed.c", "laptop/mission_mirror.c"]
    # Build to a temporary name and rename, so a run in progress never execs a half-written binary.
    subprocess.check_call(cf + ["-o", os.path.join(BIN, "noai.tmp")] + srcs + ["-lm"], cwd=ROOT)
    os.replace(os.path.join(BIN, "noai.tmp"), os.path.join(BIN, "noai"))
    subprocess.check_call(cf + ["-DFAULT_MODEL_LOADED=1", "-o", os.path.join(BIN, "ai.tmp")] + srcs + ["-lm"], cwd=ROOT)
    os.replace(os.path.join(BIN, "ai.tmp"), os.path.join(BIN, "ai"))


def run_one(out, cfg, stress, seed, ticks, keep, set_name):
    binary, cargs = CONFIGS[cfg]
    d = os.path.join(out, f"s{stress:g}", cfg)
    os.makedirs(d, exist_ok=True)
    res = os.path.join(d, f"{seed}.json")
    if os.path.exists(res):
        return
    recdir = os.path.join(out, f"s{stress:g}", "_events")
    os.makedirs(recdir, exist_ok=True)
    rec = os.path.join(recdir, f"{seed}.txt")
    with tempfile.TemporaryDirectory(prefix=f"hm{seed}_") as work:
        if cfg == "NOFAULT":
            cmd = [os.path.join(BIN, binary), str(seed), "1", str(ticks), "--fault-profile=none", "--diag", "--probe-diag"]
            cmd = [c for c in cmd if c != "--probe-diag"]
        else:
            cmd = [os.path.join(BIN, binary), str(seed), "1", str(ticks)] + V1 + [f"--fault-stress={stress:g}"]
            cmd += [a.replace("@rec", rec) for a in cargs]
            if any("@rec" in a and "events" in a for a in cargs) and not os.path.exists(rec):
                raise RuntimeError(f"event recording {rec} missing: run P first for stress {stress}")
        t0 = time.time()
        p = subprocess.run(cmd, cwd=work, capture_output=True, text=True)
        if p.returncode != 0:
            raise RuntimeError(f"{' '.join(cmd)} exited {p.returncode}\n{p.stderr[-300:]}")
        r = parse(p.stderr)
        r.update({"seed": seed, "config": cfg, "stress": stress, "wall_s": round(time.time() - t0, 1)})
        if keep:
            kd = os.path.join(d, "raw", str(seed))
            os.makedirs(kd, exist_ok=True)
            for f in ("tron_snapshot_dataset.csv", "tron_causal_truth.csv"):
                if os.path.exists(os.path.join(work, f)):
                    os.replace(os.path.join(work, f), os.path.join(kd, f))
    with open(res + ".tmp", "w") as fh:
        json.dump(r, fh)
    os.replace(res + ".tmp", res)


def parse_seeds(text):
    if "-" in text:
        lo, hi = text.split("-")
        return list(range(int(lo), int(hi) + 1))
    return [int(x) for x in text.split(",")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["build", "run", "list"])
    ap.add_argument("--configs", default="")
    ap.add_argument("--stress", default="2,7")
    ap.add_argument("--set", default="D", choices=["D", "H"])
    ap.add_argument("--seeds", default="")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--ticks", type=int, default=3000)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "headroom", "res"))
    ap.add_argument("--keep", action="store_true", help="keep the dataset and truth CSVs per seed (D1)")
    a = ap.parse_args()
    if a.cmd == "list":
        for k, (b, v) in CONFIGS.items():
            print(k, b, " ".join(v) if v is not None else "(fault-profile=none)")
        return
    if a.cmd == "build":
        build()
        return
    seeds = parse_seeds(a.seeds or ("1001-1100" if a.set == "D" else "3001-3100"))
    names = [c for c in a.configs.split(",") if c]
    for c in names:
        if c not in CONFIGS:
            sys.exit(f"unknown config {c}")
    stresses = [float(s) for s in a.stress.split(",")]
    tasks = []
    # P first within a stress (it writes the event recordings the foresight probes read)
    for st in stresses:
        for c in sorted(names, key=lambda n: (n != "P", names.index(n))):
            for s in seeds:
                tasks.append((c, st, s))
    done = [0]
    t0 = time.time()

    def work(t):
        run_one(a.out, t[0], t[1], t[2], a.ticks, a.keep, a.set)
        done[0] += 1
        if done[0] % 50 == 0:
            print(f"  {done[0]}/{len(tasks)} {time.time() - t0:.0f}s", flush=True)

    # P must finish for a stress before any foresight config reads its events: run in two phases
    need_events = [t for t in tasks if any("@rec" in x and "events" in x for x in (CONFIGS[t[0]][1] or []))]
    rest = [t for t in tasks if t not in need_events]
    for phase in (rest, need_events):
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            list(pool.map(work, phase))
    print(f"done {len(tasks)} runs in {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()

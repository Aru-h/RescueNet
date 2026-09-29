#!/usr/bin/env python3
"""Measurement driver for peer consensus (docs/architecture/CONSENSUS_DESIGN.md 7.2 and D1).

Builds the scenario binary once per compile-time variant and runs the
`table` and `compare` modes of laptop/tools/consensus_scenario_test.c in
parallel, then prints the tables as Markdown.

  consensus_sweep.py table   [--out DIR] [--seeds 1:5] [--ticks 1500] [--jobs N]
  consensus_sweep.py compare [--out DIR] [--seeds 1:5] [--ticks 3000] [--jobs N]

`table`  : false ELEVATED and detection per loss model x q, with the fast path
           on and off, and periodic vs random comm-fault emission.
`compare`: the D1 comparison. ENFORCE=0 (shadow), ENFORCE=1 with the local cap
           off (the shipped setting) and ENFORCE=1 with CONSENSUS_CAP_LOCAL_
           EVALUATORS=1: detection, missed recoveries on short faults, and
           false escalations, scored against the fault injector's ground truth.

Every number depends on simulator behaviour: re-run after the base branch
changes (the FIX branch changes the twin, quarantine and sweeps).
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

MODELS = ["iid", "src", "ge3", "ge10", "burst"]
LOSSES = ["0.00", "0.05", "0.10", "0.20", "0.40"]


def build(out, name, defs):
    binary = os.path.join(out, name)
    subprocess.run(
        ["make", "-s", "build-consensus-scenario", f"SCEN_BIN={binary}", f"SCEN_DEFS={defs}"],
        cwd=ROOT, check=True)
    return binary


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout.strip(), r.returncode


def table(args):
    variants = {
        "fast-on/periodic": "-DCONSENSUS_ENFORCE=1",
        "fast-off/periodic": "-DCONSENSUS_ENFORCE=1 -DCONSENSUS_FAST_PATH=0",
        "fast-on/random": "-DCONSENSUS_ENFORCE=1 -DTEAM_TEST_RANDOM_EMISSION=1",
        "fast-off/random": "-DCONSENSUS_ENFORCE=1 -DCONSENSUS_FAST_PATH=0 -DTEAM_TEST_RANDOM_EMISSION=1",
    }
    binaries = {k: build(args.out, "t_" + re.sub(r"\W", "_", k), v) for k, v in variants.items()}
    jobs = []
    for vname, binary in binaries.items():
        for model in MODELS:
            for q in LOSSES:
                if q == "0.00" and model != "iid":
                    continue        # zero loss is the same under every model
                jobs.append((vname, model, q,
                             [binary, "table", "--model", model, "--loss", q,
                              "--seeds", args.seeds, "--ticks", str(args.ticks)]))
    results = {}
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run, j[3]): j for j in jobs}
        for f in cf.as_completed(futs):
            vname, model, q, _ = futs[f]
            out, rc = f.result()
            results[(vname, model, q)] = out
            print(f"[done] {vname} {model} q={q}", file=sys.stderr)
    raw = os.path.join(args.out, "table_raw.txt")
    with open(raw, "w") as fh:
        for k in sorted(results):
            fh.write(f"{k}\t{results[k]}\n")
    print(f"raw rows in {raw}", file=sys.stderr)
    return results


def compare(args):
    variants = {
        "ENFORCE=0 (shadow)": "-DCONSENSUS_ENFORCE=0",
        "ENFORCE=1, cap off (shipped)": "-DCONSENSUS_ENFORCE=1",
        "ENFORCE=1, cap on": "-DCONSENSUS_ENFORCE=1 -DCONSENSUS_CAP_LOCAL_EVALUATORS=1",
    }
    binaries = {k: build(args.out, "c_" + re.sub(r"\W", "_", k), v) for k, v in variants.items()}
    jobs = []
    for vname, binary in binaries.items():
        for profile in ["none", "mobility", "communication", "combined"]:
            jobs.append((vname, profile,
                         [binary, "compare", "--profile", profile, "--loss", "0.05",
                          "--seeds", args.seeds, "--ticks", str(args.ticks)]))
    results = {}
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(run, j[2]): j for j in jobs}
        for f in cf.as_completed(futs):
            vname, profile, _ = futs[f]
            out, rc = f.result()
            results[(vname, profile)] = out
            print(f"[done] {vname} {profile}", file=sys.stderr)
    raw = os.path.join(args.out, "compare_raw.txt")
    with open(raw, "w") as fh:
        for k in sorted(results):
            fh.write(f"{k}\t{results[k]}\n")
    print(f"raw rows in {raw}", file=sys.stderr)
    return results


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["table", "compare"])
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "consensus-sweep"))
    ap.add_argument("--seeds", default="1:5")
    ap.add_argument("--ticks", type=int, default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    if args.ticks is None:
        args.ticks = 1500 if args.mode == "table" else 3000
    res = table(args) if args.mode == "table" else compare(args)
    for k in sorted(res):
        print(k, res[k])


if __name__ == "__main__":
    main()

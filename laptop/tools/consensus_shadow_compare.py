#!/usr/bin/env python3
"""Shadow-mode regression for peer consensus (CONSENSUS_DESIGN.md 7.7).

With CONSENSUS_ENFORCE=0 the virtual bus and consensus run, but nothing they
compute may reach the simulation. This runs a baseline `tron` (built from the
base ref) and a candidate `tron` (this tree, virtual bus on, enforcement off)
with the same seeds and profiles, and checks that every column the baseline
has is identical in the candidate, row for row. The candidate's appended
consensus columns are ignored.

run_id is the one exception: it is the process start time plus the seed, so it
differs between any two runs made in different seconds.

usage: consensus_shadow_compare.py BASE_TRON CANDIDATE_TRON [--seeds N] [--ticks T]
"""
import argparse
import os
import subprocess
import sys
import tempfile

PROFILES = ["none", "combined", "communication"]


def run(binary, workdir, seeds, ticks, profile):
    subprocess.run(
        [binary, "1", str(seeds), str(ticks), f"--fault-profile={profile}"],
        cwd=workdir, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return os.path.join(workdir, "tron_snapshot_dataset.csv")


def compare(base_csv, cand_csv):
    with open(base_csv) as b, open(cand_csv) as c:
        base_header = b.readline().rstrip("\n").split(",")
        cand_header = c.readline().rstrip("\n").split(",")
        n = len(base_header)
        if cand_header[:n] != base_header:
            return f"candidate header does not start with the baseline header"
        rows = 0
        for line_b, line_c in zip(b, c):
            rows += 1
            fb = line_b.rstrip("\n").split(",")
            fc = line_c.rstrip("\n").split(",")[:n]
            if fb[1:] != fc[1:]:      # skip run_id (index 0)
                for i, (x, y) in enumerate(zip(fb, fc)):
                    if i and x != y:
                        return f"row {rows}: column {base_header[i]}: base {x} != candidate {y}"
                return f"row {rows}: differs"
        extra_b = b.readline()
        extra_c = c.readline()
        if extra_b or extra_c:
            return "different row counts"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base")
    ap.add_argument("candidate")
    ap.add_argument("--seeds", type=int, default=3)
    ap.add_argument("--ticks", type=int, default=1500)
    args = ap.parse_args()

    failed = 0
    for profile in PROFILES:
        with tempfile.TemporaryDirectory() as db, tempfile.TemporaryDirectory() as dc:
            base_csv = run(os.path.abspath(args.base), db, args.seeds, args.ticks, profile)
            cand_csv = run(os.path.abspath(args.candidate), dc, args.seeds, args.ticks, profile)
            err = compare(base_csv, cand_csv)
            if err:
                failed += 1
                print(f"shadow regression FAILED, profile {profile}: {err}")
            else:
                print(f"shadow regression ok: profile {profile}, {args.seeds} seeds x {args.ticks} ticks, "
                      f"all pre-existing columns identical")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

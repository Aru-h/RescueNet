#!/usr/bin/env python3
"""Shadow-mode regression for peer consensus (CONSENSUS_DESIGN.md 7.7).

With CONSENSUS_ENFORCE=0 the virtual bus and consensus run, but nothing they
compute may reach the simulation. This runs a baseline `tron` (built from the
base ref) and a candidate `tron` (this tree, virtual bus on, enforcement off)
with the same seeds and profiles, and checks that every column the baseline
has is identical in the candidate, row for row. The candidate's appended
consensus AND evidence columns (evidence_progress..evidence_timeout,
confirmed_mobility_impairment - feature-contract Decision 2, added after the
base ref) are ignored, being past the baseline's own column count.

EXCLUDED_COLUMNS lists pre-existing columns dropped from the comparison
entirely - only run_id: base computes it as process-start-time + seed (the
old scheme); candidate uses a deterministic composite hash of world_seed/
fault profile/CONSENSUS_ENFORCE/vbus link loss (feature-contract Decision
5), with no dependence on wall-clock time. The two schemes necessarily
disagree even for identical run configuration - this isn't the "different
process, different second" flakiness the old comment on this line
described, it's every run, by construction. There is no sentinel mapping
that would make the two run_id schemes agree, so unlike the columns below
it is skipped outright rather than normalized.

REMAPPED_COLUMNS lists pre-existing columns compared *after* mapping the
base's old no-task sentinel to the candidate's new one, so a genuine
regression in either still fails the check - only the documented,
intentional difference is absorbed:

  incident_priority,
  victim_count       feature-contract Decision 4: these now read 0, not -1,
                     on a row where the team has no incident assigned (base
                     still writes -1). Mapped as base_value = 0 if base_value
                     == "-1" else base_value, then compared normally.
                     incident_req_capability is untouched - it keeps -1 as
                     its own one-hot "none" category on both sides.

Numeric columns are compared as floats within FLOAT_TOLERANCE, not by
exact string equality: the candidate now writes every continuous column
at %.9g (feature-contract parity fix) while the base ref still writes
its old fixed-decimal-place formats (%.2f/%.3f/%.4f/%.6f), so the same
underlying value legitimately prints as different text on the two sides.
0.01 is generous enough to absorb the base's worst-case rounding (2
decimal places) while still catching a real behavioral difference of
any size that matters. A column that isn't parseable as a float on
either side (there are none left in this CSV, but the fallback is kept
defensive) still needs exact string equality.

usage: consensus_shadow_compare.py BASE_TRON CANDIDATE_TRON [--seeds N] [--ticks T]
"""
import argparse
import os
import subprocess
import sys
import tempfile

PROFILES = ["none", "combined", "communication"]

# See the module docstring.
EXCLUDED_COLUMNS = {"run_id"}
REMAPPED_COLUMNS = {"incident_priority", "victim_count"}
FLOAT_TOLERANCE = 0.01


def _values_match(x, y):
    if x == y:
        return True
    try:
        return abs(float(x) - float(y)) <= FLOAT_TOLERANCE
    except ValueError:
        return False


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
        excluded_idx = {i for i, name in enumerate(base_header) if name in EXCLUDED_COLUMNS}
        remapped_idx = {i for i, name in enumerate(base_header) if name in REMAPPED_COLUMNS}
        rows = 0
        for line_b, line_c in zip(b, c):
            rows += 1
            fb = line_b.rstrip("\n").split(",")
            fc = line_c.rstrip("\n").split(",")[:n]
            for i, (x, y) in enumerate(zip(fb, fc)):
                if i in excluded_idx:
                    continue
                if i in remapped_idx and x == "-1":
                    x = "0"
                if not _values_match(x, y):
                    return f"row {rows}: column {base_header[i]}: base {x} != candidate {y}"
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

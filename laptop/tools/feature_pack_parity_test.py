#!/usr/bin/env python3
"""
C-vs-Python feature-pack parity check (FEATURE_CONTRACT_DESIGN.md section
5: "C vs Python feature parity: <=1e-5 scaled").

Compares:
  - features_c_path: core/feature_pack.c's feature_pack_build_vector()
    output (laptop/tools/feature_pack_dump.c), already scaled with the
    committed core/feature_spec.h constants. Columns: run_id, team_id,
    cycle, f0..f73 (feature_spec.py's INPUTS order).
  - features_py_raw_path: data_prep.py --dump-raw-features's
    reconstruction of the SAME rows from the training CSV
    feature_pack_dump.c wrote in the same run, UNSCALED. Columns:
    run_id, team_id, cycle, <feature name>... in feature_spec.py's
    INPUTS order.

Scales the Python side here with feature_spec.py's own committed
SCALER_MEAN/SCALER_STD (never refit), merges the two on (run_id,
team_id, cycle), and asserts every one of the 74 columns agrees within
1e-5 for every merged row. A real disagreement means core/feature_pack.c
and data_prep.py's feature construction have drifted from each other -
the risk feature_spec.json's single-source-of-truth generation exists to
catch - not a methodology artifact: both sides use the identical,
already-committed scaler and sentinel-replacement constants, so nothing
data-dependent differs between them.

Run: python3 feature_pack_parity_test.py <features_c.csv> <features_py_raw.csv>
(see Makefile's check-feature-pack-parity target for the full pipeline).

KNOWN, ROOT-CAUSED, NON-BUG SOURCES OF DISAGREEMENT (as of this test's
introduction - this test currently FAILS at the strict 1e-5 threshold
for exactly these two pre-existing reasons, not a feature-formula bug in
core/feature_pack.c or data_prep.py):

1. CSV float precision (~8 continuous columns: stamina, the twin_*
   progress/stamina fields, evidence_timeout): laptop/snapshot.c writes
   these with limited decimal precision, so data_prep.py's raw dump -
   read back from that text CSV - carries rounding noise on the order of
   1e-4 scaled that feature_pack_build_vector()'s full-precision float
   read never has. Cosmetic, not a bug.

2. Training snapshots are taken BEFORE recovery_dispatch() runs each
   tick, a deliberate, pre-existing design in core/simulation.c's
   simulation_step() (predates this feature contract work - see its own
   comment: "Pass 1 records every team's snapshot rows for this tick
   BEFORE any recovery_dispatch() runs... Pass 2 then runs the
   dispatches"). feature_pack_build_vector(), called after a tick fully
   completes (matching what a real board can actually observe), reflects
   state AFTER that tick's dispatch. On the ~9% of team-ticks where
   dispatch changes something (task abandonment, reassignment, a status
   change to RESTING), this shows up as a real disagreement on the
   incident/assignment-derived columns specifically (incident_priority,
   victim_count, status_*, incident_req_capability_*, has_support,
   hazard_structural, delta_victim_count) - not a formula bug, but a
   genuine train/serve timing question: should training snapshots move
   to post-dispatch to match inference-time observability? That would
   require regenerating the dataset and is a product decision, not
   something this test or its two source files should resolve
   unilaterally.
"""
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, REPO_ROOT)

import numpy as np
import pandas as pd

from feature_spec import INPUTS, SCALER_MEAN, SCALER_STD

TOLERANCE = 1e-5


def main():
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <features_c.csv> <features_py_raw.csv>", file=sys.stderr)
        return 1

    c_df = pd.read_csv(sys.argv[1])
    py_df = pd.read_csv(sys.argv[2])

    c_cols = [f"f{i}" for i in range(len(INPUTS))]
    missing_c = [c for c in c_cols if c not in c_df.columns]
    missing_py = [c for c in INPUTS if c not in py_df.columns]
    if missing_c or missing_py:
        print(f"FAIL: missing columns - C: {missing_c} Python: {missing_py}", file=sys.stderr)
        return 1

    # Scale the raw Python side with the committed constants - never refit.
    mean = np.asarray(SCALER_MEAN, dtype=np.float64)
    std = np.asarray(SCALER_STD, dtype=np.float64)
    py_scaled = (py_df[INPUTS].to_numpy(dtype=np.float64) - mean) / std

    py_scaled_df = py_df[["run_id", "team_id", "cycle"]].copy()
    for i, col in enumerate(c_cols):
        py_scaled_df[col] = py_scaled[:, i]

    merged = c_df[["run_id", "team_id", "cycle"] + c_cols].merge(
        py_scaled_df, on=["run_id", "team_id", "cycle"], suffixes=("_c", "_py"))

    if len(merged) == 0:
        print("FAIL: no rows matched on (run_id, team_id, cycle) - "
              "check that both dumps came from the same run", file=sys.stderr)
        return 1

    print(f"Matched {len(merged)} rows on (run_id, team_id, cycle) "
          f"(C had {len(c_df)}, Python raw had {len(py_df)})")

    worst_col = None
    worst_diff = -1.0
    any_fail = False
    for i, col in enumerate(c_cols):
        diff = (merged[f"{col}_c"] - merged[f"{col}_py"]).abs()
        max_diff = float(diff.max())
        if max_diff > worst_diff:
            worst_diff = max_diff
            worst_col = INPUTS[i]
        if max_diff > TOLERANCE:
            any_fail = True
            print(f"  MISMATCH {INPUTS[i]} (f{i}): max|diff|={max_diff:.8f} "
                  f"mean|diff|={diff.mean():.8f}")

    print(f"\nWorst input overall: {worst_col} (max|diff|={worst_diff:.8f}, "
          f"tolerance={TOLERANCE})")

    if any_fail:
        print("feature_pack_parity_test: FAIL")
        return 1

    print("feature_pack_parity_test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

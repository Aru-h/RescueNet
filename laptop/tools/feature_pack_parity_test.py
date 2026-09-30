#!/usr/bin/env python3
"""
C-vs-Python feature-pack parity check (docs/ml/FEATURE_CONTRACT_DESIGN.md section
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

Passes all 74 columns within 1e-5 (worst column typically ~5e-8, float32
epsilon) as of the fixes below - it did not always. Two real, root-caused
issues surfaced while building this test and were fixed rather than
tolerated:

1. CSV float precision: laptop/snapshot.c used to write continuous
   columns (stamina, the twin_* progress/stamina fields, evidence_*) at
   a fixed decimal-place count (%.2f/%.4f/%.6f), so data_prep.py's raw
   dump - read back from that text CSV - carried rounding noise the
   full-precision float read never had. Fixed: every float column is
   now %.9g (round-trips a float32 exactly). This changes every already-
   collected dataset's exact byte values - regenerate before comparing
   an old dataset against a new one.

2. Training-snapshot timing vs. this test's own sampling point: training
   rows are deliberately captured BEFORE recovery_dispatch() runs each
   tick (core/simulation.c's Pass 1/Pass 2 split - a team's own
   situation, not the recovery system's decisions about it, is what the
   fault predictor should see). This test used to call feature_pack_
   build_vector() itself, from outside, after a tick had fully finished
   (i.e. after dispatch) - a different, unfair sampling point, not a
   real bug in either production file. Fixed with a real design change,
   not just a test-methodology fix: core/feature_pack.c now caches the
   vector at Pass-1 time every tick (feature_pack_cache_tick(), called
   from core/simulation.c just before recovery_manager_step()), and
   both core/recovery_manager.c's evaluate_fault_predictor() and this
   test's own feature_pack_dump.c read that same cached vector
   (feature_pack_get_cached_vector()) instead of building fresh - see
   core/feature_pack.h's own doc for the full reasoning.

A third, smaller issue surfaced only once (1) and (2) were fixed and the
mismatch shrank to one column at ~1.5e-5: generate_feature_spec.py wrote
core/feature_spec.h's SCALER_MEAN/SCALER_STD arrays at a fixed 8 decimal
places, which silently lost precision on small-magnitude columns
(evidence_timeout's mean ~0.0007, std ~0.009) relative to the full-
precision values feature_spec.py's own SCALER_MEAN/SCALER_STD already
had. Fixed: both are now %.9g via a small helper that also guards the
one case %.9g alone gets wrong for a C literal - an exact 0.0 formats as
bare "0", and "0f" is not a valid C floating constant (appends ".0"
when the formatted text has neither a decimal point nor an exponent).
"""
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO_ROOT, "ml"))

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
    # float32, not float64: core/feature_pack.c does this division in a
    # 32-bit float, and FEATURE_SPEC_SCALER_MEAN/STD are themselves
    # float32 arrays - matching that precision here, not just the
    # formula, is what closes the gap to bit-level agreement rather than
    # leaving a ~1e-5 float32-vs-float64 rounding residual on every
    # column, most visible on the ones with a small std.
    mean = np.asarray(SCALER_MEAN, dtype=np.float32)
    std = np.asarray(SCALER_STD, dtype=np.float32)
    py_scaled = (py_df[INPUTS].to_numpy(dtype=np.float32) - mean) / std

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

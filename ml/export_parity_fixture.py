"""
export_parity_fixture.py -- writes the PyTorch-vs-C model parity fixture:
laptop/tools/model_parity_fixture.csv, checked by `make check-model-parity`
(laptop/tools/model_parity_test.c).

Each row is one already-SCALED 75-float input vector (the shape
fault_predictor_run() takes - feature_pack.c applies the scaler, the
predictor never does) plus the 5 sigmoid scores the trained PyTorch model
(eval mode, float32) produces for it. Rows are a mix of real dataset rows
(evenly spaced) and edge cases: all
zeros, per-feature max / min of the scaled test set, and uniform +/-
large values.

Needs fault_predictor_checkpoint.pt in the working directory, plus real rows
from EITHER data_prep_output/X_test_scaled.npy OR --real-rows <features_c.csv>
(the scaled vectors laptop/tools/feature_pack_dump.c writes from a real
simulator run: `feature_pack_dump 5 300 1001 features_c.csv`). Regenerate after every retrain/re-export (repo root):

    python ml/export_parity_fixture.py
    git add laptop/tools/model_parity_fixture.csv

Columns: name, in0..in74, out0..out4 (floats written with %.9g, which
round-trips float32 exactly).
"""

import argparse
import os
import sys

import numpy as np
import torch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export import CHECKPOINT_PATH, DATA_DIR, load_checkpoint, rebuild_model  # noqa: E402

FIXTURE_PATH = "laptop/tools/model_parity_fixture.csv"
NUM_REAL_ROWS = 12


def load_real_rows(path):
    if path:
        import pandas as pd
        df = pd.read_csv(path)
        return df[[c for c in df.columns if c.startswith("f")
                   and c[1:].isdigit()]].to_numpy(dtype=np.float32)
    return np.load(f"{DATA_DIR}/X_test_scaled.npy").astype(np.float32)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--real-rows", help="scaled feature CSV from feature_pack_dump (instead of X_test_scaled.npy)")
    ap.add_argument("--checkpoint", default=CHECKPOINT_PATH)
    args = ap.parse_args()
    ckpt = load_checkpoint(args.checkpoint)
    model, config = rebuild_model(ckpt)
    n_in = config.input_dim

    x_test = load_real_rows(args.real_rows)
    if x_test.shape[1] != n_in:
        raise ValueError(f"X_test_scaled has {x_test.shape[1]} columns, model expects {n_in}")

    names, rows = [], []
    picks = np.linspace(0, len(x_test) - 1, NUM_REAL_ROWS).astype(int)
    for k, i in enumerate(picks):
        names.append(f"real_row_{int(i)}")
        rows.append(x_test[i])

    col_max = x_test.max(axis=0).astype(np.float32)
    col_min = x_test.min(axis=0).astype(np.float32)
    edge = [
        ("edge_all_zeros", np.zeros(n_in, np.float32)),
        ("edge_data_max", col_max),
        ("edge_data_min", col_min),
        ("edge_all_plus5", np.full(n_in, 5.0, np.float32)),
        ("edge_all_minus5", np.full(n_in, -5.0, np.float32)),
        ("edge_alternating", np.where(np.arange(n_in) % 2 == 0, 3.0, -3.0).astype(np.float32)),
        ("edge_all_plus50", np.full(n_in, 50.0, np.float32)),  # saturates the sigmoids
    ]
    for name, vec in edge:
        names.append(name)
        rows.append(vec)

    x = torch.from_numpy(np.stack(rows).astype(np.float32))
    with torch.no_grad():
        y = torch.sigmoid(model(x)).numpy().astype(np.float32)

    os.makedirs(os.path.dirname(FIXTURE_PATH), exist_ok=True)
    with open(FIXTURE_PATH, "w") as f:
        f.write("name," + ",".join(f"in{i}" for i in range(n_in)) + ","
                + ",".join(f"out{k}" for k in range(y.shape[1])) + "\n")
        for name, vec, out in zip(names, x.numpy(), y):
            f.write(name + "," + ",".join(f"{v:.9g}" for v in vec) + ","
                    + ",".join(f"{v:.9g}" for v in out) + "\n")
    print(f"wrote {len(names)} vectors ({len(picks)} real, {len(edge)} edge) to {FIXTURE_PATH}")


if __name__ == "__main__":
    main()

"""
export.py -- folds a trained FaultPredictorNet checkpoint (model.py) into the
flat C-array weights fault_predictor.c's hand-written forward pass consumes.

Three pieces:
  (a) load the .pt checkpoint, walk its actual nn.Module tree, fold each
      Linear+BatchNorm pair into one effective weight/bias.
  (b) extract final per-layer weight/bias arrays, plus scaler.pkl's
      mean_/std_.
  (c) write all of it as C arrays into fault_predictor_weights.h, using the
      exact same names/shapes locked in fault_predictor.h/
      fault_predictor_weights.h already.

Run this only against a checkpoint you have declared final (after evaluate.py's
numbers are judged acceptable). Running it against an in-progress checkpoint
produces a valid, loadable header -- just not the accepted final one. Nothing
in this script enforces that; it is a workflow rule, not a code guard.

Usage (from the repo root; copy the output into core/ afterwards):
    python ml/export.py
    cp fault_predictor_weights.h core/

dtype: float32 only. The model is ~18.3K parameters (about 73 KB as float32),
which fits comfortably in the STM32N6570-DK's memory, and the target has a
hardware FPU, so float32 is a direct, faithful port of what model.py computes
with nothing extra to get subtly wrong. The int16 path is stubbed below, not
implemented -- switch QUANT_DTYPE only if float32 ever fails the tick budget,
and only after real quantization logic replaces the stub.
"""

import os
import json
import pickle

import numpy as np
import torch
import torch.nn as nn

from model import build_model, FaultPredictorNet, FAULT_CLASS_NAMES

DATA_DIR = "data_prep_output"
CHECKPOINT_PATH = "fault_predictor_checkpoint.pt"
SCALER_PATH = f"{DATA_DIR}/scaler.pkl"
WEIGHTS_HEADER_PATH = "fault_predictor_weights.h"

# Must match fault_predictor.h / fault_predictor_weights.h exactly -- these
# are NOT re-derived from the checkpoint, they're the locked contract both
# sides were built against. If the checkpoint's real shape doesn't match,
# that's a signal something upstream (hidden_dims, input_dim) drifted from
# what the C side was built for -- fail loudly, don't silently adapt.
#
# 75 = the current feature contract (core/feature_spec.h's
# FEATURE_SPEC_NUM_INPUTS, from data_prep.py's feature_columns incl.
# path_is_unreachable, env_raw_travel_cost and incident_req_capability
# with its 6 categories -1..4). Must move together with
# FEATURE_SPEC_NUM_INPUTS, or check_shape_matches_locked_contract() below
# will (correctly) refuse to export.
EXPECTED_INPUT_DIM = 75
EXPECTED_HIDDEN_DIMS = (128, 64)
EXPECTED_OUTPUT_DIM = 5

QUANT_DTYPE = "float32"  # only supported value right now -- see module docstring


def load_checkpoint(path):
    # weights_only=False: PyTorch 2.6+ defaults torch.load to weights_only=True,
    # which refuses to unpickle train.py's embedded FaultPredictorConfig object
    # (a plain dataclass, not a security concern here). This is our own
    # checkpoint, produced locally by train.py, not a downloaded/untrusted
    # file -- weights_only=False is the correct call for this specific case,
    # not a blanket "always disable" pattern to copy elsewhere.
    ckpt = torch.load(path, map_location="cpu", weights_only=False)

    required_keys = {"model_state_dict", "config", "class_names", "feature_columns"}
    missing = required_keys - ckpt.keys()
    if missing:
        raise ValueError(
            f"Checkpoint at {path} is missing expected key(s) {missing} -- "
            "this doesn't look like a train.py checkpoint. Not proceeding."
        )

    if ckpt["class_names"] != FAULT_CLASS_NAMES:
        raise ValueError(
            f"Checkpoint class order {ckpt['class_names']} doesn't match "
            f"model.py's FAULT_CLASS_NAMES {FAULT_CLASS_NAMES}. This must match "
            "exactly, or every downstream score gets attributed to the wrong "
            "fault class on the board -- not proceeding."
        )

    return ckpt


def rebuild_model(ckpt):
    """Reconstruct the exact architecture the checkpoint was trained with,
    then load its trained weights into it -- rather than trying to read
    weight tensors directly out of the raw state_dict by string key. This
    way BatchNorm folding below walks real nn.Module objects (so it works
    regardless of exact hidden_dims/dropout/use_batchnorm, rather than
    hardcoding trunk.0/trunk.1/trunk.4/trunk.5-style indices that shift the
    moment any of those change)."""
    config = ckpt["config"]
    model = build_model(
        input_dim=config.input_dim,
        class_names=ckpt["class_names"],
        hidden_dims=config.hidden_dims,
        num_classes=config.num_classes,
        dropout=config.dropout,
        use_batchnorm=config.use_batchnorm,
    )
    model.load_state_dict(ckpt["model_state_dict"])
    model.eval()
    return model, config


def check_shape_matches_locked_contract(config, input_dim):
    """The C side (fault_predictor_weights.h) has fixed array dimensions.
    If the checkpoint's real architecture doesn't match what those arrays
    were sized for, every downstream write below would silently produce a
    header that doesn't fit fault_predictor.c's forward pass. Fail here,
    loudly, instead."""
    problems = []
    if input_dim != EXPECTED_INPUT_DIM:
        problems.append(
            f"input_dim={input_dim}, expected {EXPECTED_INPUT_DIM} "
            "(feature_columns.pkl length changed since fault_predictor.h "
            "was written -- update FEATURE_SPEC_NUM_INPUTS there too, not "
            "just here)"
        )
    if tuple(config.hidden_dims) != EXPECTED_HIDDEN_DIMS:
        problems.append(
            f"hidden_dims={tuple(config.hidden_dims)}, expected "
            f"{EXPECTED_HIDDEN_DIMS} (the C-side arrays were sized for this "
            "shape; if hidden_dims was changed on purpose, "
            "fault_predictor_weights.h's W1/W2/W3 array shapes need updating "
            "to match before this script's output will compile correctly.)"
        )
    if config.num_classes != EXPECTED_OUTPUT_DIM:
        problems.append(
            f"num_classes={config.num_classes}, expected {EXPECTED_OUTPUT_DIM}"
        )
    if not config.use_batchnorm:
        problems.append(
            "config.use_batchnorm is False -- the folding logic below assumes "
            "every hidden Linear is followed by a BatchNorm1d. Not the case "
            "here; fold_linear_batchnorm() needs a real code change, not just "
            "a re-run, before this checkpoint can be exported."
        )
    if problems:
        raise ValueError(
            "Checkpoint architecture doesn't match the locked C-side contract:\n  - "
            + "\n  - ".join(problems)
        )


def fold_linear_batchnorm(linear: nn.Linear, bn: nn.BatchNorm1d):
    """Standard Linear+BatchNorm fold, eval-mode statistics (running_mean/
    running_var -- the ones actually used at inference time, not the
    per-batch stats used during training).

    BN(eval):  y = gamma * (z - running_mean) / sqrt(running_var + eps) + beta
    Linear:    z = W x + b

    Substituting:
      y = (gamma / sqrt(running_var + eps)) * (W x + b - running_mean) + beta
        = [(gamma / sqrt(running_var + eps)) * W] x
          + [(gamma / sqrt(running_var + eps)) * (b - running_mean) + beta]

    So: W_folded = W * scale[:, None],  b_folded = (b - running_mean) * scale + beta
        where scale = gamma / sqrt(running_var + eps)
    """
    W = linear.weight.detach().numpy().astype(np.float64)   # (out, in)
    b = linear.bias.detach().numpy().astype(np.float64)     # (out,)

    gamma = bn.weight.detach().numpy().astype(np.float64)
    beta = bn.bias.detach().numpy().astype(np.float64)
    running_mean = bn.running_mean.detach().numpy().astype(np.float64)
    running_var = bn.running_var.detach().numpy().astype(np.float64)
    eps = bn.eps

    scale = gamma / np.sqrt(running_var + eps)   # (out,)

    W_folded = W * scale[:, None]
    b_folded = (b - running_mean) * scale + beta

    return W_folded.astype(np.float32), b_folded.astype(np.float32)


def extract_folded_layers(model: FaultPredictorNet):
    """Walk model.trunk's actual children in order, pairing each Linear with
    the BatchNorm1d that immediately follows it (ReLU/Dropout have no
    parameters and are skipped). Falls back to raising rather than guessing
    if a Linear isn't followed by a BatchNorm1d -- see
    check_shape_matches_locked_contract()'s use_batchnorm guard, which
    should already have caught this, but this is the actual safety net.
    """
    trunk_children = list(model.trunk.children())
    folded_hidden_layers = []

    i = 0
    while i < len(trunk_children):
        layer = trunk_children[i]
        if isinstance(layer, nn.Linear):
            if i + 1 >= len(trunk_children) or not isinstance(trunk_children[i + 1], nn.BatchNorm1d):
                raise ValueError(
                    f"trunk[{i}] is a Linear not immediately followed by a "
                    "BatchNorm1d -- fold_linear_batchnorm() can't handle this "
                    "layout. (Expected Linear -> BatchNorm1d -> ReLU -> "
                    "[Dropout], repeated, per model.py's construction.)"
                )
            bn = trunk_children[i + 1]
            W_folded, b_folded = fold_linear_batchnorm(layer, bn)
            folded_hidden_layers.append((W_folded, b_folded))
            i += 2  # skip past the BatchNorm we just consumed
        else:
            i += 1  # ReLU / Dropout -- no parameters, nothing to fold

    if len(folded_hidden_layers) != len(EXPECTED_HIDDEN_DIMS):
        raise ValueError(
            f"Found {len(folded_hidden_layers)} Linear+BatchNorm pairs in "
            f"model.trunk, expected {len(EXPECTED_HIDDEN_DIMS)} "
            f"(one per hidden_dims entry, {EXPECTED_HIDDEN_DIMS})."
        )

    # Final output layer: plain Linear, no BatchNorm, no folding needed.
    W_out = model.output_layer.weight.detach().numpy().astype(np.float32)
    b_out = model.output_layer.bias.detach().numpy().astype(np.float32)

    return folded_hidden_layers, (W_out, b_out)


class SafeStandardScaler:
    """Stub, NOT a real re-implementation of data_prep.py's class -- exists
    only so pickle can resolve the class name when unpickling scaler.pkl.
    data_prep.py has no `if __name__ == "__main__":` guard, so it always
    runs as the top-level script, meaning its SafeStandardScaler got
    __module__="__main__" baked into the pickle at save time. export.py is
    also run directly (python export.py), so IT is __main__ in this
    process too -- defining a class of the same name here (before
    pickle.load) satisfies that lookup. pickle fills in the real
    attributes (mean_, std_, zero_var_mask_) from the pickled data
    regardless of this stub's body -- we only ever read mean_/std_ off
    the result below, never call fit()/transform(), so no real logic is
    needed here."""
    pass


def load_scaler(path):
    with open(path, "rb") as f:
        scaler = pickle.load(f)
    if not hasattr(scaler, "mean_") or not hasattr(scaler, "std_"):
        raise ValueError(
            f"{path} doesn't look like a SafeStandardScaler (data_prep.py) -- "
            "missing mean_/std_."
        )
    n_zero_var = int(getattr(scaler, "zero_var_mask_", np.array([])).sum())
    if n_zero_var:
        print(
            f"Note: {n_zero_var} feature(s) had zero variance at fit time "
            "(std_ floored to 1.0 by SafeStandardScaler, not a bug here)."
        )
    return scaler.mean_.astype(np.float32), scaler.std_.astype(np.float32)


# ── C header formatting ─────────────────────────────────────────────────
def c_float(v):
    """Formats a value as a valid C float literal. '{:.9g}' prints exact
    integers like 0.0 / 1.0 as '0' / '1', and '0f' / '1f' is NOT valid C
    ("invalid suffix f on integer constant") -- zero scaler means and the
    std=1.0 floor for zero-variance features hit this every run. Adds a
    '.0' when the text has no decimal point or exponent."""
    v = float(v)
    if not np.isfinite(v):
        raise ValueError(f"non-finite value {v} cannot be exported to a C header")
    text = f"{v:.9g}"
    if not any(ch in text for ch in ".eE"):
        text += ".0"
    return text + "f"


def format_1d_array(name, values, values_per_line=8):
    lines = [f"static const float {name}[{len(values)}] = {{"]
    for start in range(0, len(values), values_per_line):
        chunk = values[start:start + values_per_line]
        body = ", ".join(c_float(v) for v in chunk)
        lines.append(f"    {body},")
    lines.append("};")
    return "\n".join(lines)


def format_2d_array(name, matrix):
    rows, cols = matrix.shape
    lines = [f"static const float {name}[{rows}][{cols}] = {{"]
    for row in matrix:
        body = ", ".join(c_float(v) for v in row)
        lines.append(f"    {{ {body} }},")
    lines.append("};")
    return "\n".join(lines)


def write_weights_header(path, scaler_mean, scaler_std, hidden_layers, output_layer, source_info):
    if QUANT_DTYPE != "float32":
        raise NotImplementedError(
            f"QUANT_DTYPE={QUANT_DTYPE!r} is not implemented. Only float32 "
            "export exists right now -- int16 quantization (scale factors, "
            "int32 accumulators in fault_predictor.c's forward pass) is real, "
            "unwritten work, not a one-line dtype swap. Don't flip this "
            "without writing that logic first."
        )

    (W1, b1), (W2, b2) = hidden_layers
    W3, b3 = output_layer

    header = f"""#ifndef FAULT_PREDICTOR_WEIGHTS_H
#define FAULT_PREDICTOR_WEIGHTS_H

#include "fault_predictor.h"   /* FEATURE_SPEC_NUM_INPUTS, NUM_OPERATIONAL_FAULT_CLASSES */

/*
 * Real trained weights, exported by export.py.
 *   source checkpoint : {source_info['checkpoint_path']}
 *   trained epoch      : {source_info['epoch']}
 *   val_loss           : {source_info['val_loss']:.6f}
 *   input_dim          : {source_info['input_dim']}
 *   hidden_dims        : {source_info['hidden_dims']}
 *   class order        : {source_info['class_names']}
 *
 * Linear+BatchNorm pairs already folded (fold_linear_batchnorm() in
 * export.py) -- fault_predictor.c's forward pass does NOT need a separate
 * BatchNorm step, W1/b1 and W2/b2 already account for it.
 *
 * dtype: float32. This is a generated file -- re-run export.py against a
 * (possibly new) final checkpoint rather than hand-editing these numbers.
 */

#define FAULT_PREDICTOR_HIDDEN1_DIM {W1.shape[0]}
#define FAULT_PREDICTOR_HIDDEN2_DIM {W2.shape[0]}
#define FAULT_PREDICTOR_OUTPUT_DIM  NUM_OPERATIONAL_FAULT_CLASSES   /* {W3.shape[0]} */

/* ---- Scaler (scaler.pkl's mean_ / std_, one entry per input feature) ---- */
{format_1d_array("scaler_mean", scaler_mean)}

{format_1d_array("scaler_std", scaler_std)}

/* ---- Hidden layer 1: Linear({W1.shape[1]} -> {W1.shape[0]}) + folded BatchNorm + ReLU ---- */
{format_2d_array("W1", W1)}

{format_1d_array("b1", b1)}

/* ---- Hidden layer 2: Linear({W2.shape[1]} -> {W2.shape[0]}) + folded BatchNorm + ReLU ---- */
{format_2d_array("W2", W2)}

{format_1d_array("b2", b2)}

/* ---- Output layer: Linear({W3.shape[1]} -> {W3.shape[0]}), raw logits (sigmoid applied in C) ---- */
{format_2d_array("W3", W3)}

{format_1d_array("b3", b3)}

#endif /* FAULT_PREDICTOR_WEIGHTS_H */
"""

    with open(path, "w") as f:
        f.write(header)


def check_matches_feature_spec(feature_columns, scaler_mean, scaler_std,
                               spec_path="feature_spec.json"):
    """Guards the two things that would silently break the board: the
    checkpoint's feature ORDER and the scaler must both equal the committed
    feature_spec.json (which core/feature_spec.h is generated from). The
    board scales inputs with feature_spec.h's constants, NOT this header's
    scaler_mean/scaler_std, so any mismatch here = wrong predictions."""
    if not os.path.exists(spec_path):
        print(f"WARNING: {spec_path} not found - skipped feature-order/scaler "
              "consistency check against the board's feature_spec.")
        return
    with open(spec_path) as f:
        spec = json.load(f)
    if list(feature_columns) != list(spec["inputs"]):
        raise ValueError(
            "Checkpoint feature_columns order != feature_spec.json inputs. "
            "The model was trained on a different feature set/order than the "
            "board builds - do not export.")
    if (not np.allclose(scaler_mean, spec["scaler_mean"], rtol=1e-5, atol=1e-6)
            or not np.allclose(scaler_std, spec["scaler_std"], rtol=1e-5, atol=1e-6)):
        raise ValueError(
            "scaler.pkl != feature_spec.json scaler_mean/std. core/feature_spec.h "
            "(what the board really scales with) would not match this "
            "model's training scaler - fix before exporting.")
    print("Consistency check OK: feature order + scaler match feature_spec.json.")


# ── Main ─────────────────────────────────────────────────────────────
def main():
    print(f"Loading checkpoint: {CHECKPOINT_PATH}")
    ckpt = load_checkpoint(CHECKPOINT_PATH)

    input_dim = len(ckpt["feature_columns"])
    model, config = rebuild_model(ckpt)
    check_shape_matches_locked_contract(config, input_dim)

    print(f"Checkpoint: epoch={ckpt['epoch']}, val_loss={ckpt['val_loss']:.6f}")
    print(f"Architecture: input_dim={input_dim}, hidden_dims={tuple(config.hidden_dims)}, "
          f"num_classes={config.num_classes}")

    print("Folding Linear+BatchNorm pairs...")
    hidden_layers, output_layer = extract_folded_layers(model)
    for idx, (W, b) in enumerate(hidden_layers, start=1):
        print(f"  hidden layer {idx}: W{W.shape}, b{b.shape}")
    print(f"  output layer: W{output_layer[0].shape}, b{output_layer[1].shape}")

    print(f"Loading scaler: {SCALER_PATH}")
    scaler_mean, scaler_std = load_scaler(SCALER_PATH)
    if len(scaler_mean) != input_dim:
        raise ValueError(
            f"scaler.pkl has {len(scaler_mean)} features, checkpoint has "
            f"{input_dim} -- these must come from the same data_prep.py run."
        )
    check_matches_feature_spec(ckpt["feature_columns"], scaler_mean, scaler_std)

    source_info = {
        "checkpoint_path": CHECKPOINT_PATH,
        "epoch": ckpt["epoch"],
        "val_loss": ckpt["val_loss"],
        "input_dim": input_dim,
        "hidden_dims": tuple(config.hidden_dims),
        "class_names": ckpt["class_names"],
    }

    print(f"Writing {WEIGHTS_HEADER_PATH}...")
    write_weights_header(WEIGHTS_HEADER_PATH, scaler_mean, scaler_std,
                          hidden_layers, output_layer, source_info)

    print(f"\nDone. {WEIGHTS_HEADER_PATH} now holds real trained weights.")
    print("Next (manual): cp fault_predictor_weights.h core/, then regenerate the "
          "parity fixture (python ml/export_parity_fixture.py) and run "
          "`make check-model-parity`. The model is built in only with "
          "-DFAULT_MODEL_LOADED=1 (`make tron-ai`); fault_predictor.c must NOT "
          "normalize its input again -- feature_pack_build_vector() already "
          "scales with feature_spec.h.")


if __name__ == "__main__":
    main()
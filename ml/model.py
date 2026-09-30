"""
model.py -- fault-predictor architecture.

One shared trunk, one final linear layer producing 5 independent logits -- one
per fault class (MOBILITY, COMMUNICATION, RESOURCE, ENVIRONMENT, MISSION).
Deliberately a single-head design: the task is present-tick detection (is a
fault active right now), so every row has a fully known multi-hot ground-truth
answer and there is no reason to split the network into separate heads.

This file has no CSV/data dependency and does not import data_prep.py, so it
can be reviewed and smoke-tested with a random tensor on its own
(`python model.py`).
"""

from dataclasses import dataclass, field
from typing import Sequence, Optional

import torch
import torch.nn as nn


# NUM_FAULT_CLASSES / FAULT_CLASS_NAMES are duplicated here (not imported from
# data_prep.py) on purpose — data_prep.py is a top-level script, not a library
# module: importing anything from it would execute its entire CSV-loading /
# preprocessing pipeline as a side effect the moment model.py is imported.
# fault_class_names.pkl (saved by data_prep.py) is the actual source of truth
# for class order at train/eval time — train.py should load that pickle and
# pass class_names in explicitly, rather than trusting the copy below.
FAULT_CLASS_NAMES = ["mobility", "communication", "resource", "environment", "mission"]
NUM_FAULT_CLASSES = len(FAULT_CLASS_NAMES)


@dataclass
class FaultPredictorConfig:
    """All architecture knobs in one place, so trying a different shape later
    is a one-line config change rather than a rewrite of the model class."""

    input_dim: int  # Not hardcoded: read from len(feature_columns.pkl) at
                     # train time (75 for the submitted model).
    hidden_dims: Sequence[int] = field(default_factory=lambda: (128, 64))
    num_classes: int = NUM_FAULT_CLASSES
    dropout: float = 0.2
    use_batchnorm: bool = True
    class_names: Optional[Sequence[str]] = None  # for bookkeeping/printing only


class FaultPredictorNet(nn.Module):
    """Shared trunk (Linear -> BatchNorm -> ReLU -> Dropout, repeated per
    hidden_dims entry) feeding one final Linear(last_hidden, num_classes)
    layer. forward() returns raw logits, not sigmoid probabilities (train.py
    uses BCEWithLogitsLoss)."""

    def __init__(self, config: FaultPredictorConfig):
        super().__init__()
        self.config = config

        layers = []
        in_dim = config.input_dim
        for hidden_dim in config.hidden_dims:
            layers.append(nn.Linear(in_dim, hidden_dim))
            if config.use_batchnorm:
                layers.append(nn.BatchNorm1d(hidden_dim))
            layers.append(nn.ReLU(inplace=True))
            if config.dropout > 0:
                layers.append(nn.Dropout(config.dropout))
            in_dim = hidden_dim

        self.trunk = nn.Sequential(*layers)
        self.output_layer = nn.Linear(in_dim, config.num_classes)

        self.class_names = list(config.class_names) if config.class_names else None
        if self.class_names is not None:
            assert len(self.class_names) == config.num_classes, (
                f"class_names has {len(self.class_names)} entries but "
                f"num_classes={config.num_classes} — these must match."
            )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """x: (batch, input_dim) -> logits: (batch, num_classes).

        Deliberately returns raw logits, not post-sigmoid probabilities —
        pair this with nn.BCEWithLogitsLoss in train.py, not nn.BCELoss.
        This also lets train.py use BCEWithLogitsLoss's built-in pos_weight
        argument for class-imbalance-aware loss, without a separate
        weighting mechanism.
        """
        features = self.trunk(x)
        return self.output_layer(features)

    @torch.no_grad()
    def predict_proba(self, x: torch.Tensor) -> torch.Tensor:
        """Convenience wrapper for evaluate.py / manual inspection — applies
        sigmoid and runs in eval/no_grad mode. NOT used internally by
        forward() or during training; training always goes through raw
        logits + BCEWithLogitsLoss."""
        was_training = self.training
        self.eval()
        try:
            return torch.sigmoid(self.forward(x))
        finally:
            self.train(was_training)

    def num_parameters(self) -> int:
        return sum(p.numel() for p in self.parameters() if p.requires_grad)


def build_model(input_dim: int, class_names: Optional[Sequence[str]] = None,
                 **overrides) -> FaultPredictorNet:
    """Small convenience factory — train.py can do
    `build_model(input_dim=X.shape[1], class_names=fault_class_names)`
    without importing FaultPredictorConfig directly."""
    config = FaultPredictorConfig(input_dim=input_dim, class_names=class_names, **overrides)
    return FaultPredictorNet(config)


if __name__ == "__main__":
    # Smoke test only -- no CSV, no real data. Confirms the architecture is
    # wired correctly (shapes, forward pass, predict_proba) using a random
    # tensor. 75 matches the current feature contract
    # (core/feature_spec.h's FEATURE_SPEC_NUM_INPUTS); the real value used
    # for training always comes from feature_columns.pkl, not from here.
    PLACEHOLDER_INPUT_DIM = 75

    model = build_model(input_dim=PLACEHOLDER_INPUT_DIM, class_names=FAULT_CLASS_NAMES)
    print(model)
    print(f"\nTrainable parameters: {model.num_parameters():,}")

    dummy_batch = torch.randn(16, PLACEHOLDER_INPUT_DIM)
    logits = model(dummy_batch)
    probs = model.predict_proba(dummy_batch)

    assert logits.shape == (16, NUM_FAULT_CLASSES), f"Unexpected logits shape: {logits.shape}"
    assert probs.shape == (16, NUM_FAULT_CLASSES), f"Unexpected probs shape: {probs.shape}"
    assert torch.all((probs >= 0) & (probs <= 1)), "predict_proba output outside [0, 1]"

    print(f"\nlogits shape: {tuple(logits.shape)}  (expect (16, {NUM_FAULT_CLASSES}))")
    print(f"probs shape:  {tuple(probs.shape)}  (expect (16, {NUM_FAULT_CLASSES}))")
    print("Smoke test passed -- architecture wiring is correct. This says "
          "nothing about whether these are the right hidden_dims/dropout/etc. "
          "for the real data; that is a training-time question.")
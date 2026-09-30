"""
train.py -- trains FaultPredictorNet (model.py) on the outputs of data_prep.py.

Loads the already-preprocessed, already-scaled arrays from data_prep_output/
(X_train_scaled.npy, y_train.npy, etc.) -- does NOT touch the raw CSV or
re-run any preprocessing. Run data_prep.py first.

Settings this file uses:
  - pos_weight: N/P per class (negatives over positives), from the class base
    rates naive_baseline.py reports -- mobility 3.21, communication 3.21,
    resource 3.22, environment 13.38, mission 11.15. Recompute if the
    dataset changes.
  - batch_size = 1024 -- large batch, deliberate: the model is tiny (~18.7K
    trainable parameters) so memory isn't the constraint, and fewer, bigger
    update steps are faster wall-clock on CPU than many small-batch steps.
  - epoch cap = 25, with early stopping on val loss (patience 2) -- not a
    fixed count. With millions of rows one epoch is already thousands of
    update steps, so the model usually converges well before the cap.
  - hidden_dims/dropout left at model.py's defaults (128, 64 / 0.2).
"""

import pickle
import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader

from model import build_model, FAULT_CLASS_NAMES

DATA_DIR = "data_prep_output"
CHECKPOINT_PATH = "fault_predictor_checkpoint.pt"

BATCH_SIZE = 1024
MAX_EPOCHS = 25
EARLY_STOP_PATIENCE = 2  # stop if val loss doesn't improve for this many epochs in a row
LEARNING_RATE = 1e-3     # Adam default; not tuned

# pos_weight, in FAULT_CLASS_NAMES order (mobility, communication, resource,
# environment, mission): N/P per class, from naive_baseline.py's base rates.
POS_WEIGHT = torch.tensor([3.21, 3.21, 3.22, 13.38, 11.15], dtype=torch.float32)
# order: mobility, communication, resource, environment, mission

DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")


# ── Dataset ──────────────────────────────────────────────────────────
class FaultDataset(Dataset):
    """Thin wrapper around the .npy arrays data_prep.py already produced --
    no transformation happens here, scaling/encoding is already done."""

    def __init__(self, X_path, y_path):
        self.X = np.load(X_path).astype(np.float32)
        self.y = np.load(y_path).astype(np.float32)
        assert len(self.X) == len(self.y), "X/y length mismatch"

    def __len__(self):
        return len(self.X)

    def __getitem__(self, idx):
        return torch.from_numpy(self.X[idx]), torch.from_numpy(self.y[idx])


# ── One epoch, train or eval ────────────────────────────────────────
def run_epoch(model, loader, loss_fn, optimizer=None):
    """optimizer=None -> eval mode, no gradient updates (used for val/test).
    optimizer given -> train mode, weights get updated."""
    is_train = optimizer is not None
    model.train(is_train)

    total_loss = 0.0
    total_rows = 0

    context = torch.enable_grad() if is_train else torch.no_grad()
    with context:
        for X_batch, y_batch in loader:
            X_batch = X_batch.to(DEVICE)
            y_batch = y_batch.to(DEVICE)

            if is_train:
                optimizer.zero_grad()

            logits = model(X_batch)
            loss = loss_fn(logits, y_batch)

            if is_train:
                loss.backward()
                optimizer.step()

            total_loss += loss.item() * X_batch.size(0)
            total_rows += X_batch.size(0)

    return total_loss / total_rows


# ── Main ─────────────────────────────────────────────────────────────
def main():
    print(f"Device: {DEVICE}")

    with open(f"{DATA_DIR}/feature_columns.pkl", "rb") as f:
        feature_columns = pickle.load(f)
    with open(f"{DATA_DIR}/fault_class_names.pkl", "rb") as f:
        class_names = pickle.load(f)

    assert class_names == FAULT_CLASS_NAMES, (
        f"Class order mismatch between data_prep_output ({class_names}) and "
        f"model.py ({FAULT_CLASS_NAMES}) -- pos_weight order would be wrong "
        "if these don't match exactly. Stop and reconcile before training."
    )

    input_dim = len(feature_columns)
    print(f"input_dim (real, from feature_columns.pkl): {input_dim}")

    train_ds = FaultDataset(f"{DATA_DIR}/X_train_scaled.npy", f"{DATA_DIR}/y_train.npy")
    val_ds = FaultDataset(f"{DATA_DIR}/X_val_scaled.npy", f"{DATA_DIR}/y_val.npy")

    print(f"Train rows: {len(train_ds):,}  Val rows: {len(val_ds):,}")

    train_loader = DataLoader(train_ds, batch_size=BATCH_SIZE, shuffle=True, drop_last=True)
    val_loader = DataLoader(val_ds, batch_size=BATCH_SIZE, shuffle=False)

    model = build_model(input_dim=input_dim, class_names=class_names).to(DEVICE)
    print(model)
    print(f"Trainable parameters: {model.num_parameters():,}")

    loss_fn = nn.BCEWithLogitsLoss(pos_weight=POS_WEIGHT.to(DEVICE))
    optimizer = torch.optim.Adam(model.parameters(), lr=LEARNING_RATE)

    best_val_loss = float("inf")
    epochs_without_improvement = 0

    print(f"\nTraining: up to {MAX_EPOCHS} epochs, batch_size={BATCH_SIZE}, "
          f"early stop after {EARLY_STOP_PATIENCE} epochs without val improvement.\n")

    for epoch in range(1, MAX_EPOCHS + 1):
        train_loss = run_epoch(model, train_loader, loss_fn, optimizer)
        val_loss = run_epoch(model, val_loader, loss_fn, optimizer=None)

        print(f"Epoch {epoch:2d}/{MAX_EPOCHS}  train_loss={train_loss:.5f}  val_loss={val_loss:.5f}")

        if val_loss < best_val_loss:
            best_val_loss = val_loss
            epochs_without_improvement = 0
            torch.save({
                "model_state_dict": model.state_dict(),
                "config": model.config,
                "epoch": epoch,
                "val_loss": val_loss,
                "class_names": class_names,
                "feature_columns": feature_columns,
            }, CHECKPOINT_PATH)
            print(f"  -> new best val_loss, checkpoint saved to {CHECKPOINT_PATH}")
        else:
            epochs_without_improvement += 1
            print(f"  -> no improvement ({epochs_without_improvement}/{EARLY_STOP_PATIENCE})")
            if epochs_without_improvement >= EARLY_STOP_PATIENCE:
                print(f"\nEarly stopping at epoch {epoch} -- val_loss hasn't improved "
                      f"for {EARLY_STOP_PATIENCE} consecutive epochs.")
                break

    print(f"\nDone. Best val_loss: {best_val_loss:.5f} (checkpoint: {CHECKPOINT_PATH})")
    print("Next: evaluate.py loads this checkpoint and reports per-class "
          "precision/recall/lead-time against the naive baseline.")


if __name__ == "__main__":
    main()
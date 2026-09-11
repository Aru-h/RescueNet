import pandas as pd
import numpy as np

CSV = "tron_snapshot_dataset.csv"
CHUNK = 2_000_000  # rows per chunk, safe for limited RAM

V2_ROW_COUNT = 41_056_470  # v2's full row count -- only a valid 1:1 comparison
                            # when this script is run against the FULL 733-seed
                            # file. On a smaller smoke sample, this gets scaled
                            # down proportionally instead of compared directly --
                            # see the "v2 row-count cross-check" section below.
V2_SEED_COUNT = 733

usecols = [
    "world_seed", "event_type", "watchdog_anomaly_detected",
    "watchdog_failure_mode", "twin_remaining_path_cost",
]

# For the duplicate-row check: rows for the same (run_id, team_id, cycle) are
# expected to be adjacent or very close together in the file (the tick loop
# writes EVENT_TICK for all teams in one pass, then any dispatched-event rows
# in a second pass shortly after -- see core/simulation.c's two-pass split).
# A small carry-over buffer across chunk boundaries catches the rare case where
# a duplicate pair happens to straddle a chunk edge, without needing to hold
# the whole file's keys in memory.
KEY_COLS = ["run_id", "team_id", "cycle"]
OVERLAP_ROWS = 50  # generous margin -- far more than the few rows any real
                    # duplicate pair could actually be apart

total_rows = 0
n_cols = None
seeds_seen = set()
event_type_counts = pd.Series(dtype="int64")
anomaly_sum = 0
failure_mode_counts = pd.Series(dtype="int64")
cost_sum = 0.0
cost_count = 0
nan_total = 0
nan_by_col = pd.Series(dtype="int64")

duplicate_row_count = 0
carry_over = None  # last OVERLAP_ROWS rows of the previous chunk, for boundary dedup

print("Scanning in chunks (safe for limited RAM)...")
first_chunk = True

for chunk in pd.read_csv(CSV, chunksize=CHUNK):
    if first_chunk:
        n_cols = chunk.shape[1]
        first_chunk = False

    total_rows += len(chunk)
    seeds_seen.update(chunk["world_seed"].unique())

    event_type_counts = event_type_counts.add(
        chunk["event_type"].value_counts(), fill_value=0
    )

    anomaly_sum += chunk["watchdog_anomaly_detected"].sum()

    anomalous = chunk[chunk["watchdog_anomaly_detected"] == 1]
    failure_mode_counts = failure_mode_counts.add(
        anomalous["watchdog_failure_mode"].value_counts(), fill_value=0
    )

    mask = (chunk["twin_remaining_path_cost"] != 65535) & (chunk["twin_remaining_path_cost"] != 0)
    cost_sum += chunk.loc[mask, "twin_remaining_path_cost"].sum()
    cost_count += mask.sum()

    # Widened NaN check: whole chunk (all 67 columns), not just usecols.
    chunk_nan_by_col = chunk.isna().sum()
    nan_by_col = nan_by_col.add(chunk_nan_by_col, fill_value=0)
    nan_total += chunk_nan_by_col.sum()

    # Duplicate-row check, with boundary carry-over.
    check_frame = chunk if carry_over is None else pd.concat([carry_over, chunk], ignore_index=True)
    group_sizes = check_frame.groupby(KEY_COLS).size()
    dup_groups = group_sizes[group_sizes > 1]
    # Rows attributable to duplication = sum(size - 1) per duplicated key.
    duplicate_row_count += int((dup_groups - 1).sum())

    carry_over = chunk.tail(OVERLAP_ROWS)

print("\n=== RESULTS ===")
print("Total rows:", total_rows)
print("Total columns:", n_cols, "(expect 67, same as v2)")

seeds = sorted(seeds_seen)
print("\n--- Seed coverage ---")
print("Distinct seeds:", len(seeds))
print("Min:", seeds[0], "Max:", seeds[-1])
print("Contiguous?", seeds[-1] - seeds[0] + 1 == len(seeds), "(expect True)")

print("\n--- Full event_type breakdown ---")
print(event_type_counts.sort_index())

print("\n--- EVENT_TICK coverage ---")
tick_rows = event_type_counts.get(12, 0)
print("EVENT_TICK (12) rows:", int(tick_rows))
print("Fraction from EVENT_TICK:", tick_rows / total_rows)

print("\n--- v2 row-count cross-check ---")
non_tick_rows = total_rows - int(tick_rows)
print("Non-EVENT_TICK rows:", non_tick_rows)
if len(seeds) == V2_SEED_COUNT:
    print("v2's full row count:", V2_ROW_COUNT)
    print("Exact match?", non_tick_rows == V2_ROW_COUNT,
          "(expect True -- confirms nothing else in the simulator drifted since v2)")
else:
    expected_scaled = V2_ROW_COUNT * (len(seeds) / V2_SEED_COUNT)
    print(f"This is a {len(seeds)}-seed sample, not the full {V2_SEED_COUNT}-seed run --")
    print("exact match against v2's full row count is not meaningful here.")
    print(f"v2's per-seed average scaled to {len(seeds)} seed(s): ~{expected_scaled:,.0f}")
    print(f"Actual non-EVENT_TICK rows this run: {non_tick_rows:,}")
    ratio = non_tick_rows / expected_scaled if expected_scaled else float("nan")
    print(f"Ratio (actual / scaled-expected): {ratio:.3f}  "
          "(expect close to 1.0 -- large deviation may indicate a real issue, "
          "but some spread is normal since v2's per-seed row counts aren't identical)")

print("\n--- Duplicate-row check (run_id, team_id, cycle) ---")
print("Rows with a same-tick companion row:", duplicate_row_count)
print("This is EXPECTED to be large (~= non-EVENT_TICK row count) --")
print("every tick with a real event also has an EVENT_TICK row, by design.")
print("Only worry if this is near-ZERO (would mean EVENT_TICK isn't firing)")
print("or wildly larger than non-EVENT_TICK row count (would mean a genuine")
print("triplicate/over-logging bug, like the team_set_available one).")

print("\n--- Anomaly rate ---")
rate = anomaly_sum / total_rows
print(f"Anomaly rate: {rate*100:.3f}%  (v2 was 8.74%)")

print("\n--- Failure mode breakdown (watchdog_anomaly_detected==1) ---")
print(failure_mode_counts)
print("(v2 was: 5->2990016, 3->451792, 1->128091, 4->17061, 2->793)")

print(f"\n--- mean_nonzero_real_cost: {cost_sum/cost_count:.2f}  (v2 was 54.14) ---")

print("\n--- NaN check (full 67-column frame, not just usecols) ---")
print("Total NaNs across all columns:", int(nan_total))
nonzero_nan_cols = nan_by_col[nan_by_col > 0]
if len(nonzero_nan_cols) > 0:
    print("Columns with NaNs:")
    print(nonzero_nan_cols)
else:
    print("No columns have any NaNs.")

print("\nDone.")
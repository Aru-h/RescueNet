"""THROWAWAY. Step 1: run a text-patched COPY of data_prep-2.py on the merged CSV.

The repo's data_prep-2.py is executed unchanged except for three textual
substitutions applied to an in-memory copy (nothing tracked is modified):
  * CSV_PATH / OUTPUT_DIR point into $TRON_G_WORK
  * after group_split(): also save the split indices and per-row metadata
  * after the final downcast: also save the unscaled feature frame
Everything else (dedup, evidence merge, one-hot, leak checks, run_id group split,
scaler) is the repo's own code, so features and split match data_prep-2.py.
"""
import glob, os, sys, re
import pandas as pd

REPO = os.environ.get("REPO", os.path.abspath(os.path.join(os.path.dirname(__file__), "../../..")))
WORK = os.environ.get("TRON_G_WORK", os.path.abspath("work"))
os.makedirs(WORK, exist_ok=True)

merged = os.path.join(WORK, "tron_snapshot_dataset_merged.csv")
if not os.path.exists(merged):
    frames = [pd.read_csv(p) for p in sorted(glob.glob(f"{WORK}/shard*/tron_snapshot_dataset.csv"))]
    df = pd.concat(frames, ignore_index=True)
    # run_id in the sim is (process start epoch + seed): unique per seed only
    # while every process starts in a compatible order. Re-key on world_seed.
    n_before = df["run_id"].nunique()
    df["run_id"] = df["world_seed"]
    print(f"run_id re-keyed to world_seed ({n_before} -> {df['run_id'].nunique()} unique)")
    df.to_csv(merged, index=False)
    del df, frames
print("merged csv:", merged, os.path.getsize(merged) // 2**20, "MiB")

src = open(os.path.join(REPO, "data_prep-2.py")).read()
out_dir = os.path.join(WORK, os.environ.get("PREP_DIR", "prep"))
os.makedirs(out_dir, exist_ok=True)

def sub(old, new, s):
    assert s.count(old) == 1, old
    return s.replace(old, new)

src = sub('OUTPUT_DIR = "data_prep_output"', f'OUTPUT_DIR = {out_dir!r}', src)
src = sub('CSV_PATH = "simulation_output_final_v4.csv"', f'CSV_PATH = {merged!r}', src)
src = sub('train_idx, val_idx, test_idx = group_split(df, y_balance, group_col="run_id")',
 'train_idx, val_idx, test_idx = group_split(df, y_balance, group_col="run_id")\n'
 'df[["run_id","world_seed","cycle","team_id","heartbeat_emitted","loc_x","loc_y",'
 '"watchdog_anomaly_detected","watchdog_anomaly_confidence","watchdog_evidence_flags"]]'
 '.assign(split=np.select([df.index.isin(train_idx), df.index.isin(val_idx)], [0, 1], 2))'
 '.to_pickle(f"{OUTPUT_DIR}/meta.pkl")\n'
 'np.savez(f"{OUTPUT_DIR}/split_idx.npz", train=train_idx, val=val_idx, test=test_idx)\n'
 'y_multi.to_pickle(f"{OUTPUT_DIR}/y_all.pkl")', src)
src = sub('X = downcast_numeric_columns(X)\nprint("Number of features (with path_is_unreachable):"',
 'X = downcast_numeric_columns(X)\nX.to_pickle(f"{OUTPUT_DIR}/X_raw.pkl")\nprint("Number of features (with path_is_unreachable):"', src)

sys.path.insert(0, REPO)          # evidence_channels.py
os.chdir(WORK)
# run in __main__'s own namespace so the script's pickled SafeStandardScaler resolves
exec(compile(src, "data_prep-2.py(patched copy)", "exec"), sys.modules["__main__"].__dict__)

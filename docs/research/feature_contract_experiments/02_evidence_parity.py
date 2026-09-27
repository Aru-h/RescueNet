"""THROWAWAY. Step 2: Python evidence mirror vs the C watchdog's own logged output.

evidence_channels.py is the training-time source of evidence_*. The C watchdog
logs, per row: watchdog_evidence_flags (bit set iff channel >= 0.20),
watchdog_anomaly_detected (weighted confidence >= 0.10) and
watchdog_anomaly_confidence (%.4f). Compare on every logged row.
Also reports how many ticks had no row (the mirror forward-fills those).
"""
import os, sys
import numpy as np, pandas as pd
REPO = os.environ.get("REPO", os.path.abspath(os.path.join(os.path.dirname(__file__), "../../..")))
WORK = os.environ.get("TRON_G_WORK", os.path.abspath("work"))
sys.path.insert(0, REPO)
import evidence_channels as ec

N_SEEDS = int(os.environ.get("N_SEEDS", "8"))
df = pd.read_csv(f"{WORK}/tron_snapshot_dataset_merged.csv")
df = df[df["world_seed"] <= N_SEEDS].copy()
df["_row_seq"] = np.arange(len(df), dtype=np.int32)
df = df.sort_values(["run_id", "team_id", "cycle", "_row_seq"]).drop_duplicates(["run_id", "team_id", "cycle"], keep="last")
res = ec.compute_evidence_features(df)
rep = ec.validate_against_watchdog(res)
print("seeds:", N_SEEDS, "rows compared:", rep["rows_compared"])
print("anomaly_detected agreement: %.5f" % rep["overall_anomaly_detected_agreement"])
for ch, a in rep["channels"].items():
    print("  flag %-16s agreement %.5f" % (ch, a))
lo = res[res["was_logged"]]
d = (lo["recomputed_anomaly_confidence"] - lo["watchdog_anomaly_confidence"]).abs()
print("confidence |mirror - C| : mean %.2e  p99 %.2e  max %.2e ; rows > 5e-5 (print rounding): %.4f%%" %
      (d.mean(), d.quantile(.99), d.max(), 100 * (d > 5e-5).mean()))
print("ticks forward-filled (no row logged): %.3f%% of %d" % (100 * (~res["was_logged"]).mean(), len(res)))
if "event_type" in lo:
    print(lo.groupby("event_type").size().head(10))

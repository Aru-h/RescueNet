#!/usr/bin/env python3
"""
laptop/tools/ai_bias_checks.py

Like-for-like checks that do not depend on how make evaluate credits
detections. Same ground truth (simulator fault_active_* columns), same ticks:
  1. Threshold-free separation (ROC AUC): AI per-class score vs the watchdog's
     single anomaly_confidence, per class.
  2. Watchdog threshold curve: anomaly_detected = (anomaly_confidence >= T);
     precision/recall/F1 vs "any fault active" at several T (0.10 = in use).
  3. Rule evaluators vs AI at tick level, per class (AI-off run + offline scores).
  4. Resting effect: flag rate on fault ticks when the team is resting vs active.
  5. How much of the AI-on run's fused action >= RETURN_TO_SAFE_ZONE happens
     while the team is already resting.

usage: python3 laptop/tools/ai_bias_checks.py <closed_dir> <open_dir> [--checkpoint PATH]
"""
import argparse, sys
from pathlib import Path
import numpy as np, pandas as pd
from sklearn.metrics import roc_auc_score

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE)); sys.path.insert(0, str(HERE.parents[1] / "ml"))
import ai_closed_loop_analysis as A  # noqa: E402

CLS, KEY = A.CLS, A.KEY


def prf(y, p):
    return A.prf(y.astype(int), p.astype(int))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument("closed"); ap.add_argument("open")
    ap.add_argument("--checkpoint", default="/tmp/ckpt/fault_predictor_checkpoint.pt"); a = ap.parse_args()
    import torch
    from export import load_checkpoint, rebuild_model
    m, _ = rebuild_model(load_checkpoint(a.checkpoint))
    od = A.load_ticks(a.open).merge(A.read_all(a.open, "features.csv"), on=KEY)
    with torch.no_grad():
        S = torch.sigmoid(m(torch.tensor(od[[f"f{i}" for i in range(75)]].to_numpy(np.float32)))).numpy()
    conf = od["watchdog_anomaly_confidence"].to_numpy()
    anyf = od[[f"fault_active_{c}" for c in CLS]].any(axis=1).to_numpy()
    print(f"AI-off run, {len(od):,} team-ticks; ticks with any fault active: {100*anyf.mean():.1f}%\n")

    print("### 1. Threshold-free separation (ROC AUC vs ground truth; 0.5 = chance)\n")
    print("| class | AI score | watchdog anomaly_confidence |\n|---|---|---|")
    for k, c in enumerate(CLS):
        y = od[f"fault_active_{c}"].to_numpy()
        print(f"| {c} | {roc_auc_score(y, S[:, k]):.3f} | {roc_auc_score(y, conf):.3f} |")
    print(f"| any fault | {roc_auc_score(anyf, S[:, :3].max(1)):.3f} (max of mob/comm/res) | {roc_auc_score(anyf, conf):.3f} |")

    print("\n### 2. Watchdog threshold: anomaly_detected = (anomaly_confidence >= T), truth = any fault active\n")
    print("| T | flagged share | precision | recall | F1 |\n|---|---|---|---|---|")
    for T in (0.05, 0.10, 0.18, 0.30, 0.50):
        p = conf >= T; pr, rc, f1 = prf(anyf, p)
        print(f"| {T:.2f}{' (in use)' if T == 0.10 else ''} | {100*p.mean():.1f}% | {pr:.3f} | {rc:.3f} | {f1:.3f} |")
    pa = od["watchdog_anomaly_detected"].to_numpy() != 0
    print(f"\nsanity: logged watchdog_anomaly_detected flag share {100*pa.mean():.1f}%")

    print("\n### 3. Tick-level, per class: rule evaluator vs AI (AI-off run, same ticks)\n")
    print("| class | rule evaluator P / R / F1 | AI (tuned threshold) P / R / F1 |\n|---|---|---|")
    ev = {"mobility": ["eval_mobility_reason"], "communication": ["eval_communication_reason"],
          "resource": ["eval_stamina_reason", "eval_equipment_reason"]}
    rest = (od.status == 3).to_numpy()
    for k, c in enumerate(CLS[:3]):
        y = od[f"fault_active_{c}"].to_numpy()
        rule = np.zeros(len(od), bool)
        for e in ev[c]:
            rule |= od[e].to_numpy() != 0
        ai = S[:, k] >= A.THR[k]
        r1, r2, r3 = prf(y, rule); a1, a2, a3 = prf(y, ai)
        print(f"| {c} | {r1:.3f} / {r2:.3f} / {r3:.3f} | {a1:.3f} / {a2:.3f} / {a3:.3f} |")
        ev[c] = (y, rule, ai)

    print(f"\n### 4. Resting effect (AI-off run): share of fault ticks that were flagged, team resting vs not (teams resting {100*rest.mean():.1f}% of all ticks)\n")
    print("| class | fault ticks resting | rule flags when resting | rule flags when active | AI flags when resting | AI flags when active |\n|---|---|---|---|---|---|")
    for c in CLS[:3]:
        y, rule, ai = ev[c]; y = y.astype(bool)
        f = lambda mask, x: 100 * x[y & mask].mean() if (y & mask).any() else float("nan")
        print(f"| {c} | {100*(y & rest).sum()/y.sum():.0f}% | {f(rest, rule):.1f}% | {f(~rest, rule):.1f}% | {f(rest, ai):.1f}% | {f(~rest, ai):.1f}% |")

    cd = A.load_ticks(a.closed)
    act = cd.recovery_recommendation >= 2
    print(f"\n### 5. AI-on run: fused action >= RETURN_TO_SAFE_ZONE on {100*act.mean():.1f}% of team-ticks; "
          f"of those, team already RESTING: {100*((cd.status == 3) & act).sum()/act.sum():.1f}%, heading_to_rest: {100*((cd.heading_to_rest != 0) & act).sum()/act.sum():.1f}%")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
laptop/tools/ai_closed_loop_analysis.py

Answers "is the AI being undercounted / does it degrade in closed loop?" with
ground truth, using two runs of the SAME seeds:

  closed/ : ./tron_ai-equivalent run (AI acting) via ai_score_dump  -> scores.csv
            + tron_snapshot_dataset.csv
  open/   : AI-off run (rules+consensus) via a feature dump          -> features.csv
            + tron_snapshot_dataset.csv   (scores computed offline with the
            checkpoint, i.e. what the model WOULD say on AI-off trajectories)

Outputs (all against the simulator's ground-truth fault_active_* columns):
  1. Tick-level per-class precision/recall/F1 and macro F1 (5 classes / 4
     detectable classes excluding environment), open loop vs closed loop, at
     the given thresholds (default: mobility 0.573, communication 0.965,
     resource 0.705, environment 0.706, mission 0.731 - env/mission are OFF in
     the build, scored here only to complete the macro F1).
  2. Episode-level detection credit in the closed-loop run: rules-only (what
     make evaluate counts), AI-only (score >= threshold at any tick of the
     episode), and their union, next to the AI-off run's rules-only rate.
  3. How often the AI's action came before the rule evaluators flagged.

usage: python3 laptop/tools/ai_closed_loop_analysis.py <closed_dir> <open_dir> [--checkpoint PATH]
"""
import argparse, sys
from pathlib import Path
import numpy as np, pandas as pd

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parents[1] / "ml"))
import eval_metrics as em  # noqa: E402

CLS = ["mobility", "communication", "resource", "environment", "mission"]
THR = [0.573, 0.965, 0.705, 0.706, 0.731]
KEY = ["run_id", "team_id", "cycle"]


def part_dirs(d):
    """<d> itself, or its per-seed subdirectories (one process per seed)."""
    d = Path(d)
    return [d] if (d / "tron_snapshot_dataset.csv").exists() else sorted(p for p in d.iterdir() if p.is_dir())


def read_all(d, name, **kw):
    return pd.concat([pd.read_csv(p / name, **kw) for p in part_dirs(d)], ignore_index=True)


def load_ticks(d):
    df = read_all(d, "tron_snapshot_dataset.csv", low_memory=False)
    return df[df.event_type == em.EVENT_TICK].copy()


def prf(y, p):
    tp = int(((y == 1) & (p == 1)).sum()); fp = int(((y == 0) & (p == 1)).sum()); fn = int(((y == 1) & (p == 0)).sum())
    pr = tp / (tp + fp) if tp + fp else 0.0; rc = tp / (tp + fn) if tp + fn else 0.0
    return pr, rc, (2 * pr * rc / (pr + rc) if pr + rc else 0.0)


def tick_table(name, df, S):
    print(f"\n### {name}: tick-level, ground truth = fault_active_*  ({len(df):,} team-ticks)\n")
    print("| class | base rate | threshold | precision | recall | F1 | flagged share of ticks |")
    print("|---|---|---|---|---|---|---|")
    f1s = []
    for k, c in enumerate(CLS):
        y = df[f"fault_active_{c}"].to_numpy(); p = (S[:, k] >= THR[k]).astype(int)
        pr, rc, f1 = prf(y, p); f1s.append(f1)
        print(f"| {c} | {y.mean():.3f} | {THR[k]} | {pr:.3f} | {rc:.3f} | {f1:.3f} | {p.mean():.3f} |")
    print(f"\nmacro F1, 5 classes: **{np.mean(f1s):.3f}**   4 detectable (excl. environment): **{np.mean([f for c, f in zip(CLS, f1s) if c != 'environment']):.3f}**")


def episodes(df, col):
    """(run, team, start, end) spans where col == 1."""
    out = []
    for (r, t), g in df.sort_values(KEY).groupby(["run_id", "team_id"]):
        cyc = g["cycle"].to_numpy(); v = g[col].to_numpy(); i = 0
        while i < len(v):
            if v[i]:
                j = i
                while j + 1 < len(v) and v[j + 1]:
                    j += 1
                out.append((r, t, cyc[i], cyc[j])); i = j + 1
            else:
                i += 1
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("closed"); ap.add_argument("open")
    ap.add_argument("--checkpoint", default="/tmp/ckpt/fault_predictor_checkpoint.pt")
    a = ap.parse_args()

    # ---- closed loop
    cd = load_ticks(a.closed)
    sc = read_all(a.closed, "scores.csv")
    cd = cd.merge(sc, on=KEY)
    Sc = cd[[f"s_{c}" for c in CLS]].to_numpy()
    # ---- open loop: score AI-off features offline with the checkpoint
    import torch
    from export import load_checkpoint, rebuild_model
    m, _ = rebuild_model(load_checkpoint(a.checkpoint))
    od = load_ticks(a.open)
    ft = read_all(a.open, "features.csv")
    od = od.merge(ft, on=KEY)
    with torch.no_grad():
        So = torch.sigmoid(m(torch.tensor(od[[f"f{i}" for i in range(75)]].to_numpy(np.float32)))).numpy()

    tick_table("OPEN LOOP (AI off; model scores computed offline on the AI-off trajectory)", od, So)
    tick_table("CLOSED LOOP (AI acting; scores logged live)", cd, Sc)

    # ---- team availability
    for nm, d in (("AI off", od), ("AI on", cd)):
        print(f"\n{nm}: team-ticks RESTING {100*(d.status==3).mean():.1f}%, heading_to_rest {100*(d.heading_to_rest!=0).mean():.1f}%, "
              f"fused action >= RETURN_TO_SAFE_ZONE {100*(d.recovery_recommendation>=2).mean():.1f}%")

    # ---- episode-level credit (closed loop)
    print("\n### Episode-level detection, closed-loop run (episode = contiguous ground-truth fault span for one team)\n")
    print("| class | episodes | rules only (what `make evaluate` counts) | AI only (score>=thr in episode) | rules OR AI | AI-off run: rules only | AI acted (fused action reason=PREDICTED_FAULT) before rules flagged |")
    print("|---|---|---|---|---|---|---|")
    cd = cd.sort_values(KEY); od = od.sort_values(KEY)
    for k, c in enumerate(CLS[:3]):
        col = f"fault_active_{c}"
        # per-row rule flags from any evaluator (predictor excluded)
        def rule_flag(d):
            ev = {"mobility": ["eval_mobility_reason"], "communication": ["eval_communication_reason"],
                  "resource": ["eval_stamina_reason", "eval_equipment_reason"]}[c]
            f = np.zeros(len(d), bool)
            for e in ev:
                f |= d[e].to_numpy() != 0
            return f
        cd["_rule"] = rule_flag(cd); od["_rule"] = rule_flag(od)
        cd["_ai"] = cd[f"s_{c}"].to_numpy() >= THR[k]
        cd["_aiact"] = ((cd.recovery_recommendation >= 2) & (cd.recovery_reason == em.REASON_PREDICTED_FAULT)).to_numpy()
        eps = episodes(cd, col); n = len(eps)
        gi = {key: g.reset_index(drop=True) for key, g in cd.groupby(["run_id", "team_id"])}
        r_only = a_only = both = act_first = 0
        for (r, t, s, e) in eps:
            g = gi[(r, t)]; w = g[(g.cycle >= s) & (g.cycle <= e)]
            rf = w["_rule"].any(); af = w["_ai"].any()
            r_only += rf; a_only += af; both += (rf or af)
            if w["_aiact"].any():
                first_ai = w.loc[w["_aiact"], "cycle"].min()
                first_rule = w.loc[w["_rule"], "cycle"].min() if rf else 10**9
                act_first += first_ai <= first_rule
        eo = episodes(od, col); ro = sum(od[(od.run_id == r) & (od.team_id == t) & (od.cycle >= s) & (od.cycle <= e)]["_rule"].any() for (r, t, s, e) in eo[:3000])
        n_o = min(len(eo), 3000)
        print(f"| {c} | {n} | {100*r_only/n:.1f}% | {100*a_only/n:.1f}% | {100*both/n:.1f}% | {100*ro/n_o:.1f}% | {100*act_first/n:.1f}% of episodes |")


if __name__ == "__main__":
    main()

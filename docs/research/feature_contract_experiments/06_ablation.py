"""THROWAWAY. Step 6: which inputs can be dropped because the board cannot compute them?

Trains the same full-input MLP as 04_bottleneck.py with feature groups removed
(columns of work/prep/X_raw.pkl, i.e. data_prep-2.py's order) and reports per-class F1 on TEST
at val-tuned thresholds. Groups follow the board-computability findings in the research doc.
"""
import os, sys, numpy as np, pandas as pd, torch, torch.nn as nn
WORK = os.environ.get("TRON_G_WORK", os.path.abspath("work")); P = f"{WORK}/prep"
SEEDS = [int(s) for s in os.environ.get("SEEDS", "0,1").split(",")]; EPOCHS = int(os.environ.get("EPOCHS", "12"))
torch.set_num_threads(os.cpu_count())
cols = list(pd.read_pickle(f"{P}/X_raw.pkl").columns)
X = {n: np.load(f"{P}/X_{n}_scaled.npy") for n in ("train", "val", "test")}
y = {n: np.load(f"{P}/y_{n}.npy").astype(np.float32) for n in X}
T = lambda a: torch.from_numpy(np.ascontiguousarray(a, dtype=np.float32))
pre = lambda *ps: [c for c in cols if c.startswith(ps)]
groups = {
    "all 72 (baseline)": [],
    "-zone (team_zone_id_*, team_zone_trust)": pre("team_zone_id_", "team_zone_trust"),
    "-zone -env_raw_travel_cost": pre("team_zone_id_", "team_zone_trust", "env_raw_travel_cost"),
    "-incident context (priority,victims,capability,support,hazards)": pre("incident_priority", "victim_count", "incident_req_capability_", "support_team_", "hazard_"),
    "-evidence_* -confirmed_mobility (raw telemetry only)": pre("evidence_", "confirmed_mobility_impairment"),
    "-zone -env -incident (board-computable core)": pre("team_zone_id_", "team_zone_trust", "env_raw_travel_cost", "incident_priority", "victim_count", "incident_req_capability_", "support_team_", "hazard_"),
}
def f1(yt, s, thr):
    p = s >= thr; tp = (p & (yt > .5)).sum(0); fp = (p & (yt < .5)).sum(0); fn = (~p & (yt > .5)).sum(0)
    pr = tp / np.maximum(tp + fp, 1); rc = tp / np.maximum(tp + fn, 1); return 2 * pr * rc / np.maximum(pr + rc, 1e-9)
def run(keep, seed):
    torch.manual_seed(seed)
    Xt, Xv, Xe = [T(X[n][:, keep]) for n in ("train", "val", "test")]; yt = T(y["train"])
    m = nn.Sequential(nn.Linear(len(keep), 128), nn.ReLU(), nn.Linear(128, 64), nn.ReLU(), nn.Linear(64, 5))
    pw = torch.clamp((1 - yt.mean(0)) / yt.mean(0).clamp_min(1e-4), max=20.0) ** 0.5
    lf = nn.BCEWithLogitsLoss(pos_weight=pw); opt = torch.optim.Adam(m.parameters(), lr=2e-3)
    sch = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=2e-3, total_steps=EPOCHS * ((len(Xt) + 2047) // 2048)); best, st = 1e9, None
    for ep in range(EPOCHS):
        m.train(); perm = torch.randperm(len(Xt))
        for i in range(0, len(Xt), 2048):
            b = perm[i:i + 2048]; opt.zero_grad(); lf(m(Xt[b]), yt[b]).backward(); opt.step(); sch.step()
        m.eval()
        with torch.no_grad(): vl = lf(m(Xv), T(y["val"])).item()
        if vl < best: best, st = vl, {k: v.clone() for k, v in m.state_dict().items()}
    m.load_state_dict(st); m.eval()
    with torch.no_grad(): sv, se = torch.sigmoid(m(Xv)).numpy(), torch.sigmoid(m(Xe)).numpy()
    g = np.linspace(.02, .98, 49)
    thr = np.array([g[np.argmax([f1(y["val"][:, [c]], sv[:, [c]], t)[0] for t in g])] for c in range(5)])
    return f1(y["test"], se, thr)
print("| inputs | n | mobility | communication | resource | environment | mission | macro |\n|---|---|---|---|---|---|---|---|")
for name, drop in groups.items():
    keep = [i for i, c in enumerate(cols) if c not in set(drop)]
    f = np.mean([run(keep, s) for s in SEEDS], 0)
    print("| %s | %d | %s | %.3f |" % (name, len(keep), " | ".join("%.3f" % v for v in f), f.mean()), flush=True)

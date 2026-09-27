"""THROWAWAY. Step 5: peer-side judge = f(target's self-reported embedding, peer's own observations).

tron_peer_obs.csv does not exist yet (feat/consensus is design-only), so a peer's observations
are SIMULATED from the dataset with the vocabulary CONSENSUS_DESIGN.md §5.2 defines:
  rx_on_time (beacon heard: target emitted that tick AND the link did not drop it, iid loss q),
  rx_age_ticks, comm_misses over the last 6 ticks, mob_stalled/mob_window_n over the last 4
  consecutive received pairs (reported loc unchanged), plaus_fail (reported step > 1 cell/tick),
  heard9 (how many of the 9 peers heard the target this tick - the heard_bits fast-path signal).
The peer never sees the target's fresh state: it holds the LAST RECEIVED report (stale by rx_age).

Report = 8-dim int8 split-model embedding (option (a), k=8, from 04_bottleneck.py's recipe).
Heads compared (all predict the target's 5 fault classes, tested at per-class val-tuned thresholds):
  A  report only (stale)        B  observations only        C  report + observations
  C* same as C but trained with corrupted reports mixed in (containment training)
Corruptions applied to the report at TEST time: none | stale12 (frozen 12 ticks) | healthy-spoof (report
of a random healthy sample) | noise | on 30% of rows only. Also: how often do >=7 of 9 peers
(independent link loss, identical corrupted report) all reach the same wrong 'no fault' verdict.
"""
import os, sys, time, json
import numpy as np, torch, torch.nn as nn
WORK = os.environ.get("TRON_G_WORK", os.path.abspath("work")); P = f"{WORK}/prep"
Q = float(os.environ.get("LOSS", "0.10")); K = 8; REPORT = os.environ.get("REPORT", "embed"); EPOCHS = int(os.environ.get("EPOCHS", "10"))
CLASSES = ["mobility", "communication", "resource", "environment", "mission"]
torch.set_num_threads(os.cpu_count()); torch.manual_seed(0); rng = np.random.default_rng(0)

M = __import__("pandas").read_pickle(f"{P}/meta.pkl"); idx = np.load(f"{P}/split_idx.npz")
Xs = {n: np.load(f"{P}/X_{n}_scaled.npy") for n in ("train", "val", "test")}
ys = {n: np.load(f"{P}/y_{n}.npy").astype(np.float32) for n in ("train", "val", "test")}
T_ = lambda a: torch.from_numpy(np.ascontiguousarray(a, dtype=np.float32))
ticks = M.groupby(["run_id", "team_id"]).size(); assert ticks.nunique() == 1, "every stream must have all ticks"
NT = int(ticks.iloc[0]); S = len(ticks)
assert (M["cycle"].to_numpy().reshape(S, NT)[:, 1] - M["cycle"].to_numpy().reshape(S, NT)[:, 0] == 1).all()
split_of_row = M["split"].to_numpy()                       # 0 train 1 val 2 test
split_of_stream = split_of_row.reshape(S, NT)[:, 0]

def mlp(din, hidden, dout=5):
    L, d = [], din
    for h in hidden: L += [nn.Linear(d, h), nn.ReLU()]; d = h
    return nn.Sequential(*L, nn.Linear(d, dout))
def fq(z, bits=8):
    n = 2 ** (bits - 1) - 1; q = torch.round(z.clamp(-1, 1) * n) / n; return z + (q - z).detach()
class Split(nn.Module):
    def __init__(s, D):
        super().__init__(); s.enc = nn.Sequential(nn.Linear(D, 128), nn.ReLU(), nn.Linear(128, 64), nn.ReLU(), nn.Linear(64, K))
        s.head = mlp(K, (64, 32))
    def code(s, x): return fq(torch.tanh(s.enc(x)))
    def forward(s, x): return s.head(s.code(x))
def fit(m, X, y, Xv, yv, epochs=EPOCHS, bs=2048, lr=2e-3, aug=None):
    ytr = T_(y); pos_w = torch.clamp((1 - ytr.mean(0)) / ytr.mean(0).clamp_min(1e-4), max=20.0) ** 0.5
    opt = torch.optim.Adam(m.parameters(), lr=lr); lossf = nn.BCEWithLogitsLoss(pos_weight=pos_w)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=epochs * ((len(X) + bs - 1) // bs))
    best, st = 1e9, None
    for ep in range(epochs):
        m.train(); perm = torch.randperm(len(X))
        for i in range(0, len(X), bs):
            b = perm[i:i + bs]; xb = X[b]
            if aug is not None: xb = aug(xb)
            opt.zero_grad(); lossf(m(xb), ytr[b]).backward(); opt.step(); sched.step()
        m.eval()
        with torch.no_grad(): vl = lossf(m(Xv), T_(yv)).item()
        if vl < best: best, st = vl, {k: v.clone() for k, v in m.state_dict().items()}
    m.load_state_dict(st); m.eval(); return m
def pred(m, X):
    with torch.no_grad(): return torch.sigmoid(torch.cat([m(X[i:i + 200000]) for i in range(0, len(X), 200000)])).numpy()
def f1s(y, s, thr):
    p = s >= thr; tp = (p & (y > .5)).sum(0); fp = (p & (y < .5)).sum(0); fn = (~p & (y > .5)).sum(0)
    pr = tp / np.maximum(tp + fp, 1); rc = tp / np.maximum(tp + fn, 1); return 2 * pr * rc / np.maximum(pr + rc, 1e-9)
def tune(y, s):
    g = np.linspace(.02, .98, 49); return np.array([g[np.argmax([f1s(y[:, [c]], s[:, [c]], t)[0] for t in g])] for c in range(5)])

# ---- 1. sender-side embedding (option (a) k=8 int8 QAT)
t0 = time.time()
enc = fit(Split(Xs["train"].shape[1]), T_(Xs["train"]), ys["train"], T_(Xs["val"]), ys["val"])
with torch.no_grad():
    z_all = {n: torch.cat([enc.code(T_(Xs[n][i:i + 200000])) for i in range(0, len(Xs[n]), 200000)]).numpy() for n in Xs}
print("sender encoder trained %.0fs; sender-side (fresh report) F1 = %s" % (time.time() - t0,
      np.round(f1s(ys["test"], pred(enc, T_(Xs["test"])), tune(ys["val"], pred(enc, T_(Xs["val"])))), 3)))
if REPORT == "scores":   # report = the sender's own 5 fault scores, u8-quantised, instead of the k=8 embedding
    with torch.no_grad():
        z_all = {n: np.round(torch.cat([torch.sigmoid(enc(T_(Xs[n][i:i + 200000]))) for i in range(0, len(Xs[n]), 200000)]).numpy() * 255) / 255
                 for n in Xs}
    K = 5
Z = np.zeros((S * NT, K), np.float32)
for n, key in (("train", 0), ("val", 1), ("test", 2)): Z[split_of_row == key] = z_all[n]
Z = Z.reshape(S, NT, K)
emit = M["heartbeat_emitted"].to_numpy().reshape(S, NT).astype(bool)
loc = np.stack([M["loc_x"].to_numpy(), M["loc_y"].to_numpy()], 1).reshape(S, NT, 2)
tt = np.arange(NT)[None, :]

def observer_view(rx_rng, q=Q):
    """Everything one observer holds about every stream at every tick."""
    rx = emit & (rx_rng.random((S, NT)) > q)
    last = np.maximum.accumulate(np.where(rx, tt, -1), axis=1)
    age = np.where(last < 0, 12, np.minimum(tt - last, 12)).astype(np.float32)
    z_stale = np.take_along_axis(Z, np.maximum(last, 0)[..., None], 1) * (last >= 0)[..., None]
    miss = (~rx).astype(np.float32); cs = np.cumsum(miss, 1)
    m6 = cs - np.concatenate([np.zeros((S, 6)), cs[:, :-6]], 1)
    pair = np.zeros((S, NT), bool); pair[:, 1:] = rx[:, 1:] & rx[:, :-1]
    same = np.zeros((S, NT), bool); same[:, 1:] = (loc[:, 1:] == loc[:, :-1]).all(-1)
    jump = np.zeros((S, NT), bool); jump[:, 1:] = np.abs(loc[:, 1:] - loc[:, :-1]).sum(-1) > 2
    def win(a, w=4):
        c = np.cumsum(a.astype(np.float32), 1); return c - np.concatenate([np.zeros((S, w)), c[:, :-w]], 1)
    obs = np.stack([rx.astype(np.float32), m6 / 6, age / 12, win(pair & same) / 4, win(pair) / 4, win(pair & jump) / 4], -1)
    return obs.astype(np.float32), z_stale.astype(np.float32), age

def heard9(n_obs=9, q=Q):
    return (emit[None] & (rng.random((n_obs, S, NT)) > q)).sum(0).astype(np.float32) / n_obs

def flat(a, key): return a.reshape(S * NT, -1)[split_of_row == key]
obs, zs, age = observer_view(np.random.default_rng(11))
h9 = heard9()[..., None]
OBS = np.concatenate([obs, h9], -1)
def feats(kind, obs_, zs_, h9_, age_):
    o = np.concatenate([obs_, h9_], -1)
    return {"A": np.concatenate([zs_, age_[..., None] / 12], -1), "B": o, "C": np.concatenate([zs_, o], -1)}[kind]

Y = {n: ys[n] for n in ys}
# ---- corruption operators on the *stale report* seen by peers
healthy_pool = z_all["test"][ys["test"].max(1) == 0]
def corrupt(zs_, mode, frac=1.0, r=np.random.default_rng(5)):
    z = zs_.copy(); n = len(z); sel = r.random(n) < frac
    if mode == "spoof_healthy": z[sel] = healthy_pool[r.integers(0, len(healthy_pool), sel.sum())]
    elif mode == "noise": z[sel] = r.uniform(-1, 1, (sel.sum(), z.shape[1])).round(2)
    elif mode == "zeros": z[sel] = 0
    return z
def stale_shift(lag):   # report frozen `lag` ticks behind (on top of ordinary link staleness)
    zz = np.concatenate([np.zeros((S, lag, K), np.float32), zs[:, :-lag]], 1); return zz

models, res = {}, {}
def aug_fn(mode_probs):
    pool = torch.from_numpy(z_all["train"][ys["train"].max(1) == 0])
    def f(xb):
        xb = xb.clone(); r = torch.rand(len(xb)); nz = K
        m1 = r < mode_probs; kk = torch.randint(0, 3, (len(xb),))
        sp = m1 & (kk == 0); xb[sp, :nz] = pool[torch.randint(0, len(pool), (int(sp.sum()),))]
        no = m1 & (kk == 1); xb[no, :nz] = torch.empty(int(no.sum()), nz).uniform_(-1, 1)
        ze = m1 & (kk == 2); xb[ze, :nz] = 0
        return xb
    return f
for kind in ("A", "B", "C", "C*"):
    base = "C" if kind == "C*" else kind
    Xk = {n: T_(feats(base, obs, zs, h9, age).reshape(S * NT, -1)[split_of_row == i]) for n, i in (("train", 0), ("val", 1), ("test", 2))}
    m = mlp(Xk["train"].shape[1], (64, 32))
    models[kind] = fit(m, Xk["train"], Y["train"], Xk["val"], Y["val"], aug=aug_fn(0.35) if kind == "C*" else None)
    thr = tune(Y["val"], pred(models[kind], Xk["val"]))
    res[kind] = (models[kind], thr)

def run(kind, obs_, zs_, h9_, age_, ytrue):
    m, thr = res[kind]; base = "C" if kind == "C*" else kind
    X = T_(feats(base, obs_, zs_, h9_, age_).reshape(S * NT, -1)[split_of_row == 2])
    return f1s(ytrue, pred(m, X), thr)
ytest = Y["test"]
print("\ncondition: report seen by the peer (link loss q=%.2f)\n" % Q)
print("| condition | head | " + " | ".join(CLASSES) + " | macro |"); print("|---|---|" + "---|" * 6)
conds = [("clean (stale by link loss only)", zs), ("frozen 12 ticks", stale_shift(12)),
         ("all reports 'healthy' (spoof)", corrupt(zs.reshape(-1, K), "spoof_healthy").reshape(S, NT, K)),
         ("30% reports 'healthy' (spoof)", corrupt(zs.reshape(-1, K), "spoof_healthy", .3).reshape(S, NT, K)),
         ("all reports random noise", corrupt(zs.reshape(-1, K), "noise").reshape(S, NT, K)),
         ("all reports zeroed", corrupt(zs.reshape(-1, K), "zeros").reshape(S, NT, K))]
out = []
for name, zc in conds:
    for kind in ("A", "B", "C", "C*"):
        f = run(kind, obs, zc, h9, age, ytest)
        print("| %s | %s | %s | %.3f |" % (name, kind, " | ".join("%.3f" % x for x in f), f.mean())); out.append((name, kind, f.tolist()))

# ---- common-mode: 9 independent observers, ONE corrupted report broadcast to all
print("\nCommon-mode test: 'all reports healthy' spoof, 9 observers with independent link loss q=%.2f." % Q)
print("Share of truly-faulty target-ticks where >=7 of 9 peers output 'no fault' for that class (a quorum-sized wrong verdict):")
zsp = corrupt(zs.reshape(-1, K), "spoof_healthy").reshape(S, NT, K)
votes = {k: np.zeros((9,) + (int((split_of_row == 2).sum()), 5), bool) for k in ("A", "C", "C*")}
for o in range(9):
    ob, zo, ag = observer_view(np.random.default_rng(100 + o))
    zc = np.take_along_axis(zsp, np.maximum(np.maximum.accumulate(np.where(ob[..., 0] > 0, tt, -1), 1), 0)[..., None], 1)  # spoof carried the same staleness
    zc = zc * (np.maximum.accumulate(np.where(ob[..., 0] > 0, tt, -1), 1) >= 0)[..., None]
    h = heard9()[..., None]
    for kind in ("A", "C", "C*"):
        m, thr = res[kind]; base = "C" if kind == "C*" else kind
        X = T_(feats(base, ob, zc, h, ag).reshape(S * NT, -1)[split_of_row == 2])
        votes[kind][o] = pred(m, X) >= thr
for kind in ("A", "C", "C*"):
    nof = (~votes[kind]).sum(0) >= 7
    print("  head %-2s: " % kind + "  ".join("%s %.3f" % (c[:4], (nof[:, i] & (ytest[:, i] > .5)).sum() / max((ytest[:, i] > .5).sum(), 1)) for i, c in enumerate(CLASSES)))
json.dump(out, open(f"{WORK}/peer_head_results.json", "w"))

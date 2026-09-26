"""THROWAWAY. Step 4: can a peer reproduce the fault scores from a small payload?

Inputs : work/prep/X_{train,val,test}_scaled.npy, y_*.npy (from 01_prepare.py, i.e.
         data_prep-2.py's own features and run_id group split), work/prep/wire_spec.json
         (per-feature quantiser from 03_wire_format.py).
Metric : per-class precision/recall/F1 on TEST at a per-class threshold chosen on VAL
         (max F1) - the same protocol for every option.
Options: full (baseline) | split-bottleneck k=4/8/16 int8 (QAT) and 4-bit | PCA k | AE k |
         scores-only (u8, u4) | packed format (features quantised per wire_spec, full model).
"""
import json, os, sys, time
import numpy as np, torch, torch.nn as nn
from sklearn.decomposition import PCA

WORK = os.environ.get("TRON_G_WORK", os.path.abspath("work"))
P = f"{WORK}/prep"
SEEDS = [int(s) for s in os.environ.get("SEEDS", "0,1,2").split(",")]
EPOCHS = int(os.environ.get("EPOCHS", "8"))
CLASSES = ["mobility", "communication", "resource", "environment", "mission"]
torch.set_num_threads(os.cpu_count())

Xtr, Xva, Xte = [np.load(f"{P}/X_{s}_scaled.npy") for s in ("train", "val", "test")]
ytr, yva, yte = [np.load(f"{P}/y_{s}.npy").astype(np.float32) for s in ("train", "val", "test")]
D = Xtr.shape[1]
print("train/val/test rows:", len(Xtr), len(Xva), len(Xte), " D =", D, " pos rate train:", ytr.mean(0).round(4))
T = lambda a: torch.from_numpy(np.ascontiguousarray(a, dtype=np.float32))
Xtr_t, Xva_t, Xte_t, ytr_t = T(Xtr), T(Xva), T(Xte), T(ytr)
pos_w = torch.clamp((1 - ytr_t.mean(0)) / ytr_t.mean(0).clamp_min(1e-4), max=20.0) ** 0.5   # mild reweight


def fq(z, bits):
    """Uniform fake-quantiser on [-1,1], straight-through estimator."""
    n = 2 ** (bits - 1) - 1
    q = torch.round(z.clamp(-1, 1) * n) / n
    return z + (q - z).detach()


class Net(nn.Module):
    def __init__(self, din, k=None, bits=8, hidden=(128, 64), head=(64, 32)):
        super().__init__()
        self.k, self.bits = k, bits
        L, d = [], din
        for h in hidden:
            L += [nn.Linear(d, h), nn.ReLU()]; d = h
        self.enc = nn.Sequential(*L)
        self.bott = nn.Linear(d, k) if k else None
        d = k if k else d
        H = []
        for h in head:
            H += [nn.Linear(d, h), nn.ReLU()]; d = h
        H += [nn.Linear(d, 5)]
        self.head = nn.Sequential(*H)

    def code(self, x):
        z = self.enc(x)
        if self.bott is not None:
            z = fq(torch.tanh(self.bott(z)), self.bits)
        return z

    def forward(self, x):
        return self.head(self.code(x))


def fit(model, X, y, Xv, yv, epochs=EPOCHS, bs=2048, lr=2e-3, seed=0):
    g = torch.Generator().manual_seed(seed)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=lr, total_steps=epochs * ((len(X) + bs - 1) // bs))
    lossf = nn.BCEWithLogitsLoss(pos_weight=pos_w)
    best, best_state = 1e9, None
    for ep in range(epochs):
        model.train()
        perm = torch.randperm(len(X), generator=g)
        for i in range(0, len(X), bs):
            idx = perm[i:i + bs]
            opt.zero_grad(); lossf(model(X[idx]), y[idx]).backward(); opt.step(); sched.step()
        model.eval()
        with torch.no_grad():
            vl = lossf(model(Xv), yv).item()
        if vl < best:
            best, best_state = vl, {k: v.clone() for k, v in model.state_dict().items()}
    model.load_state_dict(best_state); model.eval()
    return model


def predict(model, X):
    with torch.no_grad():
        return torch.sigmoid(torch.cat([model(X[i:i + 100000]) for i in range(0, len(X), 100000)])).numpy()


def prf(y, s, thr):
    p = s >= thr
    tp = (p & (y > 0.5)).sum(0); fp = (p & (y < 0.5)).sum(0); fn = (~p & (y > 0.5)).sum(0)
    pr = tp / np.maximum(tp + fp, 1); rc = tp / np.maximum(tp + fn, 1)
    return pr, rc, 2 * pr * rc / np.maximum(pr + rc, 1e-9)


def tune(y, s):
    grid = np.linspace(0.02, 0.98, 49)
    return np.array([grid[np.argmax([prf(y[:, [c]], s[:, [c]], t)[2][0] for t in grid])] for c in range(5)])


def evaluate(name, sva, ste, payload, extra=""):
    thr = tune(yva, sva)
    pr, rc, f1 = prf(yte, ste, thr)
    return dict(name=name, payload_bytes=payload, f1=f1.tolist(), p=pr.tolist(), r=rc.tolist(),
                macro_f1=float(f1.mean()), extra=extra)


def quant_unit(a, lo, hi, bits):
    n = 2 ** bits - 1
    return np.round((np.clip(a, lo, hi) - lo) / (hi - lo) * n) / n * (hi - lo) + lo


results = []
def rec(seed, r):
    r["seed"] = seed; results.append(r)
    print("seed %d %-28s bytes=%-6s macroF1=%.4f  F1=%s" % (seed, r["name"], r["payload_bytes"], r["macro_f1"],
          " ".join("%.3f" % x for x in r["f1"])), flush=True)


for seed in SEEDS:
    torch.manual_seed(seed); np.random.seed(seed)
    t0 = time.time()
    # --- baseline
    full = fit(Net(D), Xtr_t, ytr_t, Xva_t, T(yva), seed=seed)
    sva, ste = predict(full, Xva_t), predict(full, Xte_t)
    rec(seed, evaluate("full-input", sva, ste, "n/a (D=%d floats)" % D))
    # --- (d) scores only, quantised
    for bits in (8, 4):
        n = 2 ** bits - 1
        rec(seed, evaluate("(d) scores-only u%d" % bits, np.round(sva * n) / n, np.round(ste * n) / n,
                           "%.1f" % (5 * bits / 8)))
    # --- (e) packed format: features quantised per wire_spec, full model unchanged
    for eb in (8, 6):
        Xqv, Xqt = [T(np.load(f"{P}/Xq{eb}_{n}_scaled.npy")) for n in ("val", "test")]
        rec(seed, evaluate("(e) packed evid=%db (model unchanged)" % eb, predict(full, Xqv), predict(full, Xqt),
                           "%.1f" % ({8: 181, 6: 167}[eb] / 8)))
    for k in (4, 8, 16):
        # --- (a) split bottleneck, int8 QAT and 4-bit QAT
        for bits in (8, 4):
            m = fit(Net(D, k=k, bits=bits), Xtr_t, ytr_t, Xva_t, T(yva), seed=seed)
            rec(seed, evaluate("(a) split k=%d int%d QAT" % (k, bits), predict(m, Xva_t), predict(m, Xte_t),
                               "%.1f" % (k * bits / 8)))
        # --- (b) PCA -> int8 -> MLP head
        pca = PCA(n_components=k, random_state=seed).fit(Xtr[::5])
        Ztr, Zva, Zte = [pca.transform(a).astype(np.float32) for a in (Xtr, Xva, Xte)]
        lo, hi = np.percentile(Ztr, 0.1, 0), np.percentile(Ztr, 99.9, 0)
        qz = lambda Z: np.stack([quant_unit(Z[:, i], lo[i], hi[i], 8) for i in range(k)], 1).astype(np.float32)
        Ztr, Zva, Zte = qz(Ztr), qz(Zva), qz(Zte)
        mu, sd = Ztr.mean(0), Ztr.std(0) + 1e-6
        Ztr, Zva, Zte = [(a - mu) / sd for a in (Ztr, Zva, Zte)]
        h = fit(Net(k, hidden=(), head=(64, 32)), T(Ztr), ytr_t, T(Zva), T(yva), seed=seed)
        rec(seed, evaluate("(b) PCA k=%d int8" % k, predict(h, T(Zva)), predict(h, T(Zte)), str(k),
                           "explained var %.3f" % pca.explained_variance_ratio_.sum()))
        # --- (c) autoencoder (unsupervised) -> int8 latent -> MLP head
        ae_enc = nn.Sequential(nn.Linear(D, 64), nn.ReLU(), nn.Linear(64, k), nn.Tanh())
        ae_dec = nn.Sequential(nn.Linear(k, 64), nn.ReLU(), nn.Linear(64, D))
        opt = torch.optim.Adam(list(ae_enc.parameters()) + list(ae_dec.parameters()), lr=2e-3)
        for ep in range(max(3, EPOCHS // 2)):
            perm = torch.randperm(len(Xtr_t))
            for i in range(0, len(perm), 2048):
                x = Xtr_t[perm[i:i + 2048]]
                opt.zero_grad(); nn.functional.mse_loss(ae_dec(fq(ae_enc(x), 8)), x).backward(); opt.step()
        with torch.no_grad():
            enc = lambda X: fq(ae_enc(X), 8)
            Ztr, Zva, Zte = enc(Xtr_t), enc(Xva_t), enc(Xte_t)
        h = fit(Net(k, hidden=(), head=(64, 32)), Ztr, ytr_t, Zva, T(yva), seed=seed)
        rec(seed, evaluate("(c) autoencoder k=%d int8" % k, predict(h, Zva), predict(h, Zte), str(k)))
    print("seed %d done in %.0fs" % (seed, time.time() - t0), flush=True)

json.dump(results, open(f"{WORK}/bottleneck_results.json", "w"), indent=1)
# summary table: mean over seeds
names = list(dict.fromkeys(r["name"] for r in results))
print("\n| option | payload B | " + " | ".join(CLASSES) + " | macro |")
print("|---|---|" + "---|" * 6)
for n in names:
    rs = [r for r in results if r["name"] == n]
    f = np.mean([r["f1"] for r in rs], 0)
    print("| %s | %s | %s | %.3f |" % (n, rs[0]["payload_bytes"], " | ".join("%.3f" % x for x in f), f.mean()))

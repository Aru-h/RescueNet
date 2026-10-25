#!/usr/bin/env python3
"""
laptop/tools/headroom_report.py -- Step 5 analysis (docs/HEADROOM_MAP_PREREG.md, sections 6 and 10.6).

  headroom_report.py screen  --res DIR [--tiers 1,2,3] [--out FILE.md] [--json FILE.json]
  headroom_report.py confirm --res DIR [--tiers ...]       survivors of the screen, on the held-out seeds, with Holm
  headroom_report.py budget  --res DIR                      time budget (TB) per stress and specialization
  headroom_report.py abs     --res DIR --configs P,PD ...   absolute means per stress

Headroom = probe minus matched baseline, paired by seed. Incidents resolved: positive is better. Victims lost is reported
as baseline minus probe, so positive is better for both. Paired bootstrap, 20000 resamples, seed 0. Survives the
screen when, for at least one metric, the CI lower bound is above 0 and the point estimate is at least the
materiality threshold (1.0 incident or 1.0 victim per seed). Holm over the whole screened family.
"""
import argparse
import glob
import json
import os
import sys

import numpy as np

B = 20000
THRESH = {"incidents": 1.0, "victims": 1.0}
DEV = range(1001, 1101)
HELD = range(3001, 3101)

# id, label, probe config, baseline config, tier, kind (probe | effect | capacity | ref), realisable on a real system
C = []


def add(cid, label, probe, base, tier, kind="probe", real=""):
    C.append(dict(id=cid, label=label, probe=probe, base=base, tier=tier, kind=kind, real=real))


add("D2_0", "D2 detector oracle, no delay (PD)", "PD", "P", 1, "probe", "ceiling: a perfect detector")
for k in (5, 10, 20, 40):
    add(f"D2_{k}", f"D2 detector oracle delayed {k} ticks", f"D2_{k}", "P", 1, "probe", "any real detector has delay")
add("D3a", "D3a oracle, 30% misses / 1% false alarms", "D3a", "P", 1, "probe", "plausible detector operating point")
for q in (25, 50, 75):
    add(f"D3b_deg_{q}", f"D3b baseline evidence dropout {q}% (effect on P)", f"D3b_P_{q}", "P", 1, "effect", "sensor dropout")
    add(f"D3b_PD_{q}", f"D3b oracle headroom at dropout {q}%", f"D3b_PD_{q}", f"D3b_P_{q}", 1, "probe", "ceiling")
    add(f"D3b_PA_{q}", f"D3b shipped-AI gain at dropout {q}%", f"D3b_PA_{q}", f"D3b_P_{q}", 1, "probe", "shipped AI, context only")
for m in ("1", "0.5", "0.25"):
    add(f"S1_ii_{m}", f"S1 hazard forecast + prevention x{m} (level ii)", f"S1_ii_{m}", "P", 1, "probe", "learnable (wear model)")
    add(f"S1_i_{m}", f"S1 draw-aware forecast + prevention x{m} (level i)", f"S1_i_{m}", "P", 1, "probe", "not realisable (draws)")
add("M1_ii", "M1 survival-rate urgency (level ii)", "M1_ii", "P", 1, "probe", "yes: age and victim count are known")
add("M1_i", "M1 realised-death urgency (level i)", "M1_i", "P", 1, "probe", "not realisable")
add("M2", "M2 fire-spread foresight, 50 ticks", "M2", "P", 1, "probe", "partly (fire forecast)")
add("M3", "M3 aftershock-collapse foresight, 50 ticks", "M3", "P", 1, "probe", "unlikely (collapse timing)")
for t in (10, 50, 200):
    add(f"M4_{t}", f"M4 hazard-arrival foresight, {t} ticks", f"M4_{t}", "P", 1, "probe", "partly")
add("M5", "M5 discovery oracle (no fog of war)", "M5", "P", 1, "probe", "search or reporting system")
add("DQ1", "DQ1 central optimal assignment", "DQ1", "P", 1, "probe", "laptop-only bound")
add("DQ2", "DQ2 perfect comms (no comm faults, no bus loss)", "DQ2", "P", 1, "probe", "better radios or relays")
for x in ("0.5", "0.75", "1.5", "2"):
    add(f"K1_rest_{x}", f"K1 rest duration x{x}", f"K1_rest_{x}", "P", 1, "capacity", "operations")
    add(f"K1_drain_{x}", f"K1 stamina drain x{x}", f"K1_drain_{x}", "P", 1, "capacity", "operations")
for k in (1, 2, 3):
    add(f"K2_{k}", f"K2 {k} team(s) offline", f"K2_{k}", "P", 1, "capacity", "measurement")
for x in ("1.25", "1.5", "2", "3", "4"):
    add(f"W1a_{x}", f"W1a work x{x} (drain per tick unchanged)", f"W1a_{x}", "P", 2, "probe", "needs tools and machinery")
    add(f"W1b_{x}", f"W1b work x{x} (drain tied to work)", f"W1b_{x}", "P", 2, "probe", "needs tools and machinery")
for s in ("search", "medical", "extraction", "perimeter", "support"):
    for x in (2, 3):
        add(f"W2_{s}_{x}", f"W2 {s} work x{x}", f"W2_{s}_{x}", "P", 2, "probe", "needs tools")
for st in ("early", "late"):
    for x in (2, 4):
        add(f"W3_{st}_{x}", f"W3 {st} stages x{x}", f"W3_{st}_{x}", "P", 2, "probe", "needs tools")
add("W0", "W0 instant work", "W0", "P", 2, "probe", "ceiling")
add("V0_task", "V0 instant travel to tasks", "V0_task", "P", 2, "probe", "ceiling")
add("V0_all", "V0 instant travel to tasks and to rest", "V0_all", "P", 2, "probe", "ceiling")
for x in (2, 4):
    add(f"V1_{x}", f"V1 search/extraction x{x} direct, perimeter x0.5", f"V1_{x}", "P", 2, "probe", "aerial or off-road assets")
    add(f"V1_{x}_act", f"V1 x{x}: road actions on vs off", f"V1_{x}_act", f"V1_{x}", 2, "probe", "already implemented")
for m in "abc":
    add(f"TM1_{m}", f"TM1 team mix {m}", f"TM1_{m}", "P", 2, "probe", "free (configuration)")
add("R_info", "R1: value of road information (omniscient minus R1)", "P", "R1", 3, "probe", "ceiling for any road-information system")
add("R_info_s", "R1s: omniscient minus strict R1 (radius 0)", "P", "R1s", 3, "probe", "ceiling")
add("R_pred", "R2 minus R1: value of a damage predictor", "R2", "R1", 3, "probe", "no structure in this sim")
add("R_share", "R3 minus R1: value of better sharing", "R3", "R1", 3, "probe", "broadcast, mesh")
add("ACT_omni", "REROUTE+CLEAR_ROAD on vs off, omniscient", "ACT", "P", 3, "probe", "implemented")
add("ACT_R1", "REROUTE+CLEAR_ROAD on vs off, under R1", "R1_act", "R1", 3, "probe", "implemented")
add("ACT_R2", "REROUTE+CLEAR_ROAD on vs off, under R2", "R2_act", "R2", 3, "probe", "implemented")
add("RR1", "RR1 relief, rate-aware trigger", "RR1", "P", 4, "probe", "operations")
add("RR1f", "RR1 relief, perfect-foresight trigger", "RR1f", "P", 4, "probe", "not realisable (realised faults)")
add("RR2a", "RR2a SUPPORT-delivered repair (idle or priced preemption)", "RR2a", "P", 4, "probe", "needs spares and a free support team")
add("RR3", "RR3 lane first (clear_road on) vs clear_road alone", "RR3", "RR3_base", 4, "probe", "Tohoku-style lane opening")
add("RR4", "RR4 degraded mode, capability-aware bids", "RR4", "P", 4, "probe", "yes: scoring rule")
add("RR5", "RR5 autonomy on communication loss", "RR5", "P", 4, "probe", "yes: operating rule")
add("RR0", "RR0 all five responses (not jointly priced)", "RR0_P", "P", 4, "probe", "see components")
add("RR_key_PD", "KEY: detector-oracle headroom with RR0 on (PD+RR0 minus P+RR0)", "RR0_PD", "RR0_P", 4, "probe", "ceiling")
add("RR_key_S1", "KEY: hazard-oracle + prevention with RR0 on (minus P+RR0)", "RR0_S1", "RR0_P", 4, "probe", "level ii forecast")
for _k, _lab in [("RK_lat_0", "latency 0"), ("RK_lat_10", "latency 10"), ("RK_lat_30", "latency 30"), ("RK_lat_100", "latency 100"),
                 ("RK_loss_0.2", "loss 0.2"), ("RK_loss_0.5", "loss 0.5"), ("RK_rad_2", "radius 2"), ("RK_rad_4", "radius 4")]:
    add(_k + "_val", f"C: value of road information at {_lab} (omniscient minus setting)", "P", _k, 5, "probe", "ceiling")
    add(_k + "_act", f"C: REROUTE+CLEAR_ROAD on vs off at {_lab}", _k + "_act", _k, 5, "probe", "implemented")
add("GAP", "reference: no faults (fixed gates) minus P", "NOFAULT", "P", 0, "ref", "")


CSV = None   # {(stress, config, seed): result-like dict}, filled by --csv so the analysis runs from the committed tables


def load_csv(paths):
    import csv
    global CSV
    CSV = {}
    for p in paths:
        with open(p, newline="") as fh:
            for row in csv.DictReader(fh):
                def num(k):
                    v = row.get(k, "")
                    return float(v) if v != "" else 0.0
                CSV[(float(row["stress"]), row["config"], int(row["seed"]))] = {
                    "incidents": num("incidents"), "victims_lost": num("victims_lost"), "victims_seen": num("victims_seen"),
                    "spawned": num("spawned"), "resolved_by_prio": {"1": num("p1_resolved")}, "prio_lost": [num("p1_victims_lost")],
                    "insvc": num("teams_in_service"), "tb": {}, "seed": int(row["seed"])}


def load(res, stress, cfg, seeds):
    if CSV is not None:
        return {s: CSV[(float(stress), cfg, s)] for s in seeds if (float(stress), cfg, s) in CSV}
    d = os.path.join(res, f"s{stress:g}", cfg)
    out = {}
    for s in seeds:
        p = os.path.join(d, f"{s}.json")
        if os.path.exists(p):
            out[s] = json.load(open(p))
    return out


def metric(r, name):
    if name == "incidents":
        return float(r["incidents"])
    if name == "victims":
        return float(r["victims_lost"])
    if name == "spawned":
        return float(r["spawned"])
    if name == "seen":
        return float(r["victims_seen"])
    if name == "insvc":
        if "insvc" in r:
            return float(r["insvc"])
        return sum(sum(v.get(k, 0) for k in ("on_scene", "to_task", "available")) for v in r["tb"].values()) / 3000.0
    if name == "p1":
        return float(r.get("resolved_by_prio", {}).get("1", 0))
    if name == "p1lost":
        return float(r.get("prio_lost", [0, 0, 0, 0])[0])
    raise KeyError(name)


def boot_idx(n):
    rng = np.random.default_rng(0)
    return rng.integers(0, n, size=(B, n))


def paired(res, stress, probe, base, seeds, name):
    a = load(res, stress, probe, seeds)
    b = load(res, stress, base, seeds)
    common = sorted(set(a) & set(b))
    if len(common) < 5:
        return None
    pa = np.array([metric(a[s], name) for s in common])
    pb = np.array([metric(b[s], name) for s in common])
    d = (pa - pb) if name == "incidents" else (pb - pa) if name == "victims" else (pa - pb)
    return common, d


def ci_p(d):
    n = len(d)
    idx = boot_idx(n)
    means = d[idx].mean(axis=1)
    lo, hi = np.percentile(means, [2.5, 97.5])
    le = (np.sum(means <= 0) + 1) / (B + 1)
    ge = (np.sum(means >= 0) + 1) / (B + 1)
    p = min(1.0, 2 * min(le, ge))
    return float(d.mean()), float(lo), float(hi), float(p), means


def fit_slope(res, stress, seeds, idx=None):
    cfgs = ["P"] + [c["probe"] for c in C if c["id"].startswith("K1_")]
    xs, ys = [], []
    data = {c: load(res, stress, c, seeds) for c in cfgs}
    common = sorted(set.intersection(*[set(v) for v in data.values()])) if all(data.values()) else []
    if len(common) < 5:
        return None
    X = np.array([[metric(data[c][s], "insvc") for s in common] for c in cfgs])
    Y = np.array([[metric(data[c][s], "incidents") for s in common] for c in cfgs])

    def slope(ix):
        x = X[:, ix].mean(axis=1)
        y = Y[:, ix].mean(axis=1)
        vx = ((x - x.mean()) ** 2).sum()
        return float(((x - x.mean()) * (y - y.mean())).sum() / vx) if vx > 1e-9 else float("nan")

    n = len(common)
    s0 = slope(np.arange(n))
    bi = boot_idx(n)[:2000]
    sb = np.array([slope(i) for i in bi])
    return s0, sb, common


def fmt(m, lo, hi):
    return f"{m:+.2f} [{lo:+.2f}, {hi:+.2f}]"


def screen_rows(res, tiers, seeds, stresses=(2, 7)):
    rows = []
    for c in C:
        if c["tier"] not in tiers and not (c["tier"] == 0 and 0 in tiers):
            continue
        for st in stresses:
            r = {"c": c, "stress": st}
            for name in ("incidents", "victims"):
                pr = paired(res, st, c["probe"], c["base"], seeds, name)
                if pr is None:
                    r[name] = None
                    continue
                common, d = pr
                m, lo, hi, p, means = ci_p(d)
                r[name] = dict(n=len(common), mean=m, lo=lo, hi=hi, p=p)
            for name in ("spawned", "seen", "p1", "insvc"):
                pr = paired(res, st, c["probe"], c["base"], seeds, name)
                if pr is not None:
                    r[name] = float(pr[1].mean())
            rows.append(r)
    return rows


def survives(row):
    out = []
    for name in ("incidents", "victims"):
        v = row.get(name)
        if v and v["lo"] > 0 and v["mean"] >= THRESH[name]:
            out.append(name)
    return out


def te(res, st, seeds, row_mean, probe, base, name):
    sl = fit_slope(res, st, seeds)
    if sl is None or name != "incidents":
        return None
    s0, sb, _ = sl
    if not np.isfinite(s0) or s0 <= 0.05:
        return None
    pr = paired(res, st, probe, base, seeds, name)
    if pr is None:
        return None
    d = pr[1]
    n = len(d)
    bi = boot_idx(n)[:2000]
    hb = d[bi].mean(axis=1)
    ok = sb > 0.05
    tb = hb[ok] / sb[ok]
    lo, hi = np.percentile(tb, [2.5, 97.5]) if len(tb) > 50 else (float("nan"), float("nan"))
    return row_mean / s0, float(lo), float(hi)


def render_screen(res, rows, seeds, title):
    lines = [f"### {title}", "",
             "| Probe | Stress | Incidents / seed [95% CI] | Team-equiv. | Victims lost / seed (positive = fewer) [95% CI] | Spawned | Survives |",
             "|---|---|---|---|---|---|---|"]
    for r in rows:
        c = r["c"]
        inc, vic = r.get("incidents"), r.get("victims")
        if inc is None and vic is None:
            lines.append(f"| {c['label']} | {r['stress']:g} | not run | | | | |")
            continue
        t = te(res, r["stress"], seeds, inc["mean"], c["probe"], c["base"], "incidents") if inc else None
        tstr = f"{t[0]:+.1f} [{t[1]:+.1f}, {t[2]:+.1f}]" if t else ""
        sv = survives(r) if c["kind"] == "probe" else []
        lines.append(f"| {c['label']} | {r['stress']:g} | {fmt(inc['mean'], inc['lo'], inc['hi']) if inc else ''} | {tstr} | "
                     f"{fmt(vic['mean'], vic['lo'], vic['hi']) if vic else ''} | {r.get('spawned', float('nan')):+.1f} | "
                     f"{', '.join(sv) if sv else ('(not a probe)' if c['kind'] != 'probe' else 'no')} |")
    return "\n".join(lines)


def holm(pvals, m):
    """pvals: list of (key, p) for the tests that reached confirmation; m: whole screened family size.
    The untested remainder of the family counts as p = 1."""
    order = sorted(pvals, key=lambda kv: kv[1])
    adj = {}
    run = 0.0
    for j, (k, p) in enumerate(order, start=1):
        run = max(run, min(1.0, (m - j + 1) * p))
        adj[k] = run
    return adj


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["screen", "confirm", "budget", "abs"])
    ap.add_argument("--res", required=True)
    ap.add_argument("--tiers", default="0,1,2,3")
    ap.add_argument("--configs", default="")
    ap.add_argument("--out")
    ap.add_argument("--json")
    ap.add_argument("--csv", default="", help="comma-separated per-run CSVs (docs/data/headroom/*.csv) to analyse instead of --res")
    a = ap.parse_args()
    if a.csv:
        load_csv([p for p in a.csv.split(",") if p])
    tiers = {int(t) for t in a.tiers.split(",")}
    text = ""
    if a.cmd == "screen":
        rows = screen_rows(a.res, tiers, DEV)
        text = render_screen(a.res, rows, DEV, "Screen: development seeds 1001-1100")
        fam = sum(1 for r in rows if r["c"]["kind"] == "probe" and r.get("incidents") is not None) * 2
        text += f"\n\nFamily size screened so far (probe contrasts with data x stress x metric): {fam}"
        if a.json:
            json.dump([{k: v for k, v in r.items() if k != "c"} | {"id": r["c"]["id"]} for r in rows], open(a.json, "w"), indent=1)
    elif a.cmd == "confirm":
        rows = screen_rows(a.res, tiers, DEV)
        fam = sum(1 for r in rows if r["c"]["kind"] == "probe" and r.get("incidents") is not None) * 2
        lines = ["### Confirmation: held-out seeds 3001-3100 (survivors of the screen)", "",
                 f"Family size m = {fam} (probe contrasts x stress x metric). Holm over the whole family; unadjusted p beside it.", "",
                 "| Probe | Stress | Metric | Screen | Held-out [95% CI] | p | Holm p | Confirmed |", "|---|---|---|---|---|---|---|---|"]
        tests = []
        for r in rows:
            for name in survives(r) if r["c"]["kind"] == "probe" else []:
                pr = paired(a.res, r["stress"], r["c"]["probe"], r["c"]["base"], HELD, name)
                tests.append((r, name, pr))
        pv = []
        for i, (r, name, pr) in enumerate(tests):
            if pr is not None:
                pv.append((i, ci_p(pr[1])[3]))
        adj = holm(pv, max(fam, 1))
        for i, (r, name, pr) in enumerate(tests):
            c = r["c"]
            v = r[name]
            if pr is None:
                lines.append(f"| {c['label']} | {r['stress']:g} | {name} | {fmt(v['mean'], v['lo'], v['hi'])} | not run | | | |")
                continue
            m, lo, hi, p, _ = ci_p(pr[1])
            conf = (lo > 0 and np.sign(m) == np.sign(v["mean"]) and adj[i] < 0.05)
            lines.append(f"| {c['label']} | {r['stress']:g} | {name} | {fmt(v['mean'], v['lo'], v['hi'])} | {fmt(m, lo, hi)} | {p:.4f} | {adj[i]:.3f} | {'yes' if conf else 'no'} |")
        if not tests:
            lines.append("| (no probe survived the screen) | | | | | | | |")
        text = "\n".join(lines)
    elif a.cmd == "budget":
        lines = ["### Time budget (share of team-ticks, mean over seeds)", "",
                 "| Condition | Spec | resting | to rest | held in repair | faulted | on scene | to task | available | in service (teams of 10) |",
                 "|---|---|---|---|---|---|---|---|---|---|"]
        names = ["resting", "to_rest", "held", "faulted", "on_scene", "to_task", "available"]
        specs = ["SEARCH", "MEDICAL", "EXTRACTION", "PERIMETER", "SUPPORT"]
        cfgs = [("P", 2), ("P", 7), ("F", 2), ("F", 7), ("NOFAULT", 2)]
        for cfg, st in cfgs:
            data = load(a.res, st, cfg, DEV)
            if not data:
                continue
            tot = {sp: {n: 0 for n in names} for sp in range(5)}
            for r in data.values():
                for sp, v in r["tb"].items():
                    for n in names:
                        tot[int(sp)][n] += v.get(n, 0)
            allv = {n: sum(tot[sp][n] for sp in range(5)) for n in names}
            T = sum(allv.values())
            if T == 0:
                continue   # no time budget in this run (the no-fault reference has no probe diagnostics)
            label = f"{cfg} stress {st:g}" if cfg != "NOFAULT" else "no faults (fixed gates)"
            lines.append(f"| {label} | all | " + " | ".join(f"{100 * allv[n] / T:.1f}%" for n in names) +
                         f" | {10 * (allv['on_scene'] + allv['to_task'] + allv['available']) / T:.2f} |")
            for sp in range(5):
                Ts = sum(tot[sp].values())
                if Ts:
                    lines.append(f"| | {specs[sp]} | " + " | ".join(f"{100 * tot[sp][n] / Ts:.1f}%" for n in names) +
                                 f" | {10 * (tot[sp]['on_scene'] + tot[sp]['to_task'] + tot[sp]['available']) / Ts:.2f} |")
        text = "\n".join(lines)
    elif a.cmd == "abs":
        lines = ["| Config | Stress | Incidents | Victims lost | Spawned | Seeds |", "|---|---|---|---|---|---|"]
        for cfg in a.configs.split(","):
            for st in (1, 2, 3.5, 7):
                d = load(a.res, st, cfg, list(DEV) + list(HELD))
                if d:
                    lines.append(f"| {cfg} | {st:g} | {np.mean([metric(r, 'incidents') for r in d.values()]):.2f} | "
                                 f"{np.mean([metric(r, 'victims') for r in d.values()]):.2f} | "
                                 f"{np.mean([metric(r, 'spawned') for r in d.values()]):.1f} | {len(d)} |")
        text = "\n".join(lines)
    print(text)
    if a.out:
        open(a.out, "w").write(text + "\n")


if __name__ == "__main__":
    main()

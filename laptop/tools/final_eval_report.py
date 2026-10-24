#!/usr/bin/env python3
"""Step 4 analysis for laptop/tools/final_eval.sh: tables, paired intervals, teams in service and figures.

  final_eval_report.py <out dir> [--stress "1 2 3.5 7"] [--figures DIR] [--ref-3.6 DIR]

<out dir>/s<stress>/<D|H>_<system>.json are fault_exposure.py --json files (D = development seeds 1001-1020, H = held-out
3001-3020; systems F, P, PD, FA, PA; stress 0 holds the no-fault reference, system F).  Writes <out dir>/summary.md and
results.json, and PNG figures if matplotlib is installed.  Paired differences are by seed; the 95% intervals are the
paired bootstrap used throughout the project (10000 resamples, seed 0, planner_report.ci).
"""
import argparse, json, os, random, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from planner_report import ci  # noqa: E402

SYSTEMS = ["F", "P", "PD", "FA", "PA"]
LONG = {"F": "fixed gates", "P": "planner (no AI)", "PD": "planner + detector oracle (ceiling)",
        "FA": "fixed gates + AI", "PA": "planner + AI"}
AI_SYSTEMS = ("FA", "PA")
SVC = ["working", "available", "faulted", "returning", "resting"]


def load(out, st, name):
    rec = {}
    for s in "DH":
        p = os.path.join(out, f"s{st}", f"{s}_{name}.json")
        if os.path.exists(p):
            for seed, v in json.load(open(p))["per_seed"].items():
                rec[(s, seed)] = v
    return rec


def mean(v):
    return sum(v) / len(v) if v else float("nan")


def fmt_ci(v):
    if not v:
        return "n/a"
    lo, hi = ci(v)
    return f"{mean(v):+.2f} [{lo:+.2f}, {hi:+.2f}]"


def paired(out, st, a, b, key):
    """b minus a, paired by seed -> {'D': [...], 'H': [...], 'P': pooled}"""
    ra, rb = load(out, st, a), load(out, st, b)
    res = {"D": [], "H": []}
    for k in sorted(set(ra) & set(rb)):
        if ra[k].get(key) is not None and rb[k].get(key) is not None:
            res[k[0]].append(rb[k][key] - ra[k][key])
    res["P"] = res["D"] + res["H"]
    return res


def level(out, st, name, key):
    r = load(out, st, name)
    d = [v[key] for k, v in sorted(r.items()) if k[0] == "D" and v.get(key) is not None]
    h = [v[key] for k, v in sorted(r.items()) if k[0] == "H" and v.get(key) is not None]
    return d, h, d + h


def service(out, st, name):
    r = load(out, st, name)
    if not r:
        return None
    sh = {k: mean([v["service"][k] for v in r.values() if v.get("service")]) for k in SVC}
    sh["in_service"] = sh["working"] + sh["available"]
    sh["teams_in_service"] = 10.0 * sh["in_service"]
    return sh


def rank(v):
    order = sorted(range(len(v)), key=lambda i: v[i])
    r = [0.0] * len(v)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and v[order[j + 1]] == v[order[i]]:
            j += 1
        for k in range(i, j + 1):
            r[order[k]] = (i + j) / 2.0 + 1.0
        i = j + 1
    return r


def spearman(x, y):
    if len(x) < 3:
        return float("nan")
    rx, ry = rank(x), rank(y)
    mx, my = mean(rx), mean(ry)
    num = sum((a - mx) * (b - my) for a, b in zip(rx, ry))
    den = (sum((a - mx) ** 2 for a in rx) * sum((b - my) ** 2 for b in ry)) ** 0.5
    return num / den if den else float("nan")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--stress", default="1 2 3.5 7")
    ap.add_argument("--figures", default=None)
    ap.add_argument("--ref-3.6", dest="ref36", default=None)
    a = ap.parse_args()
    out = a.out
    stresses = a.stress.split()
    label = open(os.path.join(out, "model_label.txt")).read().strip() if os.path.exists(os.path.join(out, "model_label.txt")) \
        else "shipped model (trained on the old fault model - context only)"
    prov = open(os.path.join(out, "provenance.txt")).read() if os.path.exists(os.path.join(out, "provenance.txt")) else ""
    L = []
    w = L.append
    res = {"model_label": label, "stress": {}}

    w("# Final evaluation: results\n")
    w(f"AI rows: **{label}**.\n")
    if prov:
        w("```\n" + prov.strip() + "\n```\n")

    # ---- levels
    w("## Incidents resolved and victims lost per seed\n")
    w("| Stress | System | Incidents dev | held-out | pooled | Victims lost dev | held-out | pooled | Teams in service (of 10) |")
    w("|---|---|---|---|---|---|---|---|---|")
    for st in stresses:
        res["stress"][st] = {"levels": {}, "service": {}}
        for sy in SYSTEMS:
            d, h, p = level(out, st, sy, "incidents_resolved")
            if not p:
                continue
            vd, vh, vp = level(out, st, sy, "victims_lost")
            sv = service(out, st, sy)
            res["stress"][st]["levels"][sy] = {"incidents": mean(p), "victims_lost": mean(vp)}
            res["stress"][st]["service"][sy] = sv
            nm = f"{sy} {LONG[sy]}" + (" *" if sy in AI_SYSTEMS else "")
            w(f"| {st} | {nm} | {mean(d):.2f} | {mean(h):.2f} | {mean(p):.2f} | {mean(vd):.2f} | {mean(vh):.2f} | {mean(vp):.2f} | "
              f"{sv['teams_in_service']:.2f} |")
    w(f"\n\\* {label}.\n")
    d0, h0, p0 = level(out, "0", "F", "incidents_resolved")
    if p0:
        sv0 = service(out, "0", "F")
        vp0 = level(out, "0", "F", "victims_lost")[2]
        vtxt = f"{mean(vp0):.2f} victims lost, " if vp0 else "victims lost not reported for this profile, "
        w(f"Reference, no faults at all (fixed gates, `--fault-profile=none`): {mean(p0):.2f} incidents resolved per seed, "
          f"{vtxt}teams in service {sv0['teams_in_service']:.2f} of 10. Added after the pre-registration "
          f"as a reference row only.\n")
        res["reference_none"] = {"incidents": mean(p0), "victims_lost": (mean(vp0) if vp0 else None), "service": sv0}

    # ---- teams in service
    w("## Teams in service (share of team-ticks)\n")
    w("Resting, returning to rest, faulted (mobility or resource fault active) and in service (working or available).\n")
    w("| Stress | System | Working | Available | Faulted | Returning | Resting | In service | Teams in service |")
    w("|---|---|---|---|---|---|---|---|---|")
    for st in stresses + (["0"] if p0 else []):
        for sy in (SYSTEMS if st != "0" else ["F"]):
            sv = service(out, st, sy)
            if sv:
                w(f"| {st} | {sy} | " + " | ".join(f"{100 * sv[k]:.1f}%" for k in SVC) +
                  f" | {100 * sv['in_service']:.1f}% | {sv['teams_in_service']:.2f} |")
    w("")

    # ---- paired differences
    w("## Paired differences, incidents resolved per seed (95% bootstrap CI)\n")
    w("| Stress | Comparison | Dev | Held-out | Pooled |")
    w("|---|---|---|---|---|")
    comps = [("Detector headroom: PD minus P", "P", "PD"), ("Planner minus fixed gates: P minus F", "F", "P"),
             ("Shipped-AI gain on fixed gates: FA minus F *", "F", "FA"), ("Shipped-AI gain on planner: PA minus P *", "P", "PA")]
    head = {}
    ok = []
    for st in stresses:
        for title, base, other in comps:
            r = paired(out, st, base, other, "incidents_resolved")
            if not r["P"]:
                continue
            w(f"| {st} | {title} | {fmt_ci(r['D'])} | {fmt_ci(r['H'])} | **{fmt_ci(r['P'])}** |")
            lo, hi = ci(r["P"])
            res["stress"][st].setdefault("diffs", {})[title] = {"mean": mean(r["P"]), "lo": lo, "hi": hi,
                                                                "dev": mean(r["D"]), "held": mean(r["H"])}
            if base == "P" and other == "PD":
                head[st] = (mean(r["P"]), lo, hi)
    w("")
    w("## Paired differences, victims lost per seed (95% CI; no conclusion is drawn from one comparison alone)\n")
    w("| Stress | Comparison | Dev | Held-out | Pooled |")
    w("|---|---|---|---|---|")
    for st in stresses:
        for title, base, other in comps:
            r = paired(out, st, base, other, "victims_lost")
            if r["P"]:
                w(f"| {st} | {title} | {fmt_ci(r['D'])} | {fmt_ci(r['H'])} | {fmt_ci(r['P'])} |")
    w("")

    # ---- headroom vs stress and vs teams in service
    ok = [st for st in stresses if st in head and service(out, st, "P")]
    if len(ok) >= 3:
        xs = [10.0 * (service(out, st, "P")["working"] + service(out, st, "P")["available"]) for st in ok]
        ys = [head[st][0] for st in ok]
        rho_cap = spearman(xs, ys)
        rho_str = spearman([float(s) for s in ok], ys)
        w("## Headroom against stress and against teams in service\n")
        w("| Stress | Teams in service (P) | Detector headroom (PD minus P) |")
        w("|---|---|---|")
        for st, x in zip(ok, xs):
            h = head[st]
            w(f"| {st} | {x:.2f} | {h[0]:+.2f} [{h[1]:+.2f}, {h[2]:+.2f}] |")
        w(f"\nSpearman rank correlation of headroom with stress: {rho_str:+.2f}; with teams in service: {rho_cap:+.2f} "
          f"({len(ok)} points; a descriptive summary, not a test).\n")
        res["headroom_vs_service"] = {"stress": ok, "teams_in_service": xs, "headroom": ys, "rho_stress": rho_str,
                                      "rho_service": rho_cap}

    # ---- fault ending
    w("## What ends a fault (fixed gates F, mean per seed over all seeds)\n")
    w("| Stress | Class | Onsets | Base repair (go-home) | Field repair | Natural recovery | Still active |")
    w("|---|---|---|---|---|---|---|")
    ending = {}
    for st in stresses:
        r = load(out, st, "F")
        if not r:
            continue
        for c, cn, kind in ((0, "mobility", 0), (2, "resource", 1)):
            def m(key):
                return mean([v["persist"].get(key, 0.0) for v in r.values() if v.get("persist")])
            on, base, nat, fld = m(f"fault{c}_onsets"), m(f"fault{c}_cleared_by_cause"), m(f"fault{c}_cleared_by_backstop"), m(f"field{kind}_ok")
            still = on - base - nat - fld
            ending[(st, cn)] = (on, base, fld, nat, still)
            w(f"| {st} | {cn} | {on:.1f} | {base:.1f} | {fld:.1f} | {nat:.1f} | {still:.1f} |")
    w("")

    # ---- reproducibility against Step 3.6
    if a.ref36:
        w("## Reproduction of the Step 3.6 numbers (stress 2 and 7, same seeds)\n")
        names = {"F": "f", "P": "p", "PD": "pdet", "FA": "f_ai", "PA": "p_ai"}
        for st in ("2", "7"):
            if st not in stresses:
                continue
            for sy, old in names.items():
                bad = n = 0
                for s in "DH":
                    p = os.path.join(a.ref36, f"s{st}", f"{s}_{old}.json")
                    if not os.path.exists(p):
                        continue
                    ref = json.load(open(p))["per_seed"]
                    cur = load(out, st, sy)
                    for seed, v in ref.items():
                        n += 1
                        c = cur.get((s, seed))
                        if c is None or c["incidents_resolved"] != v["incidents_resolved"] or c["victims_lost"] != v["victims_lost"]:
                            bad += 1
                w(f"* stress {st}, {sy}: {n - bad} of {n} seeds identical (incidents resolved and victims lost)")
        w("")

    open(os.path.join(out, "summary.md"), "w").write("\n".join(L) + "\n")
    json.dump(res, open(os.path.join(out, "results.json"), "w"), indent=1, default=str)
    print("\n".join(L))

    # ---- figures
    if a.figures:
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
        except Exception as e:  # noqa: BLE001
            print(f"[figures skipped: matplotlib not available ({e}); pip install matplotlib]")
            return
        os.makedirs(a.figures, exist_ok=True)
        col = {"F": "#1b6ca8", "P": "#e08a1e", "PD": "#2a9d8f", "FA": "#7b5ea7", "PA": "#c0392b"}

        def err(st, base, other):
            r = paired(out, st, base, other, "incidents_resolved")["P"]
            lo, hi = ci(r)
            return mean(r), mean(r) - lo, hi - mean(r)

        sx = [float(s) for s in ok]
        fig, ax = plt.subplots(1, 2, figsize=(11, 4.2), constrained_layout=True)
        for (base, other, lab, c, mk) in (("P", "PD", "detector headroom (planner + oracle minus planner)", col["PD"], "o"),
                                          ("F", "FA", "AI gain on fixed gates *", col["FA"], "s"),
                                          ("P", "PA", "AI gain on planner *", col["PA"], "^")):
            pts = [err(st, base, other) for st in ok]
            ax[0].errorbar(sx, [p[0] for p in pts], yerr=[[p[1] for p in pts], [p[2] for p in pts]], marker=mk, color=c,
                           capsize=3, label=lab)
            cap = [10.0 * (service(out, st, "P")["working"] + service(out, st, "P")["available"]) for st in ok]
            ax[1].errorbar(cap, [p[0] for p in pts], yerr=[[p[1] for p in pts], [p[2] for p in pts]], marker=mk, color=c,
                           capsize=3, label=lab)
        for st, x in zip(ok, cap):
            ax[1].annotate(f"stress {st}", (x, head[st][0]), textcoords="offset points", xytext=(4, 6), fontsize=8)
        ax[0].set_xlabel("fault stress (x baseline)")
        ax[1].set_xlabel("mean teams in service (of 10), planner")
        for x_ in ax:
            x_.axhline(0, color="#888", lw=0.8)
            x_.set_ylabel("paired difference, incidents / seed (95% CI)", fontsize=9)
        ax[0].set_xscale("log")
        ax[0].set_xticks(sx)
        ax[0].set_xticklabels([str(s) for s in ok])
        ax[0].legend(fontsize=8, loc="upper left")
        fig.suptitle(f"When does detection matter? Headroom vs stress and vs effective capacity.\n* {label}", fontsize=9)
        fig.savefig(os.path.join(a.figures, "headroom_vs_stress.png"), dpi=150)
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(6.5, 4.2), constrained_layout=True)
        for sy in SYSTEMS:
            xs_, ys_, lo_, hi_ = [], [], [], []
            for st in stresses:
                p = level(out, st, sy, "incidents_resolved")[2]
                if p:
                    random.seed(0)
                    bs = sorted(mean([random.choice(p) for _ in p]) for _ in range(2000))
                    xs_.append(float(st)); ys_.append(mean(p)); lo_.append(mean(p) - bs[50]); hi_.append(bs[1949] - mean(p))
            if xs_:
                ax.errorbar(xs_, ys_, yerr=[lo_, hi_], marker="o", capsize=2, color=col[sy],
                            label=f"{sy} {LONG[sy]}" + (" *" if sy in AI_SYSTEMS else ""))
        if p0:
            ax.axhline(mean(p0), color="#888", ls="--", lw=0.8, label="no faults (reference)")
        ax.set_xscale("log"); ax.set_xticks([float(s) for s in stresses]); ax.set_xticklabels(stresses)
        ax.set_xlabel("fault stress (x baseline)"); ax.set_ylabel("incidents resolved per seed (mean, 95% CI)")
        ax.legend(fontsize=7)
        fig.savefig(os.path.join(a.figures, "incidents_vs_stress.png"), dpi=150)
        plt.close(fig)

        fig, ax = plt.subplots(1, 2, figsize=(10, 4), constrained_layout=True)
        bar_cols = ["#1b6ca8", "#2a9d8f", "#e08a1e", "#c0392b", "#999999"]
        for j, cn in enumerate(("mobility", "resource")):
            bottoms = [0.0] * len(stresses)
            for i, (lab, idx) in enumerate((("go-home repair", 1), ("field repair", 2), ("natural recovery", 3), ("still active", 4))):
                vals = []
                for st in stresses:
                    e = ending.get((st, cn))
                    vals.append(100.0 * e[idx] / e[0] if e and e[0] else 0.0)
                ax[j].bar(range(len(stresses)), vals, bottom=bottoms, color=bar_cols[i], label=lab)
                bottoms = [b + v for b, v in zip(bottoms, vals)]
            ax[j].set_xticks(range(len(stresses))); ax[j].set_xticklabels(stresses)
            ax[j].set_title(f"{cn} faults (fixed gates)"); ax[j].set_xlabel("fault stress"); ax[j].set_ylabel("share of fault onsets (%)")
        ax[0].legend(fontsize=7)
        fig.savefig(os.path.join(a.figures, "fault_ending.png"), dpi=150)
        plt.close(fig)

        fig, ax = plt.subplots(figsize=(6.5, 4), constrained_layout=True)
        labels_ = list(stresses) + (["none"] if p0 else [])
        bottoms = [0.0] * len(labels_)
        for k, lab in enumerate(SVC):
            vals = []
            for st in stresses + (["0"] if p0 else []):
                sv = service(out, st, "P" if st != "0" else "F")
                vals.append(10.0 * sv[lab] if sv else 0.0)
            ax.bar(range(len(labels_)), vals, bottom=bottoms, color=bar_cols[k % len(bar_cols)], label=lab)
            bottoms = [b + v for b, v in zip(bottoms, vals)]
        ax.set_xticks(range(len(labels_))); ax.set_xticklabels(labels_)
        ax.set_xlabel("fault stress (none = no-fault reference, fixed gates)"); ax.set_ylabel("teams (of 10), mean over team-ticks")
        ax.legend(fontsize=7, ncol=2)
        fig.savefig(os.path.join(a.figures, "teams_in_service.png"), dpi=150)
        plt.close(fig)
        print(f"[figures written to {a.figures}]")


if __name__ == "__main__":
    main()

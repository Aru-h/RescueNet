#!/usr/bin/env python3
"""Step 3.5 report: paired comparisons of fault_exposure.py --json runs named <dir>/D_<name>.json (development
seeds) and <dir>/H_<name>.json (held-out seeds), with a bootstrap 95% CI (10000 resamples, seed 0).

  planner_report.py <dir> abs <name> [<name> ...]
        mean incidents resolved and victims lost per seed, development / held-out / pooled
  planner_report.py <dir> diff <base> <name> [<name> ...] [--sets D,H]
        <name> minus <base>, paired by seed: incidents resolved and victims lost, development / held-out / pooled
  planner_report.py <dir> stats <name> [<name> ...]
        mean planner counters per seed (preemptions, approvals, chattering ...)
"""
import json, os, random, sys


def ci(v, n=10000):
    random.seed(0)
    m = sorted(sum(random.choice(v) for _ in v) / len(v) for _ in range(n))
    return m[int(0.025 * n)], m[int(0.975 * n)]


def load(d, s, name):
    p = os.path.join(d, f"{s}_{name}.json")
    return json.load(open(p)) if os.path.exists(p) else None


def per_seed(j, key):
    return {k: v[key] for k, v in j["per_seed"].items() if v.get(key) is not None}


def fmt_diff(vals):
    if not vals:
        return "n/a"
    lo, hi = ci(vals)
    return f"{sum(vals) / len(vals):+.2f} [{lo:+.2f}, {hi:+.2f}]"


def cmd_abs(d, names):
    print(f"{'condition':26s}{'dev inc':>9s}{'held inc':>9s}{'pooled inc':>11s}{'dev lost':>10s}{'held lost':>10s}{'pooled lost':>12s}")
    for n in names:
        row = []
        agg = {"incidents_resolved": [], "victims_lost": []}
        cells = {}
        for s in "DH":
            j = load(d, s, n)
            for key in agg:
                v = list(per_seed(j, key).values()) if j else []
                cells[(s, key)] = f"{sum(v) / len(v):.2f}" if v else "n/a"
                agg[key] += v
        pooled = {k: (f"{sum(v) / len(v):.2f}" if v else "n/a") for k, v in agg.items()}
        print(f"{n:26s}{cells[('D', 'incidents_resolved')]:>9s}{cells[('H', 'incidents_resolved')]:>9s}{pooled['incidents_resolved']:>11s}"
              f"{cells[('D', 'victims_lost')]:>10s}{cells[('H', 'victims_lost')]:>10s}{pooled['victims_lost']:>12s}")


def cmd_diff(d, base, names, sets="DH"):
    print(f"{'condition (minus ' + base + ')':40s}| incidents resolved: dev | held-out | pooled")
    for n in names:
        out = {}
        pooled = {"incidents_resolved": [], "victims_lost": []}
        for s in sets:
            b, o = load(d, s, base), load(d, s, n)
            for key in pooled:
                if b is None or o is None:
                    out[(s, key)] = "n/a"
                    continue
                bs, os_ = per_seed(b, key), per_seed(o, key)
                vals = [os_[k] - bs[k] for k in sorted(set(bs) & set(os_), key=int)]
                out[(s, key)] = fmt_diff(vals)
                pooled[key] += vals
        cols = [out.get((s, "incidents_resolved"), "n/a") for s in sets]
        pv = fmt_diff(pooled["incidents_resolved"]) if len(sets) > 1 else ""
        lv = [out.get((s, "victims_lost"), "n/a") for s in sets]
        pl = fmt_diff(pooled["victims_lost"]) if len(sets) > 1 else ""
        print(f"{n:40s}| " + " | ".join(cols) + (f" | {pv}" if pv else "") + "  || victims lost: " + " | ".join(lv) + (f" | {pl}" if pl else ""))


def cmd_stats(d, names):
    keys = ["preempt_clear_road", "preempt_reinforce", "succeeded_clear", "failed_clear", "succeeded_reinforce", "failed_reinforce",
            "go_home_planner", "go_home_floor", "approvals_requested", "action_changes", "reversals10"]
    print(f"{'condition':26s}" + "".join(f"{k[:13]:>14s}" for k in keys))
    for n in names:
        for s in "DH":
            j = load(d, s, n)
            if j is None or "planner" not in j:
                continue
            p = j["planner"]
            print(f"{s + ' ' + n:26s}" + "".join(f"{p.get(k, 0.0):>14.1f}" for k in keys))


if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    d, cmd = sys.argv[1], sys.argv[2]
    args = sys.argv[3:]
    sets = "DH"
    if "--sets" in args:
        i = args.index("--sets")
        sets = args[i + 1].replace(",", "")
        args = args[:i] + args[i + 2:]
    if cmd == "abs":
        cmd_abs(d, args)
    elif cmd == "diff":
        cmd_diff(d, args[0], args[1:], sets)
    elif cmd == "stats":
        cmd_stats(d, args)
    else:
        sys.exit(__doc__)

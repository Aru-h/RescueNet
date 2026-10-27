#!/usr/bin/env python3
"""Step 3.6 fault-ending breakdown per class from fault_exposure.py --json runs (<dir>/D_<name>.json, H_<name>.json).

  persist_report.py <dir> <name> [<name> ...]

Per condition, mean per seed over the development and held-out seeds pooled (40 seeds): onsets, how faults ended
(base repair = the team rested at a safe zone, field repair, natural recovery tail), faults still active at the end of
the run, escalations, share of team-ticks with the fault active, mean length by end cause, and field-repair and
preventive-action counts.
"""
import json, os, sys

NAMES = {0: "mobility", 2: "resource"}
TEAM_TICKS = 10 * 3000


def load(d, name):
    out = []
    for s in "DH":
        p = os.path.join(d, f"{s}_{name}.json")
        if os.path.exists(p):
            out += list(json.load(open(p))["per_seed"].values())
    return out


def mean(rows, key, sub="persist"):
    v = [r[sub].get(key, 0.0) for r in rows if r.get(sub)]
    return sum(v) / len(v) if v else float("nan")


def main(d, names):
    for name in names:
        rows = load(d, name)
        if not rows:
            print(f"{name}: no data")
            continue
        print(f"== {name}: {len(rows)} seeds (mean per seed)")
        print("class      onsets  base-repair  field-repair  natural  still-active  escalations  active-share  len(base)  len(natural)")
        for c, cname in NAMES.items():
            on = mean(rows, f"fault{c}_onsets")
            base = mean(rows, f"fault{c}_cleared_by_cause")
            nat = mean(rows, f"fault{c}_cleared_by_backstop")
            kind = 0 if c == 0 else 1
            fld = mean(rows, f"field{kind}_ok")
            esc = mean(rows, f"persist{c}_escalations")
            exp = mean(rows, f"fault{c}_exposure_ticks")
            still = on - base - nat - fld
            lb_ticks = mean(rows, f"faultend{c}_cause_ticks")
            ep = mean(rows, f"fault{c}_episode_ticks")
            lb = lb_ticks / base if base else float("nan")
            ln = (ep - lb_ticks - mean(rows, f"persist{c}_field_ticks_total")) / nat if nat else float("nan")
            print(f"{cname:9s} {on:7.1f} {base:11.1f} {fld:13.1f} {nat:8.1f} {still:13.1f} {esc:12.1f} {100*exp/TEAM_TICKS:11.1f}% {lb:10.1f} {ln:13.1f}")
        print("field activity (mean per seed): " + ", ".join(
            f"{lbl}: started {mean(rows, f'field{k}_started'):.1f} ok {mean(rows, f'field{k}_ok'):.1f} failed {mean(rows, f'field{k}_failed'):.1f} cancelled {mean(rows, f'field{k}_cancelled'):.1f}"
            for k, lbl in [(0, "mobility repair"), (1, "resource repair"), (2, "maintenance")]))
        if any("holdcancel0_stamina" in r.get("persist", {}) for r in rows):
            hc = lambda k, key: mean(rows, f"holdcancel{k}_{key}")
            for k, lbl in [(0, "mobility repair"), (1, "resource repair")]:
                print(f"hold-repair, {lbl}: cancelled by stamina trigger {hc(k, 'stamina'):.2f}, other-class trigger {hc(k, 'other_class'):.2f}, "
                      f"planner above floor {hc(k, 'planner'):.2f}, task released/resolved {hc(k, 'released'):.2f}, "
                      f"fault ended another way {hc(k, 'fault_ended'):.2f}, other/unattributed {hc(k, 'other'):.2f}")
            h = lambda key: mean(rows, f"hold_{key}")
            print(f"hold-repair: recommendations capped RETURN {h('capped_return'):.1f} REALLOC {h('capped_realloc'):.1f} ABORT {h('capped_abort'):.1f}; "
                  f"cap lifted by stamina {h('lifted_stamina'):.1f}, other class {h('lifted_other'):.1f}; "
                  f"second attempts refused {h('h2_refused'):.1f}; resets after success {h('h3_resets'):.1f}")
        pl = lambda k: mean(rows, k, "planner")
        print(f"planner: field repair chosen {pl('fr_chosen'):.1f}, prevent considered {pl('prevent_considered'):.0f}, "
              f"PREVENT {pl('prevent_rest'):.2f}, PREVENT_FIELD {pl('prevent_field'):.2f}; held ticks {mean(rows, 'prevent_held_ticks'):.0f}, "
              f"abort releases {mean(rows, 'prevent_abort_releases'):.2f}, realloc releases {mean(rows, 'prevent_realloc_releases'):.2f}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])

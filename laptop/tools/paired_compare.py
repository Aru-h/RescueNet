#!/usr/bin/env python3
"""
laptop/tools/paired_compare.py - paired difference of two fault_exposure.py
--json runs over the same seeds (incidents resolved and victims lost), with a
bootstrap 95% CI (10000 resamples, seed 0).

usage: paired_compare.py BASE.json OTHER.json [OTHER2.json ...]
prints OTHER - BASE per run.
"""
import json
import random
import sys


def ci(v, n=10000):
    random.seed(0)
    means = sorted(sum(random.choice(v) for _ in v) / len(v) for _ in range(n))
    return means[int(0.025 * n)], means[int(0.975 * n)]


def main():
    base = json.load(open(sys.argv[1]))["per_seed"]
    print(f"base: {sys.argv[1]}")
    for path in sys.argv[2:]:
        other = json.load(open(path))["per_seed"]
        seeds = sorted(set(base) & set(other), key=int)
        for key, label in (("incidents_resolved", "incidents resolved"), ("victims_lost", "victims lost")):
            d = [other[s][key] - base[s][key] for s in seeds if other[s][key] is not None and base[s][key] is not None]
            if not d:
                continue
            lo, hi = ci(d)
            print(f"{path}: {label}: {sum(d)/len(d):+.2f}  95% CI [{lo:+.2f}, {hi:+.2f}]  (n={len(d)})")


if __name__ == "__main__":
    main()

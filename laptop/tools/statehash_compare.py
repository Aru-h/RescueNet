#!/usr/bin/env python3
"""Compare STATEHASH lines from two runs (PC and/or board console logs).

    statehash_compare.py reference.log candidate.log

Each log may contain anything; only lines holding `STATEHASH tick=N all=...`
are read (a board console prefix such as `tron: ` or an IDE timestamp is
ignored). Ticks are matched by number.

A MISSING line is a failure, not noise: the board prints a STATEHASH every N
ticks and the firmware never drops one on purpose (it waits for room in the log
buffer; if it still cannot, it prints "REQUIRED lines LOST"). So any tick that
one log has inside the other log's tick range, but the other lacks, fails the
comparison. Pass --allow-missing only to compare logs printed with different
intervals (e.g. a PC run against a board run).

Exit 0: every compared tick matches and none is missing. Exit 1: a mismatch
(reported with the sub-hash that diverged: teams / inc / flt / rec, which says
which part of the state split first) or a missing line. Exit 2: nothing to
compare.

core/state_hash.h explains what is hashed and why sub-hashes are kept.
"""
import re
import sys

LINE = re.compile(r"STATEHASH\s+tick=(\d+)\s+all=([0-9a-fA-F]+)\s+teams=([0-9a-fA-F]+)"
                  r"\s+inc=([0-9a-fA-F]+)\s+flt=([0-9a-fA-F]+)\s+rec=([0-9a-fA-F]+)")
FIELDS = ("all", "teams", "inc", "flt", "rec")


def load(path):
    out = {}
    with open(path, errors="replace") as fh:
        for line in fh:
            m = LINE.search(line)
            if m:
                out[int(m.group(1))] = dict(zip(FIELDS, (g.lower() for g in m.groups()[1:])))
    return out


def missing_ticks(have, other):
    """Ticks `have` printed inside `other`'s tick range that `other` lacks."""
    if not other:
        return []
    lo, hi = min(other), max(other)
    return sorted(t for t in have if lo <= t <= hi and t not in other)


def main(argv):
    args = [a for a in argv[1:] if a != "--allow-missing"]
    allow_missing = len(args) != len(argv) - 1
    if len(args) != 2:
        print(__doc__)
        return 2
    ref, cand = load(args[0]), load(args[1])
    common = sorted(set(ref) & set(cand))
    if not common:
        print("no common STATEHASH ticks (reference has %d, candidate has %d)" % (len(ref), len(cand)))
        return 2
    for tick in common:
        bad = [f for f in FIELDS if ref[tick][f] != cand[tick][f]]
        if bad:
            print("MISMATCH at tick %d: %s differ" % (tick, ", ".join(bad)))
            for f in bad:
                print("  %-5s ref=%s cand=%s" % (f, ref[tick][f], cand[tick][f]))
            print("%d of %d common ticks matched before this one" % (common.index(tick), len(common)))
            return 1
    if not allow_missing:
        gap_in_cand = missing_ticks(ref, cand)
        gap_in_ref = missing_ticks(cand, ref)
        if gap_in_cand or gap_in_ref:
            if gap_in_cand:
                print("MISSING in candidate: STATEHASH ticks %s (reference has them)" % gap_in_cand)
            if gap_in_ref:
                print("MISSING in reference: STATEHASH ticks %s (candidate has them)" % gap_in_ref)
            print("a missing STATEHASH line is a failure; see --allow-missing only for different intervals")
            return 1
    print("MATCH: %d common ticks (%d..%d) identical in all five hashes"
          % (len(common), common[0], common[-1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

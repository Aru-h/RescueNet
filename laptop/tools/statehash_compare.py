#!/usr/bin/env python3
"""Compare STATEHASH lines from two runs (PC and/or board console logs).

    statehash_compare.py reference.log candidate.log

Each log may contain anything; only lines holding `STATEHASH tick=N all=...`
are read (a board console prefix such as `tron: ` or an IDE timestamp is
ignored). Ticks are matched by number, so a candidate that printed a
different subset is compared on the ticks both have.

Exit 0: every common tick matches. Exit 1: first mismatch is reported with
the sub-hash that diverged (teams / inc / flt / rec), which says which part
of the state split first. Exit 2: nothing to compare.

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


def main(argv):
    if len(argv) != 3:
        print(__doc__)
        return 2
    ref, cand = load(argv[1]), load(argv[2])
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
    print("MATCH: %d common ticks (%d..%d) identical in all five hashes"
          % (len(common), common[0], common[-1]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

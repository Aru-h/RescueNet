#!/usr/bin/env python3
"""Self-test of statehash_compare.py on board-style logs: console prefix,
CRLF line ends, binary frame bytes interleaved, a PC-style bare line, a
reordered/partial candidate, and a real mismatch."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "statehash_compare.py")


def line(tick, a="1", t="2", i="3", f="4", r="5", prefix="[Board Log] tron: "):
    return "%sSTATEHASH tick=%d all=%016x teams=%016x inc=%016x flt=%016x rec=%016x" % (
        prefix, tick, int(a, 16), int(t, 16), int(i, 16), int(f, 16), int(r, 16))


def run(ref_lines, cand_lines, junk=b""):
    with tempfile.TemporaryDirectory() as d:
        ref = os.path.join(d, "ref.log")
        cand = os.path.join(d, "cand.log")
        with open(ref, "wb") as fh:
            fh.write(("\n".join(ref_lines) + "\n").encode())
        with open(cand, "wb") as fh:
            fh.write(b"tron: CONFIG x\r\n" + junk)
            fh.write(("\r\n".join(cand_lines) + "\r\n").encode())
        p = subprocess.run([sys.executable, TOOL, ref, cand], capture_output=True, text=True)
        return p.returncode, p.stdout


def main():
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    ref = [line(50), line(100, a="aa"), line(150, a="bb", i="9")]

    rc, out = run(ref, [line(50), line(100, a="aa"), line(150, a="bb", i="9")], junk=b"\xa5\x5a\x00\x01\xb6\x6b")
    check(rc == 0 and "MATCH: 3" in out, "identical logs (CRLF, binary junk) must match: %r" % out)

    rc, out = run(ref, [line(50, prefix=""), line(150, a="bb", i="9", prefix="")])
    check(rc == 0 and "MATCH: 2" in out, "bare PC-style lines and a subset of ticks must match: %r" % out)

    rc, out = run(ref, [line(50), line(100, a="aa", t="77")])
    check(rc == 1 and "tick 100" in out and "teams" in out, "teams divergence must be reported at tick 100: %r" % out)

    rc, out = run(ref, ["no hash lines here"])
    check(rc == 2, "no common ticks must exit 2: %r" % out)

    if not ok:
        return 1
    print("PASS: statehash_compare handles board-style logs, subsets, mismatches and empty input")
    return 0


if __name__ == "__main__":
    sys.exit(main())

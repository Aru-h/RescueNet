#!/usr/bin/env python3
"""Fit and report the Step 3.5 planner's calibration tables.

The planner (core/recovery_planner.c) turns three evidence sources - the AI's class scores, the watchdog's
anomaly confidence and the consensus severity - into P(fault | evidence) through monotone lookup tables
(core/planner_calibration.h, 32 bins on [0, 1]). The simulator emits per-bin histograms with
`--recovery=utility --planner=none --planner-calib --diag` ("[PLANNERCAL] ..." lines on stderr).

  fit     read dump runs (directories holding diag.log), pool them, fit an isotonic (pool-adjacent-violators)
          map per source and class, rewrite core/planner_calibration.h.
            planner_calibration_fit.py fit --ai <runs dir of the AI-on build> --off <runs dir of the AI-off build> [--header core/planner_calibration.h]
          The AI table and the *_AIBUILD watchdog / consensus tables come from the AI-on dump; the *_NOAI tables
          from the AI-off dump (the condition the no-AI planner runs in). Use DEV seeds only.

  report  reliability bins and ECE from dumps taken with the fitted tables (the "cal" and "fused" histograms).
            planner_calibration_fit.py report <runs dir> [<runs dir> ...]

No numpy / sklearn needed.
"""
import glob, os, re, sys

BINS = 32
LINE = re.compile(r"\[PLANNERCAL\] kind=(\w+) src=(\d) class=(\d) bin=(\d+) n=(\d+) pos=(\d+)(?: sum=([\d.]+))?")
SRC_NAMES = ["AI", "watchdog", "consensus", "fused"]
CLS_NAMES = ["mobility", "communication", "resource"]


def load(dirs):
    """-> {(kind, src, cls): [ [n, pos, sum] * BINS ]}, number of runs"""
    out, runs = {}, 0
    for d in dirs:
        for log in glob.glob(os.path.join(d, "*", "diag.log")) or glob.glob(os.path.join(d, "diag.log")):
            runs += 1
            for m in LINE.finditer(open(log).read()):
                kind, src, cls, b, n, pos, s = m.groups()
                row = out.setdefault((kind, int(src), int(cls)), [[0, 0, 0.0] for _ in range(BINS)])
                row[int(b)][0] += int(n)
                row[int(b)][1] += int(pos)
                row[int(b)][2] += float(s) if s else 0.0
    return out, runs


def pav(bins):
    """Weighted isotonic regression (non-decreasing) over bins of [n, pos]; returns BINS values."""
    blocks = []  # [weight, positives, first_bin, last_bin]
    for b, (n, pos, _) in enumerate(bins):
        if n == 0:
            continue
        blocks.append([n, pos, b, b])
        while len(blocks) > 1 and blocks[-2][1] / blocks[-2][0] > blocks[-1][1] / blocks[-1][0]:
            w, p, lo, hi = blocks.pop()
            blocks[-1][0] += w
            blocks[-1][1] += p
            blocks[-1][3] = hi
    vals = [None] * BINS
    for w, p, lo, hi in blocks:
        for b in range(lo, hi + 1):
            if bins[b][0] > 0:
                vals[b] = p / w
    # empty bins: carry the nearest populated value (below, else above) to keep the map monotone
    last = None
    for b in range(BINS):
        if vals[b] is not None:
            last = vals[b]
        elif last is not None:
            vals[b] = last
    nxt = None
    for b in range(BINS - 1, -1, -1):
        if vals[b] is not None:
            nxt = vals[b]
        elif nxt is not None:
            vals[b] = nxt
    return [0.0 if v is None else v for v in vals]


def fmt_rows(rows):
    return "\n".join("    { " + ", ".join(f"{v:.4f}f" for v in r) + " }," for r in rows)


def cmd_fit(args):
    ai_dir = args[args.index("--ai") + 1]
    off_dir = args[args.index("--off") + 1]
    header = args[args.index("--header") + 1] if "--header" in args else "core/planner_calibration.h"
    ai, n_ai = load([ai_dir])
    off, n_off = load([off_dir])
    if not n_ai or not n_off:
        sys.exit("no dump runs found")

    def table(data, src, classes):
        rows = []
        for c in classes:
            bins = data.get(("raw", src, c), [[0, 0, 0.0] for _ in range(BINS)])
            rows.append(pav(bins))
        return rows
    ai_t = table(ai, 0, range(3))
    tables = (("PLANNER_CAL_AI[3]", ai_t),
              ("PLANNER_CAL_WD_AIBUILD[3]", table(ai, 1, range(3))), ("PLANNER_CAL_WD_NOAI[3]", table(off, 1, range(3))),
              ("PLANNER_CAL_CONS_AIBUILD[2]", table(ai, 2, range(2))), ("PLANNER_CAL_CONS_NOAI[2]", table(off, 2, range(2))))
    text = open(header).read()
    for tag, rows in tables:
        m = re.search(r"(static const float " + re.escape(tag) + r"\[PLANNER_CAL_BINS\] = \{\n)(.*?)(\n\};)", text, re.S)
        if not m:
            sys.exit("table not found: " + tag)
        text = text[:m.start(2)] + fmt_rows(rows) + text[m.end(2):]
    open(header, "w").write(text)
    print(f"fitted from {n_ai} AI-on and {n_off} AI-off runs; wrote {header}")
    for name, data, src, cls in (("AI", ai, 0, range(3)), ("watchdog(AI)", ai, 1, range(3)), ("watchdog(no AI)", off, 1, range(3)),
                                 ("consensus(AI)", ai, 2, range(2)), ("consensus(no AI)", off, 2, range(2))):
        for c in cls:
            bins = data.get(("raw", src, c))
            if bins:
                n = sum(b[0] for b in bins)
                pos = sum(b[1] for b in bins)
                print(f"  {name:9s} {CLS_NAMES[c]:13s} samples {n:9d}  base rate {pos / max(n, 1):.4f}")


def cmd_report(dirs):
    data, runs = load(dirs)
    print(f"reliability from {runs} runs; ECE = sum_b (n_b / N) * |mean predicted P - observed frequency|")
    print(f"{'source':10s}{'class':14s}{'samples':>10s}{'base rate':>10s}{'ECE':>8s}{'Brier':>8s}")
    detail = []
    for (kind, src, cls), bins in sorted(data.items()):
        if kind not in ("cal", "fused"):
            continue
        n = sum(b[0] for b in bins)
        if n == 0:
            continue
        pos = sum(b[1] for b in bins)
        ece = sum(b[0] / n * abs(b[2] / b[0] - b[1] / b[0]) for b in bins if b[0])
        # Brier from bin means (a lower bound: within-bin spread is not kept)
        brier = sum((b[1] * (1 - b[2] / b[0]) ** 2 + (b[0] - b[1]) * (b[2] / b[0]) ** 2) for b in bins if b[0]) / n
        print(f"{SRC_NAMES[src]:10s}{CLS_NAMES[cls]:14s}{n:>10d}{pos / n:>10.4f}{ece:>8.4f}{brier:>8.4f}")
        detail.append((SRC_NAMES[src], CLS_NAMES[cls], bins))
    print("\nreliability bins (non-empty, predicted P bin -> n, mean P, observed freq):")
    for src, cls, bins in detail:
        if src != "fused":
            continue
        print(f"  fused / {cls}")
        for b, (n, pos, s) in enumerate(bins):
            if n:
                print(f"    [{b / BINS:.3f}-{(b + 1) / BINS:.3f})  n={n:8d}  mean P {s / n:.4f}  freq {pos / n:.4f}")


if __name__ == "__main__":
    if len(sys.argv) < 2 or sys.argv[1] not in ("fit", "report"):
        sys.exit(__doc__)
    (cmd_fit if sys.argv[1] == "fit" else cmd_report)(sys.argv[2:] if sys.argv[1] == "fit" else sys.argv[2:])

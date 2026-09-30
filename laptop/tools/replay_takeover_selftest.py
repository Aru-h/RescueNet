#!/usr/bin/env python3
"""
Self-test for "take over from replay" (Part 5): `tron --live
--fast-forward=T` must rebuild the exact state of an uninterrupted run
by deterministic re-simulation, so frames T..T+50 are byte-identical to
the uninterrupted run's - also when the recording being resumed
contains applied overrides (replayed via --override-script).

Drives the real ./tron binary; needs no Flask or browser.
Run: python3 replay_takeover_selftest.py (from laptop/tools).
"""

import json
import os
import subprocess
import sys
import tempfile
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(TOOLS_DIR, "..", ".."))
BINARY = os.path.join(ROOT, "tron")
FAILURES = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what)
    if not cond:
        FAILURES.append(what)


def run_frames(seed, ticks, profile, extra=()):
    """All LIVEFRAME lines (raw text, keyed by tick) plus the other stdout lines."""
    proc = subprocess.run(
        [BINARY, str(seed), "1", str(ticks), "--live", "--fault-profile", profile, *extra],
        cwd=ROOT, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=300)
    frames, others = {}, []
    for line in proc.stdout.splitlines():
        if line.startswith("LIVEFRAME "):
            frames[json.loads(line[10:])["tick"]] = line
        else:
            others.append(line)
    return frames, others, proc


def test_plain_fast_forward():
    for seed, profile, t in ((7, "combined", 100), (3, "none", 60), (11, "combined", 160)):
        total = t + 51
        base, _, _ = run_frames(seed, total, profile)
        ff, others, proc = run_frames(seed, total, profile, (f"--fast-forward={t}",))
        want = list(range(t, t + 51))
        check(sorted(ff) == want, f"seed {seed} {profile}: fast-forward emits exactly frames {t}..{t + 50}")
        check(all(ff.get(k) == base[k] for k in want),
              f"seed {seed} {profile}: frames {t}..{t + 50} identical to the uninterrupted run")
        check(any(o.startswith("STATICENV ") for o in others) and any(o.startswith("STATICMAP ") for o in others),
              f"seed {seed} {profile}: static env/map sent once, before frame {t}")


def test_no_flag_unchanged():
    a, oa, _ = run_frames(5, 80, "combined")
    b, ob, _ = run_frames(5, 80, "combined")
    check(a == b and oa == ob, "without --fast-forward two runs are byte-identical (determinism intact)")
    check(not any(o.startswith("FFPROGRESS") for o in oa), "no FFPROGRESS lines without --fast-forward")


def test_bad_args():
    p = subprocess.run([BINARY, "1", "1", "50", "--fast-forward=50", "--live"], cwd=ROOT,
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    check(p.returncode != 0, "--fast-forward not before the last tick is rejected")
    p = subprocess.run([BINARY, "1", "1", "50", "--fast-forward=10"], cwd=ROOT,
                       stdin=subprocess.DEVNULL, capture_output=True, text=True)
    check(p.returncode != 0, "--fast-forward without --live is rejected")


def live_run_with_overrides(seed, ticks, profile):
    """Paced live run. From tick 40 it tries an OVERRIDE (a different
    team/incident pair each frame, one at a time) until one is applied,
    then sends an OVERRIDE_CLEAR for that team 30 ticks later. Returns
    (frames by tick, script lines of the applied ones, lines sent, acks).
    Acks come back one per line, in order - the same pairing app.py's
    recorder relies on."""
    proc = subprocess.Popen(
        [BINARY, str(seed), "1", str(ticks), "--live", "--fault-profile", profile, "--tick-delay-ms=8"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    frames, sent, acks = {}, [], []
    applied_at, cleared, attempt = None, False, 0

    def send(cmd):
        sent.append(cmd)
        proc.stdin.write(cmd + "\n")
        proc.stdin.flush()

    try:
        for line in proc.stdout:
            if line.startswith("LIVEFRAME "):
                f = json.loads(line[10:])
                k = f["tick"]
                frames[k] = line.rstrip("\n")
                if len(sent) != len(acks):
                    continue  # one line in flight at a time
                if applied_at is None and k >= 40:
                    incs = [i for i in f["incidents"] if i["status"] != 3 and i.get("pool", 0) == 0]
                    if incs:
                        team = f["teams"][attempt % len(f["teams"])]["id"]
                        send(f"OVERRIDE {k} {team} {incs[attempt % len(incs)]['id']}")
                        attempt += 1
                elif applied_at is not None and not cleared and k >= applied_at + 30:
                    send(f"OVERRIDE_CLEAR {k} {sent[-1].split()[2]}")
                    cleared = True
            elif line.startswith("OVERRIDEACK "):
                acks.append(json.loads(line[len("OVERRIDEACK "):]))
                if applied_at is None and acks[-1]["result"] == "applied":
                    applied_at = acks[-1]["effective_tick"]
    finally:
        proc.wait(timeout=120)
    script = []
    for cmd, ack in zip(sent, acks):
        if ack["result"] == "applied":
            parts = cmd.split()
            parts[1] = str(ack["effective_tick"])
            script.append(" ".join(parts))
    return frames, script, sent, acks


def test_with_overrides():
    seed, profile, total, t = 7, "combined", 220, 150
    frames, script, sent, acks = live_run_with_overrides(seed, total, profile)
    check(len(sent) == len(acks) and len(sent) >= 1, f"every override line got an ack (sent={sent}, acks={len(acks)})")
    check(any(l.startswith("OVERRIDE ") for l in script), f"an OVERRIDE (not just a clear) was applied (script={script})")
    if not any(l.startswith("OVERRIDE ") for l in script):
        return
    with tempfile.NamedTemporaryFile("w", suffix=".ovr", delete=False) as fh:
        fh.write("\n".join(script) + "\n")
        path = fh.name
    try:
        ff, _, _ = run_frames(seed, total, profile, (f"--fast-forward={t}", f"--override-script={path}"))
        want = list(range(t, min(total, t + 51)))
        check(all(ff.get(k) == frames.get(k) for k in want),
              f"with recorded overrides: frames {want[0]}..{want[-1]} identical to the original overridden run")
        plain, _, _ = run_frames(seed, total, profile, (f"--fast-forward={t}",))
        check(any(plain.get(k) != frames.get(k) for k in want),
              "without the override script the resumed run would diverge (script is really needed)")
    finally:
        os.unlink(path)


def main():
    if not os.path.exists(BINARY):
        print(f"FAIL: {BINARY} not built (run make)")
        return 1
    test_plain_fast_forward()
    test_no_flag_unchanged()
    test_bad_args()
    test_with_overrides()
    if FAILURES:
        print(f"\n{len(FAILURES)} FAILED")
        return 1
    print("\nreplay_takeover_selftest: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

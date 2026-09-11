#!/usr/bin/env python3
"""
Bounce detector for the tron RESTING<->EN_ROUTE oscillation bug.

Runs ./tron with --trace, parses the [DIAG] status-transition lines, and
flags any team whose RESTING(3) exit is followed by a RESTING(3) re-entry
within `--window` ticks - the signature of the bug (a team leaving rest,
getting immediately turned around, and coming right back without doing
any real work in between).

Usage:
    python3 bounce_check.py --seeds 1 50 --ticks 5000
    python3 bounce_check.py --seeds 1 50 --ticks 5000 --fault-profile combined
    python3 bounce_check.py --seeds 1 20 --ticks 4000 --window 8 --binary ./tron

Notes:
    - --seeds START END is a half-open range like Python's range(): END is
      exclusive, so `--seeds 1 51` runs seeds 1..50.
    - Positional args to ./tron are (seed, seed_count, ticks_per_seed) - this
      script always passes seed_count=1 and iterates seeds itself, so each
      run is independent and easy to attribute.
    - A nonzero bounce count under a fault profile isn't automatically a bug:
      a team legitimately can, and arguably should, keep retreating to a
      safe zone for as long as a real fault is genuinely unresolved. Worth
      eyeballing a few of the reported (team, tick) windows with --trace
      by hand to see whether real work is happening in between, or whether
      it's a tight back-and-forth with nothing accomplished.
    - Zero bounces expected with no fault profile at all, at any seed/tick
      count - that's the original, fully-fixed bug. Any nonzero count there
      would mean something regressed.
"""
import argparse
import re
import subprocess
import sys

DIAG_PATTERN = re.compile(r"\[DIAG\] tick=(\d+) team=(\d+) status (\d)->(\d)")
STATUS_RESTING = 3


def run_one_seed(binary, seed, ticks, fault_profile, timeout):
    cmd = [binary, str(seed), "1", str(ticks), "--trace"]
    if fault_profile:
        cmd += ["--fault-profile", fault_profile]
    result = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return result.stdout + result.stderr


def find_bounces(trace_output, seed, fault_profile, window):
    last_rest_exit_tick = {}  # team_id -> tick it last left RESTING
    bounces = []
    for line in trace_output.splitlines():
        m = DIAG_PATTERN.search(line)
        if not m:
            continue
        tick, team, prev_status, new_status = (
            int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
        )
        if prev_status == STATUS_RESTING and new_status != STATUS_RESTING:
            last_rest_exit_tick[team] = tick
        if new_status == STATUS_RESTING and team in last_rest_exit_tick:
            dt = tick - last_rest_exit_tick[team]
            if dt <= window:
                bounces.append({
                    "seed": seed,
                    "fault_profile": fault_profile,
                    "team": team,
                    "rest_exit_tick": last_rest_exit_tick[team],
                    "rest_reentry_tick": tick,
                    "ticks_away": dt,
                })
            del last_rest_exit_tick[team]
    return bounces


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seeds", nargs=2, type=int, metavar=("START", "END"), required=True,
                         help="Seed range, END exclusive (like Python range()).")
    parser.add_argument("--ticks", type=int, required=True, help="Ticks per seed.")
    parser.add_argument("--fault-profile", default=None, help="Passed through to ./tron --fault-profile. Omit for no faults.")
    parser.add_argument("--window", type=int, default=6, help="Max ticks between RESTING exit and re-entry to count as a bounce (default 6).")
    parser.add_argument("--binary", default="./tron", help="Path to the tron binary (default ./tron).")
    parser.add_argument("--timeout", type=int, default=120, help="Per-seed subprocess timeout in seconds (default 120).")
    parser.add_argument("--max-print", type=int, default=40, help="Max individual bounce events to print (default 40).")
    args = parser.parse_args()

    all_bounces = []
    for seed in range(args.seeds[0], args.seeds[1]):
        try:
            output = run_one_seed(args.binary, seed, args.ticks, args.fault_profile, args.timeout)
        except subprocess.TimeoutExpired:
            print(f"seed={seed}: TIMED OUT after {args.timeout}s, skipping", file=sys.stderr)
            continue
        bounces = find_bounces(output, seed, args.fault_profile, args.window)
        all_bounces += bounces
        print(f"seed={seed} fault_profile={args.fault_profile} ticks={args.ticks} bounces={len(bounces)}", flush=True)

    print(f"\nTOTAL bounces: {len(all_bounces)} across seeds {args.seeds[0]}..{args.seeds[1]-1}, "
          f"{args.ticks} ticks each, fault_profile={args.fault_profile}, window<={args.window}")
    for b in all_bounces[:args.max_print]:
        print(f"  seed={b['seed']:<4} team={b['team']:<3} "
              f"rest_exit={b['rest_exit_tick']:<8} rest_reentry={b['rest_reentry_tick']:<8} "
              f"ticks_away={b['ticks_away']}")
    if len(all_bounces) > args.max_print:
        print(f"  ... and {len(all_bounces) - args.max_print} more")


if __name__ == "__main__":
    main()
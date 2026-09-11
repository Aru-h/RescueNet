#!/usr/bin/env python3
"""
verify_stuck_loop_bugs.py

Reads tron_snapshot_dataset.csv (produced by ./tron) and checks, directly
against real simulation output, whether two specific claims discussed in
recent debugging are actually happening:

  1. THE STAMINA STUCK-LOOP CLAIM
     "A team hits MIN_STAMINA while STATUS_EN_ROUTE, gets routed toward a
     safe zone, and then stays stuck logging EVENT_TEAM_REST every tick for
     most of the run instead of reaching STATUS_RESTING."
     For every (run_id, team_id) this script finds every maximal run of
     consecutive-tick EVENT_TEAM_REST rows ("stuck streaks") and reports:
       - how long the streak is, and what % of the whole run that is
       - whether assigned_incident (the safe-zone task id) changes DURING
         the streak (churn => the reclaim-race theory) or stays constant
       - whether the team's (loc_x, loc_y) actually changes during the
         streak (moving-but-never-arriving vs. genuinely frozen in place)
       - the FAULT_CLASS_MOBILITY state at the start/end of the streak

  2. THE EVENT_TEAM_AVAILABLE DUPLICATE-LOG CLAIM
     "abandon_task_for_recovery()'s fallback path lost its
     `if(team->status != STATUS_AVAILABLE)` guard, so it can log
     EVENT_TEAM_AVAILABLE twice in a row for a team that was already
     AVAILABLE, inflating the row count with no real state change."
     For every (run_id, team_id) this script flags any two consecutive
     EVENT_TEAM_AVAILABLE rows (no other event row for that team in
     between) as a duplicate-log hit.

Usage:
    python3 verify_stuck_loop_bugs.py [csv_path] [--ticks N] [--stuck-pct P]

    csv_path        Path to tron_snapshot_dataset.csv. If omitted, the
                     script looks in the current directory and a couple of
                     likely parent directories.
    --ticks N        Ticks-per-seed the run was launched with (matches
                     ./tron's 3rd positional arg; default 10000, same as
                     main.c's own default). Used to compute "% of run
                     stuck" - if you ran with a non-default tick count,
                     pass it here or the percentages will be wrong.
    --stuck-pct P    Threshold (0-100) for flagging a streak as "the bug
                     reproduced" in the summary verdict. Default 50.

No third-party dependencies - stdlib only, so it drops straight into
laptop/tools/ and runs with whatever python3 is already on the machine.
"""

import argparse
import csv
import os
import sys
from collections import defaultdict

EVENT_NAMES = {
    0: "EVENT_NONE",
    1: "EVENT_NEW_INCIDENT",
    2: "EVENT_TEAM_ASSIGNED",
    3: "EVENT_TEAM_ON_SCENE",
    4: "EVENT_STAGE_COMPLETE",
    5: "EVENT_TEAM_REST",
    6: "EVENT_TEAM_AVAILABLE",
    7: "EVENT_INCIDENT_RESOLVED",
    8: "EVENT_FAULT_INJECTED",
    9: "EVENT_FAULT_RECOVERED",
    10: "EVENT_WATCHDOG_ANOMALY_CHANGED",
    11: "EVENT_RECOVERY_ACTION_CHANGED",
    12: "EVENT_TICK",  # only present if your local EVENT_TICK/EVENT_TICK_SAMPLE
                        # change is actually the binary you built
}

STATUS_NAMES = {0: "AVAILABLE", 1: "EN_ROUTE", 2: "ON_SCENE", 3: "RESTING"}

EVENT_TEAM_REST = 5
EVENT_TEAM_AVAILABLE = 6
STATUS_AVAILABLE = 0


def find_default_csv():
    candidates = [
        "tron_snapshot_dataset.csv",
        os.path.join("..", "tron_snapshot_dataset.csv"),
        os.path.join("..", "..", "tron_snapshot_dataset.csv"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return None


def load_rows(csv_path):
    """Load and lightly type-convert every row. Returns a list of dicts."""
    rows = []
    with open(csv_path, newline="") as f:
        reader = csv.DictReader(f)
        required = {"run_id", "cycle", "team_id", "status", "stamina",
                    "event_type", "assigned_incident", "loc_x", "loc_y",
                    "fault_active_mobility", "fault_severity_mobility"}
        missing = required - set(reader.fieldnames or [])
        if missing:
            sys.exit(
                "CSV is missing expected columns: {}\n"
                "Header found: {}\n"
                "This script was written against a specific row schema - "
                "if the schema has changed, update the column list at the "
                "top of load_rows().".format(sorted(missing), reader.fieldnames)
            )
        for r in reader:
            try:
                rows.append({
                    "run_id": int(r["run_id"]),
                    "cycle": int(r["cycle"]),
                    "team_id": int(r["team_id"]),
                    "status": int(r["status"]),
                    "stamina": float(r["stamina"]),
                    "event_type": int(r["event_type"]),
                    "assigned_incident": int(r["assigned_incident"]),
                    "loc_x": int(r["loc_x"]),
                    "loc_y": int(r["loc_y"]),
                    "fault_active_mobility": int(r["fault_active_mobility"]),
                    "fault_severity_mobility": int(r["fault_severity_mobility"]),
                    "watchdog_anomaly_detected": int(r.get("watchdog_anomaly_detected", 0)),
                })
            except (ValueError, KeyError):
                # Skip any malformed/partial row (e.g. a truncated last line
                # from a run that was killed mid-write) rather than crashing.
                continue
    return rows


def check_stamina_stuck_loop(rows, ticks_per_seed, stuck_pct_threshold):
    print("=" * 78)
    print("CHECK 1: EN_ROUTE stamina stuck-loop")
    print("=" * 78)

    rest_rows = defaultdict(list)  # (run_id, team_id) -> sorted list of rows
    for r in rows:
        if r["event_type"] == EVENT_TEAM_REST:
            rest_rows[(r["run_id"], r["team_id"])].append(r)

    if not rest_rows:
        print("No EVENT_TEAM_REST rows found at all - either the run never "
              "hit MIN_STAMINA, or event_type=5 isn't what you expect in "
              "this build. Nothing to check here.\n")
        return []

    flagged = []
    for (run_id, team_id), team_rows in sorted(rest_rows.items()):
        team_rows.sort(key=lambda r: r["cycle"])

        # Break into maximal streaks of consecutive-tick EVENT_TEAM_REST rows.
        streaks = []
        current = [team_rows[0]]
        for prev, cur in zip(team_rows, team_rows[1:]):
            if cur["cycle"] == prev["cycle"] + 1:
                current.append(cur)
            else:
                streaks.append(current)
                current = [cur]
        streaks.append(current)

        for streak in streaks:
            length = len(streak)
            pct = 100.0 * length / ticks_per_seed
            if pct < stuck_pct_threshold:
                continue

            incident_ids = {s["assigned_incident"] for s in streak}
            locs = {(s["loc_x"], s["loc_y"]) for s in streak}
            start, end = streak[0], streak[-1]

            flagged.append({
                "run_id": run_id,
                "team_id": team_id,
                "start_cycle": start["cycle"],
                "end_cycle": end["cycle"],
                "length": length,
                "pct_of_run": pct,
                "distinct_incident_ids": len(incident_ids),
                "distinct_locations": len(locs),
                "mobility_fault_active_start": bool(start["fault_active_mobility"]),
                "mobility_fault_severity_start": start["fault_severity_mobility"],
                "mobility_fault_active_end": bool(end["fault_active_mobility"]),
            })

    if not flagged:
        print(f"No stuck streaks >= {stuck_pct_threshold}% of a "
              f"{ticks_per_seed}-tick run found. The stamina stuck-loop "
              f"does NOT reproduce at this threshold in this dataset.\n")
        return flagged

    print(f"Found {len(flagged)} streak(s) >= {stuck_pct_threshold}% of the run stuck "
          f"logging EVENT_TEAM_REST:\n")
    print(f"{'run_id':>10} {'team':>4} {'cycles':>17} {'len':>6} {'%run':>6} "
          f"{'#incident_ids':>13} {'#locations':>10} {'mobility_fault':>14}")
    for f in flagged:
        cycles = f"{f['start_cycle']}-{f['end_cycle']}"
        fault_str = "start:%s->end:%s" % (
            "Y" if f["mobility_fault_active_start"] else "N",
            "Y" if f["mobility_fault_active_end"] else "N",
        )
        print(f"{f['run_id']:>10} {f['team_id']:>4} {cycles:>17} {f['length']:>6} "
              f"{f['pct_of_run']:>5.1f}% {f['distinct_incident_ids']:>13} "
              f"{f['distinct_locations']:>10} {fault_str:>14}")

    churned = [f for f in flagged if f["distinct_incident_ids"] > 1]
    stationary = [f for f in flagged if f["distinct_locations"] == 1]
    moved_but_stuck = [f for f in flagged
                        if f["distinct_locations"] > 1 and f["distinct_incident_ids"] == 1]

    print()
    if churned:
        print(f"-> {len(churned)} streak(s) show the team's assigned safe-zone "
              f"task (assigned_incident) CHANGING mid-streak.")
        print("   This is the signature of the reclaim-race theory: the team's "
              "in-flight RETURN_TO_SAFE_ZONE task is getting silently replaced, "
              "so it never gets to finish a single trip.")
    if stationary:
        print(f"-> {len(stationary)} streak(s) never change (loc_x, loc_y) at all "
              f"during the whole stuck period.")
        print("   The team isn't moving AT ALL while stuck - move_team() isn't "
              "advancing it, which points at something upstream of movement "
              "(no path found, assignment pool mismatch, etc.) rather than "
              "just slow travel.")
    if moved_but_stuck:
        print(f"-> {len(moved_but_stuck)} streak(s) keep the SAME assigned_incident "
              f"the whole time but DO change location.")
        print("   The team is genuinely traveling toward one fixed safe zone the "
              "whole streak and just hasn't arrived by the end of the run - check "
              "mobility fault severity/distance rather than a reassignment bug.")
    print()
    return flagged


def check_duplicate_available_events(rows):
    print("=" * 78)
    print("CHECK 2: EVENT_TEAM_AVAILABLE duplicate-log (missing guard)")
    print("=" * 78)

    by_team = defaultdict(list)
    for r in rows:
        by_team[(r["run_id"], r["team_id"])].append(r)

    hits = []
    for (run_id, team_id), team_rows in sorted(by_team.items()):
        team_rows.sort(key=lambda r: (r["cycle"], r["event_type"]))
        prev = None
        for r in team_rows:
            if (prev is not None
                    and prev["event_type"] == EVENT_TEAM_AVAILABLE
                    and r["event_type"] == EVENT_TEAM_AVAILABLE
                    and prev["status"] == STATUS_AVAILABLE
                    and r["status"] == STATUS_AVAILABLE):
                hits.append({
                    "run_id": run_id,
                    "team_id": team_id,
                    "first_cycle": prev["cycle"],
                    "second_cycle": r["cycle"],
                })
            prev = r

    if not hits:
        print("No back-to-back EVENT_TEAM_AVAILABLE rows found for any team "
              "with no other event in between. The missing-guard bug does "
              "NOT reproduce in this dataset.\n")
        return hits

    print(f"Found {len(hits)} duplicate-log hit(s) - two EVENT_TEAM_AVAILABLE "
          f"rows in a row for the same team with no real status change in "
          f"between:\n")
    print(f"{'run_id':>10} {'team':>4} {'first_cycle':>12} {'second_cycle':>13}")
    for h in hits[:40]:
        print(f"{h['run_id']:>10} {h['team_id']:>4} {h['first_cycle']:>12} "
              f"{h['second_cycle']:>13}")
    if len(hits) > 40:
        print(f"... and {len(hits) - 40} more.")
    print()
    return hits


def print_overview(rows, ticks_per_seed):
    run_ids = sorted({r["run_id"] for r in rows})
    team_ids = sorted({r["team_id"] for r in rows})
    counts = defaultdict(int)
    for r in rows:
        counts[r["event_type"]] += 1

    print("=" * 78)
    print("OVERVIEW")
    print("=" * 78)
    print(f"Total rows:         {len(rows):,}")
    print(f"Distinct run_ids:   {len(run_ids)} (seeds)")
    print(f"Distinct team_ids:  {len(team_ids)}")
    print(f"Assumed ticks/seed: {ticks_per_seed} "
          f"(pass --ticks if you didn't run with the default)")
    print()
    print(f"{'event_type':>12}  {'name':<30}  {'count':>12}")
    for et in sorted(counts):
        name = EVENT_NAMES.get(et, f"UNKNOWN({et})")
        print(f"{et:>12}  {name:<30}  {counts[et]:>12,}")

    if counts.get(12):
        expected_tick_rows = len(run_ids) * len(team_ids) * ticks_per_seed
        actual = counts[12]
        print()
        print(f"EVENT_TICK present: {actual:,} rows vs. an expected "
              f"{expected_tick_rows:,} (seeds x teams x ticks_per_seed) "
              f"if it's logged unconditionally every tick.")
        if actual != expected_tick_rows:
            print("  -> MISMATCH. Either ticks_per_seed passed to this script "
                  "is wrong, or EVENT_TICK isn't actually unconditional in "
                  "this build.")

    anomaly_rate = 100.0 * sum(r["watchdog_anomaly_detected"] for r in rows) / len(rows)
    print(f"\nOverall watchdog_anomaly_detected rate across ALL rows: "
          f"{anomaly_rate:.3f}%")
    print("(Note: this is over every logged row, not per-tick, so it's only "
          "comparable to a prior baseline if that baseline used the same "
          "logging density - e.g. don't compare directly against a baseline "
          "computed only over EVENT_TICK rows.)\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv_path", nargs="?", default=None,
                         help="Path to tron_snapshot_dataset.csv")
    parser.add_argument("--ticks", type=int, default=10000,
                         help="ticks_per_seed the run used (default: 10000, "
                              "matching ./tron's own default)")
    parser.add_argument("--stuck-pct", type=float, default=50.0,
                         help="Flag a streak as stuck if it covers at least "
                              "this %% of the run (default: 50)")
    args = parser.parse_args()

    csv_path = args.csv_path or find_default_csv()
    if csv_path is None or not os.path.isfile(csv_path):
        sys.exit(
            "Could not find tron_snapshot_dataset.csv. Run ./tron first "
            "(from the directory the CSV should land in), or pass the path "
            "explicitly: python3 verify_stuck_loop_bugs.py /path/to/tron_snapshot_dataset.csv"
        )

    print(f"Reading {csv_path} ...\n")
    rows = load_rows(csv_path)
    if not rows:
        sys.exit("CSV loaded but contained zero usable rows.")

    print_overview(rows, args.ticks)
    stuck = check_stamina_stuck_loop(rows, args.ticks, args.stuck_pct)
    dup = check_duplicate_available_events(rows)

    print("=" * 78)
    print("VERDICT")
    print("=" * 78)
    print(f"Stamina stuck-loop bug reproduced:      "
          f"{'YES - ' + str(len(stuck)) + ' streak(s)' if stuck else 'NO'}")
    print(f"EVENT_TEAM_AVAILABLE duplicate-log bug:  "
          f"{'YES - ' + str(len(dup)) + ' hit(s)' if dup else 'NO'}")


if __name__ == "__main__":
    main()

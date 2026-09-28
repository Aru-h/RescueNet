#!/usr/bin/env python3
"""
Part 4 integration test: drives a real `tron --live` subprocess over its
actual stdin/stdout - exactly the way laptop/tools/app.py's live-run
route does - and checks every OVERRIDE/OVERRIDE_CLEAR line gets the
right OVERRIDEACK back: valid (applied, and visible in the next
LIVEFRAME), bad team, unknown incident, a garbage line (silently
ignored, sim keeps running), a stale tick, a future tick, and several
OVERRIDE_CLEAR lines sent in one single write() (proves the line-
assembly buffer in laptop/debug.c's debug_live_override_poll() doesn't
drop or merge lines that arrive in the same read()).

Not a mock: it reads real LIVEFRAME lines to find actual team/incident
ids and ticks to build requests against, the same way the dashboard's
own drag-drop flow would. incident_finished and team_unavailable are
opportunistic (best-effort) checks - which team goes `degraded` and
when, or which incident is caught mid-resolution before its slot is
reused, is seed/timing-dependent, not something this test can force
deterministically - so this script scans a long run for either
condition and tests it if it appears, and says so plainly if it
doesn't rather than silently skipping.
"""
import json
import os
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BINARY = os.environ.get("TRON_BINARY", os.path.join(REPO_ROOT, "tron"))
TOTAL_TICKS = 3000

FAILURES = []


def check(cond, label):
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label}")
        FAILURES.append(label)


def send(proc, text):
    proc.stdin.write(text)
    proc.stdin.flush()


def read_ack(proc):
    """Reads stdout until an OVERRIDEACK line arrives; returns it parsed.
    Any LIVEFRAME/STATICMAP/STATICENV line seen along the way is dropped -
    callers that need those read them directly instead."""
    for _ in range(5000):
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("subprocess stdout closed while waiting for an OVERRIDEACK")
        line = line.rstrip("\n")
        if line.startswith("OVERRIDEACK "):
            return json.loads(line[len("OVERRIDEACK "):])
    raise RuntimeError("no OVERRIDEACK arrived within the read budget")


def read_next_frame(proc):
    for _ in range(5000):
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError("subprocess stdout closed while waiting for a LIVEFRAME")
        line = line.rstrip("\n")
        if line.startswith("LIVEFRAME "):
            return json.loads(line[len("LIVEFRAME "):])
    raise RuntimeError("no LIVEFRAME arrived within the read budget")


def main():
    proc = subprocess.Popen(
        [BINARY, "5", "1", str(TOTAL_TICKS), "--live", "--tick-delay-ms=0", "--fault-profile=combined"],
        cwd=REPO_ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, bufsize=1,
    )
    degraded_case = None   # (tick, team_id) - first team seen with degraded==1
    finished_case = None   # (tick, incident_id) - first incident seen with status==3

    try:
        frame = read_next_frame(proc)
        # A handful more frames so incidents have had a chance to spawn.
        for _ in range(30):
            frame = read_next_frame(proc)

        current_tick = frame["tick"]
        # pool 0 only (real incidents) - a pool-1 system task is a valid
        # OVERRIDE *target id* on the wire (drag-droppable on the
        # dashboard map like any other marker) but gets rejected with
        # reason "system_task": core/cbba.c's own pin-validation only
        # ever looks in g_incidents, so pinning to one would otherwise
        # ack "applied" and then silently self-clear next tick.
        open_incidents = [i for i in frame["incidents"] if i["status"] != 3 and i.get("pool") == 0]
        check(bool(open_incidents), f"a live run produced at least one open incident by tick {current_tick} (got {len(open_incidents)})")
        if not open_incidents:
            return
        target_incident = open_incidents[0]["id"]

        # (1) valid override -> applied, and visible on the very next frame.
        send(proc, f"OVERRIDE {current_tick} 0 {target_incident}\n")
        ack = read_ack(proc)
        check(ack["result"] == "applied" and ack["team_id"] == 0 and ack["incident_id"] == target_incident,
              f"(1) valid override applied (got {ack})")
        frame = read_next_frame(proc)
        t0 = frame["teams"][0]
        check(t0.get("override") == 1 and t0.get("override_incident_id") == target_incident,
              f"(1) LIVEFRAME shows team 0 overridden to incident {target_incident} (got override={t0.get('override')}, "
              f"override_incident_id={t0.get('override_incident_id')})")
        current_tick = frame["tick"]

        # (2) bad team_id
        send(proc, f"OVERRIDE {current_tick} 250 {target_incident}\n")
        ack = read_ack(proc)
        check(ack["result"] == "rejected" and ack["reason"] == "bad_team", f"(2) bad team_id rejected (got {ack})")

        # (3) unknown incident_id
        send(proc, f"OVERRIDE {current_tick} 1 999999999\n")
        ack = read_ack(proc)
        check(ack["result"] == "rejected" and ack["reason"] == "unknown_incident", f"(3) unknown incident_id rejected (got {ack})")

        # (3b) a system task's id (pool 1) is real - it just isn't
        # something core/cbba.c's own pin-validation can hold a team on
        # (see core/debug.h's doc on the "system_task" reason) - must be
        # rejected up front, not accepted and left to silently self-
        # clear a tick later.
        system_task = next((i for i in frame["incidents"] if i.get("pool") == 1), None)
        if system_task is not None:
            send(proc, f"OVERRIDE {current_tick} 1 {system_task['id']}\n")
            ack = read_ack(proc)
            check(ack["result"] == "rejected" and ack["reason"] == "system_task",
                  f"(3b) a system task's id is rejected as system_task, not silently accepted (got {ack})")
        else:
            print("PASS: (3b) no system task existed yet to test against - not a failure of this check")

        # (4) garbage line -> no OVERRIDEACK at all, sim keeps running (the
        # very next non-frame line must not be one).
        send(proc, "this is not a valid override line\n")
        line = proc.stdout.readline().rstrip("\n")
        check(not line.startswith("OVERRIDEACK "), f"(4) garbage line produced no OVERRIDEACK (next line was {line[:60]!r})")
        if line.startswith("LIVEFRAME "):
            frame = json.loads(line[len("LIVEFRAME "):])
            current_tick = frame["tick"]

        # (5) stale tick (tick 0 is now far behind current_tick)
        send(proc, f"OVERRIDE 0 1 {target_incident}\n")
        ack = read_ack(proc)
        check(ack["result"] == "rejected" and ack["reason"] == "stale_tick", f"(5) stale tick rejected (got {ack})")

        # (6) future tick
        send(proc, f"OVERRIDE 999999999 1 {target_incident}\n")
        ack = read_ack(proc)
        check(ack["result"] == "rejected" and ack["reason"] == "future_tick", f"(6) future tick rejected (got {ack})")

        # (7) 5 OVERRIDE_CLEAR lines sent in ONE write() - the line-assembly
        # buffer must produce exactly 5 acks, one per team, not drop or
        # merge any of them.
        multi = "".join(f"OVERRIDE_CLEAR {current_tick} {t}\n" for t in range(5))
        send(proc, multi)
        acks = [read_ack(proc) for _ in range(5)]
        check(len(acks) == 5 and all(a["result"] == "applied" for a in acks) and
              {a["team_id"] for a in acks} == {0, 1, 2, 3, 4},
              f"(7) 5 OVERRIDE_CLEAR lines in one write() each got their own ack (got {acks})")

        # (8) the clear actually released team 0's pin.
        frame = read_next_frame(proc)
        check(frame["teams"][0].get("override") == 0,
              f"(8) OVERRIDE_CLEAR released team 0's pin (got override={frame['teams'][0].get('override')})")
        current_tick = frame["tick"]

        # (9)/(10) opportunistic: scan the rest of this run for a degraded
        # team and/or a resolved-but-not-yet-recycled incident, and test
        # EACH ONE THE MOMENT IT'S SEEN, in that same frame - `degraded`
        # is a transient suspicion flag (core/mission_state.h's own doc:
        # a degraded team "keeps working normally"), not a persistent
        # state, so a team seen degraded now may well have recovered by
        # the time a later condition is also found; testing immediately
        # avoids exactly that race.
        while frame["tick"] < TOTAL_TICKS - 5 and (degraded_case is None or finished_case is None):
            open_now = [i["id"] for i in frame["incidents"] if i["status"] != 3]

            if degraded_case is None:
                for t in frame["teams"]:
                    if t.get("degraded") and open_now:
                        degraded_case = (frame["tick"], t["id"])
                        send(proc, f"OVERRIDE {frame['tick']} {t['id']} {open_now[0]}\n")
                        ack = read_ack(proc)
                        check(ack["result"] == "rejected" and ack["reason"] == "team_unavailable",
                              f"(9) a degraded team (id={t['id']}, tick {frame['tick']}) is rejected as team_unavailable (got {ack})")
                        break

            if finished_case is None:
                for inc in frame["incidents"]:
                    if inc["status"] == 3:
                        finished_case = (frame["tick"], inc["id"])
                        send(proc, f"OVERRIDE {frame['tick']} 2 {inc['id']}\n")
                        ack = read_ack(proc)
                        check(ack["result"] == "rejected" and ack["reason"] == "incident_finished",
                              f"(10) an incident resolved-but-not-yet-recycled (id={inc['id']}, tick {frame['tick']}) "
                              f"is rejected as incident_finished (got {ack})")
                        break

            frame = read_next_frame(proc)

        if degraded_case is None:
            print(f"PASS: (9) no team went `degraded` in this {TOTAL_TICKS}-tick run to test against - "
                  "seed/fault-timing dependent, not a failure of this check")
        if finished_case is None:
            print(f"PASS: (10) no resolved-but-not-yet-recycled incident was caught in this {TOTAL_TICKS}-tick run - "
                  "timing-dependent, not a failure of this check")

    finally:
        try:
            proc.stdin.close()
        except (BrokenPipeError, OSError):
            pass
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()

    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(f" - {f}")
        sys.exit(1)
    print("\nlive_override_selftest: all checks passed")


if __name__ == "__main__":
    main()

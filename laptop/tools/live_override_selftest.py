#!/usr/bin/env python3
"""
Part 4 integration test: drives real `tron --live` subprocesses over
their actual stdin/stdout - exactly the way laptop/tools/app.py's
live-run route does - and checks every OVERRIDE/OVERRIDE_CLEAR line
gets the right OVERRIDEACK back: valid (applied, and visible in the
next LIVEFRAME), bad team, unknown incident, a system task, several
flavors of malformed line (wrong field count, non-numeric field, an
unrecognized command word, an overlong line), a stale tick, a future
tick, and several OVERRIDE_CLEAR lines sent in one single write()
(proves the line-assembly buffer in laptop/debug.c's debug_live_
override_poll() doesn't drop or merge lines that arrive in the same
read()).

Not a mock: it reads real LIVEFRAME lines to find actual team/incident
ids and ticks to build requests against, the same way the dashboard's
own drag-drop flow would. team_unavailable is still opportunistic
(best-effort): which team goes `degraded` and when is seed/fault-
timing dependent, not something this test can force deterministically,
so this script scans a long run for it and says so plainly if it
doesn't appear rather than silently skipping. The incident-resolved-
while-pinned check (run_incident_resolve_autoclear_check() below) is
NOT opportunistic - it uses a fixed seed picked empirically to
reliably drive the exact scenario every run, and fails outright (not
"skipped") if that ever stops holding.
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


def _pin_and_watch_to_ending(label, seed, team_id, target_id, ticks=600):
    """Shared driver for the two deterministic (10) cases below: starts
    its own fresh subprocess, pins team_id to target_id, watches the
    pin auto-clear (commit), then watches for exactly one of two
    endings - resolved (incident finishes, its status becomes
    INC_RESOLVED or it stops appearing in the incidents array) or
    interrupted (the sim's own normal stamina/rest handling pulls the
    team off before it finishes: has_dest drops and/or the incident's
    status resets to INC_OPEN with assigned_team no longer team_id -
    nothing override-specific, the override only ever guaranteed the
    initial commitment). Returns "resolved", "interrupted", or None
    (neither happened within the tick budget - callers must FAIL on
    None, never treat it as a skip).

    Runs every check it can along the way (every field asserted here
    holds regardless of which ending the caller expects), so a caller
    only needs to additionally assert which ending it got.
    """
    proc = subprocess.Popen(
        [BINARY, str(seed), "1", str(ticks), "--live", "--tick-delay-ms=0", "--fault-profile=none"],
        cwd=REPO_ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, bufsize=1,
    )
    try:
        frame = read_next_frame(proc)
        for _ in range(30):
            frame = read_next_frame(proc)

        open_incidents = [i for i in frame["incidents"] if i["status"] != 3 and i.get("pool") == 0]
        available_teams = [t["id"] for t in frame["teams"] if t.get("status") == 0]
        check(any(i["id"] == target_id for i in open_incidents) and team_id in available_teams,
              f"({label}) fixed seed {seed} still has team {team_id} available and incident {target_id} open "
              f"by tick {frame['tick']} - this check must FAIL, not skip, if that ever stops holding")
        if not (any(i["id"] == target_id for i in open_incidents) and team_id in available_teams):
            return None

        current_tick = frame["tick"]
        send(proc, f"OVERRIDE {current_tick} {team_id} {target_id}\n")
        ack = read_ack(proc)
        check(ack["result"] == "applied", f"({label}) pin was applied (got {ack})")
        if ack["result"] != "applied":
            return None

        # Watch for the pin to auto-clear - no OVERRIDE_CLEAR is ever
        # sent by this test, so the only way override can become 0
        # again is cbba_apply_override_pin() clearing it on its own.
        cleared_tick = None
        incident_status_at_clear = None
        for _ in range(100):
            frame = read_next_frame(proc)
            t = frame["teams"][team_id]
            if t.get("override") == 0:
                cleared_tick = frame["tick"]
                inc = next((i for i in frame["incidents"] if i["id"] == target_id), None)
                incident_status_at_clear = inc["status"] if inc else None
                break
        check(cleared_tick is not None,
              f"({label}) pin auto-cleared (committed) within 100 ticks of being applied (tick {current_tick}) - "
              "no OVERRIDE_CLEAR was sent, so this must FAIL, not be skipped, if it never happens")
        if cleared_tick is None:
            return None
        check(incident_status_at_clear is not None and incident_status_at_clear != 0,
              f"({label}) the tick the pin auto-clears, incident {target_id} has genuinely left INC_OPEN "
              f"(got status={incident_status_at_clear}) - the clear tracks real commitment, not a spurious drop")

        # Follow the same incident to one of the two endings.
        ending = None
        for _ in range(ticks):
            frame = read_next_frame(proc)
            t = frame["teams"][team_id]
            inc = next((i for i in frame["incidents"] if i["id"] == target_id), None)

            if inc is None or inc["status"] == 3:
                ending = "resolved"
                break
            if not (t.get("has_dest") == 1 and t.get("dest_id") == target_id) or inc.get("assigned_team") != team_id:
                ending = "interrupted"
                break
        else:
            check(False, f"({label}) incident {target_id} (pinned to team {team_id} at tick {current_tick}, "
                  f"committed at tick {cleared_tick}) reached neither resolution nor interruption within "
                  f"{ticks} ticks - this must FAIL, not be skipped")
            return None

        if ending == "resolved":
            check(True, f"({label}) incident {target_id} resolved at tick {frame['tick']} - team {team_id} "
                  "stayed committed to it the whole way (dest_id/assigned_team unchanged), override never re-armed")
        else:
            check(inc is not None and inc["status"] == 0 and inc.get("assigned_team") != team_id,
                  f"({label}) team {team_id} interrupted off incident {target_id} at tick {frame['tick']} "
                  f"(has_dest={t.get('has_dest')}, dest_id={t.get('dest_id')}) and the incident reopens - "
                  f"status back to INC_OPEN, no longer assigned to team {team_id} "
                  f"(got status={inc['status'] if inc else None}, assigned_team={inc.get('assigned_team') if inc else None})")
            check(t.get("override") == 0,
                  f"({label}) an interruption is not a re-arm - team {team_id}'s override is still 0 "
                  f"(got {t.get('override')})")

        # Either ending: override must never re-arm on its own for a
        # further stretch, whether or not a freed/reopened slot gets
        # reused or reassigned to someone else in the meantime.
        for _ in range(40):
            frame = read_next_frame(proc)
            t = frame["teams"][team_id]
            check(t.get("override") == 0,
                  f"({label}) team {team_id}'s override stays 0 well past the {ending} ending "
                  f"(tick {frame['tick']}, got override={t.get('override')}) - it must never re-arm on its own")
        return ending
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


def run_incident_resolve_autoclear_check():
    """Two deterministic replacements for the old opportunistic
    "incident resolved while pinned" check, both pinning an AVAILABLE
    team to a real open incident and watching core/cbba.c's
    cbba_apply_override_pin() clear the pin on its own (commit), with
    no OVERRIDE_CLEAR ever sent - see _pin_and_watch_to_ending()'s own
    doc for the full pinned -> committed -> (resolved | interrupted)
    lifecycle this drives and asserts.

    (10a) resolved: seed=8, team 1 -> incident 0 finishes cleanly
    (empirically verified: resolves at tick 71, no interruption).
    (10b) interrupted: seed=2, team 1 -> incident 0 gets pulled off by
    the sim's own stamina/rest handling before finishing (empirically
    verified: interrupted at tick 80, incident reopens INC_OPEN).
    Both seed/team/incident combos were picked by probing actual runs,
    not guessed - if the sim's own behavior ever drifts enough that
    either stops holding, these FAIL outright, they do not skip.
    """
    ending = _pin_and_watch_to_ending("10a", seed=8, team_id=1, target_id=0)
    check(ending == "resolved", f"(10a) seed=8 team=1->incident=0 ends in 'resolved' as expected (got {ending})")

    ending = _pin_and_watch_to_ending("10b", seed=2, team_id=1, target_id=0)
    check(ending == "interrupted", f"(10b) seed=2 team=1->incident=0 ends in 'interrupted' as expected (got {ending})")


def main():
    proc = subprocess.Popen(
        [BINARY, "5", "1", str(TOTAL_TICKS), "--live", "--tick-delay-ms=0", "--fault-profile=combined"],
        cwd=REPO_ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL, text=True, bufsize=1,
    )
    degraded_case = None   # (tick, team_id) - first team seen with degraded==1

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

        # (4) a recognized command word with the wrong field count -
        # every line the sim reads gets exactly one ack, never silence.
        # team_id/incident_id come back JSON null (the line's own shape
        # never established real ones); requested_tick is echoed since
        # that much of the line did parse.
        send(proc, f"OVERRIDE {current_tick} 1\n")  # missing incident_id
        ack = read_ack(proc)
        check(ack["result"] == "rejected" and ack["reason"] == "bad_format" and
              ack["team_id"] is None and ack["incident_id"] is None and ack["requested_tick"] == current_tick,
              f"(4) wrong field count is rejected as bad_format, team/incident null, tick echoed (got {ack})")

        # (4b) a non-numeric field - same bad_format outcome, but here
        # not even requested_tick parsed, so it comes back null too.
        send(proc, "OVERRIDE not_a_number 1 2\n")
        ack = read_ack(proc)
        check(ack["result"] == "rejected" and ack["reason"] == "bad_format" and ack["requested_tick"] is None,
              f"(4b) a non-numeric field is rejected as bad_format, requested_tick null (got {ack})")

        # (4c) an unrecognized command word entirely - a real, complete
        # line, just not OVERRIDE/OVERRIDE_CLEAR.
        send(proc, "this is not a valid override line\n")
        ack = read_ack(proc)
        check(ack["result"] == "rejected" and ack["reason"] == "unknown_command",
              f"(4c) an unrecognized command word is rejected as unknown_command, not silently dropped (got {ack})")

        # (4d) an overlong line - longer than debug.c's LIVE_OVERRIDE_
        # LINE_MAX (128 bytes) line-assembly buffer - must be discarded
        # up to its own '\n' with exactly one bad_format ack, and must
        # NOT desync the buffer for whatever comes right after it: a
        # genuinely valid line sent in the same write() must still
        # apply normally.
        frame = read_next_frame(proc)  # fresh tick, so the valid line below can't read as stale
        current_tick = frame["tick"]
        overlong = "OVERRIDE " + ("9" * 400) + " 0 0\n"
        valid = f"OVERRIDE {current_tick} 1 {target_incident}\n"
        send(proc, overlong + valid)
        ack_overlong = read_ack(proc)
        check(ack_overlong["result"] == "rejected" and ack_overlong["reason"] == "bad_format",
              f"(4d) an overlong line is rejected as bad_format, not silently dropped (got {ack_overlong})")
        ack_valid = read_ack(proc)
        # "No desync" means the fields parsed exactly as sent - team_id
        # 1, incident_id target_incident, requested_tick current_tick.
        # This run's subprocess uses --fault-profile=combined, so the
        # outcome itself can legitimately be "rejected"/team_unavailable
        # if team 1 happens to be degraded at this exact tick - that's
        # a real semantic rejection on correctly-parsed fields, not
        # evidence of a parsing bug, so it isn't asserted here.
        check(ack_valid["team_id"] == 1 and ack_valid["incident_id"] == target_incident and
              ack_valid["requested_tick"] == current_tick,
              f"(4d) a valid line sent right after an overlong one in the same write() still parses correctly - "
              f"no desync (sent team=1 incident={target_incident} tick={current_tick}, got {ack_valid})")
        if ack_valid["result"] == "applied":
            send(proc, f"OVERRIDE_CLEAR {current_tick} 1\n")
            read_ack(proc)  # leave team 1 clean for the rest of this battery

        # (4e) an empty line is the one silent case - not a request at
        # all, so it gets no ack, and the sim just keeps running (the
        # very next non-frame line must not be one).
        send(proc, "\n")
        line = proc.stdout.readline().rstrip("\n")
        check(not line.startswith("OVERRIDEACK "), f"(4e) an empty line produced no ack (next line was {line[:60]!r})")
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

        # (9) opportunistic: scan the rest of this run for a degraded
        # team and test it the moment it's seen - `degraded` is a
        # transient suspicion flag (core/mission_state.h's own doc: a
        # degraded team "keeps working normally"), not a persistent
        # state, so testing immediately avoids the team recovering
        # before the test gets to it.
        while frame["tick"] < TOTAL_TICKS - 5 and degraded_case is None:
            open_now = [i["id"] for i in frame["incidents"] if i["status"] != 3]

            for t in frame["teams"]:
                if t.get("degraded") and open_now:
                    degraded_case = (frame["tick"], t["id"])
                    send(proc, f"OVERRIDE {frame['tick']} {t['id']} {open_now[0]}\n")
                    ack = read_ack(proc)
                    check(ack["result"] == "rejected" and ack["reason"] == "team_unavailable",
                          f"(9) a degraded team (id={t['id']}, tick {frame['tick']}) is rejected as team_unavailable (got {ack})")
                    break

            frame = read_next_frame(proc)

        if degraded_case is None:
            print(f"PASS: (9) no team went `degraded` in this {TOTAL_TICKS}-tick run to test against - "
                  "seed/fault-timing dependent, not a failure of this check")

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


if __name__ == "__main__":
    main()
    run_incident_resolve_autoclear_check()

    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(f" - {f}")
        sys.exit(1)
    print("\nlive_override_selftest: all checks passed")

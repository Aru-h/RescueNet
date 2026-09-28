#!/usr/bin/env python3
"""
End-to-end smoke test for the Live Ops dashboard rework: map camera,
consensus network, detection timeline, side panels, theme toggle, and
the one-screen Live Ops layout - driven through a real Chromium via
Playwright against a real `app.py` (SIM_ONLY mode - no serial port or
board hardware needed) and a real `tron --live` run.

Playwright is a DEV/TEST-ONLY DEPENDENCY of the dashboard tooling, not
of the simulator, the dataset tooling or the board builds:
  - it is intentionally absent from the repo root's requirements.txt;
  - see laptop/tools/requirements-dashboard-dev.txt for the one place
    it's declared (`pip install -r laptop/tools/requirements-dashboard-
    dev.txt`, then `python -m playwright install chromium` once, in a
    dev environment - never as part of a normal build or the other
    check-* gates);
  - this script never calls `playwright install` itself, so running it
    never triggers a browser download on its own. It looks for a
    machine-preinstalled Chromium at /opt/pw-browsers/chromium first
    (the path this project's CI/sandbox images vendor it at - this may
    be the executable itself, e.g. a symlink, or a directory to search),
    then falls back to whatever Playwright's own `chromium.launch()`
    finds already installed (e.g. via `playwright install` run by hand
    in a dev checkout).

SKIP (exit 0) is reserved for exactly one situation: Playwright itself
isn't importable, or no usable Chromium can be found. Once the browser
is up, every other failure - app.py not starting, a page error, a
failed assertion - is a real FAIL (exit 1). A missing dev dependency
should not fail a check-* batch; a broken dashboard should.

Run directly: `python3 dashboard_e2e_selftest.py` (from laptop/tools),
or `make check-dashboard-e2e`.
"""

import contextlib
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
HOST = "127.0.0.1"
PORT = 5050  # matches app.py's hardcoded DASHBOARD_PORT
BASE_URL = f"http://{HOST}:{PORT}"

VENDORED_CHROMIUM_PATH = "/opt/pw-browsers/chromium"

# Live Ops must fit on one screen (no page scrolling) at these sizes;
# 1470x830 is a 13" MacBook Air M4 at default resolution minus browser
# chrome, 1280x720 is the smaller size it must still work at.
ONE_SCREEN_VIEWPORTS = [(1470, 830), (1280, 720)]

FAILURES = []

# Chromium logs a failed HTTP request (any 4xx/5xx response) as a
# console "error"-level message on its own, independent of whatever
# the page's own JS does with that response - "Failed to load
# resource: the server responded with a status of 400 ()". Several
# checks deliberately trigger an EXPECTED failure response (a failed
# override, a stale pause/resume against an already-ended run) to
# prove the page handles it gracefully; Chromium logging that expected
# failure is not itself a bug, so it's excluded here rather than
# masked by weakening what "no console errors" means everywhere else.
_EXPECTED_HTTP_FAILURE_NOISE = ("responded with a status of 400", "responded with a status of 409")


def real_console_errors(errors):
    return [e for e in errors if not any(marker in e for marker in _EXPECTED_HTTP_FAILURE_NOISE)]


def check(cond, label):
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label}")
        FAILURES.append(label)


def fail_and_exit(message):
    """For setup failures that are NOT 'Playwright/Chromium missing' -
    these must fail the check, not skip it (see module docstring)."""
    print(f"FAIL: dashboard_e2e_selftest ({message})")
    FAILURES.append(message)


def _port_open(host, port):
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.settimeout(0.3)
        return s.connect_ex((host, port)) == 0


def _find_vendored_chromium_executable():
    """An executable Chromium/Chrome path to hand Playwright as
    `executable_path`, preferring the machine-preinstalled location
    this project's CI/sandbox images use (see module docstring).
    Returns None (never downloads, never raises) if nothing usable is
    found there.

    VENDORED_CHROMIUM_PATH may itself be the executable (a real binary
    or, commonly, a symlink to one - `os.path.isfile` follows symlinks,
    so this is checked first and used directly) rather than a
    directory to search; if it's a directory instead, it's walked for
    a chrome/chromium/headless_shell binary, matching how Playwright's
    own browser cache is laid out.
    """
    if os.path.isfile(VENDORED_CHROMIUM_PATH) and os.access(VENDORED_CHROMIUM_PATH, os.X_OK):
        return VENDORED_CHROMIUM_PATH
    if not os.path.isdir(VENDORED_CHROMIUM_PATH):
        return None
    for root, _dirs, files in os.walk(VENDORED_CHROMIUM_PATH):
        for name in ("chrome", "chromium", "headless_shell", "Chromium"):
            if name in files:
                candidate = os.path.join(root, name)
                if os.access(candidate, os.X_OK):
                    return candidate
    return None


def _live_run_scrollheight_check(browser, width, height):
    """Opens a fresh page at (width, height), enters simulation mode,
    starts a short live run, and asserts the Live Ops tab needs no
    page scroll: document.documentElement.scrollHeight must equal
    window.innerHeight (a small rounding tolerance is allowed for
    sub-pixel layout, not for an actual extra row of content)."""
    page = browser.new_page(viewport={"width": width, "height": height})
    console_errors = []
    page.on("pageerror", lambda exc: console_errors.append(str(exc)))
    page.on("console", lambda m: console_errors.append(m.text) if m.type == "error" else None)
    try:
        page.goto(BASE_URL, wait_until="networkidle")
        page.click("text=Bypass & Enter Dashboard")
        page.wait_for_selector("#main-dashboard.active", timeout=5000)

        is_live_active = page.eval_on_selector("#tab-live", "el => !el.classList.contains('hidden')")
        check(is_live_active, f"[{width}x{height}] Live Ops is the default active tab")

        page.fill("#seed", "5")
        page.fill("#ticks", "50")
        page.select_option("#faultProfile", "combined")
        page.fill("#tickDelay", "5")
        page.click("#liveBtn")
        page.wait_for_timeout(2500)

        metrics = page.evaluate("""() => ({
          scrollHeight: document.documentElement.scrollHeight,
          innerHeight: window.innerHeight,
          scrollWidth: document.documentElement.scrollWidth,
          innerWidth: window.innerWidth,
        })""")
        check(abs(metrics["scrollHeight"] - metrics["innerHeight"]) <= 1,
              f"[{width}x{height}] Live Ops needs no page scroll "
              f"(document.scrollHeight={metrics['scrollHeight']} vs. window.innerHeight={metrics['innerHeight']})")
        check(abs(metrics["scrollWidth"] - metrics["innerWidth"]) <= 1,
              f"[{width}x{height}] Live Ops has no horizontal page overflow "
              f"(document.scrollWidth={metrics['scrollWidth']} vs. window.innerWidth={metrics['innerWidth']})")

        stop_btn = page.query_selector("#stopLiveBtn")
        if stop_btn and stop_btn.is_visible():
            stop_btn.click()
        page.wait_for_timeout(200)
        check(len(console_errors) == 0,
              f"[{width}x{height}] no console/page errors during the run (got {len(console_errors)}: "
              f"{console_errors[:5]})")
    finally:
        page.close()


def _run_control_state_check(page, console_errors):
    """Regression coverage for a real bug report: (a) a failed override
    must never affect the live run itself - frames keep arriving; (b) a
    genuine pause actually stops frames, and resume actually resumes
    them; (c) once the run has ended, no run control (Pause/Resume/
    Stop) is visible/clickable - covers the case where the client's UI
    state could otherwise go stale (e.g. showing "paused" forever) if a
    control's request ever came back non-ok instead of updating state."""
    page.select_option("#faultProfile", "none")
    page.fill("#seed", "31")
    page.fill("#ticks", "45")
    page.fill("#tickDelay", "150")
    page.click("#liveBtn")
    page.wait_for_timeout(1000)

    # (a) An override for a nonexistent incident_id (999999) must only
    # show its own error, never touch the run: frames keep arriving for
    # the next 5 seconds. Since Part 4, sim mode actually processes this
    # request (laptop/debug.c's debug_live_override_poll()) and comes
    # back "Rejected" with reason unknown_incident, rather than the
    # pre-Part-4 "Failed" (board not connected) - both are a failure
    # outcome for this check's purposes, so it accepts either wording.
    injected = page.evaluate("""() => {
      const frame = window._lastFrame;
      if (!frame) return false;
      const team = frame.teams[0];
      pendingOverride = { team, teamIndex: 0, from: {x: team.x, y: team.y}, to: { id: 999999, x: 5, y: 5 } };
      selectedMarker = { type: 'team', id: team.id };
      renderInspector();
      return true;
    }""")
    check(injected, "(a) able to stage a pending override on the live run")
    page.click("#overrideConfirmBtn")
    page.wait_for_timeout(400)
    override_failed = page.evaluate("() => document.getElementById('overrideBox').textContent")
    check(bool(override_failed and any(w in override_failed for w in ("Failed", "failed", "Rejected", "rejected"))),
          f"(a) the override shows a failure in its own box (got {override_failed!r})")

    tick_at_failure = page.evaluate("() => window._lastFrame ? window._lastFrame.tick : -1")
    page.wait_for_timeout(5000)
    tick_after_5s = page.evaluate("() => window._lastFrame ? window._lastFrame.tick : -1")
    state_after_5s = page.eval_on_selector("#runStateLabel", "el => el.textContent")
    check(tick_after_5s > tick_at_failure,
          f"(a) the live run kept producing frames for 5s after a failed override (tick {tick_at_failure} -> {tick_after_5s})")
    check(state_after_5s == "running",
          f"(a) run state is still 'running' 5s after a failed override (got {state_after_5s!r})")

    # (b) A genuine pause actually stops frames; resume actually
    # resumes them.
    page.click("#pauseLiveBtn")
    page.wait_for_timeout(300)
    state_paused = page.eval_on_selector("#runStateLabel", "el => el.textContent")
    check(state_paused == "paused", f"(b) Pause switches state to 'paused' (got {state_paused!r})")
    tick_at_pause = page.evaluate("() => window._lastFrame ? window._lastFrame.tick : -1")
    page.wait_for_timeout(1500)
    tick_while_paused = page.evaluate("() => window._lastFrame ? window._lastFrame.tick : -1")
    check(tick_while_paused == tick_at_pause,
          f"(b) frames stop arriving while genuinely paused (tick stayed at {tick_at_pause}, got {tick_while_paused})")

    page.click("#resumeLiveBtn")
    page.wait_for_timeout(300)
    state_resumed = page.eval_on_selector("#runStateLabel", "el => el.textContent")
    check(state_resumed == "running", f"(b) Resume switches state back to 'running' (got {state_resumed!r})")
    page.wait_for_timeout(1500)
    tick_after_resume = page.evaluate("() => window._lastFrame ? window._lastFrame.tick : -1")
    check(tick_after_resume > tick_while_paused,
          f"(b) frames resume arriving after Resume (tick {tick_while_paused} -> {tick_after_resume})")

    # (c) Let the run finish naturally (ticks=45 is short), then
    # confirm no run control is left visible/clickable - regression for
    # the bug where a stale "paused" UI state left Resume clickable
    # forever after the server-side run had already ended, 409-ing on
    # every click instead of ever resyncing the UI.
    for _ in range(40):
        state = page.eval_on_selector("#runStateLabel", "el => el.textContent")
        if state == "finished":
            break
        page.wait_for_timeout(300)
    else:
        fail_and_exit("(c) live run did not reach 'finished' within 12s of its natural end")
        return

    pause_hidden = page.eval_on_selector("#pauseLiveBtn", "el => el.classList.contains('hidden')")
    resume_hidden = page.eval_on_selector("#resumeLiveBtn", "el => el.classList.contains('hidden')")
    stop_hidden = page.eval_on_selector("#stopLiveBtn", "el => el.classList.contains('hidden')")
    check(pause_hidden and resume_hidden and stop_hidden,
          f"(c) once the run has ended, no run control is visible "
          f"(pause_hidden={pause_hidden}, resume_hidden={resume_hidden}, stop_hidden={stop_hidden})")

    # Belt-and-suspenders regression for the exact reported symptom: even
    # if a control were still visible/clicked here, clicking it must
    # resync the UI to "finished" rather than leaving it stuck - simulate
    # a stale click directly against the now-dead run.
    page.evaluate("() => resumeLive()")
    page.wait_for_timeout(400)
    state_after_stale_resume = page.eval_on_selector("#runStateLabel", "el => el.textContent")
    check(state_after_stale_resume == "finished",
          f"(c) a resume attempt against an already-ended run resyncs the UI to 'finished', not stuck (got {state_after_stale_resume!r})")

    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0,
          f"(a)(b)(c) no UNEXPECTED console/page errors during the run-control checks "
          f"(got {len(real_errors)}, ignoring the deliberately-triggered 400/409 network noise: {real_errors[:5]})")

    # Leave the page clean for whatever runs next (_replay_check): the
    # deliberate override failure and stale-resume above both leave
    # overrideStatus set, which would otherwise make renderOverrideBox()
    # show stale content regardless of pendingOverride/drag state.
    page.evaluate("() => { overrideStatus = null; pendingOverride = null; selectedMarker = null; renderInspector(); }")


def _live_override_apply_release_check(page, console_errors):
    """Part 4: a VALID sim-mode override (a real open incident) is
    actually applied, shows up live on LIVEFRAME (which the map/team-
    card indicators and the Release button all read directly, so this
    covers them transitively), gets logged to the Incident Journal, and
    Release actually clears it. Runs its own live run with a generous
    tick budget - _run_control_state_check's own 45-tick run is sized
    for its pause/resume/stop lifecycle checks and doesn't leave enough
    wall-clock headroom for the polling this needs on top of that."""
    page.select_option("#faultProfile", "none")
    page.fill("#seed", "31")
    page.fill("#ticks", "350")
    page.fill("#tickDelay", "150")
    page.click("#liveBtn")

    # Polls rather than one fixed wait: incident spawn timing is itself
    # randomized (fault_profile=none still spawns incidents on its own
    # schedule), so a single fixed delay was intermittently too short.
    staged = None
    for _ in range(20):
        page.wait_for_timeout(500)
        staged = page.evaluate("""() => {
          const frame = window._lastFrame;
          // pool 0 only (real incidents) - a pool-1 system task is
          // rejected with reason "system_task" (core/debug.h's doc):
          // core/cbba.c's own pin-validation only ever looks in
          // g_incidents, so this checks the same thing a real drag-
          // drop onto a real incident marker would.
          const inc = frame && (frame.incidents || []).find(i => i.status !== 3 && i.pool === 0);
          if (!inc) return null;
          const team = frame.teams[0];
          pendingOverride = { team, teamIndex: 0, from: {x: team.x, y: team.y}, to: inc };
          selectedMarker = { type: 'team', id: team.id };
          renderInspector();
          return inc.id;
        }""")
        if staged is not None:
            break
    check(staged is not None, f"a real open incident was available to override onto (got incident id {staged})")
    if staged is not None:
        journal_count_before = page.evaluate("() => (window._journalEvents || []).length")
        page.click("#overrideConfirmBtn")
        applied = False
        box_text = None
        # Polled together, not as two sequential loops: a short-lived
        # incident (already nearly done when staged - a real,
        # observed case, not hypothetical) can have
        # cbba_apply_override_pin() auto-clear the pin again within a
        # tick or two of it being set, so checking LIVEFRAME only
        # AFTER the box-text loop already exited could miss a window
        # that came and went while that loop was still polling.
        saw_override_applied = False
        for _ in range(60):
            page.wait_for_timeout(200)
            box_text = page.evaluate("() => document.getElementById('overrideBox').textContent")
            if box_text and "Applied" in box_text:
                applied = True
            team0_state = page.evaluate("""() => ({
              override: window._lastFrame.teams[0].override,
              override_incident_id: window._lastFrame.teams[0].override_incident_id,
            })""")
            if team0_state["override"] == 1 and team0_state["override_incident_id"] == staged:
                saw_override_applied = True
            if applied and saw_override_applied:
                break
        check(applied, f"a valid override is applied (got override box text: {box_text!r})")
        check(saw_override_applied,
              f"LIVEFRAME showed team 0 overridden to incident {staged} at some point during processing (got {team0_state})")

        journal_has_new_applied = page.evaluate(f"""() => {{
          const events = window._journalEvents || [];
          return events.length > {journal_count_before} &&
                 events.some(e => e.kind === 'override' && e.team === 0 && e.pillCls === 'good' && e.label.includes('applied'));
        }}""")
        check(journal_has_new_applied, "the applied override was logged to the Incident Journal as a new entry")

        page.evaluate("() => { overrideStatus = null; pendingOverride = null; selectedMarker = null; renderInspector(); }")
        page.wait_for_timeout(100)

        page.evaluate("() => releaseLiveOverride(0)")
        released = False
        for _ in range(60):
            page.wait_for_timeout(200)
            if page.evaluate("() => window._lastFrame.teams[0].override === 0"):
                released = True
                break
        check(released, "Release actually clears the override (LIVEFRAME team 0 override back to 0)")

    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0,
          f"no UNEXPECTED console/page errors during the valid-override/release check "
          f"(got {len(real_errors)}, ignoring the deliberately-triggered 400/409 network noise: {real_errors[:5]})")

    stop_btn = page.query_selector("#stopLiveBtn")
    if stop_btn and stop_btn.is_visible():
        stop_btn.click()
        page.wait_for_timeout(300)
    page.evaluate("() => { overrideStatus = null; pendingOverride = null; selectedMarker = null; renderInspector(); }")


def _replay_check(page, console_errors):
    """Part 2: records a short live run (already produced a recording
    file server-side, since /api/run-live always records - see app.py's
    _RecordingWriter), switches to Replay mode, loads it, and exercises
    play/pause, scrubbing, jump-to-next-detection, and the drag-drop
    lockout - all against a real recording produced by this same run,
    not a hand-built fixture."""
    page.select_option("#faultProfile", "combined")
    page.fill("#seed", "21")
    page.fill("#ticks", "150")
    page.fill("#tickDelay", "3")
    page.click("#liveBtn")
    for _ in range(60):
        if page.eval_on_selector("#runStateLabel", "el => el.textContent") == "finished":
            break
        page.wait_for_timeout(200)
    else:
        fail_and_exit("live run for the replay test did not reach 'finished' within 12s")
        return

    page.select_option("#modeSelect", "replay")
    live_hidden = page.eval_on_selector("#liveControlsGroup", "el => el.classList.contains('hidden')")
    replay_shown = page.eval_on_selector("#replayControlsGroup", "el => !el.classList.contains('hidden')")
    check(live_hidden and replay_shown, "switching to Replay mode swaps the topbar's live controls for replay controls")

    page.wait_for_timeout(300)  # /api/recordings round-trip populating #replaySelect
    option_count = page.eval_on_selector_all("#replaySelect option", "els => els.length")
    check(option_count >= 1, f"the recordings list is populated after the live run finished (got {option_count} option(s))")
    if option_count < 1:
        return

    page.select_option("#replaySelect", index=0)  # newest first - the run just completed
    page.click("#replayLoadBtn")
    page.wait_for_timeout(300)

    frame_count = page.evaluate("() => Replay.frameCount")
    check(frame_count > 0, f"the selected recording loaded with frames (got {frame_count})")
    if frame_count <= 0:
        return

    scrub_visible = page.eval_on_selector("#replayScrubBar", "el => !el.classList.contains('hidden')")
    check(scrub_visible, "the replay scrub bar appears once a recording is loaded")

    team_rows = page.eval_on_selector_all("#liveTeamTable tbody tr", "els => els.length")
    check(team_rows == 10, f"a loaded recording's first frame renders all 10 teams in the Teams table (got {team_rows})")

    # Play / Pause.
    page.click("#replayPlayBtn")
    page.wait_for_timeout(50)
    state_playing = page.eval_on_selector("#replayStateLabel", "el => el.textContent")
    check(state_playing == "playing", f"clicking Play switches replay state to 'playing' (got {state_playing!r})")
    page.wait_for_timeout(400)
    idx_while_playing = page.evaluate("() => Replay.currentIndex")
    check(idx_while_playing > 0, f"playback actually advances through frames (currentIndex={idx_while_playing})")
    page.click("#replayPlayBtn")
    page.wait_for_timeout(50)
    state_paused = page.eval_on_selector("#replayStateLabel", "el => el.textContent")
    check(state_paused == "paused", f"clicking Pause switches replay state back to 'paused' (got {state_paused!r})")

    # Scrubbing: jump straight to an arbitrary frame - array indexing,
    # not a re-fetch/re-parse of the recording (see replay.js's own
    # comment on why this is safe to do repeatedly).
    target = min(10, frame_count - 1)
    page.evaluate(f"""() => {{
      const el = document.getElementById('replayScrubber');
      el.value = {target};
      el.dispatchEvent(new Event('input'));
    }}""")
    page.wait_for_timeout(100)
    idx_after_scrub = page.evaluate("() => Replay.currentIndex")
    check(idx_after_scrub == target, f"scrubbing the range input seeks to that exact frame index (expected {target}, got {idx_after_scrub})")

    # Jump to next detection - only asserted strictly if this particular
    # seeded run actually produced one (combined faults over 150 ticks
    # almost always do, but this must not be a flaky assertion on a run
    # that happened not to).
    page.evaluate("() => Replay.seekToIndex(0)")
    idx_before_jump = page.evaluate("() => Replay.currentIndex")
    page.click("#replayJumpBtn")
    page.wait_for_timeout(100)
    idx_after_jump = page.evaluate("() => Replay.currentIndex")
    if idx_after_jump > idx_before_jump:
        check(True, f"Next detection jumps forward to a real detection event (tick index {idx_before_jump} -> {idx_after_jump})")
    else:
        print(f"INFO: no detection event found in this run's recording after index {idx_before_jump} "
              "(not a failure - combined faults don't guarantee one in every seeded run); "
              "Next detection correctly left the position unchanged")

    # Drag-drop lockout: a real mouse drag from a team marker onto an
    # incident marker must NOT open the override box while in replay.
    # A later frame is used (not tick 0) since incidents typically
    # haven't been discovered/assigned yet at the very start of a run -
    # scanning several candidate frames for a team/incident pair that
    # actually exists keeps this assertion from being skipped in
    # practice rather than actually exercising the drag path.
    positions = None
    for probe_idx in (30, 60, 90, 120, min(10, frame_count - 1), 0):
        if probe_idx < 0 or probe_idx >= frame_count:
            continue
        page.evaluate(f"() => Replay.seekToIndex({probe_idx})")
        page.wait_for_timeout(80)
        positions = page.evaluate("""() => {
          const frame = window._lastFrame;
          if (!frame) return null;
          for (const team of frame.teams) {
            const inc = frame.incidents.find(i => i.status !== 3 && i.id !== team.dest_id);
            if (inc) {
              const rect = document.getElementById('mapCanvas').getBoundingClientRect();
              const teamPt = mapCamera.worldToScreen(team.x, team.y);
              const incPt = mapCamera.worldToScreen(inc.x, inc.y);
              return { teamX: rect.left + teamPt.x, teamY: rect.top + teamPt.y, incX: rect.left + incPt.x, incY: rect.top + incPt.y };
            }
          }
          return null;
        }""")
        if positions:
            break
    replay_active = page.evaluate("() => Replay.isActive()")
    check(replay_active, "Replay.isActive() is true while a recording is loaded (gates map drag-drop)")
    if positions:
        page.mouse.move(positions["teamX"], positions["teamY"])
        page.mouse.down()
        page.mouse.move(positions["incX"], positions["incY"], steps=5)
        page.mouse.up()
        page.wait_for_timeout(150)
        override_box_hidden = page.eval_on_selector("#overrideBox", "el => el.classList.contains('hidden')")
        check(override_box_hidden, "dragging a team onto an incident in Replay mode does not open the override box (team markers aren't draggable)")
    else:
        print("INFO: no eligible (team, incident) pair found at tick 0 of this recording to drag-test - skipped that one assertion")

    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0,
          f"no UNEXPECTED console/page errors during the replay checks "
          f"(got {len(real_errors)}, ignoring earlier deliberately-triggered 400/409 network noise: {real_errors[:5]})")

    # Back to Live mode - leaves the page in a clean state for anything after this.
    page.select_option("#modeSelect", "live")
    page.wait_for_timeout(100)
    scrub_hidden_again = page.eval_on_selector("#replayScrubBar", "el => el.classList.contains('hidden')")
    check(scrub_hidden_again, "switching back to Live mode hides the replay scrub bar again")


def main():
    try:
        from playwright.sync_api import sync_playwright
    except ImportError as exc:
        print("SKIP: dashboard_e2e_selftest (playwright not installed - "
              "it is a dev/test-only dependency of the dashboard tooling, "
              "never required for a normal build; "
              "pip install -r laptop/tools/requirements-dashboard-dev.txt "
              f"and `python -m playwright install chromium` to enable this check): {exc}")
        return 0

    app_proc = None
    we_started_it = False
    # Recordings this test's own live run(s) produce go to a throwaway
    # temp directory, not laptop/tools/recordings/ - that folder holds
    # exactly one curated demo file tracked in git, and this test would
    # otherwise litter it with a fresh recording on every run.
    recordings_tmpdir = tempfile.mkdtemp(prefix="dashboard_e2e_recordings_")
    if not _port_open(HOST, PORT):
        we_started_it = True
        env = dict(os.environ, SIM_ONLY="1", DASHBOARD_RECORDINGS_DIR=recordings_tmpdir)
        app_proc = subprocess.Popen(
            [sys.executable, "app.py"], cwd=TOOLS_DIR, env=env,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        for _ in range(50):
            if _port_open(HOST, PORT):
                break
            time.sleep(0.2)
        else:
            # app.py failing to start is a real bug/environment problem,
            # not a missing dev-only dependency - this must FAIL, not skip.
            app_proc.kill()
            fail_and_exit(f"app.py did not come up on {HOST}:{PORT} within 10s")
            return 1
    # else: something (a dev's own `python app.py`, hardware or sim mode)
    # is already listening on the dashboard's port - reuse it rather than
    # fighting over the port or killing someone else's server.

    try:
        with sync_playwright() as p:
            launch_kwargs = {}
            vendored = _find_vendored_chromium_executable()
            if vendored:
                launch_kwargs["executable_path"] = vendored
            try:
                browser = p.chromium.launch(**launch_kwargs)
            except Exception as exc:  # noqa: BLE001
                print("SKIP: dashboard_e2e_selftest (no usable Chromium - "
                      f"not preinstalled at {VENDORED_CHROMIUM_PATH} and Playwright's "
                      "own launch() found none either; run `python -m playwright "
                      "install chromium` in a dev environment to enable this check "
                      f"- this script never does that itself): {exc}")
                return 0

            console_errors = []
            page = browser.new_page(viewport={"width": 1366, "height": 900})
            page.on("pageerror", lambda exc: console_errors.append(str(exc)))
            page.on("console", lambda m: console_errors.append(m.text) if m.type == "error" else None)

            page.goto(BASE_URL, wait_until="networkidle")
            check(page.title() != "", "index page loads")

            # Regression test for a pre-existing bug (present before this
            # dashboard rework too, confirmed against the unmodified
            # template): #startup-screen's own ID-selector CSS rule used
            # to always beat the .app-screen.active toggle, so the
            # startup gate never actually hid once the dashboard became
            # active. Fixed by scoping that rule to #startup-screen.active.
            page.click("text=Bypass & Enter Dashboard")
            page.wait_for_selector("#main-dashboard.active", timeout=5000)
            startup_display = page.eval_on_selector("#startup-screen", "el => getComputedStyle(el).display")
            check(startup_display == "none", "startup screen actually hides once simulation mode is entered")

            default_fault = page.eval_on_selector("#faultProfile", "el => el.value")
            check(default_fault == "random", f"default fault profile is 'random' (UI pass item 3, got {default_fault!r})")

            # A short real live run (simulator-only - no board/hardware
            # involved) so the map/consensus-network/timeline/radio panels
            # all get real LIVEFRAME data to render, not just their
            # empty/initial state.
            page.fill("#seed", "3")
            page.fill("#ticks", "60")
            page.select_option("#faultProfile", "communication")
            page.fill("#tickDelay", "5")
            page.click("#liveBtn")
            page.wait_for_timeout(4000)

            has_frame = page.evaluate("() => !!window._lastFrame")
            check(has_frame, "a live frame arrived and was stored")

            hit_count = page.evaluate("() => (typeof mapHitTargets !== 'undefined') ? mapHitTargets.length : -1")
            check(hit_count > 0, f"the map produced hit targets for the current frame (got {hit_count})")

            # Regression test for a real bug report: STATICENV's building
            # x/y is the footprint's CENTRE (core/environment.c's
            # place_building_rect: loc_x = x + w/2, w/h are the full
            # footprint), not its top-left - drawBuildings() must derive
            # the footprint rect by subtracting HALF the footprint from
            # that centre, or every building renders shifted by half its
            # own size. Tests the exported pure function directly rather
            # than inferring centring from rendered pixels.
            buildings = page.evaluate("() => (staticEnvironment && staticEnvironment.buildings) || []")
            check(len(buildings) > 0, f"static environment produced at least one building (got {len(buildings)})")
            if buildings:
                b = buildings[0]
                rect = page.evaluate("(b) => MapRender.buildingFootprintWorldRect(b)", b)
                w = max(1, b["w"] or 1)
                h = max(1, b["h"] or 1)
                expected_x, expected_y = b["x"] - w / 2, b["y"] - h / 2
                centred = abs(rect["x"] - expected_x) < 1e-6 and abs(rect["y"] - expected_y) < 1e-6
                check(centred,
                      "building footprint is centred on STATICENV's x/y, not shifted by half its footprint "
                      f"(building x={b['x']} y={b['y']} w={b['w']} h={b['h']} -> rect {rect!r}, "
                      f"expected top-left ({expected_x}, {expected_y}))")

            cam_zoom = page.evaluate("() => mapCamera.state.zoom")
            cam_min = page.evaluate("() => mapCamera.state.minZoom")
            cam_max = page.evaluate("() => mapCamera.state.maxZoom")
            check(cam_min <= cam_zoom <= cam_max,
                  f"camera zoom ({cam_zoom}) is within its own [min, max] bounds ({cam_min}, {cam_max}) "
                  "- regression test for the hidden-canvas 1x1-placeholder bug")

            team_rows = page.eval_on_selector_all("#liveTeamTable tbody tr", "els => els.length")
            check(team_rows == 10, f"Team Status table has one row per team (got {team_rows})")

            # Teams table columns (UI pass item 6): Pos/Hours/Votes were
            # dropped so the table needs no horizontal scroll - assert
            # both the column count and the actual fit.
            page.click(".side-tab-btn[data-sidetab='teams']")
            teams_visible = page.eval_on_selector("#sidetab-teams", "el => !el.classList.contains('hidden')")
            check(teams_visible, "clicking the Teams side-tab shows the team table panel")
            network_hidden = page.eval_on_selector("#sidetab-network", "el => el.classList.contains('hidden')")
            check(network_hidden, "switching to Teams hides the Network side-tab panel")
            header_cells = page.eval_on_selector_all("#liveTeamTable thead th", "els => els.map(e => e.textContent.trim())")
            check(header_cells == ["Team", "Status", "Stamina", "Fault(injected)", "PAR", "Mob", "Comm"],
                  f"Teams table has exactly the 7 kept columns, in order (got {header_cells})")
            teams_overflow = page.evaluate("""() => {
              const t = document.getElementById('liveTeamTable');
              return { scrollWidth: t.parentElement.scrollWidth, clientWidth: t.parentElement.clientWidth };
            }""")
            check(teams_overflow["scrollWidth"] <= teams_overflow["clientWidth"] + 1,
                  f"Teams panel has no horizontal overflow (scrollWidth={teams_overflow['scrollWidth']} "
                  f"vs. clientWidth={teams_overflow['clientWidth']})")
            page.click(".side-tab-btn[data-sidetab='network']")

            # Selected-team card (UI pass item 8): compact 2-row format,
            # tier pills, live/dark count, votes spelled out in full
            # (this is where the Votes column moved to, off the table).
            page.evaluate("() => { selectedMarker = {type:'team', id: window._lastFrame.teams[0].id}; renderInspector(); }")
            page.wait_for_timeout(100)
            card_text = page.eval_on_selector("#mapInspector", "el => el.textContent")
            check(bool(card_text and "Team 0" in card_text and "live" in card_text and "dark" in card_text),
                  f"selected-team card shows identity + live/dark count (got {card_text!r})")
            check(bool(card_text and "fault" in card_text and "clear" in card_text and "abstain" in card_text),
                  f"selected-team card spells out the vote breakdown (got {card_text!r})")

            # Network tab (UI pass item 7): legend + ring + team card all
            # fit without the side panel needing its own inner scroll.
            net_fit = page.evaluate("""() => {
              const c = document.querySelector('.live-side-content');
              return { scrollHeight: c.scrollHeight, clientHeight: c.clientHeight };
            }""")
            check(net_fit["scrollHeight"] <= net_fit["clientHeight"] + 1,
                  f"Network tab fits without inner scroll (content scrollHeight={net_fit['scrollHeight']} "
                  f"vs. clientHeight={net_fit['clientHeight']})")

            # Pending override box (UI pass item 9): appears in the
            # Network tab, and both action buttons stay fully inside
            # the viewport - nothing cut off at the panel's bottom edge.
            page.evaluate("""() => {
              const frame = window._lastFrame;
              const team = frame.teams[1];
              const inc = frame.incidents.find(i => i.status !== 3 && i.id !== team.dest_id);
              if (inc) {
                pendingOverride = { team, teamIndex: team.i !== undefined ? team.i : 1, from: {x: team.x, y: team.y}, to: inc };
                selectedMarker = {type:'team', id: team.id};
                renderInspector();
              }
            }""")
            page.wait_for_timeout(150)
            override_fit = page.evaluate("""() => {
              const confirmBtn = document.getElementById('overrideConfirmBtn');
              const cancelBtn = document.getElementById('overrideCancelBtn');
              if (!confirmBtn || !cancelBtn) return null;
              const vw = window.innerWidth, vh = window.innerHeight;
              const within = (r) => r.top >= 0 && r.left >= 0 && r.bottom <= vh + 1 && r.right <= vw + 1 && r.width > 0 && r.height > 0;
              return { confirmWithin: within(confirmBtn.getBoundingClientRect()), cancelWithin: within(cancelBtn.getBoundingClientRect()) };
            }""")
            check(bool(override_fit and override_fit["confirmWithin"] and override_fit["cancelWithin"]),
                  f"pending override's Confirm/Cancel buttons are fully inside the viewport (got {override_fit})")
            page.evaluate("() => { pendingOverride = null; renderInspector(); }")

            # Radio panel moved to Overview (UI pass item 5), replacing
            # cumulative airtime with "channel busy %".
            page.click("[data-tab='overview']")
            page.wait_for_timeout(150)
            radio_text = page.eval_on_selector("#overviewRadioPanelBody", "el => el.textContent")
            check(bool(radio_text and radio_text.strip()), "Overview's Radio panel populated from vbus")
            check("Per-receiver loss" in (radio_text or ""), "Radio panel labels loss as per-receiver, not per-broadcast")
            check("Channel busy" in (radio_text or ""), "Radio panel shows channel busy %, not raw cumulative airtime")
            page.click("[data-tab='live']")
            page.wait_for_timeout(150)

            # Radio per-receiver loss %: dropped / (delivered + dropped),
            # not dropped / sent (see panels.js's radioLossPct()).
            vbus = page.evaluate("() => window._lastFrame.vbus")
            if vbus and (vbus["delivered"] + vbus["dropped"]) > 0:
                expected_pct = vbus["dropped"] / (vbus["delivered"] + vbus["dropped"]) * 100
                loss_text = page.eval_on_selector("#topbarLoss", "el => el.textContent")
                got_pct = float(loss_text.rstrip("%")) if loss_text and loss_text != "n/a" else None
                check(got_pct is not None and abs(got_pct - expected_pct) < 0.15,
                      f"topbar radio loss ({loss_text!r}) matches dropped/(delivered+dropped) "
                      f"(expected {expected_pct:.1f}%)")

            # Only one theme toggle exists (a UI pass removed the header's
            # duplicate).
            toggle_count = page.evaluate("() => document.querySelectorAll('#themeToggleBtn, #themeToggleBtnBar').length")
            check(toggle_count == 1, f"exactly one theme toggle button exists (got {toggle_count})")

            page.click("#themeToggleBtn")
            page.wait_for_timeout(150)
            theme_attr = page.eval_on_selector("html", "el => el.getAttribute('data-theme')")
            check(theme_attr == "light", f"theme toggle switches to light (got {theme_attr!r})")
            bg_light = page.eval_on_selector("body", "el => getComputedStyle(el).backgroundColor")
            page.click("#themeToggleBtn")
            page.wait_for_timeout(150)
            bg_dark = page.eval_on_selector("body", "el => getComputedStyle(el).backgroundColor")
            check(bg_light != bg_dark, "toggling the theme actually changes the rendered background colour")

            # Pause/Resume/Stop change the run state (UI pass item 2).
            # A fresh, deliberately long-running live run: the earlier
            # 60-tick/5ms run this test already exercised may well have
            # finished by now (several checks and waits happened since),
            # and a finished run hides #pauseLiveBtn - clicking a hidden
            # button would just time out rather than test anything.
            if page.query_selector("#stopLiveBtn") and page.is_visible("#stopLiveBtn"):
                page.click("#stopLiveBtn")
                page.wait_for_timeout(200)
            page.fill("#seed", "11")
            page.fill("#ticks", "5000")
            page.fill("#tickDelay", "50")
            page.click("#liveBtn")
            page.wait_for_timeout(500)
            state_running = page.eval_on_selector("#runStateLabel", "el => el.textContent")
            check(state_running == "running", f"run state starts at 'running' (got {state_running!r})")
            page.click("#pauseLiveBtn")
            page.wait_for_timeout(300)
            state_paused = page.eval_on_selector("#runStateLabel", "el => el.textContent")
            pause_btn_hidden = page.eval_on_selector("#pauseLiveBtn", "el => el.classList.contains('hidden')")
            resume_btn_shown = page.eval_on_selector("#resumeLiveBtn", "el => !el.classList.contains('hidden')")
            check(state_paused == "paused" and pause_btn_hidden and resume_btn_shown,
                  f"Pause switches run state to 'paused' and swaps the button for Resume (state={state_paused!r})")
            page.click("#resumeLiveBtn")
            page.wait_for_timeout(300)
            state_resumed = page.eval_on_selector("#runStateLabel", "el => el.textContent")
            check(state_resumed == "running", f"Resume switches run state back to 'running' (got {state_resumed!r})")

            # The run may already have finished on its own (a short,
            # fast tick-delay run easily beats the wait above) - #stopLiveBtn
            # goes hidden again once it has, so only click it if it's still
            # there to click.
            stop_btn = page.query_selector("#stopLiveBtn")
            if stop_btn and stop_btn.is_visible():
                stop_btn.click()
                page.wait_for_timeout(300)
                state_stopped = page.eval_on_selector("#runStateLabel", "el => el.textContent")
                check(state_stopped == "idle", f"Stop switches run state to 'idle' (got {state_stopped!r})")

            check(len(console_errors) == 0,
                  f"no browser console errors / page errors during the run (got {len(console_errors)}: "
                  f"{console_errors[:5]})")

            # Regression coverage for the failed-override/stale-run-control
            # bug report: a failed override must never affect the run, a
            # genuine pause/resume must actually stop/resume frames, and no
            # run control may be left clickable once the run has ended.
            if page.query_selector("#stopLiveBtn") and page.is_visible("#stopLiveBtn"):
                page.click("#stopLiveBtn")
                page.wait_for_timeout(200)
            _run_control_state_check(page, console_errors)

            # Part 4: a valid sim-mode override, applied and released.
            _live_override_apply_release_check(page, console_errors)

            # Part 2: replay of a recorded live run.
            _replay_check(page, console_errors)

            page.close()

            # One-screen Live Ops layout at the two required sizes.
            for width, height in ONE_SCREEN_VIEWPORTS:
                _live_run_scrollheight_check(browser, width, height)

            browser.close()
    finally:
        if we_started_it and app_proc is not None:
            app_proc.terminate()
            try:
                app_proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                app_proc.kill()
        shutil.rmtree(recordings_tmpdir, ignore_errors=True)

    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(" -", f)
        return 1
    print("\nAll dashboard_e2e_selftest checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

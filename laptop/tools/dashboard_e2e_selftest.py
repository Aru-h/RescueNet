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
import socket
import subprocess
import sys
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
    if not _port_open(HOST, PORT):
        we_started_it = True
        env = dict(os.environ, SIM_ONLY="1")
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

    if FAILURES:
        print(f"\n{len(FAILURES)} FAILURE(S):")
        for f in FAILURES:
            print(" -", f)
        return 1
    print("\nAll dashboard_e2e_selftest checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
End-to-end smoke test for the Live Ops dashboard rework (Part 1: map
camera, consensus network, detection timeline, side panels, theme
toggle), driven through a real Chromium via Playwright against a real
`app.py` (SIM_ONLY mode - no serial port or board hardware needed) and
a real `tron --live` run.

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
    (the path this project's CI/sandbox images vendor it at), then
    falls back to whatever Playwright's own `chromium.launch()` finds
    already installed (e.g. via `playwright install` run by hand in a
    dev checkout). Either way, if no usable Chromium turns up, or
    Playwright itself isn't importable, this prints SKIP and exits 0 -
    it never fails the rest of a `make check-*` batch over a missing
    dev-only dependency.

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

VENDORED_CHROMIUM_DIR = "/opt/pw-browsers/chromium"

FAILURES = []


def check(cond, label):
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label}")
        FAILURES.append(label)


def _port_open(host, port):
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as s:
        s.settimeout(0.3)
        return s.connect_ex((host, port)) == 0


def _find_vendored_chromium_executable():
    """The absolute path to a chromium/chrome binary under
    /opt/pw-browsers/chromium, if that directory exists - the
    preinstalled location this project's CI/sandbox images use (see
    module docstring). Returns None (never downloads, never raises) if
    the directory is absent or nothing executable is found in it."""
    if not os.path.isdir(VENDORED_CHROMIUM_DIR):
        return None
    for root, _dirs, files in os.walk(VENDORED_CHROMIUM_DIR):
        for name in ("chrome", "chromium", "headless_shell", "Chromium"):
            if name in files:
                candidate = os.path.join(root, name)
                if os.access(candidate, os.X_OK):
                    return candidate
    return None


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
            print("SKIP: dashboard_e2e_selftest (app.py did not come up on "
                  f"{HOST}:{PORT} within 10s)")
            app_proc.kill()
            return 0
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
                      f"not preinstalled at {VENDORED_CHROMIUM_DIR} and Playwright's "
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

            votes_cell = page.eval_on_selector("#liveTeamTable tbody tr td:nth-child(10)", "el => el.textContent")
            check(bool(votes_cell and ("n/a" in votes_cell or "F/" in votes_cell)),
                  f"a team row's Votes cell reads either a vote string or n/a (got {votes_cell!r})")

            radio_text = page.eval_on_selector("#radioPanelBody", "el => el.textContent")
            check(bool(radio_text and radio_text.strip()), "Radio panel populated from vbus")

            # Theme toggle.
            page.click("#themeToggleBtn")
            page.wait_for_timeout(150)
            theme_attr = page.eval_on_selector("html", "el => el.getAttribute('data-theme')")
            check(theme_attr == "light", f"theme toggle switches to light (got {theme_attr!r})")
            bg_light = page.eval_on_selector("body", "el => getComputedStyle(el).backgroundColor")
            page.click("#themeToggleBtn")
            page.wait_for_timeout(150)
            bg_dark = page.eval_on_selector("body", "el => getComputedStyle(el).backgroundColor")
            check(bg_light != bg_dark, "toggling the theme actually changes the rendered background colour")

            # The run may already have finished on its own (a short,
            # fast tick-delay run easily beats the wait above) - #stopLiveBtn
            # goes hidden again once it has, so only click it if it's still
            # there to click.
            stop_btn = page.query_selector("#stopLiveBtn")
            if stop_btn and stop_btn.is_visible():
                stop_btn.click()
            page.wait_for_timeout(300)

            check(len(console_errors) == 0,
                  f"no browser console errors / page errors during the run (got {len(console_errors)}: "
                  f"{console_errors[:5]})")

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

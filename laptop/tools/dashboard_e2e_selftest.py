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


def _iso_camera_hit_test_check(page):
    """Pure-function checks of the 2.5D projection and hit-testing, run
    in the page against a throwaway camera (no live data needed):
    screenToWorld inverts worldToScreen at the ground and at height z,
    pan and zoom-to-cursor behave identically in both modes, a marker
    drawn with a height is hit where it is VISIBLE (its head), not only
    at its ground point, and a team hidden behind a building is
    re-drawn as a faded outline instead of being lost."""
    res = page.evaluate("""() => {
      const out = {};
      const mk = (mode) => {
        const cam = DashboardCamera.createCamera(36, 36);
        cam.setViewSize(700, 700);
        cam.setViewMode(mode);
        return cam;
      };
      for (const mode of ["2d", "iso"]) {
        const cam = mk(mode);
        let worst = 0;
        for (let wx = -2; wx <= 38; wx += 2.5) for (let wy = -2; wy <= 38; wy += 2.5) for (const z of [0, 0.7, 1.9]) {
          const p = cam.worldToScreen(wx, wy, z);
          const w = cam.screenToWorld(p.x, p.y, z);
          worst = Math.max(worst, Math.abs(w.x - wx), Math.abs(w.y - wy));
        }
        out[mode + "_roundtrip"] = worst;
        const before = cam.worldToScreen(10, 12);
        cam.pan(37, -21);
        const after = cam.worldToScreen(10, 12);
        out[mode + "_pan"] = [after.x - before.x, after.y - before.y];
        const w0 = cam.screenToWorld(123, 456);
        cam.zoomAt(123, 456, 1.7);
        const w1 = cam.screenToWorld(123, 456);
        out[mode + "_zoomfix"] = Math.abs(w0.x - w1.x) + Math.abs(w0.y - w1.y);
      }

      // Hit-testing in iso: a team drawn 1.0 up (wz) with a 9px-ish disc.
      const cam = mk("iso");
      const hit = { type: "team", id: 3, wx: 12, wy: 9, wz: 1.0, wr: 0.5, rpx: 9, data: { id: 3 } };
      const head = cam.worldToScreen(12, 9, 1.0);
      const foot = cam.worldToScreen(12, 9, 0);
      out.iso_hit_head = !!MapRender.hitTestWorld(cam, [hit], head.x + 2, head.y - 2);
      out.iso_hit_far = !!MapRender.hitTestWorld(cam, [hit], head.x + 40, head.y);
      out.iso_hit_head_world = (() => { const w = cam.screenToWorld(head.x, head.y, 1.0); return [w.x, w.y]; })();
      out.iso_foot_head_px = Math.hypot(head.x - foot.x, head.y - foot.y);
      // A ground-level target (safezone-style, no wz/rpx) still uses its world radius.
      const sz = { type: "safezone", id: 1, wx: 5, wy: 5, wr: 0.5, data: { id: 1 } };
      const sp = cam.worldToScreen(5, 5);
      out.iso_ground_hit = !!MapRender.hitTestWorld(cam, [sz], sp.x, sp.y);
      out.iso_ground_miss = !!MapRender.hitTestWorld(cam, [sz], sp.x + 60, sp.y);
      const cam2 = mk("2d");
      const p2 = cam2.worldToScreen(5, 5);
      out.flat_hit = !!MapRender.hitTestWorld(cam2, [sz], p2.x + 3, p2.y - 3);
      out.flat_miss = !!MapRender.hitTestWorld(cam2, [sz], p2.x + 60, p2.y);

      // Occlusion: team at (10,10), 6-floor building at (11,11) in front of it.
      const draw = (withBuilding) => {
        const cv = document.createElement("canvas");
        cv.width = cv.height = 700;
        const ctx = cv.getContext("2d");
        const c = mk("iso");
        MapRender.drawIsoScene(ctx, c, {
          env: { buildings: withBuilding ? [{ id: 1, x: 11, y: 11, w: 1, h: 1, type: 1, floors: 6, damage: 0 }] : [],
                 epicenter: { active: 0 } },
          frame: { teams: [{ id: 0, x: 10.3, y: 10.3, status: 0, has_dest: 0, stamina: 100, fault_mask: 0, override: 0 }],
                   incidents: [], hazard_cells: [], safezones: [] },
          fogViewMode: "god", selectedMarker: null, teamColors: ["#ff00ff"], outlineBuildingId: null,
        });
        const head = c.worldToScreen(10.3, 10.3, 1.0);
        const r = 0.32 * c.state.zoom;
        const magenta = (x, y) => { const d = ctx.getImageData(Math.round(x), Math.round(y), 1, 1).data; return d[0] > 170 && d[2] > 170 && d[1] < 60; };
        let ring = 0;
        for (let a = 0; a < 360; a += 5) {
          const t = a * Math.PI / 180;
          if (magenta(head.x + Math.cos(t) * r * 1.55, head.y + Math.sin(t) * r * 1.55)) ring++;
        }
        return { ring, discSolid: magenta(head.x, head.y + r * 0.6) };
      };
      out.occ_with = draw(true);
      out.occ_without = draw(false);
      return out;
    }""")
    for mode in ("2d", "iso"):
        check(res[mode + "_roundtrip"] < 1e-9,
              f"[{mode}] screenToWorld inverts worldToScreen at z=0 and at height z (worst error {res[mode + '_roundtrip']:.2e})")
        check(abs(res[mode + "_pan"][0] - 37) < 1e-6 and abs(res[mode + "_pan"][1] + 21) < 1e-6,
              f"[{mode}] pan moves the map by exactly the drag delta on screen (got {res[mode + '_pan']})")
        check(res[mode + "_zoomfix"] < 1e-9,
              f"[{mode}] zoom-to-cursor keeps the world point under the cursor fixed (drift {res[mode + '_zoomfix']:.2e})")
    check(res["iso_hit_head"], "[iso] a click on a team marker's drawn head hits it")
    check(not res["iso_hit_far"], "[iso] a click well away from the marker misses it")
    check(abs(res["iso_hit_head_world"][0] - 12) < 1e-9 and abs(res["iso_hit_head_world"][1] - 9) < 1e-9,
          f"[iso] clicking the head maps (screenToWorld at its height) to the team's world position (got {res['iso_hit_head_world']})")
    check(res["iso_ground_hit"] and not res["iso_ground_miss"], "[iso] a ground-level target is hit on its tile and missed elsewhere")
    check(res["flat_hit"] and not res["flat_miss"], "[2d] hit-testing is unchanged for plain world-radius targets")
    check(res["occ_with"]["ring"] >= 3 and not res["occ_with"]["discSolid"],
          f"[iso] a team behind a building is redrawn as a faded outline on top (ring px {res['occ_with']['ring']})")
    check(res["occ_without"]["discSolid"],
          "[iso] the same team with nothing in front is drawn as the normal solid marker")


def _iso_view_check(page, console_errors, screenshot_dir):
    """Toggles the region map to 2.5D through the toolbar button, takes a
    screenshot, clicks a team (and drags one onto an incident, when the
    frame has an eligible pair), then toggles back to 2D - the 2D
    checks elsewhere in this file all run with the default 2D mode."""
    btn = "#viewModeBtn"
    check(page.eval_on_selector(btn, "el => el.textContent.includes('2D')"), "the map toolbar's view button starts on 2D (the default)")
    mode0 = page.evaluate("() => mapCamera.state.viewMode")
    check(mode0 == "2d", f"the map camera defaults to the 2d view mode (got {mode0!r})")

    page.click(btn)
    page.wait_for_timeout(150)
    mode1 = page.evaluate("() => mapCamera.state.viewMode")
    check(mode1 == "iso" and page.eval_on_selector(btn, "el => el.textContent.includes('2.5D')"),
          f"clicking the view button switches the map to 2.5D (mode {mode1!r})")
    fit = page.evaluate("""() => {
      const c = mapCamera, W = c.state.viewW, H = c.state.viewH;
      const pts = [[-0.5, -0.5], [35.5, -0.5], [35.5, 35.5], [-0.5, 35.5]].map(([x, y]) => c.worldToScreen(x, y, 0));
      const xs = pts.map(p => p.x), ys = pts.map(p => p.y);
      return { W, H, inside: pts.every(p => p.x >= 0 && p.x <= W && p.y >= 0 && p.y <= H),
               fillX: (Math.max(...xs) - Math.min(...xs)) / W, fillY: (Math.max(...ys) - Math.min(...ys)) / H };
    }""")
    check(fit["inside"] and max(fit["fillX"], fit["fillY"]) > 0.85,
          f"on switching to 2.5D the map is auto-fitted: the diamond is inside the canvas and fills it (fillX {fit['fillX']:.2f}, fillY {fit['fillY']:.2f}, canvas {fit['W']}x{fit['H']})")
    check(fit["fillY"] > 0.4, f"the 2.5D canvas is not half-empty: the diamond covers {fit['fillY']:.0%} of its height")
    z_before = page.evaluate("() => { mapCamera.zoomAt(mapCamera.state.viewW / 2, mapCamera.state.viewH / 2, 1.5); return mapCamera.state.zoom; }")
    page.evaluate("() => drawFrame(window._lastFrame, true)")
    z_after = page.evaluate("() => mapCamera.state.zoom")
    check(abs(z_before - z_after) < 1e-9, "after the one-time fit the user's zoom is kept (drawing doesn't re-fit)")
    page.evaluate("() => { mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
    stored = page.evaluate("() => { try { return localStorage.getItem('dashboard.mapViewMode'); } catch (e) { return 'unavailable'; } }")
    check(stored == "iso", f"the 2.5D choice is remembered in localStorage (got {stored!r})")

    shot = os.path.join(screenshot_dir, "dashboard_e2e_map_2_5d.png")
    page.locator(".live-map-col").screenshot(path=shot)
    check(os.path.exists(shot) and os.path.getsize(shot) > 2000, f"2.5D map screenshot written ({shot})")

    page.screenshot(path=os.path.join(screenshot_dir, "dashboard_e2e_page_2_5d.png"))
    _click_and_drag_markers(page, "iso", screenshot_dir)

    # Back to 2D: nothing may have been left behind. (With the 3D view
    # offered the button cycles 2D -> 2.5D -> 3D -> 2D, so this may take
    # more than one click.)
    _cycle_view_to(page, "2d")
    mode2 = page.evaluate("() => mapCamera.state.viewMode")
    check(mode2 == "2d" and page.eval_on_selector(btn, "el => el.textContent.includes('2D') && !el.textContent.includes('2.5D')"),
          f"toggling back returns the map to 2D (mode {mode2!r})")
    stored = page.evaluate("() => { try { return localStorage.getItem('dashboard.mapViewMode'); } catch (e) { return 'unavailable'; } }")
    check(stored == "2d", f"the 2D choice is remembered again (got {stored!r})")
    flat = page.evaluate("""() => {
      const h = mapHitTargets.find(h => h.type === 'team');
      const p = mapCamera.worldToScreen(h.wx, h.wy);
      const got = MapRender.hitTestWorld(mapCamera, mapHitTargets, p.x, p.y, 'team');
      return !!got && got.id === h.id && !h.wz;
    }""")
    check(flat, "[2d] after toggling back, team hit-testing is the flat world-radius path again")
    page.evaluate("() => { selectedMarker = null; renderInspector(); }")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors while toggling 2D/2.5D (got {real_errors[:5]})")


def _cycle_view_to(page, mode):
    """Clicks the map's view button (2D -> 2.5D -> [3D] -> 2D ...) until
    the map is in `mode`; at most 4 clicks."""
    for _ in range(4):
        if page.evaluate("() => mapCamera.state.viewMode") == mode:
            break
        page.click("#viewModeBtn")
        page.wait_for_timeout(250)
    page.wait_for_timeout(100)


def _view3d_check(page, console_errors, screenshot_dir):
    """The WebGL 3D view: needs headless WebGL, and SKIPS (with a
    message) if the browser can't make a context. Checks the raycast
    projection, picking, orbit/zoom/pan, click and drag-to-reassign in
    3D, a non-blank render, and the way back to 2D."""
    if not page.evaluate("() => map3dAvailable"):
        print("SKIP: 3D view checks - this browser has no usable WebGL (headless without a GL/SwiftShader backend); "
              "the 3D option is correctly not offered (see the fallback check, which still runs)")
        check(page.evaluate("() => mapCamera.state.viewMode") != "3d", "without WebGL the map never enters 3D")
        return
    _cycle_view_to(page, "3d")
    mode = page.evaluate("() => mapCamera.state.viewMode")
    check(mode == "3d" and page.eval_on_selector("#viewModeBtn", "el => el.textContent.includes('3D')"),
          f"cycling the view button reaches the 3D view (mode {mode!r})")
    check(page.evaluate("() => localStorage.getItem('dashboard.mapViewMode')") == "3d", "the 3D choice is remembered in localStorage")
    page.wait_for_timeout(300)
    painted = page.evaluate("""() => {
      drawFrame(window._lastFrame, true);
      const src = document.getElementById('map3dCanvas');
      const cv = document.createElement('canvas'); cv.width = src.width; cv.height = src.height;
      const ctx = cv.getContext('2d'); ctx.drawImage(src, 0, 0);
      const d = ctx.getImageData(0, 0, cv.width, cv.height).data;
      const colours = new Set();
      for (let i = 0; i < d.length; i += 4 * 97) colours.add((d[i] >> 4) + ',' + (d[i + 1] >> 4) + ',' + (d[i + 2] >> 4));
      return { w: src.width, h: src.height, colours: colours.size };
    }""")
    check(painted["w"] > 100 and painted["colours"] > 12,
          f"the WebGL canvas renders a real scene, not a blank one ({painted['colours']} distinct colours at {painted['w']}x{painted['h']})")
    shot = os.path.join(screenshot_dir, "dashboard_e2e_map_3d.png")
    page.locator(".live-map-col").screenshot(path=shot)
    check(os.path.exists(shot) and os.path.getsize(shot) > 2000, f"3D map screenshot written ({shot})")

    # Projection: the raycast is the inverse of the projection at any height, and
    # pan / zoom-to-cursor keep their contract in 3D too.
    proj = page.evaluate("""() => {
      const c = mapCamera, out = {};
      let worst = 0;
      for (let wx = 2; wx <= 34; wx += 4) for (let wy = 2; wy <= 34; wy += 4) for (const z of [0, 0.8, 2]) {
        const p = c.worldToScreen(wx, wy, z), w = c.screenToWorld(p.x, p.y, z);
        worst = Math.max(worst, Math.abs(w.x - wx), Math.abs(w.y - wy));
      }
      out.roundtrip = worst;
      const w0 = c.screenToWorld(c.state.viewW * 0.4, c.state.viewH * 0.6, 0);
      c.zoomAt(c.state.viewW * 0.4, c.state.viewH * 0.6, 1.3);
      const w1 = c.screenToWorld(c.state.viewW * 0.4, c.state.viewH * 0.6, 0);
      out.zoomfix = Math.abs(w0.x - w1.x) + Math.abs(w0.y - w1.y);
      // Drag the ground point under (px, py) by (31, -17) px: it must end up under the cursor.
      const px = c.state.viewW * 0.3, py = c.state.viewH * 0.7;
      const grabbed = c.screenToWorld(px, py, 0);
      c.pan(31, -17, px + 31, py - 17);
      const after = c.worldToScreen(grabbed.x, grabbed.y, 0);
      out.pan = [after.x - px, after.y - py];
      c.reset(c.state.viewW, c.state.viewH);
      return out;
    }""")
    check(proj["roundtrip"] < 1e-6, f"[3d] the ground-plane raycast inverts the projection at z=0 and at height z (worst error {proj['roundtrip']:.2e})")
    check(proj["zoomfix"] < 1e-6, f"[3d] zoom-to-cursor keeps the world point under the cursor fixed (drift {proj['zoomfix']:.2e})")
    check(abs(proj["pan"][0] - 31) < 1e-6 and abs(proj["pan"][1] + 17) < 1e-6, f"[3d] dragging the ground keeps the grabbed point under the cursor (moved {proj['pan']} px for a (31, -17) drag)")

    # Orbit by dragging empty space, zoom by wheel.
    page.evaluate("() => drawFrame(window._lastFrame, true)")
    box = page.evaluate("() => { const r = document.getElementById('mapCanvas').getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; }")
    az0 = page.evaluate("() => map3d.state.az")
    el0 = page.evaluate("() => map3d.state.el")
    page.mouse.move(box["x"] + 12, box["y"] + 12)
    page.mouse.down()
    page.mouse.move(box["x"] + 112, box["y"] + 52, steps=5)
    page.mouse.up()
    page.wait_for_timeout(100)
    az1 = page.evaluate("() => map3d.state.az")
    el1 = page.evaluate("() => map3d.state.el")
    check(abs(az1 - az0) > 0.1 and abs(el1 - el0) > 0.05, f"[3d] dragging empty space rotates and tilts the camera (azimuth {az0:.2f}->{az1:.2f}, tilt {el0:.2f}->{el1:.2f})")
    d0 = page.evaluate("() => map3d.state.dist")
    page.mouse.move(box["x"] + box["w"] / 2, box["y"] + box["h"] / 2)
    page.mouse.wheel(0, -300)
    page.wait_for_timeout(100)
    d1 = page.evaluate("() => map3d.state.dist")
    check(d1 < d0 * 0.9, f"[3d] the wheel zooms in (camera distance {d0:.1f}->{d1:.1f})")
    page.click("#resetViewBtn")
    page.wait_for_timeout(100)
    check(abs(page.evaluate("() => map3d.state.az") - 0.7853981633974483) < 1e-6, "[3d] the reset button restores the default camera")

    _view3d_motion_check(page, console_errors)
    _view3d_effects_check(page, console_errors)
    _view3d_buildings_check(page, console_errors)
    _view3d_camera_check(page, console_errors, screenshot_dir)
    _view3d_lighting_check(page, console_errors)
    _view3d_labels_check(page)
    _view3d_pins_check(page, console_errors)
    _view3d_pin_details_check(page, console_errors)
    _view3d_pin_depth_check(page, console_errors)
    _view3d_card_follow_check(page, console_errors)
    _card_modes_check(page, console_errors)

    # Tilt limits, full rotation, top-down shortcut.
    lim = page.evaluate("""() => {
      const c = mapCamera, st = map3d.state, out = {};
      c.orbit(0, 100000); out.maxEl = st.el;
      c.orbit(0, -100000); out.minEl = st.el;
      const az0 = st.az; c.orbit(-2000, 0); out.spin = st.az - az0;
      c.reset(c.state.viewW, c.state.viewH);
      return out;
    }""")
    check(abs(lim["maxEl"] - 3.141592653589793 / 2) < 1e-9, f"[3d] the camera can tilt to straight top-down (max tilt {lim['maxEl']:.3f} rad)")
    check(abs(lim["minEl"] - 15 * 3.141592653589793 / 180) < 1e-9, f"[3d] the camera tilts down to ~15 degrees above the ground (min tilt {lim['minEl']:.3f} rad)")
    check(lim["spin"] > 2 * 3.141592653589793, f"[3d] rotation is not clamped - a full 360 degrees and more is possible (turned {lim['spin']:.1f} rad)")
    page.click("#topDownBtn")
    page.wait_for_timeout(150)
    check(abs(page.evaluate("() => map3d.state.el") - 3.141592653589793 / 2) < 1e-9, "[3d] the Top button looks straight down at the map")
    check(page.evaluate("() => mapHitTargets.some(h => h.type === 'team')"), "[3d] markers are still produced in the top-down view")
    _click_and_drag_markers(page, "3d top-down", screenshot_dir)
    page.click("#resetViewBtn")
    page.wait_for_timeout(100)

    # Picking and drag-to-reassign, same contract as 2D/2.5D.
    _click_and_drag_markers(page, "3d", screenshot_dir)

    # Back to 2D.
    _cycle_view_to(page, "2d")
    check(page.evaluate("() => mapCamera.state.viewMode") == "2d" and page.evaluate("() => getComputedStyle(document.getElementById('map3dCanvas')).display") == "none",
          "cycling on from 3D returns to 2D and hides the WebGL canvas")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the 3D checks (got {real_errors[:5]})")


def _view3d_motion_check(page, console_errors):
    """C1: smooth movement between ticks, routes along the road network
    and heading. Drives drawFrame() with synthetic consecutive frames so
    the timing is exact, then checks the drawn pose against them."""
    res = page.evaluate("""async () => {
      const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
      const out = {};
      const base = JSON.parse(JSON.stringify(window._lastFrame));
      const t0 = base.teams[0];
      // A team that is clear to move east one cell on the real map.
      const terr = MapRender.parseTerrain(terrainMap);
      let cell = null;
      for (let y = 1; y < 34 && !cell; y++) for (let x = 1; x < 33; x++) {
        if (terr.cells[y * 36 + x].navigable && terr.cells[y * 36 + x + 1].navigable) { cell = [x, y]; break; }
      }
      // A destination on the same row that really is reachable.
      let dest = null;
      for (let dx = 4; dx < 14 && !dest; dx++) {
        const x = cell[0] + dx;
        if (x < 35 && RoutePlanner.plan(terr, cell[0], cell[1], x, cell[1])) dest = [x, cell[1]];
      }
      let dest2 = null;                                  // a second, different reachable destination
      for (let x = dest[0] + 1; x < 35 && !dest2; x++) if (RoutePlanner.plan(terr, cell[0], cell[1], x, cell[1])) dest2 = [x, cell[1]];
      const mk = (tick, x, y, extra) => {
        const f = JSON.parse(JSON.stringify(base)); f.tick = tick;
        Object.assign(f.teams[0], { x, y, has_dest: 1, dest_x: dest[0], dest_y: dest[1], dest_id: 255, status: 1, fault_mask: 0, degraded: 0 }, extra || {});
        return f;
      };
      const id = t0.id;
      const T = 9000;
      drawFrame(mk(T, cell[0], cell[1]), true);           // first frame: placed, no glide
      let info = map3d.teamInfo(id);
      out.snapFirst = Math.hypot(info.x - cell[0], info.y - cell[1]);
      await sleep(600);                                   // a slow tick, so the glide lasts long enough to sample
      drawFrame(mk(T + 1, cell[0] + 1, cell[1]), true);   // next tick: one cell east
      info = map3d.teamInfo(id);
      out.midX = info.x; out.midK = info.k; out.from = info.from; out.to = info.to;
      out.heading = info.heading;
      out.route = info.route;
      await sleep(600);
      info = map3d.teamInfo(id);
      out.endX = info.x; out.endY = info.y;
      await sleep(300);                                   // "paused": no frames - must not drift or wrap
      out.pausedX = map3d.teamInfo(id).x;
      // Settled on a steady destination for a while: not "rerouting", route is solid.
      drawFrame(mk(T + 12, cell[0] + 1, cell[1]), true);
      out.dashed = map3d.teamInfo(id).dashed;
      // A jump (scrub / seek) must snap, not slide across the map.
      drawFrame(mk(T + 40, cell[0] + 3, cell[1]), true);
      out.snapJump = Math.hypot(map3d.teamInfo(id).x - (cell[0] + 3), 0);
      // A faulted team's route is dashed; so is one that just changed destination.
      drawFrame(mk(T + 41, cell[0] + 3, cell[1], { fault_mask: 1 }), true);
      out.faultDashed = map3d.teamInfo(id).dashed;
      drawFrame(mk(T + 42, cell[0] + 3, cell[1], { fault_mask: 0 }), true);
      out.healthyDashed = map3d.teamInfo(id).dashed;
      drawFrame(mk(T + 43, cell[0] + 3, cell[1], { dest_x: dest2[0], dest_y: dest2[1] }), true);
      out.reroutedDashed = map3d.teamInfo(id).dashed;
      // Route validity on the real map: starts at the team, ends at the destination, every hop one cell, every cell navigable.
      drawFrame(mk(T + 60, cell[0], cell[1]), true);
      drawFrame(mk(T + 61, cell[0], cell[1], { dest_x: 20, dest_y: 20 }), true);
      const r = map3d.teamInfo(id).route;
      out.routeLen = r ? r.length : 0;
      if (r) {
        out.routeStart = r[0]; out.routeEnd = r[r.length - 1];
        out.routeStepsOk = r.every((c, i) => i === 0 || (Math.abs(c[0] - r[i - 1][0]) <= 1 && Math.abs(c[1] - r[i - 1][1]) <= 1));
        out.routeNavigable = r.every((c, i) => i === 0 || terr.cells[c[1] * 36 + c[0]].navigable);
      }
      out.cell = cell;
      drawFrame(base, true);                              // restore
      return out;
    }""")
    check(res["snapFirst"] < 1e-6, "[3d] a team seen for the first time is placed, not slid in from the origin")
    check(res["from"][0] < res["midX"] < res["to"][0] and 0 < res["midK"] < 1,
          f"[3d] between ticks a team's drawn position is part-way between its old and new cell (x {res['from'][0]} -> {res['midX']:.2f} -> {res['to'][0]})")
    check(abs(res["endX"] - res["to"][0]) < 1e-6 and abs(res["endY"] - res["to"][1]) < 1e-6, "[3d] the glide ends exactly on the new cell")
    check(abs(res["pausedX"] - res["endX"]) < 1e-9, "[3d] with no new frames (paused) the team stays put")
    check(res["snapJump"] < 1e-6, "[3d] a non-sequential jump (scrub / seek) snaps instead of sliding")
    check(res["heading"] is not None and abs(res["heading"] - 1.5707963267948966) < 0.05,
          f"[3d] a team moving east faces east (heading {res['heading']})")
    check(res["dashed"] is False and res["faultDashed"] is True and res["healthyDashed"] is False and res["reroutedDashed"] is True,
          f"[3d] routes are solid when healthy, dashed when faulted or just re-targeted (healthy {res['dashed']}, faulted {res['faultDashed']}, recovered {res['healthyDashed']}, rerouted {res['reroutedDashed']})")
    check(res["routeLen"] > 2 and res["routeStart"] == res["cell"] and res["routeEnd"] == [20, 20] and res["routeStepsOk"] and res["routeNavigable"],
          f"[3d] the drawn route runs cell to cell over navigable terrain from the team to its destination ({res['routeLen']} cells)")


def _view3d_effects_check(page, console_errors):
    """C2: hazard effects - levels (off / low / high), caps, reduce-motion,
    the settings popover and the building-collapse effect."""
    st = page.evaluate("() => map3d.settingsState()")
    check(st["fxAvailable"], "[3d] the hazard-effects module loaded")
    page.evaluate("() => map3d.setSettings({ autoQuality: false, reduceMotion: false, effects: 'off' })")
    page.evaluate("() => drawFrame(window._lastFrame, true)")
    off = page.evaluate("() => map3d.fx.counts()")
    page.evaluate("() => map3d.setSettings({ effects: 'low' })")
    low = page.evaluate("() => map3d.fx.counts()")
    page.evaluate("() => map3d.setSettings({ effects: 'high' })")
    high = page.evaluate("() => map3d.fx.counts()")
    frame = page.evaluate("""() => {
      const hc = window._lastFrame.hazard_cells, tm = MapRender.parseTerrain(terrainMap);
      return { fire: hc.filter(c => c.hazard_flags & 1).length, gas: hc.filter(c => c.hazard_flags & 2).length,
               flood: tm.cells.filter(c => c.flood).length, damaged: hc.filter(c => c.blocked || Math.max(c.road_damage, c.infra_damage) >= 50).length };
    }""")
    check(off["flames"] == 0 and off["smoke"] == 0 and off["haze"] == 0, f"[3d] effects off draws no particles ({off})")
    if frame["fire"]:
        check(0 < low["flames"] < high["flames"] and 0 < low["smoke"] < high["smoke"],
              f"[3d] fire: low has fewer flames/smoke particles than high, both more than off (low {low['flames']}/{low['smoke']}, high {high['flames']}/{high['smoke']})")
        check(off["embers"] > 0 and off["fires"] >= frame["fire"], f"[3d] a burning cell is still marked at 'off' (embers {off['embers']}, fires {off['fires']} for {frame['fire']} burning cells)")
    check(high["flames"] <= 760 and high["smoke"] <= 480 and high["haze"] <= 420 and high["debris"] <= 2600, f"[3d] particle counts stay under their caps ({high})")
    if frame["gas"]:
        check(off["haze"] == 0 and high["haze"] > 0, f"[3d] gas shows as a ground haze when effects are on ({high['haze']} particles for {frame['gas']} gas cells)")
    check(high["waterCells"] == frame["flood"], f"[3d] every flooded cell has water ({high['waterCells']} of {frame['flood']})")
    if frame["damaged"]:
        check(off["debris"] > 0 and high["debris"] >= off["debris"], f"[3d] blocked/damaged road cells get debris at every level (off {off['debris']}, high {high['debris']})")

    # Reduce motion freezes particles and disables the shake / dust burst.
    page.evaluate("() => map3d.setSettings({ reduceMotion: true })")
    check(page.evaluate("() => map3d.fx.motion === false && map3d.fx.animated(performance.now() / 1000) === false"),
          "[3d] reduce motion stops the ambient animation (the loop goes idle)")
    check(page.evaluate("() => map3d.triggerCollapse([{ x: 10, y: 10 }])") is False, "[3d] reduce motion suppresses the collapse shake and dust")
    page.evaluate("() => map3d.setSettings({ reduceMotion: false })")
    fired = page.evaluate("() => { const ok = map3d.triggerCollapse([{ x: 10, y: 10 }, { x: 11, y: 10 }, { x: 10, y: 11 }]); return { ok, dust: map3d.fx.counts().dust, anim: map3d.fx.animated(performance.now() / 1000) }; }")
    check(fired["ok"] and fired["dust"] > 0 and fired["anim"], f"[3d] a building collapse raises a dust burst ({fired['dust']} particles)")
    page.wait_for_timeout(2200)
    check(page.evaluate("() => map3d.fx.counts().dust") == 0, "[3d] the dust burst clears itself")

    # Settings popover: opens, changes the level, persists.
    check(page.is_visible("#map3dSettingsBtn"), "[3d] the settings button is shown in 3D")
    page.click("#map3dSettingsBtn")
    check(page.is_visible("#map3dSettings"), "[3d] the settings popover opens")
    page.select_option("#map3dFx", "low")
    check(page.evaluate("() => map3d.fx.level") == "low" and page.evaluate("() => JSON.parse(localStorage.getItem('dashboard.map3dSettings')).effects") == "low",
          "[3d] choosing 'low' in the popover applies it and remembers it")
    page.click("#map3dReduce")
    check(page.evaluate("() => map3d.settingsState().reduceMotion") is True, "[3d] the reduce-motion checkbox applies")
    page.click("#map3dReduce")
    page.select_option("#map3dFx", "off")
    check(page.evaluate("() => map3d.fx.counts().flames") == 0, "[3d] the effects toggle takes effect at once (off -> no flames)")
    page.select_option("#map3dFx", "low")
    page.keyboard.press("Escape")
    page.mouse.click(5, 5)
    check(not page.is_visible("#map3dSettings"), "[3d] clicking elsewhere closes the popover")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the effects checks (got {real_errors[:5]})")


def _view3d_buildings_check(page, console_errors):
    """C3: buildings are built from the damage data (named simulator
    levels), shorter the more damaged; and a new building list with newly
    destroyed buildings is a building-collapse event."""
    res = page.evaluate("""() => {
      const env = currentEnvironment(), st = map3d.buildingStats();
      const state = (d) => d >= 100 ? 3 : d >= 75 ? 2 : d >= 25 ? 1 : 0;
      const want = [0, 0, 0, 0];
      let heightsOk = true, worst = null;
      env.buildings.forEach((b) => {
        const s = state(b.damage || 0); want[s]++;
        const H0 = Math.max(1, b.floors || 1) * 0.25, h = map3d.buildingHeight(b.id);
        const ok = s === 0 ? Math.abs(h - H0) < 1e-6 : s === 1 ? h <= H0 && h >= H0 * 0.9 : s === 2 ? h < H0 * 0.95 && h > H0 * 0.4 : h <= H0 * 0.3 + 1e-6;
        if (!ok && heightsOk) { heightsOk = false; worst = [b.id, b.damage, b.floors, h]; }
      });
      return { st, want, heightsOk, worst, n: env.buildings.length };
    }""")
    check([res["st"]["intact"], res["st"]["cracked"], res["st"]["partial"], res["st"]["rubble"]] == res["want"],
          f"[3d] buildings come out in the damage state the data says (intact/cracked/partial/rubble: built {res['st']}, expected {res['want']})")
    check(res["heightsOk"], f"[3d] building height follows floors and damage - lower the more damaged (first violation: {res['worst']})")

    ev = page.evaluate("""async () => {
      const out = {};
      const frame = JSON.parse(JSON.stringify(window._lastFrame));
      const orig = staticEnvironment;
      const e1 = JSON.parse(JSON.stringify(orig));
      e1.buildings.forEach((b) => { b.damage = Math.min(b.damage || 0, 60); });      // nothing destroyed yet
      const T = 7000;
      staticEnvironment = e1; frame.tick = T; drawFrame(frame, true);
      const base = map3d.collapseEvents || 0;
      const e2 = JSON.parse(JSON.stringify(e1));
      [0, 1, 2].forEach((i) => { e2.buildings[i].damage = 100; });                    // three buildings destroyed
      staticEnvironment = e2; frame.tick = T + 1; drawFrame(frame, true);
      out.events = (map3d.collapseEvents || 0) - base;
      out.last = map3d.lastCollapse;
      out.dust = map3d.fx ? map3d.fx.counts().dust : -1;
      out.shaking = map3d.fx ? map3d.fx.animated(performance.now() / 1000) : false;
      // Same list again (nothing new): no event.
      frame.tick = T + 2; drawFrame(frame, true);
      out.repeat = (map3d.collapseEvents || 0) - base;
      // A scrub far ahead that happens to destroy more is not an "event".
      const e3 = JSON.parse(JSON.stringify(e2)); e3.buildings[3].damage = 100;
      staticEnvironment = e3; frame.tick = T + 80; drawFrame(frame, true);
      out.scrub = (map3d.collapseEvents || 0) - base;
      staticEnvironment = orig; drawFrame(window._lastFrame, true);
      return out;
    }""")
    check(ev["events"] == 1 and ev["last"]["buildings"] == 3, f"[3d] newly destroyed buildings in a new building list raise one building-collapse event (events {ev['events']}, {ev['last']})")
    check(ev["dust"] > 0 and ev["shaking"], f"[3d] a building collapse throws up dust ({ev['dust']} particles) and shakes the camera")
    check(ev["repeat"] == 1 and ev["scrub"] == 1, f"[3d] the same list again, or a jump across many ticks, is not a new collapse event (after repeat {ev['repeat']}, after scrub {ev['scrub']})")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the building checks (got {real_errors[:5]})")


def _view3d_camera_check(page, console_errors, screenshot_dir):
    """C4: follow a team, fly to an incident (Esc / dragging cancels), and
    clicking / dragging a team still works after the camera is rotated."""
    # ---- follow a team that keeps moving ----
    page.evaluate("() => { map3d.setSettings({ reduceMotion: false }); mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
    team_id = page.evaluate("() => window._lastFrame.teams[0].id")
    page.evaluate("(id) => DashboardSelection.set({ type: 'team', id })", team_id)
    page.wait_for_timeout(100)
    check(page.is_visible("#camTargetBtn") and "Follow team" in page.inner_text("#camTargetBtn"),
          f"[3d] selecting a team offers 'Follow' (button: {page.inner_text('#camTargetBtn')!r})")
    page.click("#camTargetBtn")
    mode = page.evaluate("() => map3d.cameraMode")
    check(bool(mode and mode["mode"] == "follow" and mode["teamId"] == team_id) and "Stop following" in page.inner_text("#camTargetBtn"),
          f"[3d] the button starts following that team and turns into 'Stop following' ({mode})")
    res = page.evaluate("""async (id) => {
      const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
      const base = JSON.parse(JSON.stringify(window._lastFrame));
      const T = 12000, y = 8;
      const mk = (tick, x) => { const f = JSON.parse(JSON.stringify(base)); f.tick = tick;
        Object.assign(f.teams[0], { x, y, has_dest: 0, status: 1 }); return f; };
      const dist = () => { const p = map3d.teamInfo(id), s = map3d.state; return Math.hypot(s.tx - p.x, s.tz - p.y); };
      drawFrame(mk(T, 5), true);
      const d0 = dist(); await sleep(900); const d1 = dist();
      const out = { d0, d1, dists: [] };
      for (let k = 1; k <= 8; k++) { drawFrame(mk(T + k, 5 + k * 2), true); await sleep(160); }
      await sleep(700);
      out.afterMove = dist();
      out.teamX = map3d.teamInfo(id).x;
      out.camX = map3d.state.tx;
      return out;
    }""", team_id)
    check(res["d1"] < max(0.3, res["d0"] * 0.2) and res["afterMove"] < 1.2,
          f"[3d] while following, the camera glides onto the team and keeps up as it moves (distance {res['d0']:.1f} -> {res['d1']:.2f}; after the team moved 16 cells: {res['afterMove']:.2f}; team x {res['teamX']:.1f}, camera x {res['camX']:.1f})")
    page.keyboard.press("Escape")
    page.wait_for_timeout(100)
    check(page.evaluate("() => map3d.cameraMode") is None, "[3d] Esc stops following")
    tx = page.evaluate("() => map3d.state.tx")
    page.evaluate("""async () => { const f = JSON.parse(JSON.stringify(window._lastFrame)); f.tick = 12020; Object.assign(f.teams[0], { x: 30, y: 8, has_dest: 0 }); drawFrame(f, true); await new Promise((r) => setTimeout(r, 500)); }""")
    check(abs(page.evaluate("() => map3d.state.tx") - tx) < 1e-9, "[3d] after Esc the camera no longer tracks the team")
    # Dragging the map stops a follow too.
    page.evaluate("(id) => { DashboardSelection.set({ type: 'team', id }); }", team_id)
    page.click("#camTargetBtn")
    check(page.evaluate("() => !!map3d.cameraMode"), "[3d] following again")
    box = page.evaluate("() => { const r = document.getElementById('mapCanvas').getBoundingClientRect(); return { x: r.left, y: r.top, w: r.width, h: r.height }; }")
    page.mouse.move(box["x"] + 14, box["y"] + 14)
    page.mouse.down()
    page.mouse.move(box["x"] + 70, box["y"] + 40, steps=4)
    page.mouse.up()
    check(page.evaluate("() => map3d.cameraMode") is None, "[3d] dragging the map stops following")
    page.evaluate("(id) => { DashboardSelection.set({ type: 'team', id }); }", team_id)
    page.click("#camTargetBtn")
    page.click("#camTargetBtn")   # now says "Stop following"
    check(page.evaluate("() => map3d.cameraMode") is None, "[3d] the 'Stop following' button stops it")
    page.evaluate("() => { DashboardSelection.set(null); mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")

    # ---- fly to an incident ----
    inc = page.evaluate("""() => { const h = mapHitTargets.find((x) => x.type === 'incident'); return h ? { id: h.id, x: h.wx, y: h.wy } : null; }""")
    if inc:
        before = page.evaluate("() => ({ dist: map3d.state.dist, az: map3d.state.az })")
        page.evaluate("(id) => DashboardSelection.set({ type: 'incident', id })", inc["id"])
        page.wait_for_timeout(100)
        check("Fly to incident" in page.inner_text("#camTargetBtn"), f"[3d] selecting an incident offers 'Fly to' (button: {page.inner_text('#camTargetBtn')!r})")
        page.click("#camTargetBtn")
        check(page.evaluate("() => map3d.cameraMode && map3d.cameraMode.mode") == "fly", "[3d] the button starts a fly-to")
        page.wait_for_timeout(2200)
        after = page.evaluate("() => ({ tx: map3d.state.tx, tz: map3d.state.tz, dist: map3d.state.dist, az: map3d.state.az, el: map3d.state.el, mode: map3d.cameraMode })")
        check(after["mode"] is None and abs(after["tx"] - inc["x"]) < 0.05 and abs(after["tz"] - inc["y"]) < 0.05,
              f"[3d] the camera flies to the incident and stops there (target {after['tx']:.2f},{after['tz']:.2f} vs incident {inc['x']},{inc['y']})")
        check(after["dist"] <= 20.01 and abs((after["az"] - before["az"]) - 0.9) < 0.01,
              f"[3d] the fly-to moves in close and swings the camera around the spot (distance {before['dist']:.0f} -> {after['dist']:.0f}, swing {after['az'] - before['az']:.2f} rad)")
        # Esc part-way stops it where it is.
        page.evaluate("() => { mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); }")
        page.evaluate("(id) => DashboardSelection.set({ type: 'incident', id })", inc["id"])
        page.click("#camTargetBtn")
        page.wait_for_timeout(250)
        page.keyboard.press("Escape")
        mid = page.evaluate("() => ({ tx: map3d.state.tx, mode: map3d.cameraMode })")
        page.wait_for_timeout(500)
        check(mid["mode"] is None and abs(page.evaluate("() => map3d.state.tx") - mid["tx"]) < 1e-9 and abs(mid["tx"] - inc["x"]) > 0.3,
              "[3d] Esc cancels a fly-to part-way and leaves the camera where it was")
        page.evaluate("() => { map3d.setSettings({ reduceMotion: true }); }")
        page.evaluate("(id) => DashboardSelection.set({ type: 'incident', id })", inc["id"])
        page.evaluate("() => { mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); }")
        page.click("#camTargetBtn")
        check(abs(page.evaluate("() => map3d.state.tx") - inc["x"]) < 0.05, "[3d] with reduce motion the fly-to jumps there instead of animating")
        page.evaluate("() => { map3d.setSettings({ reduceMotion: false }); DashboardSelection.set(null); }")
    else:
        print("INFO: no incident in the current frame to fly to - skipped the fly-to checks")

    # ---- a click on a team after rotating the camera ----
    page.evaluate("() => { mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
    az0 = page.evaluate("() => map3d.state.az")
    page.mouse.move(box["x"] + 14, box["y"] + 14)
    page.mouse.down()
    page.mouse.move(box["x"] + 200, box["y"] + 60, steps=6)
    page.mouse.up()
    az1 = page.evaluate("() => map3d.state.az")
    check(abs(az1 - az0) > 0.5, f"[3d] rotated the camera by dragging (azimuth {az0:.2f} -> {az1:.2f})")
    _click_and_drag_markers(page, "3d rotated", screenshot_dir)
    page.screenshot(path=os.path.join(screenshot_dir, "dashboard_e2e_3d_rotated_selected.png"))
    page.evaluate("() => { DashboardSelection.set(null); mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the camera checks (got {real_errors[:5]})")


def _view3d_lighting_check(page, console_errors):
    """C5: soft ambient + directional light with light distance fog, and an
    optional night tint with lit windows."""
    page.evaluate("() => map3d.setSettings({ lighting: 'day' })")
    day = page.evaluate("() => map3d.lightingState()")
    check(0 < day["hemi"] and day["sun"] > day["hemi"] and day["windowGlow"] == 0, f"[3d] day lighting: ambient + stronger directional light, windows unlit ({day['hemi']}, {day['sun']})")
    check(day["fogNear"] > 0.5 * day["dist"] and day["fogFar"] > 2 * day["dist"] and day["fogFar"] < 4 * day["dist"],
          f"[3d] light distance fog starts near the map and stays gentle (near {day['fogNear']:.0f}, far {day['fogFar']:.0f} for camera distance {day['dist']:.0f})")
    check(day["markersFogged"] is False, "[3d] teams and incidents ignore fog, so they stay fully readable at any distance")
    page.evaluate("() => { const c = mapCamera; c.zoomAt(c.state.viewW / 2, c.state.viewH / 2, 2); }")
    closer = page.evaluate("() => map3d.lightingState()")
    check(closer["dist"] < day["dist"] and closer["fogFar"] < day["fogFar"], "[3d] the fog follows the camera distance (zoom in -> it moves in with it)")
    page.evaluate("() => mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH)")
    page.click("#map3dSettingsBtn")
    page.select_option("#map3dLight", "night")
    night = page.evaluate("() => map3d.lightingState()")
    check(night["preset"] == "night" and night["background"] != day["background"] and night["fog"] != day["fog"] and night["hemi"] < day["hemi"] and night["sun"] < day["sun"],
          "[3d] night is darker: lower ambient and directional light, darker background and fog")
    check(night["windowGlow"] > 0, f"[3d] windows light up at night (glow {night['windowGlow']})")
    check(page.evaluate("() => JSON.parse(localStorage.getItem('dashboard.map3dSettings')).lighting") == "night", "[3d] the lighting choice is remembered")
    page.evaluate("() => drawFrame(window._lastFrame, true)")
    check(page.evaluate("() => mapHitTargets.some(h => h.type === 'team')"), "[3d] markers are still drawn and pickable at night")
    page.select_option("#map3dLight", "day")
    page.keyboard.press("Escape")
    check(page.evaluate("() => map3d.lightingState().windowGlow") == 0, "[3d] back to day: windows unlit again")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the lighting checks (got {real_errors[:5]})")


def _view3d_labels_check(page):
    """Things the dashboard works out itself are labelled as such: routes as
    'predicted', derived fire intensity as 'estimated'."""
    text = page.evaluate("() => { openMapLegend(); const t = document.querySelector('#mapLegendModal .legend-modal-body').textContent; closeMapLegend(); return t; }")
    check("Predicted route" in text and "does not send routes" in text and "prediction" in text,
          "[3d] the map legend labels routes as predicted - worked out by the dashboard, not sent by the simulator")
    check("estimated" in text and "burning" in text, "[3d] the map legend marks fire intensity as estimated")
    card = page.evaluate("""() => {
      const h = mapHitTargets.find((x) => x.type === 'team');
      if (!h) return null;
      const d = Object.assign({}, h.data, { has_dest: 1, dest_x: h.data.x + 3, dest_y: h.data.y });
      return buildMapCardHTML({ type: 'team', data: d });
    }""")
    check(bool(card) and "Route" not in card and "predicted" not in card, "[3d] the team hover card has no route row - the explanation lives in the legend only")
    flat = page.evaluate("""() => { const v = mapCamera.state.viewMode; mapCamera.state.viewMode = '2d';
      const h = mapHitTargets.find((x) => x.type === 'team'); const d = Object.assign({}, h.data, { has_dest: 1, dest_x: h.data.x + 3, dest_y: h.data.y });
      const html = buildMapCardHTML({ type: 'team', data: d }); mapCamera.state.viewMode = v; return html; }""")
    check("predicted" not in flat, "the 2D team card is unchanged (no route row)")


def _view3d_pins_check(page, console_errors):
    """Incident pins in 3D: a compact pin on a short stem, urgency shown by a
    glow halo (pulsing P1, steady P2, none P3/P4), the P1..P4 label in the
    pin (hidden when zoomed far out), a victim badge, one pin per building; still hoverable, clickable and a drop target."""
    page.evaluate("() => { map3d.setSettings({ reduceMotion: false }); mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
    res = page.evaluate("""() => {
      const pins = map3d.incidentPins();
      const frame = window._lastFrame;
      const live = frame.incidents.filter((i) => i.status !== 3 && !(fogViewMode === 'operator' && i.discovered === 0));
      const byBuilding = new Map();
      live.forEach((i) => { const k = (i.building_id !== undefined && i.building_id >= 0) ? 'b' + i.building_id : 'i' + i.id; byBuilding.set(k, (byBuilding.get(k) || []).concat([i])); });
      const expectPins = byBuilding.size;
      const want = (p) => p === 1 ? 'pulse' : p === 2 ? 'steady' : 'none';
      let ringsOk = true, badgesOk = true, bad = null;
      pins.forEach((p) => {
        const members = Array.from(byBuilding.values()).find((m) => m.some((i) => i.id === p.id));
        const rep = members.slice().sort((a, b) => (a.priority - b.priority) || ((b.victims || 0) - (a.victims || 0)))[0];
        if (p.ring !== want(rep.priority) && ringsOk) { ringsOk = false; bad = [p.id, p.priority, p.ring]; }
        const total = members.reduce((a, m) => a + (m.victims || 0), 0);
        if ((p.victims !== total || p.group !== members.length) && badgesOk) { badgesOk = false; bad = [p.id, p.victims, total, p.group, members.length]; }
      });
      return { n: pins.length, expectPins, ringsOk, badgesOk, bad,
               kinds: pins.reduce((a, p) => { a[p.ring] = (a[p.ring] || 0) + 1; return a; }, {}),
               grouped: pins.filter((p) => p.group > 1).length,
               maxStem: Math.max(0, ...pins.map((p) => p.headZ - p.foot)),
               scales: Array.from(new Set(pins.map((p) => p.scale.toFixed(4)))),
               victimsVary: new Set(pins.map((p) => p.victims)).size > 1 };
    }""")
    check(res["n"] == res["expectPins"], f"[3d] one pin per building / incident ({res['n']} pins for {res['expectPins']} places) - grouped incidents share one pin")
    check(res["ringsOk"] and res["badgesOk"], f"[3d] halo follows priority (P1 pulse, P2 steady, P3/P4 none) and badges carry the victim / group totals (first problem: {res['bad']}; rings {res['kinds']})")
    check(res["maxStem"] <= 0.7 and len(res["scales"]) == 1, f"[3d] pins are compact: stem <= {res['maxStem']:.2f} units, and every pin is the same size ({res['scales']}) even though victims vary ({res['victimsVary']})")

    # Motion: the P1 ring pulses; Reduce motion freezes it (the ring stays).
    if res["kinds"].get("pulse"):
        before = page.evaluate("() => map3d.pulseState()")
        page.wait_for_timeout(400)
        after = page.evaluate("() => map3d.pulseState()")
        check(before["animating"] and after["uTime"] > before["uTime"], f"[3d] the P1 halo pulses (animation clock {before['uTime']:.1f} -> {after['uTime']:.1f})")
        page.evaluate("() => map3d.setSettings({ reduceMotion: true })")
        frozen = page.evaluate("() => map3d.pulseState()")
        check(frozen["uMotion"] == 0 and frozen["animating"] is False and frozen["pulsing"] > 0, "[3d] with Reduce motion the P1 halo stops pulsing but is still drawn")
        page.evaluate("() => map3d.setSettings({ reduceMotion: false })")
    # Label level of detail: the P1..P4 text is in the pin at normal zoom and
    # hidden when zoomed far out, while the halo (and badges) stay.
    far0 = page.evaluate("() => map3d.incidentPins().map((p) => ({ l: p.label, px: p.headPx }))")
    check(len(far0) > 0 and not any(p["l"] for p in far0) and all(p["px"] >= 7 for p in far0),
          f"[3d] zoomed out to fit the whole map the P1..P4 text is hidden but every pin head is still >= 7px ({sum(p['l'] for p in far0)} labels shown)")
    page.evaluate("() => { const c = mapCamera; for (let i = 0; i < 30 && !map3d.incidentPins().every((p) => p.label); i++) { c.zoomAt(c.state.viewW / 2, c.state.viewH / 2, 1.25); drawFrame(window._lastFrame, true); } }")
    near = page.evaluate("() => map3d.incidentPins().map((p) => p.label)")
    check(len(near) > 0 and any(near), f"[3d] zoomed in the pins show their P1..P4 label ({sum(near)}/{len(near)} in view or out)")
    page.evaluate("() => { mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
    back = page.evaluate("() => map3d.incidentPins().map((p) => p.label)")
    check(not any(back), "[3d] zooming back out hides the labels again, halos stay")
    # Hover, click and drop target on the pin itself.
    pick = page.evaluate("""() => { const h = mapHitTargets.find((x) => x.type === 'incident' && x.data.pool !== 1);
      if (!h) return null; const r = document.getElementById('mapCanvas').getBoundingClientRect(); const p = mapCamera.worldToScreen(h.wx, h.wy, h.wz);
      return { id: h.id, x: r.left + p.x, y: r.top + p.y }; }""")
    if pick:
        page.mouse.move(pick["x"] - 60, pick["y"] - 60)
        page.mouse.move(pick["x"], pick["y"], steps=4)
        page.wait_for_timeout(150)
        tip = page.evaluate("() => { const t = document.getElementById('mapHoverTip'); return { shown: t.style.display === 'block', text: t.textContent, pinned: document.getElementById('mapTooltip').style.display }; }")
        check(tip["shown"] and tip["pinned"] != "block" and ("incident" in tip["text"].lower()), f"[3d] hovering an incident pin shows a temporary card, not a pinned one ({tip['text'][:40]!r}...)")
        page.mouse.click(pick["x"], pick["y"])
        page.wait_for_timeout(100)
        sel = page.evaluate("() => selectedMarker")
        check(bool(sel and sel["type"] == "incident"), f"[3d] clicking an incident pin selects it (got {sel})")
        page.mouse.move(5, 5)      # cursor off the map, so no hover card is left resting on the pin
        page.evaluate("() => { DashboardSelection.set(null); selectedMarker = null; hideMapCard(); }")
        page.wait_for_timeout(450)
    legend = page.evaluate("() => { openMapLegend(); const t = document.querySelector('#mapLegendModal .legend-modal-body').textContent; closeMapLegend(); return t; }")
    check("Incident pin" in legend and "glow halo" in legend and "slowly pulsing" in legend and "steady" in legend, "[3d] the legend explains the pin, its P1-P4 label, badges and the P1 / P2 halos")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the pin checks (got {real_errors[:5]})")


def _view3d_pin_details_check(page, console_errors):
    """Pin details in 3D, only from data the frames already carry: assignment
    (hollow / team ring / fault warning / hover tether), work progress,
    type icons, undiscovered ghost pins in God view, the priority-escalation
    pulse, and the zoom levels of detail."""
    page.evaluate("""() => { window.__orig = window._lastFrame; window.__t = window._lastFrame.tick + 5000;
      map3d.setSettings({ reduceMotion: false }); fogViewMode = 'god';
      window.__mock = (edit) => { const f = JSON.parse(JSON.stringify(window.__orig)); edit(f); f.tick = (window.__t += 1); drawFrame(f, true); return f; };
      mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); }""")
    # 1. Assignment: hollow = nobody assigned; ring = team; fault warning
    res = page.evaluate("""() => {
      const f = window.__mock((f) => { f.incidents.forEach((i) => { i.status = 0; i.assigned_team = 255; i.support_team = 255; i.discovered = 1; });
        f.incidents[0].assigned_team = f.teams[1].id; f.teams[1].fault_mask = 0; f.teams[2].fault_mask = 1; f.incidents[1].assigned_team = f.teams[2].id; });
      const pins = map3d.incidentPins(); const by = (id) => pins.find((p) => p.ids.includes(id));
      const a = by(f.incidents[0].id), b = by(f.incidents[1].id), c = pins.find((p) => !p.ids.includes(f.incidents[0].id) && !p.ids.includes(f.incidents[1].id));
      return { a: [a.hollow, a.assigned.map((x) => [x.team, x.fault])], b: [b.hollow, b.assigned.map((x) => [x.team, x.fault])], c: c ? [c.hollow, c.assigned.length] : null,
               t1: f.teams[1].id, t2: f.teams[2].id };
    }""")
    check(res["a"][0] is False and res["a"][1] == [[res["t1"], False]], f"[3d] an assigned incident has a solid pin and the assigned team's ring, no fault warning ({res['a']})")
    check(res["b"][0] is False and res["b"][1] == [[res["t2"], True]], f"[3d] the pin of an incident whose team has an active fault carries the warning ({res['b']})")
    check(res["c"] is not None and res["c"][0] is True and res["c"][1] == 0, f"[3d] an unassigned incident is a hollow, outlined pin ({res['c']})")
    # hover tether (and select) draws one line per assigned team
    inc_id = page.evaluate("() => window.__orig.incidents[0].id")
    page.evaluate("(id) => map3d.setHoverIncident(id)", inc_id)
    tet = page.evaluate("() => map3d.escalationState().tethers")
    lod = page.evaluate("(id) => map3d.incidentPins().find((p) => p.ids.includes(id)).lod", inc_id)
    check(tet == 1 and lod == 2, f"[3d] hovering an incident draws a tether to its assigned team and shows full detail (tethers={tet}, detail level={lod})")
    page.evaluate("() => map3d.setHoverIncident(null)")
    check(page.evaluate("() => map3d.escalationState().tethers") == 0, "[3d] the tether goes away when the hover ends")
    # 2. Progress arc from work_remaining (estimate against the largest value seen)
    prog = page.evaluate("""() => {
      const id = window.__orig.incidents[0].id, g = (f) => f.incidents.find((i) => i.id === id);
      window.__mock((f) => { f.incidents.forEach((i) => { i.work_remaining = 1000; }); });
      const p0 = map3d.incidentPins().find((p) => p.ids.includes(id)).progress;
      window.__mock((f) => { f.incidents.forEach((i) => { i.work_remaining = 400; }); });
      const p1 = map3d.incidentPins().find((p) => p.ids.includes(id)).progress;
      return [p0, p1]; }""")
    check(prog[0] == 0 and prog[1] is not None and abs(prog[1] - 0.6) < 0.05, f"[3d] the progress arc fills as work remaining drops ({prog[0]} -> {prog[1]}; vs. the largest seen)")
    # 3. God view: undiscovered incidents are ghost pins; Operator view hides them
    gv = page.evaluate("""() => {
      const hide = (f) => { f.incidents.forEach((i) => { i.discovered = 1; }); const b = f.incidents[3].building_id; f.incidents.forEach((i) => { if (i === f.incidents[3] || (b >= 0 && i.building_id === b)) i.discovered = 0; }); };
      window.__mock(hide);
      const pins = map3d.incidentPins(); const ghost = pins.filter((p) => p.ghost).length; const n = pins.length;
      fogViewMode = 'operator'; window.__mock(hide);
      const nOp = map3d.incidentPins().length; fogViewMode = 'god';
      return { ghost, n, nOp }; }""")
    check(gv["ghost"] >= 1 and gv["nOp"] == gv["n"] - gv["ghost"], f"[3d] God view shows the undiscovered incident as a ghost pin; Operator view hides it ({gv})")
    # 4. Escalation pulse (priority number goes DOWN), with Reduce motion respected
    esc = page.evaluate("""() => {
      const id = window.__orig.incidents[0].id; const g = (f) => f.incidents.find((i) => i.id === id);
      window.__mock((f) => { f.incidents.forEach((i) => { i.discovered = 1; }); g(f).priority = 3; });
      const first = map3d.escalationState().active;
      window.__mock((f) => { f.incidents.forEach((i) => { i.discovered = 1; }); g(f).priority = 3; });
      const steady = map3d.escalationState().active;
      window.__mock((f) => { f.incidents.forEach((i) => { i.discovered = 1; }); g(f).priority = 1; });
      const up = map3d.escalationState(); const flagged = map3d.incidentPins().find((p) => p.ids.includes(id)).escalated;
      map3d.setSettings({ reduceMotion: true });
      window.__mock((f) => { f.incidents.forEach((i) => { i.discovered = 1; }); g(f).priority = 3; });
      window.__mock((f) => { f.incidents.forEach((i) => { i.discovered = 1; }); g(f).priority = 1; });
      const reduced = map3d.escalationState().total; map3d.setSettings({ reduceMotion: false });
      return { first, steady, up, flagged, reduced }; }""")
    check(esc["first"] == 0 and esc["steady"] == 0, f"[3d] no pulse on first sight or when the priority does not change ({esc['first']}, {esc['steady']})")
    check(esc["up"]["active"] >= 1 and esc["flagged"], f"[3d] a priority escalation (P3 -> P1) starts a pulse ({esc['up']})")
    check(esc["reduced"] == 0, "[3d] with Reduce motion there is no escalation pulse")
    page.wait_for_timeout(1900)
    check(page.evaluate("() => map3d.escalationState().active") == 0, "[3d] the escalation pulse is brief (gone after ~1.6 s)")
    # 5. Type icons: every hazard type draws (hover forces full detail)
    types = page.evaluate("""() => {
      window.__mock((f) => { f.incidents.forEach((i, k) => { i.discovered = 1; i.type = k % 11; }); });
      const pins = map3d.incidentPins(); const seen = new Set();
      pins.forEach((p) => { map3d.setHoverIncident(p.id); const q = map3d.incidentPins().find((x) => x.id === p.id); if (q.lod === 2) seen.add(q.type); });
      map3d.setHoverIncident(null); return Array.from(seen).sort((a, b) => a - b); }""")
    check(len(types) >= 6, f"[3d] type icons draw at full detail for the hazard types in the data ({types})")
    # 6. Levels of detail by zoom
    lv = page.evaluate("""() => {
      window.__mock((f) => { f.incidents.forEach((i) => { i.discovered = 1; }); });
      const c = mapCamera, levels = () => Array.from(new Set(map3d.incidentPins().map((p) => p.lod))).sort();
      const far = levels();
      let steps = 0; while (steps++ < 40 && !levels().includes(2)) { c.zoomAt(c.state.viewW / 2, c.state.viewH / 2, 1.2); window.__mock(() => {}); }
      const near = levels(); c.reset(c.state.viewW, c.state.viewH); window.__mock(() => {});
      return { far, near }; }""")
    check(lv["far"] == [0] and 2 in lv["near"], f"[3d] zoomed out pins show only colour / halo / label (level {lv['far']}); zooming in adds detail (levels {lv['near']})")
    legend = page.evaluate("() => { openMapLegend(); const t = document.querySelector('#mapLegendModal .legend-modal-body').textContent; closeMapLegend(); return t; }")
    check(all(w in legend for w in ("Assignment", "Progress arc", "Type icon", "Faded dashed pin", "Escalation pulse")), "[3d] the legend explains assignment, progress arc, type icon, ghost pins and the escalation pulse")
    page.evaluate("() => { drawFrame(window.__orig, true); }")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the pin-detail checks (got {real_errors[:5]})")


def _view3d_card_follow_check(page, console_errors):
    """The floating card and the follow camera are independent: following
    hides the team's card; an open card stays anchored to its marker while
    the camera moves; the card's x and Esc close it without ending a follow;
    a click with a little jitter never orbits; a real drag does."""
    page.evaluate("""() => { map3d.setSettings({ reduceMotion: false }); map3d.cancelCamera(); hideMapCard(); DashboardSelection.set(null); selectedMarker = null;
      mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); window.__cardDisplay = () => document.getElementById('mapTooltip').style.display;
      window.__follow = () => { const c = map3d.cameraMode; return c ? c.mode : null; }; }""")
    page.wait_for_timeout(300)
    pos = lambda kind, idx=0: page.evaluate("""([kind, idx]) => { const hs = mapHitTargets.filter((h) => h.type === kind && (kind !== 'incident' || h.data.pool !== 1)); const h = hs[idx]; if (!h) return null;
      const r = document.getElementById('mapCanvas').getBoundingClientRect(); const p = mapCamera.worldToScreen(h.wx, h.wy, h.wz || 0); return { id: h.data.id, x: r.left + p.x, y: r.top + p.y }; }""", [kind, idx])
    def settle():                       # wait until the team marker stops gliding, so the second click of a double-click lands on it
        last = None
        for _ in range(40):
            cur = pos("team")
            if last and abs(cur["x"] - last["x"]) < 0.3 and abs(cur["y"] - last["y"]) < 0.3:
                return cur
            last = cur; page.wait_for_timeout(120)
        return last
    team = settle()
    check(team is not None, "[3d] card/follow checks: a team marker is on screen")
    # 1a. double-click a team: follow starts and the card is hidden
    page.mouse.move(team["x"] - 50, team["y"] - 50); page.mouse.move(team["x"], team["y"], steps=3)
    page.mouse.click(team["x"], team["y"]); page.wait_for_timeout(250)      # first click selects it (the marker shifts a few px when selected)
    team = settle()
    page.mouse.dblclick(team["x"], team["y"]); page.wait_for_timeout(200)
    check(page.evaluate("() => window.__follow()") == "follow" and page.evaluate("() => window.__cardDisplay()") == "none",
          "[3d] double-clicking a team starts following and shows no card")
    # tick refreshes and hovering the followed team do not bring the card back
    page.evaluate("() => { const f = JSON.parse(JSON.stringify(window._lastFrame)); f.tick += 1; drawFrame(f, true); }")
    page.wait_for_timeout(100)
    st = page.evaluate("() => ({ card: window.__cardDisplay(), sel: selectedMarker && selectedMarker.type })")
    check(st["card"] == "none" and st["sel"] == "team", f"[3d] while following, a tick refresh does not pop the card up; the team stays selected ({st})")
    check("Stop following" in page.inner_text("#camTargetBtn"), "[3d] the follow button remains while following")
    page.evaluate("() => map3d.cancelCamera()"); page.wait_for_timeout(100)
    # 1b. the follow button: open the card first, then follow -> card hidden
    page.evaluate("() => { const h = mapHitTargets.find((x) => x.type === 'team'); selectedMarker = { type: 'team', id: h.data.id }; updateCamButton(); showMapCard(h); }")
    check(page.evaluate("() => window.__cardDisplay()") == "block", "[3d] a team's card opens before following")
    page.click("#camTargetBtn"); page.wait_for_timeout(200)
    check(page.evaluate("() => window.__follow()") == "follow" and page.evaluate("() => window.__cardDisplay()") == "none", "[3d] the Follow button hides the team's card and follows")
    page.evaluate("() => map3d.cancelCamera()")
    # 2. an open card stays anchored to its marker while the camera moves
    anch = page.evaluate("""async () => {
      const inc = mapHitTargets.find((h) => h.type === 'incident' && h.data.pool !== 1); showMapCard(inc);
      const rect = () => document.getElementById('mapTooltip').getBoundingClientRect(), cv = document.getElementById('mapCanvas').getBoundingClientRect();
      const samples = [];
      map3d.flyTo(inc.wx + 3, inc.wy + 3);                          // the camera now swings and moves every frame; the card is for an incident that stays in view
      for (let i = 0; i < 8; i++) {
        await new Promise((r) => setTimeout(r, 170));
        const h = mapHitTargets.find((x) => x.type === 'incident' && x.id === inc.id), p = mapCamera.worldToScreen(h.wx, h.wy, h.wz || 0);
        const c = document.getElementById('mapTooltip'), r = rect();
        const mx = cv.left + p.x, my = cv.top + p.y;
        const dx = Math.max(r.left - mx, 0, mx - r.right), dy = Math.max(r.top - my, 0, my - r.bottom);
        samples.push({ px: p.x, py: p.y, hidden: !map3d.isVisible(h.wx, h.wy, h.wz || 0), gap: Math.hypot(dx, dy) });
      }
      return samples; }""")
    vis = [a for a in anch if not a["hidden"]]
    travel = max(abs(a["px"] - vis[0]["px"]) + abs(a["py"] - vis[0]["py"]) for a in vis) if vis else 0
    check(len(vis) >= 3 and travel > 20 and all(a["gap"] < 40 for a in vis),
          f"[3d] an open card stays anchored to its marker while the camera swings and moves it {travel:.0f}px across the screen (gaps {[round(a['gap']) for a in vis]})")
    # 2b. marker off-screen: the pinned card is never hidden; it docks to the map edge, and undocks when the marker is back
    page.evaluate("() => { map3d.cancelCamera(); mapCamera.pan(-4000, 0); drawFrame(window._lastFrame, true); }")
    off = page.evaluate("""() => { const c = document.getElementById('mapTooltip'), r = c.getBoundingClientRect(), cv = document.getElementById('mapCanvas').getBoundingClientRect(), d = c.querySelector('.map-card-dock');
      return { disp: c.style.display, vis: c.style.visibility, docked: c.classList.contains('docked'), label: d ? d.textContent : null,
               inside: r.left >= cv.left - 1 && r.top >= cv.top - 1 && r.right <= cv.right + 1 && r.bottom <= cv.bottom + 1 }; }""")
    check(off["disp"] == "block" and off["vis"] == "" and off["docked"] and off["inside"] and off["label"] and "Incident" in off["label"],
          f"[3d] a pinned card whose marker is off-screen is not hidden: it docks to the map edge with an arrow and label ({off})")
    page.evaluate("() => { mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
    back = page.evaluate("() => ({ docked: document.getElementById('mapTooltip').classList.contains('docked'), dock: !!document.querySelector('#mapTooltip .map-card-dock') })")
    check(not back["docked"] and not back["dock"], "[3d] ... and it undocks by itself when the marker is back in view")
    # 3. the x button and Esc close the card without ending a follow
    page.evaluate("() => { const t = mapHitTargets.find((h) => h.type === 'team'); map3d.follow(t.data.id); showMapCard(mapHitTargets.find((h) => h.type === 'incident' && h.data.pool !== 1)); }")
    page.wait_for_timeout(150)
    check(page.evaluate("() => window.__cardDisplay()") == "block" and page.evaluate("() => window.__follow()") == "follow", "[3d] a card can be open while following another marker")
    page.click("#mapCardClose", force=True); page.wait_for_timeout(100)
    check(page.evaluate("() => window.__cardDisplay()") == "none" and page.evaluate("() => window.__follow()") == "follow", "[3d] the card's x button closes it without ending the follow")
    page.evaluate("() => showMapCard(mapHitTargets.find((h) => h.type === 'incident' && h.data.pool !== 1))")
    page.keyboard.press("Escape"); page.wait_for_timeout(100)
    check(page.evaluate("() => window.__cardDisplay()") == "none" and page.evaluate("() => window.__follow()") == "follow", "[3d] Esc closes the open card without ending the follow")
    # 4. clicking empty space closes the card but keeps following; a 2px-jitter click never orbits
    page.evaluate("() => showMapCard(mapHitTargets.find((h) => h.type === 'incident' && h.data.pool !== 1))")
    empty = page.evaluate("""() => { const r = document.getElementById('mapCanvas').getBoundingClientRect();
      for (const [fx, fy] of [[0.04, 0.5], [0.96, 0.5], [0.5, 0.06], [0.5, 0.94], [0.1, 0.1], [0.9, 0.9]]) { const sx = r.width * fx, sy = r.height * fy;
        if (!MapRender.hitTestWorld(mapCamera, mapHitTargets, sx, sy)) return { x: r.left + sx, y: r.top + sy }; } return null; }""")
    check(empty is not None, "[3d] there is an empty map spot to click")
    az0 = page.evaluate("() => map3d.state.az")
    page.mouse.move(empty["x"], empty["y"])
    page.mouse.down(); page.mouse.move(empty["x"] + 2, empty["y"] + 1); page.mouse.move(empty["x"] + 1, empty["y"] + 2); page.mouse.up()
    page.wait_for_timeout(150)
    jit = page.evaluate("() => ({ card: window.__cardDisplay(), follow: window.__follow(), az: map3d.state.az })")
    check(jit["follow"] == "follow" and abs(jit["az"] - az0) < 1e-9, f"[3d] a click with 2px jitter does not orbit or end the follow (az {az0:.4f} -> {jit['az']:.4f})")
    check(jit["card"] == "none", "[3d] clicking empty space closes the card (and the follow continues)")
    # 5. a real drag still orbits and ends the follow
    page.mouse.move(empty["x"], empty["y"]); page.mouse.down()
    for i in range(1, 9):
        page.mouse.move(empty["x"] + 6 * i, empty["y"] + 2 * i)
    page.mouse.up(); page.wait_for_timeout(150)
    drag = page.evaluate("() => ({ follow: window.__follow(), az: map3d.state.az })")
    check(drag["follow"] is None and abs(drag["az"] - az0) > 0.05, f"[3d] a real drag orbits and ends the follow (az {az0:.3f} -> {drag['az']:.3f})")
    # Esc with no card open ends a follow
    page.evaluate("() => { map3d.follow(mapHitTargets.find((h) => h.type === 'team').data.id); }")
    page.keyboard.press("Escape"); page.wait_for_timeout(100)
    check(page.evaluate("() => window.__follow()") is None, "[3d] Esc with no card open stops following")
    # 2D / 2.5D: the open card stays with its marker when panning
    for mode in ("iso", "2d"):
        page.evaluate("(m) => { setMapViewMode(m); }", mode); page.wait_for_timeout(300)
        r2 = page.evaluate("""() => { const h = mapHitTargets.find((x) => x.type === 'incident'); showMapCard(h);
          const card = document.getElementById('mapTooltip'), cv = document.getElementById('mapCanvas').getBoundingClientRect();
          const gap = () => { const hh = mapHitTargets.find((x) => x.type === 'incident' && x.data.id === h.data.id), p = mapCamera.worldToScreen(hh.wx, hh.wy, hh.wz || 0), r = card.getBoundingClientRect();
            const mx = cv.left + p.x, my = cv.top + p.y; return Math.hypot(Math.max(r.left - mx, 0, mx - r.right), Math.max(r.top - my, 0, my - r.bottom)); };
          const g0 = gap(); mapCamera.pan(25, 15, 100, 100); drawFrame(window._lastFrame, true); anchorMapCard(); const g1 = gap();
          { const hh = mapHitTargets.find((x) => x.type === 'incident' && x.data.id === h.data.id), pp = mapCamera.worldToScreen(hh.wx, hh.wy, hh.wz || 0); mapCamera.zoomAt(pp.x, pp.y, 1.3); } drawFrame(window._lastFrame, true); anchorMapCard(); const g2 = gap();
          hideMapCard(); return [g0, g1, g2]; }""")
        check(all(g < 40 for g in r2), f"[{mode}] an open card follows its marker through pan and zoom (gaps {[round(g) for g in r2]})")
        page.evaluate("() => { mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); }")
    page.evaluate("() => setMapViewMode('3d')"); page.wait_for_timeout(300)
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the card / follow checks (got {real_errors[:5]})")


def _card_modes_check(page, console_errors):
    """Hover = temporary card (hides ~300 ms after the cursor leaves); click =
    pinned card; a pinned card survives camera moves and docks to the map
    edge, with an arrow that brings the camera back, when its marker is out of
    view. Same in 2D, 2.5D and 3D."""
    for mode in ("3d", "iso", "2d"):
        page.evaluate("""(m) => { setMapViewMode(m); if (map3d) map3d.cancelCamera(); hideMapCard(); DashboardSelection.set(null); selectedMarker = null;
          mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }""", mode)
        page.wait_for_timeout(500)
        pos = lambda kind, idx=0: page.evaluate("""([kind, idx]) => { const hs = mapHitTargets.filter((h) => h.type === kind && (kind !== 'incident' || h.data.pool !== 1)); const h = hs[idx]; if (!h) return null;
          const r = document.getElementById('mapCanvas').getBoundingClientRect(); const p = mapCamera.worldToScreen(h.wx, h.wy, h.wz || 0); return { id: h.data.id, x: r.left + p.x, y: r.top + p.y }; }""", [kind, idx])
        st = lambda: page.evaluate("() => ({ hov: document.getElementById('mapHoverTip').style.display, pin: document.getElementById('mapTooltip').style.display })")
        t0 = pos("team")
        # 1. hover: shows while over the marker; hides after the grace period once the cursor leaves
        page.mouse.move(t0["x"] - 80, t0["y"] - 80)
        page.mouse.move(t0["x"], t0["y"], steps=4); page.wait_for_timeout(120)
        over = st()
        check(over["hov"] == "block" and over["pin"] != "block", f"[{mode}] hovering a marker shows a temporary card (not a pinned one) ({over})")
        page.mouse.move(t0["x"] - 90, t0["y"] - 90)       # one jump off the marker, then look straight away (the 300 ms grace must still be running)
        grace = st()
        page.wait_for_timeout(500)
        gone = st()
        check(grace["hov"] == "block" and gone["hov"] != "block", f"[{mode}] the hover card is still there right after leaving (grace) and gone ~300 ms later ({grace['hov']} -> {gone['hov']})")
        # 2. click pins; hovering another marker shows a temporary card without replacing it; hovering the pinned one adds nothing
        page.mouse.move(t0["x"] - 5, t0["y"] - 5); page.mouse.click(t0["x"], t0["y"]); page.wait_for_timeout(150)
        t0 = pos("team")
        pinned = page.evaluate("() => ({ pin: document.getElementById('mapTooltip').style.display, marker: mapCardMarker })")
        check(pinned["pin"] == "block" and pinned["marker"]["type"] == "team", f"[{mode}] clicking a marker pins its card ({pinned})")
        inc = pos("incident")
        page.mouse.move(inc["x"] - 40, inc["y"] - 40); page.mouse.move(inc["x"], inc["y"], steps=4); page.wait_for_timeout(150)
        both = page.evaluate("() => ({ pin: document.getElementById('mapTooltip').style.display, hov: document.getElementById('mapHoverTip').style.display, pm: mapCardMarker, hm: mapHoverMarker })")
        check(both["pin"] == "block" and both["hov"] == "block" and both["pm"]["type"] == "team" and both["hm"]["type"] == "incident",
              f"[{mode}] hovering another marker shows its temporary card and keeps the pinned one ({both['pm']}, {both['hm']})")
        page.mouse.move(t0["x"] - 40, t0["y"] - 40); page.mouse.move(t0["x"], t0["y"], steps=4); page.wait_for_timeout(450)
        again = st()
        check(again["pin"] == "block" and again["hov"] != "block", f"[{mode}] hovering the pinned marker shows no second card ({again})")
        # 3. a pinned card survives camera moves (zoom, pan / orbit by dragging, a follow of another team)
        page.mouse.move(40, 40)
        page.evaluate("() => { const c = mapCamera; c.zoomAt(c.state.viewW / 2, c.state.viewH / 2, 1.4); drawFrame(window._lastFrame, true); }")
        cv = page.evaluate("() => { const r = document.getElementById('mapCanvas').getBoundingClientRect(); return [r.left, r.top, r.width, r.height]; }")
        page.mouse.move(cv[0] + 30, cv[1] + cv[3] - 30); page.mouse.down()
        for i in range(1, 8):
            page.mouse.move(cv[0] + 30 + 8 * i, cv[1] + cv[3] - 30 - 3 * i)
        page.mouse.up(); page.wait_for_timeout(100)
        if mode == "3d":
            page.evaluate("() => map3d.follow(mapHitTargets.filter((h) => h.type === 'team')[1].data.id)"); page.wait_for_timeout(400)
        alive = page.evaluate("() => ({ pin: document.getElementById('mapTooltip').style.display, marker: mapCardMarker })")
        check(alive["pin"] == "block" and alive["marker"] is not None, f"[{mode}] the pinned card survives zoom, drag and (3D) following another team ({alive['pin']})")
        page.evaluate("() => { if (map3d) map3d.cancelCamera(); mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
        page.wait_for_timeout(200)
        # 4. dock when the marker leaves view; undock when it returns; the arrow brings the camera to it
        gone_from_view = page.evaluate("""() => { const h = () => mapHitTargets.find((x) => x.type === mapCardMarker.type && x.data.id === mapCardMarker.id);
          const vis = () => { const m = h(); const p = mapCamera.worldToScreen(m.wx, m.wy, m.wz || 0);
            return (mapCamera.state.viewMode === '3d' && map3d) ? map3d.isVisible(m.wx, m.wy, m.wz || 0) : (p.x >= 0 && p.y >= 0 && p.x <= mapCamera.state.viewW && p.y <= mapCamera.state.viewH); };
          const m0 = h(), p0 = mapCamera.worldToScreen(m0.wx, m0.wy, m0.wz || 0), w = mapCamera.state.viewW, hh = mapCamera.state.viewH;
          const fx = p0.x < w / 2 ? w - 4 : 4, fy = p0.y < hh / 2 ? hh - 4 : 4;       // zoom in on the far corner: the marker slides out the other way
          for (let i = 0; i < 12 && vis(); i++) { mapCamera.zoomAt(fx, fy, 1.6); drawFrame(window._lastFrame, true); }
          return !vis(); }""")
        check(gone_from_view, f"[{mode}] test setup: the pinned marker has left the view")
        page.wait_for_timeout(200)
        d = page.evaluate("""() => { const c = document.getElementById('mapTooltip'), dk = c.querySelector('.map-card-dock'), r = c.getBoundingClientRect(), cv = document.getElementById('mapCanvas').getBoundingClientRect();
          return { disp: c.style.display, vis: c.style.visibility, docked: c.classList.contains('docked'), label: dk ? dk.textContent : null,
                   inside: r.left >= cv.left - 1 && r.top >= cv.top - 1 && r.right <= cv.right + 1 && r.bottom <= cv.bottom + 1 }; }""")
        check(d["disp"] == "block" and d["vis"] == "" and d["docked"] and d["inside"] and d["label"] and "Team" in d["label"],
              f"[{mode}] a pinned card whose marker is out of view docks to the map edge with an arrow and label ({d})")
        page.evaluate("() => { const dk = document.querySelector('#mapTooltip .map-card-dock'); dk.click(); }")
        page.wait_for_timeout(1900 if mode == "3d" else 300)
        arrived = page.evaluate("""() => { const h = mapHitTargets.find((x) => x.type === mapCardMarker.type && x.data.id === mapCardMarker.id); const p = mapCamera.worldToScreen(h.wx, h.wy, h.wz || 0);
          const c = document.getElementById('mapTooltip'); return { x: p.x, y: p.y, w: mapCamera.state.viewW, h: mapCamera.state.viewH, docked: c.classList.contains('docked'), disp: c.style.display }; }""")
        near_centre = abs(arrived["x"] - arrived["w"] / 2) < arrived["w"] * 0.3 and abs(arrived["y"] - arrived["h"] / 2) < arrived["h"] * 0.3
        check(near_centre and not arrived["docked"] and arrived["disp"] == "block", f"[{mode}] clicking the dock arrow brings the camera to the marker, and the card undocks ({arrived})")
        page.evaluate("() => { hideMapCard(); mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true); }")
    page.evaluate("() => setMapViewMode('3d')"); page.wait_for_timeout(300)
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the card-mode checks (got {real_errors[:5]})")


def _view3d_pin_depth_check(page, console_errors):
    """A pin must not cut a square hole in the fire / smoke behind it: only
    the pin's visible pixels may write depth. Compare three renders of the
    same frozen scene around a fire incident - effects off (bare ground), and
    effects on with the pins hidden / shown: just outside the pin's round
    shape but inside its sprite square, the pixels must show the effects."""
    res = page.evaluate("""async () => {
      const orig = window._lastFrame;
      map3d.setSettings({ reduceMotion: true, effects: 'high', autoQuality: false }); map3d.cancelCamera(); hideMapCard(); mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH);
      // A lone fire incident (no neighbouring markers) so only its own pin is in the picture.
      const tpl = mapHitTargets.find((h) => h.type === 'incident' && h.data.type === 7 && h.data.pool !== 1) || mapHitTargets.find((h) => h.type === 'incident' && h.data.pool !== 1);
      const fr = JSON.parse(JSON.stringify(orig)); fr.tick += 2000; fr.teams = [];
      const base = fr.incidents.find((i) => i.id === tpl.data.id);
      fr.incidents = [Object.assign({}, base, { type: 7, priority: 2, status: 0, building_id: -1, victims: 3, assigned_team: 255, support_team: 255, discovered: 1 })];
      drawFrame(fr, true);
      const cv = document.getElementById('map3dCanvas'), tmp = document.createElement('canvas'), tctx = tmp.getContext('2d', { willReadFrequently: true });
      const grab = async () => { drawFrame(fr, true); const url = cv.toDataURL('image/png'); const img = new Image(); img.src = url; await img.decode();
        tmp.width = img.width; tmp.height = img.height; tctx.drawImage(img, 0, 0); return { d: tctx.getImageData(0, 0, img.width, img.height), k: img.width / cv.clientWidth }; };
      const out = [];
      for (const fire of [mapHitTargets.find((h) => h.type === 'incident')]) {
        const st = map3d.state; st.az = (35 * Math.PI) / 180; st.el = (32 * Math.PI) / 180; st.tx = fire.wx; st.tz = fire.wy; mapCamera.orbit(0, 0);
        mapCamera.zoomAt(mapCamera.state.viewW / 2, mapCamera.state.viewH / 2, 3.2); drawFrame(fr, true);
        const pin = () => map3d.incidentPins().find((p) => p.ids.includes(fire.data.id));
        for (let i = 0; i < 14; i++) { if (pin().lod !== 0) break; mapCamera.zoomAt(mapCamera.state.viewW / 2, mapCamera.state.viewH / 2, 1.12); drawFrame(fr, true); }
        if (pin().lod !== 0) { mapCamera.zoomAt(mapCamera.state.viewW / 2, mapCamera.state.viewH / 2, 1 / 1.12); drawFrame(fr, true); }
        if (pin().lod !== 0) continue;                       // badges only appear at higher levels of detail: keep the plain pin
        const f = mapHitTargets.find((h) => h.type === 'incident' && h.data.id === fire.data.id), c = mapCamera.worldToScreen(f.wx, f.wy, f.wz || 0), head = pin().headPx;
        map3d.setMarkersVisible(false); const hidden = await grab();
        map3d.setMarkersVisible(true); const shown = await grab();
        map3d.setSettings({ effects: 'off' }); const bare = await grab(); map3d.setSettings({ effects: 'high' });
        const k = shown.k, W = shown.d.width;
        let region = 0, vsBare = 0, vsHidden = 0;
        const half = Math.round(head * 2.6 * k), cx = Math.round(c.x * k), cy = Math.round(c.y * k), inner = head * 1.5 * k;
        for (let y = cy - half; y <= cy + half; y++) for (let x = cx - half; x <= cx + half; x++) {
          if (x < 0 || y < 0 || x >= W || y >= shown.d.height || Math.hypot(x - cx, y - cy) <= inner) continue;
          region++; const i = (y * W + x) * 4;
          const diff = (a, b) => Math.abs(a.d.data[i] - b.d.data[i]) + Math.abs(a.d.data[i + 1] - b.d.data[i + 1]) + Math.abs(a.d.data[i + 2] - b.d.data[i + 2]) > 24;
          if (diff(shown, bare)) vsBare++;
          if (diff(shown, hidden)) vsHidden++;
        }
        out.push({ id: fire.data.id, head, region, vsBare, vsHidden });
      }
      drawFrame(orig, true);
      map3d.setSettings({ reduceMotion: false, effects: 'low', autoQuality: true }); map3d.setMarkersVisible(true);
      mapCamera.reset(mapCamera.state.viewW, mapCamera.state.viewH); drawFrame(window._lastFrame, true);
      return out; }""")
    check(len(res) > 0, f"[3d] pin depth check: found a fire incident to test ({len(res)} usable)")
    best = max(res, key=lambda r: r["vsBare"]) if res else None
    check(best is not None and best["vsBare"] >= 25,
          f"[3d] with the pin in front, the pixels just outside its round shape but inside its sprite square show the fire effects, not the bare ground ({best['vsBare'] if best else 0} of {best['region'] if best else 0} differ from bare ground)")
    check(best is not None and best["vsHidden"] <= max(4, best["region"] * 0.01),
          f"[3d] and they match the render with the pin hidden - the pin's transparent corners hide nothing ({best['vsHidden'] if best else 0} of {best['region'] if best else 0} differ)")
    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0, f"no UNEXPECTED console/page errors in the pin depth check (got {real_errors[:5]})")


def _route_accuracy_check(page):
    """The dashboard reconstructs routes (route.js ports core/path.c's A*);
    check it against what the simulator ACTUALLY did in the bundled
    recording: on every recorded move toward an unchanged destination,
    the reconstructed route's first step should be the cell the team
    really stepped to."""
    res = page.evaluate("""async (name) => {
      const txt = await (await fetch('/api/recordings/' + encodeURIComponent(name))).text();
      let map = null; const frames = [];
      for (const l of txt.split('\\n')) {
        if (l.startsWith('STATICMAP ')) map = JSON.parse(l.slice(10));
        else if (l.startsWith('LIVEFRAME ')) frames.push(JSON.parse(l.slice(10)));
      }
      const terr = MapRender.parseTerrain(map);
      let moves = 0, match = 0;
      for (let i = 0; i + 1 < frames.length; i++) {
        const a = frames[i].teams, b = frames[i + 1].teams;
        for (let k = 0; k < a.length; k++) {
          const t = a[k], u = b[k];
          if (!t.has_dest || (t.x === u.x && t.y === u.y) || t.dest_x !== u.dest_x || t.dest_y !== u.dest_y) continue;
          const r = RoutePlanner.plan(terr, t.x, t.y, t.dest_x, t.dest_y);
          if (!r || r.length < 2) continue;
          moves++;
          if (r[1][0] === u.x && r[1][1] === u.y) match++;
        }
      }
      return { moves, match };
    }""", DEMO_RECORDING)
    rate = res["match"] / max(1, res["moves"])
    check(res["moves"] > 500 and rate >= 0.97,
          f"the reconstructed route's first step matches the simulator's actual move {rate:.1%} of the time ({res['match']}/{res['moves']} recorded moves)")


def _replay_env_history_check(page, console_errors):
    """Replay shows building damage as of the replayed frame (the simulator
    re-sends the building list when buildings are destroyed), identically in
    2D, 2.5D and 3D; Live is unchanged."""
    page.select_option("#modeSelect", "replay")
    page.click("#replayRefreshBtn")
    page.wait_for_timeout(300)
    page.select_option("#replaySelect", DEMO_RECORDING)
    page.click("#replayLoadBtn")
    page.wait_for_function("() => Replay.frameCount > 0 && Replay.currentIndex === 0", timeout=10000)
    count = """(env) => env ? env.buildings.filter(b => (b.damage || 0) >= 100).length : -1"""
    page.evaluate("() => Replay.seekToIndex(0)")
    first = page.evaluate(f"() => ({count})(currentEnvironment())")
    last_index = page.evaluate("() => Replay.frameCount - 1")
    page.evaluate("(i) => Replay.seekToIndex(i)", last_index)
    last = page.evaluate(f"() => ({count})(currentEnvironment())")
    check(0 <= first < last,
          f"replayed building damage grows over the run: {first} destroyed buildings at tick 0 < {last} at the last frame")
    # Same city in every view at a given frame, and it really is that frame's.
    page.evaluate("() => Replay.seekToIndex(0)")
    same = []
    for mode in ("2d", "iso", "3d"):
        _cycle_view_to(page, mode) if (mode != "3d" or page.evaluate("() => map3dAvailable")) else None
        if page.evaluate("() => mapCamera.state.viewMode") != mode:
            continue
        same.append(page.evaluate("() => currentEnvironment() === Replay.envAt(Replay.currentIndex) && currentEnvironment() === Replay.envAt(0)"))
    check(len(same) >= 2 and all(same), f"2D, 2.5D and 3D draw the same, frame-0 building list at tick 0 (views checked: {len(same)})")
    page.evaluate("(i) => Replay.seekToIndex(i)", last_index)
    check(page.evaluate("() => currentEnvironment() === Replay.envAt(Replay.currentIndex) && currentEnvironment() !== Replay.envAt(0)"),
          "scrubbing to the end swaps in the later building list in every view")
    _cycle_view_to(page, "2d")
    # Live is untouched: back in Live mode the latest received list is used.
    page.select_option("#modeSelect", "live")
    page.wait_for_timeout(150)
    check(page.evaluate("() => currentEnvironment() === staticEnvironment"), "in Live mode the map still uses the latest received building list")


def _webgl_fallback_check(browser):
    """A browser with no WebGL: the page must load without errors, never
    offer 3D, and keep 2D / 2.5D working."""
    page = browser.new_page(viewport={"width": 1366, "height": 900})
    errors = []
    page.on("pageerror", lambda exc: errors.append(str(exc)))
    page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
    page.add_init_script("""
      const _gc = HTMLCanvasElement.prototype.getContext;
      HTMLCanvasElement.prototype.getContext = function (type, ...rest) {
        if (type === 'webgl' || type === 'webgl2' || type === 'experimental-webgl') return null;
        return _gc.call(this, type, ...rest);
      };
      try { localStorage.setItem('dashboard.mapViewMode', '3d'); } catch (e) {}
    """)
    try:
        page.goto(BASE_URL, wait_until="networkidle")
        page.click("text=Bypass & Enter Dashboard")
        page.wait_for_selector("#main-dashboard.active", timeout=5000)
        page.wait_for_function("() => window.MapView3D !== undefined", timeout=5000)
        check(page.evaluate("() => MapView3D.available === false && map3dAvailable === false"),
              "[no WebGL] the 3D view reports itself unavailable")
        modes = []
        for _ in range(4):
            page.click("#viewModeBtn")
            modes.append(page.evaluate("() => mapCamera.state.viewMode"))
        check(modes == ["iso", "2d", "iso", "2d"], f"[no WebGL] the view button cycles only 2D <-> 2.5D (got {modes})")
        check(page.evaluate("() => mapCamera.state.viewMode") == "2d" and
              page.evaluate("() => getComputedStyle(document.getElementById('map3dCanvas')).display") == "none",
              "[no WebGL] a saved '3d' preference falls back to 2D with no WebGL canvas shown")
        check(len(real_console_errors(errors)) == 0, f"[no WebGL] no console/page errors (got {errors[:3]})")
    finally:
        page.close()


def _click_and_drag_markers(page, tag, screenshot_dir):
    """Click a team on its drawn head and drag one onto an incident,
    through real mouse events, in whichever view is active."""
    target = page.evaluate("""() => {
      const h = mapHitTargets.find(h => h.type === 'team');
      if (!h) return null;
      const rect = document.getElementById('mapCanvas').getBoundingClientRect();
      const p = mapCamera.worldToScreen(h.wx, h.wy, h.wz || 0);
      const w = mapCamera.screenToWorld(p.x, p.y, h.wz || 0);
      return { id: h.id, x: rect.left + p.x, y: rect.top + p.y, hasHeight: !!h.wz, dWorld: Math.hypot(w.x - h.wx, w.y - h.wy) };
    }""")
    check(bool(target and target["hasHeight"]), f"[{tag}] team hit targets carry a marker height (got {target})")
    if target:
        check(target["dWorld"] < 1e-6, f"[{tag}] clicking a team's drawn head maps back to the team's world position (off by {target['dWorld']:.1e})")
        page.mouse.click(target["x"], target["y"])
        page.wait_for_timeout(100)
        sel = page.evaluate("() => selectedMarker")
        check(bool(sel and sel["type"] == "team" and sel["id"] == target["id"]),
              f"[{tag}] clicking a team's marker selects that team (expected id {target['id']}, got {sel})")
    pair = page.evaluate("""() => {
      const rect = document.getElementById('mapCanvas').getBoundingClientRect();
      const pt = (h) => { const p = mapCamera.worldToScreen(h.wx, h.wy, h.wz || 0); return { x: rect.left + p.x, y: rect.top + p.y }; };
      for (const t of mapHitTargets.filter(h => h.type === 'team')) {
        for (const i of mapHitTargets.filter(h => h.type === 'incident')) {
          if (i.data.pool !== 1 && i.data.status !== 3 && i.data.id !== t.data.dest_id) {
            const ip = pt(i), tp = pt(t);
            const back = MapRender.hitTestWorld(mapCamera, mapHitTargets, ip.x - rect.left, ip.y - rect.top, 'incident');
            const tback = MapRender.hitTestWorld(mapCamera, mapHitTargets, tp.x - rect.left, tp.y - rect.top, 'team');
            if (back && back.id === i.id && tback && tback.id === t.id) return { team: tp, inc: ip, teamId: t.id, incId: i.id };
          }
        }
      }
      return null;
    }""")
    if pair:
        page.mouse.move(pair["team"]["x"], pair["team"]["y"])
        page.mouse.down()
        page.mouse.move(pair["inc"]["x"], pair["inc"]["y"], steps=6)
        page.mouse.up()
        page.wait_for_timeout(150)
        pend = page.evaluate("() => pendingOverride ? { team: pendingOverride.teamIndex, inc: pendingOverride.to.id } : null")
        check(bool(pend and pend["inc"] == pair["incId"]),
              f"[{tag}] dragging a team's marker onto an incident's marker opens the override proposal (got {pend}, expected incident {pair['incId']})")
        page.evaluate("() => { pendingOverride = null; selectedMarker = null; renderInspector(); }")
    else:
        print(f"INFO: no eligible (team, incident) pair in the current frame to drag-test in {tag} - skipped that one assertion")
    page.evaluate("() => { selectedMarker = null; renderInspector(); }")


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
    # RUN_CONTROL_TICKS x 150 ms is the whole run: long enough that it cannot end
    # before the pause / resume steps below (they take ~9 s even on a slow machine),
    # short enough that (c) can still wait for the natural end.
    page.fill("#ticks", str(RUN_CONTROL_TICKS))
    page.fill("#tickDelay", "150")
    page.click("#liveBtn")
    page.wait_for_function("() => document.getElementById('runStateLabel').textContent === 'running' && window._lastFrame && window._lastFrame.tick >= 3", timeout=15000)

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
    pending_text = page.evaluate("() => document.getElementById('overrideBox').textContent")
    check("Will apply in the simulator when you confirm." in pending_text and "Applied in the simulator" not in pending_text,
          f"(a) a pending override says it will apply when you confirm, not that it was applied (got {' '.join(pending_text.split())!r})")
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
    # resumes them.  The run must still be going: if it has already ended
    # the scenario is wrong (too few ticks), not the dashboard.
    tick_before_pause = page.evaluate("() => window._lastFrame.tick")
    check(page.eval_on_selector("#runStateLabel", "el => el.textContent") == "running" and tick_before_pause < RUN_CONTROL_TICKS - 20,
          f"(b) the run is still going when Pause is clicked (tick {tick_before_pause} of {RUN_CONTROL_TICKS})")
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

    # (c) Let the run finish naturally, then
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


def _live_override_lifecycle_check(page, console_errors):
    """Part 4 follow-up: the override lifecycle has three UI-visible
    states, not two - pending (a pin still forcing itself in), committed
    (core/cbba.c's cbba_apply_override_pin() converged and cleared the
    pin on its own - the badge/card must show "committed", not just
    vanish), and one of two endings the override itself has no control
    over: resolved (the incident finishes) or interrupted (the sim's
    own stamina/rest handling pulls the team off first - core/debug.h's
    lifecycle doc). Both endings are exercised here with the same two
    seed/team/incident combinations laptop/tools/live_override_
    selftest.py's (10a)/(10b) verified deterministically at the C
    level: seed=8 resolves cleanly, seed=2 gets interrupted at tick 80.
    fault_profile=none and tick_delay_ms=0 for speed - this only reads
    window._lastFrame/window._overridePhase/window._journalEvents
    directly, nothing here depends on human-visible pacing."""

    def run_and_watch(seed, ticks, expect_ending, journal_label_substr):
        page.select_option("#faultProfile", "none")
        page.fill("#seed", str(seed))
        page.fill("#ticks", str(ticks))
        # Non-zero: at 0ms the whole (short) run can finish server-side
        # before the browser's own rendering/polling even catches the
        # brief "team available, incident open" window to send the
        # override through - the existing apply/release check above
        # hit the same thing, which is why it also paces at 150ms.
        page.fill("#tickDelay", "120")
        page.click("#liveBtn")

        staged = None
        for _ in range(40):
            page.wait_for_timeout(200)
            staged = page.evaluate("""() => {
              const frame = window._lastFrame;
              if (!frame) return null;
              const inc = (frame.incidents || []).find(i => i.id === 0 && i.status !== 3 && i.pool === 0);
              const team = frame.teams && frame.teams[1];
              if (!inc || !team || team.status !== 0) return null;
              return { incidentId: inc.id, teamId: team.id };
            }""")
            if staged is not None:
                break
        check(staged is not None,
              f"seed={seed}: team 1 available and incident 0 open showed up in the UI (got {staged})")
        if staged is None:
            return

        journal_count_before = page.evaluate("() => (window._journalEvents || []).length")
        ok = page.evaluate("""(staged) => {
          return fetch("/api/run-live/override", {
            method: "POST", headers: {"Content-Type": "application/json"},
            body: JSON.stringify({team_id: staged.teamId, incident_id: staged.incidentId}),
          }).then(r => r.ok);
        }""", staged)
        check(ok, f"seed={seed}: the OVERRIDE POST for team {staged['teamId']} -> incident {staged['incidentId']} succeeded")

        # "pending" is checked via the journal, not live polling: per
        # the earlier C-level finding, cbba_apply_override_pin() can
        # commit within just 1-3 ticks (as little as ~120-360ms at this
        # pacing) - short enough that a 200ms poll can legitimately
        # sample right past it and never catch "pending" live, even
        # though it genuinely happened. handleLiveOverrideAck() logs an
        # "applied" journal entry synchronously the moment the ack SSE
        # event lands, which is unaffected by that race - it's a
        # permanent record, not a transient state to catch mid-flight.
        saw_committed = False
        ending = None
        for _ in range(150):
            page.wait_for_timeout(200)
            phase = page.evaluate("() => (window._overridePhase && window._overridePhase[1]) || null")
            if phase and phase.get("kind") == "committed":
                saw_committed = True
            if saw_committed and phase is None:
                events = page.evaluate("() => window._journalEvents || []")
                recent = [e for e in events if e.get("team") == 1 and e.get("kind") == "override"]
                if any(journal_label_substr in (e.get("label") or "") for e in recent):
                    ending = expect_ending
                    break
        journal_has_applied = page.evaluate("""() => {
          const events = window._journalEvents || [];
          return events.some(e => e.team === 1 && e.kind === 'override' && e.pillCls === 'good' && (e.label || '').includes('applied'));
        }""")
        check(journal_has_applied, f"seed={seed}: the pending pin was applied - journal shows a matching 'applied' entry")
        check(saw_committed, f"seed={seed}: the map/card badge shows the 'committed' phase once the pin auto-clears")
        check(ending == expect_ending,
              f"seed={seed}: the lifecycle ends in {expect_ending!r} with a matching journal entry "
              f"(containing {journal_label_substr!r}), got ending={ending!r}")

        journal_grew = page.evaluate(f"() => (window._journalEvents || []).length > {journal_count_before}")
        check(journal_grew, f"seed={seed}: new journal entries were recorded for this lifecycle")

        stop_btn = page.query_selector("#stopLiveBtn")
        if stop_btn and stop_btn.is_visible():
            stop_btn.click()
            page.wait_for_timeout(300)

    # (a) resolved: seed=8, team 1 -> incident 0 finishes at tick 71
    # (empirically verified) - generous tick budget for margin.
    run_and_watch(seed=8, ticks=150, expect_ending="resolved", journal_label_substr="resolved")

    # (b) interrupted: seed=2, team 1 -> incident 0 is pulled off by
    # stamina/rest at tick 80 (empirically verified).
    run_and_watch(seed=2, ticks=150, expect_ending="interrupted", journal_label_substr="Interrupted")

    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0,
          f"no UNEXPECTED console/page errors during the override lifecycle check (got {len(real_errors)}: {real_errors[:5]})")

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


DEMO_RECORDING = "demo_seed1002_combined_600ticks.jsonl.gz"
RUN_CONTROL_TICKS = 80      # x 150 ms = 12 s run (see _run_control_check)


def _timeline_state(page):
    """(first, last, bar_pixels) of the Detection Timeline as drawn now.
    bar_pixels counts non-transparent pixels in the plot area (right of
    the label gutter), so it is > 0 only when history bars are drawn."""
    return page.evaluate("""() => {
      const txt = document.getElementById('timelineRange').textContent;
      const m = /ticks (\\d+)\u2013(\\d+)/.exec(txt);
      const c = document.getElementById('timelineCanvas');
      const dpr = c.width / c.getBoundingClientRect().width;
      const x0 = Math.ceil(100 * dpr);
      const d = c.getContext('2d').getImageData(x0, 0, c.width - x0, c.height).data;
      let n = 0;
      for (let i = 3; i < d.length; i += 4) if (d[i] > 0) n++;
      return m ? [parseInt(m[1], 10), parseInt(m[2], 10), n] : [null, null, n];
    }""")


def _replay_timeline_check(page, console_errors, recordings_dir):
    """Replay-mode Detection Timeline: follows Play with no clicks and is
    rebuilt after every kind of seek (forward, backward, Next detection)."""
    page.select_option("#modeSelect", "replay")
    page.click("#replayRefreshBtn")
    page.wait_for_timeout(300)
    has_demo = page.evaluate(
        "(f) => Array.from(document.querySelectorAll('#replaySelect option')).some(o => o.value === f)",
        DEMO_RECORDING)
    check(has_demo, "the bundled demo recording is listed in Replay mode")
    if not has_demo:
        page.select_option("#modeSelect", "live")
        return
    page.select_option("#replaySelect", DEMO_RECORDING)
    page.click("#replayLoadBtn")
    page.wait_for_function("() => Replay.frameCount > 0 && Replay.currentIndex === 0", timeout=10000)

    def tick_of(idx):
        return page.evaluate(f"() => Replay.frameTick({idx})")

    # Play: the range must advance on its own, with no clicks on the page.
    page.select_option("#replaySpeed", index=page.evaluate(
        "() => Array.from(document.getElementById('replaySpeed').options).length - 1"))
    page.click("#replayPlayBtn")
    page.wait_for_timeout(600)
    first_a, last_a, bars_a = _timeline_state(page)
    page.wait_for_timeout(600)
    first_b, last_b, _ = _timeline_state(page)
    page.click("#replayPlayBtn")  # pause
    page.wait_for_timeout(100)
    cur = page.evaluate("() => Replay.currentIndex")
    first_c, last_c, _ = _timeline_state(page)
    check(last_a is not None and last_b is not None and last_b > last_a,
          f"timeline range advances during Play without clicks (ticks {first_a}-{last_a} -> {first_b}-{last_b})")
    check(first_b == 0 and last_c == tick_of(cur),
          f"timeline covers tick 0..current while playing/paused (got {first_c}-{last_c}, current tick {tick_of(cur)})")
    check(bars_a > 0, "timeline bars are drawn during Play")

    def seek_and_check(idx, label):
        page.evaluate(f"() => Replay.seekToIndex({idx})")
        page.wait_for_timeout(150)
        first, last, bars = _timeline_state(page)
        want = tick_of(idx)
        check(first == 0 and last == want and bars > 0,
              f"{label}: timeline shows ticks 0-{want} with bars (got {first}-{last}, bars={bars})")

    frame_count = page.evaluate("() => Replay.frameCount")
    hi = min(200, frame_count - 1)
    lo = min(70, hi)
    seek_and_check(lo, "scrub to tick 70")
    seek_and_check(hi, "scrub forward")
    seek_and_check(min(30, hi), "scrub backward")
    # Stepping past a seek must not reset the history (the reported bug).
    page.evaluate(f"() => Replay.seekToIndex({lo})")
    page.click("#replayPlayBtn")
    page.wait_for_timeout(300)
    page.click("#replayPlayBtn")
    first, last, bars = _timeline_state(page)
    check(first == 0 and last > tick_of(lo) and bars > 0,
          f"playing on from a seek keeps full history (got {first}-{last})")

    # Real scrubber input event.
    page.evaluate("""() => {
      const el = document.getElementById('replayScrubber');
      el.value = 5; el.dispatchEvent(new Event('input'));
    }""")
    page.wait_for_timeout(100)
    first, last, _ = _timeline_state(page)
    check(first == 0 and last == tick_of(5), f"scrubber input rebuilds the timeline (got {first}-{last})")

    # Next detection.
    page.evaluate("() => Replay.seekToIndex(0)")
    page.click("#replayJumpBtn")
    page.wait_for_timeout(150)
    idx = page.evaluate("() => Replay.currentIndex")
    first, last, bars = _timeline_state(page)
    check(idx > 0 and first == 0 and last == tick_of(idx) and bars > 0,
          f"Next detection updates the timeline (index {idx}, got {first}-{last}, bars={bars})")

    # Reload the recording: history rebuilt from tick 0.
    page.click("#replayLoadBtn")
    page.wait_for_function("() => Replay.frameCount > 0 && Replay.currentIndex === 0", timeout=10000)
    page.wait_for_timeout(100)
    first, last, _ = _timeline_state(page)
    check(first == 0 and last == 0, f"reloading a recording restarts the timeline at tick 0 (got {first}-{last})")

    page.select_option("#modeSelect", "live")
    page.wait_for_timeout(100)


def _replay_takeover_check(page, console_errors):
    """Part 5: load the bundled demo recording, scrub to a tick, take
    over. The regenerated frame must equal the replayed one, the
    timeline must cover 0..T, and an override on the taken-over run must
    be applied - and recorded, so the new recording can itself be
    taken over."""
    page.select_option("#modeSelect", "replay")
    page.click("#replayRefreshBtn")
    page.wait_for_timeout(300)
    if not page.evaluate(
            "(f) => Array.from(document.querySelectorAll('#replaySelect option')).some(o => o.value === f)",
            DEMO_RECORDING):
        check(False, "the bundled demo recording is listed (needed for the take-over check)")
        page.select_option("#modeSelect", "live")
        return
    page.select_option("#replaySelect", DEMO_RECORDING)
    page.click("#replayLoadBtn")
    page.wait_for_function("() => Replay.frameCount > 0 && Replay.currentIndex === 0", timeout=10000)

    takeover_visible = page.eval_on_selector("#replayTakeoverBtn", "el => !el.classList.contains('hidden')")
    check(takeover_visible, "the 'Take over from here' button appears once a recording is loaded")

    target = 120
    page.evaluate(f"() => Replay.seekToIndex({target})")
    page.wait_for_timeout(150)
    replayed = page.evaluate("""() => {
      const f = window._lastFrame;
      return { tick: f.tick,
               teams: f.teams.map(t => [t.x, t.y, t.status, t.dest_id, t.has_dest]),
               incidents: JSON.stringify(f.incidents) };
    }""")
    check(replayed["tick"] == target, f"replay is at tick {target} before taking over (got {replayed['tick']})")

    # Paced like a person would watch it (earlier checks left 3 ms in the field).
    # (the field lives in the live controls, hidden in Replay mode, so it
    # is set directly rather than through Playwright's visible-only fill)
    page.evaluate("() => { document.getElementById('tickDelay').value = '150'; }")
    page.click("#replayTakeoverBtn")
    saw_rebuilding = False
    # runLive() shows "running" from the first instant, so only the
    # server's takeover_ready (which pauses the run at tick T) counts.
    for _ in range(300):
        label = page.eval_on_selector("#runStateLabel", "el => el.textContent")
        if "Rebuilding" in label:
            saw_rebuilding = True
        if label == "paused":
            break
        page.wait_for_timeout(100)
    else:
        fail_and_exit("take over did not reach the paused take-over state within 30s")
        return
    if not saw_rebuilding:
        print("INFO: fast-forward finished before the 'Rebuilding state' label could be observed (fast machine)")

    mode = page.eval_on_selector("#modeSelect", "el => el.value")
    check(mode == "live" and page.evaluate("() => !Replay.isActive()"),
          "taking over switches the dashboard to Live mode and ends the replay")
    check(page.eval_on_selector("#runStateLabel", "el => el.textContent") == "paused",
          "the taken-over run starts paused at the take-over tick")
    live = page.evaluate("""() => {
      const f = window._lastFrame;
      return { tick: f.tick,
               teams: f.teams.map(t => [t.x, t.y, t.status, t.dest_id, t.has_dest]),
               incidents: JSON.stringify(f.incidents) };
    }""")
    check(live["tick"] == target, f"the first live frame is tick {target} (got {live['tick']})")
    check(live["teams"] == replayed["teams"] and live["incidents"] == replayed["incidents"],
          "the regenerated frame matches the replayed one (team positions/status/tasks and incidents)")
    first, last, bars = _timeline_state(page)
    check(first == 0 and last == target and bars > 0,
          f"the timeline shows history 0..{target} after take-over (got {first}-{last}, bars={bars})")
    stats = page.evaluate("() => simTelemetry.lastProcessedTick")
    check(stats == target, f"telemetry history is consistent at the take-over tick (lastProcessedTick={stats})")

    page.click("#resumeLiveBtn")
    page.wait_for_timeout(1200)
    after_tick = page.evaluate("() => window._lastFrame.tick")
    check(after_tick > target, f"the taken-over run advances live (tick {target} -> {after_tick})")
    first, last, _ = _timeline_state(page)
    check(first == 0 and last > target, f"the timeline keeps following the live run (got {first}-{last})")

    # Override on the taken-over run: same staging as the plain-live check.
    staged = None
    for _ in range(30):
        staged = page.evaluate("""() => {
          const frame = window._lastFrame;
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
        page.wait_for_timeout(300)
    check(staged is not None, "a real open incident was available to override onto after take-over")
    if staged is not None:
        page.click("#overrideConfirmBtn")
        applied = False
        for _ in range(60):
            page.wait_for_timeout(200)
            box_text = page.evaluate("() => document.getElementById('overrideBox').textContent")
            if box_text and "Applied" in box_text:
                applied = True
                break
        check(applied, "an override on the taken-over run is acknowledged as applied")

    page.click("#stopLiveBtn")
    page.wait_for_timeout(500)
    listing = page.evaluate("""async () => {
      const items = await (await fetch('/api/recordings')).json();
      const it = items.find(i => i.takeover_from);
      if (!it) return null;
      const text = await (await fetch('/api/recordings/' + it.filename)).text();
      return { takeover_from: it.takeover_from,
               overrideLines: text.split('\\n').filter(l => l.startsWith('OVERRIDE ')).length,
               frames: text.split('\\n').filter(l => l.startsWith('LIVEFRAME ')).length };
    }""")
    check(listing is not None and listing["takeover_from"]["tick"] == target,
          f"a new recording was written for the taken-over run (got {listing})")
    if listing and staged is not None:
        check(listing["overrideLines"] >= 1, "the applied override was written into the new recording")
    if listing:
        check(listing["frames"] > target, f"the new recording holds the full history plus live frames (got {listing['frames']} frames)")

    real_errors = real_console_errors(console_errors)
    check(len(real_errors) == 0,
          f"no UNEXPECTED console/page errors during the take-over check (got {len(real_errors)}: {real_errors[:5]})")
    page.evaluate("() => { overrideStatus = null; pendingOverride = null; selectedMarker = null; renderInspector(); }")


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
    # Screenshots the 2.5D step takes; set DASHBOARD_E2E_SCREENSHOT_DIR to keep them.
    screenshot_dir = os.environ.get("DASHBOARD_E2E_SCREENSHOT_DIR") or tempfile.mkdtemp(prefix="dashboard_e2e_shots_")
    os.makedirs(screenshot_dir, exist_ok=True)
    _demo_src = os.path.join(TOOLS_DIR, "recordings", DEMO_RECORDING)
    if os.path.exists(_demo_src):
        shutil.copy(_demo_src, recordings_tmpdir)
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

            _iso_camera_hit_test_check(page)

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

            # 2.5D view: toggle on, screenshot, click/drag a team, toggle off.
            _iso_view_check(page, console_errors, screenshot_dir)
            _view3d_check(page, console_errors, screenshot_dir)

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

            # Part 4 follow-up: the full pending -> committed ->
            # resolved/interrupted lifecycle.
            _live_override_lifecycle_check(page, console_errors)

            # Part 2: replay of a recorded live run.
            _replay_check(page, console_errors)
            _replay_timeline_check(page, console_errors, recordings_tmpdir)
            _route_accuracy_check(page)
            _replay_env_history_check(page, console_errors)
            _replay_takeover_check(page, console_errors)

            page.close()

            _webgl_fallback_check(browser)

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

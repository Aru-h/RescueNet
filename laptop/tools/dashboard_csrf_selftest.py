#!/usr/bin/env python3
"""
Self-test for app.py's cross-site request forgery guard
(_reject_cross_site_writes()), run through Flask's test client - no
server, serial port or board needed.

Every POST route on the dashboard does something real (a dispatch
override written to the boards, a mission launch, a serial port opened, a
600 s sim run started), and loopback is not a trust boundary against the
user's own browser. Checks that:
  - a same-origin (or header-less, i.e. tooling) JSON POST still works;
  - a CORS-"simple" POST (text/plain, form-encoded) - the kind a hostile
    page can send cross-site with no preflight - is refused with 415 and
    has no side effect;
  - a POST whose Origin/Referer names another site is refused with 403,
    even with a JSON body;
  - a JSON body that isn't an object is a 400, not a 500;
  - a non-numeric seed/ticks/... on /api/run, /api/run-live or
    /api/board/connect-sim is a 400, not a 500 (and starts nothing);
  - GET routes are unaffected.

The real bridge is replaced by a recorder, so "no side effect" means no
bridge call was made at all.

Run directly: `python3 dashboard_csrf_selftest.py` (from laptop/tools).
Skips (exit 0) when Flask isn't installed, like the other bridge selftests.
"""

import os
import sys

FAILURES = []


def check(cond, label):
    if cond:
        print(f"PASS: {label}")
    else:
        print(f"FAIL: {label}")
        FAILURES.append(label)


class RecordingBridge:
    """Stands in for TelemetryBridge/SimBoardSource: records every call so
    a refused request can be shown to have done nothing."""

    def __init__(self):
        self.calls = []

    def connect(self, *args, **kwargs):
        self.calls.append(("connect", args, kwargs))
        return True

    def disconnect(self):
        self.calls.append(("disconnect",))


def main():
    # Sim-only mode so /api/board/connect-sim is reachable; set before
    # app.py is imported, which is when it reads the flag.
    os.environ["SIM_ONLY"] = "1"
    try:
        import app as app_module
    except ImportError as exc:
        print(f"SKIP: dashboard_csrf_selftest (Flask/app.py import failed: {exc})")
        return 0

    bridge = RecordingBridge()
    app_module.bridge = bridge
    client = app_module.app.test_client()
    json_ct = {"Content-Type": "application/json"}
    same_origin = f"http://{app_module.DASHBOARD_HOST}:{app_module.DASHBOARD_PORT}"

    # --- Allowed -------------------------------------------------------
    for origin in (same_origin, f"http://localhost:{app_module.DASHBOARD_PORT}"):
        resp = client.post("/api/board/disconnect", data="{}",
                           headers={**json_ct, "Origin": origin})
        check(resp.status_code == 200, f"same-origin JSON POST from {origin} accepted")
    resp = client.post("/api/board/connect-sim", json={"seed": 3, "ticks": 10},
                       headers={"Origin": same_origin})
    check(resp.status_code == 200 and bridge.calls[-1][2].get("seed") == 3,
          "same-origin JSON POST with a body reaches the route")
    resp = client.post("/api/board/disconnect", data="{}", headers=json_ct)
    check(resp.status_code == 200, "JSON POST without Origin/Referer (tooling) accepted")
    resp = client.post("/api/board/disconnect", data="{}",
                       headers={**json_ct, "Referer": same_origin + "/bench/a"})
    check(resp.status_code == 200, "JSON POST with a same-origin Referer accepted")

    # --- CORS-simple content types refused ------------------------------
    calls_before = len(bridge.calls)
    for route in ("/api/board/override", "/api/board/launch", "/api/board/connect",
                  "/api/board/connect-sim", "/api/board/disconnect", "/api/run",
                  "/api/run-live"):
        resp = client.post(route, data='{"team_id": 1, "incident_id": 2, "port": "/dev/x"}',
                           headers={"Content-Type": "text/plain"})
        check(resp.status_code == 415, f"text/plain POST to {route} refused (415)")
        resp = client.post(route, data="port=/dev/x",
                           headers={"Content-Type": "application/x-www-form-urlencoded"})
        check(resp.status_code == 415, f"form-encoded POST to {route} refused (415)")
    resp = client.post("/api/board/disconnect")
    check(resp.status_code == 415, "bodyless POST without a content type refused (415)")
    check(len(bridge.calls) == calls_before, "refused content types had no side effect")

    # --- Cross-origin refused, even as JSON ------------------------------
    calls_before = len(bridge.calls)
    for origin in ("http://evil.example", "null",
                   f"http://127.0.0.1:{app_module.DASHBOARD_PORT + 1}",
                   f"https://127.0.0.1:{app_module.DASHBOARD_PORT}"):
        resp = client.post("/api/board/disconnect", data="{}",
                           headers={**json_ct, "Origin": origin})
        check(resp.status_code == 403, f"JSON POST from Origin {origin} refused (403)")
    resp = client.post("/api/board/disconnect", data="{}",
                       headers={**json_ct, "Referer": "http://evil.example/page"})
    check(resp.status_code == 403, "JSON POST with a cross-site Referer refused (403)")
    check(len(bridge.calls) == calls_before, "cross-origin requests had no side effect")

    # --- Malformed bodies ------------------------------------------------
    resp = client.post("/api/board/connect-sim", data="[1, 2]", headers=json_ct)
    check(resp.status_code == 400, "JSON array body is a 400")
    resp = client.post("/api/board/connect-sim", data="{not json", headers=json_ct)
    check(resp.status_code == 400, "malformed JSON body is a 400")

    # --- Non-numeric parameters are a 400, not a 500 ----------------------
    def no_build():
        FAILURES.append("build_binary() called for a request that should have been refused")
        return (False, "")
    app_module.build_binary = no_build
    calls_before = len(bridge.calls)
    for route, body in (("/api/run", {"seed": "x"}),
                        ("/api/run", {"ticks": None}),
                        ("/api/run-live", {"tick_delay_ms": "fast"}),
                        ("/api/board/connect-sim", {"ticks": [1]})):
        resp = client.post(route, json=body)
        check(resp.status_code == 400, f"{route} with {body} is a 400")
    check(len(bridge.calls) == calls_before, "bad parameters started nothing")

    # --- Reads unaffected ------------------------------------------------
    resp = client.get("/api/history", headers={"Origin": "http://evil.example"})
    check(resp.status_code == 200, "GET routes are not gated")

    if FAILURES:
        print(f"dashboard_csrf_selftest: {len(FAILURES)} check(s) FAILED")
        return 1
    print("dashboard_csrf_selftest: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
CONCORD / REMCE local test dashboard.

Run this from inside your project directory (the folder that contains
Makefile, main.c, etc.) with:

    python3 app.py

Then open http://127.0.0.1:5050 in a browser.

It shells out to `make` and `./tron <args>` on your behalf, parses the
stderr [DIAG] lines and the generated tron_snapshot_dataset.csv, and
serves everything to a single-page dashboard so you can run + inspect
a sim in two clicks instead of digging through terminal output.
"""

import csv
import json
import os
import re
import subprocess
import time
from collections import defaultdict, Counter
from pathlib import Path

from flask import Flask, Response, jsonify, request, send_from_directory, stream_with_context

TOOLS_DIR = Path(__file__).resolve().parent
# The Makefile, main.c, and the built `tron` binary live at the repo
# root (one level up from tools/), since the laptop/board platform
# split moved this dashboard into tools/ alongside the other
# laptop-only tooling (analysis.c, label_snapshots.c). Everything
# this script runs (`make`, `./tron`) needs to happen with that
# directory as cwd, not tools/ itself.
PROJECT_DIR = TOOLS_DIR.parent
CSV_PATH = PROJECT_DIR / "tron_snapshot_dataset.csv"
HISTORY_PATH = PROJECT_DIR / ".dashboard_history.json"
BINARY = PROJECT_DIR / "tron"

app = Flask(__name__, static_folder="static", template_folder="templates")

EVENT_NAMES = [
    "NONE", "NEW_INCIDENT", "TEAM_ASSIGNED", "TEAM_ON_SCENE",
    "STAGE_COMPLETE", "TEAM_REST", "TEAM_AVAILABLE", "INCIDENT_RESOLVED",
    "FAULT_INJECTED", "FAULT_RECOVERED", "WATCHDOG_ANOMALY_CHANGED",
    "RECOVERY_ACTION_CHANGED",
]

STATUS_NAMES = ["AVAILABLE", "EN_ROUTE", "ON_SCENE", "RESTING"]

RECOVERY_ACTION_NAMES = [
    "CONTINUE_MONITORING", "LOCAL_RECOVERY", "RETURN_TO_SAFE_ZONE",
    "REQUEST_TASK_REALLOCATION", "ABORT_MISSION",
]

# TEAM_NAMES[team_id % 5] gives the specialization (team.c: specialization = i % 5,
# since NUM_TEAMS=10 means 2 teams per specialization). Index directly for
# team_id < 5 only if NUM_TEAMS is ever reduced back to 5.
TEAM_NAMES = ["Search", "Medical", "Extraction", "Perimeter", "Support"]

TEAM_COLORS = ["#f85149", "#58a6ff", "#3fb950", "#d29922", "#bc8cff"]

INCIDENT_STATUS_NAMES = ["OPEN", "ASSIGNED", "IN_PROGRESS", "RESOLVED"]

PRIORITY_NAMES = {1: "CRITICAL", 2: "HIGH", 3: "MEDIUM", 4: "LOW"}

DIAG_STATUS_RE = re.compile(
    r"\[DIAG\] tick=(\d+) team=(\d+) status (\d)->(\d) stamina=([\d.]+) hours=(\d+)"
)
DIAG_RESTED_RE = re.compile(
    r"\[DIAG\] tick=(\d+) team=(\d+) RESTED ticks=(\d+) stamina ([\d.]+)->([\d.]+)"
)
DIAG_SUMMARY_RE = re.compile(
    r"\[DIAG SUMMARY\] safezone_fallback_count=(\d+) discarded_signal_count=(\d+)"
)
DIAG_WORK_RE = re.compile(
    r"\[DIAG WORK\] in_progress_entries=(\d+) work_progress_calls=(\d+)"
)
DIAG_THROUGHPUT_RE = re.compile(
    r"\[DIAG THROUGHPUT\] team=(\d+) incidents_resolved=(\d+) stage_completions=(\d+) resting=(\d+) working=(\d+) available=(\d+)"
)


def build_binary():
    """Runs `make` in the project dir. Returns (ok, output)."""
    try:
        result = subprocess.run(
            ["make"], cwd=PROJECT_DIR, capture_output=True, text=True, timeout=120
        )
        ok = result.returncode == 0
        return ok, result.stdout + result.stderr
    except Exception as exc:  # noqa: BLE001
        return False, str(exc)


def parse_diag(stderr_text):
    rested = []       # list of dicts: tick, team, ticks, stamina_before, stamina_after
    transitions = []  # list of dicts: tick, team, from, to, stamina, hours
    summary = {}
    work = {}
    throughput = {}

    for line in stderr_text.splitlines():
        m = DIAG_RESTED_RE.search(line)
        if m:
            rested.append({
                "tick": int(m.group(1)),
                "team": int(m.group(2)),
                "duration": int(m.group(3)),
                "stamina_before": float(m.group(4)),
                "stamina_after": float(m.group(5)),
            })
            continue

        m = DIAG_STATUS_RE.search(line)
        if m:
            transitions.append({
                "tick": int(m.group(1)),
                "team": int(m.group(2)),
                "from": int(m.group(3)),
                "to": int(m.group(4)),
                "stamina": float(m.group(5)),
                "hours": int(m.group(6)),
            })
            continue

        m = DIAG_SUMMARY_RE.search(line)
        if m:
            summary = {
                "safezone_fallback_count": int(m.group(1)),
                "discarded_signal_count": int(m.group(2)),
            }
            continue

        m = DIAG_WORK_RE.search(line)
        if m:
            work = {
                "in_progress_entries": int(m.group(1)),
                "work_progress_calls": int(m.group(2)),
            }
            continue

        m = DIAG_THROUGHPUT_RE.search(line)
        if m:
            t = int(m.group(1))
            throughput[t] = {
                "team": t,
                "team_name": f"{TEAM_NAMES[t % len(TEAM_NAMES)]} #{t}" if TEAM_NAMES else f"team{t}",
                "incidents_resolved": int(m.group(2)),
                "stage_completions": int(m.group(3)),
                "resting": int(m.group(4)),
                "working": int(m.group(5)),
                "available": int(m.group(6)),
            }

    # rest-cycle quality check: how many rest cycles got cut off after
    # a single tick (the historical symptom of the ABORT_MISSION bug)
    # vs. cycles that ran long enough to matter.
    interrupted = sum(1 for r in rested if r["duration"] <= 1)
    sustained = sum(1 for r in rested if r["duration"] >= 10)

    return {
        "rested_events": rested,
        "transitions_sample": transitions[-500:],  # cap payload size
        "summary": summary,
        "work": work,
        "throughput": [throughput[k] for k in sorted(throughput)],
        "rest_cycle_count": len(rested),
        "rest_cycles_interrupted_1tick": interrupted,
        "rest_cycles_sustained_10plus": sustained,
    }


def parse_csv(max_points_per_series=400):
    """Parses tron_snapshot_dataset.csv into chart-ready aggregates."""
    if not CSV_PATH.exists():
        return None

    stamina_by_team = defaultdict(list)   # team -> list of (cycle, stamina)
    event_counts = Counter()
    watchdog_anomalies = 0
    recovery_counts = Counter()
    fault_active_ticks = 0
    total_rows = 0

    with open(CSV_PATH, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            total_rows += 1
            try:
                team = int(row["team_id"])
                cycle = int(row["cycle"])
                stamina = float(row["stamina"])
            except (KeyError, ValueError):
                continue

            stamina_by_team[team].append((cycle, stamina))

            try:
                ev = int(row.get("event_type", 0))
                event_counts[EVENT_NAMES[ev] if ev < len(EVENT_NAMES) else str(ev)] += 1
            except ValueError:
                pass

            if row.get("watchdog_anomaly_detected") in ("1", "true", "True"):
                watchdog_anomalies += 1

            rec = row.get("recovery_recommendation", "")
            if rec not in ("", None):
                try:
                    rec_i = int(rec)
                    recovery_counts[
                        RECOVERY_ACTION_NAMES[rec_i] if rec_i < len(RECOVERY_ACTION_NAMES) else rec
                    ] += 1
                except ValueError:
                    pass

            fault_mask = row.get("fault_active_mask", "0")
            try:
                if int(fault_mask) != 0:
                    fault_active_ticks += 1
            except ValueError:
                pass

    # downsample each team's stamina series so the chart stays light
    stamina_series = {}
    for team, points in stamina_by_team.items():
        points.sort(key=lambda p: p[0])
        n = len(points)
        if n > max_points_per_series:
            step = n / max_points_per_series
            sampled = [points[int(i * step)] for i in range(max_points_per_series)]
        else:
            sampled = points
        name = f"{TEAM_NAMES[team % len(TEAM_NAMES)]} #{team}" if TEAM_NAMES else f"team{team}"
        stamina_series[name] = [{"cycle": c, "stamina": s} for c, s in sampled]

    return {
        "total_rows": total_rows,
        "stamina_series": stamina_series,
        "event_counts": dict(event_counts),
        "watchdog_anomalies": watchdog_anomalies,
        "recovery_counts": dict(recovery_counts),
        "fault_active_ticks": fault_active_ticks,
    }


def load_history():
    if HISTORY_PATH.exists():
        try:
            return json.loads(HISTORY_PATH.read_text())
        except json.JSONDecodeError:
            return []
    return []


def save_history(entry, keep=20):
    history = load_history()
    history.insert(0, entry)
    history = history[:keep]
    HISTORY_PATH.write_text(json.dumps(history, indent=2))
    return history


@app.route("/")
def index():
    return send_from_directory(app.template_folder, "index.html")


@app.route("/api/run", methods=["POST"])
def api_run():
    payload = request.get_json(force=True) or {}
    seed = int(payload.get("seed", 1))
    seed_count = int(payload.get("seed_count", 1))
    ticks = int(payload.get("ticks", 2000))
    fault_profile = payload.get("fault_profile", "none")

    build_ok, build_log = build_binary()
    if not build_ok:
        return jsonify({"error": "build failed", "build_log": build_log}), 500

    args = [str(BINARY), str(seed), str(seed_count), str(ticks)]
    if fault_profile and fault_profile != "none":
        args += ["--fault-profile", fault_profile]

    start = time.time()
    try:
        result = subprocess.run(
            args, cwd=PROJECT_DIR, capture_output=True, text=True, timeout=600
        )
    except subprocess.TimeoutExpired:
        return jsonify({"error": "simulation timed out after 600s"}), 500
    elapsed = time.time() - start

    diag = parse_diag(result.stderr)
    telemetry = parse_csv()

    run_record = {
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
        "seed": seed,
        "seed_count": seed_count,
        "ticks": ticks,
        "fault_profile": fault_profile,
        "elapsed_seconds": round(elapsed, 2),
        "returncode": result.returncode,
        "summary": diag.get("summary", {}),
        "rest_cycle_count": diag.get("rest_cycle_count", 0),
        "rest_cycles_interrupted_1tick": diag.get("rest_cycles_interrupted_1tick", 0),
        "rest_cycles_sustained_10plus": diag.get("rest_cycles_sustained_10plus", 0),
        "csv_rows": telemetry["total_rows"] if telemetry else 0,
    }
    history = save_history(run_record)

    return jsonify({
        "params": {
            "seed": seed, "seed_count": seed_count, "ticks": ticks,
            "fault_profile": fault_profile,
        },
        "elapsed_seconds": round(elapsed, 2),
        "returncode": result.returncode,
        "stdout_tail": result.stdout[-4000:],
        "stderr_tail": result.stderr[-2000:],
        "diag": diag,
        "telemetry": telemetry,
        "history": history,
    })


@app.route("/api/run-live", methods=["POST"])
def api_run_live():
    """Streams one Server-Sent Event per simulation tick as it happens,
    so a live map/status view can render in real time instead of
    waiting for the whole run to finish. Pass tick_delay_ms > 0 to make
    it watchable instead of flashing by.
    """
    payload = request.get_json(force=True) or {}
    seed = int(payload.get("seed", 1))
    ticks = int(payload.get("ticks", 500))
    fault_profile = payload.get("fault_profile", "none")
    tick_delay_ms = int(payload.get("tick_delay_ms", 150))

    build_ok, build_log = build_binary()
    if not build_ok:
        def err_stream():
            yield f"data: {json.dumps({'type': 'error', 'message': 'build failed', 'log': build_log})}\n\n"
        return Response(err_stream(), mimetype="text/event-stream")

    args = [str(BINARY), str(seed), "1", str(ticks),
            "--live", f"--tick-delay-ms={tick_delay_ms}"]
    if fault_profile and fault_profile != "none":
        args += ["--fault-profile", fault_profile]

    def generate():
        proc = subprocess.Popen(
            args, cwd=PROJECT_DIR, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1,
        )
        stderr_lines = []
        start = time.time()
        try:
            for line in proc.stdout:
                line = line.rstrip("\n")
                if not line:
                    continue
                if line.startswith("LIVEFRAME "):
                    frame = line[len("LIVEFRAME "):]
                    yield f"data: {json.dumps({'type': 'frame', 'frame': json.loads(frame)})}\n\n"
                elif line.startswith("STATICENV "):
                    env = line[len("STATICENV "):]
                    yield f"data: {json.dumps({'type': 'static_env', 'env': json.loads(env)})}\n\n"
                else:
                    stderr_lines.append(line)
                    if line.startswith("[DIAG"):
                        yield f"data: {json.dumps({'type': 'diag_line', 'line': line})}\n\n"
            proc.wait(timeout=30)
        except GeneratorExit:
            proc.kill()
            raise
        finally:
            if proc.poll() is None:
                proc.kill()

        elapsed = time.time() - start
        diag = parse_diag("\n".join(stderr_lines))
        telemetry = parse_csv()

        run_record = {
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "seed": seed, "seed_count": 1, "ticks": ticks,
            "fault_profile": fault_profile, "elapsed_seconds": round(elapsed, 2),
            "returncode": proc.returncode,
            "summary": diag.get("summary", {}),
            "rest_cycle_count": diag.get("rest_cycle_count", 0),
            "rest_cycles_interrupted_1tick": diag.get("rest_cycles_interrupted_1tick", 0),
            "rest_cycles_sustained_10plus": diag.get("rest_cycles_sustained_10plus", 0),
            "csv_rows": telemetry["total_rows"] if telemetry else 0,
        }
        history = save_history(run_record)

        yield f"data: {json.dumps({'type': 'done', 'diag': diag, 'telemetry': telemetry, 'history': history, 'elapsed_seconds': round(elapsed, 2)})}\n\n"

    return Response(stream_with_context(generate()), mimetype="text/event-stream",
                     headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.route("/api/history")
def api_history():
    return jsonify(load_history())


@app.route("/api/status")
def api_status():
    return jsonify({
        "project_dir": str(PROJECT_DIR),
        "makefile_found": (PROJECT_DIR / "Makefile").exists(),
        "binary_exists": BINARY.exists(),
        "csv_exists": CSV_PATH.exists(),
    })


if __name__ == "__main__":
    print(f"Project dir: {PROJECT_DIR}")
    print("Starting dashboard on http://127.0.0.1:5050")
    app.run(host="127.0.0.1", port=5050, debug=False)
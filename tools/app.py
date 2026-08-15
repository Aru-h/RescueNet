#!/usr/bin/env python3
"""
CONCORD / REMCE Unified Local Dashboard & Hardware Telemetry Bridge.

Run this from inside your tools directory with:

    python3 app.py

Then open http://127.0.0.1:5050 in a browser.

This unified Flask application combines:
  1. The local simulation harness (`make`, `./tron`, stderr diagnostic parsing,
     CSV telemetry aggregation, history logs, and SSE live streams).
  2. The hardware USB serial bridge for the STM32N6 board (USB Virtual COM
     connection, `READY` handshake parsing, `START` command transmission, and 
     real-time hardware snapshot queue ingestion).
"""

import csv
import json
import os
import queue
import re
import subprocess
import threading
import time
from collections import Counter, defaultdict
from pathlib import Path
import serial
from flask import Flask, Response, jsonify, render_template, request, stream_with_context

from telemetry_bridge import TelemetryFrameParser

TOOLS_DIR = Path(__file__).resolve().parent
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

RECOVERY_ACTION_NAMES = [
    "CONTINUE_MONITORING", "LOCAL_RECOVERY", "RETURN_TO_SAFE_ZONE",
    "REQUEST_TASK_REALLOCATION", "ABORT_MISSION",
]

TEAM_NAMES = ["Search", "Medical", "Extraction", "Perimeter", "Support"]

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


class TelemetryBridge:
    """Manages serial communication, packet parsing, and command transmission

    between the Flask backend and the target STM32N6 board.

    Board telemetry (platform/board/snapshot.c's ring drain) arrives as
    binary, magic-byte-framed wire frames per core/telemetry_frame.h -
    see telemetry_bridge.TelemetryFrameParser for the byte-stream
    scanning/resync logic, shared with the standalone bridge. Plain
    tm_printf debug text shares the same UART and is handled via the
    existing line-based READY/log path.
    """

    def __init__(self):
        self.port = "/dev/ttyACM0"
        self.baudrate = 115200
        self.serial_conn = None
        self.running = False
        self.nodes_ready = set()
        self.telemetry_queue = queue.Queue()
        self.thread = None
        self.mission_started = False
        self._parser = TelemetryFrameParser()
        self._text_buf = bytearray()

    def connect(self, port: str, baudrate: int) -> bool:
        """Opens the USB serial port connected to the STM32N6 board."""
        try:
            self.port = port
            self.baudrate = int(baudrate)
            self.serial_conn = serial.Serial(self.port, self.baudrate, timeout=0.1)
            self.running = True
            self.nodes_ready.clear()
            self.mission_started = False
            self.thread = threading.Thread(target=self._read_loop, daemon=True)
            self.thread.start()
            return True
        except Exception as exc:  # noqa: BLE001
            print(f"Failed to connect to serial port {self.port}: {exc}")
            return False

    def _handle_text_line(self, line: str):
        if not line:
            return

        # Parse READY handshake notifications from board nodes
        if "READY" in line:
            parts = line.split(":")
            node_id = parts[0].strip() if len(parts) > 1 else "STM32N6_Board"
            self.nodes_ready.add(node_id)
        else:
            print(f"[Board Log] {line}")

    def _consume_text_bytes(self, data: bytes):
        """Dispatch raw text bytes (anything not part of a valid binary
        telemetry frame) through the existing newline-delimited READY/log
        handling."""
        self._text_buf.extend(data)
        while b"\n" in self._text_buf:
            line_bytes, _, rest = self._text_buf.partition(b"\n")
            self._text_buf = bytearray(rest)
            self._handle_text_line(line_bytes.decode("utf-8", errors="ignore").strip())

    def _read_loop(self):
        """Background thread listening for incoming byte streams and parsing packets.

        Reads raw bytes (binary wire format, not line-delimited text) and
        scans for magic-byte-framed telemetry frames per
        core/telemetry_frame.h, resyncing on a checksum mismatch instead
        of crashing or discarding the whole stream. Decoded frames go on
        telemetry_queue as structured dicts; everything else (READY
        handshakes, general tm_printf debug output) flows through the
        same line-based log path as before.
        """
        while self.running and self.serial_conn and self.serial_conn.is_open:
            try:
                waiting = self.serial_conn.in_waiting
                chunk = self.serial_conn.read(waiting if waiting else 1)
                if not chunk:
                    continue

                for kind, payload in self._parser.feed(chunk):
                    if kind == "frame":
                        self.telemetry_queue.put(payload)
                    else:
                        self._consume_text_bytes(payload)

            except Exception as e:
                print(f"Serial read error: {e}")
                break

    def send_start(self) -> bool:
        """Transmit the explicit text/binary START command packet down the USB line."""
        if self.serial_conn and self.serial_conn.is_open:
            try:
                self.serial_conn.write(b"START\n")
                self.mission_started = True
                print("Sent START command packet to board.")
                return True
            except Exception as e:
                print(f"Failed to send START command: {e}")
        return False

    def disconnect(self):
        """Safely close serial connection and stop background threads."""
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
        self.serial_conn = None
        self.nodes_ready.clear()
        self.mission_started = False


# Global bridge singleton instance for hardware board session
bridge = TelemetryBridge()


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
    rested = []
    transitions = []
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

    interrupted = sum(1 for r in rested if r["duration"] <= 1)
    sustained = sum(1 for r in rested if r["duration"] >= 10)

    return {
        "rested_events": rested,
        "transitions_sample": transitions[-500:],
        "summary": summary,
        "work": work,
        "throughput": [throughput[k] for k in sorted(throughput)],
        "rest_cycle_count": len(rested),
        "rest_cycles_interrupted_1tick": interrupted,
        "rest_cycles_sustained_10plus": sustained,
    }


def parse_csv(max_points_per_series=400):
    if not CSV_PATH.exists():
        return None

    stamina_by_team = defaultdict(list)
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


# Flask Routes
@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/status")
def api_status():
    connected = bridge.serial_conn is not None and bridge.serial_conn.is_open
    expected_nodes = ["STM32N6_Board"]

    new_frames = []
    while not bridge.telemetry_queue.empty():
        new_frames.append(bridge.telemetry_queue.get())

    return jsonify({
        "project_dir": str(PROJECT_DIR),
        "makefile_found": (PROJECT_DIR / "Makefile").exists(),
        "binary_exists": BINARY.exists(),
        "csv_exists": CSV_PATH.exists(),
        "board": {
            "connected": connected,
            "port": bridge.port,
            "baudrate": bridge.baudrate,
            "nodes_ready": list(bridge.nodes_ready),
            "expected_nodes": expected_nodes,
            "all_ready": set(expected_nodes).issubset(bridge.nodes_ready),
            "mission_started": bridge.mission_started,
            "new_telemetry": new_frames,
        }
    })


@app.route("/api/board/connect", methods=["POST"])
def api_board_connect():
    data = request.get_json(force=True) or {}
    port = data.get("port", "/dev/ttyACM0")
    baudrate = data.get("baudrate", 115200)
    success = bridge.connect(port, baudrate)
    if success:
        return jsonify({"status": "success", "port": port})
    return jsonify({"status": "error", "message": "Failed to open serial port"}), 400


@app.route("/api/board/disconnect", methods=["POST"])
def api_board_disconnect():
    bridge.disconnect()
    return jsonify({"status": "success"})


@app.route("/api/board/launch", methods=["POST"])
def api_board_launch():
    success = bridge.send_start()
    if success:
        return jsonify({"status": "success"})
    return jsonify({"status": "error", "message": "Failed to send START command"}), 400


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


if __name__ == "__main__":
    print(f"Project dir: {PROJECT_DIR}")
    print("Starting unified dashboard on http://127.0.0.1:5050")
    app.run(host="127.0.0.1", port=5050, debug=False)
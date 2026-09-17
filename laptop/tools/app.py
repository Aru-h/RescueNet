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
import re
import struct
import subprocess
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

from flask import Flask, Response, jsonify, render_template, request, stream_with_context

# TelemetryBridge is the real-hardware bridge (serial port, handshake,
# override acks, team status reports, telemetry frames). app.py used to keep
# its own copy of the class; there is now only the one in telemetry_bridge.py.
from telemetry_bridge import TelemetryBridge
from sim_board_source import SimBoardSource

TOOLS_DIR = Path(__file__).resolve().parent
PROJECT_DIR = TOOLS_DIR.parent.parent
CSV_PATH = PROJECT_DIR / "tron_snapshot_dataset.csv"
HISTORY_PATH = PROJECT_DIR / ".dashboard_history.json"
BINARY = PROJECT_DIR / "tron"

# --- Headless / sim-only mode --------------------------------------------
# Explicit opt-in only (per this feature's own handoff doc: "don't
# silently replace the hardware path") - either SIM_ONLY=1 in the
# environment or --no-hardware on the command line. When active,
# `bridge` below is a SimBoardSource (laptop simulator only, see that
# module) instead of a TelemetryBridge (real serial/board hardware) -
# every other route is unchanged and reads `bridge` exactly the same
# way regardless of which one it is. Default (neither set) is the
# original hardware-only behavior, completely unaffected by any of
# this - SIM_ONLY defaults to False, bridge defaults to a real
# TelemetryBridge, unchanged from before this mode existed.
SIM_ONLY = os.environ.get("SIM_ONLY") == "1" or "--no-hardware" in sys.argv[1:]

# --- Wire format constants (mirrors core/mission_link_override.h) -------
#
# Not imported from telemetry_bridge.py: this prompt's file ownership is
# core/mission_link_override.h/.c plus this file and templates/index.html
# only (telemetry_bridge.py belongs to an earlier prompt's file set - see
# that module's own HandshakeCoordinator for the precedent this mirrors).
# Self-contained here for the same reason pack_handshake_req() is
# self-contained in telemetry_bridge.py rather than split across files.
#
# See core/mission_link_override.h's own "AUTHORITATIVE REGISTRY" comment
# (section 2, on OVERRIDE_MAGIC0/1) for the running list of every magic
# pair assigned across this project and the non-ASCII-first-byte
# invariant they all follow - this is a mirror of that header's values,
# not a fresh allocation, and must stay in sync with it.
#
# CORRECTION: an earlier draft of this file used 0x28/0x82 for this pair.
# 0x28 is ASCII '(' - printable, and every OTHER magic pair on this same
# USART1 wire (telemetry, roster/incident/fault/status, handshake) uses a
# first byte >0x7E specifically so it can never appear in the free-running
# ASCII tm_printf debug text also sharing that wire (see core/
# mission_link.h section 5's doc). 0x28 would have broken that resync
# guarantee for this message type alone. Corrected to 0x87/0x78 here and
# in core/mission_link_override.h together, not left silently mismatched
# between the two sides of the wire.

OVERRIDE_MAGIC0 = 0x87
OVERRIDE_MAGIC1 = 0x78
OVERRIDE_MAGIC = bytes((OVERRIDE_MAGIC0, OVERRIDE_MAGIC1))

# MissionLinkOverride_t: team_id (u8), incident_id (u32 LE) - packed,
# matches core/mission_link_override.h's __attribute__((packed)) struct.
_OVERRIDE_STRUCT = struct.Struct("<BI")
OVERRIDE_PAYLOAD_SIZE = _OVERRIDE_STRUCT.size
assert OVERRIDE_PAYLOAD_SIZE == 5
OVERRIDE_WIRE_SIZE = 2 + OVERRIDE_PAYLOAD_SIZE + 1  # 8


def _override_checksum(payload: bytes) -> int:
    """Same additive checksum scheme every mission-link-family wire
    format in this project uses - mirrors core/mission_link_override.c's
    override_checksum()."""
    return sum(payload) & 0xFF


def pack_override(team_id: int, incident_id: int) -> bytes:
    """Builds the laptop -> DK MissionLinkOverride_t wire frame -
    mirrors core/mission_link_override.c's mission_link_override_pack().
    The board answers with MissionLinkOverrideAck_t frames, parsed by
    telemetry_bridge.OverrideAckParser and tracked per team in
    bridge.overrides (see /api/board/override/status)."""
    payload = _OVERRIDE_STRUCT.pack(team_id, incident_id)
    return OVERRIDE_MAGIC + payload + bytes((_override_checksum(payload),))


app = Flask(__name__, static_folder="static", template_folder="templates")

# Index = EventType_t value in core/mission_state.h
EVENT_NAMES = [
    "NONE", "NEW_INCIDENT", "TEAM_ASSIGNED", "TEAM_ON_SCENE",
    "STAGE_COMPLETE", "TEAM_REST", "TEAM_AVAILABLE", "INCIDENT_RESOLVED",
    "FAULT_INJECTED", "FAULT_RECOVERED", "WATCHDOG_ANOMALY_CHANGED",
    "RECOVERY_ACTION_CHANGED", "TICK",
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


# Global data-source singleton every route below reads as `bridge`.
# Real hardware (TelemetryBridge) unless SIM_ONLY explicitly opted in,
# in which case it's the laptop-simulator-backed SimBoardSource
# instead - see the SIM_ONLY comment above and sim_board_source.py's
# own top comment for why this swap is safe: both classes expose the
# same public surface (serial_conn/nodes_ready/telemetry_queue/
# mission_started/handshake/connect/disconnect/send_start).
if SIM_ONLY:
    bridge = SimBoardSource(PROJECT_DIR, BINARY)
else:
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
@app.route("/bench/a")
def bench_a():
    return render_template("bench_a.html")


@app.route("/bench/b")
def bench_b():
    return render_template("bench_b.html")


@app.route("/bench/c")
def bench_c():
    return render_template("bench_c.html")


@app.route("/bench/d")
def bench_d():
    return render_template("bench_d.html")


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/status")
def api_status():
    connected = bridge.serial_conn is not None and bridge.serial_conn.is_open

    new_frames = []
    while not bridge.telemetry_queue.empty():
        new_frames.append(bridge.telemetry_queue.get())

    return jsonify({
        "project_dir": str(PROJECT_DIR),
        "makefile_found": (PROJECT_DIR / "Makefile").exists(),
        "binary_exists": BINARY.exists(),
        "csv_exists": CSV_PATH.exists(),
        "sim_only": SIM_ONLY,
        "board": {
            "connected": connected,
            "port": bridge.port,
            "baudrate": bridge.baudrate,
            "nodes_ready": list(bridge.nodes_ready),
            "mission_started": bridge.mission_started,
            "new_telemetry": new_frames,
            # Mission-link liveness handshake (core/mission_link_
            # handshake.h) - this is what "all_ready"/launch-readiness
            # actually means now: replies confirmed for board_id 0
            # (DK) AND every peer board (relayed through DK) the
            # HandshakeCoordinator.NUM_BOARDS constant expects, not the
            # single hardcoded-node-name text check this used to be.
            # See telemetry_bridge.HandshakeCoordinator's docstring for
            # the full sequence and the retry/timeout/DK-only-override
            # policy behind peer_timed_out / dk_only_override /
            # can_proceed below.
            "handshake": bridge.handshake.status_dict(),
            # Latest MissionLinkStatus_t per team (board 0's own teams plus
            # the ones it forwards), with age_sec/stale - see
            # telemetry_bridge.TeamStatusTracker. Empty in sim-only mode.
            "team_status": (bridge.team_status.all()
                            if hasattr(bridge, "team_status") else []),
            # Mission feed progress (frames sent/pending, open incidents) -
            # None until /api/board/launch starts one.
            "mission": (bridge.mission_status()
                        if hasattr(bridge, "mission_status") else None),
            # Kept for any existing caller still reading this top-level
            # key directly: now derived from the real handshake
            # (can_proceed) instead of the old single-node text check,
            # so "start simulation" gating is correct rather than
            # assumed. New code should read board.handshake instead,
            # which has the finer-grained per-board/timeout detail
            # this single boolean can't express.
            "all_ready": bridge.handshake.can_proceed,
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


@app.route("/api/board/connect-sim", methods=["POST"])
def api_board_connect_sim():
    """Sim-mode equivalent of /api/board/connect above - starts
    `tron --live` as bridge's data source instead of opening a serial
    port (see SimBoardSource.connect()). Only reachable when app.py
    was started with SIM_ONLY=1 / --no-hardware: this is the "explicit
    opt-in, never silently replace the hardware path" boundary the
    SIM_ONLY comment above describes, enforced here rather than by
    trusting the caller not to hit this route by mistake in normal
    (hardware) mode.
    """
    if not SIM_ONLY:
        return jsonify({
            "status": "error",
            "message": "Sim mode is not enabled for this app.py process "
                       "- restart with SIM_ONLY=1 or --no-hardware",
        }), 400

    data = request.get_json(force=True) or {}
    seed = int(data.get("seed", 1))
    ticks = int(data.get("ticks", 2000))
    fault_profile = data.get("fault_profile", "none")
    tick_delay_ms = int(data.get("tick_delay_ms", 0))

    success = bridge.connect(
        seed=seed, ticks=ticks, fault_profile=fault_profile,
        tick_delay_ms=tick_delay_ms,
    )
    if success:
        return jsonify({
            "status": "success",
            "seed": seed, "ticks": ticks,
            "fault_profile": fault_profile, "tick_delay_ms": tick_delay_ms,
        })
    return jsonify({"status": "error", "message": "Failed to start sim run (build failed or tron did not start - check server log)"}), 400


@app.route("/api/board/disconnect", methods=["POST"])
def api_board_disconnect():
    bridge.disconnect()
    return jsonify({"status": "success"})


@app.route("/api/board/launch", methods=["POST"])
def api_board_launch():
    """Starts the mission on the boards: START, then the board mission (see
    telemetry_bridge.MissionFeed) - the laptop runs the world and the clock,
    the boards run the teams, and the dashboard shows what the boards report
    (GET /api/board/mission/stream). Optional JSON body: seed, ticks,
    fault_profile, and the pace - time_scale (1 = real time, one tick per
    36 s; the default) or an explicit tick_delay_ms."""
    data = request.get_json(silent=True) or {}
    try:
        seed = int(data.get("seed", 1))
        ticks = int(data.get("ticks", 10000))
        tick_delay_ms = data.get("tick_delay_ms")
        tick_delay_ms = None if tick_delay_ms is None else int(tick_delay_ms)
        time_scale = float(data.get("time_scale", 1.0))
    except (TypeError, ValueError):
        return jsonify({"status": "error",
                        "message": "seed, ticks, tick_delay_ms and time_scale must be numbers"}), 400
    if not time_scale > 0 or (tick_delay_ms is not None and tick_delay_ms < 0):
        return jsonify({"status": "error",
                        "message": "time_scale must be positive and tick_delay_ms not negative"}), 400
    fault_profile = str(data.get("fault_profile", "none"))

    success = bridge.send_start()
    if not success:
        return jsonify({
            "status": "error",
            "message": "Failed to send START command",
            "handshake": bridge.handshake.status_dict(),
        }), 400

    if not hasattr(bridge, "start_mission"):
        return jsonify({"status": "success"})  # sim-only source has no boards to feed

    build_ok, build_log = build_binary()
    if not build_ok:
        return jsonify({"status": "error", "message": "build failed", "build_log": build_log}), 500

    ok, message = bridge.start_mission(BINARY, seed=seed, ticks=ticks,
                                       tick_delay_ms=tick_delay_ms, fault_profile=fault_profile,
                                       time_scale=time_scale)
    if not ok:
        return jsonify({"status": "error", "message": f"Mission feed not started: {message}"}), 400
    return jsonify({"status": "success", "mission": bridge.mission_status()})


@app.route("/api/board/mission/stream")
def api_board_mission_stream():
    """Server-sent events for the running board mission, in the same shape
    /api/run-live streams (static_env, then frame messages), so the Live Ops
    view renders it unchanged. Each frame is the laptop's world with every
    team as its board last reported it. Ends when the mission stops."""
    mission = getattr(bridge, "mission", None)

    def generate():
        if mission is None:
            yield f"data: {json.dumps({'type': 'error', 'message': 'no board mission running'})}\n\n"
            return
        sent_env = None
        last_seq = 0
        idle_since = time.time()
        while getattr(bridge, "mission", None) is mission:
            done = mission.finished  # read first: nothing new can follow it
            env, frames = mission.live_events(last_seq)
            if env is not None and env is not sent_env:
                sent_env = env
                yield f"data: {json.dumps({'type': 'static_env', 'env': env})}\n\n"
            for seq, frame in frames:
                last_seq = seq
                yield f"data: {json.dumps({'type': 'frame', 'frame': frame})}\n\n"
            if frames:
                idle_since = time.time()
            elif time.time() - idle_since > 15:
                # Keeps the connection from being closed as idle between
                # real-time ticks (36 s apart).
                yield ": keepalive\n\n"
                idle_since = time.time()
            if done and not frames:
                break
            time.sleep(0.25)
        yield f"data: {json.dumps({'type': 'mission_end', 'mission': mission.status_dict()})}\n\n"

    return Response(stream_with_context(generate()), mimetype="text/event-stream",
                    headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.route("/api/board/mission/stop", methods=["POST"])
def api_board_mission_stop():
    """Stops sending mission frames (the boards keep what they have)."""
    if hasattr(bridge, "stop_mission"):
        bridge.stop_mission()
    return jsonify({"status": "success"})


@app.route("/api/board/continue-without-nucleo", methods=["POST"])
def api_board_continue_without_nucleo():
    """Operator-driven escape hatch for the case core/mission_link_
    handshake.c's top comment (timeout/retry policy) and
    telemetry_bridge.HandshakeCoordinator's docstring both call out
    explicitly: NUCLEO never replies (LoRa module not flashed yet,
    still on the bench, etc.). Does NOT let the laptop block forever
    (a peer board timing out gives the dashboard a concrete state to show
    instead of an indefinite spinner) and does NOT silently proceed
    DK-only either (a demo running silently degraded is worse than one
    that makes the operator confirm it) - this route is the explicit
    confirmation step in between. A no-op (see
    HandshakeCoordinator.request_dk_only_override()) if board 1 hasn't
    actually timed out yet or board 0 isn't ready, so calling it early
    can't be used to skip the check.
    """
    overridden = bridge.handshake.request_dk_only_override()
    if overridden:
        return jsonify({"status": "success", "handshake": bridge.handshake.status_dict()})
    return jsonify({
        "status": "error",
        "message": "Override not applicable yet (board 0 not ready, or board 1 hasn't timed out)",
        "handshake": bridge.handshake.status_dict(),
    }), 400


@app.route("/api/board/override", methods=["POST"])
def api_board_override():
    """The Flask route core/mission_link_override.h's pipeline doc
    (step 1-2) describes: the dashboard's drag-drop already builds a
    {team_id, incident_id} pair on confirm (laptop/tools/templates/index.html)
    - this route packs it into a MissionLinkOverride_t wire frame and
    writes it to USART1 (bridge.serial_conn), the laptop's only channel.

    It does NOT decide DK-vs-NUCLEO routing itself - see that header's
    own top comment, step 3: DK's board-side mission-link layer
    (core/mission_link_override.c's mission_link_override_route_or_
    apply(), hooked into platform/board/mission_link_task.c's RX loop
    per that header's handoff note) is what actually decides, once the
    frame arrives, whether the team lives on this board or needs
    relaying to NUCLEO. This route's only job is getting the directive
    onto the one wire the laptop has.

    Gated on bridge.handshake.can_proceed, same as send_start(): there
    is no point addressing an override at a board that hasn't even
    confirmed it's alive and past its own init yet (see
    telemetry_bridge.HandshakeCoordinator's docstring) - an override
    written into that silence would have exactly the same "channel
    nothing listens on" problem core/mission_link_handshake.h's own
    top comment was written to fix in the first place.
    """
    data = request.get_json(force=True) or {}

    try:
        team_id = int(data["team_id"])
        incident_id = int(data["incident_id"])
    except (KeyError, TypeError, ValueError):
        return jsonify({
            "status": "error",
            "message": "Request body must include integer team_id and incident_id",
        }), 400

    if not (0 <= team_id <= 255):
        return jsonify({"status": "error", "message": "team_id out of range (0-255)"}), 400
    if not (0 <= incident_id <= 0xFFFFFFFF):
        return jsonify({"status": "error", "message": "incident_id out of range (u32)"}), 400

    if not bridge.handshake.can_proceed:
        return jsonify({
            "status": "error",
            "message": "Board handshake not complete - cannot send override yet",
            "handshake": bridge.handshake.status_dict(),
        }), 400

    if not (bridge.serial_conn and bridge.serial_conn.is_open):
        return jsonify({"status": "error", "message": "Board not connected"}), 400

    try:
        frame = pack_override(team_id, incident_id)
        if hasattr(bridge, "write_bytes"):
            bridge.write_bytes(frame)  # shares the serial write lock
        else:
            bridge.serial_conn.write(frame)
    except Exception as exc:  # noqa: BLE001
        return jsonify({"status": "error", "message": f"Failed to write to serial port: {exc}"}), 500

    # "success" here only means the bytes left the laptop. Whether a board
    # applied it comes back as acks - poll /api/board/override/status.
    bridge.overrides.note_sent(team_id, incident_id)
    return jsonify({"status": "success", "team_id": team_id, "incident_id": incident_id,
                    "override": bridge.overrides.get(team_id)})


@app.route("/api/board/override/status")
def api_board_override_status():
    """What the boards have acknowledged for each team's latest override:
    sent, relayed, then applied / rejected_not_ready / rejected_bad_team /
    relay_failed, or unconfirmed if no final ack arrived in time (see
    telemetry_bridge.OverrideTracker). ?team_id=N returns just that team."""
    tracker = getattr(bridge, "overrides", None)
    if tracker is None:
        return jsonify({"status": "error", "message": "No board override tracking in this mode"}), 400

    team_arg = request.args.get("team_id")
    if team_arg is None:
        return jsonify({"status": "success", "overrides": tracker.all()})

    try:
        team_id = int(team_arg)
    except ValueError:
        return jsonify({"status": "error", "message": "team_id must be an integer"}), 400

    entry = tracker.get(team_id)
    if entry is None:
        return jsonify({"status": "error", "message": f"No override sent for team {team_id}"}), 404
    return jsonify({"status": "success", "override": entry})


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

    args = [str(BINARY), str(seed), str(seed_count), str(ticks), "--trace", "--diag"]
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
            "--live", "--trace", "--diag", f"--tick-delay-ms={tick_delay_ms}"]
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
    if SIM_ONLY:
        print("Starting dashboard in SIM-ONLY mode: no serial port will be "
              "opened, no board hardware required. POST /api/board/"
              "connect-sim to start a simulated run "
              "(seed/ticks/fault_profile/tick_delay_ms).")
    else:
        print("Starting dashboard in hardware mode (default) - "
              "/api/board/connect opens a real serial port. Start with "
              "SIM_ONLY=1 or --no-hardware for headless/hardware-free mode.")
    print("Starting unified dashboard on http://127.0.0.1:5050")
    app.run(host="127.0.0.1", port=5050, debug=False)
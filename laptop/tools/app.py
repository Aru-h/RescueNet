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
import struct
import subprocess
import sys
import threading
import time
from collections import Counter, defaultdict
from pathlib import Path

try:
    import serial
except ImportError:
    # pyserial is only actually needed by the real hardware path
    # (TelemetryBridge.connect() below). Importing it unconditionally
    # would make app.py fail to even start in an environment that
    # deliberately doesn't have pyserial installed - exactly the
    # "no pyserial port required at all" bar the headless/SIM_ONLY
    # mode is supposed to meet (see SIM_ONLY below and
    # sim_board_source.py's own top comment). TelemetryBridge.connect()
    # checks for this and fails clearly, rather than at import time,
    # if hardware mode is actually attempted without it installed.
    serial = None

from flask import Flask, Response, jsonify, render_template, request, stream_with_context

from telemetry_bridge import HandshakeCoordinator, TelemetryFrameParser, pack_handshake_req
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
    The laptop only ever sends this shape (there is no reply - see that
    header's own top comment: "this is a one-shot directive, not
    something that expects a reply"), so this module has no need for a
    parser, only this packer."""
    payload = _OVERRIDE_STRUCT.pack(team_id, incident_id)
    return OVERRIDE_MAGIC + payload + bytes((_override_checksum(payload),))


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
    scanning/resync logic, shared with the standalone bridge. The
    mission-link liveness handshake (core/mission_link_handshake.h)
    arrives the same way, via telemetry_bridge.HandshakeCoordinator -
    see that class's docstring for the full request/reply sequence and
    retry/timeout policy this delegates to (this class does not
    reimplement it - see this file's history for why a duplicate
    implementation here would have been the wrong move: it existed
    only because `nodes_ready`/`all_ready` predate that class, not
    because this app's needs are actually different from the
    standalone bridge's). Plain tm_printf debug text shares the same
    UART and is handled via the existing line-based READY/log path.
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
        self.handshake = HandshakeCoordinator()

    def connect(self, port: str, baudrate: int) -> bool:
        """Opens the USB serial port connected to the STM32N6 board."""
        if serial is None:
            print("Failed to connect: pyserial is not installed "
                  "(pip install pyserial, or run app.py in sim-only "
                  "mode instead - see SIM_ONLY/--no-hardware)")
            return False
        try:
            self.port = port
            self.baudrate = int(baudrate)
            self.serial_conn = serial.Serial(self.port, self.baudrate, timeout=0.1)
            self.running = True
            self.nodes_ready.clear()
            self.mission_started = False
            self.handshake.start()
            self.thread = threading.Thread(target=self._read_loop, daemon=True)
            self.thread.start()
            return True
        except Exception as exc:  # noqa: BLE001
            print(f"Failed to connect to serial port {self.port}: {exc}")
            return False

    def _handle_text_line(self, line: str):
        if not line:
            return

        # Legacy plain-text READY notifications. Superseded by the
        # binary mission-link handshake (self.handshake) for actually
        # gating readiness/launch decisions - see this class's
        # docstring - but still recorded here too for the debug-log
        # view, since a board's tm_printf output isn't going away.
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

        Reads raw bytes (binary wire format, not line-delimited text)
        and scans for magic-byte-framed frames of both kinds this wire
        carries: handshake replies first (self.handshake.feed(), see
        telemetry_bridge.HandshakeCoordinator), then telemetry frames
        per core/telemetry_frame.h on whatever's left
        (self._parser.feed()) - same chaining
        telemetry_bridge.TelemetryBridge._read_loop uses, and for the
        same reason (see that method's docstring). Decoded telemetry
        frames go on telemetry_queue as structured dicts; decoded
        handshake replies update self.handshake directly, read
        synchronously by /api/status; everything else (READY
        handshakes, general tm_printf debug output) flows through the
        same line-based log path as before.

        Also drives the handshake's retry timer the same way
        telemetry_bridge.TelemetryBridge._read_loop does, piggybacked
        on this loop's existing ~10Hz (0.1s read timeout) cadence.
        """
        while self.running and self.serial_conn and self.serial_conn.is_open:
            try:
                if self.handshake.due_for_req():
                    self.serial_conn.write(pack_handshake_req(0))
                    self.handshake.note_req_sent()

                waiting = self.serial_conn.in_waiting
                chunk = self.serial_conn.read(waiting if waiting else 1)
                if not chunk:
                    continue

                leftover = self.handshake.feed(chunk)

                for kind, payload in self._parser.feed(leftover):
                    if kind == "frame":
                        self.telemetry_queue.put(payload)
                    else:
                        self._consume_text_bytes(payload)

            except Exception as e:
                print(f"Serial read error: {e}")
                break

    def send_start(self) -> bool:
        """Transmit the explicit text/binary START command packet down the USB line.

        Gated on self.handshake.can_proceed - see
        telemetry_bridge.TelemetryBridge.send_start()'s docstring for
        why (both boards ready, or board 0 ready plus an explicit
        continue-without-NUCLEO override - see
        /api/board/continue-without-nucleo below) and for what this
        gate does and does not fix (the board-side START protocol
        itself remains a stub; this only stops the command from being
        sent before it's safe to).
        """
        if not self.handshake.can_proceed:
            print("Refusing to send START: handshake not complete "
                  f"({self.handshake.status_dict()})")
            return False

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
        self.handshake = HandshakeCoordinator()


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
    success = bridge.send_start()
    if success:
        return jsonify({"status": "success"})
    return jsonify({
        "status": "error",
        "message": "Failed to send START command",
        "handshake": bridge.handshake.status_dict(),
    }), 400


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
        bridge.serial_conn.write(pack_override(team_id, incident_id))
    except Exception as exc:  # noqa: BLE001
        return jsonify({"status": "error", "message": f"Failed to write to serial port: {exc}"}), 500

    return jsonify({"status": "success", "team_id": team_id, "incident_id": incident_id})


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
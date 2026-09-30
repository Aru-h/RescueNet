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
import gzip
import json
import os
import re
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
from collections import Counter, defaultdict
from pathlib import Path
from urllib.parse import urlsplit

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
# TRON_BINARY overrides the simulator binary (default ./tron, AI off). Relative
# paths resolve against PROJECT_DIR, not the cwd. For the AI build:
#   make tron-ai && TRON_BINARY=./tron_ai SIM_ONLY=1 python laptop/tools/app.py
BINARY = Path(os.environ.get("TRON_BINARY", PROJECT_DIR / "tron"))
if not BINARY.is_absolute():
    BINARY = PROJECT_DIR / BINARY

# Part 2 (replay): every line a live run forwards over SSE (STATICENV
# once, LIVEFRAME every tick, STATICMAP on change, and OVERRIDE/
# OVERRIDEACK once Part 4 lands) is also saved here, gzip-compressed,
# so the dashboard can play a run back later without the simulator or a
# board in the room. See _RecordingWriter below and /api/recordings*.
# Gitignored except one curated demo file - see .gitignore.
#
# Overridable via DASHBOARD_RECORDINGS_DIR so dashboard_e2e_selftest.py
# can point this at a throwaway temp directory instead of littering the
# real recordings/ folder (and the one curated demo file it holds)
# with test runs on every check-dashboard-e2e invocation.
RECORDINGS_DIR = Path(os.environ.get("DASHBOARD_RECORDINGS_DIR", str(TOOLS_DIR / "recordings")))
RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)
_RECORDING_NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+\.jsonl\.gz$")

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

# Where the dashboard listens (see app.run() at the bottom of this file).
# Loopback only - but loopback is NOT a trust boundary against the
# user's own browser: any web page open in it can POST to
# http://127.0.0.1:5050, and every POST route here does something real
# (a dispatch override written to the boards, a mission launch, a serial
# port opened, a 600 s sim run started). See _reject_cross_site_writes().
DASHBOARD_HOST = "127.0.0.1"
DASHBOARD_PORT = 5050

# Origins the dashboard's own pages are served from. localhost and
# 127.0.0.1 are both listed because the user may type either into the
# address bar; each is a distinct origin to the browser.
ALLOWED_ORIGINS = frozenset({
    f"http://{DASHBOARD_HOST}:{DASHBOARD_PORT}",
    f"http://127.0.0.1:{DASHBOARD_PORT}",
    f"http://localhost:{DASHBOARD_PORT}",
})

_STATE_CHANGING_METHODS = frozenset({"POST", "PUT", "PATCH", "DELETE"})


def _origin_of(url):
    """scheme://host[:port] of a URL (the form an Origin header takes),
    lower-cased; "" if it can't be parsed."""
    try:
        parts = urlsplit(url)
    except ValueError:
        return ""
    if not parts.scheme or not parts.netloc:
        return ""
    return f"{parts.scheme}://{parts.netloc}".lower()


@app.before_request
def _reject_cross_site_writes():
    """Cross-site request forgery guard for every state-changing route.

    Two independent checks, either of which is enough against a hostile
    page in the user's browser:

    1. Origin/Referer. A browser always attaches Origin to a cross-origin
       POST (and usually Referer), so a request that carries one naming a
       site other than this dashboard is refused outright. Requests with
       neither header (curl, the selftests, the Python tooling) are not
       browser cross-site requests and fall through to check 2.

    2. Content-Type: application/json is required. The routes used to
       parse the body whatever its content type, so a CORS-"simple"
       text/plain POST - which a browser sends cross-site with no
       preflight - went straight through. A JSON content type is not
       CORS-simple: the browser must preflight it first, and this server
       never answers a preflight with Access-Control-Allow-Origin, so the
       real request is never sent.

    The dashboard's own JS (templates/, static/) sends JSON with the
    matching header on every POST, including the bodyless ones.
    """
    if request.method not in _STATE_CHANGING_METHODS:
        return None

    origin = request.headers.get("Origin")
    if origin is not None:
        if origin.lower() not in ALLOWED_ORIGINS:
            return jsonify({"status": "error",
                            "message": "Cross-origin request refused"}), 403
    else:
        referer = request.headers.get("Referer")
        if referer and _origin_of(referer) not in ALLOWED_ORIGINS:
            return jsonify({"status": "error",
                            "message": "Cross-origin request refused"}), 403

    if not request.is_json:
        return jsonify({"status": "error",
                        "message": "Content-Type must be application/json"}), 415
    return None


def _json_object_body():
    """The request's JSON body as a dict: {} for an empty body, None if
    the body is not a JSON object (caller answers 400). Content-Type was
    already enforced by _reject_cross_site_writes()."""
    if not request.get_data(cache=True):
        return {}
    data = request.get_json(silent=True)
    return data if isinstance(data, dict) else None


def _bad_json_body():
    return jsonify({"status": "error",
                    "message": "Request body must be a JSON object"}), 400

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


def _git_commit():
    """The current commit, for a recording's header (Part 5 will compare
    this against the commit a recording was made on to warn about a
    possible divergence on take-over). "unknown" rather than raising if
    git isn't available or this isn't a checkout - a recording is still
    useful without it."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=PROJECT_DIR,
            capture_output=True, text=True, timeout=5,
        )
        return result.stdout.strip() if result.returncode == 0 else "unknown"
    except Exception:  # noqa: BLE001
        return "unknown"


_MODEL_LOADED_CACHE = {}


def _model_loaded():
    """True/False: was the simulator binary built with the AI fault predictor
    (FAULT_MODEL_LOADED=1)? Asked of the binary itself (`--model-info`), so it
    is right for whatever TRON_BINARY points at; TRON_MODEL_LOADED=0/1
    overrides it. None if it cannot be determined (e.g. an older binary)."""
    override = os.environ.get("TRON_MODEL_LOADED")
    if override in ("0", "1"):
        return override == "1"
    key = str(BINARY)
    if key not in _MODEL_LOADED_CACHE:
        value = None
        try:
            with tempfile.TemporaryDirectory() as scratch:
                result = subprocess.run([key, "--model-info"], cwd=scratch,
                                        capture_output=True, text=True, timeout=5)
            match = re.search(r"model_loaded=([01])", result.stdout)
            if match:
                value = match.group(1) == "1"
        except Exception:  # noqa: BLE001
            pass
        _MODEL_LOADED_CACHE[key] = value
    return _MODEL_LOADED_CACHE[key]


def _recording_filename(seed, fault_profile, link_loss):
    """File name includes seed, profile and link loss (spec) so a
    directory listing is somewhat readable without opening any file;
    the timestamp keeps repeated runs with the same parameters from
    colliding. Sanitized: fault_profile is user-supplied (the fault-
    profile <select>) and must not be able to escape RECORDINGS_DIR or
    inject path separators."""
    safe_profile = re.sub(r"[^A-Za-z0-9]+", "", str(fault_profile) or "none") or "none"
    loss_str = "default" if link_loss is None else f"{link_loss:g}".replace(".", "p")
    ts = time.strftime("%Y%m%d-%H%M%S")
    return f"recording_seed{seed}_{safe_profile}_loss{loss_str}_{ts}.jsonl.gz"


class _RecordingWriter:
    """Saves every line a live run forwards to a gzip file under
    RECORDINGS_DIR, first line a RECORDING header (seed, fault profile,
    ticks, link loss, tick of start, git commit) - see CONSENSUS_
    DESIGN.md's dashboard handoff notes and /api/recordings* below for
    the read side. A run that errors or is stopped early still leaves a
    valid (shorter) recording behind: close() is always called from
    /api/run-live's `finally` block.
    """

    def __init__(self, seed, fault_profile, ticks, link_loss, start_tick=0, extra=None):
        self.path = RECORDINGS_DIR / _recording_filename(seed, fault_profile, link_loss)
        self._fh = gzip.open(self.path, "wt")
        header = {
            "type": "recording_header",
            "seed": seed,
            "fault_profile": fault_profile,
            "ticks": ticks,
            "link_loss": link_loss,
            "start_tick": start_tick,
            "git_commit": _git_commit(),
            "model_loaded": _model_loaded(),
            "created": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        if extra:
            header.update(extra)
        self._fh.write("RECORDING " + json.dumps(header) + "\n")

    def write_line(self, line):
        self._fh.write(line + "\n")

    def close(self):
        try:
            self._fh.close()
        except Exception:  # noqa: BLE001
            pass


def _read_recording_header(path):
    """Just the first line's parsed JSON - cheap enough to do for every
    file in a directory listing without decompressing the whole thing."""
    try:
        with gzip.open(path, "rt") as f:
            first = f.readline()
    except OSError:
        return {}
    if not first.startswith("RECORDING "):
        return {}
    try:
        return json.loads(first[len("RECORDING "):])
    except json.JSONDecodeError:
        return {}


@app.route("/api/recordings")
def api_recordings():
    """Lists saved recordings, newest first, with their header fields -
    what the Replay mode selector in index.html populates itself from."""
    items = []
    for path in RECORDINGS_DIR.glob("*.jsonl.gz"):
        header = _read_recording_header(path)
        items.append({
            "filename": path.name,
            "size_bytes": path.stat().st_size,
            "mtime": path.stat().st_mtime,
            **{k: v for k, v in header.items() if k != "type"},
        })
    items.sort(key=lambda it: it["mtime"], reverse=True)
    return jsonify(items)


@app.route("/api/recordings/<name>")
def api_recording_file(name):
    """The full decompressed recording, for the client to parse ONCE
    into an in-memory frame array (see static/dashboard/replay.js) -
    every later scrub/play/jump is then array indexing, not a re-fetch.
    `name` is checked against a strict allow-list pattern (no `/`, no
    `..`) before touching the filesystem, so this can't be used to read
    anything outside RECORDINGS_DIR."""
    if not _RECORDING_NAME_RE.match(name):
        return jsonify({"status": "error", "message": "invalid recording name"}), 400
    path = RECORDINGS_DIR / name
    if not path.is_file():
        return jsonify({"status": "error", "message": "recording not found"}), 404
    with gzip.open(path, "rt") as f:
        content = f.read()
    return Response(content, mimetype="text/plain")


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
    data = _json_object_body()
    if data is None:
        return _bad_json_body()
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

    data = _json_object_body()
    if data is None:
        return _bad_json_body()
    try:
        seed = int(data.get("seed", 1))
        ticks = int(data.get("ticks", 2000))
        tick_delay_ms = int(data.get("tick_delay_ms", 0))
    except (TypeError, ValueError):
        return jsonify({"status": "error",
                        "message": "seed, ticks and tick_delay_ms must be integers"}), 400
    fault_profile = data.get("fault_profile", "none")

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
    data = _json_object_body()
    if data is None:
        return _bad_json_body()
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

    Note: overriding a team onto a system task (not a real incident)
    is not supported on this path - it will ack "applied" and then
    silently self-clear a tick later (core/cbba.c's cbba_apply_
    override_pin() own note explains why). Unlike the sim-only --live
    path (laptop/debug.c), this route has no pre-check for it.
    """
    data = _json_object_body()
    if data is None:
        return _bad_json_body()

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
    payload = _json_object_body()
    if payload is None:
        return _bad_json_body()
    try:
        seed = int(payload.get("seed", 1))
        seed_count = int(payload.get("seed_count", 1))
        ticks = int(payload.get("ticks", 2000))
    except (TypeError, ValueError):
        return jsonify({"error": "seed, seed_count and ticks must be integers"}), 400
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


# The one live run in flight (the tron process /api/run-live is streaming),
# so /api/run-live/pause, /resume and /stop can reach it. Pause and resume
# are SIGSTOP/SIGCONT: the tick loop simply freezes and continues, with its
# world state intact. Guarded by a lock because Flask serves these routes
# and the streaming generator on different threads.
_live_lock = threading.Lock()
_live_proc = None
_live_paused = False
# Every line written to the live run's stdin, in order. The simulator
# answers each non-empty line with exactly one OVERRIDEACK, in order, so
# the n-th ack belongs to the n-th entry here; that pairing is how an
# applied ack (which does not say whether it was a set or a clear) is
# recorded as a replayable OVERRIDE / OVERRIDE_CLEAR line.
_live_sent_cmds = []
# Part 4: the most recent tick this live run's own LIVEFRAME output
# reported, so /api/run-live/override can stamp a request with "as of
# what world state" without the browser needing to track and resend
# it. Not lock-guarded like _live_proc/_live_paused above: it's a
# single int written once per tick by generate()'s own thread and read
# by Flask's request threads, which is GIL-atomic enough for this - a
# few ticks of staleness on a fast read is harmless (laptop/debug.c's
# own staleness window is 20 ticks), unlike _live_proc/_live_paused,
# where a torn read/write could apply a pause to a process that's
# already gone.
_live_last_tick = 0


def _live_signal(action):
    """Applies pause/resume/stop to the live run. Returns (body, status)."""
    global _live_paused
    with _live_lock:
        proc = _live_proc
        if proc is None or proc.poll() is not None:
            return {"ok": False, "error": "no live run in progress"}, 409
        if action != "stop" and not hasattr(signal, "SIGSTOP"):
            return {"ok": False, "error": "pause/resume not supported on this platform"}, 501
        if action == "pause":
            proc.send_signal(signal.SIGSTOP)
            _live_paused = True
        elif action == "resume":
            proc.send_signal(signal.SIGCONT)
            _live_paused = False
        else:
            proc.kill()   # SIGKILL also ends a stopped process
            _live_paused = False
        return {"ok": True, "action": action, "paused": _live_paused}, 200


@app.route("/api/run-live/pause", methods=["POST"])
def api_run_live_pause():
    body, status = _live_signal("pause")
    return jsonify(body), status


@app.route("/api/run-live/resume", methods=["POST"])
def api_run_live_resume():
    body, status = _live_signal("resume")
    return jsonify(body), status


@app.route("/api/run-live/stop", methods=["POST"])
def api_run_live_stop():
    body, status = _live_signal("stop")
    return jsonify(body), status


def _live_write_line(text):
    """Writes one line to the live run's own stdin - laptop/debug.c's
    debug_live_override_poll() on the other end. Returns (ok, error).
    Distinct from _live_signal() above: this writes to a pipe rather
    than sending a process signal, but shares its same "no live run in
    progress" 409 contract so the frontend's existing pause/resume 409-
    handling code (see index.html's pauseLive()/resumeLive() - a real
    bug fix from an earlier pass) doesn't need a second code path for
    this endpoint family."""
    with _live_lock:
        proc = _live_proc
        if proc is None or proc.poll() is not None:
            return False, "no live run in progress"
        try:
            proc.stdin.write(text)
            proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            return False, f"failed to write to the live run's stdin: {exc}"
        _live_sent_cmds.append(text.strip())
    return True, None


@app.route("/api/run-live/override", methods=["POST"])
def api_run_live_override():
    """Part 4: sim-mode manual override - a team, an incident, sent as a
    plain text OVERRIDE line over the live run's own stdin (laptop/
    debug.c's debug_live_override_poll(), see core/debug.h's doc for
    the line protocol and every rejection reason). Completely separate
    from /api/board/override above: that route packs a binary
    MissionLinkOverride_t and writes it to a real serial port for
    board hardware; this one only ever reaches the simulator this
    laptop is itself running, and the board path is untouched by it.

    Unlike /api/board/override, "success" here only ever means the
    line was written - there is no synchronous outcome to report yet.
    The actual outcome (applied, or rejected with a reason) streams
    back as an `override_ack` SSE event a moment later; whether the
    team IS currently overridden is simply read live off every
    LIVEFRAME's own teams[].override/override_incident_id fields
    (sourced from the same core/mission_state.h pin the board path
    already uses), not tracked separately here the way the board
    path's OverrideTracker has to (LIVEFRAME already carries the
    live answer every tick, so nothing needs polling)."""
    data = _json_object_body()
    if data is None:
        return _bad_json_body()

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

    tick = _live_last_tick
    ok, error = _live_write_line(f"OVERRIDE {tick} {team_id} {incident_id}\n")
    if not ok:
        return jsonify({"status": "error", "message": error}), 409
    return jsonify({"status": "success", "team_id": team_id, "incident_id": incident_id, "tick": tick})


@app.route("/api/run-live/override/clear", methods=["POST"])
def api_run_live_override_clear():
    """Releases a team's sim-mode override pin early - the "Release"
    button on an overridden team's card. Same OVERRIDE_CLEAR line
    laptop/debug.c already accepts; a pin also clears itself
    automatically once its incident resolves (core/cbba.c's
    cbba_apply_override_pin()), which the dashboard sees the same way,
    live off LIVEFRAME, with no separate code path needed for that
    case."""
    data = _json_object_body()
    if data is None:
        return _bad_json_body()

    try:
        team_id = int(data["team_id"])
    except (KeyError, TypeError, ValueError):
        return jsonify({"status": "error", "message": "Request body must include integer team_id"}), 400
    if not (0 <= team_id <= 255):
        return jsonify({"status": "error", "message": "team_id out of range (0-255)"}), 400

    tick = _live_last_tick
    ok, error = _live_write_line(f"OVERRIDE_CLEAR {tick} {team_id}\n")
    if not ok:
        return jsonify({"status": "error", "message": error}), 409
    return jsonify({"status": "success", "team_id": team_id, "tick": tick})


@app.route("/api/run-live", methods=["POST"])
def api_run_live():
    payload = _json_object_body()
    if payload is None:
        return _bad_json_body()
    try:
        seed = int(payload.get("seed", 1))
        ticks = int(payload.get("ticks", 500))
        tick_delay_ms = int(payload.get("tick_delay_ms", 150))
    except (TypeError, ValueError):
        return jsonify({"error": "seed, ticks and tick_delay_ms must be integers"}), 400
    fault_profile = payload.get("fault_profile", "none")

    # Per-link loss on the virtual peer bus (core/vbus.c's IID model,
    # docs/architecture/CONSENSUS_DESIGN.md 3.5 decision D4: default 0.05 when this flag
    # is omitted). Optional - the UI leaves this blank most of the time
    # and lets the simulator's own default apply; recorded into the
    # header either way so a replay's provenance is complete.
    link_loss = payload.get("link_loss")
    if link_loss is not None:
        try:
            link_loss = float(link_loss)
        except (TypeError, ValueError):
            return jsonify({"error": "link_loss must be a number"}), 400
        if not (0.0 <= link_loss <= 1.0):
            return jsonify({"error": "link_loss must be between 0 and 1"}), 400

    build_ok, build_log = build_binary()
    if not build_ok:
        def err_stream():
            yield f"data: {json.dumps({'type': 'error', 'message': 'build failed', 'log': build_log})}\n\n"
        return Response(err_stream(), mimetype="text/event-stream")

    args = [str(BINARY), str(seed), "1", str(ticks),
            "--live", "--trace", "--diag", f"--tick-delay-ms={tick_delay_ms}"]
    if fault_profile and fault_profile != "none":
        args += ["--fault-profile", fault_profile]
    if link_loss is not None:
        args += [f"--vbus-loss={link_loss}"]
    return _live_run_response(args, seed, ticks, fault_profile, link_loss)


def _override_script_line(cmd, effective_tick, team_id, incident_id):
    """The --override-script line for one applied override."""
    if cmd == "OVERRIDE_CLEAR":
        return f"OVERRIDE_CLEAR {effective_tick} {team_id}"
    return f"OVERRIDE {effective_tick} {team_id} {incident_id}"


def _frame_diff(want, got, path=""):
    """Path of the first difference between two LIVEFRAME values (None
    when they agree): what a takeover checks before going live. Dicts
    are compared on the keys BOTH sides have, so a recording made
    before a field was added to the frame (older recordings lack e.g.
    teams[].override) still matches when the simulation itself is
    unchanged; every shared field, list length and element must be
    equal."""
    if isinstance(want, dict) and isinstance(got, dict):
        for key in sorted(want.keys() & got.keys()):
            diff = _frame_diff(want[key], got[key], f"{path}.{key}" if path else key)
            if diff:
                return diff
        return None
    if isinstance(want, list) and isinstance(got, list):
        if len(want) != len(got):
            return f"{path} has {len(got)} entries, recording has {len(want)}"
        for i, (w, g) in enumerate(zip(want, got)):
            diff = _frame_diff(w, g, f"{path}[{i}]")
            if diff:
                return diff
        return None
    return None if want == got else path


def _live_run_response(args, seed, ticks, fault_profile, link_loss, takeover=None):
    """Runs `args` (a tron --live command line) and streams it as SSE.
    With `takeover` (see api_replay_takeover) the process starts with
    --fast-forward: nothing is recorded or shown until its first frame,
    which must equal the recording's frame at that tick."""
    def generate():
        global _live_proc, _live_paused, _live_last_tick
        proc = subprocess.Popen(
            args, cwd=PROJECT_DIR, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1,
        )
        with _live_lock:
            # If a previous run's browser tab disconnected without
            # clicking Stop (closed/refreshed mid-run), its own
            # generate() call is still alive on another thread, reading
            # its own now-orphaned process and racing this one to write
            # _live_last_tick below - kill it now so only this run's
            # ticks can reach that global from here on.
            old_proc = _live_proc
            _live_proc = proc
            _live_paused = False
            del _live_sent_cmds[:]
        if old_proc is not None and old_proc.poll() is None:
            old_proc.kill()
        _live_last_tick = 0
        stderr_lines = []
        start = time.time()
        recorder = None if takeover else _RecordingWriter(seed, fault_profile, ticks, link_loss)
        fast_forwarding = takeover is not None
        held_lines = []  # static lines that arrive before a takeover is verified

        def record(text):
            if recorder is not None:
                recorder.write_line(text)
            else:
                held_lines.append(text)

        try:
            for line in proc.stdout:
                line = line.rstrip("\n")
                if not line:
                    continue
                if line.startswith("FFPROGRESS "):
                    if fast_forwarding:
                        prog = json.loads(line[len("FFPROGRESS "):])
                        yield f"data: {json.dumps({'type': 'ff_progress', **prog})}\n\n"
                    continue
                if line.startswith("LIVEFRAME "):
                    frame = json.loads(line[len("LIVEFRAME "):])
                    if fast_forwarding:
                        fast_forwarding = False
                        diff = _frame_diff(takeover["rec_frame"], frame)
                        if diff is not None:
                            proc.kill()
                            msg = ("Cannot take over: the state regenerated at tick "
                                   f"{takeover['tick']} does not match the recording (first difference: {diff}). "
                                   "The recording was made on a different code version or is not reproducible.")
                            yield f"data: {json.dumps({'type': 'takeover_error', 'message': msg})}\n\n"
                            return
                        recorder = _RecordingWriter(
                            seed, fault_profile, ticks, link_loss,
                            extra={"takeover_from": {"recording": takeover["source"], "tick": takeover["tick"]}})
                        for prefix_line in takeover["prefix_lines"] + held_lines:
                            recorder.write_line(prefix_line)
                        del held_lines[:]
                        paused = False
                        if takeover["start_paused"] and hasattr(signal, "SIGSTOP"):
                            _, status = _live_signal("pause")
                            paused = status == 200
                        ready = {"type": "takeover_ready", "tick": takeover["tick"], "paused": paused,
                                 "warning": takeover["warning"], "recording": recorder.path.name,
                                 "prior_overrides": takeover["prior_overrides"]}
                        yield f"data: {json.dumps(ready)}\n\n"
                    record(line)
                    # Only this generate() call's own process may update
                    # the shared tick global - guards against a stray
                    # write from an orphaned previous run that hasn't
                    # finished dying yet (see the kill above; poll()
                    # isn't instant).
                    if _live_proc is proc:
                        _live_last_tick = frame.get("tick", _live_last_tick)
                    yield f"data: {json.dumps({'type': 'frame', 'frame': frame})}\n\n"
                elif line.startswith("STATICENV "):
                    record(line)
                    env = line[len("STATICENV "):]
                    yield f"data: {json.dumps({'type': 'static_env', 'env': json.loads(env)})}\n\n"
                elif line.startswith("STATICMAP "):
                    record(line)
                    terrain = line[len("STATICMAP "):]
                    yield f"data: {json.dumps({'type': 'static_map', 'map': json.loads(terrain)})}\n\n"
                elif line.startswith("OVERRIDEACK "):
                    # Part 4, sim-mode only - a completely separate channel
                    # from the board path's binary MissionLinkOverrideAck_t/
                    # OverrideTracker (laptop/tools/telemetry_bridge.py).
                    ack = line[len("OVERRIDEACK "):]
                    # Recorded (raw, plus an OVERRIDE line for applied
                    # ones) so a takeover of THIS recording can replay
                    # the override at its effective tick.
                    with _live_lock:
                        cmd = _live_sent_cmds.pop(0) if _live_sent_cmds else ""
                    ack_obj = json.loads(ack)
                    record(line)
                    if ack_obj.get("result") == "applied" and cmd:
                        record("OVERRIDE " + json.dumps({
                            "cmd": cmd.split()[0], "effective_tick": ack_obj["effective_tick"],
                            "team_id": ack_obj["team_id"], "incident_id": ack_obj["incident_id"]}))
                    yield f"data: {json.dumps({'type': 'override_ack', **ack_obj})}\n\n"
                else:
                    stderr_lines.append(line)
                    if line.startswith("[DIAG") and not fast_forwarding:
                        yield f"data: {json.dumps({'type': 'diag_line', 'line': line})}\n\n"
            proc.wait(timeout=30)
        except GeneratorExit:
            proc.kill()
            raise
        finally:
            if proc.poll() is None:
                proc.kill()
            with _live_lock:
                if _live_proc is proc:
                    _live_proc = None
                    _live_paused = False
            if recorder is not None:
                recorder.close()
            if takeover and takeover.get("script_file"):
                try:
                    os.unlink(takeover["script_file"])
                except OSError:
                    pass

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

        yield f"data: {json.dumps({'type': 'done', 'diag': diag, 'telemetry': telemetry, 'history': history, 'elapsed_seconds': round(elapsed, 2), 'recording': recorder.path.name if recorder else None})}\n\n"

    return Response(stream_with_context(generate()), mimetype="text/event-stream",
                     headers={"Cache-Control": "no-cache", "X-Accel-Buffering": "no"})


@app.route("/api/replay/takeover", methods=["POST"])
def api_replay_takeover():
    """Part 5: continue a recorded run as a LIVE simulation from `tick`.
    No state snapshot exists, so the simulator re-simulates ticks
    0..tick-1 (`--fast-forward`), replaying the recording's applied
    overrides at their effective ticks, and the regenerated frame at
    `tick` must equal the recording's (see _live_run_response) before
    anything goes live. Streams SSE like /api/run-live, preceded by
    ff_progress events and one takeover_ready (or takeover_error)."""
    payload = _json_object_body()
    if payload is None:
        return _bad_json_body()
    name = payload.get("recording")
    try:
        tick = int(payload.get("tick"))
        tick_delay_ms = int(payload.get("tick_delay_ms", 150))
    except (TypeError, ValueError):
        return jsonify({"status": "error", "message": "tick and tick_delay_ms must be integers"}), 400
    if not isinstance(name, str) or not _RECORDING_NAME_RE.match(name):
        return jsonify({"status": "error", "message": "invalid recording name"}), 400
    path = RECORDINGS_DIR / name
    if not path.is_file():
        return jsonify({"status": "error", "message": "recording not found"}), 404

    header, rec_frame, prefix_lines, script, prior = None, None, [], [], []
    try:
        with gzip.open(path, "rt") as fh:
            for raw in fh:
                raw = raw.rstrip("\n")
                if raw.startswith("RECORDING "):
                    header = json.loads(raw[len("RECORDING "):])
                elif raw.startswith("LIVEFRAME "):
                    frame = json.loads(raw[len("LIVEFRAME "):])
                    if frame.get("tick") == tick:
                        rec_frame = frame
                    if frame.get("tick", 0) < tick:
                        prefix_lines.append(raw)
                elif raw.startswith("OVERRIDE "):
                    ov = json.loads(raw[len("OVERRIDE "):])
                    if ov["effective_tick"] < tick:
                        prefix_lines.append(raw)
                        script.append(_override_script_line(
                            ov["cmd"], ov["effective_tick"], ov["team_id"], ov["incident_id"]))
                        prior.append(ov)
    except (OSError, ValueError, KeyError) as exc:
        return jsonify({"status": "error", "message": f"cannot read recording: {exc}"}), 400
    if not header or "seed" not in header:
        return jsonify({"status": "error", "message": "recording has no usable header"}), 400
    if rec_frame is None:
        return jsonify({"status": "error", "message": f"recording has no frame at tick {tick}"}), 400
    ticks = int(header.get("ticks") or 0)
    if not (0 < tick < ticks):
        return jsonify({"status": "error",
                        "message": f"tick must be between 1 and {ticks - 1} for this recording"}), 400

    seed = int(header["seed"])
    fault_profile = header.get("fault_profile", "none")
    link_loss = header.get("link_loss")

    warning = None
    rec_commit, here_commit = header.get("git_commit"), _git_commit()
    if rec_commit != here_commit:
        warning = (f"Recording was made on commit {rec_commit or 'unknown'}, this dashboard runs "
                   f"{here_commit or 'unknown'}; trying anyway.")

    build_ok, build_log = build_binary()
    if not build_ok:
        def err_stream():
            yield f"data: {json.dumps({'type': 'error', 'message': 'build failed', 'log': build_log})}\n\n"
        return Response(err_stream(), mimetype="text/event-stream")

    args = [str(BINARY), str(seed), "1", str(ticks), "--live", "--trace", "--diag",
            f"--tick-delay-ms={tick_delay_ms}", f"--fast-forward={tick}"]
    script_file = None
    if script:
        script_file = tempfile.NamedTemporaryFile("w", suffix=".ovr", delete=False)
        script_file.write("\n".join(script) + "\n")
        script_file.close()
        args.append(f"--override-script={script_file.name}")
    if fault_profile and fault_profile != "none":
        args += ["--fault-profile", fault_profile]
    if link_loss is not None:
        args += [f"--vbus-loss={link_loss}"]
    takeover = {
        "tick": tick, "rec_frame": rec_frame, "prefix_lines": prefix_lines, "source": name,
        "warning": warning, "prior_overrides": prior,
        "start_paused": bool(payload.get("start_paused", True)),
        # Read by the simulator once at start-up; deleted when the run ends.
        "script_file": script_file.name if script_file else None,
    }
    return _live_run_response(args, seed, ticks, fault_profile, link_loss, takeover=takeover)


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
    print(f"Starting unified dashboard on http://{DASHBOARD_HOST}:{DASHBOARD_PORT}")
    # threaded=True, made explicit rather than relying on whatever a
    # given Werkzeug version happens to default to: /api/run-live's own
    # response is a long-lived SSE stream that stays open for the
    # entire run, so every other route (pause/resume/stop, and now
    # Part 4's override/override/clear) already depends on being served
    # concurrently with it, not queued behind it - _live_lock exists
    # precisely because those routes run on a different thread than the
    # streaming generator.
    app.run(host=DASHBOARD_HOST, port=DASHBOARD_PORT, debug=False, threaded=True)
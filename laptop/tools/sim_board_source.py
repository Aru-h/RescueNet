"""
sim_board_source.py - a laptop-simulator-backed drop-in for app.py's
TelemetryBridge (the real hardware/serial consumer), used ONLY when
app.py is started in headless/sim-only mode (see app.py's SIM_ONLY /
--no-hardware flag). Opens zero serial ports and requires zero board
hardware connected - see the module docstring in app.py for the full
handoff this implements.

--------------------------------------------------------------------
Why a new class instead of reusing TelemetryBridge directly
--------------------------------------------------------------------
TelemetryBridge's job is decoding a real byte stream off a real board
(magic-byte-framed telemetry frames, a mission-link handshake, plain
tm_printf log lines - see that class's own docstring in app.py).
None of that applies here: there is no wire, no handshake with a real
peer, no framing/resync problem to solve. What this class actually
needs to do is drive `tron` (the same freestanding-clean simulator
binary core/`s own build already produces, no board required) as a
subprocess in --live mode and adapt its output onto the exact same
public surface app.py's routes already read from `bridge` - so
/api/status, /api/board/launch, /api/board/disconnect etc. all work
completely unchanged regardless of which class `bridge` actually is.

Chosen over decoding `tron`'s CSV/telemetry output into the real
board's own {run_id, cycle, team_id, event_type} wire-frame shape
(the "same interface" reading of the handoff doc): the doc explicitly
left the actual downstream data shape as our own call, and `tron
--live` already emits a far richer per-tick JSON line (every team's
position/stamina/status/destination - see laptop/debug.c's
debug_emit_live_frame()) than that 4-field wire struct ever carries.
Shoehorning the richer sim data down into the sparser hardware shape
would throw away exactly the detail an AI training pipeline is likely
to want. So: same *mechanism* (a background thread pushing decoded
items onto bridge.telemetry_queue, drained by /api/status same as
today), richer *payload* shape, clearly tagged so a downstream
consumer never confuses a sim frame for a real hardware frame.

--------------------------------------------------------------------
What this does NOT do
--------------------------------------------------------------------
- Never imports or touches `serial`/pyserial at all.
- Never performs a real mission-link handshake with anything - there
  is no peer board to handshake with. self.handshake below is a
  lightweight stand-in with the same .can_proceed / .status_dict()
  shape TelemetryBridge's real HandshakeCoordinator exposes (so
  /api/status's existing JSON shape doesn't change between modes),
  always reporting ready, tagged with "source": "sim" so a downstream
  consumer can tell the two apart.
- Never answers /api/board/override meaningfully - overriding a
  team's assignment only makes sense with a real board's mission-link
  layer on the other end (core/mission_link_override.c) to apply it.
  self.serial_conn.write() below raises a clear, caught-by-the-route
  error explaining why, rather than silently doing nothing or
  crashing with an unrelated AttributeError.
"""

import json
import queue
import re
import subprocess
import threading
from pathlib import Path

DIAG_LINE_RE = re.compile(r"^\[DIAG")


class _NoOverrideConn:
    """Stands in for TelemetryBridge's real self.serial_conn just
    closely enough that app.py's existing `bridge.serial_conn and
    bridge.serial_conn.is_open` checks (in /api/status, /api/board/
    override) keep working unmodified in sim mode. is_open mirrors
    whether the sim subprocess is actually running. write() exists
    only to fail loudly and clearly if something calls it (see
    module docstring's "What this does NOT do")."""

    def __init__(self, source: "SimBoardSource"):
        self._source = source

    @property
    def is_open(self) -> bool:
        return self._source.running

    def write(self, _data: bytes):
        raise NotImplementedError(
            "sim mode has no real board to relay a mission-link override "
            "to - /api/board/override is a hardware-only route"
        )

    def close(self):
        pass


class _SimHandshakeStatus:
    """Same .can_proceed / .status_dict() shape TelemetryBridge's real
    telemetry_bridge.HandshakeCoordinator exposes (see that class),
    so /api/status's JSON shape is identical in both modes - but
    always reports ready, since there is no real peer board to wait
    on in sim mode, and tags itself so a downstream consumer can tell
    the two apart rather than mistaking a sim run for a confirmed
    two-board handshake."""

    can_proceed = True

    def status_dict(self) -> dict:
        return {
            "board0_ready": True,
            "peer_ready": {},
            "peer_timed_out": {},
            "all_ready": True,
            "any_peer_timed_out": False,
            "dk_only_override": False,
            "can_proceed": True,
            "source": "sim",
        }


class SimBoardSource:
    """Drop-in for app.py's TelemetryBridge, backed by `tron --live`
    instead of a real board over serial. Public attributes/methods
    mirror TelemetryBridge's own (serial_conn, nodes_ready,
    telemetry_queue, mission_started, handshake, port, baudrate,
    connect/disconnect/send_start) so every existing app.py route
    that reads `bridge` works unchanged - see this module's own
    top comment for why a new class exists instead of reusing
    TelemetryBridge directly.
    """

    def __init__(self, project_dir: Path, binary: Path):
        self.project_dir = project_dir
        self.binary = binary
        self.port = "sim"
        self.baudrate = 0
        self.serial_conn = _NoOverrideConn(self)
        self.running = False
        self.nodes_ready = set()
        self.telemetry_queue = queue.Queue()
        self.thread = None
        self.mission_started = False
        self.handshake = _SimHandshakeStatus()
        self._proc = None
        self._last_run_params = {}

    def _build(self) -> tuple[bool, str]:
        """Same `make` invocation app.py's own build_binary() does -
        duplicated rather than imported to avoid a circular import
        (app.py imports this module to construct `bridge`)."""
        try:
            result = subprocess.run(
                ["make"], cwd=self.project_dir, capture_output=True,
                text=True, timeout=120,
            )
            return result.returncode == 0, result.stdout + result.stderr
        except Exception as exc:  # noqa: BLE001
            return False, str(exc)

    def connect(self, *, seed: int = 1, ticks: int = 2000,
                fault_profile: str = "none", tick_delay_ms: int = 0) -> bool:
        """Starts `tron` as a subprocess in --live mode and begins
        streaming its output onto self.telemetry_queue - the sim
        equivalent of TelemetryBridge.connect() opening a real serial
        port, but there is no port/baudrate here, only simulation
        parameters (seed/ticks/fault_profile), plus tick_delay_ms to
        choose pace: 0 (default) means `tron` sleeps between ticks
        not at all - as fast as possible, for bulk data generation -
        any positive value paces it to look like a live board instead
        (see laptop/main.c's own --tick-delay-ms handling, unchanged
        by this module). Returns False (mirroring TelemetryBridge's
        own connect() contract) on a build failure or a subprocess
        that fails to start; never touches a serial port.
        """
        ok, log = self._build()
        if not ok:
            print(f"[sim] build failed, cannot start sim-only run:\n{log}")
            return False

        args = [
            str(self.binary), str(seed), "1", str(ticks),
            "--live", "--trace", "--diag", f"--tick-delay-ms={tick_delay_ms}",
        ]
        if fault_profile and fault_profile != "none":
            args += ["--fault-profile", fault_profile]

        try:
            self._proc = subprocess.Popen(
                args, cwd=self.project_dir, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT, text=True, bufsize=1,
            )
        except Exception as exc:  # noqa: BLE001
            print(f"[sim] failed to start tron subprocess: {exc}")
            return False

        self.running = True
        self.nodes_ready = {"SIM"}
        self.mission_started = False
        self._last_run_params = {
            "seed": seed, "ticks": ticks,
            "fault_profile": fault_profile, "tick_delay_ms": tick_delay_ms,
        }
        self.thread = threading.Thread(target=self._read_loop, daemon=True)
        self.thread.start()
        return True

    def _read_loop(self):
        """Reads tron's own stdout (LIVEFRAME/STATICENV JSON lines,
        plus [DIAG] text - the exact same output shape /api/run-live's
        SSE stream already parses) and pushes decoded items onto
        telemetry_queue, drained by /api/status same as a real
        board's frames are today. Every item is tagged with its own
        "type" so a downstream consumer distinguishes a full per-tick
        frame from the one-time static layout from a diagnostic line
        - mirrors the SSE event "type" field /api/run-live already
        sends, so the same frontend parsing logic could drive either
        source with minimal change, though that wiring is left for
        whoever builds the actual downstream consumer (see this
        module's own top comment).
        """
        proc = self._proc
        try:
            for line in proc.stdout:
                if not self.running:
                    break
                line = line.rstrip("\n")
                if not line:
                    continue
                if line.startswith("LIVEFRAME "):
                    frame = json.loads(line[len("LIVEFRAME "):])
                    self.telemetry_queue.put({"type": "sim_frame", "frame": frame})
                elif line.startswith("STATICENV "):
                    env = json.loads(line[len("STATICENV "):])
                    self.telemetry_queue.put({"type": "sim_static_env", "env": env})
                elif DIAG_LINE_RE.match(line):
                    self.telemetry_queue.put({"type": "sim_diag_line", "line": line})
            proc.wait(timeout=30)
        except Exception as exc:  # noqa: BLE001
            print(f"[sim] read loop error: {exc}")
        finally:
            if proc.poll() is None:
                proc.kill()
            self.running = False
            self.telemetry_queue.put({
                "type": "sim_done",
                "returncode": proc.returncode,
                "params": self._last_run_params,
            })

    def send_start(self) -> bool:
        """Sim data is already flowing as soon as connect() returns -
        there is no real board waiting for a START command - so this
        just marks the run as launched for /api/status's existing
        mission_started field, same as TelemetryBridge.send_start()
        does for a real board."""
        self.mission_started = True
        return True

    def disconnect(self):
        """Mirrors TelemetryBridge.disconnect()'s shape: stop the
        background thread, tear down the subprocess, reset state -
        never touches a serial port because there never was one."""
        self.running = False
        if self._proc is not None and self._proc.poll() is None:
            self._proc.kill()
        if self.thread:
            self.thread.join(timeout=2.0)
        self._proc = None
        self.nodes_ready.clear()
        self.mission_started = False
        self.handshake = _SimHandshakeStatus()

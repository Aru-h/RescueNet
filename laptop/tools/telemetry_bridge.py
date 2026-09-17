#!/usr/bin/env python3
"""
STM32N6 Board Telemetry Bridge and Serial Communication Layer.

This module acts as the host-side counterpart to platform/board/main_board.c
and platform/board/snapshot.c. It opens the designated USB Virtual COM port
connected to the STM32N6570-DK board, spawns a background thread to safely
consume the incoming byte stream without blocking the UI event loop, and handles:
  1. The mission-link liveness handshake (core/mission_link_handshake.h) -
     sending MissionLinkHandshakeReq_t to DK, decoding
     MissionLinkHandshakeReply_t for both board_id 0 (DK) and board_id 1
     (NUCLEO, relayed through DK) - see HandshakeCoordinator below. This is
     what the old plain-text `READY` line parsing has been replaced with;
     see that class's docstring for why a text-based liveness signal wasn't
     good enough on its own.
  2. Decoding incoming live telemetry frames into a thread-safe queue for
     real-time dashboard visualization.
  3. Transmitting explicit binary/text command packets (e.g., `START`) down
     the USB line once board nodes clear initialization.

Wire format
-----------
USART1 is a single shared line: platform/board/snapshot.c's ring drain writes
binary telemetry frames on it, core/mission_link_handshake.c's board-side
state machine writes binary handshake frames on it, and free-running
tm_printf debug text from the rest of the firmware can appear on it at any
time, interleaved at arbitrary byte boundaries. There is no separate channel
to tell any of these apart, so this module reads raw bytes (not readline())
and scans for frame boundaries itself, per the wire contracts documented in
core/telemetry_frame.h and core/mission_link_handshake.h:

    byte 0-1   magic          0xA5 0x5A (telemetry), 0xB6 0x6B (handshake),
                              0x96 0x69 (override ack), 0xF9 0x9F (team status)
    byte 2-N   payload        frame-type-specific (see each header)
    byte N+1   checksum       1-byte additive checksum over the payload

Telemetry frame total size: TELEMETRY_FRAME_WIRE_SIZE (13) bytes. Handshake
reply frame total size: HANDSHAKE_REPLY_WIRE_SIZE (6) bytes - this module
only ever needs to recognize Reply frames, never Req (the laptop only ever
sends a Req, never receives one - see HandshakeCoordinator's docstring for
the full sequence). See those headers for the fuller rationale (magic bytes
are non-ASCII so they can't appear in tm_printf text; checksum mismatches
are an expected resync case, not an error; gaps in `cycle` between
consecutive telemetry frames are expected under the ring's overwrite-oldest
overflow policy and are not a transport error).
"""

import queue
import struct
import threading
import time

try:
    import serial
except ImportError:
    # Only TelemetryBridge.connect() below (real hardware) actually
    # needs pyserial. HandshakeCoordinator/TelemetryFrameParser (the
    # two things app.py imports from this module even in sim-only
    # mode - see app.py's SIM_ONLY) have no serial dependency at all,
    # so this module must stay importable without pyserial installed
    # for app.py's own "no pyserial port required at all" sim-only
    # bar to actually hold. TelemetryBridge.connect() checks for this
    # itself and fails clearly if hardware mode is attempted without
    # pyserial installed.
    serial = None


# --- Wire format constants (mirrors core/telemetry_frame.h) -------------

TELEMETRY_FRAME_MAGIC0 = 0xA5
TELEMETRY_FRAME_MAGIC1 = 0x5A
TELEMETRY_FRAME_MAGIC = bytes((TELEMETRY_FRAME_MAGIC0, TELEMETRY_FRAME_MAGIC1))

# run_id: u32 LE, cycle: u32 LE, team_id: u8, event_type: u8 - packed,
# no padding (matches TelemetryFrame_t's __attribute__((packed))).
_PAYLOAD_STRUCT = struct.Struct("<IIBB")
TELEMETRY_FRAME_PAYLOAD_SIZE = _PAYLOAD_STRUCT.size  # 10 bytes
assert TELEMETRY_FRAME_PAYLOAD_SIZE == 10

TELEMETRY_FRAME_WIRE_SIZE = 2 + TELEMETRY_FRAME_PAYLOAD_SIZE + 1  # 13 bytes


def telemetry_frame_checksum(payload: bytes) -> int:
    """1-byte additive checksum over a TELEMETRY_FRAME_PAYLOAD_SIZE-byte
    payload buffer - mirrors the TELEMETRY_FRAME_CHECKSUM() macro in
    core/telemetry_frame.h (sum of payload bytes, mod 256)."""
    return sum(payload) & 0xFF


# --- Wire format constants (mirrors core/mission_link_handshake.h) ------
#
# Magic bytes verified distinct from TELEMETRY_FRAME_MAGIC0/1 above and
# from every core/mission_link.h magic pair - see
# core/mission_link_handshake.h's own top-of-file distinctness check for
# the full cross-reference; this is the same pair, just mirrored here for
# the host side the same way TELEMETRY_FRAME_MAGIC0/1 above mirrors
# core/telemetry_frame.h's.

HANDSHAKE_MAGIC0 = 0xB6
HANDSHAKE_MAGIC1 = 0x6B
HANDSHAKE_MAGIC = bytes((HANDSHAKE_MAGIC0, HANDSHAKE_MAGIC1))

MISSION_LINK_PROTOCOL_VERSION = 1  # mirrors core/mission_link_handshake.h

# MissionLinkHandshakeReq_t: board_id (u8), protocol_version (u8) - the
# laptop only ever SENDS this shape, never receives it (see
# HandshakeCoordinator's docstring), so this module has no need for a
# parser for it, only the packer below.
_REQ_STRUCT = struct.Struct("<BB")
HANDSHAKE_REQ_PAYLOAD_SIZE = _REQ_STRUCT.size
assert HANDSHAKE_REQ_PAYLOAD_SIZE == 2
HANDSHAKE_REQ_WIRE_SIZE = 2 + HANDSHAKE_REQ_PAYLOAD_SIZE + 1  # 5

# MissionLinkHandshakeReply_t: board_id (u8), protocol_version (u8),
# ready (u8) - the laptop only ever RECEIVES this shape.
_REPLY_STRUCT = struct.Struct("<BBB")
HANDSHAKE_REPLY_PAYLOAD_SIZE = _REPLY_STRUCT.size
assert HANDSHAKE_REPLY_PAYLOAD_SIZE == 3
HANDSHAKE_REPLY_WIRE_SIZE = 2 + HANDSHAKE_REPLY_PAYLOAD_SIZE + 1  # 6


def handshake_checksum(payload: bytes) -> int:
    """1-byte additive checksum over a handshake payload buffer - same
    scheme as telemetry_frame_checksum() above, mirrors
    core/mission_link_handshake.c's handshake_checksum()."""
    return sum(payload) & 0xFF


def pack_handshake_req(board_id: int, protocol_version: int = MISSION_LINK_PROTOCOL_VERSION) -> bytes:
    """Builds the laptop -> DK MissionLinkHandshakeReq_t wire frame -
    mirrors core/mission_link_handshake.c's
    mission_link_handshake_pack_req(). board_id is always 0 in this
    module's actual usage (the laptop only ever addresses DK directly;
    DK is responsible for reaching NUCLEO on its own - see
    HandshakeCoordinator's docstring), but the parameter is kept
    general rather than hardcoded so a test can exercise the codec
    without reaching into a private constant."""
    payload = _REQ_STRUCT.pack(board_id, protocol_version)
    return HANDSHAKE_MAGIC + payload + bytes((handshake_checksum(payload),))


class TelemetryFrameParser:
    """Stateful byte-stream scanner implementing the resync behavior
    core/telemetry_frame.h section 2 describes: scan for the two magic
    bytes, then check whether a full frame follows and its checksum
    verifies. If it does, emit a decoded frame and consume it. If it
    doesn't (a magic-byte collision inside debug text, or a torn
    frame), that is the expected failure mode, not an error - drop
    exactly one byte and resume scanning from there, rather than
    discarding the whole buffer or raising.

    feed() is fed raw bytes as they arrive off the wire and returns a
    list of decoded events, each either:
        ("frame", {"run_id": int, "cycle": int, "team_id": int, "event_type": int})
        ("text", bytes)   -- raw bytes that are not part of a valid frame,
                              for the caller to fold into its existing
                              line-based debug log path.
    """

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        if data:
            self._buf.extend(data)

        events = []

        while True:
            idx = self._buf.find(TELEMETRY_FRAME_MAGIC)

            if idx == -1:
                # No magic pair in the buffer. Flush everything as text,
                # except possibly hold back a single trailing 0xA5 byte
                # in case the next read delivers the 0x5A that completes
                # the magic pair split across two reads.
                if self._buf and self._buf[-1] == TELEMETRY_FRAME_MAGIC0:
                    if len(self._buf) > 1:
                        events.append(("text", bytes(self._buf[:-1])))
                        del self._buf[:-1]
                elif self._buf:
                    events.append(("text", bytes(self._buf)))
                    self._buf.clear()
                break

            if idx > 0:
                # Emit the bytes preceding the magic pair as plain text.
                events.append(("text", bytes(self._buf[:idx])))
                del self._buf[:idx]

            # Buffer now starts at the magic pair (offset 0).
            if len(self._buf) < TELEMETRY_FRAME_WIRE_SIZE:
                # Not enough bytes yet for a full candidate frame - wait
                # for more data before deciding anything.
                break

            candidate = bytes(self._buf[:TELEMETRY_FRAME_WIRE_SIZE])
            payload = candidate[2:2 + TELEMETRY_FRAME_PAYLOAD_SIZE]
            claimed_checksum = candidate[2 + TELEMETRY_FRAME_PAYLOAD_SIZE]

            if telemetry_frame_checksum(payload) == claimed_checksum:
                run_id, cycle, team_id, event_type = _PAYLOAD_STRUCT.unpack(payload)
                events.append((
                    "frame",
                    {
                        "run_id": run_id,
                        "cycle": cycle,
                        "team_id": team_id,
                        "event_type": event_type,
                    },
                ))
                del self._buf[:TELEMETRY_FRAME_WIRE_SIZE]
            else:
                # Checksum mismatch: a magic-byte collision inside text,
                # or a torn frame. Reject this candidate and resync by
                # shifting exactly one byte, then keep scanning - do not
                # discard the whole buffer or treat this as fatal.
                del self._buf[:1]

        return events


class HandshakeReplyParser:
    """Stateful byte-stream scanner for MissionLinkHandshakeReply_t
    frames (core/mission_link_handshake.h) - same resync shape as
    TelemetryFrameParser above (scan for magic, verify checksum, drop
    one byte and resync on a mismatch rather than discarding
    everything), scoped to this one frame shape. The laptop never
    needs to recognize a Req on the wire since it only ever sends
    those, never receives them (see HandshakeCoordinator below).

    Deliberately a separate class from TelemetryFrameParser rather
    than folded into it: that class is scoped to
    core/telemetry_frame.h's wire format specifically (its name says
    so), and conflating a second, unrelated frame type's magic bytes
    and payload shape into it would blur which header governs which
    part of its behavior. Composition instead - see TelemetryBridge.
    _read_loop below for how the two are chained on the same raw byte
    stream (this one runs first; whatever it doesn't recognize as a
    Reply frame is passed on to TelemetryFrameParser exactly as if it
    had arrived directly, so plain debug text and telemetry frames are
    unaffected by this class existing at all).

    feed() returns a list of events, each either:
        ("reply", {"board_id": int, "protocol_version": int, "ready": bool})
        ("passthrough", bytes)  -- not part of a valid Reply frame;
                                    caller should feed this to
                                    TelemetryFrameParser next.
    """

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        if data:
            self._buf.extend(data)

        events = []

        while True:
            idx = self._buf.find(HANDSHAKE_MAGIC)

            if idx == -1:
                if self._buf and self._buf[-1] == HANDSHAKE_MAGIC0:
                    if len(self._buf) > 1:
                        events.append(("passthrough", bytes(self._buf[:-1])))
                        del self._buf[:-1]
                elif self._buf:
                    events.append(("passthrough", bytes(self._buf)))
                    self._buf.clear()
                break

            if idx > 0:
                events.append(("passthrough", bytes(self._buf[:idx])))
                del self._buf[:idx]

            # Buffer now starts at the magic pair. This parser only
            # ever expects a Reply-shaped frame (HANDSHAKE_REPLY_WIRE_
            # SIZE bytes) here - the laptop never receives a Req -
            # so, unlike a parser that has to disambiguate multiple
            # shapes sharing one magic pair (see core/mission_link_
            # handshake.c's own note on this), there is only one
            # candidate length to wait for.
            if len(self._buf) < HANDSHAKE_REPLY_WIRE_SIZE:
                break

            candidate = bytes(self._buf[:HANDSHAKE_REPLY_WIRE_SIZE])
            payload = candidate[2:2 + HANDSHAKE_REPLY_PAYLOAD_SIZE]
            claimed_checksum = candidate[2 + HANDSHAKE_REPLY_PAYLOAD_SIZE]

            if handshake_checksum(payload) == claimed_checksum:
                board_id, protocol_version, ready = _REPLY_STRUCT.unpack(payload)
                events.append((
                    "reply",
                    {
                        "board_id": board_id,
                        "protocol_version": protocol_version,
                        "ready": bool(ready),
                    },
                ))
                del self._buf[:HANDSHAKE_REPLY_WIRE_SIZE]
            else:
                # Pass the rejected byte on rather than deleting it: the
                # magic pair can occur inside another binary frame (an
                # override ack's incident_id, a telemetry cycle count), and
                # swallowing that byte would corrupt the frame for the
                # parser downstream.
                events.append(("passthrough", bytes(self._buf[:1])))
                del self._buf[:1]

        return events


# --- Override acknowledgement (mirrors core/mission_link_override.h section 3)

OVERRIDE_ACK_MAGIC0 = 0x96
OVERRIDE_ACK_MAGIC1 = 0x69
OVERRIDE_ACK_MAGIC = bytes((OVERRIDE_ACK_MAGIC0, OVERRIDE_ACK_MAGIC1))

# MissionLinkOverrideAck_t: team_id (u8), incident_id (u32 LE), result (u8), packed.
_OVERRIDE_ACK_STRUCT = struct.Struct("<BIB")
OVERRIDE_ACK_PAYLOAD_SIZE = _OVERRIDE_ACK_STRUCT.size
assert OVERRIDE_ACK_PAYLOAD_SIZE == 6
OVERRIDE_ACK_WIRE_SIZE = 2 + OVERRIDE_ACK_PAYLOAD_SIZE + 1  # 9

OVERRIDE_ACK_APPLIED = 1
OVERRIDE_ACK_RELAYED = 2
OVERRIDE_ACK_NOT_READY = 3
OVERRIDE_ACK_RELAY_FAILED = 4
OVERRIDE_ACK_BAD_TEAM = 5

# Seconds to wait for a final ack before calling an override unconfirmed.
# Nothing on the link retransmits, so a lost relay or ack never arrives;
# generous because a relayed override crosses LoRa twice behind other
# traffic.
OVERRIDE_ACK_TIMEOUT_SEC = 10.0


def override_ack_checksum(payload: bytes) -> int:
    """Same additive checksum as core/mission_link_override.c."""
    return sum(payload) & 0xFF


def pack_override_ack(team_id: int, incident_id: int, result: int) -> bytes:
    """Test helper - mirrors mission_link_override_pack_ack(). The laptop
    only receives acks."""
    payload = _OVERRIDE_ACK_STRUCT.pack(team_id, incident_id, result)
    return OVERRIDE_ACK_MAGIC + payload + bytes((override_ack_checksum(payload),))


class OverrideAckParser:
    """Byte-stream scanner for MissionLinkOverrideAck_t frames, same shape
    and resync policy as HandshakeReplyParser. feed() returns events:
        ("ack", {"team_id": int, "incident_id": int, "result": int})
        ("passthrough", bytes)  -- not part of an ack; feed onward.
    """

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        if data:
            self._buf.extend(data)

        events = []

        while True:
            idx = self._buf.find(OVERRIDE_ACK_MAGIC)

            if idx == -1:
                if self._buf and self._buf[-1] == OVERRIDE_ACK_MAGIC0:
                    if len(self._buf) > 1:
                        events.append(("passthrough", bytes(self._buf[:-1])))
                        del self._buf[:-1]
                elif self._buf:
                    events.append(("passthrough", bytes(self._buf)))
                    self._buf.clear()
                break

            if idx > 0:
                events.append(("passthrough", bytes(self._buf[:idx])))
                del self._buf[:idx]

            if len(self._buf) < OVERRIDE_ACK_WIRE_SIZE:
                break

            payload = bytes(self._buf[2:2 + OVERRIDE_ACK_PAYLOAD_SIZE])
            if override_ack_checksum(payload) == self._buf[2 + OVERRIDE_ACK_PAYLOAD_SIZE]:
                team_id, incident_id, result = _OVERRIDE_ACK_STRUCT.unpack(payload)
                events.append(("ack", {"team_id": team_id, "incident_id": incident_id,
                                       "result": result}))
                del self._buf[:OVERRIDE_ACK_WIRE_SIZE]
            else:
                events.append(("passthrough", bytes(self._buf[:1])))
                del self._buf[:1]

        return events


class OverrideTracker:
    """What the dashboard knows about each team's latest override.

    States: "sent" (written to USART1, nothing back yet), "relayed" (board
    0 queued it for the hosting board), then one final state - "applied",
    "rejected_not_ready", "rejected_bad_team", "relay_failed", or
    "unconfirmed" (no final ack within OVERRIDE_ACK_TIMEOUT_SEC).

    One entry per team: a newer override for the same team replaces the
    older one, matching the board, where a team holds a single pin. An ack
    is applied only if its incident_id matches the team's current entry,
    so a late ack for a replaced override can't mislabel the new one.
    Thread-safe: acks arrive on the serial read thread, queries on Flask's.
    """

    _FINAL_FOR_RESULT = {
        OVERRIDE_ACK_APPLIED: "applied",
        OVERRIDE_ACK_NOT_READY: "rejected_not_ready",
        OVERRIDE_ACK_RELAY_FAILED: "relay_failed",
        OVERRIDE_ACK_BAD_TEAM: "rejected_bad_team",
    }
    FINAL_STATES = frozenset(_FINAL_FOR_RESULT.values()) | {"unconfirmed"}

    def __init__(self, timeout_sec: float = OVERRIDE_ACK_TIMEOUT_SEC):
        self.timeout_sec = timeout_sec
        self._entries = {}  # team_id -> dict
        self._lock = threading.Lock()

    def note_sent(self, team_id: int, incident_id: int, now: float = None):
        now = time.time() if now is None else now
        with self._lock:
            self._entries[team_id] = {
                "team_id": team_id,
                "incident_id": incident_id,
                "state": "sent",
                "sent_at": now,
                "updated_at": now,
            }

    def note_ack(self, team_id: int, incident_id: int, result: int, now: float = None) -> bool:
        """Returns True if the ack updated an entry."""
        now = time.time() if now is None else now
        with self._lock:
            entry = self._entries.get(team_id)
            if entry is None or entry["incident_id"] != incident_id:
                return False
            if entry["state"] in self.FINAL_STATES:
                return False
            if result == OVERRIDE_ACK_RELAYED:
                entry["state"] = "relayed"
            elif result in self._FINAL_FOR_RESULT:
                entry["state"] = self._FINAL_FOR_RESULT[result]
            else:
                return False
            entry["updated_at"] = now
            return True

    def _expire(self, entry: dict, now: float):
        if entry["state"] not in self.FINAL_STATES and now - entry["sent_at"] > self.timeout_sec:
            entry["state"] = "unconfirmed"
            entry["updated_at"] = now

    def get(self, team_id: int, now: float = None):
        now = time.time() if now is None else now
        with self._lock:
            entry = self._entries.get(team_id)
            if entry is None:
                return None
            self._expire(entry, now)
            return dict(entry)

    def all(self, now: float = None):
        now = time.time() if now is None else now
        with self._lock:
            for entry in self._entries.values():
                self._expire(entry, now)
            return [dict(e) for e in sorted(self._entries.values(), key=lambda e: e["team_id"])]


# --- Team status reports (mirrors core/mission_link.h, MissionLinkStatus_t v2)

MISSION_LINK_STATUS_MAGIC0 = 0xF9
MISSION_LINK_STATUS_MAGIC1 = 0x9F
MISSION_LINK_STATUS_MAGIC = bytes((MISSION_LINK_STATUS_MAGIC0, MISSION_LINK_STATUS_MAGIC1))

# team_id, status, equipment_level, assigned_incident_id (u32),
# work_remaining_at_report (u16), work_rate, stamina_at_report, report_tick (u32)
_STATUS_STRUCT = struct.Struct("<BBBIHBBI")
MISSION_LINK_STATUS_PAYLOAD_SIZE = _STATUS_STRUCT.size
assert MISSION_LINK_STATUS_PAYLOAD_SIZE == 15
MISSION_LINK_STATUS_WIRE_SIZE = 2 + MISSION_LINK_STATUS_PAYLOAD_SIZE + 1  # 18

MISSION_LINK_NO_INCIDENT = 0xFFFFFFFF

# Index = TeamStatus_t value in core/mission_state.h
TEAM_STATUS_NAMES = ["AVAILABLE", "EN_ROUTE", "ON_SCENE", "RESTING"]

# Boards re-send every MISSION_LINK_HEARTBEAT_INTERVAL_MS (2120 ms) even when
# nothing changed; three missed heartbeats means the team's board has gone quiet.
MISSION_LINK_HEARTBEAT_INTERVAL_SEC = 2.12
TEAM_STATUS_STALE_SEC = 3 * MISSION_LINK_HEARTBEAT_INTERVAL_SEC


def mission_link_status_checksum(payload: bytes) -> int:
    """Mirrors core/mission_link.c's mission_link_checksum()."""
    return sum(payload) & 0xFF


def pack_mission_link_status(team_id: int, status: int, equipment_level: int,
                             assigned_incident_id: int, work_remaining: int,
                             work_rate: int, stamina: int, report_tick: int) -> bytes:
    """Mirrors mission_link_pack_status(). The laptop never sends these;
    this exists for tests."""
    payload = _STATUS_STRUCT.pack(team_id, status, equipment_level, assigned_incident_id,
                                  work_remaining, work_rate, stamina, report_tick)
    return MISSION_LINK_STATUS_MAGIC + payload + bytes((mission_link_status_checksum(payload),))


class MissionLinkStatusParser:
    """Byte-stream scanner for MissionLinkStatus_t frames (board 0 writes its
    own teams' reports and forwards the other boards'), same shape and
    resync policy as OverrideAckParser. feed() returns events:
        ("status", {team_id, status, equipment_level, assigned_incident_id,
                    work_remaining_at_report, work_rate, stamina_at_report,
                    report_tick})
        ("passthrough", bytes)  -- not part of a status frame; feed onward.
    """

    def __init__(self):
        self._buf = bytearray()

    def feed(self, data: bytes):
        if data:
            self._buf.extend(data)

        events = []

        while True:
            idx = self._buf.find(MISSION_LINK_STATUS_MAGIC)

            if idx == -1:
                if self._buf and self._buf[-1] == MISSION_LINK_STATUS_MAGIC0:
                    if len(self._buf) > 1:
                        events.append(("passthrough", bytes(self._buf[:-1])))
                        del self._buf[:-1]
                elif self._buf:
                    events.append(("passthrough", bytes(self._buf)))
                    self._buf.clear()
                break

            if idx > 0:
                events.append(("passthrough", bytes(self._buf[:idx])))
                del self._buf[:idx]

            if len(self._buf) < MISSION_LINK_STATUS_WIRE_SIZE:
                break

            payload = bytes(self._buf[2:2 + MISSION_LINK_STATUS_PAYLOAD_SIZE])
            if mission_link_status_checksum(payload) == self._buf[2 + MISSION_LINK_STATUS_PAYLOAD_SIZE]:
                (team_id, status, equipment_level, assigned_incident_id, work_remaining,
                 work_rate, stamina, report_tick) = _STATUS_STRUCT.unpack(payload)
                events.append(("status", {
                    "team_id": team_id,
                    "status": status,
                    "equipment_level": equipment_level,
                    "assigned_incident_id": assigned_incident_id,
                    "work_remaining_at_report": work_remaining,
                    "work_rate": work_rate,
                    "stamina_at_report": stamina,
                    "report_tick": report_tick,
                }))
                del self._buf[:MISSION_LINK_STATUS_WIRE_SIZE]
            else:
                events.append(("passthrough", bytes(self._buf[:1])))
                del self._buf[:1]

        return events


class TeamStatusTracker:
    """Latest MissionLinkStatus_t report per team, last write wins (the
    board re-anchors on every report - see mission_link.h section 4).

    get()/all() add received_at, age_sec, stale (no report for
    TEAM_STATUS_STALE_SEC, i.e. several missed heartbeats), status_name and
    assigned (False when assigned_incident_id is MISSION_LINK_NO_INCIDENT).
    No laptop-side extrapolation of work/stamina here: report_tick is the
    board's sim tick, which the laptop has no clock for.
    Thread-safe: reports arrive on the serial read thread, queries on Flask's.
    """

    def __init__(self, stale_sec: float = TEAM_STATUS_STALE_SEC):
        self.stale_sec = stale_sec
        self._entries = {}  # team_id -> dict
        self._lock = threading.Lock()

    def note(self, report: dict, now: float = None):
        now = time.time() if now is None else now
        with self._lock:
            entry = dict(report)
            entry["received_at"] = now
            self._entries[report["team_id"]] = entry

    def _view(self, entry: dict, now: float) -> dict:
        view = dict(entry)
        view["age_sec"] = max(0.0, now - entry["received_at"])
        view["stale"] = view["age_sec"] > self.stale_sec
        status = entry["status"]
        view["status_name"] = TEAM_STATUS_NAMES[status] if status < len(TEAM_STATUS_NAMES) else None
        view["assigned"] = entry["assigned_incident_id"] != MISSION_LINK_NO_INCIDENT
        return view

    def get(self, team_id: int, now: float = None):
        now = time.time() if now is None else now
        with self._lock:
            entry = self._entries.get(team_id)
            return None if entry is None else self._view(entry, now)

    def all(self, now: float = None):
        now = time.time() if now is None else now
        with self._lock:
            return [self._view(self._entries[t], now) for t in sorted(self._entries)]


class HandshakeCoordinator:
    """Laptop-side counterpart to core/mission_link_handshake.c's
    board-side state machine (see that file's top-of-file comment for
    the full six-step sequence this implements the laptop's half of).

    GENERALIZED to N boards (see CHANGELOG.md) - was hardcoded to
    exactly board 0 (DK) + board 1 (NUCLEO). NUM_BOARDS below is the
    one knob: change it and every method here follows without further
    edits, mirroring core/mission_link_handshake.c's own
    HANDSHAKE_MAX_BOARDS / comms_transport_board_count() split (a
    compile-time cap there vs. a runtime constant here, since Python
    has no macros, but the same "one number drives the loop bounds"
    idea). With today's actual project (NUM_TEAMS=10, 2 boards),
    NUM_BOARDS=2 behaves identically to the old board0/board1-named
    version - this generalization changes nothing about today's actual
    behavior, only what happens if a board is added or removed.

    The actual problem this solves: without this, the laptop has no
    way to know every board is alive and past its own init before it
    starts pushing MissionLinkRoster_t/Incident/Fault traffic at teams
    that might not exist yet on the receiving end - see
    core/mission_link_handshake.h's own top comment. A plain-text
    `READY` line (the previous approach) could not distinguish "DK is
    alive but the other board isn't" from "every board is alive", had
    no notion of a reply actually confirming completion of the
    SPECIFIC board's own init (fault_injector_init/watchdog_init/etc -
    see MissionLinkHandshakeReply_t.ready's doc) rather than merely
    "the UART ISR is up", and had no retry/timeout policy at all -
    this class's job is to be a small, testable state machine that
    answers exactly one question correctly: is it safe to proceed, and
    if not, what should the operator be told about why.

    Sequence, laptop side only (see core/mission_link_handshake.c for
    the board side of each numbered step):

      1. due_for_req() goes true immediately once start() is called;
         the caller (TelemetryBridge._read_loop below) sends
         pack_handshake_req(0) to DK and calls note_req_sent().
      2. DK's Reply{board_id=0} arrives on the wire; feed() decodes it
         via HandshakeReplyParser and records it.
      3-4. DK relays every OTHER board's Reply{board_id=peer} (or,
         after DK's own per-peer timeout, a synthetic ready=0 one -
         see core/mission_link_handshake.c's timeout policy)
         whenever it has one, unprompted - the laptop never sends a
         second kind of Req asking about a specific peer. feed()
         records whichever board_id shows up, in whatever order they
         arrive.
      5. all_ready is true only once board_id 0 AND every peer board
         id in range(1, NUM_BOARDS) have replied ready=True - order of
         arrival does not matter (see
         laptop/tools/handshake_selftest.py for the out-of-order proof).
      6. Timeout/retry policy (laptop side; see core/mission_link_
         handshake.c's own comment for why this exists on both ends
         rather than trusting DK's tick-based timeout alone): if a
         peer never replies at all within PEER_TIMEOUT_SEC of start()
         (covers the case where DK itself isn't relaying anything -
         e.g. this handshake logic isn't flashed onto DK yet - which
         DK's own tick-based timeout can't detect since it depends on
         DK's firmware actually running), OR DK relays its own
         synthetic ready=0 for that peer, that peer is "timed out".
         Neither case flips all_ready or lets traffic proceed
         automatically - can_proceed only additionally allows it once
         the operator has explicitly called
         request_dk_only_override() (see app.py's
         /api/board/continue-without-nucleo route), and only once
         EVERY currently-not-ready peer has genuinely timed out (not
         just some of them - a peer that might still reply in time
         should never be silently skipped), so a demo never silently
         runs degraded without the operator confirming it - it also
         never blocks forever, since a timed-out peer gives the
         dashboard something concrete to show and act on instead of an
         indefinite spinner.
    """

    REQ_RETRY_INTERVAL_SEC = 2.0
    # Slow keepalive Req even after board 0 has replied ready. A DK that
    # reboots while the laptop stays connected waits at its handshake gate
    # for a Req, and without this the laptop (which still believes board 0
    # is ready) never sends one again - the same "asker stops asking" hang
    # board 0 had toward rebooted LoRa peers. The DK answers every Req it
    # receives (mission_link_task.c g_own_reply_resend_pending), and
    # note_reply() just overwrites, so an up-to-date DK costs one 6-byte
    # reply per interval.
    REQ_KEEPALIVE_INTERVAL_SEC = 5.0
    PEER_TIMEOUT_SEC = 30.0

    #: Total boards this deployment expects, board 0 (DK) included.
    #: THE one knob to change if a board is added or removed - see
    #: this class's own docstring above. Board ids expected on the
    #: wire: 0..NUM_BOARDS-1.
    NUM_BOARDS = 2

    def __init__(self):
        self.replies = {}  # board_id -> {"protocol_version": int, "ready": bool}
        self._parser = HandshakeReplyParser()
        self._started_at = None
        self._req_sent_at = None
        self.dk_only_override = False

    @property
    def peer_board_ids(self):
        """Every non-DK board id this deployment expects, derived
        from NUM_BOARDS - the one place this range is computed, so
        every other method below calls this instead of re-deriving
        it (same reasoning as core/comms_transport.c's
        comms_transport_board_count() being the single source of
        board count on the C side)."""
        return range(1, self.NUM_BOARDS)

    def start(self):
        """Call once when the laptop begins trying to reach the
        board(s) - e.g. right after the serial port opens."""
        self.replies = {}
        self._started_at = time.time()
        self._req_sent_at = None
        self.dk_only_override = False

    def feed(self, data: bytes) -> bytes:
        """Feed raw USART1 bytes. Returns whatever was NOT part of a
        valid Reply frame, for the caller to pass on to
        TelemetryFrameParser next (see this module's top-of-file wire
        format note and TelemetryBridge._read_loop below)."""
        leftover = bytearray()
        for kind, payload in self._parser.feed(data):
            if kind == "reply":
                self.note_reply(payload["board_id"], payload["protocol_version"], payload["ready"])
            else:
                leftover.extend(payload)
        return bytes(leftover)

    def note_reply(self, board_id: int, protocol_version: int, ready: bool):
        """Records a reply. Exposed directly (not just via feed())
        so tests can drive this state machine without needing to
        round-trip through the byte-stream codec - see
        laptop/tools/handshake_selftest.py."""
        self.replies[board_id] = {
            "protocol_version": protocol_version,
            "ready": bool(ready),
        }

    def due_for_req(self) -> bool:
        """Whether a fresh MissionLinkHandshakeReq_t{board_id=0} is
        due right now."""
        if self._started_at is None:
            return False
        if self._req_sent_at is None:
            return True
        interval = (self.REQ_KEEPALIVE_INTERVAL_SEC if self.board0_ready
                    else self.REQ_RETRY_INTERVAL_SEC)
        return (time.time() - self._req_sent_at) >= interval

    def note_req_sent(self):
        self._req_sent_at = time.time()

    @property
    def board0_ready(self) -> bool:
        return self.replies.get(0, {}).get("ready", False)

    def board_ready(self, board_id: int) -> bool:
        """Whether a specific peer board has confirmed ready. Use
        this (not a per-board named property) for any peer id -
        the generalized replacement for the old board1_ready."""
        return self.replies.get(board_id, {}).get("ready", False)

    def board_timed_out(self, board_id: int) -> bool:
        """True once there is a definitive negative signal for this
        specific peer board: either DK's own synthetic ready=False
        relay for it (see this class's docstring, step 6), or this
        class's own PEER_TIMEOUT_SEC backstop elapsing with nothing
        at all recorded for this board_id yet. The generalized
        replacement for the old board1_timed_out - every peer is
        checked independently, on the same shared start_at clock
        (mirrors core/mission_link_handshake.c's own single shared
        g_board_init_tick for the same reason: every relay leg is
        expected to start around the same time, at boot)."""
        if board_id in self.replies and not self.replies[board_id]["ready"]:
            return True
        if self._started_at is None or self.board_ready(board_id):
            return False
        return (time.time() - self._started_at) >= self.PEER_TIMEOUT_SEC

    @property
    def all_peers_ready(self) -> bool:
        return all(self.board_ready(b) for b in self.peer_board_ids)

    @property
    def any_peer_timed_out(self) -> bool:
        return any(self.board_timed_out(b) for b in self.peer_board_ids)

    @property
    def all_ready(self) -> bool:
        """True only once board 0 AND every peer board have confirmed
        ready - the property laptop/tools/handshake_selftest.py exists
        to check, since this is the exact thing a naive implementation
        (flip ready as soon as ANY reply arrives) would get wrong for
        whichever board replies last."""
        return self.board0_ready and self.all_peers_ready

    @property
    def can_proceed(self) -> bool:
        """True once it is safe for the laptop to send roster/
        incident/fault traffic and for the dashboard to enable its
        "start simulation" control: either every board is ready, or
        board 0 is ready and the operator has explicitly accepted
        running degraded after request_dk_only_override() confirmed
        every still-not-ready peer has genuinely timed out."""
        return self.all_ready or (self.board0_ready and self.dk_only_override)

    def request_dk_only_override(self) -> bool:
        """Operator-driven escape hatch (app.py's /api/board/continue-
        without-nucleo route). Only takes effect once board 0 is ready
        AND every peer that isn't already ready has genuinely timed
        out (not just some of them - a peer that might still reply in
        time is never silently skipped), so a stray/early call can't
        skip the check it exists to gate. Returns whether the
        override is now in effect."""
        not_ready_peers = [b for b in self.peer_board_ids if not self.board_ready(b)]
        every_not_ready_peer_timed_out = all(self.board_timed_out(b) for b in not_ready_peers)
        if self.board0_ready and every_not_ready_peer_timed_out:
            self.dk_only_override = True
        return self.dk_only_override

    def status_dict(self) -> dict:
        """JSON-serializable snapshot for app.py's /api/status.
        peer_ready/peer_timed_out are keyed by board_id (string keys,
        since JSON object keys are always strings) rather than named
        per-board fields - generalized from the old board1_ready/
        board1_timed_out pair, which only worked for exactly one
        peer."""
        return {
            "board0_ready": self.board0_ready,
            "peer_ready": {str(b): self.board_ready(b) for b in self.peer_board_ids},
            "peer_timed_out": {str(b): self.board_timed_out(b) for b in self.peer_board_ids},
            "all_ready": self.all_ready,
            "any_peer_timed_out": self.any_peer_timed_out,
            "dk_only_override": self.dk_only_override,
            "can_proceed": self.can_proceed,
        }


class TelemetryBridge:
    """Manages serial communication, packet parsing, and command transmission

    between the laptop and the target STM32N6 board. This is the one
    implementation: app.py's Flask dashboard imports it (app.py used to
    carry its own copy of this class, which had to be kept in step by hand).
    """

    def __init__(self, port: str = "/dev/ttyACM0", baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.serial_conn = None
        self.running = False
        self.nodes_ready = set()
        self.telemetry_queue = queue.Queue()
        self.thread = None
        self.mission_started = False
        self._parser = TelemetryFrameParser()
        self._text_buf = bytearray()
        self.handshake = HandshakeCoordinator()
        self._override_ack_parser = OverrideAckParser()
        self.overrides = OverrideTracker()
        self._status_parser = MissionLinkStatusParser()
        self.team_status = TeamStatusTracker()

    def connect(self, port: str = None, baudrate: int = None) -> bool:
        """Opens the USB serial port connected to the STM32N6 board and spins up

        the background read loop thread for asynchronous stream ingestion.
        port/baudrate override the constructor's values when given.
        """
        if serial is None:
            print("Failed to connect: pyserial is not installed "
                  "(pip install pyserial, or run app.py in sim-only "
                  "mode instead - see SIM_ONLY/--no-hardware)")
            return False
        try:
            if port is not None:
                self.port = port
            if baudrate is not None:
                self.baudrate = int(baudrate)
            # 0.1s timeout ensures the read loop can check self.running flags
            # promptly during disconnect requests without locking indefinitely.
            self.serial_conn = serial.Serial(
                self.port, self.baudrate, timeout=0.1
            )
            self.running = True
            self.nodes_ready.clear()
            self.mission_started = False
            self.handshake.start()
            self.thread = threading.Thread(
                target=self._read_loop, daemon=True
            )
            self.thread.start()
            return True
        except Exception as exc:  # noqa: BLE001
            print(f"Failed to connect to serial port {self.port}: {exc}")
            return False

    def _handle_text_line(self, line: str):
        """Same READY-handshake / generic log handling the old line-based
        reader did, just now invoked from complete lines extracted out of
        the raw byte stream instead of from serial.readline()."""
        if not line:
            return

        if "READY" in line:
            parts = line.split(":")
            node_id = parts[0].strip() if len(parts) > 1 else "STM32N6_Board"
            self.nodes_ready.add(node_id)
        else:
            # Echo raw board stdout logs (tm_printf debug lines) for
            # debugging / monitor visibility.
            print(f"[Board Log] {line}")

    def _consume_text_bytes(self, data: bytes):
        """Accumulate raw text bytes (anything the frame parser didn't
        recognize as part of a valid binary frame) and dispatch complete
        newline-delimited lines through the existing text/log path."""
        self._text_buf.extend(data)
        while b"\n" in self._text_buf:
            line_bytes, _, rest = self._text_buf.partition(b"\n")
            self._text_buf = bytearray(rest)
            line = line_bytes.decode("utf-8", errors="ignore").strip()
            self._handle_text_line(line)

    def _read_loop(self):
        """Background daemon thread listening for incoming byte streams.

        Reads raw bytes (this is a binary wire format, not line-delimited
        text) and scans for magic-byte-framed frames of BOTH kinds that
        can appear on this wire: handshake replies
        (core/mission_link_handshake.h, via self.handshake.feed()) first,
        then telemetry frames (core/telemetry_frame.h, via self._parser)
        on whatever bytes the handshake scanner didn't recognize - see
        this module's top-of-file wire format note for why handshake
        frames need their own byte-level scan rather than flowing through
        the old newline-delimited text/log path the way plain `READY`
        lines used to. Decoded telemetry frames go on telemetry_queue;
        decoded handshake replies update self.handshake directly (no
        queue - app.py reads self.handshake.status_dict() synchronously,
        same as it already reads self.nodes_ready); anything left over
        (general tm_printf debug output) flows through the existing
        line-based log path.

        Also drives the handshake's own retry timer
        (self.handshake.due_for_req()): this loop's 0.1s read timeout
        already gives it a steady ~10Hz cadence, so no second thread is
        needed just to resend MissionLinkHandshakeReq_t on schedule.
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

                self.ingest_bytes(chunk)

            except Exception as e:
                print(f"Serial read error: {e}")
                break

    def ingest_bytes(self, chunk: bytes):
        """One pass of raw USART1 bytes through every scanner: handshake
        replies, override acks, team status reports, then telemetry frames;
        whatever is left is debug text. Split out of _read_loop so tests can
        drive the whole chain without a serial port.

        The telemetry parser goes last because it drops the first byte of
        a candidate that fails its checksum; every scanner before it passes
        such bytes on, so a magic pair inside another frame can't damage it."""
        leftover = self.handshake.feed(chunk)

        after_acks = bytearray()
        for kind, payload in self._override_ack_parser.feed(leftover):
            if kind == "ack":
                self.overrides.note_ack(payload["team_id"], payload["incident_id"],
                                        payload["result"])
            else:
                after_acks.extend(payload)

        remaining = bytearray()
        for kind, payload in self._status_parser.feed(bytes(after_acks)):
            if kind == "status":
                self.team_status.note(payload)
            else:
                remaining.extend(payload)

        for kind, payload in self._parser.feed(bytes(remaining)):
            if kind == "frame":
                self.telemetry_queue.put(payload)
            else:
                self._consume_text_bytes(payload)

    def send_start(self) -> bool:
        """Transmits the explicit text/binary START command packet down the USB line

        to transition the board's mission state from IDLE_WAITING_FOR_HOST to
        MISSION_STATE_EXECUTING (triggering init_search_zones()).

        Now gated on self.handshake.can_proceed rather than firing
        unconditionally: the previous version of this method wrote
        directly into a channel nothing on the board actually listens on
        yet (see this module's top-of-file note and core/mission_link_
        handshake.h's own "actual problem this solves" framing) with no
        check that either board was even alive. The gate fixes the
        "writes into nothing" half of that problem for THIS method: a
        caller can no longer send a command before both boards (or an
        explicit DK-only override - see HandshakeCoordinator.
        request_dk_only_override()) have confirmed readiness.

        What this method still does NOT fix, and is explicitly out of
        scope here: there is no real START wire protocol on the board
        side at all yet (main_board.c currently begins running its sim
        immediately on boot with hardcoded BOARD_SEED/BOARD_SEED_COUNT
        constants - see that file - rather than waiting for any command
        from the laptop). The b"START\\n" text write below is therefore
        still a stub in the sense that nothing currently reads it; unlike
        the handshake itself, inventing a real board-side START protocol
        is a different prompt's problem, not silently solved here.
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
                print(f"Failed to transmit START command: {e}")
        return False

    def disconnect(self):
        """Safely terminates the background read loop, joins threads, and

        closes the underlying serial connection.
        """
        self.running = False
        if self.thread:
            self.thread.join(timeout=1.0)
        if self.serial_conn and self.serial_conn.is_open:
            self.serial_conn.close()
            print("Closed USB serial connection.")
        self.serial_conn = None
        self.nodes_ready.clear()
        self.mission_started = False
        self.handshake = HandshakeCoordinator()

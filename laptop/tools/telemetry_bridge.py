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

import json
import queue
import shutil
import struct
import subprocess
import tempfile
import threading
import time
from collections import OrderedDict

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


# --- Team status reports (mirrors core/mission_link.h, MissionLinkStatus_t v3)

MISSION_LINK_STATUS_MAGIC0 = 0xF9
MISSION_LINK_STATUS_MAGIC1 = 0x9F
MISSION_LINK_STATUS_MAGIC = bytes((MISSION_LINK_STATUS_MAGIC0, MISSION_LINK_STATUS_MAGIC1))

# team_id, status, equipment_level, assigned_incident_id (u32),
# work_remaining_at_report (u16), work_rate, stamina_at_report, report_tick (u32),
# loc_x (i16), loc_y (i16)
_STATUS_STRUCT = struct.Struct("<BBBIHBBIhh")
MISSION_LINK_STATUS_PAYLOAD_SIZE = _STATUS_STRUCT.size
assert MISSION_LINK_STATUS_PAYLOAD_SIZE == 19
MISSION_LINK_STATUS_WIRE_SIZE = 2 + MISSION_LINK_STATUS_PAYLOAD_SIZE + 1  # 22

MISSION_LINK_NO_INCIDENT = 0xFFFFFFFF

# Index = TeamStatus_t value in core/mission_state.h
TEAM_STATUS_NAMES = ["AVAILABLE", "EN_ROUTE", "ON_SCENE", "RESTING"]

# Boards re-send every MISSION_LINK_HEARTBEAT_INTERVAL_MS (core/mission_link.h -
# 5000 ms, recalibrated from 2120 ms alongside CBBA_ROUND_TIMEOUT_MS's own
# recalibration in core/cbba.h - keep this literal in sync with that macro,
# there is no automatic link between them) even when nothing changed; three
# missed heartbeats means the team's board has gone quiet.
MISSION_LINK_HEARTBEAT_INTERVAL_SEC = 5.0
TEAM_STATUS_STALE_SEC = 3 * MISSION_LINK_HEARTBEAT_INTERVAL_SEC


def mission_link_status_checksum(payload: bytes) -> int:
    """Mirrors core/mission_link.c's mission_link_checksum()."""
    return sum(payload) & 0xFF


def pack_mission_link_status(team_id: int, status: int, equipment_level: int,
                             assigned_incident_id: int, work_remaining: int,
                             work_rate: int, stamina: int, report_tick: int,
                             loc_x: int = 0, loc_y: int = 0) -> bytes:
    """Mirrors mission_link_pack_status(). The laptop never sends these to a
    board; the bridge re-packs decoded reports for the mission simulator's
    stdin (MissionFeed.feed_status) and tests build them here."""
    payload = _STATUS_STRUCT.pack(team_id, status, equipment_level, assigned_incident_id,
                                  work_remaining, work_rate, stamina, report_tick,
                                  loc_x, loc_y)
    return MISSION_LINK_STATUS_MAGIC + payload + bytes((mission_link_status_checksum(payload),))


class MissionLinkStatusParser:
    """Byte-stream scanner for MissionLinkStatus_t frames (board 0 writes its
    own teams' reports and forwards the other boards'), same shape and
    resync policy as OverrideAckParser. feed() returns events:
        ("status", {team_id, status, equipment_level, assigned_incident_id,
                    work_remaining_at_report, work_rate, stamina_at_report,
                    report_tick, loc_x, loc_y})
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
                 work_rate, stamina, report_tick, loc_x, loc_y) = _STATUS_STRUCT.unpack(payload)
                events.append(("status", {
                    "team_id": team_id,
                    "status": status,
                    "equipment_level": equipment_level,
                    "assigned_incident_id": assigned_incident_id,
                    "work_remaining_at_report": work_remaining,
                    "work_rate": work_rate,
                    "stamina_at_report": stamina,
                    "report_tick": report_tick,
                    "loc_x": loc_x,
                    "loc_y": loc_y,
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
    No extrapolation of work/stamina here; during a board mission the
    simulator's mirror does that (laptop/mission_mirror.h) and the dashboard
    shows its LIVEFRAMEs.
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


# --- Board mission: laptop <-> board frames ---------------------------------
#
# The laptop is the world authority and the mission clock (core/mission_link.h);
# the boards run the teams. `tron <seed> 1 <ticks> --mission-feed --live`
# runs the world side (laptop/mission_mirror.h):
#   stdout  "MLFRAME <tick> <hex>" - roster/incident/fault/clock frames the
#           boards need (laptop/mission_feed.h), plus LIVEFRAME/STATICENV JSON
#           for the dashboard, built from the boards' reports;
#   stdin   "MLSTATUS <hex>" - every team status report board 0 writes.
# MissionFeed runs that process, puts its frames on the serial port, and hands
# it the reports.

MISSION_LINK_ROSTER_MAGIC = bytes((0xC3, 0x3C))
MISSION_LINK_INCIDENT_MAGIC = bytes((0xD4, 0x4D))
MISSION_LINK_FAULT_MAGIC = bytes((0xE7, 0x7E))
MISSION_LINK_CLOCK_MAGIC = bytes((0xCB, 0xBC))
MISSION_LINK_MAP_ROW_MAGIC = bytes((0xDA, 0xAD))
MISSION_LINK_SITES_MAGIC = bytes((0xE5, 0x5E))
# Feature-contract Decision D (single-board submission): district map and
# zone trust, USART1 only - see core/mission_link.h sections 5e/5f.
MISSION_LINK_DISTRICT_ROW_MAGIC = bytes((0xB6, 0x6B))
MISSION_LINK_ZONE_TRUST_MAGIC = bytes((0xA8, 0x8A))
MISSION_LINK_ROSTER_WIRE_SIZE = 13
MISSION_LINK_INCIDENT_WIRE_SIZE = 19
MISSION_LINK_FAULT_WIRE_SIZE = 14
MISSION_LINK_CLOCK_WIRE_SIZE = 7
MISSION_LINK_MAP_ROW_WIRE_SIZE = 40
MISSION_LINK_SITES_WIRE_SIZE = 36
MISSION_LINK_DISTRICT_ROW_WIRE_SIZE = 22
MISSION_LINK_ZONE_TRUST_WIRE_SIZE = 53
INCIDENT_STATUS_RESOLVED = 3  # IncidentStatus_t INC_RESOLVED
_INCIDENT_STATUS_WIRE_OFFSET = 2 + 13  # magic + (id 4, loc 4, work 2, priority, type, req)

# core/mission_state.h TICK_TIME_SEC: one sim tick is 36 s of real time.
TICK_TIME_SEC = 36


def tick_delay_ms_for_time_scale(time_scale: float) -> int:
    """Wall-clock milliseconds per tick for a mission running time_scale
    times faster than real life (1 = real time, one tick per 36 s)."""
    if not time_scale > 0:
        raise ValueError("time_scale must be positive")
    return max(1, int(round(TICK_TIME_SEC * 1000 / time_scale)))


def mission_frame_key(frame: bytes):
    """("roster", team) / ("incident", incident_id) / ("fault", team) /
    ("clock", 0) / ("map_row", y) / ("sites", 0) / ("district_row", y) /
    ("zone_trust", 0) for a complete, checksum-valid mission-link wire
    frame, else None."""
    magic = frame[:2]
    sizes = {
        MISSION_LINK_ROSTER_MAGIC: MISSION_LINK_ROSTER_WIRE_SIZE,
        MISSION_LINK_INCIDENT_MAGIC: MISSION_LINK_INCIDENT_WIRE_SIZE,
        MISSION_LINK_FAULT_MAGIC: MISSION_LINK_FAULT_WIRE_SIZE,
        MISSION_LINK_CLOCK_MAGIC: MISSION_LINK_CLOCK_WIRE_SIZE,
        MISSION_LINK_MAP_ROW_MAGIC: MISSION_LINK_MAP_ROW_WIRE_SIZE,
        MISSION_LINK_SITES_MAGIC: MISSION_LINK_SITES_WIRE_SIZE,
        MISSION_LINK_DISTRICT_ROW_MAGIC: MISSION_LINK_DISTRICT_ROW_WIRE_SIZE,
        MISSION_LINK_ZONE_TRUST_MAGIC: MISSION_LINK_ZONE_TRUST_WIRE_SIZE,
    }
    size = sizes.get(magic)
    if size is None or len(frame) != size or (sum(frame[2:-1]) & 0xFF) != frame[-1]:
        return None
    if magic == MISSION_LINK_INCIDENT_MAGIC:
        return ("incident", struct.unpack_from("<I", frame, 2)[0])
    if magic == MISSION_LINK_CLOCK_MAGIC:
        return ("clock", 0)
    if magic == MISSION_LINK_MAP_ROW_MAGIC:
        return ("map_row", frame[2])
    if magic == MISSION_LINK_SITES_MAGIC:
        return ("sites", 0)
    if magic == MISSION_LINK_DISTRICT_ROW_MAGIC:
        return ("district_row", frame[2])
    if magic == MISSION_LINK_ZONE_TRUST_MAGIC:
        return ("zone_trust", 0)
    return ("roster" if magic == MISSION_LINK_ROSTER_MAGIC else "fault", frame[2])


class MissionFeed:
    """Runs one board mission's world simulation and connects it to board 0.

    Newest state wins: a pending frame for the same roster team, incident id,
    fault team or the clock is replaced in place, so a slow link never builds
    a backlog of stale updates. Frames leave at most MAX_FRAMES_PER_SEC,
    spaced out so board 0's UART and relay queues are not flooded.

    Re-sends, because a frame relayed over LoRa can be lost:
    - every roster, and the sites, every ROSTER_RESEND_SEC (boards apply one
      roster per team per boot and ignore repeats, so this is safe
      mid-mission - lost-frame recovery only, NOT reboot recovery, see
      force_full_resend()'s own doc below for why those two got split);
    - every route map row every MAP_RESEND_SEC (changed rows already go out
      as they change; this is lost-row recovery only, same split as above);
    - one still-open incident every INCIDENT_REFRESH_SEC, round-robin (boards
      keep their own execution state over a re-send - see
      mission_link_apply_incident());
    - alongside it, one recently RESOLVED incident every INCIDENT_REFRESH_SEC,
      round-robin, each at most RESOLVED_RESEND_COUNT more times and for at
      most RESOLVED_RESEND_WINDOW_SEC after its first re-send, keeping at
      most RESOLVED_KEPT of them (oldest forgotten first). A RESOLVED frame
      lost on the LoRa relay used to be gone for good: the incident left
      _open_incidents, so nothing ever re-sent it, and board 1 kept a stale
      incident - and the slot it occupies - forever (review finding M4).
      Re-sending it is harmless: a board that already closed it has no slot
      for it and ignores the frame (mission_link_apply_incident()).
    - the current clock tick every CLOCK_RESEND_SEC, so a lost clock frame
      delays a board by seconds rather than a whole tick.

    ROSTER_RESEND_SEC/MAP_RESEND_SEC RECALIBRATED (were 5.0/30.0): a board
    that has never rebooted needs neither one to fire quickly - the only
    thing that used to depend on that speed was reboot recovery, which
    force_full_resend() below now covers immediately (triggered off
    HandshakeCoordinator's own reboot-detection edge, not this timer) and
    at 5.0/30.0 these two backstops sent, continuously, even with nothing
    actually changed: (NUM_TEAMS + 1 sites) roster-tier frames every 5s =
    11/5 = 2.2 frames/sec, plus GRID_HEIGHT map-row frames every 30s =
    36/30 = 1.2 frames/sec - 3.4 frames/sec of pure backstop traffic
    against a MAX_FRAMES_PER_SEC ceiling of 10, i.e. up to a third of this
    link's entire USART1/relay budget spent re-sending data nothing ever
    asked to have re-sent. Lengthened 4x each (11/20 = 0.55/sec, 36/120 =
    0.3/sec -> 0.85/sec combined, a ~75% cut) rather than further: this is
    now purely a lost-single-frame backstop, and 20s/120s is still well
    inside what a real drop should tolerate (map cells outlive a single
    sim tick by design - see core/search_zones.c - and one missed roster
    re-send just means waiting for the next one, never a hang, since
    force_full_resend() is the actual boot-blocking path now).

    feed_line()/pump() are the whole frame logic and take an explicit clock,
    so tests drive them directly; start() only adds the subprocess and
    threads.
    """

    MAX_FRAMES_PER_SEC = 10.0
    ROSTER_RESEND_SEC = 20.0
    INCIDENT_REFRESH_SEC = 2.0
    CLOCK_RESEND_SEC = 3.0
    MAP_RESEND_SEC = 120.0
    RESOLVED_RESEND_COUNT = 5
    RESOLVED_RESEND_WINDOW_SEC = 60.0
    RESOLVED_KEPT = 40              # 2 x NUM_INCIDENTS
    LIVE_FRAMES_KEPT = 50

    def __init__(self, write_fn, clock=time.monotonic):
        self._write = write_fn
        self._clock = clock
        self._lock = threading.Lock()
        self._pending = OrderedDict()   # key -> frame bytes
        self._rosters = OrderedDict()   # team -> frame bytes
        self._open_incidents = OrderedDict()  # incident_id -> latest frame
        # incident_id -> [RESOLVED frame, re-sends left, deadline or None]
        self._resolved_incidents = OrderedDict()
        self._clock_frame = None
        self._sites_frame = None
        self._map_rows = OrderedDict()  # row -> latest frame
        # District map (mission_link.h 5e): same per-row resend bucket as
        # _map_rows above - fixed after world-gen, so this backstop is not
        # expected to ever have anything new to send, but reboot recovery
        # (force_full_resend()) still needs it. Zone trust (5f) is its own
        # entry, not a per-row table, since it's one fixed-size frame.
        self._district_rows = OrderedDict()  # row -> latest frame
        self._zone_trust_frame = None
        self._next_map_resend = None
        self._next_roster_resend = None
        self._next_incident_refresh = None
        self._next_clock_resend = None
        self._last_send = None
        self.frames_parsed = 0
        self.frames_rejected = 0
        self.frames_sent = 0
        self.reports_forwarded = 0
        self.last_tick = 0
        self.time_scale = None
        self.tick_delay_ms = None
        self._live_seq = 0
        self._live_frames = []          # [(seq, frame dict)], newest last
        self._static_env = None
        self._proc = None
        self._stdin_lock = threading.Lock()
        self._workdir = None
        self._threads = []
        self._stop = threading.Event()
        self._input_finished = threading.Event()
        self.error = None

    def feed_line(self, line: str) -> bool:
        """One simulator stdout line. MLFRAME lines are queued for the
        boards; LIVEFRAME/STATICENV lines are kept for the dashboard."""
        if line.startswith("LIVEFRAME ") or line.startswith("STATICENV "):
            return self._feed_live_line(line)
        parts = line.strip().split()
        if len(parts) != 3 or parts[0] != "MLFRAME":
            return False
        try:
            tick = int(parts[1])
            frame = bytes.fromhex(parts[2])
        except ValueError:
            self.frames_rejected += 1
            return False
        key = mission_frame_key(frame)
        if key is None:
            self.frames_rejected += 1
            return False

        with self._lock:
            self.frames_parsed += 1
            self.last_tick = tick
            kind, ident = key
            if kind == "roster":
                self._rosters[ident] = frame
            elif kind == "incident":
                if frame[_INCIDENT_STATUS_WIRE_OFFSET] == INCIDENT_STATUS_RESOLVED:
                    self._open_incidents.pop(ident, None)
                    # Deadline starts at its first re-send - feed_line() has
                    # no clock of its own (see pump()).
                    self._resolved_incidents.pop(ident, None)
                    self._resolved_incidents[ident] = [frame, self.RESOLVED_RESEND_COUNT, None]
                    while len(self._resolved_incidents) > self.RESOLVED_KEPT:
                        self._resolved_incidents.popitem(last=False)
                else:
                    self._resolved_incidents.pop(ident, None)
                    self._open_incidents[ident] = frame
            elif kind == "clock":
                self._clock_frame = frame
            elif kind == "map_row":
                self._map_rows[ident] = frame
            elif kind == "sites":
                self._sites_frame = frame
            elif kind == "district_row":
                self._district_rows[ident] = frame
            elif kind == "zone_trust":
                self._zone_trust_frame = frame
            self._pending[key] = frame
        return True

    def _feed_live_line(self, line: str) -> bool:
        kind, _, payload = line.strip().partition(" ")
        try:
            data = json.loads(payload)
        except ValueError:
            return False
        with self._lock:
            if kind == "STATICENV":
                self._static_env = data
            else:
                self._live_seq += 1
                self._live_frames.append((self._live_seq, data))
                del self._live_frames[:-self.LIVE_FRAMES_KEPT]
        return True

    def live_events(self, after_seq: int = 0):
        """(static_env or None, [(seq, frame)] newer than after_seq)."""
        with self._lock:
            return self._static_env, [(s, f) for s, f in self._live_frames if s > after_seq]

    def pump(self, now=None) -> int:
        """Schedules re-sends that are due and sends at most one frame.
        Returns the number of frames written (0 or 1)."""
        now = self._clock() if now is None else now
        with self._lock:
            if self._next_roster_resend is None:
                self._next_roster_resend = now + self.ROSTER_RESEND_SEC
                self._next_incident_refresh = now + self.INCIDENT_REFRESH_SEC
                self._next_clock_resend = now + self.CLOCK_RESEND_SEC
                self._next_map_resend = now + self.MAP_RESEND_SEC
            if now >= self._next_roster_resend:
                for team, frame in self._rosters.items():
                    self._pending.setdefault(("roster", team), frame)
                if self._sites_frame is not None:
                    self._pending.setdefault(("sites", 0), self._sites_frame)
                self._next_roster_resend = now + self.ROSTER_RESEND_SEC
            if now >= self._next_incident_refresh:
                if self._open_incidents:
                    ident, frame = next(iter(self._open_incidents.items()))
                    self._open_incidents.move_to_end(ident)
                    self._pending.setdefault(("incident", ident), frame)
                self._resend_one_resolved(now)
                self._next_incident_refresh = now + self.INCIDENT_REFRESH_SEC
            if now >= self._next_map_resend:
                for row, frame in self._map_rows.items():
                    self._pending.setdefault(("map_row", row), frame)
                for row, frame in self._district_rows.items():
                    self._pending.setdefault(("district_row", row), frame)
                if self._zone_trust_frame is not None:
                    self._pending.setdefault(("zone_trust", 0), self._zone_trust_frame)
                self._next_map_resend = now + self.MAP_RESEND_SEC
            if now >= self._next_clock_resend:
                if self._clock_frame is not None:
                    self._pending.setdefault(("clock", 0), self._clock_frame)
                self._next_clock_resend = now + self.CLOCK_RESEND_SEC

            if not self._pending:
                return 0
            if self._last_send is not None and now - self._last_send < 1.0 / self.MAX_FRAMES_PER_SEC:
                return 0
            _, frame = self._pending.popitem(last=False)
            self._last_send = now

        self._write(frame)
        with self._lock:
            self.frames_sent += 1
        return 1

    def _resend_one_resolved(self, now):
        """Queues the next recently RESOLVED incident for a re-send, and
        forgets any whose budget or window ran out. Caller holds _lock."""
        while self._resolved_incidents:
            ident, entry = next(iter(self._resolved_incidents.items()))
            frame, left, deadline = entry
            if deadline is None:
                deadline = entry[2] = now + self.RESOLVED_RESEND_WINDOW_SEC
            if left <= 0 or now > deadline:
                del self._resolved_incidents[ident]
                continue
            entry[1] = left - 1
            if entry[1] <= 0:
                del self._resolved_incidents[ident]
            else:
                self._resolved_incidents.move_to_end(ident)
            # A pending frame for this id is at least as new - keep it.
            self._pending.setdefault(("incident", ident), frame)
            return

    def force_full_resend(self):
        """Queues every roster, the sites frame, and every map row right
        now - the reboot-recovery path, decoupled from ROSTER_RESEND_SEC/
        MAP_RESEND_SEC (see this class's own docstring for why those two
        were safe to lengthen once this existed). Call this the moment
        TelemetryBridge sees a HandshakeCoordinator.consume_newly_ready()
        edge for ANY board - it doesn't matter which one rebooted, since
        this laptop process has no way to address a resend to only the
        teams that board hosts (comms_transport_board_id_for_team() is a
        board-side-only concept - see core/comms_transport.c), and
        resending to a board that didn't need it is harmless (roster
        apply is idempotent - one-shot per team per boot - and a map
        row/sites resend a board didn't need is just a frame it already
        has). Does NOT touch the periodic timers, so the normal backstop
        cadence continues unaffected on top of this."""
        with self._lock:
            for team, frame in self._rosters.items():
                self._pending.setdefault(("roster", team), frame)
            if self._sites_frame is not None:
                self._pending.setdefault(("sites", 0), self._sites_frame)
            for row, frame in self._map_rows.items():
                self._pending.setdefault(("map_row", row), frame)
            for row, frame in self._district_rows.items():
                self._pending.setdefault(("district_row", row), frame)
            if self._zone_trust_frame is not None:
                self._pending.setdefault(("zone_trust", 0), self._zone_trust_frame)

    def pending_count(self) -> int:
        with self._lock:
            return len(self._pending)

    def feed_status(self, report: dict) -> bool:
        """Hands one decoded board status report to the simulator's mirror.
        Returns False when no simulator is listening."""
        proc = self._proc
        if proc is None or proc.stdin is None or proc.poll() is not None:
            return False
        frame = pack_mission_link_status(
            report["team_id"], report["status"], report["equipment_level"],
            report["assigned_incident_id"], report["work_remaining_at_report"],
            report["work_rate"], report["stamina_at_report"], report["report_tick"],
            report.get("loc_x", 0), report.get("loc_y", 0))
        try:
            with self._stdin_lock:
                proc.stdin.write(f"MLSTATUS {frame.hex()}\n")
                proc.stdin.flush()
        except (OSError, ValueError):
            return False
        with self._lock:
            self.reports_forwarded += 1
        return True

    # -- process + threads -------------------------------------------------

    def start(self, binary, seed=1, ticks=10000, tick_delay_ms=None, fault_profile="none",
              time_scale=1.0):
        """Runs the world simulation for a board mission in a temporary
        directory (it writes a snapshot CSV into its working directory, which
        must not clobber the project's dataset). The pace is time_scale times
        real time (1 = one tick per 36 s) unless tick_delay_ms is given."""
        if tick_delay_ms is None:
            tick_delay_ms = tick_delay_ms_for_time_scale(time_scale)
        elif int(tick_delay_ms) > 0:
            time_scale = TICK_TIME_SEC * 1000 / int(tick_delay_ms)
        else:
            time_scale = None  # 0: unpaced, as fast as the host runs
        self.tick_delay_ms = int(tick_delay_ms)
        self.time_scale = time_scale
        self._workdir = tempfile.mkdtemp(prefix="tron_mission_feed_")
        args = [str(binary), str(int(seed)), "1", str(int(ticks)), "--mission-feed", "--live",
                f"--tick-delay-ms={self.tick_delay_ms}"]
        if fault_profile and fault_profile != "none":
            args += ["--fault-profile", str(fault_profile)]
        self._proc = subprocess.Popen(args, cwd=self._workdir, stdin=subprocess.PIPE,
                                      stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                      text=True, bufsize=1)
        self._threads = [threading.Thread(target=self._read_loop, daemon=True),
                         threading.Thread(target=self._send_loop, daemon=True)]
        for t in self._threads:
            t.start()

    def _read_loop(self):
        try:
            for line in self._proc.stdout:
                if self._stop.is_set():
                    break
                self.feed_line(line)
        except Exception as exc:  # noqa: BLE001
            self.error = f"feed read failed: {exc}"
        finally:
            self._input_finished.set()

    def _send_loop(self):
        while not self._stop.is_set():
            try:
                if self.pump() == 0:
                    time.sleep(0.01)
            except Exception as exc:  # noqa: BLE001
                self.error = f"serial write failed: {exc}"
                return

    @property
    def running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None and not self._stop.is_set()

    @property
    def finished(self) -> bool:
        """Stopped, or the simulator exited and all its output was read."""
        return self._stop.is_set() or (not self.running and self._input_finished.is_set())

    def stop(self):
        self._stop.set()
        if self._proc is not None and self._proc.poll() is None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                self._proc.kill()
        if self._proc is not None and self._proc.stdin is not None:
            try:
                self._proc.stdin.close()
            except OSError:
                pass
        for t in self._threads:
            t.join(timeout=1.0)
        if self._workdir:
            shutil.rmtree(self._workdir, ignore_errors=True)
            self._workdir = None

    def status_dict(self) -> dict:
        with self._lock:
            return {
                "running": self.running,
                "simulator_exit_code": None if self._proc is None else self._proc.poll(),
                # True once every line the simulator printed has been read.
                "input_finished": self._input_finished.is_set(),
                "last_tick": self.last_tick,
                "time_scale": self.time_scale,
                "tick_delay_ms": self.tick_delay_ms,
                "frames_parsed": self.frames_parsed,
                "frames_rejected": self.frames_rejected,
                "frames_sent": self.frames_sent,
                "frames_pending": len(self._pending),
                "reports_forwarded": self.reports_forwarded,
                "rosters": len(self._rosters),
                "map_rows": len(self._map_rows),
                "district_rows": len(self._district_rows),
                "zone_trust_received": self._zone_trust_frame is not None,
                "open_incidents": len(self._open_incidents),
                "resolved_resending": len(self._resolved_incidents),
                "error": self.error,
            }


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
        # False->True edges on board_ready(board_id), queued for
        # consume_newly_ready() below - see that method's own doc for why
        # this exists (reboot-triggered resend, not just first-boot).
        self._was_ready = {}
        self._newly_ready = []

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
        self._was_ready = {}
        self._newly_ready = []

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
        laptop/tools/handshake_selftest.py.

        Also tracks False->True edges on this board's readiness (see
        consume_newly_ready()'s own doc) - covers both this board's
        first-ever ready reply (edge from "never seen" - not in
        self._was_ready - to ready) and a REBOOT (DK's synthetic
        ready=False relay, see core/mission_link_task.c's peer-session-
        count check, flips this board back to not-ready first, so the
        eventual real reply again edges False->True)."""
        ready = bool(ready)
        was_ready = self._was_ready.get(board_id, False)
        if ready and not was_ready:
            self._newly_ready.append(board_id)
        self._was_ready[board_id] = ready

        self.replies[board_id] = {
            "protocol_version": protocol_version,
            "ready": ready,
        }

    def consume_newly_ready(self) -> list:
        """Board ids that have just (since the last call) transitioned
        from not-ready to ready - drains and returns the queue.

        This is the signal a rebooted board's roster/sites/map needs
        an IMMEDIATE re-send rather than waiting for MissionFeed's own
        periodic backstop (see MissionFeed.force_full_resend()'s own
        doc for why that backstop was safe to lengthen once this
        existed): a board that just rebooted has forgotten every
        roster it was ever given (core/board mission_link_task.c
        clears g_roster_applied_for_team[] on init), and the ONLY
        other way it gets that data again is this same handshake
        reply's own board_ready() edge - the exact liveness signal
        that already existed for a different purpose (gating
        can_proceed) here, not a new probe added just for this."""
        newly_ready, self._newly_ready = self._newly_ready, []
        return newly_ready

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
        self.read_errors = 0            # exceptions caught by _read_loop()
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
        # Serial writes come from the read loop (handshake Reqs), the mission
        # feed's thread and Flask routes (START, overrides); one lock keeps
        # two frames from interleaving byte-wise on the wire.
        self._write_lock = threading.Lock()
        self.mission = None

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

        Survives transient errors (logged and counted in read_errors); only
        a closed port or a port-level OSError ends it.

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
                    self.write_bytes(pack_handshake_req(0))
                    self.handshake.note_req_sent()

                waiting = self.serial_conn.in_waiting
                chunk = self.serial_conn.read(waiting if waiting else 1)
                if not chunk:
                    continue

                self.ingest_bytes(chunk)

            except OSError as e:
                # The port itself failed (pyserial's SerialException is an
                # OSError: unplugged board, closed or reset device). Retrying
                # reads on it just spins; reopening is start()'s job.
                self.read_errors += 1
                print(f"Serial read error (port lost, stopping reader): {e}")
                break
            except Exception as e:  # noqa: BLE001
                # Anything else (a parser or tracker hiccup on one odd chunk)
                # used to end this thread for good, silently leaving the
                # bridge deaf for the rest of the mission. Log, count, keep
                # reading; the loop condition still stops on a closed port.
                self.read_errors += 1
                print(f"Serial read error (continuing): {e}")
                time.sleep(0.05)

    def ingest_bytes(self, chunk: bytes):
        """One pass of raw USART1 bytes through every scanner: handshake
        replies, override acks, team status reports, then telemetry frames;
        whatever is left is debug text. Split out of _read_loop so tests can
        drive the whole chain without a serial port.

        The telemetry parser goes last because it drops the first byte of
        a candidate that fails its checksum; every scanner before it passes
        such bytes on, so a magic pair inside another frame can't damage it."""
        leftover = self.handshake.feed(chunk)

        newly_ready = self.handshake.consume_newly_ready()
        if newly_ready and self.mission is not None:
            # A board just went not-ready->ready - either first boot (its
            # own roster/map resend already covers that fine on schedule)
            # or a REBOOT (which cleared everything that board had ever
            # been given - see MissionFeed.force_full_resend()'s own doc
            # for why this can't wait for the periodic backstop anymore).
            # Harmless to fire on first boot too: force_full_resend() only
            # re-queues data already cached from this same simulator run,
            # nothing that wasn't going out anyway.
            self.mission.force_full_resend()

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
                mission = self.mission
                if mission is not None:
                    mission.feed_status(payload)
            else:
                remaining.extend(payload)

        for kind, payload in self._parser.feed(bytes(remaining)):
            if kind == "frame":
                self.telemetry_queue.put(payload)
            else:
                self._consume_text_bytes(payload)

    def write_bytes(self, data: bytes):
        """Every serial write goes through here (see _write_lock)."""
        with self._write_lock:
            self.serial_conn.write(data)

    def start_mission(self, binary, seed=1, ticks=10000, tick_delay_ms=None,
                      fault_profile="none", time_scale=1.0):
        """Starts a board mission (see MissionFeed): the world simulation,
        its frames to the boards and the boards' reports back into it. Paced
        at time_scale times real time unless tick_delay_ms is given. Same
        gate as send_start(): both boards (or an explicit DK-only override)
        must have completed the handshake, since board 0 rejects mission data
        before that. Returns (ok, message)."""
        if not self.handshake.can_proceed:
            return False, "handshake not complete"
        if not (self.serial_conn and self.serial_conn.is_open):
            return False, "board not connected"
        self.stop_mission()
        feed = MissionFeed(self.write_bytes)
        try:
            feed.start(binary, seed=seed, ticks=ticks, tick_delay_ms=tick_delay_ms,
                       fault_profile=fault_profile, time_scale=time_scale)
        except (OSError, ValueError) as exc:
            return False, f"could not start the simulator: {exc}"
        self.mission = feed
        return True, "mission feed started"

    def stop_mission(self):
        if self.mission is not None:
            self.mission.stop()
            self.mission = None

    def mission_status(self):
        return None if self.mission is None else self.mission.status_dict()

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
                self.write_bytes(b"START\n")
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
        self.stop_mission()
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

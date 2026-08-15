#!/usr/bin/env python3
"""
STM32N6 Board Telemetry Bridge and Serial Communication Layer.

This module acts as the host-side counterpart to platform/board/main_board.c
and platform/board/snapshot.c. It opens the designated USB Virtual COM port
connected to the STM32N6570-DK board, spawns a background thread to safely
consume the incoming byte stream without blocking the UI event loop, and handles:
  1. Parsing board node `READY` handshake notifications (plain text).
  2. Decoding incoming live telemetry frames into a thread-safe queue for
     real-time dashboard visualization.
  3. Transmitting explicit binary/text command packets (e.g., `START`) down
     the USB line once board nodes clear initialization.

Wire format
-----------
USART1 is a single shared line: platform/board/snapshot.c's ring drain writes
binary telemetry frames on it, and free-running tm_printf debug text from the
rest of the firmware can appear on it at any time, interleaved at arbitrary
byte boundaries. There is no separate channel to tell the two apart, so this
module reads raw bytes (not readline()) and scans for the frame boundary
itself, per the wire contract documented in core/telemetry_frame.h:

    byte 0-1   magic          0xA5 0x5A
    byte 2-11  payload        run_id (u32 LE), cycle (u32 LE),
                               team_id (u8), event_type (u8)
    byte 12    checksum       1-byte additive checksum over the payload

Total frame size: TELEMETRY_FRAME_WIRE_SIZE (13) bytes. See that header for
the full rationale (magic bytes are non-ASCII so they can't appear in
tm_printf text; checksum mismatches are an expected resync case, not an
error; gaps in `cycle` between consecutive frames are expected under the
ring's overwrite-oldest overflow policy and are not a transport error).
"""

import queue
import struct
import threading
import time
import serial


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


class TelemetryBridge:
    """Manages serial communication, packet parsing, and command transmission

    between the host Streamlit dashboard and the target STM32N6 board.
    """

    def __init__(self, port: str = "/dev/ttyACM0", baudrate: int = 115200):
        self.port = port
        self.baudrate = baudrate
        self.serial_conn = None
        self.running = False
        self.nodes_ready = set()
        self.telemetry_queue = queue.Queue()
        self.thread = None
        self._parser = TelemetryFrameParser()
        self._text_buf = bytearray()

    def connect(self) -> bool:
        """Opens the USB serial port connected to the STM32N6 board and spins up

        the background read loop thread for asynchronous stream ingestion.
        """
        try:
            # 0.1s timeout ensures the read loop can check self.running flags
            # promptly during disconnect requests without locking indefinitely.
            self.serial_conn = serial.Serial(
                self.port, self.baudrate, timeout=0.1
            )
            self.running = True
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
        text) and scans for magic-byte-framed telemetry frames per
        core/telemetry_frame.h, resyncing byte-by-byte on a checksum
        mismatch. Decoded frames go on telemetry_queue; anything left
        over (READY handshakes, general tm_printf debug output) flows
        through the same line-based log path as before.
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
        """Transmits the explicit text/binary START command packet down the USB line

        to transition the board's mission state from IDLE_WAITING_FOR_HOST to
        MISSION_STATE_EXECUTING (triggering init_search_zones()).
        """
        if self.serial_conn and self.serial_conn.is_open:
            try:
                self.serial_conn.write(b"START\n")
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

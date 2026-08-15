#!/usr/bin/env python3
"""
Standalone self-test for TelemetryFrameParser (no hardware required).

Builds a synthetic byte stream mixing:
  - plain ASCII tm_printf-style debug lines
  - well-formed binary telemetry frames (valid magic + checksum)
  - a frame with a deliberately corrupted checksum byte, to prove the
    parser resyncs (drops it, keeps scanning) instead of crashing or
    discarding everything after it
  - more valid frames and text after the corruption, to prove the
    stream recovers

Run directly: `python3 telemetry_bridge_selftest.py`
"""

import struct

from telemetry_bridge import (
    TelemetryFrameParser,
    TELEMETRY_FRAME_MAGIC0,
    TELEMETRY_FRAME_MAGIC1,
    telemetry_frame_checksum,
)

_PAYLOAD_STRUCT = struct.Struct("<IIBB")


def encode_frame(run_id, cycle, team_id, event_type, corrupt_checksum=False):
    """Build one on-wire frame: magic + payload + checksum."""
    payload = _PAYLOAD_STRUCT.pack(run_id, cycle, team_id, event_type)
    checksum = telemetry_frame_checksum(payload)
    if corrupt_checksum:
        checksum = (checksum + 1) & 0xFF  # deliberately wrong
    return bytes((TELEMETRY_FRAME_MAGIC0, TELEMETRY_FRAME_MAGIC1)) + payload + checksum.to_bytes(1, "little")


def build_stream():
    chunks = []
    chunks.append(b"tron: sim_runner_init\n")
    chunks.append(b"STM32N6_Board: READY\n")
    chunks.append(encode_frame(run_id=42, cycle=100, team_id=1, event_type=2))
    chunks.append(b"tron: seed 42 start\n")
    chunks.append(encode_frame(run_id=42, cycle=101, team_id=3, event_type=5))
    # Deliberately corrupted checksum, immediately followed by ordinary
    # ASCII debug text - proves the parser doesn't eat the following
    # legitimate text or desync permanently.
    chunks.append(encode_frame(run_id=42, cycle=102, team_id=0, event_type=9, corrupt_checksum=True))
    chunks.append(b"[DIAG] tick=102 team=0 status 1->2 stamina=88.50 hours=3\n")
    # A gap in `cycle` here (103 -> 150) is expected under the ring's
    # overwrite-oldest overflow policy, not a transport error - the
    # parser must not treat it as one.
    chunks.append(encode_frame(run_id=42, cycle=150, team_id=4, event_type=1))
    chunks.append(b"tron: seed 42 done\n")
    return b"".join(chunks)


def feed_in_pieces(parser, stream, piece_size):
    """Feed the stream through the parser in arbitrary small chunks, to
    also exercise the case where a frame's bytes straddle two reads."""
    events = []
    for i in range(0, len(stream), piece_size):
        events.extend(parser.feed(stream[i:i + piece_size]))
    events.extend(parser.feed(b""))
    return events


def main():
    stream = build_stream()
    print(f"Synthetic stream length: {len(stream)} bytes\n")

    for piece_size in (len(stream), 7, 3, 1):
        parser = TelemetryFrameParser()
        events = feed_in_pieces(parser, stream, piece_size)

        frames = [e[1] for e in events if e[0] == "frame"]
        text_chunks = [e[1] for e in events if e[0] == "text"]
        text_joined = b"".join(text_chunks).decode("utf-8", errors="ignore")

        print(f"--- feed chunk size = {piece_size} bytes ---")
        print(f"  decoded frames ({len(frames)}):")
        for f in frames:
            print(f"    {f}")
        print(f"  reassembled text:")
        for line in text_joined.splitlines():
            if line.strip():
                print(f"    {line!r}")
        print()

        # Assertions: exactly 3 valid frames decoded (the 3rd, corrupted
        # one must NOT appear), with correct field values, and no
        # exception was raised getting here.
        assert len(frames) == 3, f"expected 3 valid frames, got {len(frames)}"
        assert frames[0] == {"run_id": 42, "cycle": 100, "team_id": 1, "event_type": 2}
        assert frames[1] == {"run_id": 42, "cycle": 101, "team_id": 3, "event_type": 5}
        assert frames[2] == {"run_id": 42, "cycle": 150, "team_id": 4, "event_type": 1}
        # The debug text before and after the corrupted frame must both
        # have survived (proves resync, not stream-discard-on-error).
        assert "READY" in text_joined
        assert "seed 42 start" in text_joined
        assert "tick=102" in text_joined
        assert "seed 42 done" in text_joined

    print("ALL CHECKS PASSED: decode is correct and resync recovers "
          "cleanly after a corrupted-checksum frame, at every feed "
          "chunk size tested (whole stream, 7B, 3B, 1B-at-a-time).")


if __name__ == "__main__":
    main()

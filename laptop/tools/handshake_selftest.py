#!/usr/bin/env python3
"""
Standalone self-test for HandshakeReplyParser and HandshakeCoordinator
(no hardware required).

This is the acceptance test for core/mission_link_handshake.h's laptop
side: fakes both handshake replies arriving OUT OF ORDER and confirms
HandshakeCoordinator.all_ready only flips true once BOTH board_id 0 and
board_id 1 have replied ready - not after the first one, regardless of
which board answers first.

Also covers:
  - HandshakeReplyParser's byte-stream codec: round-trips a packed
    MissionLinkHandshakeReply_t frame, mixed with plain debug text and a
    telemetry frame on the same stream, proving it recognizes its own
    magic bytes and correctly passes everything else through unconsumed
    (see HandshakeReplyParser's docstring for why this composition, not
    a rewrite of TelemetryFrameParser, is how the two frame types share
    one wire).
  - A corrupted-checksum reply frame resyncs (dropped, one byte at a
    time) instead of desyncing the stream permanently - same resync
    contract core/mission_link_handshake.c's parse functions and
    laptop/tools/telemetry_bridge_selftest.py's existing coverage both apply to
    their own frame types.
  - DK's synthetic ready=False relay for board_id=1 (its "presumed
    absent" signal after its own timeout - see core/mission_link_
    handshake.c's timeout policy) is recorded via board_timed_out(1)
    returning True
    but does NOT flip all_ready, and does NOT let can_proceed become
    true until request_dk_only_override() is explicitly called.
  - due_for_req()'s retry timer: due immediately after start(), not due
    again immediately after note_req_sent(), and never due once board 0
    has replied ready.

Run directly: `python3 handshake_selftest.py`
"""

import struct
import time

from telemetry_bridge import (
    HandshakeCoordinator,
    HandshakeReplyParser,
    HANDSHAKE_MAGIC0,
    HANDSHAKE_MAGIC1,
    HANDSHAKE_REPLY_WIRE_SIZE,
    MISSION_LINK_PROTOCOL_VERSION,
    handshake_checksum,
    pack_handshake_req,
)
from telemetry_bridge import (
    TELEMETRY_FRAME_MAGIC0,
    TELEMETRY_FRAME_MAGIC1,
    telemetry_frame_checksum,
)

_REPLY_STRUCT = struct.Struct("<BBB")
_TELEMETRY_PAYLOAD_STRUCT = struct.Struct("<IIBB")


def encode_reply(board_id, protocol_version, ready, corrupt_checksum=False):
    """Build one on-wire MissionLinkHandshakeReply_t frame: magic +
    payload + checksum - mirrors core/mission_link_handshake.c's
    mission_link_handshake_pack_reply()."""
    payload = _REPLY_STRUCT.pack(board_id, protocol_version, 1 if ready else 0)
    checksum = handshake_checksum(payload)
    if corrupt_checksum:
        checksum = (checksum + 1) & 0xFF
    return bytes((HANDSHAKE_MAGIC0, HANDSHAKE_MAGIC1)) + payload + checksum.to_bytes(1, "little")


def encode_telemetry_frame(run_id, cycle, team_id, event_type):
    """Minimal telemetry frame encoder, mirrored here rather than
    imported, so this file's stream-mixing test doesn't need to reach
    into telemetry_bridge_selftest.py's private helper."""
    payload = _TELEMETRY_PAYLOAD_STRUCT.pack(run_id, cycle, team_id, event_type)
    checksum = telemetry_frame_checksum(payload)
    return bytes((TELEMETRY_FRAME_MAGIC0, TELEMETRY_FRAME_MAGIC1)) + payload + checksum.to_bytes(1, "little")


def test_pack_req():
    req = pack_handshake_req(0, MISSION_LINK_PROTOCOL_VERSION)
    assert req[0] == HANDSHAKE_MAGIC0 and req[1] == HANDSHAKE_MAGIC1
    assert len(req) == 5  # 2 magic + 2 payload + 1 checksum
    board_id, protocol_version = struct.unpack("<BB", req[2:4])
    assert board_id == 0
    assert protocol_version == MISSION_LINK_PROTOCOL_VERSION
    assert req[4] == handshake_checksum(req[2:4])


def test_parser_round_trip_mixed_stream():
    """Handshake replies, plain debug text, and a telemetry frame all
    on the same stream - proves HandshakeReplyParser recognizes only
    its own magic bytes and passes everything else through untouched
    for TelemetryFrameParser to handle next (see TelemetryBridge.
    _read_loop's chaining)."""
    stream = b"".join([
        b"tron: sim_runner_init\n",
        encode_reply(board_id=0, protocol_version=1, ready=True),
        b"tron: seed 1 start\n",
        encode_telemetry_frame(run_id=1, cycle=10, team_id=2, event_type=3),
        encode_reply(board_id=1, protocol_version=1, ready=True),
        b"tron: seed 1 done\n",
    ])

    for piece_size in (len(stream), 7, 3, 1):
        parser = HandshakeReplyParser()
        events = []
        for i in range(0, len(stream), piece_size):
            events.extend(parser.feed(stream[i:i + piece_size]))
        events.extend(parser.feed(b""))

        replies = [e[1] for e in events if e[0] == "reply"]
        passthrough = b"".join(e[1] for e in events if e[0] == "passthrough")

        assert len(replies) == 2, f"piece_size={piece_size}: expected 2 replies, got {len(replies)}"
        assert replies[0] == {"board_id": 0, "protocol_version": 1, "ready": True}
        assert replies[1] == {"board_id": 1, "protocol_version": 1, "ready": True}

        # Everything else (text + the telemetry frame's raw bytes) must
        # have survived untouched, for TelemetryFrameParser to decode
        # the telemetry frame from and the text lines to reach the log
        # path, same as before this parser existed.
        assert b"sim_runner_init" in passthrough
        assert b"seed 1 start" in passthrough
        assert b"seed 1 done" in passthrough
        assert encode_telemetry_frame(run_id=1, cycle=10, team_id=2, event_type=3) in passthrough


def test_parser_resyncs_past_corrupted_checksum():
    stream = b"".join([
        b"before\n",
        encode_reply(board_id=0, protocol_version=1, ready=True, corrupt_checksum=True),
        b"after\n",
        encode_reply(board_id=1, protocol_version=1, ready=True),
    ])

    parser = HandshakeReplyParser()
    events = parser.feed(stream)
    events += parser.feed(b"")

    replies = [e[1] for e in events if e[0] == "reply"]
    passthrough = b"".join(e[1] for e in events if e[0] == "passthrough")

    # Only the well-formed board_id=1 reply should decode; the
    # corrupted board_id=0 one must be rejected and its bytes folded
    # into passthrough instead (dropped one byte at a time, same as
    # every other resync-on-checksum-mismatch parser in this project).
    assert len(replies) == 1
    assert replies[0] == {"board_id": 1, "protocol_version": 1, "ready": True}
    assert b"before" in passthrough
    assert b"after" in passthrough


def test_all_ready_only_after_both_out_of_order():
    """THE acceptance test: fakes both handshake replies arriving out
    of order and confirms all_ready only flips true after both are in,
    not after the first - for both possible arrival orders."""
    for order in ([1, 0], [0, 1]):
        coord = HandshakeCoordinator()
        coord.start()

        assert coord.all_ready is False
        assert coord.can_proceed is False

        first_board = order[0]
        coord.note_reply(first_board, MISSION_LINK_PROTOCOL_VERSION, ready=True)

        # After only the FIRST reply (whichever board it is), all_ready
        # must still be False - this is the exact case a naive
        # "ready = any reply seen" implementation would get wrong.
        assert coord.all_ready is False, (
            f"all_ready flipped true after only board {first_board} replied "
            f"(order={order}) - must wait for both"
        )
        assert coord.can_proceed is False

        second_board = order[1]
        coord.note_reply(second_board, MISSION_LINK_PROTOCOL_VERSION, ready=True)

        assert coord.all_ready is True, (
            f"all_ready did not flip true after both boards replied (order={order})"
        )
        assert coord.can_proceed is True


def test_feed_drives_coordinator_out_of_order():
    """Same out-of-order proof as above, but through the real byte
    stream path (HandshakeCoordinator.feed()) instead of the direct
    note_reply() shortcut, so the codec and the aggregation logic are
    proven together, not just each in isolation."""
    coord = HandshakeCoordinator()
    coord.start()

    leftover1 = coord.feed(encode_reply(board_id=1, protocol_version=1, ready=True))
    assert leftover1 == b""
    assert coord.board_ready(1) is True
    assert coord.board0_ready is False
    assert coord.all_ready is False

    leftover2 = coord.feed(encode_reply(board_id=0, protocol_version=1, ready=True))
    assert leftover2 == b""
    assert coord.board0_ready is True
    assert coord.all_ready is True


def test_dk_only_synthetic_reply_does_not_flip_all_ready():
    coord = HandshakeCoordinator()
    coord.start()
    coord.note_reply(0, MISSION_LINK_PROTOCOL_VERSION, ready=True)

    # DK's synthetic "peer presumed absent" reply after its own
    # timeout - see core/mission_link_handshake.c's timeout policy.
    coord.note_reply(1, MISSION_LINK_PROTOCOL_VERSION, ready=False)

    assert coord.board_ready(1) is False
    assert coord.board_timed_out(1) is True
    assert coord.all_ready is False
    assert coord.can_proceed is False, "must not silently proceed DK-only without an explicit override"

    overridden = coord.request_dk_only_override()
    assert overridden is True
    assert coord.can_proceed is True
    assert coord.all_ready is False  # override lets traffic proceed; it does not pretend the peer is ready


def test_multi_peer_independence():
    """Proves the generalization itself, not just that a single peer
    still works through the renamed methods (the two tests above
    already prove that much, against NUM_BOARDS' default of 2). With
    NUM_BOARDS bumped to 3, peer 2 replying must not affect peer 1's
    own state, request_dk_only_override() must refuse to fire while
    ANY not-yet-timed-out peer is still pending (not just the one
    that happens to be checked first), and status_dict()'s per-board
    dicts must reflect each peer independently - the real risk a
    naive generalization (e.g. one shared flag instead of a per-peer
    dict) would introduce silently."""
    coord = HandshakeCoordinator()
    coord.NUM_BOARDS = 3
    coord.start()

    coord.note_reply(0, MISSION_LINK_PROTOCOL_VERSION, ready=True)
    coord.note_reply(1, MISSION_LINK_PROTOCOL_VERSION, ready=True)
    # peer 2 has not replied at all yet.

    assert coord.board_ready(1) is True
    assert coord.board_ready(2) is False
    assert coord.all_ready is False, "peer 2 hasn't replied - all_ready must not fire early"
    assert coord.can_proceed is False

    # Peer 2 hasn't timed out yet either (no time has meaningfully
    # elapsed) - the override must refuse, precisely because peer 2
    # might still reply in time and shouldn't be silently skipped.
    overridden = coord.request_dk_only_override()
    assert overridden is False
    assert coord.can_proceed is False

    status = coord.status_dict()
    assert status["peer_ready"] == {"1": True, "2": False}
    assert status["board0_ready"] is True
    assert status["all_ready"] is False

    # Now peer 2 replies too - all_ready must flip, unaffected by
    # peer 1 having replied first or the earlier refused override
    # attempt.
    coord.note_reply(2, MISSION_LINK_PROTOCOL_VERSION, ready=True)
    assert coord.all_ready is True
    assert coord.can_proceed is True


def test_dk_only_override_rejected_before_timeout():
    coord = HandshakeCoordinator()
    coord.start()
    coord.note_reply(0, MISSION_LINK_PROTOCOL_VERSION, ready=True)

    # No board_id=1 signal at all yet, and PEER_TIMEOUT_SEC hasn't
    # elapsed - the override must be a no-op here, not a way to skip
    # the check early.
    overridden = coord.request_dk_only_override()
    assert overridden is False
    assert coord.can_proceed is False


def test_due_for_req_timing():
    coord = HandshakeCoordinator()
    coord.REQ_RETRY_INTERVAL_SEC = 0.05  # speed up the test

    assert coord.due_for_req() is False, "must not be due before start()"

    coord.start()
    assert coord.due_for_req() is True, "must be due immediately after start()"

    coord.note_req_sent()
    assert coord.due_for_req() is False, "must not be due again immediately after sending"

    time.sleep(0.08)
    assert coord.due_for_req() is True, "must be due again once the retry interval elapses"

    coord.note_reply(0, MISSION_LINK_PROTOCOL_VERSION, ready=True)
    assert coord.due_for_req() is False, "must stop retrying once board 0 has replied ready"


def main():
    tests = [
        test_pack_req,
        test_parser_round_trip_mixed_stream,
        test_parser_resyncs_past_corrupted_checksum,
        test_all_ready_only_after_both_out_of_order,
        test_feed_drives_coordinator_out_of_order,
        test_dk_only_synthetic_reply_does_not_flip_all_ready,
        test_multi_peer_independence,
        test_dk_only_override_rejected_before_timeout,
        test_due_for_req_timing,
    ]

    for test in tests:
        test()
        print(f"PASS: {test.__name__}")

    print("\nALL CHECKS PASSED: HandshakeCoordinator.all_ready only flips "
          "true once both board_id 0 and board_id 1 have replied ready, "
          "regardless of arrival order.")


if __name__ == "__main__":
    main()

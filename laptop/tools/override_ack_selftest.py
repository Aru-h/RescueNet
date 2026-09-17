#!/usr/bin/env python3
"""
Self-test for override acknowledgements on the laptop side (no hardware):
telemetry_bridge.OverrideAckParser, OverrideTracker, the full USART1
scanner chain (TelemetryBridge.ingest_bytes), and - when Flask is
installed - app.py's /api/board/override + /api/board/override/status.

Run directly: `python3 override_ack_selftest.py` (from laptop/tools).
"""

import sys

from telemetry_bridge import (
    OVERRIDE_ACK_APPLIED,
    OVERRIDE_ACK_BAD_TEAM,
    OVERRIDE_ACK_NOT_READY,
    OVERRIDE_ACK_RELAY_FAILED,
    OVERRIDE_ACK_RELAYED,
    HANDSHAKE_MAGIC,
    OverrideAckParser,
    OverrideTracker,
    TelemetryBridge,
    TELEMETRY_FRAME_MAGIC,
    handshake_checksum,
    pack_override_ack,
    telemetry_frame_checksum,
)

FAILURES = []


def check(cond, what):
    if not cond:
        FAILURES.append(what)
        print(f"FAIL: {what}")


# Produced by core/mission_link_override.c's mission_link_override_pack_ack()
# for {team_id=6, incident_id=0x12346BB6, result=APPLIED}. Its payload
# contains 0xB6 0x6B - the handshake magic pair - on purpose.
C_GOLDEN_ACK = bytes.fromhex("966906b66b3412016e")


def acks_from(events):
    return [p for kind, p in events if kind == "ack"]


def passthrough_from(events):
    return b"".join(p for kind, p in events if kind == "passthrough")


def test_wire_format_matches_c():
    check(pack_override_ack(6, 0x12346BB6, OVERRIDE_ACK_APPLIED) == C_GOLDEN_ACK,
          "Python pack_override_ack() matches the C packer byte for byte")
    events = OverrideAckParser().feed(C_GOLDEN_ACK)
    check(acks_from(events) == [{"team_id": 6, "incident_id": 0x12346BB6,
                                 "result": OVERRIDE_ACK_APPLIED}],
          "C-packed ack parses")


def test_parser_resync_and_split():
    good = pack_override_ack(2, 4343, OVERRIDE_ACK_APPLIED)
    bad = bytearray(pack_override_ack(3, 99, OVERRIDE_ACK_NOT_READY))
    bad[-1] ^= 0xFF
    stream = b"tron: text\n" + bytes(bad) + b"more text\n" + good

    events = OverrideAckParser().feed(stream)
    check(acks_from(events) == [{"team_id": 2, "incident_id": 4343, "result": OVERRIDE_ACK_APPLIED}],
          "corrupted ack rejected, following ack still found")
    check(passthrough_from(events) == b"tron: text\n" + bytes(bad) + b"more text\n",
          "every non-ack byte passed through unchanged, including the corrupted frame")

    parser = OverrideAckParser()
    events = []
    for i in range(len(stream)):
        events += parser.feed(stream[i:i + 1])
    check(len(acks_from(events)) == 1 and passthrough_from(events) == passthrough_from(
        OverrideAckParser().feed(stream)),
          "same result when fed one byte at a time")


def test_tracker():
    t = OverrideTracker(timeout_sec=10.0)
    t.note_sent(6, 4242, now=100.0)
    check(t.get(6, now=100.0)["state"] == "sent", "tracker: sent")
    check(t.note_ack(6, 4242, OVERRIDE_ACK_RELAYED, now=100.1), "tracker: relayed accepted")
    check(t.get(6, now=100.1)["state"] == "relayed", "tracker: relayed")
    check(t.note_ack(6, 4242, OVERRIDE_ACK_APPLIED, now=101.0), "tracker: applied accepted")
    check(t.get(6, now=200.0)["state"] == "applied", "tracker: applied is final and never expires")
    check(not t.note_ack(6, 4242, OVERRIDE_ACK_NOT_READY, now=201.0),
          "tracker: a final state is not overwritten")

    t.note_sent(2, 1, now=100.0)
    t.note_sent(2, 2, now=101.0)  # replaces the first override for team 2
    check(not t.note_ack(2, 1, OVERRIDE_ACK_APPLIED, now=101.5),
          "tracker: late ack for a replaced override ignored")
    check(t.get(2, now=101.5)["state"] == "sent", "tracker: replacement still pending")

    for result, state in ((OVERRIDE_ACK_NOT_READY, "rejected_not_ready"),
                          (OVERRIDE_ACK_BAD_TEAM, "rejected_bad_team"),
                          (OVERRIDE_ACK_RELAY_FAILED, "relay_failed")):
        t.note_sent(9, 7, now=0.0)
        t.note_ack(9, 7, result, now=0.1)
        check(t.get(9, now=0.1)["state"] == state, f"tracker: result {result} -> {state}")

    t.note_sent(4, 5, now=0.0)
    check(not t.note_ack(4, 5, 99, now=0.1), "tracker: unknown result code ignored")
    check(t.get(4, now=9.9)["state"] == "sent", "tracker: pending before timeout")
    check(t.get(4, now=10.1)["state"] == "unconfirmed", "tracker: unconfirmed after timeout")

    t.note_sent(5, 5, now=0.0)
    t.note_ack(5, 5, OVERRIDE_ACK_RELAYED, now=0.5)
    check(t.get(5, now=10.1)["state"] == "unconfirmed",
          "tracker: relayed but never applied -> unconfirmed")
    check(not t.note_ack(5, 5, OVERRIDE_ACK_APPLIED, now=11.0),
          "tracker: ack after it was declared unconfirmed is ignored")


def handshake_reply(board_id, ready):
    payload = bytes((board_id, 1, 1 if ready else 0))
    return HANDSHAKE_MAGIC + payload + bytes((handshake_checksum(payload),))


def telemetry_frame(run_id, cycle, team_id, event_type):
    import struct
    payload = struct.pack("<IIBB", run_id, cycle, team_id, event_type)
    return TELEMETRY_FRAME_MAGIC + payload + bytes((telemetry_frame_checksum(payload),))


def test_full_chain(bridge_cls, label):
    bridge = bridge_cls()
    text_lines = []
    bridge._handle_text_line = text_lines.append
    bridge.overrides.note_sent(6, 0x12346BB6)
    stream = (b"tron: hello\n" + handshake_reply(0, True) + C_GOLDEN_ACK +
              telemetry_frame(1, 10, 2, 3) + b"tron: bye\n")
    for i in range(0, len(stream), 3):
        bridge.ingest_bytes(stream[i:i + 3])

    check(bridge.overrides.get(6)["state"] == "applied",
          f"{label}: ack with the handshake magic inside its payload still reaches the tracker")
    check(bridge.handshake.replies.get(0, {}).get("ready") is True,
          f"{label}: handshake reply still decoded")
    check(bridge.telemetry_queue.qsize() == 1, f"{label}: telemetry frame still decoded")
    check(text_lines == ["tron: hello", "tron: bye"], f"{label}: debug text lines intact")


def test_app_routes():
    try:
        import app as app_module
    except ImportError as exc:
        print(f"SKIP: app.py route checks ({exc})")
        return

    if not hasattr(app_module.bridge, "ingest_bytes"):
        print("SKIP: app.py route checks (sim-only bridge)")
        return

    test_full_chain(app_module.TelemetryBridge, "app.py bridge")

    written = []

    class FakeSerial:
        is_open = True

        def write(self, data):
            written.append(bytes(data))

    class ReadyHandshake:
        """Stands in for a completed handshake; passes bytes through like
        HandshakeCoordinator.feed() does when they hold no Reply."""
        can_proceed = True

        def status_dict(self):
            return {}

        def feed(self, data):
            return data

    bridge = app_module.bridge
    bridge.serial_conn = FakeSerial()
    bridge.handshake = ReadyHandshake()
    client = app_module.app.test_client()

    check(client.get("/api/board/override/status?team_id=6").status_code == 404,
          "app: no status before any override")
    resp = client.post("/api/board/override", json={"team_id": 6, "incident_id": 4242})
    check(resp.status_code == 200 and resp.get_json()["override"]["state"] == "sent",
          "app: POST records the override as sent")
    check(len(written) == 1 and written[0][:2] == bytes((0x87, 0x78)),
          "app: override frame written to the serial port")

    bridge.ingest_bytes(pack_override_ack(6, 4242, OVERRIDE_ACK_RELAYED))
    check(client.get("/api/board/override/status?team_id=6").get_json()["override"]["state"]
          == "relayed", "app: status shows relayed")
    bridge.ingest_bytes(pack_override_ack(6, 4242, OVERRIDE_ACK_APPLIED))
    body = client.get("/api/board/override/status?team_id=6").get_json()
    check(body["override"]["state"] == "applied", "app: status shows applied")
    check(client.get("/api/board/override/status").get_json()["overrides"][0]["team_id"] == 6,
          "app: status without team_id lists every tracked override")


def main():
    test_wire_format_matches_c()
    test_parser_resync_and_split()
    test_tracker()
    test_full_chain(TelemetryBridge, "telemetry_bridge")
    test_app_routes()

    if FAILURES:
        print(f"override_ack_selftest: {len(FAILURES)} check(s) FAILED")
        return 1
    print("override_ack_selftest: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

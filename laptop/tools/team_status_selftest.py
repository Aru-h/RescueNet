#!/usr/bin/env python3
"""
Self-test for team status reports on the laptop side (no hardware):
telemetry_bridge.MissionLinkStatusParser, TeamStatusTracker, the full
USART1 scanner chain (TelemetryBridge.ingest_bytes) with every other frame
type interleaved, and - when Flask is installed - app.py's /api/status.

Run directly: `python3 team_status_selftest.py` (from laptop/tools).
"""

import struct
import sys

from telemetry_bridge import (
    HANDSHAKE_MAGIC,
    MISSION_LINK_NO_INCIDENT,
    OVERRIDE_ACK_APPLIED,
    TELEMETRY_FRAME_MAGIC,
    TEAM_STATUS_STALE_SEC,
    MissionLinkStatusParser,
    TeamStatusTracker,
    TelemetryBridge,
    handshake_checksum,
    pack_mission_link_status,
    pack_override_ack,
    telemetry_frame_checksum,
)

FAILURES = []


def check(cond, what):
    if not cond:
        FAILURES.append(what)
        print(f"FAIL: {what}")


# Produced by core/mission_link.c's mission_link_pack_status() for
# {team 3, ON_SCENE, equipment 77, incident 0x12F99F34, work 0x9FF9, rate 5,
#  stamina 42, tick 0x00ABCDEF}. work_remaining encodes as F9 9F - the status
# magic pair itself - on purpose.
C_GOLDEN_STATUS = bytes.fromhex("f99f03024d349ff912f99f052aefcdab005e")
C_GOLDEN_STATUS_FIELDS = {
    "team_id": 3, "status": 2, "equipment_level": 77,
    "assigned_incident_id": 0x12F99F34, "work_remaining_at_report": 0x9FF9,
    "work_rate": 5, "stamina_at_report": 42, "report_tick": 0x00ABCDEF,
}
# {team 1, AVAILABLE, equipment 100, MISSION_LINK_NO_INCIDENT, 0, 0, 100, tick 7}
C_GOLDEN_IDLE = bytes.fromhex("f99f010064ffffffff0000006407000000cc")


def statuses_from(events):
    return [p for kind, p in events if kind == "status"]


def passthrough_from(events):
    return b"".join(p for kind, p in events if kind == "passthrough")


def test_wire_format_matches_c():
    check(pack_mission_link_status(3, 2, 77, 0x12F99F34, 0x9FF9, 5, 42, 0x00ABCDEF)
          == C_GOLDEN_STATUS, "Python packer matches the C packer byte for byte")
    check(pack_mission_link_status(1, 0, 100, MISSION_LINK_NO_INCIDENT, 0, 0, 100, 7)
          == C_GOLDEN_IDLE, "Python packer matches C for an idle team")
    events = MissionLinkStatusParser().feed(C_GOLDEN_STATUS + C_GOLDEN_IDLE)
    got = statuses_from(events)
    check(len(got) == 2 and got[0] == C_GOLDEN_STATUS_FIELDS and got[1]["team_id"] == 1,
          "C-packed status frames parse")
    check(passthrough_from(events) == b"", "nothing passed through for two clean frames")


def test_parser_resync_and_split():
    bad = bytearray(C_GOLDEN_IDLE)
    bad[-1] ^= 0xFF
    stream = b"tron: text\n" + bytes(bad) + b"more text\n" + C_GOLDEN_STATUS

    events = MissionLinkStatusParser().feed(stream)
    check(statuses_from(events) == [C_GOLDEN_STATUS_FIELDS],
          "corrupted frame rejected, following frame still found")
    check(passthrough_from(events) == b"tron: text\n" + bytes(bad) + b"more text\n",
          "every non-status byte passed through unchanged")

    parser = MissionLinkStatusParser()
    events = []
    for i in range(len(stream)):
        events += parser.feed(stream[i:i + 1])
    check(statuses_from(events) == [C_GOLDEN_STATUS_FIELDS] and
          passthrough_from(events) == passthrough_from(MissionLinkStatusParser().feed(stream)),
          "same result when fed one byte at a time")


def test_tracker():
    t = TeamStatusTracker()
    check(t.all() == [] and t.get(3) is None, "tracker: empty")

    t.note(dict(C_GOLDEN_STATUS_FIELDS), now=100.0)
    e = t.get(3, now=101.0)
    check(e["status_name"] == "ON_SCENE" and e["assigned"] and not e["stale"]
          and abs(e["age_sec"] - 1.0) < 1e-9, "tracker: fresh report")
    check(t.get(3, now=100.0 + TEAM_STATUS_STALE_SEC + 0.1)["stale"],
          "tracker: stale after several missed heartbeats")

    newer = dict(C_GOLDEN_STATUS_FIELDS, status=3, report_tick=0x00ABCE00)
    t.note(newer, now=110.0)
    e = t.get(3, now=110.0)
    check(e["status_name"] == "RESTING" and not e["stale"], "tracker: last report wins, refreshes age")

    t.note({"team_id": 1, "status": 0, "equipment_level": 100,
            "assigned_incident_id": MISSION_LINK_NO_INCIDENT, "work_remaining_at_report": 0,
            "work_rate": 0, "stamina_at_report": 100, "report_tick": 7}, now=110.0)
    check([x["team_id"] for x in t.all(now=110.0)] == [1, 3], "tracker: all() sorted by team")
    check(not t.get(1, now=110.0)["assigned"], "tracker: NO_INCIDENT -> assigned False")

    t.note(dict(C_GOLDEN_STATUS_FIELDS, team_id=9, status=200), now=0.0)
    check(t.get(9, now=0.0)["status_name"] is None, "tracker: unknown status code has no name")


def handshake_reply(board_id, ready):
    payload = bytes((board_id, 1, 1 if ready else 0))
    return HANDSHAKE_MAGIC + payload + bytes((handshake_checksum(payload),))


def telemetry_frame(run_id, cycle, team_id, event_type):
    payload = struct.pack("<IIBB", run_id, cycle, team_id, event_type)
    return TELEMETRY_FRAME_MAGIC + payload + bytes((telemetry_frame_checksum(payload),))


def test_full_chain():
    bridge = TelemetryBridge()
    text_lines = []
    bridge._handle_text_line = text_lines.append
    bridge.overrides.note_sent(6, 0x12346BB6)

    # A telemetry frame whose cycle count carries the status magic, and a
    # status frame whose incident id carries the telemetry magic: neither
    # scanner may damage the other's frame.
    tele_with_status_magic = telemetry_frame(1, 0x00009FF9, 2, 3)
    status_with_tele_magic = pack_mission_link_status(4, 1, 90, 0x00005AA5, 10, 2, 80, 55)
    stream = (b"tron: hello\n" + handshake_reply(0, True) + C_GOLDEN_STATUS +
              bytes.fromhex("966906b66b3412016e") + tele_with_status_magic +
              status_with_tele_magic + C_GOLDEN_IDLE + b"tron: bye\n")
    for i in range(0, len(stream), 5):
        bridge.ingest_bytes(stream[i:i + 5])

    reports = bridge.team_status.all()
    check([r["team_id"] for r in reports] == [1, 3, 4], "chain: all three status reports tracked")
    check(bridge.team_status.get(3)["work_remaining_at_report"] == 0x9FF9,
          "chain: status frame containing its own magic decoded intact")
    check(bridge.team_status.get(4)["assigned_incident_id"] == 0x00005AA5,
          "chain: status frame containing the telemetry magic decoded intact")
    check(bridge.telemetry_queue.qsize() == 1 and
          bridge.telemetry_queue.get()["cycle"] == 0x00009FF9,
          "chain: telemetry frame containing the status magic decoded intact")
    check(bridge.overrides.get(6)["state"] == "applied", "chain: override ack still decoded")
    check(bridge.handshake.replies.get(0, {}).get("ready") is True, "chain: handshake still decoded")
    check(text_lines == ["tron: hello", "tron: bye"], "chain: debug text lines intact")
    check(pack_override_ack(6, 0x12346BB6, OVERRIDE_ACK_APPLIED) ==
          bytes.fromhex("966906b66b3412016e"), "chain: ack vector is the C golden one")


def test_bridge_lifecycle():
    bridge = TelemetryBridge()
    check(bridge.mission_started is False, "bridge: mission_started starts False")
    bridge.nodes_ready.add("x")
    bridge.mission_started = True
    old_handshake = bridge.handshake
    bridge.disconnect()
    check(bridge.serial_conn is None and not bridge.nodes_ready and
          bridge.mission_started is False and bridge.handshake is not old_handshake,
          "bridge: disconnect resets connection state")


def test_app_status_route():
    try:
        import app as app_module
    except ImportError as exc:
        print(f"SKIP: app.py route checks ({exc})")
        return

    if not isinstance(app_module.bridge, TelemetryBridge):
        print("SKIP: app.py route checks (sim-only bridge)")
        return

    check(app_module.TelemetryBridge is TelemetryBridge,
          "app: uses telemetry_bridge.TelemetryBridge, not a copy")

    client = app_module.app.test_client()
    body = client.get("/api/status").get_json()
    check(body["board"]["team_status"] == [], "app: no team status before any report")

    app_module.bridge.ingest_bytes(C_GOLDEN_STATUS)
    body = client.get("/api/status").get_json()
    reports = body["board"]["team_status"]
    check(len(reports) == 1 and reports[0]["team_id"] == 3 and
          reports[0]["status_name"] == "ON_SCENE" and reports[0]["stale"] is False,
          "app: /api/status reports the decoded team status")


def main():
    test_wire_format_matches_c()
    test_parser_resync_and_split()
    test_tracker()
    test_full_chain()
    test_bridge_lifecycle()
    test_app_status_route()

    if FAILURES:
        print(f"team_status_selftest: {len(FAILURES)} check(s) FAILED")
        return 1
    print("team_status_selftest: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
Self-test for the board mission on the laptop side (no hardware):
telemetry_bridge.MissionFeed's parsing, per-key replacement, rate limit and
re-send schedule (clock included), driven with an explicit clock; report
forwarding and live-frame capture; then a real `tron --mission-feed` run
(when the binary is built) through the bridge - frames out to a fake serial
port, a board report back in, and the report showing up in the simulator's
LIVEFRAME - and app.py's /api/board/launch, stream and stop routes.

Run directly: `python3 mission_feed_selftest.py` (from laptop/tools).
"""

import struct
import sys
import threading
import time
from pathlib import Path

from telemetry_bridge import (
    HANDSHAKE_MAGIC,
    INCIDENT_STATUS_RESOLVED,
    MISSION_LINK_CLOCK_MAGIC,
    MISSION_LINK_FAULT_MAGIC,
    MISSION_LINK_INCIDENT_MAGIC,
    MISSION_LINK_MAP_ROW_MAGIC,
    MISSION_LINK_NO_INCIDENT,
    MISSION_LINK_PROTOCOL_VERSION,
    MISSION_LINK_SITES_MAGIC,
    MISSION_LINK_ROSTER_MAGIC,
    MissionFeed,
    TelemetryBridge,
    handshake_checksum,
    mission_frame_key,
    pack_mission_link_status,
    tick_delay_ms_for_time_scale,
)

FAILURES = []
_STATUS_OFFSET = 2 + 13  # incident frame: magic + id, loc, work, priority, type, req
BINARY = Path(__file__).resolve().parent.parent.parent / "tron"


def check(cond, what):
    if not cond:
        FAILURES.append(what)
        print(f"FAIL: {what}")


def frame(magic, payload):
    return magic + payload + bytes((sum(payload) & 0xFF,))


def roster(team, x=3, y=4):
    return frame(MISSION_LINK_ROSTER_MAGIC, struct.pack("<BBBBhhBB", team, 1, 2, 0, x, y, 90, 100))


def incident(incident_id, status=0, priority=1):
    return frame(MISSION_LINK_INCIDENT_MAGIC,
                 struct.pack("<IhhHBBBBBB", incident_id, 5, 6, 40, priority, 2, 1, status, 3, 1))


def fault(team, severity=7):
    # team_id, active[5], severity[5] (NUM_OPERATIONAL_FAULT_CLASSES = 5)
    return frame(MISSION_LINK_FAULT_MAGIC, bytes((team, 1, 0, 0, 0, 0, severity, 0, 0, 0, 0)))


def clock(tick):
    return frame(MISSION_LINK_CLOCK_MAGIC, struct.pack("<I", tick))


def map_row(y, fill=1):
    return frame(MISSION_LINK_MAP_ROW_MAGIC, bytes((y,)) + bytes((fill,)) * 36)


def sites():
    return frame(MISSION_LINK_SITES_MAGIC, bytes(33))


def line(tick, data):
    return f"MLFRAME {tick} {data.hex()}\n"


def test_frame_keys():
    check(mission_frame_key(roster(7)) == ("roster", 7), "roster key")
    check(mission_frame_key(incident(0x01020304)) == ("incident", 0x01020304), "incident key")
    check(mission_frame_key(fault(3)) == ("fault", 3), "fault key")
    check(mission_frame_key(clock(99)) == ("clock", 0), "clock key")
    check(mission_frame_key(map_row(17)) == ("map_row", 17), "map row key")
    check(mission_frame_key(sites()) == ("sites", 0), "sites key")
    bad = bytearray(roster(7))
    bad[-1] ^= 1
    check(mission_frame_key(bytes(bad)) is None, "bad checksum rejected")
    check(mission_frame_key(roster(7)[:-1]) is None, "short frame rejected")
    check(mission_frame_key(b"\x00\x01" + roster(7)[2:]) is None, "unknown magic rejected")


class Writer:
    def __init__(self):
        self.frames = []

    def __call__(self, data):
        self.frames.append(bytes(data))


def test_replacement_and_rate():
    w = Writer()
    feed = MissionFeed(w, clock=lambda: 0.0)
    check(not feed.feed_line("tron: some other stdout line"), "non-MLFRAME lines ignored")
    check(not feed.feed_line("MLFRAME 1 zz"), "bad hex ignored")
    for team in range(3):
        feed.feed_line(line(0, roster(team)))
    feed.feed_line(line(1, incident(9, priority=1)))
    feed.feed_line(line(2, incident(9, priority=3)))  # replaces the pending one
    check(feed.pending_count() == 4, "newer incident frame replaced the pending one")

    t = 100.0
    sent_at = []
    while feed.pending_count() and t < 110.0:
        if feed.pump(now=t):
            sent_at.append(t)
        t += 0.01
    check([mission_frame_key(f) for f in w.frames] ==
          [("roster", 0), ("roster", 1), ("roster", 2), ("incident", 9)],
          "sent in arrival order, one frame per key")
    check(w.frames[-1] == incident(9, priority=3), "only the newest incident state was sent")
    gaps = [b - a for a, b in zip(sent_at, sent_at[1:])]
    check(all(g >= 1.0 / MissionFeed.MAX_FRAMES_PER_SEC - 1e-9 for g in gaps),
          f"send rate capped at {MissionFeed.MAX_FRAMES_PER_SEC}/s (gaps {gaps})")


def test_time_scale():
    check(tick_delay_ms_for_time_scale(1) == 36000, "real time is 36 s per tick")
    check(tick_delay_ms_for_time_scale(10) == 3600, "10x is 3.6 s per tick")
    try:
        tick_delay_ms_for_time_scale(0)
        check(False, "time scale 0 rejected")
    except ValueError:
        pass


def test_clock_replacement_and_resend():
    w = Writer()
    feed = MissionFeed(w)
    feed.feed_line(line(1, clock(1)))
    feed.feed_line(line(2, clock(2)))  # replaces the pending tick 1
    check(feed.pending_count() == 1, "newer clock replaced the pending one")
    now = 0.0
    while feed.pending_count():
        feed.pump(now=now)
        now += 0.2
    check(w.frames == [clock(2)], "only the latest clock tick was sent")

    w.frames.clear()
    end = now + MissionFeed.CLOCK_RESEND_SEC + 0.5
    while now < end:
        feed.pump(now=now)
        now += 0.05
    check(w.frames == [clock(2)], f"clock re-sent once in {MissionFeed.CLOCK_RESEND_SEC}s ({len(w.frames)})")


class FakeStdin:
    def __init__(self):
        self.lines = []

    def write(self, text):
        self.lines.append(text)

    def flush(self):
        pass


class FakeProc:
    def __init__(self):
        self.stdin = FakeStdin()

    def poll(self):
        return None


def test_map_and_sites_resend():
    w = Writer()
    feed = MissionFeed(w)
    for y in range(3):
        feed.feed_line(line(0, map_row(y)))
    feed.feed_line(line(0, sites()))
    feed.feed_line(line(4, map_row(1, fill=5)))  # row 1 changed before it went out
    now = 0.0
    while feed.pending_count():
        feed.pump(now=now)
        now += 0.2
    check([mission_frame_key(f) for f in w.frames] == [("map_row", 0), ("map_row", 1), ("map_row", 2), ("sites", 0)],
          "map rows and sites sent once each")
    check(map_row(1, fill=5) in w.frames and map_row(1) not in w.frames, "only the newest row 1 was sent")
    check(feed.status_dict()["map_rows"] == 3, "status counts map rows")

    w.frames.clear()
    end = now + MissionFeed.MAP_RESEND_SEC + 0.5
    while now < end:
        feed.pump(now=now)
        now += 0.05
    keys = [mission_frame_key(f) for f in w.frames]
    check(sorted(k[1] for k in keys if k[0] == "map_row") == [0, 1, 2],
          f"every map row re-sent in {MissionFeed.MAP_RESEND_SEC}s")
    check(keys.count(("sites", 0)) == int(MissionFeed.MAP_RESEND_SEC // MissionFeed.ROSTER_RESEND_SEC),
          "sites re-sent with the rosters")
    check(map_row(1, fill=5) in w.frames, "re-send carries the newest row")


def test_status_forwarding_and_live_capture():
    feed = MissionFeed(Writer())
    report = {"team_id": 7, "status": 1, "equipment_level": 80,
              "assigned_incident_id": MISSION_LINK_NO_INCIDENT, "work_remaining_at_report": 0,
              "work_rate": 0, "stamina_at_report": 55, "report_tick": 12, "loc_x": 30, "loc_y": 2}
    check(not feed.feed_status(report), "no simulator: report not forwarded")
    feed._proc = FakeProc()
    check(feed.feed_status(report), "report forwarded")
    expected = pack_mission_link_status(7, 1, 80, MISSION_LINK_NO_INCIDENT, 0, 0, 55, 12, 30, 2)
    check(feed._proc.stdin.lines == [f"MLSTATUS {expected.hex()}\n"], "forwarded as one MLSTATUS line")

    check(feed.feed_line('STATICENV {"buildings": []}\n'), "static env captured")
    for tick in range(MissionFeed.LIVE_FRAMES_KEPT + 5):
        feed.feed_line(f'LIVEFRAME {{"tick": {tick}}}\n')
    check(not feed.feed_line("LIVEFRAME {not json"), "bad live frame ignored")
    env, frames = feed.live_events(0)
    check(env == {"buildings": []}, "static env returned")
    check(len(frames) == MissionFeed.LIVE_FRAMES_KEPT and frames[-1][1]["tick"] == MissionFeed.LIVE_FRAMES_KEPT + 4,
          "only the newest live frames kept")
    _, newer = feed.live_events(frames[-2][0])
    check([f["tick"] for _, f in newer] == [MissionFeed.LIVE_FRAMES_KEPT + 4], "live_events(after) returns only newer")
    check(feed.frames_parsed == 0, "live lines are not board frames")


def test_resends():
    w = Writer()
    feed = MissionFeed(w)
    feed.feed_line(line(0, roster(1)))
    feed.feed_line(line(0, roster(6)))
    feed.feed_line(line(5, incident(11)))
    feed.feed_line(line(6, incident(12)))

    now = 0.0
    while feed.pump(now=now) or feed.pending_count():
        now += 0.2
    first_batch = len(w.frames)
    check(first_batch == 4, "initial frames all sent")

    # Nothing new until a re-send is due.
    w.frames.clear()
    end = now + MissionFeed.ROSTER_RESEND_SEC + 0.5
    while now < end:
        feed.pump(now=now)
        now += 0.05
    rosters = [f for f in w.frames if mission_frame_key(f)[0] == "roster"]
    incidents = [mission_frame_key(f)[1] for f in w.frames if mission_frame_key(f)[0] == "incident"]
    check(len(rosters) == 2, f"every roster re-sent once in {MissionFeed.ROSTER_RESEND_SEC}s ({len(rosters)})")
    check(set(incidents) == {11, 12}, f"open incidents refreshed round-robin ({incidents})")

    # A resolved incident is sent once more and then never refreshed.
    feed.feed_line(line(9, incident(11, status=INCIDENT_STATUS_RESOLVED)))
    w.frames.clear()
    end = now + 3 * MissionFeed.INCIDENT_REFRESH_SEC + 0.5
    while now < end:
        feed.pump(now=now)
        now += 0.05
    keyed = [(mission_frame_key(f), f) for f in w.frames]
    resolved_sends = [f for k, f in keyed if k == ("incident", 11)]
    check(len(resolved_sends) == 1 and resolved_sends[0] == incident(11, status=INCIDENT_STATUS_RESOLVED),
          "resolved incident sent once, not refreshed")
    check(any(k == ("incident", 12) for k, _ in keyed), "other open incident still refreshed")
    check(feed.status_dict()["open_incidents"] == 1, "status counts open incidents")


def test_force_full_resend():
    """force_full_resend() is the reboot-recovery path this session split
    off from the periodic ROSTER_RESEND_SEC/MAP_RESEND_SEC backstops (see
    that method's own doc, and MissionFeed's docstring, for why those two
    were safely lengthened once this existed): TelemetryBridge.ingest_bytes
    calls this the instant HandshakeCoordinator reports a board went
    ready again, rather than waiting out a now much longer backstop
    interval - a rebooted board must not wait ROSTER_RESEND_SEC/
    MAP_RESEND_SEC for its roster/map back."""
    w = Writer()
    feed = MissionFeed(w)
    feed.feed_line(line(0, roster(1)))
    feed.feed_line(line(0, roster(6)))
    feed.feed_line(line(0, map_row(0)))
    feed.feed_line(line(0, map_row(1)))
    feed.feed_line(line(0, sites()))

    now = 0.0
    while feed.pump(now=now) or feed.pending_count():
        now += 0.2
    check(len(w.frames) == 5, "initial roster/map/sites frames all sent")

    # Long before either backstop is due - proves this isn't just catching
    # the next scheduled resend.
    check(now < MissionFeed.ROSTER_RESEND_SEC and now < MissionFeed.MAP_RESEND_SEC,
          "sanity: test runs well inside both backstop windows")

    w.frames.clear()
    feed.force_full_resend()
    sent_keys = set()
    while feed.pump(now=now) or feed.pending_count():
        sent_keys.add(mission_frame_key(w.frames[-1]))
        now += 0.2
    check(sent_keys == {("roster", 1), ("roster", 6), ("map_row", 0), ("map_row", 1), ("sites", 0)},
          f"force_full_resend() re-queued every roster, map row and sites frame immediately ({sent_keys})")


def handshake_reply(board_id, ready):
    payload = bytes((board_id, MISSION_LINK_PROTOCOL_VERSION, 1 if ready else 0))
    return HANDSHAKE_MAGIC + payload + bytes((handshake_checksum(payload),))


def test_reboot_edge_triggers_force_full_resend():
    """End-to-end wiring check for the reboot-recovery split introduced
    alongside force_full_resend(): TelemetryBridge.ingest_bytes must call
    mission.force_full_resend() the moment its REAL HandshakeCoordinator
    (not a ReadyHandshake stub - the point is to exercise the actual edge
    detection) reports a board went ready again, and must NOT call it on
    every byte just because a mission is attached."""
    bridge = TelemetryBridge()
    bridge.serial_conn = FakeSerial()

    calls = []

    class SpyMission:
        def force_full_resend(self):
            calls.append(1)

        def feed_status(self, report):
            return False

    bridge.mission = SpyMission()

    bridge.ingest_bytes(b"tron: noise\n")
    check(calls == [], "no handshake activity yet -> no forced resend")

    bridge.ingest_bytes(handshake_reply(0, True))
    check(calls == [1], "board 0's first ready reply triggers one forced resend")

    bridge.ingest_bytes(handshake_reply(0, True))
    check(calls == [1], "a repeat identical reply must not trigger another one")

    # Reboot: not-ready dip, then ready again.
    bridge.ingest_bytes(handshake_reply(0, False))
    check(calls == [1], "going not-ready is never itself a trigger")

    bridge.ingest_bytes(handshake_reply(0, True))
    check(calls == [1, 1], "re-becoming ready after the reboot dip triggers another forced resend")


class FakeSerial:
    is_open = True

    def __init__(self):
        self.written = bytearray()

    def write(self, data):
        self.written.extend(data)


class ReadyHandshake:
    can_proceed = True

    def status_dict(self):
        return {}

    def feed(self, data):
        return data

    def due_for_req(self):
        return False

    def consume_newly_ready(self):
        return []


def frames_in(stream: bytes):
    """Every mission frame found in a byte stream, by key."""
    keys = []
    sizes = {MISSION_LINK_ROSTER_MAGIC: 13, MISSION_LINK_INCIDENT_MAGIC: 19, MISSION_LINK_FAULT_MAGIC: 14,
             MISSION_LINK_CLOCK_MAGIC: 7, MISSION_LINK_MAP_ROW_MAGIC: 40, MISSION_LINK_SITES_MAGIC: 36}
    i = 0
    while i < len(stream) - 1:
        size = sizes.get(bytes(stream[i:i + 2]))
        key = mission_frame_key(bytes(stream[i:i + size])) if size else None
        if key:
            keys.append(key)
            i += size
        else:
            i += 1
    return keys


def test_real_simulator_through_bridge():
    if not BINARY.exists():
        print(f"SKIP: real feed run ({BINARY} not built)")
        return

    bridge = TelemetryBridge()
    bridge.serial_conn = FakeSerial()
    bridge.handshake = ReadyHandshake()
    ok, message = bridge.start_mission(BINARY, seed=3, ticks=400, tick_delay_ms=0,
                                       fault_profile="combined")
    check(ok, f"bridge started the feed ({message})")
    deadline = time.time() + 20
    while time.time() < deadline:
        st = bridge.mission_status()
        if st["input_finished"] and st["frames_pending"] == 0:
            break
        time.sleep(0.05)
    st = bridge.mission_status()
    bridge.stop_mission()

    keys = frames_in(bytes(bridge.serial_conn.written))
    check(st["simulator_exit_code"] == 0, f"simulator exited cleanly ({st})")
    check(st["frames_rejected"] == 0, "every MLFRAME line from the C packer was valid")
    check({k[1] for k in keys if k[0] == "roster"} == set(range(10)),
          "all 10 team rosters reached the serial port")
    check(any(k[0] == "incident" for k in keys), "incident frames reached the serial port")
    check(("clock", 0) in keys, "clock frames reached the serial port")
    check({k[1] for k in keys if k[0] == "map_row"} == set(range(36)), "all 36 map rows reached the serial port")
    check(("sites", 0) in keys, "sites reached the serial port")
    check(len(keys) == st["frames_sent"], "every written frame is intact on the wire")
    check(bridge.mission is None, "stop_mission cleared the feed")


def team_in(frame, team_id):
    return next((t for t in frame.get("teams", []) if t.get("id") == team_id), None)


def test_board_report_reaches_the_picture():
    """A status frame arriving on the serial side moves the team in the
    simulator's own LIVEFRAMEs - the dashboard shows the board, not a
    second simulation."""
    if not BINARY.exists():
        print(f"SKIP: mirror run ({BINARY} not built)")
        return

    bridge = TelemetryBridge()
    bridge.serial_conn = FakeSerial()
    bridge.handshake = ReadyHandshake()
    ok, message = bridge.start_mission(BINARY, seed=5, ticks=100000, tick_delay_ms=40)
    check(ok, f"mirror mission started ({message})")

    deadline = time.time() + 20
    while time.time() < deadline and not bridge.mission.live_events(0)[1]:
        time.sleep(0.05)
    _, frames = bridge.mission.live_events(0)
    check(bool(frames), "simulator produced live frames")
    before = team_in(frames[-1][1], 7) if frames else None

    # Somewhere team 7 is not, reported the way board 0 writes it to USART1.
    target = (3, 31) if not before or (before["x"], before["y"]) != (3, 31) else (4, 30)
    # Trailing text on purpose: this frame's checksum byte is 0x96, the
    # override-ack magic's first byte, which that scanner holds until the
    # next byte arrives (on a real link, the next report or log line).
    bridge.ingest_bytes(b"tron: noise\n" +
                        pack_mission_link_status(7, 1, 70, MISSION_LINK_NO_INCIDENT, 0, 0, 33, 9,
                                                 target[0], target[1]) +
                        b"tron: more\n")
    moved = None
    last_seq = frames[-1][0] if frames else 0
    deadline = time.time() + 20
    while time.time() < deadline and moved is None:
        _, newer = bridge.mission.live_events(last_seq)
        for seq, frame in newer:
            last_seq = seq
            team = team_in(frame, 7)
            if team and (team["x"], team["y"]) == target:
                moved = team
        time.sleep(0.05)
    st = bridge.mission_status()
    bridge.stop_mission()

    check(st["reports_forwarded"] == 1, f"the report was forwarded to the simulator ({st})")
    check(moved is not None, f"team 7 shows at the reported cell {target}")
    check(moved is not None and moved["status"] == 1 and abs(moved["stamina"] - 33) < 0.01,
          f"reported status and stamina shown ({moved})")


def test_report_effect_sent_before_next_tick():
    """A report that changes an incident goes out right away, not after the
    laptop's next tick - otherwise the bridge keeps re-sending the old state
    for up to a whole tick (36 s in a real-time mission). Checked by the
    frame's own tick number: it must carry the tick that was current when
    the report arrived."""
    if not BINARY.exists():
        print(f"SKIP: immediate-send run ({BINARY} not built)")
        return

    parsed = []  # (tick, frame) in arrival order
    lock = threading.Lock()

    bridge = TelemetryBridge()
    bridge.serial_conn = FakeSerial()
    bridge.handshake = ReadyHandshake()
    tick_ms = 600
    ok, message = bridge.start_mission(BINARY, seed=2, ticks=100000, tick_delay_ms=tick_ms)
    check(ok, f"slow mission started ({message})")
    feed = bridge.mission
    original = feed.feed_line

    def recording_feed_line(line):
        parts = line.split()
        if len(parts) == 3 and parts[0] == "MLFRAME":
            try:
                with lock:
                    parsed.append((int(parts[1]), bytes.fromhex(parts[2])))
            except ValueError:
                pass
        return original(line)

    feed.feed_line = recording_feed_line

    # Seed 2's first incident appears at tick 25.
    first = None
    deadline = time.monotonic() + 40 * tick_ms / 1000
    while time.monotonic() < deadline and first is None:
        with lock:
            first = next(((t, f) for t, f in parsed
                          if mission_frame_key(f) and mission_frame_key(f)[0] == "incident"), None)
        time.sleep(0.01)
    check(first is not None, "an incident frame was produced")
    if first is None:
        bridge.stop_mission()
        return

    tick, incident = first
    incident_id = struct.unpack_from("<I", incident, 2)[0]
    with lock:
        seen = len(parsed)
    bridge.ingest_bytes(pack_mission_link_status(0, 2, 90, incident_id, 100, 5, 80, tick, 3, 3) + b"x\n")

    in_progress = None
    deadline = time.monotonic() + 2 * tick_ms / 1000
    while time.monotonic() < deadline and in_progress is None:
        with lock:
            in_progress = next(((t, f) for t, f in parsed[seen:]
                                if mission_frame_key(f) == ("incident", incident_id) and f[_STATUS_OFFSET] == 2),
                               None)
        time.sleep(0.01)
    bridge.stop_mission()

    check(in_progress is not None, "incident went IN_PROGRESS on the wire after the report")
    check(in_progress is not None and in_progress[0] == tick,
          f"...stamped with the same tick, i.e. before the laptop's next tick "
          f"(report at tick {tick}, frame tick {None if in_progress is None else in_progress[0]})")


def test_app_launch_route():
    try:
        import app as app_module
    except ImportError as exc:
        print(f"SKIP: app.py launch route ({exc})")
        return
    if not isinstance(app_module.bridge, TelemetryBridge) or not BINARY.exists():
        print("SKIP: app.py launch route (sim-only bridge or tron not built)")
        return

    bridge = app_module.bridge
    bridge.serial_conn = FakeSerial()
    bridge.handshake = ReadyHandshake()
    app_module.build_binary = lambda: (True, "")  # already built; don't run make here
    client = app_module.app.test_client()

    resp = client.post("/api/board/launch", json={"seed": 2, "ticks": 200, "tick_delay_ms": 20})
    body = resp.get_json()
    check(resp.status_code == 200 and body["status"] == "success", f"launch succeeded ({body})")
    deadline = time.time() + 20
    while time.time() < deadline and len(frames_in(bytes(bridge.serial_conn.written))) < 10:
        time.sleep(0.05)
    check(bytes(bridge.serial_conn.written).startswith(b"START\n"), "START written first")
    status = client.get("/api/status").get_json()["board"]["mission"]
    check(status is not None and status["frames_sent"] >= 10, f"/api/status reports the feed ({status})")
    stream = client.get("/api/board/mission/stream", buffered=False)
    chunks = []
    for chunk in stream.response:
        chunks.append(chunk.decode() if isinstance(chunk, bytes) else chunk)
        if any('"type": "frame"' in c for c in chunks):
            break
    stream.close()
    joined = "".join(chunks)
    check('"type": "static_env"' in joined and '"type": "frame"' in joined,
          "stream route sends static_env then frames")
    check(client.post("/api/board/mission/stop").status_code == 200, "stop route")
    check(bridge.mission is None, "feed stopped")

    check(client.post("/api/board/launch", json={"seed": "x"}).status_code == 400,
          "bad launch parameters rejected")
    check(client.post("/api/board/launch", json={"time_scale": 0}).status_code == 400,
          "non-positive time scale rejected")
    ended = client.get("/api/board/mission/stream").get_data(as_text=True)
    check('"type": "error"' in ended, "stream route without a mission reports an error")


def main():
    test_frame_keys()
    test_time_scale()
    test_replacement_and_rate()
    test_clock_replacement_and_resend()
    test_map_and_sites_resend()
    test_status_forwarding_and_live_capture()
    test_resends()
    test_force_full_resend()
    test_reboot_edge_triggers_force_full_resend()
    test_real_simulator_through_bridge()
    test_board_report_reaches_the_picture()
    test_report_effect_sent_before_next_tick()
    test_app_launch_route()

    if FAILURES:
        print(f"mission_feed_selftest: {len(FAILURES)} check(s) FAILED")
        return 1
    print("mission_feed_selftest: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

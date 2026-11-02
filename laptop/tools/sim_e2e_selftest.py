#!/usr/bin/env python3
"""
laptop/tools/sim_e2e_selftest.py - end-to-end checks of the sim page (/sim)

Drives the sim page through a real Chromium (Playwright) against a real
app.py in SIM_ONLY mode (no board, no serial port) and the real
simulator: a recorded run, a live run, taking over a recording, sending a
team by hand, and the links into the page (the landing's mission setup,
the run list).

The expected values are worked out here, in Python, straight from the
recording file (worksites numbered in the order teams find them, their
state at a tick, each team's injected-fault stretches), never read back
from the page's own reader (static/shared/rn-run.js), so a bug there
fails this test instead of being agreed with.

Playwright is a dev/test-only dependency (laptop/tools/requirements-
dashboard-dev.txt). This script never installs or downloads a browser:
it uses a preinstalled /opt/pw-browsers/chromium if there is one, else
whatever Playwright's own launch() finds, and SKIPS (exit 0) with a
clear message when neither is available. Recordings the live runs make
go to a temporary folder, never laptop/tools/recordings/.

Exit code 0 when every check passes (or the test is skipped), 1 otherwise.
"""
import gzip
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
HOST = "127.0.0.1"
PORT = 5050  # app.py's DASHBOARD_PORT
BASE = f"http://{HOST}:{PORT}"
DEMO = "demo_seed1041_combined_1200ticks.jsonl.gz"
OLD_FORMAT = "old_format_seed1041.jsonl.gz"   # the demo without STATICENV gateways and team equipment (made at start)
VENDORED_CHROMIUM_PATH = "/opt/pw-browsers/chromium"
LIVE_TYPES = {0, 1, 2, 5}   # incident types that can hold a live rescue (numbered worksites)

FAILURES = []


def check(cond, label):
    print(("PASS: " if cond else "FAIL: ") + label)
    if not cond:
        FAILURES.append(label)


def _port_open(host, port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex((host, port)) == 0


def _chromium_executable():
    """The preinstalled Chromium (a binary, or a folder holding one), else None."""
    if os.path.isfile(VENDORED_CHROMIUM_PATH) and os.access(VENDORED_CHROMIUM_PATH, os.X_OK):
        return VENDORED_CHROMIUM_PATH
    if os.path.isdir(VENDORED_CHROMIUM_PATH):
        for root, _dirs, files in os.walk(VENDORED_CHROMIUM_PATH):
            for name in ("chrome", "chromium", "headless_shell", "Chromium"):
                if name in files and os.access(os.path.join(root, name), os.X_OK):
                    return os.path.join(root, name)
    return None


# ---------------------------------------------------------------- expected values, from the recording itself

def read_frames(path):
    frames = []
    with gzip.open(path, "rt") as fh:
        for line in fh:
            if line.startswith("LIVEFRAME "):
                frames.append(json.loads(line[len("LIVEFRAME "):]))
    return frames


def expected_worksites(frames, T):
    """Worksite ids known by tick T, split into complete / being worked / waiting, as the page must show them."""
    seen = {}
    for f in frames:
        if f["tick"] > T:
            break
        for i in f["incidents"]:
            if i["type"] in LIVE_TYPES and i["discovered"] and i["id"] not in seen:
                seen[i["id"]] = "A-%d" % (len(seen) + 1)
    at = next(f for f in frames if f["tick"] == T)
    now = {i["id"]: i for i in at["incidents"]}
    out = {"complete": set(), "active": set(), "open": set()}
    for inc, wid in seen.items():
        i = now.get(inc)
        if i is None or i["status"] == 3:
            out["complete"].add(wid)
        elif i["status"] == 0:
            out["open"].add(wid)
        else:
            out["active"].add(wid)
    return out


def expected_fault_spans(frames, team):
    """Stretches [from, to] of ticks when the team had an injected fault (fault_mask, ground truth)."""
    spans, cur = [], None
    for f in frames:
        t = f["tick"]
        m = next(x for x in f["teams"] if x["id"] == team)
        if m.get("fault_mask", 0):
            if cur and cur[1] == t - 1:
                cur[1] = t
            else:
                cur = [t, t]
                spans.append(cur)
    return spans


def write_old_format(src, dst):
    """The demo as a recording made before STATICENV carried gateways and frames carried team equipment."""
    with gzip.open(src, "rt") as a, gzip.open(dst, "wt") as b:
        for line in a:
            if line.startswith("STATICENV "):
                o = json.loads(line[len("STATICENV "):]); o.pop("gateways", None)
                line = "STATICENV " + json.dumps(o, separators=(",", ":")) + "\n"
            elif line.startswith("LIVEFRAME "):
                o = json.loads(line[len("LIVEFRAME "):])
                for m in o["teams"]:
                    m.pop("equipment", None)
                line = "LIVEFRAME " + json.dumps(o, separators=(",", ":")) + "\n"
            b.write(line)


# ---------------------------------------------------------------- page helpers

def dismiss_loader(page, timeout=90000):
    """The loading screen (static/shared/rn-loader.js): on the first run of a visit it waits for a key; after that it
    goes on its own. Either way, wait until it has gone."""
    page.wait_for_function("() => !document.querySelector('.rnl') || document.querySelector('.rnl.wait')", timeout=timeout)
    if page.evaluate("() => !!document.querySelector('.rnl.wait')"):
        page.keyboard.press("Enter")
    page.wait_for_function("() => !document.querySelector('.rnl')", timeout=15000)


def open_sim(page, query, timeout=90000):
    page.goto(f"{BASE}/sim?{query}")
    page.wait_for_function("() => document.getElementById('loading').hidden", timeout=timeout)
    dismiss_loader(page, timeout)


def clock(page):
    return page.evaluate("() => document.getElementById('clockSub').textContent")


def view(page, expr):
    return page.evaluate("() => { const D = RNSimTest.view(); return (" + expr + "); }")


# ---------------------------------------------------------------- the checks

def check_loader(page):
    """The loading screen: the first run of a visit plays through, ends on the city's number and waits for a key; the
    next one opens on its own."""
    page.goto(f"{BASE}/sim?run={DEMO}&t=10")
    page.wait_for_function("() => !!document.querySelector('.rnl')", timeout=20000)
    steps = page.evaluate("() => Array.from(document.querySelectorAll('.rnl .log div')).map(d => d.textContent)")
    check(len(steps) == 3 and "Reading the recording" in steps[0], f"opening a recording shows its loading steps ({steps})")
    page.wait_for_function("() => !!document.querySelector('.rnl.wait')", timeout=60000)
    title = page.evaluate("() => document.querySelector('.rnl .title').textContent")
    check("Demo mission" in title and "City 1041" in title, f"the loading screen ends on the run's name and city ({title!r})")
    check(all("DONE" in t for t in page.evaluate("() => Array.from(document.querySelectorAll('.rnl .log div')).map(d => d.textContent)")), "every step is done before it waits")
    page.wait_for_timeout(1500)
    check(page.evaluate("() => !!document.querySelector('.rnl.wait')"), "on the first run of a visit it waits for a key or a click")
    page.keyboard.press("Enter")
    page.wait_for_function("() => !document.querySelector('.rnl')", timeout=10000)
    check(True, "a key opens the page")
    page.goto(f"{BASE}/sim?run={DEMO}&t=20")
    page.wait_for_function("() => document.getElementById('loading').hidden", timeout=60000)
    page.wait_for_function("() => !document.querySelector('.rnl')", timeout=30000)
    check(True, "the next run of the visit opens on its own once loaded")


def check_recorded(page, errors, frames):
    T = 278
    open_sim(page, f"run={DEMO}&t={T}")
    check(view(page, "D.tick") == T, f"a recording opens at the tick in the address (&t={T})")
    facts = page.evaluate("() => document.getElementById('runFacts').textContent")
    check("12 h" in facts and "Mixed faults" in facts and "City 1041" in facts and "5% radio loss" in facts,
          f"the header comes from the recording ({facts!r})")
    check(page.evaluate("() => document.getElementById('runName').textContent") == "Demo mission", "the bundled demo is named Demo mission")
    with gzip.open(os.path.join(TOOLS_DIR, "recordings", DEMO), "rt") as fh:
        env = next(json.loads(l[len("STATICENV "):]) for l in fh if l.startswith("STATICENV "))
    want_exits = sorted((g["x"], g["y"]) for g in env["gateways"])
    got_exits = sorted(tuple(g) for g in view(page, "D.gateways.map(g => [g.x, g.y])"))
    check(len(want_exits) == 3 and got_exits == want_exits, f"the map's exits are the recording's ({got_exits})")
    check("Exit (evacuees leave the map)" in page.evaluate("() => document.getElementById('legend').textContent"),
          "the legend explains the exit mark")
    # the radio per team (Technical tab), computed here from the frames' heard masks
    byt = {f["tick"]: f for f in frames}
    n = len(frames[T]["teams"])
    def bits(H, i, j): return H[i] >> j & 1
    H = [t["heard"] for t in byt[T]["teams"]]
    hs, hb, of = [0] * n, [0] * n, [0] * n
    for k, f in byt.items():
        if T - 100 < k <= T:
            H2 = [t["heard"] for t in f["teams"]]
            for i in range(n):
                of[i] += n - 1
                hs[i] += sum(bits(H2, i, j) for j in range(n) if j != i)
                hb[i] += sum(bits(H2, j, i) for j in range(n) if j != i)
    want_radio = [f"{sum(bits(H, i, j) for j in range(n) if j != i)} of {n - 1}|{sum(bits(H, j, i) for j in range(n) if j != i)} of {n - 1}|"
                  f"{round(hs[i] / of[i] * 100)}% / {round(hb[i] / of[i] * 100)}%" for i in range(n)]
    page.click("[data-tab=tech]")
    page.wait_for_selector(".radio tbody tr", timeout=10000)
    got_radio = page.evaluate("() => Array.from(document.querySelectorAll('.radio tbody tr')).map(r => Array.from(r.cells).slice(1).map(c => c.textContent).join('|'))")
    check(got_radio == want_radio, f"the Technical tab's radio per team matches the frames (team 1: {got_radio[:1]})")
    vb = byt[T]["vbus"]
    check(f"{round(vb['delivered'] / (vb['delivered'] + vb['dropped']) * 100)}%" in page.evaluate("() => document.querySelector('.radio-sum').textContent"),
          "the whole-run radio line comes from the frame's bus counters")
    page.click("[data-tab=teams]")
    check(clock(page).startswith(f"Tick {T} of 1200"), f"the clock reads tick {T} of 1200 ({clock(page)!r})")
    rows = page.evaluate("() => document.querySelectorAll('#teamList .team').length")
    check(rows == len(frames[T]["teams"]), f"one row per team in the run ({rows})")

    want = expected_worksites(frames, T)
    got_done = set(view(page, "D.done.map(w => w.id)"))
    got_active = set(view(page, "D.worksites.filter(w => w.status === 1 || w.status === 2).map(w => w.id)"))
    got_open = set(view(page, "D.worksites.filter(w => w.status === 0).map(w => w.id)"))
    check(got_done == want["complete"] and got_active == want["active"] and got_open == want["open"],
          f"worksites at tick {T} match the recording: complete {sorted(got_done)}, worked {sorted(got_active)}, waiting {sorted(got_open)}")
    later = expected_worksites(frames, len(frames) - 1)
    unfound = sorted(set().union(*later.values()) - set().union(*want.values()))
    shown = set(view(page, "D.worksites.concat(D.done).map(w => w.id)"))
    check(unfound and not (set(unfound) & shown), f"worksites no team has found yet are not shown ({unfound[:4]} ...)")
    summary = page.evaluate("() => document.getElementById('sum').textContent")
    check(f"{len(want['complete'])} complete" in summary and f"{len(want['active'])} being worked" in summary and f"{len(want['open'])} waiting" in summary,
          f"the summary counts them ({summary!r})")
    # the Worksites tab: waiting ones first, longest since found first (all live rescues are critical, so priority ties),
    # then those being worked; worksites are numbered in the order found
    num = lambda w: int(w.split("-")[1])
    page.click("[data-tab=sites]")
    page.wait_for_selector(".ws[data-id]", timeout=10000)
    rows = page.evaluate("() => Array.from(document.querySelectorAll('.ws[data-id]')).map(r => [r.dataset.id, r.children[1].textContent])")
    check([r[0] for r in rows] == sorted(want["open"], key=num) + sorted(want["active"], key=num),
          f"the Worksites tab lists waiting worksites first, longest waiting first, then those being worked ({[r[0] for r in rows]})")
    mins = [int(r[1].split("found ")[1].split(" min")[0]) for r in rows if "found " in r[1]]
    check(len(mins) == len(want["open"]) and mins == sorted(mins, reverse=True),
          f"each waiting worksite says how long since it was found ({mins})")
    page.click("[data-tab=teams]")

    spans = page.evaluate("() => RNSimTest.run().spans()[0].fault")
    check(spans == expected_fault_spans(frames, 0), f"team 1's injected-fault stretches match fault_mask in the recording ({len(spans)} stretches)")

    page.keyboard.press("ArrowRight")
    check(view(page, "D.tick") == T + 1, "the right arrow steps one tick on")
    page.click("#nextEv")
    nxt = view(page, "D.tick")
    events = page.evaluate("() => RNSimTest.run().events.map(e => e[0])")
    check(nxt == min(t for t in events if t > T + 1), f"Next event jumps to the next event's tick ({nxt})")
    page.click("#playBtn"); page.wait_for_timeout(1200); page.click("#playBtn")
    check(view(page, "D.tick") > nxt, f"Play moves the clock on ({nxt} -> {view(page, 'D.tick')})")

    page.click("#vflat"); page.wait_for_timeout(300)
    check(not page.evaluate("() => RNSimTest.is3d()") and page.evaluate("() => getComputedStyle(document.querySelector('.cam')).display") == "none",
          "the flat view hides the 3D camera buttons")
    hits = page.evaluate("() => RNSimTest.flatHits().length")
    check(hits >= rows, f"the flat map draws a clickable marker for every team and worksite ({hits})")
    if page.evaluate("() => !!(window.THREE && window.THREE.OrbitControls)"):
        page.click("#v3d"); page.wait_for_timeout(500)
        check(page.evaluate("() => RNSimTest.is3d()"), "the 3D view opens (three.js from the app's own copy)")

    wid = sorted(want["active"])[0]
    page.evaluate("(w) => RNSimTest.open('w', w)", wid)
    card = page.evaluate("() => document.getElementById('markCard').innerText")
    check(wid in card and "SEND A TEAM" not in card.upper(), f"a worksite card opens; a recording offers no Send a team ({wid})")
    page.click("#tlBtn")
    strips = page.evaluate("() => document.querySelectorAll('#detail .strip').length")
    check(strips == rows, f"Detection detail has one strip per team ({strips})")
    check(page.evaluate("() => !document.getElementById('takeover').hidden"), "a recording offers Take over")


def check_old_format(page, errors):
    before = len(errors)
    open_sim(page, f"run={OLD_FORMAT}&t=200")
    check(view(page, "D.teams.every(t => t.equip === null)") and view(page, "D.gateways.length") == 0,
          "an older recording (no team equipment, no gateways) loads")
    meters = page.evaluate("() => Array.from(document.querySelectorAll('#teamList .meters b')).map(b => b.textContent)")
    check(meters[1::2] and all(m == "–" for m in meters[1::2]), "a missing equipment level shows as a dash, not a made-up number")
    check(len(errors) == before, f"no page errors on the older recording ({errors[before:][:3]})")


def check_missing_run(page):
    page.goto(f"{BASE}/sim?run=no_such_run.jsonl.gz")
    page.wait_for_function("() => /recordings|read/.test(((document.querySelector('.rnl .err') || document.getElementById('loadingTxt')) || {}).textContent || '')", timeout=20000)
    check(True, "a run that is not there says so instead of drawing nothing")


def check_takeover(page, frames, recordings_dir):
    T = 120
    open_sim(page, f"run={DEMO}&t={T}")
    before = page.evaluate("() => RNSimTest.view().teams.map(t => [t.x, t.y, t.status])")
    page.click("#takeover")
    page.wait_for_function("() => RNSimTest.live() && RNSimTest.live().state !== 'starting' && RNSimTest.view().tick === %d" % T, timeout=90000)
    check(page.evaluate("() => document.getElementById('runName').textContent") == "Live run", "Take over turns the page into a live run")
    check(page.evaluate("() => RNSimTest.view().teams.map(t => [t.x, t.y, t.status])") == before,
          f"the live run starts from the recording's state at tick {T}")
    check(page.evaluate("() => RNSimTest.run().first()") == 0 and page.evaluate("() => RNSimTest.live().state") == "paused",
          "it keeps the recording's earlier ticks and starts paused")
    page.click("#playBtn")
    page.wait_for_function("() => RNSimTest.view().tick > %d" % (T + 3), timeout=30000)
    check(True, "the taken-over run carries on live")
    page.click("#stopBtn")
    page.wait_for_function("(d) => { const r = new URLSearchParams(location.search).get('run'); return r && r !== d; }", arg=DEMO, timeout=30000)
    name = page.evaluate("() => new URLSearchParams(location.search).get('run')")
    check(os.path.exists(os.path.join(recordings_dir, name)) and page.evaluate("() => document.getElementById('runName').textContent") == "Finished run",
          f"stopping saves it as a new recording and the page carries on as that ({name})")


def check_live(page):
    open_sim(page, "seed=1012&ticks=150&faults=communication&speed=1")
    check(page.evaluate("() => document.getElementById('runName').textContent") == "Live run", "a live run starts from the address (seed, ticks, faults, speed)")
    page.wait_for_function("() => RNSimTest.view().tick >= 10", timeout=60000)
    page.click("#playBtn")
    page.wait_for_function("() => RNSimTest.live().state === 'paused'", timeout=10000)
    page.wait_for_timeout(400); a = page.evaluate("() => RNSimTest.run().last()")
    page.wait_for_timeout(1200); b = page.evaluate("() => RNSimTest.run().last()")
    check(a == b, f"pause stops the simulation itself (tick {a} -> {b})")
    page.click("#playBtn")
    page.wait_for_function(f"() => RNSimTest.run().last() > {b + 3}", timeout=20000)
    check(True, "resume carries on")
    page.keyboard.press("ArrowLeft"); page.keyboard.press("ArrowLeft"); page.keyboard.press("ArrowLeft")
    page.wait_for_timeout(800)
    lag = page.evaluate("() => RNSimTest.run().last() - RNSimTest.view().tick")
    check(lag >= 3 and page.evaluate("() => !document.getElementById('toLive').hidden"), f"looking back stays put while the run carries on ({lag} ticks behind)")
    page.click("#toLive"); page.wait_for_timeout(500)
    check(page.evaluate("() => RNSimTest.live().follow"), "Back to live follows the newest tick again")
    page.wait_for_function("() => /run=/.test(location.search)", timeout=90000)
    check(page.evaluate("() => document.getElementById('runName').textContent") == "Finished run" and "saved" in clock(page),
          f"when the run ends it is saved and the page carries on as that recording ({clock(page)!r})")


def _marker_points(page, which):
    """Page coordinates of each marker's centre that a press there would hit (the topmost marker at that point)."""
    hits = page.evaluate("""(w) => { const cv = document.getElementById(w === 'flat' ? 'cv' : 'cv3').getBoundingClientRect();
      const hs = w === 'flat' ? RNSimTest.flatHits() : RNSimTest.hits3D();
      return hs.map(h => ({ kind: h.kind, ref: h.ref, l: h.l, t: h.t, r: h.r, b: h.b, ox: cv.left, oy: cv.top })); }""", which)
    out = []
    for i, h in enumerate(hits):
        cx, cy = (h["l"] + h["r"]) / 2, (h["t"] + h["b"]) / 2
        top = None
        for g in hits:   # the page's hitAt(): the last marker drawn wins
            if g["l"] <= cx <= g["r"] and g["t"] <= cy <= g["b"]:
                top = g
        if top is h:
            out.append({"kind": h["kind"], "ref": h["ref"], "x": h["ox"] + cx, "y": h["oy"] + cy})
    return out


def _drag(page, a, b):
    page.mouse.move(a["x"], a["y"]); page.mouse.down()
    page.mouse.move((a["x"] + b["x"]) / 2, (a["y"] + b["y"]) / 2, steps=6)
    page.mouse.move(b["x"], b["y"], steps=6); page.mouse.up()


def check_override(page):
    open_sim(page, "seed=1041&ticks=400&faults=none&speed=1")
    # paused with a worksite still waiting for a team (the system may assign the one seen a moment before the pause)
    wid = None
    for _ in range(12):
        page.wait_for_function("() => { const D = RNSimTest.view(); return D.worksites.some(w => w.status === 0) && D.teams.some(t => t.status === 2 && t.tier[0] < 2 && t.tier[1] < 2); }", timeout=150000)
        page.click("#playBtn"); page.wait_for_function("() => RNSimTest.live().state === 'paused'", timeout=10000)
        wid = page.evaluate("() => { const w = RNSimTest.view().worksites.find(w => w.status === 0); return w ? w.id : null; }")
        if wid:
            break
        page.click("#playBtn"); page.wait_for_timeout(1500)
    check(wid is not None, "a worksite waiting for a team to send one to")
    page.evaluate("(w) => RNSimTest.open('w', w)", wid)
    buttons = page.evaluate("() => Array.from(document.querySelectorAll('#markCard .send button')).map(b => [+b.dataset.team, b.disabled])")
    flagged = page.evaluate("() => RNSimTest.view().raw_teams.filter(r => r.degraded || (r.consensus_tier || [0, 0]).includes(2)).map(r => r.id)")
    check(len(buttons) == page.evaluate("() => RNSimTest.view().teams.length") and all(d for t, d in buttons if t in flagged),
          f"a live worksite card offers every team; teams the system has flagged are greyed out "
          f"({'flagged now: ' + str(flagged) if flagged else 'none flagged at this tick, so only the count is checked'})")
    working = page.evaluate("() => RNSimTest.view().teams.filter(t => t.status === 2).map(t => t.id)")
    page.keyboard.press("Escape")
    # drag a team onto the worksite on the flat map (the old dashboard's gesture), then confirm on the card
    page.click("#vflat"); page.wait_for_timeout(300)
    pts = _marker_points(page, "flat")
    target = next((m for m in pts if m["kind"] == "w" and m["ref"] == wid), None)
    free = [t for t, d in buttons if not d]
    team = next((m["ref"] for m in pts if m["kind"] == "t" and m["ref"] in working and m["ref"] in free), None)
    if team is None:
        team = next(m["ref"] for m in pts if m["kind"] == "t" and m["ref"] in free)
    check(target is not None, f"the worksite's marker can be dropped on ({wid})")
    _drag(page, next(m for m in pts if m["kind"] == "t" and m["ref"] == team), target)
    ask = page.evaluate("() => (document.querySelector('#markCard .ask') || {}).textContent || ''")
    check(f"Send team {team + 1} here?" in ask, f"dragging team {team + 1} onto {wid} opens its card asking to confirm ({ask!r})")
    page.click('#markCard [data-a="confirm"]')
    page.click("#playBtn")
    page.wait_for_function("() => /goes there next|Not sent/.test((document.querySelector('#markCard .ack') || {}).textContent || '')", timeout=30000)
    ack = page.evaluate("() => document.querySelector('#markCard .ack').textContent")
    check("goes there next" in ack, f"the simulator applies it and the card says so ({ack!r})")
    page.wait_for_timeout(600)
    pinned = page.evaluate("""(t) => { const R = RNSimTest.run(), e = R.events.find(e => e[1] === 'sent'); if (!e) return false;
      return R.ticks.filter(k => k >= e[0]).some(k => { const f = R.frame(k), r = f.teams.find(r => r.id === t), i = f.incidents.find(i => i.id === e[5]);
        return (r.override === 1 && r.override_incident_id === e[5]) || (i && i.assigned_team === t); }); }""", team)
    # a free team is assigned there at once (the pin is used up before any frame shows it); a busy one shows the pin first
    check(pinned, "the frames after it show the team pinned to that worksite's incident, or already assigned to it")
    check(page.evaluate("(w) => RNSimTest.run().events.some(e => e[1] === 'sent' && e[2] === w)", wid), "it is listed as an event")
    page.evaluate("(t) => RNSimTest.open('t', t)", team)
    page.evaluate("(t) => RNSimTest.release(t)", team)
    page.wait_for_function("() => /Released|Not/.test((document.querySelector('#markCard .ack') || {}).textContent || '')", timeout=30000)
    check("Released" in page.evaluate("() => document.querySelector('#markCard .ack').textContent"), "Release is applied")
    if page.evaluate("() => !!(window.THREE && window.THREE.OrbitControls)"):
        # the same gesture in 3D: the camera holds still and the card asks; Cancel sends nothing
        page.click("#playBtn"); page.wait_for_function("() => RNSimTest.live().state === 'paused'", timeout=10000)
        page.keyboard.press("Escape"); page.click("#v3d"); page.wait_for_timeout(1200)
        ws3, tm3 = [], []
        for _ in range(10):   # a moment with a waiting worksite and a team both clickable on screen
            pts3 = _marker_points(page, "3d")
            ws3 = [m for m in pts3 if m["kind"] == "w" and page.evaluate("(w) => RNSimTest.view().worksites.some(x => x.id === w && x.status === 0)", m["ref"])]
            tm3 = [m for m in pts3 if m["kind"] == "t"]
            if ws3 and tm3:
                break
            page.click("#playBtn"); page.wait_for_timeout(1500); page.click("#playBtn")
            page.wait_for_function("() => RNSimTest.live().state === 'paused'", timeout=10000); page.wait_for_timeout(600)
        if ws3 and tm3:
            cam0 = page.evaluate("() => RNSimTest.camera()")
            sent0 = page.evaluate("() => RNSimTest.run().events.filter(e => e[1] === 'sent').length")
            _drag(page, tm3[0], ws3[0])
            ask3 = page.evaluate("() => (document.querySelector('#markCard .ask') || {}).textContent || ''")
            check("here?" in ask3 and page.evaluate("() => RNSimTest.camera()") == cam0,
                  f"in 3D, dragging a team onto a worksite asks to confirm and the camera holds still ({ask3!r})")
            page.click('#markCard [data-a="cancel"]')
            page.wait_for_timeout(300)   # the card redraws just after the click (it holds still while a button is pressed)
            check(page.evaluate("() => !document.querySelector('#markCard .ask')") and
                  page.evaluate("() => RNSimTest.run().events.filter(e => e[1] === 'sent').length") == sent0, "Cancel sends nothing")
        else:
            check(False, f"3D drag: no worksite and team markers to drag between ({len(ws3)}, {len(tm3)})")
        page.click("#playBtn")
    page.click("#stopBtn")
    page.wait_for_function("() => /run=/.test(location.search)", timeout=30000)
    name = page.evaluate("() => new URLSearchParams(location.search).get('run')")
    open_sim(page, f"run={name}")
    kinds = page.evaluate("() => RNSimTest.run().events.filter(e => e[1] === 'sent' || e[1] === 'released').map(e => e[1])")
    check(kinds == ["sent", "released"], f"the saved recording replays both as events ({kinds})")


def check_links(page):
    page.goto(f"{BASE}/landing#connect")
    page.wait_for_timeout(800)
    page.evaluate("() => { document.getElementById('msSeed').value = '1012'; }")
    page.evaluate("() => document.getElementById('msForm').requestSubmit()")
    page.wait_for_url("**/sim?**", timeout=20000)
    url = page.url
    check("/sim?seed=1012" in url and "ticks=" in url and "faults=" in url and "speed=1" in url,
          f"the landing's mission setup opens the sim page with its choices, speed included ({url.split('/', 3)[-1]})")
    page.wait_for_function("() => document.getElementById('loading').hidden || /app/.test(document.getElementById('loadingTxt').textContent)", timeout=90000)
    page.evaluate("() => fetch('/api/run-live/stop', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' })")
    page.goto(f"{BASE}/replay")
    page.wait_for_function("() => document.querySelectorAll('a.card, a.run').length > 0", timeout=20000)
    hrefs = page.evaluate("() => Array.from(document.querySelectorAll('a.card, a.run')).map(a => a.getAttribute('href'))")
    check(all(h.startswith("/sim?run=") for h in hrefs), f"every run in the run list opens on the sim page ({len(hrefs)} links)")
    page.goto(f"{BASE}/sim?run={DEMO}")
    page.wait_for_function("() => document.getElementById('loading').hidden", timeout=60000)
    dismiss_loader(page)
    check(page.evaluate("() => document.querySelector('.back').getAttribute('href')") == "/replay", "the sim page's back link goes to the run list")


def check_board_absent(page):
    """/sim?board with no board connected (the SIM_ONLY app): says so, and offers no start."""
    page.goto(f"{BASE}/sim?board")
    page.wait_for_function("() => /board|answer/i.test((document.getElementById('bState') || {}).textContent || '') && !/Asking/.test(bState.textContent)", timeout=20000)
    txt = page.evaluate("() => bState.textContent")
    check("No board connected" in txt and page.evaluate("() => bGo.disabled"), f"board mode with no board connected says so and cannot start ({txt!r})")


def check_board(page):
    """A board mission against the app's test-only fake board (DASHBOARD_FAKE_BOARD=1: a stand-in serial port and a
    completed handshake, nothing ever answers): the real launch, stream, override and stop routes."""
    page.goto(f"{BASE}/sim?board")
    page.wait_for_function("() => /ready/.test((document.getElementById('bState') || {}).textContent || '')", timeout=20000)
    check(page.evaluate("() => !bGo.disabled"), "board mode: the boards are ready and the mission can start")
    page.select_option("#bPace", "60"); page.select_option("#bTicks", "200"); page.click("#bGo")
    page.wait_for_function("() => document.getElementById('loading').hidden", timeout=90000)
    dismiss_loader(page)
    facts = page.evaluate("() => runName.textContent + ' | ' + runFacts.textContent")
    check(facts.startswith("Board mission") and "On the boards" in facts and "60" in facts, f"the board mission opens with its own header ({facts})")
    page.wait_for_function("() => RNSimTest.view().tick >= 3", timeout=30000)
    terr = page.evaluate("() => [RNSimTest.view().roads.length, RNSimTest.view().water.length, RNSimTest.view().buildings.length]")
    check(terr[0] > 0 and terr[2] > 0, f"the board mission's map has its roads, water and buildings (the feed's STATICMAP; {terr})")
    check(page.evaluate("() => playBtn.disabled"), "pause is off: a board mission runs at its own pace")
    page.click("#vflat"); page.wait_for_timeout(400)   # the flat view: the test browser's software 3D is too slow to click into
    page.wait_for_function("() => RNSimTest.view().worksites.some(w => w.status === 0)", timeout=90000)
    wid = page.evaluate("() => RNSimTest.view().worksites.find(w => w.status === 0).id")
    page.evaluate("(w) => RNSimTest.open('w', w)", wid)
    check(page.evaluate("() => document.querySelectorAll('#markCard .send button').length") > 0 and
          not page.evaluate("() => !!document.querySelector('#markCard [data-a=release]')"),
          "a board mission's worksite card can send a team (no Release: that is the simulator's own route)")
    page.click("#markCard .send button:not([disabled])")
    page.wait_for_function("() => /waiting for it to confirm|Not sent/.test((document.querySelector('#markCard .ack') || {}).textContent || '')", timeout=20000)
    check(True, "the send goes to the board (POST /api/board/override) and the card waits for its answer")
    page.wait_for_function("() => /no answer from the board/.test((document.querySelector('#markCard .ack') || {}).textContent || '')", timeout=40000)
    check(True, "the fake board never answers, and after the app's timeout the card says so instead of claiming it worked")
    page.click("#stopBtn")
    page.wait_for_function("() => /ended/.test(document.getElementById('runName').textContent + clockSub.textContent)", timeout=30000)
    check(True, "Stop ends the board mission")


def _start_app(env):
    proc = subprocess.Popen([sys.executable, "app.py"], cwd=TOOLS_DIR, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(40):
        if _port_open(HOST, PORT):
            return proc
        time.sleep(0.25)
    proc.kill()
    return None


def main():
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("SKIP: sim_e2e_selftest (Playwright is not installed - it is a dev-only dependency, "
              "see laptop/tools/requirements-dashboard-dev.txt)")
        return 0
    demo_src = os.path.join(TOOLS_DIR, "recordings", DEMO)
    if not os.path.exists(demo_src):
        print(f"FAIL: the bundled demo recording is missing ({demo_src})")
        return 1
    frames = read_frames(demo_src)
    recordings_dir = tempfile.mkdtemp(prefix="sim_e2e_recordings_")
    shutil.copy(demo_src, recordings_dir)
    write_old_format(demo_src, os.path.join(recordings_dir, OLD_FORMAT))

    app_proc = None
    if _port_open(HOST, PORT):
        print(f"FAIL: something is already listening on {HOST}:{PORT}; stop it first (this test starts its own app)")
        return 1
    env = dict(os.environ, SIM_ONLY="1", DASHBOARD_RECORDINGS_DIR=recordings_dir)
    app_proc = subprocess.Popen([sys.executable, "app.py"], cwd=TOOLS_DIR, env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(40):
            if _port_open(HOST, PORT):
                break
            time.sleep(0.25)
        else:
            print(f"FAIL: app.py did not come up on {HOST}:{PORT} within 10s")
            return 1
        with sync_playwright() as p:
            kwargs = {"args": ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"]}
            exe = _chromium_executable()
            if exe:
                kwargs["executable_path"] = exe
            try:
                browser = p.chromium.launch(**kwargs)
            except Exception as exc:  # noqa: BLE001
                print(f"SKIP: sim_e2e_selftest (no usable Chromium: not at {VENDORED_CHROMIUM_PATH}, and Playwright's own "
                      f"launch() found none; this script never downloads one): {exc}")
                return 0
            errors = []
            page = browser.new_page(viewport={"width": 1440, "height": 900})
            page.on("pageerror", lambda e: errors.append(str(e)))
            page.on("console", lambda m: errors.append(m.text) if m.type == "error" and "404" not in m.text and "ERR_INCOMPLETE_CHUNKED_ENCODING" not in m.text else None)
            for name, fn in (("loading screen", lambda: check_loader(page)),
                             ("recorded run", lambda: check_recorded(page, errors, frames)),
                             ("older recording", lambda: check_old_format(page, errors)),
                             ("missing run", lambda: check_missing_run(page)),
                             ("take over", lambda: check_takeover(page, frames, recordings_dir)),
                             ("live run", lambda: check_live(page)),
                             ("sending a team", lambda: check_override(page)),
                             ("links", lambda: check_links(page)),
                             ("board mode without a board", lambda: check_board_absent(page))):
                try:
                    fn()
                except Exception as exc:  # noqa: BLE001 - one broken step must not hide the others
                    check(False, f"{name}: the step itself failed ({type(exc).__name__}: {str(exc).splitlines()[0][:200]})")
            check(not errors, f"no page errors anywhere in the run ({errors[:3]})")
            browser.close()
    finally:
        app_proc.kill()
        app_proc.wait()
    # board mode: a second app, in hardware mode with the test-only fake board attached
    for _ in range(40):
        if not _port_open(HOST, PORT):
            break
        time.sleep(0.25)
    board_proc = _start_app(dict(os.environ, DASHBOARD_FAKE_BOARD="1", DASHBOARD_RECORDINGS_DIR=recordings_dir))
    try:
        if board_proc is None:
            check(False, "board mode: the app with the fake board did not come up")
        else:
            with sync_playwright() as p:
                kwargs = {"args": ["--use-gl=angle", "--use-angle=swiftshader", "--enable-unsafe-swiftshader"]}
                exe = _chromium_executable()
                if exe:
                    kwargs["executable_path"] = exe
                browser = p.chromium.launch(**kwargs)
                errors = []
                page = browser.new_page(viewport={"width": 1440, "height": 900})
                page.on("pageerror", lambda e: errors.append(str(e)))
                page.on("console", lambda m: errors.append(m.text) if m.type == "error" and "404" not in m.text and "ERR_INCOMPLETE_CHUNKED_ENCODING" not in m.text else None)
                try:
                    check_board(page)
                except Exception as exc:  # noqa: BLE001
                    check(False, f"board mission: the step itself failed ({type(exc).__name__}: {str(exc).splitlines()[0][:200]})")
                check(not errors, f"no page errors in the board mission ({errors[:3]})")
                browser.close()
    finally:
        if board_proc is not None:
            board_proc.kill()
            board_proc.wait()
        shutil.rmtree(recordings_dir, ignore_errors=True)
        csv = os.path.join(TOOLS_DIR, "tron_snapshot_dataset.csv")   # the simulator's own side file from the runs above
        if os.path.exists(csv) and os.path.getmtime(csv) > time.time() - 3600 and not _tracked(csv):
            os.remove(csv)
    if FAILURES:
        print(f"\n{len(FAILURES)} sim page check(s) failed:")
        for f in FAILURES:
            print(" - " + f)
        return 1
    print("\nAll sim_e2e_selftest checks passed.")
    return 0


def _tracked(path):
    try:
        return subprocess.run(["git", "ls-files", "--error-unmatch", path], cwd=TOOLS_DIR,
                              capture_output=True).returncode == 0
    except OSError:
        return True


if __name__ == "__main__":
    sys.exit(main())

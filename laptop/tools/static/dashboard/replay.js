/*
 * laptop/tools/static/dashboard/replay.js
 *
 * Part 2: replay of a recorded live run. A recording is the exact
 * sequence of lines app.py already forwards during a live run
 * (STATICENV once, LIVEFRAME every tick - see app.py's _RecordingWriter
 * and /api/recordings*). Load() fetches one recording's full
 * decompressed text ONCE and parses it into an in-memory array of
 * frame objects; every later scrub/play/jump-to-detection is then just
 * indexing that array - no re-fetching or re-parsing the file itself.
 *
 * Rendering reuses the exact same functions the live SSE loop calls
 * (drawFrame, updateLiveTables, ConsensusNetwork.update,
 * DetectionTimeline.update - see index.html's runLive()) so a replayed
 * frame looks pixel-identical to the live frame that produced it.
 *
 * A few pieces of index.html's inline script state (staticEnvironment,
 * simTelemetry) are declared with `let` at that script's top level,
 * which - unlike a `var`/`function` declaration - does NOT create a
 * `window.*` property this separate file could assign directly. Rather
 * than reach into index.html's closures, this file drives them through
 * a couple of small setter functions index.html exposes for exactly
 * this purpose (setReplayStaticEnvironment, replaySyncTelemetry).
 */
(function (global) {
  "use strict";

  const BASE_INTERVAL_MS = 150; // matches app.py's default tick_delay_ms
  const TIMELINE_WINDOW = 600;  // DetectionTimeline's own MAX_HISTORY

  let header = null;      // parsed RECORDING header line (seed, fault_profile, ticks, link_loss, git_commit, ...)
  let frames = [];        // full parsed LIVEFRAME array; array index === tick (ticks are sequential from 0)
  let currentIndex = -1;
  let playing = false;
  let speed = 1;
  let playTimer = null;
  let envHistory = [];    // [{index, env}]: every STATICENV in the recording, with the index of the first frame it applies to
  let currentFile = null; // filename of the loaded recording (for take-over)
  let active = false;     // true once a recording is loaded - gates map drag-drop (index.html reads this)

  function isActive() { return active; }

  function els() {
    return {
      select: document.getElementById("replaySelect"),
      playBtn: document.getElementById("replayPlayBtn"),
      speedField: document.getElementById("replaySpeedField"),
      jumpBtn: document.getElementById("replayJumpBtn"),
      stateWrap: document.getElementById("replayStateWrap"),
      stateLabel: document.getElementById("replayStateLabel"),
      scrubBar: document.getElementById("replayScrubBar"),
      scrubber: document.getElementById("replayScrubber"),
      tickLabel: document.getElementById("replayTickLabel"),
      takeoverBtn: document.getElementById("replayTakeoverBtn"),
    };
  }

  async function refreshList() {
    const { select } = els();
    if (!select) return;
    try {
      const resp = await fetch("/api/recordings");
      const items = await resp.json();
      const prevValue = select.value;
      select.innerHTML = "";
      if (!items.length) {
        const opt = document.createElement("option");
        opt.value = "";
        opt.textContent = "no recordings yet - run something live first";
        select.appendChild(opt);
        return;
      }
      items.forEach((it) => {
        const opt = document.createElement("option");
        opt.value = it.filename;
        const loss = (it.link_loss === null || it.link_loss === undefined) ? "default loss" : `loss ${it.link_loss}`;
        opt.textContent = `seed ${it.seed} · ${it.fault_profile} · ${loss} · ${it.ticks} ticks`;
        select.appendChild(opt);
      });
      if (prevValue && items.some((it) => it.filename === prevValue)) select.value = prevValue;
    } catch (e) {
      console.error("failed to list recordings:", e);
    }
  }

  function parseRecordingText(text) {
    const outFrames = [];
    let outHeader = null, outEnv = null, outMap = null;
    const outEnvHistory = [];
    const lines = text.split("\n");
    for (const raw of lines) {
      if (!raw) continue;
      if (raw.startsWith("RECORDING ")) {
        try { outHeader = JSON.parse(raw.slice("RECORDING ".length)); } catch (e) { /* skip malformed line */ }
      } else if (raw.startsWith("STATICENV ")) {
        try {
          outEnv = JSON.parse(raw.slice("STATICENV ".length));
          // The simulator re-sends the building list when an aftershock
          // destroys buildings, so a recording holds several; remember
          // where each one starts (the 3D view shows the city as of the
          // frame being replayed - see envAt()).
          outEnvHistory.push({ index: outFrames.length, env: outEnv });
        } catch (e) { /* skip malformed line */ }
      } else if (raw.startsWith("STATICMAP ")) {
        // Only the LAST STATICMAP line matters for replay - it can be
        // re-emitted mid-recording (terrain damage accumulating), and a
        // scrub/jump doesn't try to show terrain "as of that tick", just
        // the final/latest terrain, same simplification the live path
        // makes by only ever holding one `terrainMap` value at a time.
        try { outMap = JSON.parse(raw.slice("STATICMAP ".length)); } catch (e) { /* skip malformed line */ }
      } else if (raw.startsWith("LIVEFRAME ")) {
        try { outFrames.push(JSON.parse(raw.slice("LIVEFRAME ".length))); } catch (e) { /* skip malformed line */ }
      }
      // OVERRIDE / OVERRIDEACK lines (Part 4+) aren't emitted into a
      // Part-2/3 recording yet, so there is nothing to parse for them here.
    }
    return { header: outHeader, env: outEnv, map: outMap, frames: outFrames, envHistory: outEnvHistory };
  }

  function setButtonsVisible(visible) {
    const { playBtn, speedField, jumpBtn, stateWrap, scrubBar, takeoverBtn } = els();
    [playBtn, speedField, jumpBtn, stateWrap, scrubBar, takeoverBtn].forEach((el) => {
      if (el) el.classList.toggle("hidden", !visible);
    });
  }

  async function load(filename) {
    if (!filename) return;
    stop();
    const resp = await fetch(`/api/recordings/${encodeURIComponent(filename)}`);
    if (!resp.ok) {
      alert("Failed to load recording: " + resp.status);
      return;
    }
    const text = await resp.text();
    const parsed = parseRecordingText(text);
    header = parsed.header;
    frames = parsed.frames;
    envHistory = parsed.envHistory || [];
    currentFile = filename;
    currentIndex = -1;

    if (typeof global.telemetryReset === "function") global.telemetryReset();
    if (typeof global.setReplayStaticEnvironment === "function") global.setReplayStaticEnvironment(parsed.env);
    if (typeof global.setReplayTerrainMap === "function") global.setReplayTerrainMap(parsed.map);
    if (global.ConsensusNetwork) global.ConsensusNetwork.reset();
    if (global.DetectionTimeline) global.DetectionTimeline.reset();

    active = frames.length > 0;
    setButtonsVisible(active);
    const { scrubber } = els();
    if (scrubber) { scrubber.min = 0; scrubber.max = Math.max(0, frames.length - 1); scrubber.value = 0; }

    if (!active) {
      alert("That recording has no frames.");
      return;
    }
    setState("paused");
    updatePlayButtonLabel();
    seekToIndex(0);
  }

  function loadSelected() {
    const { select } = els();
    if (select && select.value) load(select.value);
  }

  function setState(label) {
    const { stateLabel } = els();
    if (stateLabel) stateLabel.textContent = label;
  }

  // Each LIVEFRAME already carries the run's absolute (not incremental)
  // counters, so - unlike the live SSE path's telemetryProcessFrame(),
  // which assumes ticks only ever move forward - this can be computed
  // fresh for whatever index the user scrubbed to, forward or backward,
  // by scanning the frames already in memory. lastChangeTick is found
  // exactly (not estimated) by walking back from `index` per team until
  // its status differs, which the live path can only approximate
  // because it doesn't keep the whole run in memory.
  function syncTelemetry(index) {
    if (typeof global.replaySyncTelemetry !== "function") return;
    const frame = frames[index];
    const cumulative = {
      incidents_resolved: frame.incidents_resolved,
      stage_completions: frame.stage_completions,
      work_progress_calls: frame.work_progress_calls,
      in_progress_entries: frame.in_progress_entries,
      safezone_fallback_count: frame.safezone_fallback_count,
      discarded_signal_count: frame.discarded_signal_count,
    };
    const lastChangeTick = {};
    (frame.teams || []).forEach((team, i) => {
      let lastChangeIdx = index;
      for (let j = index - 1; j >= 0; j--) {
        const prevTeam = (frames[j].teams || [])[i];
        if (!prevTeam || prevTeam.status !== team.status) break;
        lastChangeIdx = j;
      }
      lastChangeTick[i] = frames[lastChangeIdx].tick;
    });
    global.replaySyncTelemetry(cumulative, lastChangeTick);
  }

  // Rebuilds the timeline's history from frames 0..index (its own
  // window keeps the newest TIMELINE_WINDOW) and draws once.
  function rebuildTimeline(index) {
    if (!global.DetectionTimeline) return;
    const start = Math.max(0, index - TIMELINE_WINDOW + 1);
    global.DetectionTimeline.setHistory(frames.slice(start, index + 1));
  }

  // `sequential` is true only for normal playback stepping to the very
  // next frame, where appending is enough; every other entry (seek,
  // scrub, jump, load, reset) rebuilds the history.
  function renderFrameAt(index, sequential) {
    if (index < 0 || index >= frames.length) return;
    const canAppend = sequential && currentIndex >= 0 && index === currentIndex + 1;
    currentIndex = index;
    const frame = frames[index];
    syncTelemetry(index);
    global.drawFrame(frame);
    global.updateLiveTables(frame);
    if (global.ConsensusNetwork) global.ConsensusNetwork.update(frame);
    if (canAppend && global.DetectionTimeline) global.DetectionTimeline.append(frame);
    else rebuildTimeline(index);

    const { scrubber, tickLabel } = els();
    if (scrubber) scrubber.value = index;
    if (tickLabel) {
      const lastTick = frames[frames.length - 1].tick;
      tickLabel.textContent = `tick ${frame.tick} / ${lastTick}  (frame ${index + 1}/${frames.length})`;
    }
  }

  function seekToIndex(index) {
    if (!frames.length) return;
    index = Math.max(0, Math.min(frames.length - 1, index));
    renderFrameAt(index);
  }

  function scheduleNext() {
    if (!playing) return;
    const delay = BASE_INTERVAL_MS / speed;
    playTimer = setTimeout(() => {
      if (currentIndex < frames.length - 1) {
        renderFrameAt(currentIndex + 1, true);
        scheduleNext();
      } else {
        pause();
      }
    }, delay);
  }

  function play() {
    if (!frames.length || playing) return;
    if (currentIndex >= frames.length - 1) currentIndex = -1; // replay from the start once it's run off the end
    playing = true;
    setState("playing");
    updatePlayButtonLabel();
    scheduleNext();
  }

  function pause() {
    playing = false;
    if (playTimer) { clearTimeout(playTimer); playTimer = null; }
    if (active) setState("paused");
    updatePlayButtonLabel();
  }

  function togglePlay() {
    if (playing) pause(); else play();
  }

  function stop() {
    pause();
    active = false;
    currentIndex = -1;
    frames = [];
    envHistory = [];
    header = null;
    setButtonsVisible(false);
  }

  function updatePlayButtonLabel() {
    const { playBtn } = els();
    if (playBtn) playBtn.textContent = playing ? "⏸ Pause" : "▶ Play";
  }

  function setSpeed(x) { speed = x; }

  // A team "has a detection" this frame if its mobility or comm tier is
  // ELEVATED, or the recovery manager marked it degraded.
  function isDetected(team) {
    const tier = team.consensus_tier || [-1, -1];
    return tier[0] === 2 || tier[1] === 2 || !!team.degraded;
  }

  // Jumps to the next tick, after the one currently shown, where any
  // team's detection state TRANSITIONS from not-detected to detected -
  // a transition, not merely "already showing detected", so this finds
  // the moment something actually happened rather than the next tick a
  // long-since-flagged team simply still shows as ELEVATED.
  function jumpToNextDetection() {
    if (!frames.length) return;
    const startIdx = Math.max(0, currentIndex);
    let prevFlags = (frames[startIdx].teams || []).map(isDetected);
    for (let i = startIdx + 1; i < frames.length; i++) {
      const teams = frames[i].teams || [];
      for (let t = 0; t < teams.length; t++) {
        if (isDetected(teams[t]) && !prevFlags[t]) {
          seekToIndex(i);
          return;
        }
      }
      prevFlags = teams.map(isDetected);
    }
    // No further detection event ahead - nothing to do, but the state
    // label at least confirms the search ran rather than looking stuck.
    setState(playing ? "playing" : "paused");
  }

  global.Replay = {
    isActive, refreshList, load, loadSelected, play, pause, togglePlay, stop,
    seekToIndex, setSpeed, jumpToNextDetection,
    get header() { return header; },
    get frameCount() { return frames.length; },
    get currentIndex() { return currentIndex; },
    get currentFile() { return currentFile; },
    // Frames 0..index-1 (not including `index`), for seeding the
    // dashboard's history when a live run takes over at `index`.
    framesBefore(index) { return frames.slice(0, Math.max(0, index)); },
    frameTick(i) { return frames[i] ? frames[i].tick : null; },
    // The building list as it stood at frame `i` (the latest STATICENV at or
    // before it), or null if there are none. The classic 2D / 2.5D map keeps
    // using the final one, as before; only the 3D view uses this.
    envAt(i) {
      let env = null;
      for (const h of envHistory) { if (h.index <= i) env = h.env; else break; }
      return env;
    },
  };
})(window);

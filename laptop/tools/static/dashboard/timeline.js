/*
 * laptop/tools/static/dashboard/timeline.js
 *
 * Detection timeline: one row per team, a strip of "injected"
 * ground-truth fault bars (by class, from fault_mask - this is the
 * fault injector's own state, see the big warning below) above a
 * strip of "detected" marks (consensus ELEVATED per class, plus the
 * recovery manager's `degraded` flag) below, so lag and misses between
 * the two strips are visible at a glance.
 *
 * fault_mask IS GROUND TRUTH. It is written by the fault injector,
 * the same code that decides what faults exist - it is not something
 * any detector observed or concluded. This file (and the legend it
 * renders) always labels its strip "injected (ground truth)" and
 * keeps it visually separate (a different vertical position, a
 * "ground truth" heading) from the "detected" strip below, which is
 * the only strip built from what the system actually decided
 * (consensus_tier, degraded). Never merge these into one signal - the
 * entire point of this panel is showing where they disagree.
 *
 * Fault class bits mirror core/fault_config.h's FaultClass_t:
 *   0 mobility, 1 communication, 2 resource, 3 environment, 4 mission.
 */
(function (global) {
  "use strict";

  const FAULT_CLASSES = [
    { bit: 0, name: "Mobility", color: "#bc8cff" },
    { bit: 1, name: "Communication", color: "#58a6ff" },
    { bit: 2, name: "Resource", color: "#d29922" },
    { bit: 3, name: "Environment", color: "#3fb950" },
    { bit: 4, name: "Mission", color: "#f778ba" },
  ];
  const RENDER_INTERVAL_MS = 100;
  const MAX_HISTORY = 600;
  const LABEL_W = 92;

  function create() {
    let canvas = null, ctx = null, legendEl = null, rangeEl = null;
    let history = []; // per-team array of {tick, fault_mask, tierMob, tierComm, degraded}
    let numTeams = 10;
    let lastRenderMs = 0;
    let teamColors = [];
    let specNames = [];
    let selected = null;
    let trackedRange = null; // {first, last} tick shown, same for every row - see draw()

    function ensureRows() {
      while (history.length < numTeams) history.push([]);
    }

    function recordTick(frame) {
      if (!frame || !frame.teams) return;
      numTeams = frame.teams.length;
      ensureRows();
      frame.teams.forEach((t, i) => {
        const row = history[i];
        const tierMob = (t.consensus_tier && t.consensus_tier[0] !== undefined) ? t.consensus_tier[0] : -1;
        const tierComm = (t.consensus_tier && t.consensus_tier[1] !== undefined) ? t.consensus_tier[1] : -1;
        row.push({
          tick: frame.tick,
          fault_mask: t.fault_mask || 0,
          tierMob, tierComm,
          degraded: !!t.degraded,
        });
        if (row.length > MAX_HISTORY) row.shift();
      });
    }

    function layout() {
      if (!canvas) return;
      const dims = DashboardCamera.resizeCanvasForDPI(canvas);
      ctx = dims.ctx;
      return dims;
    }

    // Row height is computed from the canvas's actual height / team
    // count rather than a fixed constant, so all `numTeams` rows are
    // ALWAYS visible without scrolling or clipping regardless of how
    // tall the panel's flex-allocated box ends up being (the one-screen
    // Live Ops layout gives this panel a fixed ~176px total, which at
    // 10 teams is a much shorter row than the original fixed 30px).
    // Every sub-element below (ground-truth strip, detected lanes, team
    // label font) scales off this the same way, so the row still reads
    // as "ground truth on top, detected below" at any size.
    function currentRowH(h) {
      return Math.max(6, h / Math.max(1, numTeams));
    }

    function draw() {
      if (!ctx) return;
      const rect = canvas.getBoundingClientRect();
      const w = rect.width, h = rect.height;
      ctx.clearRect(0, 0, w, h);

      const rowH = currentRowH(h);
      const plotW = w - LABEL_W;
      // pxPerTick scales to however many ticks are actually buffered so
      // far (up to MAX_HISTORY) - a fixed px-per-tick left most of the
      // width empty early in a run (e.g. only ~1/3 used at tick 105,
      // since a fixed 3px/tick only needs 315px of a much wider panel).
      // recordTick() already caps each row at MAX_HISTORY samples via
      // .shift(), so once a run passes that many ticks this settles into
      // a fixed-width sliding window exactly like before.
      const historyLen = Math.max(1, history.reduce((m, row) => Math.max(m, row.length), 0));
      const pxPerTick = plotW / historyLen;
      const barW = Math.max(1, pxPerTick + 0.5); // +0.5 covers sub-pixel rounding gaps between adjacent bars
      // Floor of 11px regardless of row height (projector readability -
      // UI pass item 10): the panel is sized (see .live-timeline-panel
      // in index.html) so 10 rows comfortably clear this floor, but the
      // floor comes first so a label never silently shrinks below it
      // again if that sizing assumption changes.
      const labelFont = Math.max(11, Math.min(13, rowH * 0.6));

      for (let i = 0; i < numTeams; i++) {
        const y = i * rowH;
        if (y > h) break;
        const row = history[i] || [];
        const isSel = selected === i;

        if (isSel) {
          ctx.fillStyle = "rgba(88,166,255,0.08)";
          ctx.fillRect(0, y, w, rowH);
        }

        ctx.fillStyle = teamColors[i % teamColors.length] || "#8b98a5";
        ctx.font = `${labelFont}px sans-serif`;
        ctx.textAlign = "left";
        ctx.textBaseline = "middle";
        ctx.fillText(`${(specNames[i % specNames.length] || "team")} #${i}`, 4, y + rowH / 2);

        const visible = row; // recordTick() already caps this at MAX_HISTORY - the full buffer IS the visible window
        // Ground-truth strip: top ~55% of the row. Detected strip
        // (3 thin lanes): remaining ~40%, with a hairline gap and the
        // row-separator line eating the last ~5%.
        const groundY = y + rowH * 0.06;
        const groundH = rowH * 0.5;
        const detectY = y + rowH * 0.62;
        const laneH = (rowH * 0.32) / 3;

        visible.forEach((sample, idx) => {
          const x = LABEL_W + idx * pxPerTick;
          // Ground-truth strip: most-significant active class wins the
          // pixel column (simultaneous classes are rare and this stays
          // legible at 1px resolution; the legend lists all 5 colors).
          let drawn = false;
          for (const cls of FAULT_CLASSES) {
            if (sample.fault_mask & (1 << cls.bit)) {
              ctx.fillStyle = cls.color;
              ctx.fillRect(x, groundY, barW, groundH);
              drawn = true;
              break;
            }
          }
          if (!drawn) {
            ctx.fillStyle = "rgba(139,152,165,0.08)";
            ctx.fillRect(x, groundY, barW, groundH);
          }

          // Detected strip: three thin lanes (mobility ELEVATED, comm
          // ELEVATED, degraded).
          ctx.fillStyle = "rgba(139,152,165,0.06)";
          ctx.fillRect(x, detectY, barW, laneH * 3);
          if (sample.tierMob === 2) {
            ctx.fillStyle = "#f85149";
            ctx.fillRect(x, detectY, barW, laneH);
          }
          if (sample.tierComm === 2) {
            ctx.fillStyle = "#f85149";
            ctx.fillRect(x, detectY + laneH, barW, laneH);
          }
          if (sample.degraded) {
            ctx.fillStyle = "#d29922";
            ctx.fillRect(x, detectY + laneH * 2, barW, laneH);
          }
        });

        ctx.strokeStyle = "rgba(139,152,165,0.15)";
        ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(0, y + rowH); ctx.lineTo(w, y + rowH); ctx.stroke();

        // The visible tick range is the same for every row (they all
        // share one x-axis), so it's drawn once, outside this canvas
        // entirely (see rangeEl below) - drawing it per row used to
        // stack ten overlapping copies against the canvas's right
        // edge, which read as visual clutter rather than a label.
        if (i === 0 && visible.length) trackedRange = { first: visible[0].tick, last: visible[visible.length - 1].tick };
      }
      if (rangeEl) {
        rangeEl.textContent = trackedRange ? `ticks ${trackedRange.first}–${trackedRange.last}` : "";
      }
    }

    function pickRow(evt) {
      const rect = canvas.getBoundingClientRect();
      const py = evt.clientY - rect.top;
      const rowH = currentRowH(rect.height);
      const i = Math.floor(py / rowH);
      return (i >= 0 && i < numTeams) ? i : null;
    }

    function handleClick(evt) {
      const i = pickRow(evt);
      if (i === null) return;
      selected = (selected === i) ? null : i;
      draw();
      if (global.DashboardSelection) {
        global.DashboardSelection.set(selected === null ? null : { type: "team", id: i });
      }
    }

    function renderLegend() {
      if (!legendEl) return;
      const classChips = FAULT_CLASSES.map((c) =>
        `<span style="display:inline-flex;align-items:center;gap:4px;margin-right:10px;">
           <span style="width:9px;height:9px;background:${c.color};display:inline-block;border-radius:2px;"></span>${c.name}
         </span>`).join("");
      legendEl.innerHTML = `
        <div style="margin-bottom:4px;"><b>Top strip — injected (ground truth):</b> ${classChips}</div>
        <div><b>Bottom strip — detected:</b>
          <span style="display:inline-flex;align-items:center;gap:4px;margin-right:10px;"><span style="width:9px;height:9px;background:#f85149;display:inline-block;border-radius:2px;"></span>mobility ELEVATED</span>
          <span style="display:inline-flex;align-items:center;gap:4px;margin-right:10px;"><span style="width:9px;height:9px;background:#f85149;display:inline-block;border-radius:2px;"></span>comm ELEVATED</span>
          <span style="display:inline-flex;align-items:center;gap:4px;"><span style="width:9px;height:9px;background:#d29922;display:inline-block;border-radius:2px;"></span>degraded</span>
        </div>`;
    }

    function init(canvasEl, legendElArg, colors, specs, rangeElArg) {
      canvas = canvasEl;
      legendEl = legendElArg;
      rangeEl = rangeElArg || null;
      teamColors = colors || [];
      specNames = specs || [];
      layout();
      canvas.addEventListener("click", handleClick);
      // See consensus-network.js's init() for why this is a
      // ResizeObserver rather than a window "resize" listener.
      if (window.ResizeObserver) {
        new ResizeObserver(() => { layout(); draw(); }).observe(canvas);
      } else {
        window.addEventListener("resize", () => { layout(); draw(); });
      }
      renderLegend();
    }

    function update(frame) {
      recordTick(frame);
      const now = performance.now();
      if (now - lastRenderMs >= RENDER_INTERVAL_MS) {
        draw();
        lastRenderMs = now;
      }
    }

    function reset() {
      history = [];
      selected = null;
      trackedRange = null;
      if (rangeEl) rangeEl.textContent = "";
      ensureRows();
      if (ctx && canvas) {
        const rect = canvas.getBoundingClientRect();
        ctx.clearRect(0, 0, rect.width, rect.height);
      }
    }

    return { init, update, reset, layout: () => { layout(); draw(); } };
  }

  global.DetectionTimeline = create();
})(window);

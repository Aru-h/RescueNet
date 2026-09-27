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
  const PX_PER_TICK = 3;
  const ROW_H = 30;
  const LABEL_W = 92;

  function create() {
    let canvas = null, ctx = null, legendEl = null;
    let history = []; // per-team array of {tick, fault_mask, tierMob, tierComm, degraded}
    let numTeams = 10;
    let lastRenderMs = 0;
    let teamColors = [];
    let specNames = [];
    let selected = null;

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

    function draw() {
      if (!ctx) return;
      const rect = canvas.getBoundingClientRect();
      const w = rect.width, h = rect.height;
      ctx.clearRect(0, 0, w, h);

      const plotW = w - LABEL_W;
      const visibleTicks = Math.max(1, Math.floor(plotW / PX_PER_TICK));

      for (let i = 0; i < numTeams; i++) {
        const y = i * ROW_H;
        if (y > h) break;
        const row = history[i] || [];
        const isSel = selected === i;

        if (isSel) {
          ctx.fillStyle = "rgba(88,166,255,0.08)";
          ctx.fillRect(0, y, w, ROW_H);
        }

        ctx.fillStyle = teamColors[i % teamColors.length] || "#8b98a5";
        ctx.font = "11px sans-serif";
        ctx.textAlign = "left";
        ctx.textBaseline = "middle";
        ctx.fillText(`${(specNames[i % specNames.length] || "team")} #${i}`, 4, y + ROW_H / 2);

        const visible = row.slice(-visibleTicks);
        const groundY = y + 4;
        const groundH = 10;
        const detectY = y + 17;
        const laneH = 3.2;

        visible.forEach((sample, idx) => {
          const x = LABEL_W + idx * PX_PER_TICK;
          // Ground-truth strip: most-significant active class wins the
          // pixel column (simultaneous classes are rare and this stays
          // legible at 1px resolution; the legend lists all 5 colors).
          let drawn = false;
          for (const cls of FAULT_CLASSES) {
            if (sample.fault_mask & (1 << cls.bit)) {
              ctx.fillStyle = cls.color;
              ctx.fillRect(x, groundY, PX_PER_TICK, groundH);
              drawn = true;
              break;
            }
          }
          if (!drawn) {
            ctx.fillStyle = "rgba(139,152,165,0.08)";
            ctx.fillRect(x, groundY, PX_PER_TICK, groundH);
          }

          // Detected strip: three thin lanes (mobility ELEVATED, comm
          // ELEVATED, degraded).
          ctx.fillStyle = "rgba(139,152,165,0.06)";
          ctx.fillRect(x, detectY, PX_PER_TICK, laneH * 3);
          if (sample.tierMob === 2) {
            ctx.fillStyle = "#f85149";
            ctx.fillRect(x, detectY, PX_PER_TICK, laneH);
          }
          if (sample.tierComm === 2) {
            ctx.fillStyle = "#f85149";
            ctx.fillRect(x, detectY + laneH, PX_PER_TICK, laneH);
          }
          if (sample.degraded) {
            ctx.fillStyle = "#d29922";
            ctx.fillRect(x, detectY + laneH * 2, PX_PER_TICK, laneH);
          }
        });

        ctx.strokeStyle = "rgba(139,152,165,0.15)";
        ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(0, y + ROW_H); ctx.lineTo(w, y + ROW_H); ctx.stroke();

        if (visible.length) {
          const lastTick = visible[visible.length - 1].tick;
          const firstTick = visible[0].tick;
          ctx.fillStyle = "rgba(139,152,165,0.6)";
          ctx.font = "9px sans-serif";
          ctx.textAlign = "right";
          ctx.fillText(`${firstTick}–${lastTick}`, w - 2, y + 8);
        }
      }
    }

    function pickRow(evt) {
      const rect = canvas.getBoundingClientRect();
      const py = evt.clientY - rect.top;
      const i = Math.floor(py / ROW_H);
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

    function init(canvasEl, legendElArg, colors, specs) {
      canvas = canvasEl;
      legendEl = legendElArg;
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

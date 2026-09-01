// bench-common.js
// Shared data layer + canvas map renderer used by all three test-bench UIs.
// Each bench passes its own color scheme; this file never hardcodes visual style.

const TEAM_SPEC_NAMES = ["Search", "Medical", "Extraction", "Perimeter", "Support"];
const STATUS_NAMES = ["Available", "En Route", "Working", "Resting", "Degraded"];

const Bench = {
  staticEnv: null,
  lastFrame: null,
  history: [],

  async fetchStatus() {
    const r = await fetch("/api/status");
    return r.json();
  },

  async fetchHistory() {
    const r = await fetch("/api/history");
    this.history = await r.json();
    return this.history;
  },

  async runBatch(params, onDone) {
    const r = await fetch("/api/run", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(params),
    });
    const data = await r.json();
    onDone(data);
    return data;
  },

  // Streams a live run via SSE. Calls onFrame(frame) per tick, onDone(payload) at the end.
  startLive(params, { onStaticEnv, onFrame, onDiagLine, onDone, onError }) {
    fetch("/api/run-live", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(params),
    }).then(async (resp) => {
      const reader = resp.body.getReader();
      const decoder = new TextDecoder();
      let buf = "";
      while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += decoder.decode(value, { stream: true });
        const chunks = buf.split("\n\n");
        buf = chunks.pop();
        for (const chunk of chunks) {
          const line = chunk.replace(/^data:\s*/, "");
          if (!line) continue;
          let msg;
          try { msg = JSON.parse(line); } catch { continue; }
          if (msg.type === "static_env") { this.staticEnv = msg.env; onStaticEnv && onStaticEnv(msg.env); }
          else if (msg.type === "frame") { this.lastFrame = msg.frame; onFrame && onFrame(msg.frame); }
          else if (msg.type === "diag_line") { onDiagLine && onDiagLine(msg.line); }
          else if (msg.type === "error") { onError && onError(msg); }
          else if (msg.type === "done") { onDone && onDone(msg); }
        }
      }
    }).catch((e) => onError && onError({ message: String(e) }));
  },

  // Draws the region grid + buildings + teams + incidents + safezones onto a canvas.
  // colors: { grid, bg, building, buildingHazard, road, safezone, incidentCrit, incidentLow, team, teamResting, epicenter, text }
  renderMap(canvas, colors, gridSize = 36) {
    const ctx = canvas.getContext("2d");
    const dpr = window.devicePixelRatio || 1;
    const size = Math.min(canvas.clientWidth, canvas.clientHeight || canvas.clientWidth);
    canvas.width = size * dpr;
    canvas.height = size * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const cell = size / gridSize;

    ctx.fillStyle = colors.bg;
    ctx.fillRect(0, 0, size, size);

    ctx.strokeStyle = colors.grid;
    ctx.lineWidth = 1;
    for (let i = 0; i <= gridSize; i++) {
      ctx.beginPath(); ctx.moveTo(i * cell, 0); ctx.lineTo(i * cell, size); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(0, i * cell); ctx.lineTo(size, i * cell); ctx.stroke();
    }

    const env = this.staticEnv;
    if (env && env.buildings) {
      for (const b of env.buildings) {
        ctx.fillStyle = (b.hazard_flags && b.hazard_flags !== 0) ? colors.buildingHazard : colors.building;
        ctx.fillRect(b.x * cell, b.y * cell, Math.max(b.w * cell, 1), Math.max(b.h * cell, 1));
      }
    }

    const frame = this.lastFrame;
    if (frame) {
      if (frame.safezones) {
        for (const s of frame.safezones) {
          ctx.strokeStyle = colors.safezone;
          ctx.lineWidth = 2;
          ctx.strokeRect(s.x * cell - 2, s.y * cell - 2, cell + 4, cell + 4);
        }
      }
      if (frame.incidents) {
        for (const inc of frame.incidents) {
          ctx.fillStyle = (inc.priority >= 2) ? colors.incidentCrit : colors.incidentLow;
          ctx.beginPath();
          ctx.arc(inc.x * cell + cell / 2, inc.y * cell + cell / 2, Math.max(cell * 0.4, 3), 0, Math.PI * 2);
          ctx.fill();
        }
      }
      if (frame.teams) {
        for (const t of frame.teams) {
          ctx.fillStyle = (t.status === 3) ? colors.teamResting : colors.team;
          ctx.beginPath();
          ctx.arc(t.x * cell + cell / 2, t.y * cell + cell / 2, Math.max(cell * 0.32, 2.5), 0, Math.PI * 2);
          ctx.fill();
          if (t.status === 3) {
            ctx.strokeStyle = colors.teamResting;
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            ctx.arc(t.x * cell + cell / 2, t.y * cell + cell / 2, Math.max(cell * 0.32, 2.5) + 3, 0, Math.PI * 2);
            ctx.stroke();
          }
        }
      }
    }
  },

  teamName(t) {
    return `${TEAM_SPEC_NAMES[t.spec % TEAM_SPEC_NAMES.length]} #${t.id}`;
  },
  statusName(s) {
    return STATUS_NAMES[s] || `Status ${s}`;
  },
};

// bench-common.js
// Shared data layer + canvas map renderer used by all three test-bench UIs.
// Each bench passes its own color scheme; this file never hardcodes visual style.
//
// Enum values below are copied directly from core/mission_state.h — do not guess at these,
// they've been wrong once already (TeamStatus_t has no "Degraded" value, and Priority_t is
// 1-indexed with 1 = CRITICAL, not 0-indexed with higher = worse).

const TEAM_SPEC_NAMES = ["Search", "Medical", "Extraction", "Perimeter", "Support"];
const TEAM_CALLSIGNS = ["Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel", "India", "Juliett"];
const STATUS_NAMES = ["Available", "En Route", "On Scene", "Resting"]; // TeamStatus_t, 0-3

// Priority_t is 1-indexed: 1 = most severe. Index 0 is unused (kept so PRIORITY_NAMES[p] works directly).
const PRIORITY_NAMES = [null, "Critical", "High", "Medium", "Low"];
const PRIORITY_KEYS = [null, "critical", "high", "medium", "low"]; // for CSS classes

const INCIDENT_STATUS_NAMES = ["Open", "Assigned", "In Progress", "Resolved"]; // IncidentStatus_t, 0-3

const INCIDENT_TYPE_NAMES = [
  "Structural Collapse", "Confined Space", "Mass Casualty", "Chemical Exposure",
  "Unsearched Sector", "Survivor Signal", "Gas Leak", "Active Fire",
  "Equipment Cache", "Return To Safe Zone", "Evacuee Offboard",
]; // IncidentType_t, 0-10

// BuildingType_t, 0-10 (core/environment.h). Index 0 (NONE) is never actually drawn.
// Colors are deliberately desaturated, aerial-photo-plausible rooftop tones — not bright/decorative.
const BUILDING_TYPE_NAMES = [
  null, "Residential", "Apartment", "Commercial", "Office", "Industrial",
  "Medical", "Civic", "Education", "Parking", "Utility",
];
const BUILDING_TYPE_COLORS = [
  null, "#b8946b", "#8d7d70", "#6d7f8d", "#5c6c7d", "#7a7263",
  "#c7c2b8", "#9b9186", "#c2ab7c", "#8c8c88", "#63625d",
];

// Fixed light source for building drop-shadows: from the northwest, shadows fall southeast.
// Kept as a named constant rather than a magic number since buildStaticLayer() and any future
// per-building glow logic both need to agree on the same light direction.
const MAP_LIGHT_ANGLE = { dx: 1, dy: 1 };

// Small glyph per IncidentType_t category, drawn inside the white marker disc so incidents are
// distinguishable by shape (not just color) at a glance — grouped by what a responder would
// actually treat similarly, not a 1:1 icon per type (11 bespoke icons wasn't worth the complexity
// this pass; the color+shape combination already carries the important information).
function drawIncidentGlyph(ctx, type, cx, cy, r, color) {
  ctx.fillStyle = color;
  ctx.strokeStyle = color;
  ctx.lineWidth = Math.max(r * 0.28, 1);
  if (type === 7 || type === 6) {
    // Active Fire / Gas Leak — flame-like teardrop
    ctx.beginPath();
    ctx.moveTo(cx, cy - r);
    ctx.quadraticCurveTo(cx + r * 0.9, cy - r * 0.2, cx, cy + r);
    ctx.quadraticCurveTo(cx - r * 0.9, cy - r * 0.2, cx, cy - r);
    ctx.fill();
  } else if (type === 2 || type === 3) {
    // Mass Casualty / Chemical Exposure — medical cross
    const w = r * 0.42;
    ctx.fillRect(cx - w / 2, cy - r, w, r * 2);
    ctx.fillRect(cx - r, cy - w / 2, r * 2, w);
  } else if (type === 4 || type === 5) {
    // Unsearched Sector / Survivor Signal — target rings
    ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.stroke();
    ctx.beginPath(); ctx.arc(cx, cy, r * 0.42, 0, Math.PI * 2); ctx.fill();
  } else if (type === 0 || type === 1) {
    // Structural Collapse / Confined Space — warning triangle
    ctx.beginPath();
    ctx.moveTo(cx, cy - r); ctx.lineTo(cx + r * 0.95, cy + r * 0.8); ctx.lineTo(cx - r * 0.95, cy + r * 0.8);
    ctx.closePath(); ctx.fill();
  } else {
    // Equipment Cache / Return To Safe Zone / Evacuee Offboard — logistics square
    ctx.fillRect(cx - r * 0.75, cy - r * 0.75, r * 1.5, r * 1.5);
  }
}

const Bench = {
  staticEnv: null,
  lastFrame: null,
  history: [],
  hitTargets: [],  // rebuilt every renderMap() call: [{type:'team'|'incident', data, cx, cy, r}]

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

  async pauseLive() {
    const r = await fetch("/api/run-live/pause", { method: "POST" });
    return r.json();
  },
  async resumeLive() {
    const r = await fetch("/api/run-live/resume", { method: "POST" });
    return r.json();
  },
  async stopLive() {
    const r = await fetch("/api/run-live/stop", { method: "POST" });
    return r.json();
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
  // colors: { grid, bg, building, buildingHazard, road, safezone, incidentCrit, incidentLow, team,
  //           teamResting, epicenter, text, selectRing (optional, defaults to '#000') }
  // selected: optional { type: 'team'|'incident', id } — draws a highlight ring around that marker,
  // used to cross-highlight a map marker when its row is clicked in a table (or vice versa).
  renderMap(canvas, colors, gridSize = 36, selected = null) {
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

    this.hitTargets = [];
    this.cellSize = cell;

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
          const cx = inc.x * cell + cell / 2, cy = inc.y * cell + cell / 2, r = Math.max(cell * 0.4, 3);
          // Priority_t is 1=CRITICAL .. 4=LOW (lower number = more severe). Critical+High -> red tier,
          // Medium+Low -> amber tier. (Previously inverted here — priority >= 2 was treated as "critical",
          // which is backwards.)
          ctx.fillStyle = (inc.priority <= 2) ? colors.incidentCrit : colors.incidentLow;
          ctx.beginPath();
          ctx.arc(cx, cy, r, 0, Math.PI * 2);
          ctx.fill();
          this.hitTargets.push({ type: "incident", data: inc, cx, cy, r: Math.max(r, cell * 0.5) });
        }
      }
      if (frame.teams) {
        for (const t of frame.teams) {
          const cx = t.x * cell + cell / 2, cy = t.y * cell + cell / 2, r = Math.max(cell * 0.32, 2.5);
          ctx.fillStyle = (t.status === 3) ? colors.teamResting : colors.team;
          ctx.beginPath();
          ctx.arc(cx, cy, r, 0, Math.PI * 2);
          ctx.fill();
          if (t.status === 3) {
            ctx.strokeStyle = colors.teamResting;
            ctx.lineWidth = 1.5;
            ctx.beginPath();
            ctx.arc(cx, cy, r + 3, 0, Math.PI * 2);
            ctx.stroke();
          }
          this.hitTargets.push({ type: "team", data: t, cx, cy, r: Math.max(r, cell * 0.5) });
        }
      }
    }

    if (selected) {
      const match = this.hitTargets.find((h) => h.type === selected.type && h.data.id === selected.id);
      if (match) {
        ctx.strokeStyle = colors.selectRing || "#000";
        ctx.lineWidth = 2.5;
        ctx.setLineDash([]);
        ctx.beginPath();
        ctx.arc(match.cx, match.cy, match.r + 6, 0, Math.PI * 2);
        ctx.stroke();
      }
    }
  },

  // Builds (or rebuilds) the static aerial-style base layer — ground texture + buildings with
  // rooftop shading, drop shadows, and damage/hazard tinting — onto an offscreen canvas at a fixed
  // reference resolution. This only needs to run when `staticEnv` actually changes (once at run
  // start, plus again on any aftershock-triggered re-emit — see laptop/main.c), NOT every tick,
  // since buildings never move and LIVEFRAME carries no per-tick building data at all.
  //
  // Building drop-shadow technique adapted from the CC0 2D top-down shadow-casting approach
  // described at https://godotshaders.com/shader/2d-top-down-shadows/ (fixed light angle +
  // object-height-proportional offset) — the source there is a Godot shader; the offset math is
  // reused here, the rest (rooftop coloring, damage/hazard tinting, texture) is original.
  buildStaticLayer(colors, gridSize = 36, refSize = 900) {
    const off = document.createElement("canvas");
    off.width = refSize;
    off.height = refSize;
    const ctx = off.getContext("2d");
    const cell = refSize / gridSize;

    ctx.fillStyle = colors.bg;
    ctx.fillRect(0, 0, refSize, refSize);

    // Cheap procedural ground speckle so open ground reads as aerial-photo texture rather than a
    // flat fill. One-time cost since this whole layer is cached.
    ctx.fillStyle = colors.groundSpeckle || "rgba(0,0,0,0.03)";
    const speckleCount = Math.floor((refSize * refSize) / 140);
    for (let i = 0; i < speckleCount; i++) {
      const x = Math.random() * refSize, y = Math.random() * refSize;
      const s = 1 + Math.random() * 1.4;
      ctx.fillRect(x, y, s, s);
    }

    ctx.strokeStyle = colors.grid;
    ctx.lineWidth = 1;
    ctx.globalAlpha = 0.5;
    for (let i = 0; i <= gridSize; i++) {
      ctx.beginPath(); ctx.moveTo(i * cell, 0); ctx.lineTo(i * cell, refSize); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(0, i * cell); ctx.lineTo(refSize, i * cell); ctx.stroke();
    }
    ctx.globalAlpha = 1;

    const env = this.staticEnv;
    if (env && env.buildings) {
      // Pass 1: drop shadows, offset toward the southeast, scaled by building height (floors).
      for (const b of env.buildings) {
        if (b.type === 0) continue; // BUILDING_NONE
        const bw = Math.max(b.w * cell, 1), bh = Math.max(b.h * cell, 1);
        const floors = Math.max(b.floors || 1, 1);
        const offsetMag = Math.min(cell * 0.16 * Math.min(floors, 5), cell * 0.75);
        ctx.fillStyle = "rgba(8,12,10,0.28)";
        ctx.fillRect(
          b.x * cell + offsetMag * MAP_LIGHT_ANGLE.dx,
          b.y * cell + offsetMag * MAP_LIGHT_ANGLE.dy,
          bw, bh
        );
      }
      // Pass 2: rooftops, colored by real BuildingType_t, with parapet edge + damage/hazard tint.
      for (const b of env.buildings) {
        if (b.type === 0) continue;
        const bx = b.x * cell, by = b.y * cell;
        const bw = Math.max(b.w * cell, 1), bh = Math.max(b.h * cell, 1);
        ctx.fillStyle = BUILDING_TYPE_COLORS[b.type] || BUILDING_TYPE_COLORS[1];
        ctx.fillRect(bx, by, bw, bh);

        ctx.strokeStyle = "rgba(0,0,0,0.28)";
        ctx.lineWidth = 1;
        ctx.strokeRect(bx + 0.5, by + 0.5, bw - 1, bh - 1);

        if (bw > cell * 1.5 && bh > cell * 1.5) {
          ctx.strokeStyle = "rgba(0,0,0,0.12)";
          ctx.beginPath(); ctx.moveTo(bx + bw / 2, by); ctx.lineTo(bx + bw / 2, by + bh); ctx.stroke();
        }

        // damage_state (0-100) -> a proportional dark rubble/scorch overlay, never fully opaque
        // so the base rooftop color still reads through even at 100% damage.
        const dmg = Math.max(0, Math.min(100, b.damage || 0));
        if (dmg > 0) {
          ctx.fillStyle = `rgba(32,24,18,${((dmg / 100) * 0.55).toFixed(3)})`;
          ctx.fillRect(bx, by, bw, bh);
        }
        if (dmg >= 75) {
          ctx.strokeStyle = "rgba(18,12,8,0.5)";
          ctx.lineWidth = Math.max(bw, bh) * 0.045;
          ctx.beginPath(); ctx.moveTo(bx, by); ctx.lineTo(bx + bw, by + bh);
          ctx.moveTo(bx + bw, by); ctx.lineTo(bx, by + bh); ctx.stroke();
        }

        // Static (non-animated) hazard tint baked into the cached layer; the pulsing glow version
        // is drawn per-tick in renderMapV2 so it can actually animate.
        if (b.hazard_flags & 1) ctx.fillStyle = "rgba(255,90,20,0.09)";       // ENV_HAZARD_FIRE
        else if (b.hazard_flags & 2) ctx.fillStyle = "rgba(160,200,40,0.09)"; // ENV_HAZARD_GAS
        else if (b.hazard_flags & 4) ctx.fillStyle = "rgba(150,90,200,0.09)"; // ENV_HAZARD_COLLAPSE_RISK
        else ctx.fillStyle = null;
        if (ctx.fillStyle) ctx.fillRect(bx, by, bw, bh);
      }
    }

    this._staticLayerCanvas = off;
    this._staticLayerEnvRef = env;
    this._staticLayerGridSize = gridSize;
  },

  // Satellite/tac-map style renderer: blits the cached static base layer (rebuilding it first if
  // `staticEnv` has changed since the cache was built — see buildStaticLayer) then draws the
  // per-tick dynamic layer (safezones, incident/team markers, hazard pulse, selection ring) at
  // full canvas resolution on top of it. hitTargets/hitTest contract is identical to renderMap().
  renderMapV2(canvas, colors, gridSize = 36, selected = null) {
    const ctx = canvas.getContext("2d");
    const dpr = window.devicePixelRatio || 1;
    const size = Math.min(canvas.clientWidth, canvas.clientHeight || canvas.clientWidth);
    canvas.width = size * dpr;
    canvas.height = size * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);

    if (this.staticEnv !== this._staticLayerEnvRef || !this._staticLayerCanvas || this._staticLayerGridSize !== gridSize) {
      this.buildStaticLayer(colors, gridSize);
    }
    ctx.drawImage(this._staticLayerCanvas, 0, 0, size, size);

    const cell = size / gridSize;
    this.hitTargets = [];
    this.cellSize = cell;

    const frame = this.lastFrame;
    const tick = frame ? frame.tick : 0;

    const env = this.staticEnv;
    if (env && env.buildings) {
      const pulse = 0.35 + 0.25 * Math.sin(tick * 0.15);
      for (const b of env.buildings) {
        if (!b.hazard_flags) continue;
        const bx = b.x * cell, by = b.y * cell, bw = Math.max(b.w * cell, 1), bh = Math.max(b.h * cell, 1);
        const glow = (b.hazard_flags & 1) ? `rgba(255,90,20,${pulse.toFixed(3)})`
          : (b.hazard_flags & 2) ? `rgba(160,200,40,${pulse.toFixed(3)})`
          : `rgba(150,90,200,${pulse.toFixed(3)})`;
        ctx.strokeStyle = glow;
        ctx.lineWidth = 2;
        ctx.strokeRect(bx - 1, by - 1, bw + 2, bh + 2);
      }
    }

    if (frame) {
      if (frame.safezones) {
        frame.safezones.forEach((s, idx) => {
          // ASSUMPTION: falls back to array index if the payload has no real `id` field.
          // Confirm against the actual STATICENV/LIVEFRAME schema and remove this fallback
          // once a real safezone id (or lack thereof) is confirmed.
          if (s.id === undefined) s.id = idx;
          ctx.strokeStyle = colors.safezone;
          ctx.lineWidth = 2;
          const left = Math.max(s.x * cell - 2, 0);
          const top = Math.max(s.y * cell - 2, 0);
          const right = Math.min(s.x * cell + cell + 2, size);
          const bottom = Math.min(s.y * cell + cell + 2, size);
          ctx.strokeRect(left, top, right - left, bottom - top);
          const cx = s.x * cell + cell / 2, cy = s.y * cell + cell / 2;
          this.hitTargets.push({ type: "safezone", data: s, cx, cy, r: Math.max(cell * 0.6, 6) });
        });
      }
      if (frame.incidents) {
        for (const inc of frame.incidents) {
          const cx = inc.x * cell + cell / 2, cy = inc.y * cell + cell / 2;
          const baseR = Math.max(cell * 0.36, 4);
          // Priority_t: 1=CRITICAL is most severe. Critical pulses; High/Medium/Low get a static ring.
          const ringR = baseR + (inc.priority === 1 ? 2.5 * Math.abs(Math.sin(tick * 0.12)) : 0);
          const color = inc.priority <= 2 ? colors.incidentCrit : colors.incidentLow;

          ctx.globalAlpha = 0.5;
          ctx.strokeStyle = color;
          ctx.lineWidth = 1.5;
          ctx.beginPath(); ctx.arc(cx, cy, ringR + 4, 0, Math.PI * 2); ctx.stroke();
          ctx.globalAlpha = 1;

          ctx.fillStyle = "#fff";
          ctx.beginPath(); ctx.arc(cx, cy, baseR, 0, Math.PI * 2); ctx.fill();
          ctx.strokeStyle = color;
          ctx.lineWidth = 2;
          ctx.beginPath(); ctx.arc(cx, cy, baseR, 0, Math.PI * 2); ctx.stroke();
          drawIncidentGlyph(ctx, inc.type, cx, cy, baseR * 0.55, color);

          this.hitTargets.push({ type: "incident", data: inc, cx, cy, r: Math.max(baseR, cell * 0.5) });
        }
      }
      if (frame.teams) {
        for (const t of frame.teams) {
          const cx = t.x * cell + cell / 2, cy = t.y * cell + cell / 2, r = Math.max(cell * 0.3, 3);
          const color = (t.status === 3) ? colors.teamResting : colors.team;

          ctx.fillStyle = "#fff";
          ctx.beginPath(); ctx.arc(cx, cy, r + 1.5, 0, Math.PI * 2); ctx.fill();
          ctx.fillStyle = color;
          ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.fill();
          if (t.status === 3) {
            ctx.strokeStyle = color;
            ctx.lineWidth = 1.5;
            ctx.beginPath(); ctx.arc(cx, cy, r + 3, 0, Math.PI * 2); ctx.stroke();
          }
          // Heading wedge toward the team's current destination, if it has one.
          if (t.has_dest) {
            const ang = Math.atan2(t.dest_y - t.y, t.dest_x - t.x);
            const wx = cx + Math.cos(ang) * (r + 5), wy = cy + Math.sin(ang) * (r + 5);
            const perp = ang + Math.PI / 2;
            ctx.fillStyle = color;
            ctx.beginPath();
            ctx.moveTo(wx, wy);
            ctx.lineTo(cx + Math.cos(perp) * 2.5, cy + Math.sin(perp) * 2.5);
            ctx.lineTo(cx - Math.cos(perp) * 2.5, cy - Math.sin(perp) * 2.5);
            ctx.closePath();
            ctx.fill();
          }
          this.hitTargets.push({ type: "team", data: t, cx, cy, r: Math.max(r, cell * 0.5) });
        }
      }
    }

    if (selected) {
      const match = this.hitTargets.find((h) => h.type === selected.type && h.data.id === selected.id);
      if (match) {
        ctx.strokeStyle = colors.selectRing || "#000";
        ctx.lineWidth = 2.5;
        ctx.setLineDash([]);
        ctx.beginPath(); ctx.arc(match.cx, match.cy, match.r + 6, 0, Math.PI * 2); ctx.stroke();
      }
    }
  },

  // Finds the nearest hit target of a given type within its radius (+ a few px tolerance) of (px, py),
  // where px/py are mouse coordinates relative to the canvas element in CSS pixels (e.g. from a
  // 'mousedown'/'mouseup' listener using evt.offsetX/evt.offsetY on the canvas).
  hitTest(px, py, type) {
    let best = null, bestDist = Infinity;
    for (const h of this.hitTargets) {
      if (type && h.type !== type) continue;
      const d = Math.hypot(px - h.cx, py - h.cy);
      if (d <= h.r + 4 && d < bestDist) { best = h; bestDist = d; }
    }
    return best;
  },

  teamName(t) {
    return TEAM_CALLSIGNS[t.id % TEAM_CALLSIGNS.length];
  },
  teamSpec(t) {
    return TEAM_SPEC_NAMES[t.spec % TEAM_SPEC_NAMES.length];
  },
  statusName(s) {
    return STATUS_NAMES[s] || `Status ${s}`;
  },
  priorityName(p) {
    return PRIORITY_NAMES[p] || `Priority ${p}`;
  },
  priorityKey(p) {
    return PRIORITY_KEYS[p] || "low";
  },
  incidentStatusName(s) {
    return INCIDENT_STATUS_NAMES[s] || `Status ${s}`;
  },
  incidentTypeName(t) {
    return INCIDENT_TYPE_NAMES[t] || `Type ${t}`;
  },
};
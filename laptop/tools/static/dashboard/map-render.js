/*
 * laptop/tools/static/dashboard/map-render.js
 *
 * Layer-drawing functions for the Live Ops region map, factored out
 * of index.html's inline drawFrame() so that function stays a short
 * orchestrator: build a camera-aware hit-target list, hand each layer
 * to the matching draw* function here, collect its hits. All world
 * <-> screen math goes through the camera object from camera.js - no
 * function here multiplies by a fixed CELL constant, so panning,
 * zooming and window resizes all fall out of the one camera object
 * instead of needing to be re-derived per layer.
 *
 * Every draw* function takes (ctx, view, ...data) where `view` is the
 * object camera.js's createCamera() returns (worldToScreen/
 * screenToWorld/state), and returns an array of hit targets:
 *   { type, id, wx, wy, wr, data }
 * wx/wy/wr are WORLD-space center and radius, not screen space, so
 * hit-testing (see hitTestWorld below) stays correct at any zoom
 * level without needing to redo it per frame.
 */
(function (global) {
  "use strict";

  const PRIORITY_COLORS = { 1: "#f85149", 2: "#d29922", 3: "#58a6ff", 4: "#8b98a5" };
  const INCIDENT_ICONS = { 6: "☢", 7: "🔥" };
  const STATUS_RING_COLORS = {
    0: "rgba(63,185,80,0.9)",   // AVAILABLE
    1: "rgba(88,166,255,0.9)",  // EN_ROUTE
    2: "rgba(210,153,34,0.9)",  // ON_SCENE
    3: "rgba(139,152,165,0.9)", // RESTING
  };

  function drawGrid(ctx, view, gridN) {
    const w = view.state.viewW, h = view.state.viewH;
    ctx.save();
    ctx.strokeStyle = "#182028";
    ctx.lineWidth = 1;
    for (let i = 0; i <= gridN; i++) {
      const a = view.worldToScreen(i, 0);
      const b = view.worldToScreen(i, gridN);
      ctx.beginPath(); ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y); ctx.stroke();
      const c = view.worldToScreen(0, i);
      const d = view.worldToScreen(gridN, i);
      ctx.beginPath(); ctx.moveTo(c.x, c.y); ctx.lineTo(d.x, d.y); ctx.stroke();
    }
    ctx.restore();
    void w; void h;
  }

  // Buildings: shaded by floor count, tinted by damage, with a small
  // floor-count label when the building is drawn big enough (i.e.
  // zoomed in enough) to read it.
  function drawBuildings(ctx, view, env) {
    if (!env || !env.buildings) return;
    const px1 = view.worldToScreen(0, 0), px2 = view.worldToScreen(1, 0);
    const pxPerUnit = Math.abs(px2.x - px1.x) || 1;

    env.buildings.forEach((b) => {
      const wW = Math.max(1, b.w) * 0.9;
      const wH = Math.max(1, b.h) * 0.9;
      const topLeft = view.worldToScreen(b.x - wW / 2, b.y - wH / 2);
      const size = { w: wW * pxPerUnit, h: wH * pxPerUnit };

      const damageFrac = Math.min(1, (b.damage || 0) / 255);
      const floors = Math.max(1, b.floors || 1);
      const floorShade = Math.min(1, floors / 10); // taller -> a bit darker/denser fill
      const baseAlpha = 0.05 + floorShade * 0.10;

      ctx.fillStyle = damageFrac > 0.15
        ? `rgba(248,81,73,${(0.08 + damageFrac * 0.35).toFixed(3)})`
        : `rgba(139,152,165,${baseAlpha.toFixed(3)})`;
      ctx.strokeStyle = damageFrac > 0.5 ? "rgba(248,81,73,0.55)" : "rgba(139,152,165,0.22)";
      ctx.lineWidth = 1;
      ctx.fillRect(topLeft.x, topLeft.y, size.w, size.h);
      ctx.strokeRect(topLeft.x, topLeft.y, size.w, size.h);

      if (pxPerUnit >= 10 && size.w >= 16 && size.h >= 12) {
        ctx.fillStyle = "rgba(230,237,243,0.55)";
        ctx.font = `${Math.max(8, Math.min(11, pxPerUnit * 0.5))}px sans-serif`;
        ctx.textAlign = "left";
        ctx.textBaseline = "top";
        ctx.fillText(`${floors}F`, topLeft.x + 2, topLeft.y + 1);
      }
    });
  }

  function drawEpicenter(ctx, view, env) {
    if (!env || !env.epicenter || !env.epicenter.active) return;
    const ep = env.epicenter;
    const center = view.worldToScreen(ep.x, ep.y);
    const r = view.state.zoom * 1.2;
    ctx.save();
    ctx.beginPath();
    ctx.arc(center.x, center.y, r, 0, Math.PI * 2);
    ctx.strokeStyle = "rgba(248,81,73,0.35)";
    ctx.lineWidth = 1.5;
    ctx.setLineDash([4, 3]);
    ctx.stroke();
    ctx.restore();
  }

  function drawHazardCells(ctx, view, cells) {
    if (!cells) return;
    const px1 = view.worldToScreen(0, 0), px2 = view.worldToScreen(1, 0);
    const cellPx = Math.abs(px2.x - px1.x) || 1;
    cells.forEach((c) => {
      if (c.blocked) ctx.fillStyle = "rgba(248,81,73,0.28)";
      else if (c.hazard_flags) ctx.fillStyle = "rgba(210,153,34,0.20)";
      else ctx.fillStyle = "rgba(210,153,34,0.10)";
      const p = view.worldToScreen(c.x - 0.5, c.y - 0.5);
      ctx.fillRect(p.x, p.y, cellPx, cellPx);
    });
  }

  function drawSafezones(ctx, view, list) {
    const hits = [];
    (list || []).forEach((sz) => {
      if (sz.capacity === 0) return;
      const half = 0.42;
      const topLeft = view.worldToScreen(sz.x - half, sz.y - half);
      const size = view.state.zoom * half * 2;
      ctx.strokeStyle = "#3fb950";
      ctx.lineWidth = 1.5;
      ctx.strokeRect(topLeft.x, topLeft.y, size, size);
      hits.push({ type: "safezone", id: sz.id, wx: sz.x, wy: sz.y, wr: 0.5, data: sz });
    });
    return hits;
  }

  // Incidents sized by priority (P1 largest) and victim count, with a
  // small floor label when the incident is inside a building.
  function drawIncidents(ctx, view, list, opts) {
    const hits = [];
    const fogViewMode = opts.fogViewMode;
    const selectedMarker = opts.selectedMarker;
    const priorityScale = { 1: 1.25, 2: 1.05, 3: 0.9, 4: 0.75 };

    (list || []).forEach((inc) => {
      if (inc.status === 3) return; // resolved
      if (fogViewMode === "operator" && inc.discovered === 0) return;

      const color = PRIORITY_COLORS[inc.priority] || "#8b98a5";
      const pScale = priorityScale[inc.priority] || 0.85;
      const worldSize = Math.min(0.9, (0.32 + inc.victims * 0.045)) * pScale;
      const center = view.worldToScreen(inc.x, inc.y);
      const sizePx = worldSize * view.state.zoom;

      ctx.fillStyle = color;
      ctx.globalAlpha = inc.status === 0 ? 1 : 0.55;
      ctx.fillRect(center.x - sizePx / 2, center.y - sizePx / 2, sizePx, sizePx);
      ctx.globalAlpha = 1;

      const icon = INCIDENT_ICONS[inc.type];
      if (icon && sizePx >= 6) {
        ctx.font = `${Math.max(10, sizePx * 0.9)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(icon, center.x, center.y - sizePx / 2 - 7);
      }

      // Victims + floor label, only when zoomed in enough to be legible.
      if (view.state.zoom >= 12) {
        const bits = [];
        if (inc.victims) bits.push(`${inc.victims}v`);
        if (inc.building_id !== undefined && inc.building_id >= 0) bits.push(`F${inc.floor}`);
        if (bits.length) {
          ctx.fillStyle = "rgba(230,237,243,0.75)";
          ctx.font = "9px sans-serif";
          ctx.textAlign = "center";
          ctx.textBaseline = "top";
          ctx.fillText(bits.join(" "), center.x, center.y + sizePx / 2 + 2);
        }
      }

      if (selectedMarker && selectedMarker.type === "incident" && selectedMarker.id === inc.id) {
        ctx.strokeStyle = "#fff";
        ctx.lineWidth = 2;
        ctx.strokeRect(center.x - sizePx / 2 - 3, center.y - sizePx / 2 - 3, sizePx + 6, sizePx + 6);
      }

      hits.push({ type: "incident", id: inc.id, wx: inc.x, wy: inc.y, wr: Math.max(worldSize / 2, 0.4), data: inc });
    });
    return hits;
  }

  // Teams as distinct numbered markers with a status ring and a line
  // to their destination.
  function drawTeams(ctx, view, list, opts) {
    const hits = [];
    const teamColors = opts.teamColors;
    const selectedMarker = opts.selectedMarker;
    const markerWorldR = 0.32;

    (list || []).forEach((t, i) => {
      const color = teamColors[i % teamColors.length];
      const center = view.worldToScreen(t.x, t.y);

      if (t.has_dest && (t.dest_x !== t.x || t.dest_y !== t.y)) {
        const dest = view.worldToScreen(t.dest_x, t.dest_y);
        ctx.beginPath();
        ctx.moveTo(center.x, center.y);
        ctx.lineTo(dest.x, dest.y);
        ctx.strokeStyle = color;
        ctx.globalAlpha = 0.35;
        ctx.lineWidth = 1;
        ctx.setLineDash([3, 3]);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.globalAlpha = 1;
      }

      const r = markerWorldR * view.state.zoom;

      // Status ring (always drawn - not just for resting like the old
      // single-status-only ring - so "status ring" reads as a real
      // per-status indicator rather than a resting-only special case).
      ctx.beginPath();
      ctx.arc(center.x, center.y, r * 1.55, 0, Math.PI * 2);
      ctx.strokeStyle = STATUS_RING_COLORS[t.status] || "rgba(139,152,165,0.9)";
      ctx.lineWidth = 2;
      if (t.status === 3) ctx.setLineDash([3, 2]); // resting: dashed
      ctx.stroke();
      ctx.setLineDash([]);

      ctx.beginPath();
      ctx.arc(center.x, center.y, r, 0, Math.PI * 2);
      ctx.fillStyle = color;
      ctx.fill();

      // Numbered marker.
      if (r >= 6) {
        ctx.fillStyle = "#04150a";
        ctx.font = `bold ${Math.max(8, r)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(String(t.id), center.x, center.y + 0.5);
      }

      if (t.fault_mask) {
        ctx.beginPath();
        ctx.arc(center.x + r * 1.1, center.y - r * 1.1, 3, 0, Math.PI * 2);
        ctx.fillStyle = "#f85149";
        ctx.fill();
      }

      if (selectedMarker && selectedMarker.type === "team" && selectedMarker.id === t.id) {
        ctx.beginPath();
        ctx.arc(center.x, center.y, r * 1.9, 0, Math.PI * 2);
        ctx.strokeStyle = "#fff";
        ctx.lineWidth = 2;
        ctx.stroke();
      }

      hits.push({ type: "team", id: t.id, wx: t.x, wy: t.y, wr: markerWorldR + 0.18, data: { ...t, i } });
    });
    return hits;
  }

  // World-space hit test: converts the screen point to world space
  // once, then compares against each target's world-space radius, so
  // hit-testing is correct at any zoom level without per-frame tuning.
  function hitTestWorld(view, hits, sx, sy, typeFilter) {
    const w = view.screenToWorld(sx, sy);
    let best = null, bestDist = Infinity;
    for (const h of hits) {
      if (typeFilter && h.type !== typeFilter) continue;
      const dx = h.wx - w.x, dy = h.wy - w.y;
      const dist = Math.sqrt(dx * dx + dy * dy);
      if (dist <= h.wr && dist < bestDist) { best = h; bestDist = dist; }
    }
    return best;
  }

  global.MapRender = {
    drawGrid, drawBuildings, drawEpicenter, drawHazardCells,
    drawSafezones, drawIncidents, drawTeams, hitTestWorld,
  };
})(window);

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

  // Part 3: terrain, the bottom map layer, from a STATICMAP payload
  // ({width, height, rows: [hex, ...]} - see debug_emit_static_map_
  // if_changed() in laptop/debug.c). Each row is `width` cells packed
  // 8 hex chars each: 2 (flags: bit0 navigable, bit1 flood, bits2-4
  // road_class) + 2 (travel_cost) + 4 (district_id).
  const ROAD_TINT = {
    1: "rgba(139,152,165,0.10)", // ROAD_SERVICE
    2: "rgba(139,152,165,0.16)", // ROAD_LOCAL
    3: "rgba(139,152,165,0.22)", // ROAD_COLLECTOR
    4: "rgba(139,152,165,0.30)", // ROAD_ARTERIAL
  };

  // Memoized by the STATICMAP object's own identity - it only changes
  // when a new STATICMAP line actually arrives (rare; see debug.c),
  // not every tick, so this never re-parses 1296 cells on a frame
  // where terrain didn't change.
  let _terrainParsed = null; // { source, width, height, cells: Array<{navigable,flood,roadClass,travelCost,districtId}> }

  function parseTerrain(map) {
    if (!map || !map.rows) return null;
    if (_terrainParsed && _terrainParsed.source === map) return _terrainParsed;
    const { width, height, rows } = map;
    const cells = new Array(width * height);
    for (let gy = 0; gy < height; gy++) {
      const row = rows[gy] || "";
      for (let gx = 0; gx < width; gx++) {
        const off = gx * 8;
        const flags = parseInt(row.substr(off, 2), 16) || 0;
        const cost = parseInt(row.substr(off + 2, 2), 16) || 0;
        const district = parseInt(row.substr(off + 4, 4), 16) || 0;
        cells[gy * width + gx] = {
          navigable: !!(flags & 0x01),
          flood: !!(flags & 0x02),
          roadClass: (flags >> 2) & 0x07,
          travelCost: cost,
          districtId: district,
        };
      }
    }
    _terrainParsed = { source: map, width, height, cells };
    return _terrainParsed;
  }

  // The actual per-cell fill + district-border drawing is expensive
  // (up to ~1296 fillRects plus border segments) to redo every single
  // tick at a 0ms tick delay, but the terrain and the camera view both
  // change far less often than that - so this renders into an
  // offscreen bitmap ONCE per (terrain, camera position/zoom/size)
  // combination and just blits it every other tick. A pan/zoom still
  // re-renders on every step of the drag (the cache key includes the
  // camera position), same cost as today's existing full redraw on
  // every mousemove while panning - just not paid on every LIVEFRAME
  // tick when the camera hasn't moved.
  let _terrainBitmap = null;
  let _terrainBitmapKey = null;
  let _terrainBitmapSource = null; // the STATICMAP object identity the cached bitmap was rendered from

  function renderTerrainBitmap(view, t) {
    const dpr = window.devicePixelRatio || 1;
    const canvas = document.createElement("canvas");
    canvas.width = Math.max(1, Math.round(view.state.viewW * dpr));
    canvas.height = Math.max(1, Math.round(view.state.viewH * dpr));
    const octx = canvas.getContext("2d");
    octx.setTransform(dpr, 0, 0, dpr, 0, 0);

    const px1 = view.worldToScreen(0, 0), px2 = view.worldToScreen(1, 0);
    const cellPx = Math.abs(px2.x - px1.x) || 1;
    const vw = view.state.viewW, vh = view.state.viewH;

    for (let gy = 0; gy < t.height; gy++) {
      for (let gx = 0; gx < t.width; gx++) {
        const cell = t.cells[gy * t.width + gx];
        const p = view.worldToScreen(gx - 0.5, gy - 0.5);
        if (p.x + cellPx < 0 || p.y + cellPx < 0 || p.x > vw || p.y > vh) continue; // offscreen at this pan/zoom

        // Water/flood, then blocked/rubble (STATICMAP has no separate
        // rubble flag - "not navigable" is this layer's honest proxy
        // for it), then a road tint by class, else open ground (no fill).
        if (cell.flood) {
          octx.fillStyle = "rgba(56,139,253,0.22)";
          octx.fillRect(p.x, p.y, cellPx, cellPx);
        } else if (!cell.navigable) {
          octx.fillStyle = "rgba(139,152,165,0.18)";
          octx.fillRect(p.x, p.y, cellPx, cellPx);
        } else if (cell.roadClass > 0 && ROAD_TINT[cell.roadClass]) {
          octx.fillStyle = ROAD_TINT[cell.roadClass];
          octx.fillRect(p.x, p.y, cellPx, cellPx);
        }
      }
    }

    // District borders: a thin line wherever a cell's district differs
    // from its right/bottom neighbor - no separate polygon extraction
    // needed, and it reads fine at any zoom level.
    octx.save();
    octx.strokeStyle = "rgba(230,237,243,0.10)";
    octx.lineWidth = 1;
    for (let gy = 0; gy < t.height; gy++) {
      for (let gx = 0; gx < t.width; gx++) {
        const cell = t.cells[gy * t.width + gx];
        const right = gx + 1 < t.width ? t.cells[gy * t.width + gx + 1] : null;
        const below = gy + 1 < t.height ? t.cells[(gy + 1) * t.width + gx] : null;
        if (right && right.districtId !== cell.districtId) {
          const a = view.worldToScreen(gx + 0.5, gy - 0.5);
          const b = view.worldToScreen(gx + 0.5, gy + 0.5);
          octx.beginPath(); octx.moveTo(a.x, a.y); octx.lineTo(b.x, b.y); octx.stroke();
        }
        if (below && below.districtId !== cell.districtId) {
          const a = view.worldToScreen(gx - 0.5, gy + 0.5);
          const b = view.worldToScreen(gx + 0.5, gy + 0.5);
          octx.beginPath(); octx.moveTo(a.x, a.y); octx.lineTo(b.x, b.y); octx.stroke();
        }
      }
    }
    octx.restore();
    return canvas;
  }

  function drawTerrain(ctx, view, terrainMap) {
    const t = parseTerrain(terrainMap);
    if (!t) return;
    const s = view.state;
    const key = `${s.x}|${s.y}|${s.zoom}|${s.viewW}|${s.viewH}`;
    if (_terrainBitmapSource !== t.source || _terrainBitmapKey !== key || !_terrainBitmap) {
      _terrainBitmap = renderTerrainBitmap(view, t);
      _terrainBitmapKey = key;
      _terrainBitmapSource = t.source;
    }
    ctx.drawImage(_terrainBitmap, 0, 0, s.viewW, s.viewH);
  }

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
    drawGrid, drawTerrain, drawBuildings, drawEpicenter, drawHazardCells,
    drawSafezones, drawIncidents, drawTeams, hitTestWorld,
  };
})(window);

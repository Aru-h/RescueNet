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

  // Red is reserved for things that need attention right now - P1 here,
  // and the team fault dot in drawTeams - so no other priority uses it,
  // and P3 gets its own hue rather than the blue drawTerrain uses for
  // flood, so the two don't read as the same thing.
  const PRIORITY_COLORS = { 1: "#f85149", 2: "#d29922", 3: "#39c5cf", 4: "#8b98a5" };
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
  // road_class, bit5 road_blocked) + 2 (travel_cost) + 4 (district_id).
  // A brighter, distinctly-hued neutral than blocked cells or buildings
  // (both stay on the rgba(139,152,165,…) grey), so the road network
  // reads as its own layer instead of blending into "just blocked".
  const ROAD_COLOR_RGB = "158,178,207"; // light slate-blue
  const ROAD_STYLE = {
    1: { alpha: 0.24, widthFrac: 0.26 }, // ROAD_SERVICE - faintest, narrowest lane
    2: { alpha: 0.38, widthFrac: 0.38 }, // ROAD_LOCAL
    3: { alpha: 0.52, widthFrac: 0.50 }, // ROAD_COLLECTOR
    4: { alpha: 0.72, widthFrac: 0.64 }, // ROAD_ARTERIAL - boldest, widest, gets a centre line
  };

  // Diagonal hatch overlay, clipped to a rect - shared by the terrain
  // layer's blocked/rubble cells, hazard cells and building damage so
  // "damaged/blocked/hazardous" reads as one consistent visual language
  // instead of three different ad-hoc treatments.
  function drawHatch(ctx, x, y, w, h, strokeStyle, step) {
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, y, w, h);
    ctx.clip();
    ctx.strokeStyle = strokeStyle;
    ctx.lineWidth = 1;
    for (let o = -h; o < w + h; o += step) {
      ctx.beginPath();
      ctx.moveTo(x + o, y + h);
      ctx.lineTo(x + o + h, y);
      ctx.stroke();
    }
    ctx.restore();
  }

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
          roadBlocked: !!(flags & 0x20),
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

        // Water/flood (blue), then blocked/rubble (STATICMAP has no
        // separate rubble flag - "not navigable" is this layer's honest
        // proxy for it - drawn dark with a diagonal hatch so it never
        // reads as "just another road"), then a road lane by class, else
        // open ground (left unfilled).
        if (cell.flood) {
          octx.fillStyle = "rgba(56,139,253,0.28)";
          octx.fillRect(p.x, p.y, cellPx, cellPx);
        } else if (!cell.navigable) {
          octx.fillStyle = "rgba(6,9,13,0.6)";
          octx.fillRect(p.x, p.y, cellPx, cellPx);
          drawHatch(octx, p.x, p.y, cellPx, cellPx, "rgba(230,237,243,0.16)", 4);
        } else if (cell.roadClass > 0 && ROAD_STYLE[cell.roadClass]) {
          const style = ROAD_STYLE[cell.roadClass];
          const laneW = cellPx * style.widthFrac;
          octx.fillStyle = `rgba(${ROAD_COLOR_RGB},${style.alpha})`;
          const cxp = p.x + cellPx / 2, cyp = p.y + cellPx / 2;
          const north = gy > 0 ? t.cells[(gy - 1) * t.width + gx] : null;
          const south = gy + 1 < t.height ? t.cells[(gy + 1) * t.width + gx] : null;
          const west = gx > 0 ? t.cells[gy * t.width + gx - 1] : null;
          const east = gx + 1 < t.width ? t.cells[gy * t.width + gx + 1] : null;
          const isRoad = (c) => !!(c && c.roadClass > 0);
          const hasN = isRoad(north), hasS = isRoad(south), hasW = isRoad(west), hasE = isRoad(east);
          // A hub square plus an arm toward each neighbor that's ALSO a
          // road cell - only real connections get drawn, not a blind
          // "+" regardless of what's actually next door (which read as
          // scattered disconnected blobs rather than a road network).
          octx.fillRect(cxp - laneW / 2, cyp - laneW / 2, laneW, laneW);
          if (hasN) octx.fillRect(cxp - laneW / 2, p.y, laneW, cellPx / 2 + laneW / 2);
          if (hasS) octx.fillRect(cxp - laneW / 2, cyp - laneW / 2, laneW, cellPx / 2 + laneW / 2);
          if (hasW) octx.fillRect(p.x, cyp - laneW / 2, cellPx / 2 + laneW / 2, laneW);
          if (hasE) octx.fillRect(cxp - laneW / 2, cyp - laneW / 2, cellPx / 2 + laneW / 2, laneW);
          if (cell.roadClass === 4) {
            octx.strokeStyle = "rgba(255,255,255,0.5)";
            octx.lineWidth = 1;
            if (hasW || hasE) {
              octx.beginPath();
              octx.moveTo(hasW ? p.x : cxp, cyp);
              octx.lineTo(hasE ? p.x + cellPx : cxp, cyp);
              octx.stroke();
            }
            if (hasN || hasS) {
              octx.beginPath();
              octx.moveTo(cxp, hasN ? p.y : cyp);
              octx.lineTo(cxp, hasS ? p.y + cellPx : cyp);
              octx.stroke();
            }
          }
          if (cell.roadBlocked) {
            // Still drawn as a road (it's a real segment of the network,
            // just obstructed right now) but flagged unmistakably: a
            // dashed outline plus a small X, never the same look as the
            // terrain layer's "not navigable at all" dark hatch above.
            octx.save();
            octx.strokeStyle = "rgba(248,81,73,0.75)";
            octx.lineWidth = 1;
            octx.setLineDash([3, 2]);
            octx.strokeRect(p.x + 1, p.y + 1, cellPx - 2, cellPx - 2);
            octx.setLineDash([]);
            const xr = Math.min(cellPx, laneW * 1.4) / 2;
            octx.beginPath();
            octx.moveTo(cxp - xr, cyp - xr); octx.lineTo(cxp + xr, cyp + xr);
            octx.moveTo(cxp + xr, cyp - xr); octx.lineTo(cxp - xr, cyp + xr);
            octx.stroke();
            octx.restore();
          }
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

  // fit-to-view zoom, recovered from state.minZoom (= fit*0.4, see
  // camera.js's createCamera) without either file hardcoding the
  // other's constant. Several layers below gate on a multiple of this
  // rather than an absolute zoom number, so "default view" behaves the
  // same at any viewport size.
  function fitZoomOf(view) { return view.state.minZoom / 0.4; }

  // Hidden at the default (fit-to-view) zoom - it adds nothing over the
  // terrain/road layer until zoomed in far enough to actually align
  // against individual cells.
  function drawGrid(ctx, view, gridN) {
    if (view.state.zoom < fitZoomOf(view) * 1.6) return;
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
  }

  // STATICENV's building x/y IS the footprint's centre, not its
  // top-left (core/environment.c's place_building_rect: loc_x = x +
  // w/2, w/h are the full footprint - see dashboard_e2e_selftest.py's
  // building-footprint-centring check for the regression test this
  // backs). Exported so that test can call it directly rather than
  // inferring centring from rendered pixels.
  function buildingFootprintWorldRect(b) {
    const w = Math.max(1, b.w || 1), h = Math.max(1, b.h || 1);
    return { x: b.x - w / 2, y: b.y - h / 2, w, h };
  }

  // Buildings: a small dot at the true centre, sized by floor count,
  // neutral colour - no filled footprint. Even correctly centred, a
  // filled footprint fights visually with the road layer (~120
  // building-lot cells are also road cells in this generator), so the
  // footprint itself is only an outline, shown for every building once
  // zoomed in past a threshold, or forced open for one specific
  // building via opts.outlineBuildingId (index.html sets this to the
  // building behind an open incident-group hover/click card).
  function drawBuildings(ctx, view, env, opts) {
    if (!env || !env.buildings) return;
    opts = opts || {};
    const outlineBuildingId = opts.outlineBuildingId;
    const fitZoom = fitZoomOf(view);
    const showAllFootprints = view.state.zoom >= fitZoom * 2.2;
    const showLabels = view.state.zoom >= fitZoom * 1.4;

    env.buildings.forEach((b) => {
      const center = view.worldToScreen(b.x, b.y);
      const damageFrac = Math.min(1, (b.damage || 0) / 255);
      const floors = Math.max(1, b.floors || 1);
      const dotR = Math.max(2, Math.min(6, 1.5 + floors * 0.35));

      ctx.beginPath();
      ctx.arc(center.x, center.y, dotR, 0, Math.PI * 2);
      // Neutral always - red stays reserved for P1 incidents and the
      // team fault dot - damage just deepens this same grey.
      ctx.fillStyle = `rgba(139,152,165,${(0.45 + damageFrac * 0.45).toFixed(3)})`;
      ctx.fill();
      if (damageFrac > 0.15) {
        ctx.strokeStyle = "rgba(230,237,243,0.5)";
        ctx.lineWidth = 1;
        ctx.stroke();
      }

      if (showAllFootprints || b.id === outlineBuildingId) {
        const rect = buildingFootprintWorldRect(b);
        const topLeft = view.worldToScreen(rect.x, rect.y);
        const bottomRight = view.worldToScreen(rect.x + rect.w, rect.y + rect.h);
        ctx.strokeStyle = (b.id === outlineBuildingId) ? "rgba(255,255,255,0.6)" : "rgba(139,152,165,0.3)";
        ctx.lineWidth = 1;
        ctx.strokeRect(topLeft.x, topLeft.y, bottomRight.x - topLeft.x, bottomRight.y - topLeft.y);
      }

      if (showLabels && dotR >= 3) {
        // Small and deliberately low-contrast - incidents are the thing
        // that should draw the eye on this map, not the building itself.
        const label = `${floors}F`;
        ctx.font = "8px sans-serif";
        const w = ctx.measureText(label).width;
        ctx.fillStyle = "rgba(13,17,23,0.35)";
        ctx.fillRect(center.x + dotR + 1, center.y - 5, w + 3, 10);
        ctx.fillStyle = "rgba(139,152,165,0.6)";
        ctx.textAlign = "left";
        ctx.textBaseline = "top";
        ctx.fillText(label, center.x + dotR + 2.5, center.y - 4);
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
    // Purple, not red - red is reserved for P1 incidents and the team
    // fault dot.
    ctx.strokeStyle = "rgba(163,113,247,0.5)";
    ctx.lineWidth = 1.5;
    ctx.setLineDash([4, 3]);
    ctx.stroke();
    ctx.restore();
  }

  // Amber throughout, never red - red is reserved for P1 incidents and
  // the team fault dot. A blocked hazard cell gets a light amber hatch
  // (same visual language as the terrain layer's dark blocked hatch, at
  // a lower key so it doesn't compete with it); a plain hazard cell is a
  // flat amber fill, lighter still where hazard_flags is unset.
  function drawHazardCells(ctx, view, cells) {
    if (!cells) return;
    const px1 = view.worldToScreen(0, 0), px2 = view.worldToScreen(1, 0);
    const cellPx = Math.abs(px2.x - px1.x) || 1;
    cells.forEach((c) => {
      const p = view.worldToScreen(c.x - 0.5, c.y - 0.5);
      if (c.blocked) {
        ctx.fillStyle = "rgba(210,153,34,0.14)";
        ctx.fillRect(p.x, p.y, cellPx, cellPx);
        drawHatch(ctx, p.x, p.y, cellPx, cellPx, "rgba(210,153,34,0.45)", 4);
      } else {
        ctx.fillStyle = c.hazard_flags ? "rgba(210,153,34,0.20)" : "rgba(210,153,34,0.10)";
        ctx.fillRect(p.x, p.y, cellPx, cellPx);
      }
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

  // Markers only, no on-map text (map simplification decision after
  // c68a1f7): colour is priority, size is victim count, a small corner
  // badge is the hazard type. Incidents that share a building_id are
  // collapsed into one marker with a count badge - their full list is
  // attached to the hit's `groupIncidents` for the hover/click card (see
  // index.html's incidentSummaryLine / showMapCard) rather than either
  // drawing N overlapping labels or picking one incident to speak for
  // the rest.
  function drawIncidents(ctx, view, list, opts) {
    const hits = [];
    const fogViewMode = opts.fogViewMode;
    const selectedMarker = opts.selectedMarker;
    // A building-linked incident is drawn AT its building's own marker
    // (not its own raw x/y, which can sit slightly off the building's
    // true centre) so the incident and building markers are the same
    // dot, not two overlapping ones a pixel or two apart.
    const buildingsById = new Map((opts.buildings || []).map((b) => [b.id, b]));
    const priorityScale = { 1: 1.25, 2: 1.05, 3: 0.9, 4: 0.75 };

    const visible = (list || []).filter((inc) => {
      if (inc.status === 3) return false; // resolved
      if (fogViewMode === "operator" && inc.discovered === 0) return false;
      return true;
    });

    const byBuilding = new Map();
    const clusters = [];
    visible.forEach((inc) => {
      if (inc.building_id !== undefined && inc.building_id >= 0) {
        let cluster = byBuilding.get(inc.building_id);
        if (!cluster) { cluster = { members: [] }; byBuilding.set(inc.building_id, cluster); clusters.push(cluster); }
        cluster.members.push(inc);
      } else {
        clusters.push({ members: [inc] });
      }
    });

    // Representative = worst priority (lowest number) then most victims;
    // clusters themselves are drawn worst-first so a lower-priority label
    // is the one that loses the overlap check.
    clusters.forEach((c) => c.members.sort((a, b) => (a.priority - b.priority) || ((b.victims || 0) - (a.victims || 0))));
    clusters.sort((a, b) => a.members[0].priority - b.members[0].priority);

    clusters.forEach((cluster) => {
      const members = cluster.members;
      const inc = members[0];
      const isGroup = members.length > 1;

      const color = PRIORITY_COLORS[inc.priority] || "#8b98a5";
      const pScale = priorityScale[inc.priority] || 0.85;
      const worldSize = Math.min(0.9, (0.32 + inc.victims * 0.045)) * pScale;
      const building = (inc.building_id !== undefined && inc.building_id >= 0) ? buildingsById.get(inc.building_id) : null;
      const worldX = building ? building.x : inc.x, worldY = building ? building.y : inc.y;
      const center = view.worldToScreen(worldX, worldY);
      const sizePx = worldSize * view.state.zoom;

      ctx.fillStyle = color;
      ctx.globalAlpha = inc.status === 0 ? 1 : 0.55;
      ctx.fillRect(center.x - sizePx / 2, center.y - sizePx / 2, sizePx, sizePx);
      ctx.globalAlpha = 1;

      // No on-map text at all (map simplification decision after
      // c68a1f7: priority is colour, victim count is marker size, a
      // shared building is a count badge) - full detail (victims,
      // floor, hazard, status, assigned/support team, work remaining)
      // lives in the hover/click card (see showMapCard in index.html),
      // never on the map itself.
      const icon = INCIDENT_ICONS[inc.type];
      if (icon && sizePx >= 8) {
        // A small corner badge, capped at ~40% of the marker's own size
        // - never the big centered emoji this used to be, which could
        // grow past the marker and cover a building or another marker.
        const badgeD = Math.min(14, sizePx * 0.4);
        const bx = center.x - sizePx / 2 + badgeD / 2;
        const by = center.y + sizePx / 2 - badgeD / 2;
        ctx.beginPath();
        ctx.arc(bx, by, badgeD / 2, 0, Math.PI * 2);
        ctx.fillStyle = "#0d1117";
        ctx.fill();
        ctx.font = `${Math.max(7, badgeD * 0.85)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(icon, bx, by + 0.5);
      }

      if (isGroup) {
        const badgeR = 7;
        const bx = center.x + sizePx / 2;
        const by = center.y - sizePx / 2;
        ctx.beginPath();
        ctx.arc(bx, by, badgeR, 0, Math.PI * 2);
        ctx.fillStyle = "#1f6feb";
        ctx.fill();
        ctx.strokeStyle = "#fff";
        ctx.lineWidth = 1;
        ctx.stroke();
        ctx.fillStyle = "#fff";
        ctx.font = "bold 9px sans-serif";
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(String(members.length), bx, by + 0.5);
      }

      if (selectedMarker && selectedMarker.type === "incident" &&
          members.some((m) => m.id === selectedMarker.id)) {
        ctx.strokeStyle = "#fff";
        ctx.lineWidth = 2;
        ctx.strokeRect(center.x - sizePx / 2 - 3, center.y - sizePx / 2 - 3, sizePx + 6, sizePx + 6);
      }

      const hitData = isGroup ? Object.assign({}, inc, { groupIncidents: members }) : inc;
      hits.push({ type: "incident", id: inc.id, wx: worldX, wy: worldY, wr: Math.max(worldSize / 2, 0.4), data: hitData });
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
      const homeCenter = view.worldToScreen(t.x, t.y);
      const r = markerWorldR * view.state.zoom;

      // ON_SCENE teams sit at the exact incident location, so drawing
      // there covers the incident marker underneath. Offset the drawn
      // marker toward the corner instead - the offset (r*0.9 px, i.e.
      // ~0.9 marker-radius world units) stays well inside the team hit
      // target's own radius below, so clicking the visible marker still
      // resolves to this team.
      const center = (t.status === 2)
        ? { x: homeCenter.x + r * 0.9, y: homeCenter.y - r * 0.9 }
        : homeCenter;

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

      // Part 4: a manual override pin, opposite corner from the fault
      // dot above so the two never collide - a diamond, not another
      // dot, so it never reads as "another fault" at a glance. Filled
      // while pending (t.override===1, the pin is still forcing
      // itself into this team's CBBA bundle); once core/cbba.c's
      // cbba_apply_override_pin() commits it (t.override drops back
      // to 0 on its own, normally within a few ticks - see core/
      // debug.h's pinned/committed/resolved-or-interrupted doc), the
      // badge switches to outline-only for as long as
      // window._overridePhase (index.html's updateOverridePhases())
      // still tracks this team as "committed" - dropping once that
      // ends, resolved or interrupted either way.
      const phase = window._overridePhase && window._overridePhase[t.id];
      if (t.override || (phase && phase.kind === "committed")) {
        const bx = center.x - r * 1.1, by = center.y + r * 1.1;
        const s = 4;
        ctx.beginPath();
        ctx.moveTo(bx, by - s);
        ctx.lineTo(bx + s, by);
        ctx.lineTo(bx, by + s);
        ctx.lineTo(bx - s, by);
        ctx.closePath();
        if (t.override) {
          ctx.fillStyle = "#a371f7";
          ctx.fill();
          ctx.strokeStyle = "#fff";
          ctx.lineWidth = 1;
          ctx.stroke();
        } else {
          ctx.strokeStyle = "#a371f7";
          ctx.lineWidth = 1.5;
          ctx.stroke();
        }
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
    buildingFootprintWorldRect,
  };
})(window);

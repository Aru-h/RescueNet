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

  function renderTerrainBitmap(view, t, dprOverride) {
    const dpr = dprOverride || window.devicePixelRatio || 1;
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
    if (view.state.viewMode === "iso") { drawTerrainIso(ctx, view, t); return; }
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
    if (view.state.viewMode === "iso") { drawGridIso(ctx, view, gridN); return; }
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


  // ---------- evacuation gateways ----------
  // The map-edge exits where teams take evacuated people off the map. They
  // come from STATICENV's "gateways" ([{id,x,y}]); recordings made before
  // that field existed simply have none, and nothing is drawn.
  const GATEWAY_COLOR = "#2fe0a0"; // mint: clearly apart from the safe zones' leaf green (#3fb950)
  function gatewayList(env) {
    const g = env && env.gateways;
    if (!Array.isArray(g)) return [];
    return g.filter((e) => e && Number.isFinite(e.x) && Number.isFinite(e.y)).map((e) => ({ id: e.id, x: e.x, y: e.y }));
  }
  // The unit vector (world x, y) from the map interior out through the nearest edge.
  function gatewayOutward(g, W, H) {
    const dN = g.y, dS = H - 1 - g.y, dW = g.x, dE = W - 1 - g.x, m = Math.min(dN, dS, dW, dE);
    if (m === dN) return { x: 0, y: -1 };
    if (m === dS) return { x: 0, y: 1 };
    if (m === dW) return { x: -1, y: 0 };
    return { x: 1, y: 0 };
  }
  // A mountain with a tunnel arch, drawn inside the gateway's own edge cell (so it
  // never covers a neighbouring cell - on a river bank that could be water), with
  // an arrow pointing off the map and the "EXIT n" label OUTSIDE the map edge, where
  // there is nothing to cover. `box` comes from gatewayGlyphBox(); `dir` is the unit
  // screen vector pointing out of the map.
  function drawGatewayGlyph(ctx, box, dir, id, label, view) {
    const s = box.size, cx = box.cx, cy = box.cy;
    if (s < 5) return;                                                            // far too small to read: draw nothing rather than a smudge
    const rock = "#26332d", rim = "rgba(3,5,10,0.9)", lw = Math.max(1, Math.min(1.6, s / 9));
    ctx.save(); ctx.lineJoin = "round"; ctx.lineCap = "round";
    const x0 = cx - s / 2, y1 = cy + s / 2;                                        // the box: base line y1
    const peak = (px, w, h) => { ctx.beginPath(); ctx.moveTo(px - w / 2, y1); ctx.lineTo(px, y1 - h); ctx.lineTo(px + w / 2, y1); ctx.closePath(); };
    const bigX = x0 + s * 0.6, bigW = s * 0.78, bigH = s * 0.92, smlX = x0 + s * 0.24, smlW = s * 0.5, smlH = s * 0.56;
    ctx.lineWidth = lw + 2.2; ctx.strokeStyle = rim; peak(smlX, smlW, smlH); ctx.stroke(); peak(bigX, bigW, bigH); ctx.stroke();
    ctx.lineWidth = lw; ctx.strokeStyle = GATEWAY_COLOR; ctx.fillStyle = rock;
    peak(smlX, smlW, smlH); ctx.fill(); ctx.stroke(); peak(bigX, bigW, bigH); ctx.fill(); ctx.stroke();
    const aw = s * 0.2, ah = s * 0.34;                                             // tunnel mouth at the foot of the big peak
    ctx.beginPath(); ctx.moveTo(bigX - aw, y1); ctx.lineTo(bigX - aw, y1 - ah + aw); ctx.arc(bigX, y1 - ah + aw, aw, Math.PI, 0); ctx.lineTo(bigX + aw, y1); ctx.closePath();
    ctx.fillStyle = "#03050a"; ctx.fill(); ctx.lineWidth = Math.max(1, lw * 0.8); ctx.strokeStyle = GATEWAY_COLOR; ctx.stroke();
    // arrow out of the map, just beyond the edge (kept on the canvas)
    const L = Math.max(5, s * 0.34), Wd = L * 0.6, ppx = -dir.y, ppy = dir.x, vw = view.state.viewW, vh = view.state.viewH;
    let ex = cx + dir.x * (s * 0.5 + L * 0.55), ey = cy + dir.y * (s * 0.5 + L * 0.55);
    ex = Math.max(L * 0.7, Math.min(vw - L * 0.7, ex)); ey = Math.max(L * 0.7, Math.min(vh - L * 0.7, ey));
    ctx.beginPath(); ctx.moveTo(ex + dir.x * L * 0.6, ey + dir.y * L * 0.6); ctx.lineTo(ex - dir.x * L * 0.4 + ppx * Wd, ey - dir.y * L * 0.4 + ppy * Wd); ctx.lineTo(ex - dir.x * L * 0.4 - ppx * Wd, ey - dir.y * L * 0.4 - ppy * Wd); ctx.closePath();
    ctx.fillStyle = GATEWAY_COLOR; ctx.fill(); ctx.lineWidth = 1.2; ctx.strokeStyle = rim; ctx.stroke();
    if (label && s >= 16) {                                                        // hidden when zoomed out (no clutter) or when it would not fit on the canvas
      ctx.font = "bold 10px sans-serif"; ctx.textAlign = "center"; ctx.textBaseline = "middle";
      const t = "EXIT " + id, tw = ctx.measureText(t).width, th = 11, off = s * 0.5 + L * 1.2 + (Math.abs(dir.y) > 0.5 ? th / 2 : tw / 2) + 3;
      const lx = cx + dir.x * off, ly = cy + dir.y * off;
      if (lx - tw / 2 >= 0 && lx + tw / 2 <= vw && ly - th / 2 >= 0 && ly + th / 2 <= vh) {
        ctx.lineWidth = 3; ctx.strokeStyle = rim; ctx.strokeText(t, lx, ly); ctx.fillStyle = GATEWAY_COLOR; ctx.fillText(t, lx, ly);
      }
    }
    ctx.restore();
  }
  // The glyph's footprint on screen: the gateway's own cell (2D: exactly the cell; 2.5D: a little smaller, the
  // upright glyph is drawn standing on it). Also returned to tests, which check it against the water cells.
  function gatewayGlyphBox(view, g) {
    const W = view.state.worldW, H = view.state.worldH, o = gatewayOutward(g, W, H);
    const a = view.worldToScreen(g.x, g.y, 0), b = view.worldToScreen(g.x + o.x, g.y + o.y, 0);
    let dx = b.x - a.x, dy = b.y - a.y; const px = Math.hypot(dx, dy) || 1; dx /= px; dy /= px;
    const iso = view.state.viewMode === "iso", size = iso ? px * 0.8 : px;
    return { cx: a.x, cy: a.y, size, px, dir: { x: dx, y: dy }, x0: a.x - size / 2, x1: a.x + size / 2, y0: a.y - size / 2, y1: a.y + size / 2 };
  }
  function gatewayHit(g) { return { type: "gateway", id: g.id, wx: g.x, wy: g.y, wr: 0.7, low: true, data: g }; }   // low: never wins over a team / incident
  // 2D: drawn on the ground layer; returns the hit targets.
  function drawGateways(ctx, view, env) {
    return gatewayList(env).map((g) => {
      const box = gatewayGlyphBox(view, g);
      drawGatewayGlyph(ctx, box, box.dir, g.id, true, view);
      return gatewayHit(g);
    });
  }

  // Visible incidents (not resolved; in operator view not undiscovered),
  // grouped one cluster per building_id (incidents with no building are
  // singleton clusters), members and clusters both sorted worst-first.
  // Shared by the 2D and iso incident drawing.
  function buildIncidentClusters(list, fogViewMode) {
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
    return clusters;
  }

  // The incident marker proper - square, corner hazard badge, group
  // count badge, selection outline - drawn around a SCREEN-space centre.
  // Shared by 2D (centre = the incident's ground point) and iso (centre
  // = the upright marker's head), so both modes show the same glyph.
  function drawIncidentGlyph(ctx, center, sizePx, color, inc, members, selectedMarker) {
    const isGroup = members.length > 1;
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

    const clusters = buildIncidentClusters(list, fogViewMode);

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

      drawIncidentGlyph(ctx, center, sizePx, color, inc, members, selectedMarker);

      const hitData = isGroup ? Object.assign({}, inc, { groupIncidents: members }) : inc;
      hits.push({ type: "incident", id: inc.id, wx: worldX, wy: worldY, wr: Math.max(worldSize / 2, 0.4), data: hitData });
    });
    return hits;
  }

  // The team marker proper - status ring, numbered disc, fault dot,
  // override pin, selection ring - around a SCREEN-space centre, radius
  // r px. Shared by 2D and iso (see drawIncidentGlyph).
  function drawTeamGlyph(ctx, center, r, color, t, selectedMarker) {
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

      drawTeamGlyph(ctx, center, r, color, t, selectedMarker);

      hits.push({ type: "team", id: t.id, wx: t.x, wy: t.y, wr: markerWorldR + 0.18, data: { ...t, i } });
    });
    return hits;
  }

  // ===================================================================
  // Iso (2.5D) rendering - used when view.state.viewMode === "iso".
  //
  // Everything here still goes through view.worldToScreen(x, y, z) /
  // view.screenToWorld(sx, sy, z); nothing re-implements the
  // projection. Ground-plane things (tiles, hazard cells, rings, lines)
  // are drawn at z = 0; upright things (buildings, markers) get a z.
  // ===================================================================

  const ISO_FLOOR_H = 0.25;        // world units of height per building floor
  const ISO_TEAM_Z = 1.0;          // team marker head height above its footing
  const ISO_INCIDENT_Z = 0.75;     // incident marker head height above its footing
  const ISO_SLAB_T = 0.35;         // thickness of the ground slab's visible sides
  const ISO_BUILDING_RGB = [139, 152, 165]; // the same neutral grey the 2D dots use

  // core/environment.h's ENV_DAMAGE_* thresholds (damage_state is 0-255
  // but those four levels are the only named ones). STATICENV has no
  // "collapsed" flag, so SEVERE and above is what the dashboard treats
  // as collapsed/rubble.
  const ENV_DAMAGE_MINOR = 25, ENV_DAMAGE_MODERATE = 50, ENV_DAMAGE_SEVERE = 75;

  // Height in world units. STATICENV carries `floors` per building, so
  // that is the source (floors * ISO_FLOOR_H); damaged buildings are
  // shorter, collapsed ones low rubble.
  function buildingHeight(b) {
    const floors = Math.max(1, b.floors || 1);
    const dmg = b.damage || 0;
    let f = 1;
    if (dmg >= 150) f = 0.25;
    else if (dmg >= ENV_DAMAGE_SEVERE) f = 0.45;
    else if (dmg >= ENV_DAMAGE_MODERATE) f = 0.8;
    else if (dmg >= ENV_DAMAGE_MINOR) f = 0.93;
    return floors * ISO_FLOOR_H * f;
  }

  function rgba(rgb, f, a) {
    return `rgba(${Math.round(rgb[0] * f)},${Math.round(rgb[1] * f)},${Math.round(rgb[2] * f)},${a})`;
  }

  // Per-STATICENV-object preparation, memoized on the env object's
  // identity (same trick as parseTerrain): footprint rects, heights and
  // face colours are computed once, not per frame.
  let _isoEnvCache = null;
  function prepareIsoEnv(env) {
    if (_isoEnvCache && _isoEnvCache.source === env) return _isoEnvCache;
    const list = ((env && env.buildings) || []).map((b) => {
      const r = buildingFootprintWorldRect(b);
      const dmg = b.damage || 0;
      const f = 1 - 0.45 * Math.min(1, dmg / 150); // damage only ever darkens - red stays reserved for P1
      return {
        b, id: b.id, f, x0: r.x, y0: r.y, x1: r.x + r.w, y1: r.y + r.h,
        key: b.x + b.y,
        h: buildingHeight(b),
        collapsed: dmg >= ENV_DAMAGE_SEVERE,
        damaged: dmg >= ENV_DAMAGE_MODERATE,
        top: rgba(ISO_BUILDING_RGB, f, 0.97),
        left: rgba(ISO_BUILDING_RGB, f * 0.68, 0.97),
        right: rgba(ISO_BUILDING_RGB, f * 0.46, 0.97),
      };
    });
    const byId = new Map(list.map((e) => [e.id, e]));
    _isoEnvCache = { source: env, list, byId };
    return _isoEnvCache;
  }

  function strictlyInside(e, x, y) { return x > e.x0 && x < e.x1 && y > e.y0 && y < e.y1; }

  // Roof height under a ground point (0 if no building is there):
  // markers standing on a building's footprint are drawn on its roof.
  function footingZ(prep, x, y) {
    let z = 0;
    for (const e of prep.list) if (strictlyInside(e, x, y) && e.h > z) z = e.h;
    return z;
  }

  // World height gained per world-unit step toward the viewer along the
  // line of sight (+1,+1 on the ground), measured from the camera so no
  // projection constant is repeated here: a point (x+t, y+t, z+k*t)
  // lands on the same screen pixel as (x, y, z).
  function sightSlope(view) {
    const a = view.worldToScreen(0, 0, 0), b = view.worldToScreen(1, 1, 0);
    const hpx = view.heightPx(1);
    return hpx > 0 ? (b.y - a.y) / hpx : 0;
  }

  // Exact test for "is the point (x, y, z) hidden behind a building":
  // walk the line of sight toward the viewer and see whether it passes
  // through a building's box. Buildings whose footprint the point is
  // standing inside are skipped (it is on their roof). Returns the
  // occluding entries (usually none).
  function occluders(prep, slope, x, y, z) {
    const out = [];
    for (const e of prep.list) {
      if (strictlyInside(e, x, y)) continue;
      const tmin = Math.max(e.x0 - x, e.y0 - y, 0);
      const tmax = Math.min(e.x1 - x, e.y1 - y);
      if (tmax - tmin <= 1e-6) continue;
      if (z + slope * tmin < e.h) out.push(e);
    }
    return out;
  }

  function pathPoly(ctx, pts) {
    ctx.beginPath();
    ctx.moveTo(pts[0].x, pts[0].y);
    for (let i = 1; i < pts.length; i++) ctx.lineTo(pts[i].x, pts[i].y);
    ctx.closePath();
  }

  function rectPts(view, x0, y0, x1, y1, z) {
    return [view.worldToScreen(x0, y0, z), view.worldToScreen(x1, y0, z),
            view.worldToScreen(x1, y1, z), view.worldToScreen(x0, y1, z)];
  }

  // A ground-plane circle: sampled through the camera so it is exactly
  // the ellipse the projection makes of it.
  function circlePts(view, cx, cy, r, z, n) {
    const pts = new Array(n);
    for (let i = 0; i < n; i++) {
      const a = (i / n) * Math.PI * 2;
      pts[i] = view.worldToScreen(cx + Math.cos(a) * r, cy + Math.sin(a) * r, z);
    }
    return pts;
  }

  // Diagonal hatch as a cached tile pattern: filling a path with a
  // pattern is far cheaper than clipping to every diamond and stroking
  // lines, which matters for the many blocked-terrain cells in the
  // cached bitmap's one-off render.
  const _hatchPatterns = new Map();
  function hatchPattern(ctx, strokeStyle, step) {
    const s = Math.max(2, Math.round(step));
    const key = strokeStyle + "|" + s;
    let pat = _hatchPatterns.get(key);
    if (!pat) {
      const tile = document.createElement("canvas");
      tile.width = tile.height = s;
      const t = tile.getContext("2d");
      t.strokeStyle = strokeStyle;
      t.lineWidth = 1;
      t.beginPath();
      t.moveTo(-1, s + 1); t.lineTo(s + 1, -1);
      t.moveTo(-1, 2 * s + 1); t.lineTo(2 * s + 1, -1); // wraps the corner so tiles join up
      t.moveTo(-s - 1, s + 1); t.lineTo(s + 1, -s - 1);
      t.stroke();
      pat = ctx.createPattern(tile, "repeat");
      _hatchPatterns.set(key, pat);
    }
    return pat;
  }

  function hatchPoly(ctx, pts, strokeStyle, step) {
    pathPoly(ctx, pts);
    ctx.fillStyle = hatchPattern(ctx, strokeStyle, step);
    ctx.fill();
  }

  // ---- terrain ------------------------------------------------------

  // Paints the whole terrain layer (slab, flood/blocked/road tiles,
  // district borders) through `view`'s projection. Used twice: into the
  // cached offscreen bitmap (a private camera, whole map, no culling)
  // and directly onto the screen when zoomed in too far for a bitmap
  // (the real camera, cells off-screen skipped).
  function paintIsoTerrain(octx, view, t) {
    const vw = view.state.viewW, vh = view.state.viewH;
    const g = view.groundPxPerUnit();
    const W = t.width, H = t.height;

    // Slab: top plate plus the two sides facing the viewer.
    const A = -0.5, B = W - 0.5, C = H - 0.5;
    const P = (x, y, z) => view.worldToScreen(x, y, z);
    pathPoly(octx, [P(B, A, 0), P(B, C, 0), P(B, C, -ISO_SLAB_T), P(B, A, -ISO_SLAB_T)]);
    octx.fillStyle = "rgba(10,14,19,0.95)"; octx.fill();
    pathPoly(octx, [P(A, C, 0), P(B, C, 0), P(B, C, -ISO_SLAB_T), P(A, C, -ISO_SLAB_T)]);
    octx.fillStyle = "rgba(14,19,26,0.95)"; octx.fill();
    pathPoly(octx, [P(A, A, 0), P(B, A, 0), P(B, C, 0), P(A, C, 0)]);
    octx.fillStyle = "rgba(24,32,42,0.55)"; octx.fill();
    octx.strokeStyle = "rgba(230,237,243,0.10)";
    octx.lineWidth = 1;
    octx.stroke();

    const hatchStep = Math.max(2.5, g * 0.22);
    const isRoad = (c) => !!(c && c.roadClass > 0);

    for (let gy = 0; gy < H; gy++) {
      for (let gx = 0; gx < W; gx++) {
        const cell = t.cells[gy * W + gx];
        const c = view.worldToScreen(gx, gy, 0);
        if (c.x < -g || c.x > vw + g || c.y < -g || c.y > vh + g) continue;

        if (cell.flood) {
          pathPoly(octx, rectPts(view, gx - 0.5, gy - 0.5, gx + 0.5, gy + 0.5, 0));
          octx.fillStyle = "rgba(56,139,253,0.28)";
          octx.fill();
        } else if (!cell.navigable) {
          const pts = rectPts(view, gx - 0.5, gy - 0.5, gx + 0.5, gy + 0.5, 0);
          pathPoly(octx, pts);
          octx.fillStyle = "rgba(6,9,13,0.6)";
          octx.fill();
          hatchPoly(octx, pts, "rgba(230,237,243,0.16)", hatchStep);
        } else if (cell.roadClass > 0 && ROAD_STYLE[cell.roadClass]) {
          const style = ROAD_STYLE[cell.roadClass];
          const lw = style.widthFrac / 2;
          const hasN = isRoad(gy > 0 ? t.cells[(gy - 1) * W + gx] : null);
          const hasS = isRoad(gy + 1 < H ? t.cells[(gy + 1) * W + gx] : null);
          const hasW = isRoad(gx > 0 ? t.cells[gy * W + gx - 1] : null);
          const hasE = isRoad(gx + 1 < W ? t.cells[gy * W + gx + 1] : null);
          // Hub plus an arm toward each road neighbour, as ONE path so
          // the overlap isn't double-alpha'd.
          octx.beginPath();
          const addRect = (x0, y0, x1, y1) => {
            const q = rectPts(view, x0, y0, x1, y1, 0);
            octx.moveTo(q[0].x, q[0].y);
            for (let i = 1; i < 4; i++) octx.lineTo(q[i].x, q[i].y);
            octx.closePath();
          };
          addRect(gx - lw, gy - lw, gx + lw, gy + lw);
          if (hasN) addRect(gx - lw, gy - 0.5, gx + lw, gy + lw);
          if (hasS) addRect(gx - lw, gy - lw, gx + lw, gy + 0.5);
          if (hasW) addRect(gx - 0.5, gy - lw, gx + lw, gy + lw);
          if (hasE) addRect(gx - lw, gy - lw, gx + 0.5, gy + lw);
          octx.fillStyle = `rgba(${ROAD_COLOR_RGB},${style.alpha})`;
          octx.fill();
          if (cell.roadClass === 4) {
            octx.strokeStyle = "rgba(255,255,255,0.5)";
            octx.lineWidth = 1;
            octx.beginPath();
            if (hasW || hasE) {
              const a = P(hasW ? gx - 0.5 : gx, gy, 0), b = P(hasE ? gx + 0.5 : gx, gy, 0);
              octx.moveTo(a.x, a.y); octx.lineTo(b.x, b.y);
            }
            if (hasN || hasS) {
              const a = P(gx, hasN ? gy - 0.5 : gy, 0), b = P(gx, hasS ? gy + 0.5 : gy, 0);
              octx.moveTo(a.x, a.y); octx.lineTo(b.x, b.y);
            }
            octx.stroke();
          }
          if (cell.roadBlocked) {
            // Same cue as 2D: dashed outline plus an X, never the
            // "not navigable" dark hatch.
            octx.save();
            octx.strokeStyle = "rgba(248,81,73,0.75)";
            octx.lineWidth = 1;
            octx.setLineDash([3, 2]);
            pathPoly(octx, rectPts(view, gx - 0.44, gy - 0.44, gx + 0.44, gy + 0.44, 0));
            octx.stroke();
            octx.setLineDash([]);
            const xr = Math.min(1, style.widthFrac * 1.4) / 2;
            const x1 = P(gx - xr, gy - xr, 0), x2 = P(gx + xr, gy + xr, 0);
            const x3 = P(gx + xr, gy - xr, 0), x4 = P(gx - xr, gy + xr, 0);
            octx.beginPath();
            octx.moveTo(x1.x, x1.y); octx.lineTo(x2.x, x2.y);
            octx.moveTo(x3.x, x3.y); octx.lineTo(x4.x, x4.y);
            octx.stroke();
            octx.restore();
          }
        }
      }
    }

    // District borders - same rule as 2D.
    octx.save();
    octx.strokeStyle = "rgba(230,237,243,0.10)";
    octx.lineWidth = 1;
    octx.beginPath();
    for (let gy = 0; gy < H; gy++) {
      for (let gx = 0; gx < W; gx++) {
        const cell = t.cells[gy * W + gx];
        const right = gx + 1 < W ? t.cells[gy * W + gx + 1] : null;
        const below = gy + 1 < H ? t.cells[(gy + 1) * W + gx] : null;
        if (right && right.districtId !== cell.districtId) {
          const a = P(gx + 0.5, gy - 0.5, 0), b = P(gx + 0.5, gy + 0.5, 0);
          octx.moveTo(a.x, a.y); octx.lineTo(b.x, b.y);
        }
        if (below && below.districtId !== cell.districtId) {
          const a = P(gx - 0.5, gy + 0.5, 0), b = P(gx + 0.5, gy + 0.5, 0);
          octx.moveTo(a.x, a.y); octx.lineTo(b.x, b.y);
        }
      }
    }
    octx.stroke();
    octx.restore();
  }

  // The iso terrain only depends on the map and the ZOOM - a pan is
  // just a translation of the already-rendered bitmap - so unlike the
  // 2D cache (keyed on the camera position too) this re-renders only
  // when the terrain source changes or the zoom crosses into the next
  // resolution step (steps of 1.25x, always rendered at least as sharp
  // as needed so the blit never upscales by more than 1.25x).
  const ISO_BITMAP_MAX_RES = 40; // device px per world unit along an iso axis; beyond this, draw directly
  const ISO_BITMAP_BASE_RES = 4;
  const ISO_REFINE_DELAY_MS = 140;
  let _isoTerrainCache = null;   // { source, res, canvas, cam }
  let _isoRefineTimer = null;
  let _isoForceRefine = false;
  let _isoRedraw = null;         // set by setRedrawCallback(): re-draws the map after a deferred refine

  function buildIsoTerrainBitmap(t, res) {
    // A private camera of the same kind frames the whole plate in the
    // bitmap, so the bitmap and the live view share one projection.
    const margin = 4;
    const bmW = Math.ceil((t.width + t.height) * res) + margin * 2;
    const bmH = Math.ceil((t.width + t.height) * res * 0.5 + 2 * ISO_SLAB_T * 0.55 * 2 * res) + margin * 2 + Math.ceil(res);
    const canvas = document.createElement("canvas");
    canvas.width = bmW; canvas.height = bmH;
    const cam = global.DashboardCamera.createCamera(t.width, t.height);
    cam.state.viewW = bmW; cam.state.viewH = bmH;
    cam.state.zoom = 2 * res;
    cam.state.x = t.width / 2 - 0.5; cam.state.y = t.height / 2 - 0.5;
    cam.setViewMode("iso");
    paintIsoTerrain(canvas.getContext("2d"), cam, t);
    return { source: t.source, res, canvas, cam, bmW, bmH };
  }

  // Resolution is quantized in 1.25x steps. A zoom gesture is a stream
  // of frames, so the bitmap is NOT rebuilt on every step: it keeps
  // being scaled (accepted between 0.6x and 1.6x of its native size)
  // and is sharpened once, ISO_REFINE_DELAY_MS after the zoom stops
  // (see setRedrawCallback). Only a big jump (a >1.6x blow-up or <0.6x
  // shrink) rebuilds synchronously.
  function isoTerrainBitmap(view, t) {
    const dpr = window.devicePixelRatio || 1;
    const need = view.state.zoom * 0.5 * dpr; // device px per iso-axis world unit on screen
    if (need > ISO_BITMAP_MAX_RES) return null;
    const step = Math.max(0, Math.ceil(Math.log(Math.max(need, ISO_BITMAP_BASE_RES) / ISO_BITMAP_BASE_RES) / Math.log(1.25) - 1e-9));
    const res = ISO_BITMAP_BASE_RES * Math.pow(1.25, step);
    const c = _isoTerrainCache;
    if (c && c.source === t.source) {
      if (c.res === res) return c;
      const ratio = need / c.res;
      if (!_isoForceRefine && ratio >= 0.6 && ratio <= 1.6 && _isoRedraw) {
        if (_isoRefineTimer) clearTimeout(_isoRefineTimer);
        _isoRefineTimer = setTimeout(() => {
          _isoRefineTimer = null;
          _isoForceRefine = true;
          _isoRedraw();            // redraws through here again and rebuilds the bitmap
        }, ISO_REFINE_DELAY_MS);
        return c;
      }
    }
    if (_isoRefineTimer) { clearTimeout(_isoRefineTimer); _isoRefineTimer = null; }
    _isoForceRefine = false;
    _isoTerrainCache = buildIsoTerrainBitmap(t, res);
    return _isoTerrainCache;
  }

  // index.html registers a function that redraws the current frame;
  // the refine timer calls it so the sharper bitmap shows up without
  // waiting for the next tick or mouse move.
  function setRedrawCallback(fn) { _isoRedraw = fn; }

  function drawTerrainIso(ctx, view, t) {
    const bm = isoTerrainBitmap(view, t);
    if (!bm) { paintIsoTerrain(ctx, view, t); return; }
    // The bitmap holds `res` device px per iso-axis unit; the screen
    // wants zoom/2 CSS px per unit, so one bitmap px is k CSS px. The
    // bitmap's centre is the plate's centre, on the ground.
    const k = (view.state.zoom * 0.5) / bm.res;
    const o = view.worldToScreen(bm.cam.state.x, bm.cam.state.y, 0);
    ctx.drawImage(bm.canvas, o.x - k * bm.bmW / 2, o.y - k * bm.bmH / 2, k * bm.bmW, k * bm.bmH);
  }

  // Cell-edge grid for iso (the 2D grid goes through cell centres,
  // which would look off on diamond tiles).
  function drawGridIso(ctx, view, gridN) {
    ctx.save();
    ctx.strokeStyle = "#182028";
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let i = 0; i <= gridN; i++) {
      const e = i - 0.5;
      const a = view.worldToScreen(e, -0.5, 0), b = view.worldToScreen(e, gridN - 0.5, 0);
      ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y);
      const c = view.worldToScreen(-0.5, e, 0), d = view.worldToScreen(gridN - 0.5, e, 0);
      ctx.moveTo(c.x, c.y); ctx.lineTo(d.x, d.y);
    }
    ctx.stroke();
    ctx.restore();
  }

  // ---- scene --------------------------------------------------------

  function drawBuildingBox(ctx, view, e, outline) {
    const P = (x, y, z) => view.worldToScreen(x, y, z);
    const { x0, y0, x1, y1, h } = e;
    // Right face (x = x1), left face (y = y1), then the top. Those are
    // the three faces that can face the viewer in this projection.
    pathPoly(ctx, [P(x1, y0, 0), P(x1, y1, 0), P(x1, y1, h), P(x1, y0, h)]);
    ctx.fillStyle = e.right; ctx.fill();
    pathPoly(ctx, [P(x0, y1, 0), P(x1, y1, 0), P(x1, y1, h), P(x0, y1, h)]);
    ctx.fillStyle = e.left; ctx.fill();
    const top = [P(x0, y0, h), P(x1, y0, h), P(x1, y1, h), P(x0, y1, h)];
    pathPoly(ctx, top);
    ctx.fillStyle = e.top; ctx.fill();
    ctx.strokeStyle = "rgba(230,237,243,0.16)";
    ctx.lineWidth = 1;
    ctx.stroke();
    // Same "damaged" visual language as the 2D layer's hatch.
    if (e.damaged) hatchPoly(ctx, top, e.collapsed ? "rgba(230,237,243,0.32)" : "rgba(230,237,243,0.16)", 4);
    if (outline) {
      pathPoly(ctx, top);
      ctx.strokeStyle = "rgba(255,255,255,0.8)";
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
  }

  function drawShadow(ctx, view, x, y, z, r) {
    pathPoly(ctx, circlePts(view, x, y, r, z, 16));
    ctx.fillStyle = "rgba(0,0,0,0.42)";
    ctx.fill();
  }

  function drawStem(ctx, view, x, y, zFoot, zHead, color) {
    const a = view.worldToScreen(x, y, zFoot), b = view.worldToScreen(x, y, zHead);
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.lineTo(b.x, b.y);
    ctx.strokeStyle = color;
    ctx.globalAlpha = 0.85;
    ctx.lineWidth = 1.5;
    ctx.stroke();
    ctx.globalAlpha = 1;
  }

  // The data side of the iso scene, with no drawing: building boxes
  // (prepareIsoEnv), and for every visible incident cluster and team its
  // world position, footing/head heights, colour and size. 2.5D
  // (drawIsoScene) and the WebGL 3D view (map-view3d.js) both draw from
  // this one model, so heights, grouping, fog-of-war filtering,
  // ON_SCENE offsets and selection can't drift apart between modes.
  // `o`: { env, frame, fogViewMode, selectedMarker, teamColors }.
  const MARKER_WORLD_R = 0.32;
  function buildSceneModel(o) {
    const env = o.env, frame = o.frame || {}, selected = o.selectedMarker;
    const prep = prepareIsoEnv(env);
    const priorityScale = { 1: 1.25, 2: 1.05, 3: 0.9, 4: 0.75 };
    const incidents = buildIncidentClusters(frame.incidents, o.fogViewMode).map((cluster) => {
      const members = cluster.members;
      const inc = members[0];
      const building = (inc.building_id !== undefined && inc.building_id >= 0) ? prep.byId.get(inc.building_id) : null;
      const wx = building ? building.b.x : inc.x, wy = building ? building.b.y : inc.y;
      const worldSize = Math.min(0.9, (0.32 + inc.victims * 0.045)) * (priorityScale[inc.priority] || 0.85);
      const foot = footingZ(prep, wx, wy);
      return {
        members, inc, wx, wy, worldSize, foot, headZ: foot + ISO_INCIDENT_Z,
        color: PRIORITY_COLORS[inc.priority] || "#8b98a5",
        isSel: !!(selected && selected.type === "incident" && members.some((m) => m.id === selected.id)),
      };
    });
    const teams = (frame.teams || []).map((t, i) => {
      // ON_SCENE teams share the incident's ground point; step them
      // off to one side so both markers stay visible (~0.9 marker radii).
      const off = (t.status === 2) ? 0.3 : 0;
      const foot = footingZ(prep, t.x, t.y);
      return {
        t, i, wx: t.x + off, wy: t.y - off, foot, headZ: foot + ISO_TEAM_Z,
        color: o.teamColors[i % o.teamColors.length],
        isSel: !!(selected && selected.type === "team" && selected.id === t.id),
      };
    });
    return {
      prep, incidents, teams,
      hazardCells: frame.hazard_cells || [],
      safezones: (frame.safezones || []).filter((sz) => sz.capacity !== 0),
      epicenter: (env && env.epicenter && env.epicenter.active) ? env.epicenter : null,
    };
  }

  // The whole iso map above the terrain: ground-plane layers first, then
  // buildings and upright markers back to front. Returns hit targets
  // like the 2D draw* functions do (with wz/rpx added - see
  // hitTestWorld). `o`: { env, frame, fogViewMode, selectedMarker,
  // teamColors, outlineBuildingId }.
  function drawIsoScene(ctx, view, o) {
    const hits = [];
    const env = o.env, frame = o.frame || {};
    const selected = o.selectedMarker;
    const model = buildSceneModel(o);
    const prep = model.prep;
    const slope = sightSlope(view);
    const zoom = view.state.zoom;
    const vw = view.state.viewW, vh = view.state.viewH;

    // ---------- ground plane ----------
    if (model.epicenter) {
      const ep = model.epicenter;
      pathPoly(ctx, circlePts(view, ep.x, ep.y, 1.2, 0, 40));
      ctx.strokeStyle = "rgba(163,113,247,0.5)"; // purple, not red - red is reserved
      ctx.lineWidth = 1.5;
      ctx.setLineDash([4, 3]);
      ctx.stroke();
      ctx.setLineDash([]);
    }

    const hatchStep = Math.max(2.5, view.groundPxPerUnit() * 0.22);
    model.hazardCells.forEach((c) => {
      const pts = rectPts(view, c.x - 0.5, c.y - 0.5, c.x + 0.5, c.y + 0.5, 0);
      pathPoly(ctx, pts);
      if (c.blocked) {
        ctx.fillStyle = "rgba(210,153,34,0.14)"; ctx.fill();
        hatchPoly(ctx, pts, "rgba(210,153,34,0.45)", hatchStep);
      } else {
        ctx.fillStyle = c.hazard_flags ? "rgba(210,153,34,0.20)" : "rgba(210,153,34,0.10)";
        ctx.fill();
      }
    });

    model.safezones.forEach((sz) => {
      const half = 0.42;
      pathPoly(ctx, rectPts(view, sz.x - half, sz.y - half, sz.x + half, sz.y + half, 0));
      ctx.fillStyle = "rgba(63,185,80,0.10)"; ctx.fill();
      ctx.strokeStyle = "#3fb950";
      ctx.lineWidth = 1.5;
      ctx.stroke();
      hits.push({ type: "safezone", id: sz.id, wx: sz.x, wy: sz.y, wr: 0.5, data: sz });
    });

    // Where the open incident card's building is: its footprint, on the
    // ground (the roof outline is added when the box is drawn).
    if (o.outlineBuildingId !== undefined && o.outlineBuildingId !== null) {
      const e = prep.byId.get(o.outlineBuildingId);
      if (e) {
        pathPoly(ctx, rectPts(view, e.x0, e.y0, e.x1, e.y1, 0));
        ctx.strokeStyle = "rgba(255,255,255,0.6)";
        ctx.lineWidth = 1;
        ctx.stroke();
      }
    }

    // ---------- upright objects ----------
    const markers = []; // { key, z0, draw(), over() , ...} - ordered below
    const hidden = [];  // markers to re-draw as a faded outline on top

    // Evacuation gateways: upright objects on the map edge like the rest.
    gatewayList(env).forEach((g) => {
      const box = gatewayGlyphBox(view, g);
      const foot = footingZ(prep, g.x, g.y);
      markers.push({ key: g.x + g.y, z0: foot - 1, kind: 0, draw: () => drawGatewayGlyph(ctx, box, box.dir, g.id, true, view), occ: occluders(prep, slope, g.x, g.y, 0.8) });
      hits.push(gatewayHit(g));
    });

    // Incidents.
    model.incidents.forEach((mi) => {
      const { members, inc, wx, wy, worldSize, foot, headZ, color, isSel } = mi;
      const sizePx = worldSize * zoom;
      const draw = () => {
        drawShadow(ctx, view, wx, wy, foot, Math.max(0.3, worldSize * 0.6));
        drawStem(ctx, view, wx, wy, foot, headZ, color);
        drawIncidentGlyph(ctx, view.worldToScreen(wx, wy, headZ), sizePx, color, inc, members, selected);
      };
      const occ = occluders(prep, slope, wx, wy, headZ);
      const m = { key: wx + wy, z0: foot, kind: 1, draw, occ };
      if (occ.length) {
        hidden.push(() => {
          const c = view.worldToScreen(wx, wy, headZ);
          ctx.save();
          ctx.globalAlpha = 0.9;
          ctx.setLineDash([3, 2]);
          ctx.strokeStyle = color;
          ctx.fillStyle = color;
          ctx.lineWidth = 1.5;
          ctx.strokeRect(c.x - sizePx / 2 - 2, c.y - sizePx / 2 - 2, sizePx + 4, sizePx + 4);
          ctx.setLineDash([]);
          ctx.globalAlpha = 0.25;
          ctx.fillRect(c.x - sizePx / 2, c.y - sizePx / 2, sizePx, sizePx);
          if (isSel) {
            ctx.globalAlpha = 1;
            ctx.strokeStyle = "#fff";
            ctx.lineWidth = 2;
            ctx.strokeRect(c.x - sizePx / 2 - 5, c.y - sizePx / 2 - 5, sizePx + 10, sizePx + 10);
          }
          ctx.restore();
        });
      }
      markers.push(m);
      const hitData = members.length > 1 ? Object.assign({}, inc, { groupIncidents: members }) : inc;
      hits.push({ type: "incident", id: inc.id, wx, wy, wz: headZ, wr: Math.max(worldSize / 2, 0.4),
                  rpx: Math.max(sizePx / 2, 0.4 * zoom) + 2, data: hitData });
      if (isSel) {
        pathPoly(ctx, circlePts(view, wx, wy, Math.max(worldSize / 2, 0.4) + 0.25, foot, 24));
        ctx.strokeStyle = "rgba(255,255,255,0.7)";
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
    });

    // Teams.
    const markerWorldR = MARKER_WORLD_R;
    model.teams.forEach((mt) => {
      const { t, i, wx, wy, foot, headZ, color, isSel } = mt;
      const r = markerWorldR * zoom;

      if (t.has_dest && (t.dest_x !== t.x || t.dest_y !== t.y)) {
        const a = view.worldToScreen(wx, wy, 0), b = view.worldToScreen(t.dest_x, t.dest_y, 0);
        ctx.beginPath();
        ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y);
        ctx.strokeStyle = color;
        ctx.globalAlpha = 0.35;
        ctx.lineWidth = 1;
        ctx.setLineDash([3, 3]);
        ctx.stroke();
        ctx.setLineDash([]);
        ctx.globalAlpha = 1;
      }
      if (isSel) {
        pathPoly(ctx, circlePts(view, wx, wy, 0.6, foot, 24));
        ctx.strokeStyle = "rgba(255,255,255,0.7)";
        ctx.lineWidth = 1.5;
        ctx.stroke();
      }
      const draw = () => {
        drawShadow(ctx, view, wx, wy, foot, 0.34);
        drawStem(ctx, view, wx, wy, foot, headZ, color);
        drawTeamGlyph(ctx, view.worldToScreen(wx, wy, headZ), r, color, t, selected);
      };
      const occ = occluders(prep, slope, wx, wy, headZ);
      markers.push({ key: wx + wy, z0: foot, kind: 2, draw, occ });
      if (occ.length) {
        hidden.push(() => {
          // Faded "x-ray" version of the marker on top of whatever hides it.
          const c = view.worldToScreen(wx, wy, headZ);
          ctx.save();
          ctx.globalAlpha = 0.9;
          ctx.setLineDash([3, 2]);
          ctx.strokeStyle = color;
          ctx.lineWidth = 2;
          ctx.beginPath(); ctx.arc(c.x, c.y, r * 1.55, 0, Math.PI * 2); ctx.stroke();
          ctx.setLineDash([]);
          ctx.globalAlpha = 0.3;
          ctx.fillStyle = color;
          ctx.beginPath(); ctx.arc(c.x, c.y, r, 0, Math.PI * 2); ctx.fill();
          ctx.globalAlpha = 0.9;
          if (r >= 6) {
            ctx.fillStyle = "#e6edf3";
            ctx.font = `bold ${Math.max(8, r)}px sans-serif`;
            ctx.textAlign = "center";
            ctx.textBaseline = "middle";
            ctx.fillText(String(t.id), c.x, c.y + 0.5);
          }
          if (isSel) {
            ctx.strokeStyle = "#fff";
            ctx.lineWidth = 2;
            ctx.beginPath(); ctx.arc(c.x, c.y, r * 1.9, 0, Math.PI * 2); ctx.stroke();
          }
          ctx.restore();
        });
      }
      hits.push({ type: "team", id: t.id, wx, wy, wz: headZ, wr: markerWorldR + 0.18, rpx: 0.5 * zoom, data: { ...t, i } });
    });

    // ---------- painter's order ----------
    // Back to front: buildings by x+y (centre) then height. A marker is
    // ordered against the buildings by the exact line-of-sight test
    // above rather than by x+y alone (a long building's centre can be
    // "behind" a marker that is really in front of its face): it is
    // drawn just before the first building that hides it (so that
    // building covers it), or after all of them if none does. Markers
    // among themselves go by x+y, then footing height.
    const onScreen = [];
    prep.list.forEach((e) => {
      const c = view.worldToScreen(e.b.x, e.b.y, 0);
      const reach = (e.x1 - e.x0 + e.y1 - e.y0) * zoom * 0.5 + view.heightPx(e.h) + 4;
      if (c.x < -reach || c.x > vw + reach || c.y < -reach - view.heightPx(e.h) || c.y > vh + reach) return;
      onScreen.push(e);
    });
    onScreen.sort((a, b) => (a.key - b.key) || (a.h - b.h));
    const order = new Map(onScreen.map((e, idx) => [e, idx]));
    const before = new Map(); // building index -> markers drawn right before it
    const after = [];
    markers.forEach((m) => {
      let first = Infinity;
      for (const e of m.occ) { const idx = order.get(e); if (idx !== undefined && idx < first) first = idx; }
      if (first === Infinity) after.push(m);
      else { if (!before.has(first)) before.set(first, []); before.get(first).push(m); }
    });
    const byDepth = (a, b) => (a.key - b.key) || (a.z0 - b.z0) || (a.kind - b.kind);
    onScreen.forEach((e, idx) => {
      const list = before.get(idx);
      if (list) list.sort(byDepth).forEach((m) => m.draw());
      drawBuildingBox(ctx, view, e, e.id === o.outlineBuildingId);
    });
    after.sort(byDepth).forEach((m) => m.draw());

    hidden.forEach((fn) => fn());
    return hits;
  }

  // Where a drop-target highlight ring goes for a hit, in screen space:
  // around the marker as drawn (its head in iso), so index.html needn't
  // know which mode it is in.
  function markerScreenGeom(view, hit) {
    const c = view.worldToScreen(hit.wx, hit.wy, hit.wz || 0);
    return { x: c.x, y: c.y, r: hit.rpx !== undefined ? hit.rpx : hit.wr * view.state.zoom };
  }

  // The 2D terrain look (flood, blocked hatch, road network, district
  // borders) rendered once, top-down, at `pxPerCell` px per cell into a
  // canvas covering the whole map - by pointing the existing 2D terrain
  // renderer at a private camera. The WebGL view uses it as the ground
  // texture, so 3D terrain is the same drawing code as 2D, not a copy.
  function renderTerrainTexture(terrainMap, pxPerCell) {
    const t = parseTerrain(terrainMap);
    if (!t) return null;
    const cam = global.DashboardCamera.createCamera(t.width, t.height);
    cam.state.viewW = Math.round(t.width * pxPerCell);
    cam.state.viewH = Math.round(t.height * pxPerCell);
    cam.state.zoom = pxPerCell;
    cam.state.x = t.width / 2 - 0.5;
    cam.state.y = t.height / 2 - 0.5;
    return { canvas: renderTerrainBitmap(cam, t, 1), source: t.source, width: t.width, height: t.height };
  }

  // World-space hit test: converts the screen point to world space
  // once, then compares against each target's world-space radius, so
  // hit-testing is correct at any zoom level without per-frame tuning.
  //
  // A target may carry `wz` (the height of the thing you actually see,
  // e.g. an upright marker's head in iso): the click is then converted
  // on THAT plane (screenToWorld's z argument), so clicking the visible
  // marker lands exactly on its world point. If it also carries `rpx`
  // (the drawn marker's radius in screen px) the world offset is turned
  // back into a screen distance through the camera's linear part, so
  // the hit area is the disc you see rather than a world circle -
  // which on an iso screen is a squashed ellipse. 2D targets have
  // neither and take the plain world-radius path, unchanged.
  function hitTestWorld(view, hits, sx, sy, typeFilter) {
    const w0 = view.screenToWorld(sx, sy);
    let best = null, bestDist = Infinity;
    for (const h of hits) {
      if (typeFilter && h.type !== typeFilter) continue;
      const w = h.wz ? view.screenToWorld(sx, sy, h.wz) : w0;
      const dx = h.wx - w.x, dy = h.wy - w.y;
      let dist, limit;
      if (h.rpx !== undefined) {
        // Both sides in "world-unit equivalents" (px / ground px per
        // unit) so "nearest wins" stays comparable across target kinds.
        const d = view.worldDeltaToScreen(dx, dy, h.wx, h.wy, h.wz || 0), g = view.groundPxPerUnit(h.wx, h.wy);
        dist = Math.sqrt(d.x * d.x + d.y * d.y) / g;
        limit = h.rpx / g;
      } else {
        dist = Math.sqrt(dx * dx + dy * dy);
        limit = h.wr;
      }
      const rank = dist + (h.low ? 1e6 : 0);        // a "low" target (gateway) only wins when no other target is in reach
      if (dist <= limit && rank < bestDist) { best = h; bestDist = rank; }
    }
    return best;
  }

  global.MapRender = {
    drawGrid, drawTerrain, drawBuildings, drawEpicenter, drawHazardCells,
    drawSafezones, drawIncidents, drawTeams, hitTestWorld,
    gatewayList, gatewayOutward, drawGateways, drawGatewayGlyph, gatewayGlyphBox, GATEWAY_COLOR,
    buildingFootprintWorldRect,
    drawIsoScene, markerScreenGeom, buildingHeight, setRedrawCallback,
    // Shared with the WebGL view (map-view3d.js) so 3D reuses this data
    // preparation and these marker glyphs instead of re-deriving them.
    buildSceneModel, renderTerrainTexture, drawTeamGlyph, drawIncidentGlyph,
    MARKER_WORLD_R, ISO_TEAM_Z, ISO_INCIDENT_Z, PRIORITY_COLORS,
    parseTerrain, footingZ, ISO_FLOOR_H,
    ENV_DAMAGE_MINOR, ENV_DAMAGE_MODERATE, ENV_DAMAGE_SEVERE,
  };
})(window);

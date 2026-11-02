/* RescueNet map context: the city around the simulated area, drawn faded so the edge of the map reads as the edge of
   the run, not the edge of the city. It is NOT simulated (no teams, no incidents, nothing selectable).
   Preferred source: eight more cities from the simulator's own map generator, laid around the run so their streets and
   water meet it (laptop/tools/map_context.py, step 2 of two); build(run, ring, given) takes them as they are and only
   colours them. Without them, build(run, ring) falls back to continuing what the run shows at its edge, by these rules:
     - water: a coast along one side continues as open sea beyond that side; a river leaving the map keeps its width and
       meanders on;
     - roads: every road that leaves the map carries on outward at its class (main roads to the edge of the ring, side
       streets until they meet a cross street); cross streets run parallel to each side; a main road over water is a bridge;
     - buildings: fill the land between those roads, in the same sizes as the simulated area's buildings;
     - damage: the simulator's own earthquake rule (environment.c, earthquake_damage_at): damage falls by 3 for every cell
       of distance from the epicentre (city-block distance), with the base level and spread measured from the simulated
       buildings, so the colours carry on across the edge.
   Deterministic: the same run always gets the same surroundings (hashing the seed and the cell; no random stream).
   RNContext.build(run) -> context; RNContext.draw(ctx2d, context, q, ox, oy, style) paints it. */
(function (root) {
  'use strict';
  var STYLE = { water: '#1d4263', shore: '#4f8bbd', roadW: [0, .26, .4, .56, .78], roadA: [0, .2, .3, .4, .55] };

  function hash(x, y, s) {
    var h = Math.imul(x | 0, 73856093) ^ Math.imul(y | 0, 19349663) ^ Math.imul(s | 0, 83492791);
    h = Math.imul(h ^ (h >>> 13), 1274126177); h ^= h >>> 16; return (h >>> 0) / 4294967296;
  }

  function colourByDistance(run, bs, seed) {
    var W = run.W, H = run.H;
    /* damage: continues the run's own damage-by-distance curve (city-block distance from the epicentre, as in the
       simulator's earthquake rule). Mean damage is measured per distance from the run's buildings, kept non-increasing,
       and carried on past the farthest building at the steeper of its own tail slope and the simulator's 3 per cell;
       each building then takes the leftover of a run building at a similar distance, so the mix of colours carries on too. */
    var ep = run.epicentre || { x: W / 2, y: H / 2 }, centre = function (b) { return [b.x + Math.floor(b.w / 2), b.y + Math.floor(b.h / 2)]; };
    var dist = function (b) { var c = centre(b); return Math.abs(c[0] - ep.x) + Math.abs(c[1] - ep.y); };
    var clamp = function (v) { return Math.max(0, Math.min(100, v)); };
    var obs = (run.buildings || []).map(function (b) { return [dist(b), b.damage]; }).sort(function (p, q2) { return p[0] - q2[0]; });
    var curve = [], dmax = 0;
    if (obs.length) {
      dmax = obs[obs.length - 1][0];
      for (var dd = 0; dd <= dmax; dd++) {   /* mean of the run's buildings within 3 cells of this distance */
        var sum = 0, cnt = 0; obs.forEach(function (o) { if (Math.abs(o[0] - dd) <= 3) { sum += o[1]; cnt++; } });
        curve[dd] = cnt ? sum / cnt : (dd ? curve[dd - 1] : 100);
      }
      for (dd = 1; dd <= dmax; dd++) curve[dd] = Math.min(curve[dd], curve[dd - 1]);   /* damage never grows with distance */
    }
    var tail = dmax > 8 ? Math.max(3, (curve[dmax - 8] - curve[dmax]) / 8) : 3;
    var meanAt = function (d) { return !obs.length ? 0 : d <= dmax ? curve[d] : curve[dmax] - tail * (d - dmax); };
    var resid = obs.map(function (o) { return [o[0], o[1] - meanAt(o[0])]; });
    bs.forEach(function (b) {
      var d = dist(b), m = meanAt(d), near = resid.filter(function (r) { return Math.abs(r[0] - Math.min(d, dmax)) <= 4; });
      var r = near.length ? near[Math.floor(hash(b.x, b.y, seed + 31) * near.length)][1] : 0;
      b.damage = clamp(Math.round(m + r * Math.max(0, Math.min(1, m / 50))));   /* the spread shrinks as the mean nears 0 */
    });

  }

  /* the surroundings built by the simulator's own generator (laptop/tools/map_context.py, step 2 of two): taken as
     they are, only coloured here. given: { roads: [[x, y, class]], water: [[x, y]], bridges: [[x, y]], buildings: [[x, y, w, h]] } */
  function fromGenerator(run, ring, given) {
    var W = run.W, H = run.H, R = ring || given.ring || 36, GW = W + 2 * R, GH = H + 2 * R, seed = run.seed | 0;
    var road = new Uint8Array(GW * GH), water = new Uint8Array(GW * GH);
    var at = function (x, y) { return (y + R) * GW + (x + R); }, inGrid = function (x, y) { return x >= -R && y >= -R && x < W + R && y < H + R; };
    (run.roads || []).forEach(function (r) { road[at(r[0], r[1])] = r[2] || 2; });
    (run.water || []).forEach(function (c) { water[at(c[0], c[1])] = 1; });
    given.roads.forEach(function (r) { if (inGrid(r[0], r[1])) road[at(r[0], r[1])] = r[2]; });
    given.water.forEach(function (c) { if (inGrid(c[0], c[1])) water[at(c[0], c[1])] = 1; });
    var bs = given.buildings.map(function (b) { return { x: b[0], y: b[1], w: b[2], h: b[3] }; });
    colourByDistance(run, bs, seed);
    return { W: W, H: H, R: R, roads: given.roads, water: given.water, bridges: given.bridges || [], buildings: bs,
      roadAt: function (x, y) { return inGrid(x, y) ? road[at(x, y)] : 0; }, waterAt: function (x, y) { return inGrid(x, y) ? water[at(x, y)] : 0; } };
  }

  /* run: { W, H, seed, roads: [[x, y, class]], water: [[x, y]], buildings: [{ x, y, w, h, damage }] (top-left cells),
     epicentre: { x, y } }. ring: how many cells the context reaches out on each side (one sector: 36). */
  function build(run, ring, given) {
    if (given) return fromGenerator(run, ring, given);
    var W = run.W, H = run.H, R = ring || 36, seed = run.seed | 0, GW = W + 2 * R, GH = H + 2 * R;
    var road = new Uint8Array(GW * GH), water = new Uint8Array(GW * GH), used = new Uint8Array(GW * GH), seaAt = new Uint8Array(GW * GH);
    var at = function (x, y) { return (y + R) * GW + (x + R); };
    var inside = function (x, y) { return x >= 0 && y >= 0 && x < W && y < H; };
    var inGrid = function (x, y) { return x >= -R && y >= -R && x < W + R && y < H + R; };
    (run.roads || []).forEach(function (r) { road[at(r[0], r[1])] = r[2] || 2; });
    (run.water || []).forEach(function (c) { water[at(c[0], c[1])] = 1; });
    (run.buildings || []).forEach(function (b) { for (var x = b.x; x < b.x + b.w; x++) for (var y = b.y; y < b.y + b.h; y++) if (inside(x, y)) used[at(x, y)] = 1; });

    /* the four sides: walking outward from each, with "along" the coordinate that runs beside it */
    var SIDES = [
      { name: 'L', n: H, edge: function (i) { return [0, i]; }, out: function (i, d) { return [-1 - d, i]; } },
      { name: 'R', n: H, edge: function (i) { return [W - 1, i]; }, out: function (i, d) { return [W + d, i]; } },
      { name: 'T', n: W, edge: function (i) { return [i, 0]; }, out: function (i, d) { return [i, -1 - d]; } },
      { name: 'B', n: W, edge: function (i) { return [i, H - 1]; }, out: function (i, d) { return [i, H + d]; } }
    ];
    var runsOf = function (side, test) {   /* stretches of consecutive boundary cells that pass test */
      var out = [], s = -1;
      for (var i = 0; i <= side.n; i++) {
        var e = i < side.n ? side.edge(i) : null, ok = e && test(e[0], e[1]);
        if (ok && s < 0) s = i;
        if (!ok && s >= 0) { out.push([s, i - 1]); s = -1; }
      }
      return out;
    };

    /* ---- water ---- */
    var sea = {};
    SIDES.forEach(function (sd) {
      var n = 0; for (var i = 0; i < sd.n; i++) { var e = sd.edge(i); if (water[at(e[0], e[1])]) n++; }
      sea[sd.name] = n >= sd.n * .85;
    });
    for (var gy = -R; gy < H + R; gy++) for (var gx = -R; gx < W + R; gx++) {
      if (inside(gx, gy)) continue;
      if ((sea.L && gx < 0) || (sea.R && gx >= W) || (sea.T && gy < 0) || (sea.B && gy >= H)) { water[at(gx, gy)] = 1; seaAt[at(gx, gy)] = 1; }
    }
    SIDES.forEach(function (sd, k) {
      if (sea[sd.name]) return;
      runsOf(sd, function (x, y) { return water[at(x, y)]; }).forEach(function (rn) {
        var touchesCorner = rn[0] === 0 || rn[1] === sd.n - 1;   /* a coast band running along the map, not a river */
        var lo = rn[0], hi = rn[1], drift = 0;
        for (var d = 0; d < R; d++) {
          if (!touchesCorner && d > 0) {   /* a river meanders: now and then it steps one cell sideways */
            var h1 = hash(k * 7919 + d, lo, seed + 11);
            if (h1 < .18) drift = -1; else if (h1 > .82) drift = 1; else if (h1 > .4 && h1 < .6) drift = 0;
            lo += drift; hi += drift;
          }
          for (var i = lo; i <= hi; i++) { var c = sd.out(i, d); if (inGrid(c[0], c[1])) { water[at(c[0], c[1])] = 1; if (touchesCorner) seaAt[at(c[0], c[1])] = 1; } }
        }
      });
    });

    /* ---- roads ---- */
    var paint = function (x, y, cls) {
      if (!inGrid(x, y) || inside(x, y)) return false;
      if (water[at(x, y)] && (cls < 3 || seaAt[at(x, y)])) return false;   /* roads stop at the sea and side streets at a river; main roads bridge a river */
      if (road[at(x, y)] < cls) road[at(x, y)] = cls; return true;
    };
    /* cross streets: lines parallel to each side, a few cells out and then every 6-9 cells, broken into blocks */
    var cross = {};
    SIDES.forEach(function (sd, k) {
      var lines = [], d = 3 + Math.floor(hash(k, 1, seed + 3) * 3);
      while (d < R) { lines.push(d); d += 6 + Math.floor(hash(k, d, seed + 5) * 4); }
      cross[sd.name] = lines;
      lines.forEach(function (dd, li) {
        var cls = li % 3 === 1 ? 3 : 2;
        for (var i = -R; i < sd.n + R; i++) {
          if (hash(Math.floor(i / 7), dd * 31 + k, seed + 9) < .12) continue;   /* a missing block now and then */
          var c = sd.out(i, dd); paint(c[0], c[1], cls);
        }
      });
    });
    /* roads leaving the simulated area carry straight on */
    SIDES.forEach(function (sd, k) {
      /* the simulator's roads often stop a cell or two short of the edge: a road counts as leaving the map if it runs
         towards the edge (two road cells in a row, square to it) and ends within two cells of it */
      var inward = function (i, k) { var e = sd.edge(i), o = sd.out(i, 0); return [e[0] + (e[0] - o[0]) * k, e[1] + (e[1] - o[1]) * k]; };
      var leaving = function (i) {
        for (var k = 0; k <= 2; k++) { var c = inward(i, k), c2 = inward(i, k + 1); if (road[at(c[0], c[1])]) return road[at(c2[0], c2[1])] ? road[at(c[0], c[1])] : 0; }
        return 0;
      };
      var runs = [], s0 = -1;
      for (var i = 0; i <= sd.n; i++) { var lv = i < sd.n && leaving(i); if (lv && s0 < 0) s0 = i; if (!lv && s0 >= 0) { runs.push([s0, i - 1]); s0 = -1; } }
      runs.forEach(function (rn) {
        var cls = 0; for (var i = rn[0]; i <= rn[1]; i++) cls = Math.max(cls, leaving(i));
        var reach = cls >= 3 ? R : cross[sd.name][0] + 1 + (hash(rn[0], k, seed + 13) < .5 ? (cross[sd.name][1] || R) - cross[sd.name][0] : 0);
        for (var d = 0; d < Math.min(R, reach); d++) {
          var stop = false;
          for (var j = rn[0]; j <= rn[1]; j++) { var c = sd.out(j, d); if (!paint(c[0], c[1], cls)) stop = true; }
          if (stop && cls < 3) break;
        }
      });
    });
    /* side streets between them, from the first cross street outward, so the blocks are not one long strip */
    SIDES.forEach(function (sd, k) {
      var first = cross[sd.name][0];
      for (var i = -R; i < sd.n + R; i += 6 + Math.floor(hash(i, k, seed + 17) * 4)) {
        if (hash(i, k, seed + 19) < .3) continue;
        for (var d = first; d < R; d++) { var c = sd.out(i, d); if (!paint(c[0], c[1], 2) && water[at(c[0], c[1])]) break; }
      }
    });

    /* ---- buildings: the simulated area's sizes and density ---- */
    var sizes = (run.buildings || []).map(function (b) { return [b.w, b.h]; }); if (!sizes.length) sizes = [[1, 1], [2, 1], [1, 2]];
    var land = 0, built = 0;
    for (var y0 = 0; y0 < H; y0++) for (var x0 = 0; x0 < W; x0++) { var ix = at(x0, y0); if (!water[ix] && !road[ix]) { land++; if (used[ix]) built++; } }
    var density = Math.min(.9, Math.max(.25, land ? built / land : .5)) * 1.15;
    var bs = [];
    for (var by = -R; by < H + R; by++) for (var bx = -R; bx < W + R; bx++) {
      if (inside(bx, by)) continue;
      var i0 = at(bx, by); if (water[i0] || road[i0] || used[i0]) continue;
      if (hash(bx, by, seed + 23) > density * .55) continue;
      var sz = sizes[Math.floor(hash(by, bx, seed + 29) * sizes.length)], bw = sz[0], bh = sz[1], ok = true;
      for (var xx = bx; xx < bx + bw && ok; xx++) for (var yy = by; yy < by + bh && ok; yy++) {
        if (!inGrid(xx, yy) || inside(xx, yy)) { ok = false; break; }
        var ii = at(xx, yy); if (water[ii] || road[ii] || used[ii]) ok = false;
      }
      if (!ok) { bw = 1; bh = 1; }
      for (xx = bx; xx < bx + bw; xx++) for (yy = by; yy < by + bh; yy++) used[at(xx, yy)] = 1;
      bs.push({ x: bx, y: by, w: bw, h: bh });
    }
    colourByDistance(run, bs, seed);

    var roads = [], waters = [], bridges = [];
    for (var cy = -R; cy < H + R; cy++) for (var cx = -R; cx < W + R; cx++) {
      if (inside(cx, cy)) continue; var k0 = at(cx, cy);
      if (water[k0]) waters.push([cx, cy]);
      if (road[k0]) { roads.push([cx, cy, road[k0]]); if (water[k0]) bridges.push([cx, cy]); }
    }
    return { W: W, H: H, R: R, roads: roads, water: waters, bridges: bridges, buildings: bs, roadAt: function (x, y) { return inGrid(x, y) ? road[at(x, y)] : 0; }, waterAt: function (x, y) { return inGrid(x, y) ? water[at(x, y)] : 0; } };
  }

  /* how strongly a cell outside the area is drawn: full near the green line, fading to nothing at the ring's edge */
  function fade(cx, ctx) {
    var d = Math.max(-1 - cx[0], cx[0] - ctx.W, -1 - cx[1], cx[1] - ctx.H, 0);
    var k = Math.max(0, 1 - d / ctx.R); return k * k * .8;
  }
  var BANDS = 16;
  function band(c, ctx) { return Math.min(BANDS - 1, Math.floor((1 - fade(c, ctx) / .8) * BANDS)); }
  function bandAlpha(b) { var k = 1 - (b + .5) / BANDS; return k * k * .8; }

  /* paint the context. x: a 2D context already translated so that cell (0, 0) of the simulated area is at (ox, oy);
     q: pixels per cell. style: { ink, bg, mute, view: [x0, y0, x1, y1] in pixels (optional, to skip what is off screen) } */
  function draw(x, ctx, q, ox, oy, style, N) {
    var ink = style.ink, view = style.view, S = STYLE;
    var vis = function (cx, cy, w, h) { if (!view) return true; var px = ox + cx * q, py = oy + cy * q; return px + (w || 1) * q >= view[0] && py + (h || 1) * q >= view[1] && px <= view[2] && py <= view[3]; };
    x.save();
    for (var b = 0; b < BANDS; b++) {
      var a = bandAlpha(b); if (a < .01) continue;
      /* water */
      x.fillStyle = S.water;
      ctx.water.forEach(function (c) { if (band(c, ctx) === b && vis(c[0], c[1])) { x.globalAlpha = fade(c, ctx); x.fillRect(ox + c[0] * q, oy + c[1] * q, q + .5, q + .5); } });
      /* roads, joined to their neighbours */
      x.lineCap = 'round'; x.strokeStyle = ink;
      for (var cls = 1; cls <= 4; cls++) {
        x.globalAlpha = a * S.roadA[cls] / .55 * .55; x.lineWidth = q * S.roadW[cls]; x.beginPath();
        ctx.roads.forEach(function (r) {
          if (r[2] !== cls || band(r, ctx) !== b || !vis(r[0], r[1])) return;
          var px = ox + (r[0] + .5) * q, py = oy + (r[1] + .5) * q;
          [[1, 0], [0, 1], [-1, 0], [0, -1]].forEach(function (d) {
            var k = ctx.roadAt(r[0] + d[0], r[1] + d[1]); if (!k) return;
            var nx = r[0] + d[0], ny = r[1] + d[1], inner = nx >= 0 && ny >= 0 && nx < ctx.W && ny < ctx.H;
            if (!inner && (Math.min(k, cls) !== cls || (k === cls && (d[0] < 0 || d[1] < 0)))) return;
            x.moveTo(px, py); x.lineTo(px + d[0] * q * (inner ? .5 : 1), py + d[1] * q * (inner ? .5 : 1));
          });
        });
        x.stroke();
      }
      /* buildings, in the damage colours */
      ctx.buildings.forEach(function (bd) {
        if (band([bd.x, bd.y], ctx) !== b || !vis(bd.x, bd.y, bd.w, bd.h)) return;
        x.globalAlpha = fade([bd.x, bd.y], ctx); N.building(x, ox + (bd.x + .1) * q, oy + (bd.y + .1) * q, (bd.w - .2) * q, (bd.h - .2) * q, bd.damage, style.mute || ink, 1);
      });
    }
    /* bridges: the cased edges, faded with the rest */
    ctx.bridges.forEach(function (c) {
      if (!vis(c[0], c[1])) return;
      var horiz = ctx.roadAt(c[0] - 1, c[1]) || ctx.roadAt(c[0] + 1, c[1]), cls = ctx.roadAt(c[0], c[1]) || 3, of = q * (S.roadW[cls] / 2 + .14), L = q * .5;
      x.save(); x.globalAlpha = bandAlpha(band(c, ctx)) * 1.1; x.translate(ox + (c[0] + .5) * q, oy + (c[1] + .5) * q); if (!horiz || ctx.waterAt(c[0], c[1] - 1) && ctx.waterAt(c[0], c[1] + 1) && !ctx.waterAt(c[0] - 1, c[1])) x.rotate(Math.PI / 2);
      x.strokeStyle = ink; x.lineWidth = Math.max(1, q * .09); x.beginPath(); [-1, 1].forEach(function (sd) { x.moveTo(-L, sd * of); x.lineTo(L, sd * of); }); x.stroke(); x.restore();
    });
    x.restore();
  }

  root.RNContext = { build: build, draw: draw, STYLE: STYLE, hash: hash };
})(typeof window !== 'undefined' ? window : globalThis);

/*
 * laptop/tools/static/dashboard/route.js
 *
 * Reconstructs a team's route from data the dashboard already has. The
 * simulator never sends paths, only each team's position and
 * destination - but its router (core/path.c, path_search) uses nothing
 * except each cell's `navigable` flag and `travel_cost`, and both ride
 * in STATICMAP. This is a line-for-line port of that search (8-way A*
 * in tenths of a cost unit, straight 10 / diagonal 14, no corner
 * cutting, the same binary heap with decrease-key and the same
 * neighbour order, so exact-cost ties resolve the same way), returning
 * the whole route rather than just the first step.
 *
 * It is a reconstruction, not a recording: it is only as current as the
 * STATICMAP the dashboard holds (in Replay that is the run's final map,
 * see replay.js), and it assumes the simulator's own routing inputs
 * (equipment flags are ignored by core/path.c today). e2e checks it
 * against the moves the simulator actually made.
 *
 * `terrain` is MapRender's parsed terrain: { width, height, cells[] }
 * with cells[gy * width + gx] = { navigable, travelCost, ... }.
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.RoutePlanner = factory();
})(typeof window !== "undefined" ? window : this, function () {
  "use strict";

  const STRAIGHT = 10, DIAGONAL = 14, UNREACHED = 0x7fffffff;
  const DX = [1, -1, 0, 0, 1, 1, -1, -1];
  const DY = [0, 0, 1, -1, 1, -1, 1, -1];
  const UNVISITED = 0, OPEN = 1, CLOSED = 2;

  let cap = 0, g = null, parent = null, state = null, heap = null, heapPos = null, heapSize = 0;
  let W = 0, H = 0, destX = 0, destY = 0;

  function alloc(n) {
    if (n <= cap) return;
    cap = n;
    g = new Int32Array(n); parent = new Int32Array(n); state = new Uint8Array(n);
    heap = new Int32Array(n); heapPos = new Int32Array(n);
  }

  // Cell index is x*H+y, as in the C (order only matters for being unique).
  const idx = (x, y) => x * H + y;
  function fcost(cell) {
    const x = (cell / H) | 0, y = cell % H;
    const ax = Math.abs(x - destX), ay = Math.abs(y - destY);
    const diag = Math.min(ax, ay), straight = Math.max(ax, ay) - diag;
    return g[cell] + straight * STRAIGHT + diag * DIAGONAL;
  }
  function swap(i, j) {
    const ci = heap[i], cj = heap[j];
    heap[i] = cj; heap[j] = ci; heapPos[ci] = j; heapPos[cj] = i;
  }
  function siftUp(i) {
    while (i > 0) {
      const p = (i - 1) >> 1;
      if (fcost(heap[i]) >= fcost(heap[p])) break;
      swap(i, p); i = p;
    }
  }
  function siftDown(i) {
    for (;;) {
      const l = 2 * i + 1, r = 2 * i + 2;
      let s = i;
      if (l < heapSize && fcost(heap[l]) < fcost(heap[s])) s = l;
      if (r < heapSize && fcost(heap[r]) < fcost(heap[s])) s = r;
      if (s === i) break;
      swap(i, s); i = s;
    }
  }
  function push(cell) { const i = heapSize; heap[i] = cell; heapPos[cell] = i; heapSize++; siftUp(i); }
  function popMin() {
    if (heapSize === 0) return -1;
    const root = heap[0];
    heapSize--;
    if (heapSize > 0) { heap[0] = heap[heapSize]; heapPos[heap[0]] = 0; siftDown(0); }
    heapPos[root] = -1;
    return root;
  }

  function stepCost(cells, fx, fy, tx, ty) {
    const c = cells[ty * W + tx];
    if (!c || !c.navigable) return -1;
    let cost = c.travelCost; if (cost < 1) cost = 1;
    if (fx !== tx && fy !== ty) {
      const a = cells[fy * W + tx], b = cells[ty * W + fx];
      if (!a || !a.navigable || !b || !b.navigable) return -1;
      return cost * DIAGONAL;
    }
    return cost * STRAIGHT;
  }

  // Route from (sx, sy) to (dx, dy) as [[x, y], ...] including both ends,
  // or null if there is none. Like the C, only the destination has to be
  // navigable - a team may always attempt to leave where it stands.
  function plan(terrain, sx, sy, dx, dy) {
    if (!terrain) return null;
    W = terrain.width; H = terrain.height;
    const inB = (x, y) => x >= 0 && x < W && y >= 0 && y < H;
    if (!inB(sx, sy) || !inB(dx, dy)) return null;
    const dc = terrain.cells[dy * W + dx];
    if (!dc || !dc.navigable) return null;
    if (sx === dx && sy === dy) return [[sx, sy]];
    alloc(W * H);
    destX = dx; destY = dy;
    const n = W * H;
    for (let i = 0; i < n; i++) { g[i] = UNREACHED; parent[i] = i; state[i] = UNVISITED; heapPos[i] = -1; }
    heapSize = 0;
    const s = idx(sx, sy);
    g[s] = 0; parent[s] = s; state[s] = OPEN;
    push(s);
    const cells = terrain.cells;
    for (;;) {
      const cur = popMin();
      if (cur < 0) return null;
      const cx = (cur / H) | 0, cy = cur % H;
      if (cx === dx && cy === dy) break;
      state[cur] = CLOSED;
      for (let i = 0; i < 8; i++) {
        const nx = cx + DX[i], ny = cy + DY[i];
        if (!inB(nx, ny)) continue;
        const ni = idx(nx, ny);
        if (state[ni] === CLOSED) continue;
        const mc = stepCost(cells, cx, cy, nx, ny);
        if (mc < 0) continue;
        const ng = g[cur] + mc;
        if (ng < g[ni]) {
          g[ni] = ng; parent[ni] = cur;
          if (state[ni] === OPEN) siftUp(heapPos[ni]); else push(ni);
          state[ni] = OPEN;
        }
      }
    }
    const d = idx(dx, dy);
    if (g[d] === UNREACHED) return null;
    const out = [];
    let c = d;
    for (let guard = 0; guard < n; guard++) {
      out.push([(c / H) | 0, c % H]);
      if (c === s) break;
      const p = parent[c];
      if (p === c) return null;
      c = p;
    }
    return out.reverse();
  }

  return { plan };
});

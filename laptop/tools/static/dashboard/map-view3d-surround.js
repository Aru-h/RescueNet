/*
 * laptop/tools/static/dashboard/map-view3d-surround.js
 *
 * What lies around the city in the 3D view, and the evacuation gateways in
 * it: a ring of terrain (a bowl of forest and rock, or bare rock, or snow)
 * that rises from the map edge and hides the void beyond, and for every
 * gateway a tunnel portal cut into the rim at the end of the road that
 * reaches the map edge, with a green beacon and an "EXIT n" label.
 *
 * Purely decorative and dashboard-side: it is derived from
 *   - the run's seed (so a run always gets the same landscape, and a replay
 *     shows the same one),
 *   - the tallest building (so the rim is about as tall as the city: it
 *     belongs to the same world),
 *   - STATICENV's "gateways" (nothing else about them is known).
 * Rudimentary on purpose; meant to be reworked later.
 *
 * No transparent depth writes except the beacon halo (which does not write
 * depth): a transparent sprite that writes depth would cut square holes in
 * the fire / smoke drawn behind it (see the pin fix in map-view3d.js); the
 * label uses alphaTest instead.
 */
const GREEN = "#2fe0a0";   // same mint as the 2D glyph (MapRender.GATEWAY_COLOR)

const BIOMES = [
  { name: "forest", low: 0x1b2d2c, mid: 0x26403a, rock: 0x4a5663, peak: 0x8794a3, tree: 0x1f3d34, trees: 900 },
  { name: "rock",   low: 0x2a2f36, mid: 0x3f4650, rock: 0x59626e, peak: 0x9aa4b0, tree: 0x2a3a35, trees: 90 },
  { name: "snow",   low: 0x2a3440, mid: 0x4a5767, rock: 0x6f7c8c, peak: 0xb9c6d4, tree: 0x1d3330, trees: 300 },
];

function rng(seed) {
  let a = seed >>> 0;
  return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}
function makeNoise(seed) {
  const r = rng(seed), P = Array.from({ length: 512 }, () => r());
  const h = (a, b) => P[((a * 73856093) ^ (b * 19349663)) & 511];
  return (x, z) => {
    const xi = Math.floor(x), zi = Math.floor(z), fx = x - xi, fz = z - zi, u = fx * fx * (3 - 2 * fx), v = fz * fz * (3 - 2 * fz);
    const a = h(xi, zi), b = h(xi + 1, zi), c = h(xi, zi + 1), d = h(xi + 1, zi + 1);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
  };
}
// A stable seed when the page has none: from what STATICENV says about the layout.
export function seedFromLayout(env) {
  let h = 2166136261;
  const mix = (n) => { h ^= (n | 0) + 0x9e3779b9; h = Math.imul(h, 16777619); };
  ((env && env.gateways) || []).forEach((g) => { mix(g.x); mix(g.y); });
  ((env && env.buildings) || []).slice(0, 40).forEach((b) => { mix(b.x); mix(b.y); mix(b.floors); });
  return h >>> 0;
}

export function createSurround(THREE, ctx) {
  const { scene } = ctx;
  const group = new THREE.Group();
  group.name = "surround";
  scene.add(group);
  let key = "", portals = [], info = { count: 0, seed: null, biome: null, wall: 0, gateways: [] };

  function disposeGroup() {
    group.traverse((o) => {
      if (o.geometry) o.geometry.dispose();
      if (o.material) { (Array.isArray(o.material) ? o.material : [o.material]).forEach((m) => { if (m.map) m.map.dispose(); m.dispose(); }); }
    });
    group.clear();
    portals = [];
  }

  function outward(g, W, H) {
    const dN = g.y, dS = H - 1 - g.y, dW = g.x, dE = W - 1 - g.x, m = Math.min(dN, dS, dW, dE);
    if (m === dN) return { x: 0, z: -1 };
    if (m === dS) return { x: 0, z: 1 };
    if (m === dW) return { x: -1, z: 0 };
    return { x: 1, z: 0 };
  }

  function labelSprite(id) {
    const cv = document.createElement("canvas"); cv.width = 256; cv.height = 72;
    const c = cv.getContext("2d");
    c.fillStyle = "rgba(8,12,18,0.92)"; c.strokeStyle = GREEN; c.lineWidth = 5;
    c.beginPath(); c.roundRect ? c.roundRect(6, 6, 244, 60, 30) : c.rect(6, 6, 244, 60); c.fill(); c.stroke();
    c.fillStyle = GREEN; c.font = "bold 36px sans-serif"; c.textAlign = "center"; c.textBaseline = "middle"; c.fillText("EXIT " + id, 128, 38);
    const t = new THREE.CanvasTexture(cv); t.colorSpace = THREE.SRGBColorSpace;
    // alphaTest (not a transparent depth write): only the visible pixels count for depth
    return new THREE.Sprite(new THREE.SpriteMaterial({ map: t, transparent: true, alphaTest: 0.5, fog: false }));
  }
  function haloSprite() {
    const cv = document.createElement("canvas"); cv.width = cv.height = 64;
    const c = cv.getContext("2d"), g = c.createRadialGradient(32, 32, 2, 32, 32, 32);
    g.addColorStop(0, GREEN + "cc"); g.addColorStop(1, GREEN + "00"); c.fillStyle = g; c.fillRect(0, 0, 64, 64);
    return new THREE.Sprite(new THREE.SpriteMaterial({ map: new THREE.CanvasTexture(cv), transparent: true, depthWrite: false, fog: false, blending: THREE.AdditiveBlending }));
  }
  const archShape = (hw, hh) => { const s = new THREE.Shape(); s.moveTo(-hw, 0); s.lineTo(-hw, hh - hw); s.absarc(0, hh - hw, hw, Math.PI, 0, true); s.lineTo(hw, 0); s.closePath(); return s; };

  // One cell wide, flush with the map edge: its front face is on the edge line, so the road
  // that reaches the edge runs straight into the tunnel mouth. It stands entirely OUTSIDE the
  // map (z <= 0 locally), so it never overhangs a neighbouring cell - on a river bank that
  // could be water. A hill profile (vertical face, then a slope back down) in the rim's colours.
  const PORTAL_W = 1.0, PORTAL_DEPTH = 2.4;
  function buildPortal(g, W, H, wall, B, roadCell) {
    const o = outward(g, W, H), grp = new THREE.Group();
    grp.position.set(g.x - o.x * 0.5, 0, g.y - o.z * 0.5);          // the map-edge line at this cell
    grp.rotation.y = Math.atan2(-o.x, -o.z);                          // local +z points into the map; z = 0 is the edge
    const rock = new THREE.MeshStandardMaterial({ color: B.rock, roughness: 1, flatShading: true });
    const frameMat = new THREE.MeshStandardMaterial({ color: 0x8a929d, roughness: 0.9, flatShading: true });
    const black = new THREE.MeshBasicMaterial({ color: 0x03050a });
    const top = wall * 1.08;
    const prof = new THREE.Shape();                                   // side silhouette: x = distance outward (negative z), y = height
    prof.moveTo(0, 0); prof.lineTo(0, top); prof.lineTo(-0.7, top * 1.06); prof.lineTo(-1.5, top * 0.7); prof.lineTo(-PORTAL_DEPTH, 0); prof.closePath();
    const hill = new THREE.Mesh(new THREE.ExtrudeGeometry(prof, { depth: PORTAL_W, bevelEnabled: false }), rock);
    hill.geometry.translate(0, 0, -PORTAL_W / 2); hill.rotation.y = -Math.PI / 2;   // profile x -> -z (outward), extrusion -> local x (along the edge)
    hill.castShadow = true; grp.add(hill);
    const hw = 0.4, mw = 0.32;                                        // arch narrower than the one-cell block
    const fr = new THREE.Mesh(new THREE.ShapeGeometry(archShape(hw, Math.min(1.0, wall * 0.85))), frameMat); fr.position.set(0, 0.006, 0.012); grp.add(fr);
    const mo = new THREE.Mesh(new THREE.ShapeGeometry(archShape(mw, Math.min(0.88, wall * 0.75))), black); mo.position.set(0, 0.006, 0.02); grp.add(mo);
    if (!roadCell) {                                                  // the edge cell has no road drawn: a short stub into the tunnel
      const stub = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.014, 0.9), new THREE.MeshStandardMaterial({ color: 0x4f5c70, roughness: 1 }));
      stub.position.set(0, 0.012, 0.45); stub.receiveShadow = true; grp.add(stub);
    }
    const pole = new THREE.Mesh(new THREE.CylinderGeometry(0.025, 0.025, 0.5, 6), new THREE.MeshStandardMaterial({ color: 0xb8c0cc }));
    pole.position.set(0, top * 1.06 + 0.2, -0.7); grp.add(pole);
    const ball = new THREE.Mesh(new THREE.SphereGeometry(0.13, 12, 10), new THREE.MeshBasicMaterial({ color: new THREE.Color(GREEN), fog: false }));
    ball.position.set(0, top * 1.06 + 0.47, -0.7); grp.add(ball);
    const halo = haloSprite(); halo.position.copy(ball.position); halo.scale.set(0.9, 0.9, 1); halo.renderOrder = 4; grp.add(halo);
    const label = labelSprite(g.id); label.scale.set(1.2, 0.34, 1); label.position.set(0, top * 1.06 + 0.95, -0.7); label.renderOrder = 5; grp.add(label);
    group.add(grp);
    // world positions (for picking / LOD): the edge line at the gateway, and the beacon / label
    const edge = { x: g.x + o.x * 0.5, y: g.y + o.z * 0.5 };
    const world = (lz, ly) => { const v = new THREE.Vector3(0, ly, lz); grp.localToWorld(v); return v; };
    const beaconW = world(-0.7, top * 1.06 + 0.47), labelW = world(-0.7, top * 1.06 + 0.95);
    // the portal's footprint on the ground, in world (x, y): one cell wide, entirely outside the map edge
    const fx = o.x !== 0 ? [Math.min(edge.x, edge.x + o.x * PORTAL_DEPTH), Math.max(edge.x, edge.x + o.x * PORTAL_DEPTH)] : [g.x - PORTAL_W / 2, g.x + PORTAL_W / 2];
    const fy = o.z !== 0 ? [Math.min(edge.y, edge.y + o.z * PORTAL_DEPTH), Math.max(edge.y, edge.y + o.z * PORTAL_DEPTH)] : [g.y - PORTAL_W / 2, g.y + PORTAL_W / 2];
    portals.push({ g, grp, label, halo, edge, top, footprint: { x0: fx[0], x1: fx[1], y0: fy[0], y1: fy[1] }, beacon: { x: beaconW.x, y: beaconW.y, z: beaconW.z }, labelPos: { x: labelW.x, y: labelW.y, z: labelW.z }, labelShown: true });
  }

  function sync(o) {
    const { terrain, gateways, seed, maxHeight } = o;
    if (!terrain) return;
    const W = terrain.width, H = terrain.height;
    const wall = Math.max(1.2, (maxHeight || 1.5) * 1.25);          // the rim is about as tall as the tallest building
    const useSeed = (seed === null || seed === undefined || !Number.isFinite(seed)) ? seedFromLayout(o.env) : (seed >>> 0);
    const k = [W, H, useSeed, wall.toFixed(2), gateways.map((g) => g.id + ":" + g.x + "," + g.y).join(";")].join("|");
    if (k === key) return;
    key = k;
    disposeGroup();
    const B = BIOMES[useSeed % BIOMES.length], N = makeNoise(useSeed), R = 18, tot = W + 2 * R, segs = 128;
    const cx = W / 2 - 0.5, cz = H / 2 - 0.5;
    const dist = (x, z) => Math.hypot(Math.max(-x - 0.5, x - (W - 0.5), 0), Math.max(-z - 0.5, z - (H - 0.5), 0));
    const gates = gateways.map((g) => ({ g, o: outward(g, W, H) }));
    // terrain height: a steep rim at the map edge, seeded massifs growing outward; raised around each portal so it sits in the rim
    const height = (x, z) => {
      const d = dist(x, z);
      if (d === 0) return -0.15;
      const t = Math.min(1, d / R);
      const rim = wall * Math.min(1, d / 1.4) ** 0.8;
      const ridges = (N(x * 0.18, z * 0.18) * 0.9 + N(x * 0.45, z * 0.45) * 0.45) * wall * 1.1 * t;
      let h = rim + ridges + t ** 1.8 * wall * 2.2 * (0.6 + 0.8 * N(x * 0.07 + 9, z * 0.07 + 3));
      for (const { g, o: go } of gates) {
        const along = go.x !== 0 ? Math.abs(z - g.y) : Math.abs(x - g.x), out = go.x !== 0 ? (x - g.x) * go.x - 0.5 : (z - g.y) * go.z - 0.5;
        if (out > -0.2 && out < 4.0 && along < 2.6) { const w = Math.max(0, 1 - Math.max(0, along - 0.6) / 2.0) * Math.max(0, 1 - Math.max(0, out - 1.4) / 2.6); h = Math.max(h, wall * 1.08 * w + h * (1 - w)); }
      }
      return h;
    };
    const geo = new THREE.PlaneGeometry(tot, tot, segs, segs); geo.rotateX(-Math.PI / 2);
    const pos = geo.attributes.position, col = new THREE.Float32BufferAttribute(pos.count * 3, 3), c = new THREE.Color();
    const lo = new THREE.Color(B.low), mi = new THREE.Color(B.mid), ro = new THREE.Color(B.rock), pk = new THREE.Color(B.peak);
    for (let i = 0; i < pos.count; i++) {
      const x = pos.getX(i) + cx, z = pos.getZ(i) + cz, h = height(x, z);
      pos.setXYZ(i, x, h, z);
      const kk = h / (wall * 2.2);
      if (kk < 0.45) c.copy(lo).lerp(mi, N(x * 0.5, z * 0.5)); else if (kk < 0.85) c.copy(mi).lerp(ro, (kk - 0.45) / 0.4); else c.copy(ro).lerp(pk, Math.min(1, (kk - 0.85) / 0.5));
      col.setXYZ(i, c.r, c.g, c.b);
    }
    geo.setAttribute("color", col); geo.computeVertexNormals();
    const mesh = new THREE.Mesh(geo, new THREE.MeshStandardMaterial({ vertexColors: true, flatShading: true, roughness: 1 }));
    mesh.name = "terrain-ring"; group.add(mesh);
    // trees on the lower slopes (instanced)
    const r = rng(useSeed * 7 + 1), trees = new THREE.InstancedMesh(new THREE.ConeGeometry(0.18, 0.55, 5), new THREE.MeshStandardMaterial({ color: B.tree, roughness: 1, flatShading: true }), B.trees);
    const m = new THREE.Matrix4(), q = new THREE.Quaternion(), s = new THREE.Vector3(); let n = 0;
    for (let i = 0; i < 20000 && n < B.trees; i++) {
      const px = -0.5 - R + r() * tot, pz = -0.5 - R + r() * tot;   // anywhere in the ring's square; the map rectangle itself is rejected by dist()
      const d = dist(px, pz);
      if (d < 1.0 || d > R * 0.5) continue;
      const h = height(px, pz);
      if (h > wall * 1.5 || N(px * 0.5, pz * 0.5) < 0.4) continue;
      if (gates.some(({ g }) => Math.hypot(px - g.x, pz - g.y) < 3.2)) continue;   // keep the portals clear
      const sc = 0.8 + r() * 0.8; s.set(sc, sc * (0.9 + r() * 0.7), sc);
      m.compose(new THREE.Vector3(px, h + 0.25 * s.y, pz), q, s); trees.setMatrixAt(n++, m);
    }
    trees.count = n; group.add(trees);
    const roadAt = (g) => { const cell = terrain.cells && terrain.cells[g.y * W + g.x]; return !!(cell && cell.navigable && cell.roadClass > 0); };
    gateways.forEach((g) => buildPortal(g, W, H, wall, B, roadAt(g)));
    info = { count: portals.length, seed: useSeed, biome: B.name, wall, gateways: portals.map((p) => ({ id: p.g.id, x: p.g.x, y: p.g.y, facing: outward(p.g, W, H), onRoad: roadAt(p.g), footprint: p.footprint })) };
  }

  // Per frame: keep each beacon / label a readable size and hide the label when it would be too small to read.
  // ppuAt(x, y, z) = screen pixels per world unit at a point.
  const LABEL_MIN_PX = 54, LABEL_HYST = 8, BEACON_MIN_PX = 9;
  function update(ppuAt) {
    for (const p of portals) {
      const ppu = ppuAt(p.labelPos.x, p.labelPos.z, p.labelPos.y);
      const labelPx = 1.2 * ppu, k = Math.max(1, BEACON_MIN_PX / Math.max(ppu * 0.9, 0.01));
      p.halo.scale.set(0.9 * k, 0.9 * k, 1);
      const show = p.labelShown ? labelPx >= LABEL_MIN_PX - LABEL_HYST : labelPx >= LABEL_MIN_PX + LABEL_HYST;
      if (show !== p.labelShown) { p.labelShown = show; p.label.visible = show; }
    }
  }

  return {
    group, sync, update,
    // Picking targets (low priority: a team / incident under the cursor wins).
    hits() { return portals.map((p) => ({ type: "gateway", id: p.g.id, wx: p.edge.x, wy: p.edge.y, wz: 0.5, wr: 0.9, low: true, data: { id: p.g.id, x: p.g.x, y: p.g.y } })); },
    state() { return Object.assign({}, info, { labelsShown: portals.filter((p) => p.labelShown).length, portals: portals.length }); },
    dispose() { disposeGroup(); scene.remove(group); },
  };
}

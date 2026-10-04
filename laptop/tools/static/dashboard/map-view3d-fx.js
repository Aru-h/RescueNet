/*
 * laptop/tools/static/dashboard/map-view3d-fx.js
 *
 * Hazard effects for the 3D map (loaded by map-view3d.js): flames and
 * smoke on fires, animated water on flooded cells, debris on blocked /
 * damaged roads, ground haze for gas, and a dust burst for building collapses.
 * Everything is driven by data the dashboard already receives:
 *
 *   fire   - hazard_cells[].hazard_flags bit 0 (per-cell flag, no
 *            intensity is sent: intensity here is derived from how many
 *            neighbours are burning and how long the cell has burned),
 *            plus "Active fire" incidents (type 7), whose work_remaining
 *            sets the intensity of the flames on their building;
 *   gas    - hazard_flags bit 1;
 *   flood  - STATICMAP's flood flag;
 *   debris - hazard_cells[].blocked / road_damage / infra_damage.
 *
 * Cheap by construction: particles are GPU-animated THREE.Points (the
 * vertex shader moves them from a per-particle phase and one time
 * uniform - no per-frame CPU work per particle), debris is one
 * InstancedMesh, water is one merged mesh, and every count is capped by
 * the effects level ("off" / "low" / "high"). At "off" nothing moves and
 * no particles are drawn, but the causes stay visible as static
 * ember / haze / water / debris so the map never loses information.
 */

const MAX_EMITTERS = 220;
const CAPS = {
  off:  { flames: 0,   smoke: 0,   haze: 0,   dust: 0,   debris: [1, 2, 3] },
  low:  { flames: 260, smoke: 170, haze: 160, dust: 70,  debris: [1, 2, 4] },
  high: { flames: 760, smoke: 480, haze: 420, dust: 180, debris: [2, 4, 7] },
};
const PER_EMITTER = { off: [0, 0], low: [3, 2], high: [8, 5] }; // [flames, smoke]
const DEBRIS_COLORS = [0xb9a67f, 0xb4612b, 0x3d3d44]; // light damage (tan), heavy (rust), blocked/severe (charcoal)

function rng(seed) { // mulberry32
  let a = seed >>> 0;
  return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}

const VERT_COMMON = `
  attribute vec4 aSeed;          // x phase, y/z random, w intensity
  uniform float uTime, uScale, uMotion;
`;

export function createFx(THREE, ctx) {
  const { scene, MR } = ctx;
  let level = "low";
  let motion = true;          // false when reduce-motion is on: particles freeze on one frame
  let night = false;
  let last = null;            // last data passed to sync(), so a level change can rebuild
  const fireAge = new Map();  // "x,y" -> tick it was first seen burning

  const group = new THREE.Group();
  group.name = "fx";
  scene.add(group);

  const U = { uTime: { value: 0 }, uScale: { value: 600 }, uMotion: { value: 1 } };

  // ---------- particle systems ----------
  function pointsMat(vert, frag, additive) {
    return new THREE.ShaderMaterial({
      uniforms: U, vertexShader: vert, fragmentShader: frag,
      transparent: true, depthWrite: false,
      blending: additive ? THREE.AdditiveBlending : THREE.NormalBlending,
    });
  }

  const FLAME_VERT = VERT_COMMON + `
    varying float vT; varying float vI;
    void main() {
      float t = fract(uMotion * uTime * (0.9 + aSeed.y * 0.6) * 0.9 + aSeed.x);
      float inten = aSeed.w;
      vec3 p = position;
      p.y += t * (0.35 + 0.55 * inten);
      float s = uMotion * uTime;
      p.x += (aSeed.y - 0.5) * 0.5 * (1.0 - t) + sin(s * 3.0 * (0.5 + aSeed.z) + aSeed.x * 6.283) * 0.06 * t;
      p.z += (aSeed.z - 0.5) * 0.5 * (1.0 - t) + cos(s * 2.7 * (0.5 + aSeed.y) + aSeed.x * 6.283) * 0.06 * t;
      vec4 mv = modelViewMatrix * vec4(p, 1.0);
      gl_Position = projectionMatrix * mv;
      float size = (0.42 + 0.30 * inten) * (1.0 - t * 0.65) * (0.85 + 0.3 * sin(s * 11.0 + aSeed.z * 20.0));
      gl_PointSize = min(size * uScale / max(0.1, -mv.z), 96.0);
      vT = t; vI = inten;
    }`;
  const FLAME_FRAG = `
    varying float vT; varying float vI;
    void main() {
      vec2 c = gl_PointCoord - 0.5;
      float d = length(vec2(c.x * 1.35, c.y * 0.95 + 0.06));
      float a = smoothstep(0.5, 0.08, d) * (1.0 - vT) * (0.65 + 0.35 * vI);
      vec3 col = mix(vec3(1.0, 0.95, 0.55), vec3(1.0, 0.42, 0.06), vT);
      col = mix(col, vec3(0.55, 0.08, 0.02), smoothstep(0.55, 1.0, vT));
      gl_FragColor = vec4(col, a);
    }`;
  const SMOKE_VERT = VERT_COMMON + `
    varying float vT; varying float vI;
    void main() {
      float t = fract(uMotion * uTime * (0.16 + aSeed.y * 0.10) + aSeed.x);
      float inten = aSeed.w;
      vec3 p = position;
      p.y += 0.25 + t * (0.8 + 1.4 * inten);
      p.x += t * t * 0.9 + (aSeed.y - 0.5) * 0.3;     // drifts with a light wind
      p.z += t * t * 0.35 + (aSeed.z - 0.5) * 0.3;
      vec4 mv = modelViewMatrix * vec4(p, 1.0);
      gl_Position = projectionMatrix * mv;
      float size = (0.5 + 1.1 * t) * (0.7 + 0.5 * inten);
      gl_PointSize = min(size * uScale / max(0.1, -mv.z), 160.0);
      vT = t; vI = inten;
    }`;
  const SMOKE_FRAG = `
    varying float vT; varying float vI;
    void main() {
      vec2 c = gl_PointCoord - 0.5;
      float a = smoothstep(0.5, 0.0, length(c)) * 0.50 * (1.0 - vT) * smoothstep(0.0, 0.12, vT);
      vec3 col = mix(vec3(0.20), vec3(0.50), vT);
      gl_FragColor = vec4(col, a);
    }`;
  const HAZE_VERT = VERT_COMMON + `
    varying float vA;
    void main() {
      float s = uMotion * uTime;
      vec3 p = position;
      p.x += sin(s * 0.25 + aSeed.x * 6.283) * 0.35;
      p.z += cos(s * 0.21 + aSeed.y * 6.283) * 0.35;
      p.y += 0.16 + aSeed.z * 0.3 + sin(s * 0.5 + aSeed.x * 12.0) * 0.04;
      vec4 mv = modelViewMatrix * vec4(p, 1.0);
      gl_Position = projectionMatrix * mv;
      gl_PointSize = min((1.3 + aSeed.z * 1.0) * uScale / max(0.1, -mv.z), 200.0);
      vA = 0.55 + 0.45 * sin(s * 0.4 + aSeed.y * 9.0);
    }`;
  const HAZE_FRAG = `
    varying float vA;
    void main() {
      vec2 c = gl_PointCoord - 0.5;
      float a = smoothstep(0.5, 0.0, length(c)) * 0.13 * vA;
      gl_FragColor = vec4(0.70, 0.84, 0.22, a);
    }`;
  const DUST_VERT = VERT_COMMON + `
    uniform float uStart;
    varying float vT;
    void main() {
      float t = clamp((uTime - uStart) / 1.7, 0.0, 1.0);
      vec3 p = position;
      vec2 dir = vec2(aSeed.y - 0.5, aSeed.z - 0.5) * 2.0;
      p.xz += dir * t * 1.6;
      p.y += 0.1 + t * (0.5 + aSeed.x * 0.9) - t * t * 0.35;
      vec4 mv = modelViewMatrix * vec4(p, 1.0);
      gl_Position = projectionMatrix * mv;
      gl_PointSize = min((0.4 + t * 1.2) * uScale / max(0.1, -mv.z), 120.0);
      vT = t;
    }`;
  const DUST_FRAG = `
    varying float vT;
    void main() {
      vec2 c = gl_PointCoord - 0.5;
      float a = smoothstep(0.5, 0.0, length(c)) * 0.55 * (1.0 - vT);
      gl_FragColor = vec4(0.55, 0.50, 0.44, a);
    }`;

  function makePoints(vert, frag, additive, order, extra) {
    const mat = pointsMat(vert, frag, additive);
    if (extra) Object.assign(mat.uniforms = Object.assign({}, U), extra);
    const geo = new THREE.BufferGeometry();
    const pts = new THREE.Points(geo, mat);
    pts.frustumCulled = false; pts.renderOrder = order; pts.visible = false;
    group.add(pts);
    return { pts, geo, mat, count: 0 };
  }
  const flames = makePoints(FLAME_VERT, FLAME_FRAG, true, 9);
  const smoke = makePoints(SMOKE_VERT, SMOKE_FRAG, false, 8);
  const haze = makePoints(HAZE_VERT, HAZE_FRAG, false, 2);
  const dustStart = { value: -100 };
  const dust = makePoints(DUST_VERT, DUST_FRAG, false, 10, { uStart: dustStart });

  function fillPoints(sys, items, cap) {
    // items: [{x, y(height), z, seed:[phase,r1,r2], inten}] -> geometry
    const n = Math.min(items.length, cap);
    sys.count = n;
    if (n === 0) { sys.pts.visible = false; return; }
    const pos = new Float32Array(n * 3), seed = new Float32Array(n * 4);
    for (let i = 0; i < n; i++) {
      const it = items[i];
      pos[i * 3] = it.x; pos[i * 3 + 1] = it.y; pos[i * 3 + 2] = it.z;
      seed[i * 4] = it.s[0]; seed[i * 4 + 1] = it.s[1]; seed[i * 4 + 2] = it.s[2]; seed[i * 4 + 3] = it.inten;
    }
    sys.geo.setAttribute("position", new THREE.BufferAttribute(pos, 3));
    sys.geo.setAttribute("aSeed", new THREE.BufferAttribute(seed, 4));
    sys.geo.computeBoundingSphere();
    sys.pts.visible = true;
  }

  // ---------- static layers: embers, haze discs, water, debris ----------
  const discGeo = new THREE.CircleGeometry(0.5, 18);
  const emberMat = new THREE.MeshBasicMaterial({ color: 0xff7a1a, transparent: true, opacity: 0.32, depthWrite: false });
  const gasMat = new THREE.MeshBasicMaterial({ color: 0xb4d63a, transparent: true, opacity: 0.10, depthWrite: false });
  const FLAT = -Math.PI / 2;
  function instancedDiscs(mat, cap, order) {
    const m = new THREE.InstancedMesh(discGeo, mat, cap);
    m.count = 0; m.frustumCulled = false; m.renderOrder = order; m.rotation.x = FLAT;
    group.add(m);
    return m;
  }
  const embers = instancedDiscs(emberMat, 512, 1);
  const gasDiscs = instancedDiscs(gasMat, 512, 1);
  const _m4 = new THREE.Matrix4(), _q = new THREE.Quaternion(), _p = new THREE.Vector3(), _s = new THREE.Vector3();
  function setDiscs(mesh, list) {
    // mesh is rotated flat about X: local (x, y) maps to world (x, -z)... so place with world coords through the parent rotation.
    mesh.count = Math.min(list.length, mesh.instanceMatrix.count);
    for (let i = 0; i < mesh.count; i++) {
      const d = list[i];
      _m4.compose(_p.set(d.x, -d.z, 0.012 + (d.y || 0) + 0.0), _q.identity(), _s.set(d.r, d.r, 1));
      mesh.setMatrixAt(i, _m4);
    }
    mesh.instanceMatrix.needsUpdate = true;
  }

  // Water: one merged mesh over the flooded cells, animated in the shader.
  const waterMat = new THREE.ShaderMaterial({
    uniforms: Object.assign({}, U, { uLight: { value: 1 } }),
    vertexShader: `varying vec2 vP; void main() { vP = position.xz; gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0); }`,
    fragmentShader: `
      uniform float uTime, uMotion, uLight; varying vec2 vP;
      void main() {
        float t = uTime * uMotion;
        float w = sin(vP.x * 2.7 + t * 1.3) * sin(vP.y * 3.1 - t * 1.1) + 0.5 * sin((vP.x + vP.y) * 4.3 + t * 2.1);
        float glint = smoothstep(0.9, 1.35, w);
        vec3 col = mix(vec3(0.09, 0.30, 0.60), vec3(0.24, 0.55, 0.86), 0.5 + 0.28 * w) + glint * 0.45;
        gl_FragColor = vec4(col * uLight, 0.50 + 0.07 * w + glint * 0.25);
      }`,
    transparent: true, depthWrite: false,
  });
  let waterMesh = null, waterSource = null;
  function buildWater(terrain) {
    if (waterMesh) { group.remove(waterMesh); waterMesh.geometry.dispose(); waterMesh = null; }
    waterSource = terrain ? terrain.source : null;
    if (!terrain) return;
    const quads = [];
    for (let gy = 0; gy < terrain.height; gy++) for (let gx = 0; gx < terrain.width; gx++) if (terrain.cells[gy * terrain.width + gx].flood) quads.push([gx, gy]);
    if (!quads.length) return;
    const pos = new Float32Array(quads.length * 18);
    quads.forEach(([x, y], i) => {
      const x0 = x - 0.5, x1 = x + 0.5, z0 = y - 0.5, z1 = y + 0.5, h = 0.03;
      pos.set([x0, h, z0, x1, h, z0, x1, h, z1, x0, h, z0, x1, h, z1, x0, h, z1], i * 18);
    });
    const geo = new THREE.BufferGeometry();
    geo.setAttribute("position", new THREE.BufferAttribute(pos, 3));
    waterMesh = new THREE.Mesh(geo, waterMat);
    waterMesh.renderOrder = 1; waterMesh.frustumCulled = false;
    group.add(waterMesh);
    waterMesh.userData.cells = quads.length;
  }

  // Debris on blocked / damaged road cells, coloured by severity.
  const DEBRIS_CAP = 2600;
  const debris = new THREE.InstancedMesh(new THREE.IcosahedronGeometry(0.5, 0),
    new THREE.MeshStandardMaterial({ roughness: 1, metalness: 0, flatShading: true }), DEBRIS_CAP);
  debris.count = 0; debris.frustumCulled = false; debris.receiveShadow = true;
  debris.setColorAt(0, new THREE.Color(1, 1, 1));
  group.add(debris);
  let debrisSig = null;
  function severityClass(c) {
    if (c.blocked) return 3;
    const s = Math.max(c.road_damage || 0, c.infra_damage || 0);
    if (s >= 180) return 3;
    if (s >= 100) return 2;
    if (s >= 50) return 1;
    return 0;
  }
  function buildDebris(cells) {
    const per = CAPS[level].debris;
    const sig = level + "|" + cells.map((c) => c.x + "," + c.y + "," + severityClass(c)).join(";");
    if (sig === debrisSig) return;
    debrisSig = sig;
    const col = new THREE.Color(), q = new THREE.Quaternion(), e = new THREE.Euler();
    let n = 0;
    for (const c of cells) {
      const cls = severityClass(c);
      if (!cls) continue;
      const r = rng((c.x * 73856093) ^ (c.y * 19349663) ^ 0x9e37);
      const count = per[cls - 1];
      for (let k = 0; k < count && n < DEBRIS_CAP; k++) {
        const sc = (0.07 + 0.05 * cls) * (0.6 + r() * 0.9);
        e.set(r() * 3, r() * 6, r() * 3); q.setFromEuler(e);
        _m4.compose(_p.set(c.x + (r() - 0.5) * 0.85, sc * 0.35, c.y + (r() - 0.5) * 0.85), q, _s.set(sc * (1 + r() * 0.8), sc * (0.6 + r() * 0.5), sc * (1 + r() * 0.8)));
        debris.setMatrixAt(n, _m4);
        col.setHex(DEBRIS_COLORS[cls - 1]).multiplyScalar(0.8 + r() * 0.35);
        debris.setColorAt(n, col);
        n++;
      }
    }
    debris.count = n;
    debris.instanceMatrix.needsUpdate = true;
    if (debris.instanceColor) debris.instanceColor.needsUpdate = true;
  }

  // ---------- emitters from data ----------
  function collectFires(data) {
    const burning = new Set(), list = new Map();
    const key = (x, y) => x + "," + y;
    for (const c of data.hazardCells) if (c.hazard_flags & 1) burning.add(key(c.x, c.y));
    // Forget cells that stopped burning.
    for (const k of fireAge.keys()) if (!burning.has(k)) fireAge.delete(k);
    for (const c of data.hazardCells) {
      if (!(c.hazard_flags & 1)) continue;
      const k = key(c.x, c.y);
      if (!fireAge.has(k)) fireAge.set(k, data.tick || 0);
      let nb = 0;
      for (let dx = -1; dx <= 1; dx++) for (let dy = -1; dy <= 1; dy++) if ((dx || dy) && burning.has(key(c.x + dx, c.y + dy))) nb++;
      const age = (data.tick || 0) - fireAge.get(k);
      const inten = Math.max(0.4, Math.min(1.25, 0.55 + 0.1 * nb + Math.min(0.3, age / 200)));
      const prep = data.prep;
      list.set(k, { x: c.x, y: c.y, z: (data.footing || ((x, y) => MR.footingZ(prep, x, y)))(c.x, c.y), inten });
    }
    // "Active fire" incidents: flames on their building, sized by work left.
    for (const mi of data.incidents) {
      let w = -1;
      for (const m of mi.members) if (m.type === 7 && m.status !== 3) w = Math.max(w, m.work_remaining === undefined ? 60 : m.work_remaining);
      if (w < 0) continue;
      const inten = Math.max(0.4, Math.min(1.3, 0.4 + (w / 100) * 0.8));
      const k = key(mi.wx, mi.wy), prev = list.get(k);
      list.set(k, { x: mi.wx, y: mi.wy, z: mi.foot, inten: prev ? Math.max(prev.inten, inten) : inten });
    }
    return Array.from(list.values()).sort((a, b) => b.inten - a.inten).slice(0, MAX_EMITTERS);
  }

  function sync(data, force) {
    last = data;
    if (data.terrain && data.terrain.source !== waterSource) buildWater(data.terrain);
    buildDebris(data.hazardCells);

    const fires = collectFires(data);
    const caps = CAPS[level];
    const [fPer, sPer] = PER_EMITTER[level];
    const fl = [], sm = [];
    fires.forEach((f, i) => {
      const r = rng(Math.round(f.x * 977 + f.y * 131 + i));
      for (let k = 0; k < fPer; k++) fl.push({ x: f.x, y: f.z, z: f.y, s: [(k + r()) / fPer, r(), r()], inten: f.inten });
      for (let k = 0; k < sPer; k++) sm.push({ x: f.x, y: f.z, z: f.y, s: [(k + r()) / sPer, r(), r()], inten: f.inten });
    });
    fillPoints(flames, fl, caps.flames);
    fillPoints(smoke, sm, caps.smoke);

    // Gas haze (particles) + static discs (always, so "off" still shows it).
    const gas = data.hazardCells.filter((c) => c.hazard_flags & 2);
    const hz = [];
    if (level !== "off") gas.forEach((c, i) => {
      const r = rng(c.x * 31 + c.y * 17 + i);
      const n = level === "high" ? 3 : 1;
      for (let k = 0; k < n; k++) hz.push({ x: c.x + (r() - 0.5) * 0.6, y: 0, z: c.y + (r() - 0.5) * 0.6, s: [r(), r(), r()], inten: 1 });
    });
    fillPoints(haze, hz, caps.haze);
    setDiscs(gasDiscs, gas.map((c) => ({ x: c.x, y: 0.0, z: c.y, r: 1.0 })));
    setDiscs(embers, fires.map((f) => ({ x: f.x, y: f.z, z: f.y, r: 0.55 + 0.3 * f.inten })));
    counts.fires = fires.length;
  }

  const counts = { fires: 0 };

  // One-shot dust burst over collapsed buildings. `cells`: [{x, y}].
  let burstUntil = -1;
  function burst(cells, timeSec) {
    const cap = CAPS[level].dust;
    if (!motion || cap === 0 || !cells || !cells.length) return false;
    const items = [];
    const r = rng(Math.floor(timeSec * 1000) & 0xffff);
    const per = Math.max(2, Math.ceil(cap / cells.length));
    for (const c of cells) for (let k = 0; k < per && items.length < cap; k++)
      items.push({ x: c.x + (r() - 0.5) * 0.8, y: 0, z: c.y + (r() - 0.5) * 0.8, s: [r(), r(), r()], inten: 1 });
    fillPoints(dust, items, cap);
    dustStart.value = timeSec;
    burstUntil = timeSec + 1.8;
    return true;
  }

  function setLevel(l) {
    if (!CAPS[l] || l === level) return;
    level = l;
    debrisSig = null; // debris counts depend on the level
    if (last) sync(last, true);
    if (level === "off") { dust.pts.visible = false; burstUntil = -1; }
  }
  function setMotion(on) { motion = !!on; U.uMotion.value = motion ? 1 : 0; waterMat.uniforms.uMotion.value = U.uMotion.value; }
  function setNight(on) { night = !!on; waterMat.uniforms.uLight.value = night ? 0.45 : 1; }

  // `timeSec` should keep advancing; `pxScale` = drawing-buffer height / (2 tan(fov/2)).
  function update(timeSec, pxScale) {
    U.uTime.value = timeSec;
    waterMat.uniforms.uTime.value = timeSec;
    if (pxScale) U.uScale.value = pxScale;
    if (burstUntil > 0 && timeSec > burstUntil) { dust.pts.visible = false; burstUntil = -1; }
  }

  // True while something is moving (so the host keeps its animation loop alive).
  function animated(timeSec) {
    if (!motion || level === "off") return false;
    return flames.count > 0 || smoke.count > 0 || haze.count > 0 || !!waterMesh || (burstUntil > 0 && timeSec <= burstUntil);
  }

  return {
    sync, update, burst, setLevel, setMotion, setNight, animated,
    get level() { return level; },
    get motion() { return motion; },
    counts() {
      return { fires: counts.fires, flames: flames.count, smoke: smoke.count, haze: haze.count, dust: dust.pts.visible ? dust.count : 0,
               debris: debris.count, waterCells: waterMesh ? waterMesh.userData.cells : 0, embers: embers.count };
    },
  };
}

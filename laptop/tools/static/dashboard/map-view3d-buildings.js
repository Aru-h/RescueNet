/*
 * laptop/tools/static/dashboard/map-view3d-buildings.js
 *
 * Procedural buildings for the 3D map (loaded by map-view3d.js). No model
 * files: every building is assembled from three.js box / plane geometry
 * and merged (three's own BufferGeometryUtils.mergeGeometries, vendored
 * unmodified) into two meshes - one with a window facade texture, one
 * flat-coloured - so the whole city is a handful of draw calls.
 *
 * Height comes from STATICENV's `floors` (0.25 world units per floor, as
 * in 2.5D) and the look from its `damage` value, using the simulator's
 * named levels (core/environment.h):
 *
 *   < ENV_DAMAGE_MINOR (25)      intact: windows, parapet, rooftop unit
 *   MINOR..SEVERE-1 (25-74)      cracked: darker facade, cracks across the walls
 *   SEVERE..DESTROYED-1 (75-99)  partial collapse: walls cut down, a broken
 *                                jagged top with gaps, a fallen slab, debris
 *   >= ENV_DAMAGE_DESTROYED (100) rubble pile with rebar
 *
 * (STATICENV is sent once per run, so this is the damage the city started
 * with - the simulator re-sends the list when buildings are destroyed, which
 * is how later collapses arrive; the data the
 * dashboard receives.)
 *
 * Windows have an emissive map, lit warm when the lighting is set to night.
 */
import { mergeGeometries } from "/static/three/addons/BufferGeometryUtils.js";

const FLOOR_H = 0.25;        // world units of height per floor (matches map-render.js ISO_FLOOR_H)
const BAY_W = 0.4;           // window bay width
const TILE_BAYS = 4, TILE_FLOORS = 3;

function rng(seed) { // mulberry32
  let a = seed >>> 0;
  return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; };
}

export function damageState(dmg, MR) {
  if (dmg >= 100) return 3;                 // ENV_DAMAGE_DESTROYED
  if (dmg >= MR.ENV_DAMAGE_SEVERE) return 2;
  if (dmg >= MR.ENV_DAMAGE_MINOR) return 1;
  return 0;
}

export function createBuildings(THREE, ctx) {
  const { scene, MR, maxAniso } = ctx;
  const group = new THREE.Group();
  group.name = "buildings";
  scene.add(group);

  // ---- facade texture: windows (albedo) + which are lit (emissive) ----
  const W = 256, H = 192;
  function tileCanvas(lit) {
    const cv = document.createElement("canvas"); cv.width = W; cv.height = H;
    const g = cv.getContext("2d");
    g.fillStyle = lit ? "#000" : "#cfd5dc"; g.fillRect(0, 0, W, H);
    const r = rng(1234);
    const bw = W / TILE_BAYS, fh = H / TILE_FLOORS;
    for (let f = 0; f < TILE_FLOORS; f++) for (let b = 0; b < TILE_BAYS; b++) {
      const on = r() < 0.55;
      const x = b * bw + bw * 0.2, y = f * fh + fh * 0.22, w = bw * 0.6, h = fh * 0.52;
      if (lit) { if (on) { g.fillStyle = "#ffd98a"; g.fillRect(x, y, w, h); } }
      else {
        g.fillStyle = "#3b4d63"; g.fillRect(x, y, w, h);
        g.fillStyle = "rgba(255,255,255,0.18)"; g.fillRect(x, y, w, h * 0.3);   // sky glint
        g.fillStyle = "#9aa3ad"; g.fillRect(x - 1, y + h, w + 2, 3);            // sill
      }
    }
    return cv;
  }
  const facadeTex = new THREE.CanvasTexture(tileCanvas(false));
  const emissiveTex = new THREE.CanvasTexture(tileCanvas(true));
  for (const t of [facadeTex, emissiveTex]) {
    t.wrapS = t.wrapT = THREE.RepeatWrapping; t.colorSpace = THREE.SRGBColorSpace; t.anisotropy = maxAniso;
  }
  const facadeMat = new THREE.MeshStandardMaterial({ map: facadeTex, vertexColors: true, roughness: 0.85, metalness: 0.03,
    emissive: new THREE.Color(0xffffff), emissiveMap: emissiveTex, emissiveIntensity: 0 });
  const flatMat = new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.95, metalness: 0.02, flatShading: true });

  let facadeMesh = null, flatMesh = null;
  let stats = { intact: 0, cracked: 0, partial: 0, rubble: 0, parts: 0 };
  const heights = new Map(); // building id -> mean surface height (where markers stand)

  const _m = new THREE.Matrix4(), _q = new THREE.Quaternion(), _e = new THREE.Euler(), _p = new THREE.Vector3(), _s = new THREE.Vector3();
  const col = new THREE.Color();

  function tint(geo, c) {
    const n = geo.attributes.position.count, a = new Float32Array(n * 3);
    for (let i = 0; i < n; i++) { a[i * 3] = c.r; a[i * 3 + 1] = c.g; a[i * 3 + 2] = c.b; }
    geo.setAttribute("color", new THREE.BufferAttribute(a, 3));
    return geo;
  }

  function build(list) {
    dispose();
    const facade = [], flat = [];
    stats = { intact: 0, cracked: 0, partial: 0, rubble: 0, parts: 0 };
    heights.clear();

    // A flat-coloured box, centre (cx,cy,cz), size (w,h,d), Euler rotation.
    function box(w, h, d, cx, cy, cz, color, rx, ry, rz) {
      const g = new THREE.BoxGeometry(w, h, d);
      _e.set(rx || 0, ry || 0, rz || 0); _q.setFromEuler(_e);
      _m.compose(_p.set(cx, cy, cz), _q, _s.set(1, 1, 1));
      g.applyMatrix4(_m);
      flat.push(tint(g, color));
    }
    // A textured wall segment facing +Z/-Z/+X/-X, centred at (cx, h/2, cz).
    function wall(width, height, cx, cz, face, color, y0) {
      const g = new THREE.PlaneGeometry(width, height);
      const uv = g.attributes.uv;
      for (let i = 0; i < uv.count; i++) { uv.setXY(i, uv.getX(i) * width / (BAY_W * TILE_BAYS), uv.getY(i) * height / (FLOOR_H * TILE_FLOORS)); }
      _e.set(0, face, 0); _q.setFromEuler(_e);
      _m.compose(_p.set(cx, (y0 || 0) + height / 2, cz), _q, _s.set(1, 1, 1));
      g.applyMatrix4(_m);
      facade.push(tint(g, color));
    }

    list.forEach((e) => {
      const b = e.b, r = rng((b.id + 1) * 7919);
      const dmg = b.damage || 0, state = damageState(dmg, MR);
      const floors = Math.max(1, b.floors || 1);
      const H0 = floors * FLOOR_H;
      const w = e.x1 - e.x0, d = e.y1 - e.y0, cx = (e.x0 + e.x1) / 2, cz = (e.y0 + e.y1) / 2;
      // Base tone: the same neutral grey as 2D/2.5D, darker with damage, a little variation per building.
      const dark = 1 - 0.45 * Math.min(1, dmg / 100);
      const v = 0.92 + r() * 0.16;
      const base = col.setRGB(0.62 * dark * v, 0.68 * dark * v, 0.74 * dark * v, THREE.SRGBColorSpace).clone();
      const concrete = col.setRGB(0.50 * v, 0.51 * v, 0.53 * v, THREE.SRGBColorSpace).clone();
      const roofC = col.setRGB(0.36 * dark * v, 0.39 * dark * v, 0.43 * dark * v, THREE.SRGBColorSpace).clone();

      const walls = (height) => {
        wall(w, height, cx, e.y1, 0, base);            // +Z (world +y)
        wall(w, height, cx, e.y0, Math.PI, base);      // -Z
        wall(d, height, e.x1, cz, Math.PI / 2, base);  // +X
        wall(d, height, e.x0, cz, -Math.PI / 2, base); // -X
      };

      if (state <= 1) {
        const Ht = H0 * (state === 1 ? 0.97 : 1);
        walls(Ht);
        box(w, 0.03, d, cx, Ht + 0.015, cz, roofC);                       // roof slab
        if (state === 0) {                                                 // parapet + rooftop unit
          const t = 0.03, hp = 0.05;
          box(w, hp, t, cx, Ht + hp / 2, e.y0 + t / 2, base); box(w, hp, t, cx, Ht + hp / 2, e.y1 - t / 2, base);
          box(t, hp, d, e.x0 + t / 2, Ht + hp / 2, cz, base); box(t, hp, d, e.x1 - t / 2, Ht + hp / 2, cz, base);
          if (w * d >= 0.9) box(Math.min(0.3, w * 0.35), 0.1, Math.min(0.3, d * 0.5), cx + (r() - 0.5) * w * 0.4, Ht + 0.06, cz + (r() - 0.5) * d * 0.4, concrete);
          stats.intact++;
        } else {
          // Cracks: short dark zig-zags down each wall.
          const crackC = col.setRGB(0.05, 0.05, 0.06).clone();
          const sides = [[e.x0, e.x1, e.y1 + 0.006, 0], [e.x0, e.x1, e.y0 - 0.006, 0], [e.y0, e.y1, e.x1 + 0.006, 1], [e.y0, e.y1, e.x0 - 0.006, 1]];
          for (const [a0, a1, off, alongZ] of sides) {
            const n = 1 + (r() < 0.6 ? 1 : 0);
            for (let c = 0; c < n; c++) {
              let u = a0 + (a1 - a0) * (0.15 + r() * 0.7), y = Ht * (0.55 + r() * 0.4);
              const segs = 3 + Math.floor(r() * 3);
              for (let k = 0; k < segs; k++) {
                const du = (r() - 0.5) * 0.14, dy = -(0.05 + r() * 0.08);
                const len = Math.hypot(du, dy), ang = Math.atan2(dy, du);
                const mx = u + du / 2, my = y + dy / 2;
                if (alongZ) box(0.012, len, 0.012, off, my, mx, crackC, Math.atan2(du, dy), 0, 0);
                else box(len, 0.012, 0.012, mx, my, off, crackC, 0, 0, ang);
                u += du; y += dy;
              }
            }
          }
          stats.cracked++;
        }
        heights.set(b.id, Ht);
      } else if (state === 2) {
        // Partial collapse: walls cut down, a floor slab, then a broken, jagged top.
        const Hw = H0 * 0.55;
        walls(Hw);
        box(w, 0.03, d, cx, Hw + 0.015, cz, roofC);
        const cell = 0.34, nx = Math.max(1, Math.round(w / cell)), nz = Math.max(1, Math.round(d / cell));
        let sum = 0, cnt = 0;
        for (let i = 0; i < nx; i++) for (let j = 0; j < nz; j++) {
          if (r() < 0.28) continue;                                         // gap where the top has fallen away
          const top = Hw + (0.1 + r() * 0.9) * H0 * 0.27;
          const bw = w / nx, bd = d / nz;
          box(bw * 0.96, top - Hw, bd * 0.96, e.x0 + (i + 0.5) * bw, Hw + (top - Hw) / 2, e.y0 + (j + 0.5) * bd, concrete);
          sum += top; cnt++;
        }
        // A slab that has come down and leans on a wall.
        box(Math.min(w, 0.6) * 0.9, 0.04, Math.min(d, 0.5) * 0.7, cx + (r() - 0.5) * w * 0.3, Hw * 0.7, e.y1 + 0.06, concrete, -0.5 - r() * 0.3, 0, (r() - 0.5) * 0.3);
        // Debris around the base.
        for (let k = 0; k < 5; k++) {
          const s = 0.05 + r() * 0.1, side = r() * 4 | 0;
          const px = side < 2 ? e.x0 + r() * w : (side === 2 ? e.x1 + 0.1 + r() * 0.25 : e.x0 - 0.1 - r() * 0.25);
          const pz = side < 2 ? (side === 0 ? e.y1 + 0.1 + r() * 0.25 : e.y0 - 0.1 - r() * 0.25) : e.y0 + r() * d;
          box(s * (1 + r()), s * 0.7, s * (1 + r()), px, s * 0.35, pz, concrete, r() * 3, r() * 6, r() * 3);
        }
        heights.set(b.id, cnt ? (sum / cnt + Hw) / 2 : Hw);
        stats.partial++;
      } else {
        // Rubble pile with rebar.
        const Hm = Math.max(0.1, H0 * 0.28);
        const rub = [0x6b665f, 0x58544e, 0x7a736a, 0x4b4843].map((c) => new THREE.Color(c));
        box(w * 1.02, 0.03, d * 1.02, cx, 0.015, cz, rub[3]);
        const n = 12 + Math.floor(w * d * 6);
        for (let k = 0; k < n; k++) {
          const px = e.x0 + r() * w, pz = e.y0 + r() * d;
          const near = 1 - Math.min(1, Math.hypot((px - cx) / (w / 2 + 0.01), (pz - cz) / (d / 2 + 0.01)));
          const s = 0.1 + r() * 0.28, hgt = s * (0.5 + r()) * (0.4 + near);
          box(s, Math.min(hgt, Hm), s * (0.7 + r() * 0.8), px, Math.min(hgt, Hm) / 2 + 0.02, pz, rub[r() * 4 | 0], r() * 0.8, r() * 6, r() * 0.8);
        }
        for (let k = 0; k < 4; k++) {
          const rebar = col.setRGB(0.35, 0.2, 0.12).clone();
          box(0.012, 0.25 + r() * 0.2, 0.012, e.x0 + r() * w, Hm * 0.8, e.y0 + r() * d, rebar, (r() - 0.5) * 1.0, 0, (r() - 0.5) * 1.0);
        }
        heights.set(b.id, Hm * 0.55);
        stats.rubble++;
      }
    });

    stats.parts = facade.length + flat.length;
    if (facade.length) {
      facadeMesh = new THREE.Mesh(mergeGeometries(facade, false), facadeMat);
      facadeMesh.castShadow = true; facadeMesh.receiveShadow = true;
      group.add(facadeMesh);
    }
    if (flat.length) {
      flatMesh = new THREE.Mesh(mergeGeometries(flat, false), flatMat);
      flatMesh.castShadow = true; flatMesh.receiveShadow = true;
      group.add(flatMesh);
    }
    facade.forEach((g) => g.dispose()); flat.forEach((g) => g.dispose());
  }

  function dispose() {
    for (const m of [facadeMesh, flatMesh]) if (m) { group.remove(m); m.geometry.dispose(); }
    facadeMesh = flatMesh = null;
  }

  // Height a marker standing on this building rests at.
  function heightOf(e) { const h = heights.get(e.id); return h === undefined ? e.h : h; }
  function footing(prep, x, y) {
    let z = 0;
    for (const e of prep.list) if (x > e.x0 && x < e.x1 && y > e.y0 && y < e.y1) z = Math.max(z, heightOf(e));
    return z;
  }
  function setNight(on) { facadeMat.emissiveIntensity = on ? 1.15 : 0; }

  return { build, dispose, heightOf, footing, setNight, get stats() { return Object.assign({}, stats); }, group };
}

/*
 * laptop/tools/static/dashboard/map-view3d.js
 *
 * The optional "3d" view of the Live Ops region map: a WebGL scene
 * (three.js, vendored under static/three/ - no CDN) with a perspective
 * camera, orbit/tilt/zoom/pan, lighting and shadows.
 *
 * It does NOT parse or interpret simulator data itself. Everything it
 * draws comes from the same preparation 2.5D uses (map-render.js):
 *   - MapRender.buildSceneModel(): building boxes (height from `floors`,
 *     damage-shortened/darkened), visible incident clusters, teams, their
 *     world positions, footing/head heights, colours and selection;
 *   - MapRender.renderTerrainTexture(): the 2D terrain drawing, used as
 *     the ground texture;
 *   - MapRender.drawTeamGlyph()/drawIncidentGlyph(): the 2D marker
 *     glyphs, drawn onto small canvases and used as billboard sprites.
 *
 * World -> three.js axes: world (x, y) is the ground plane and z the
 * height, so a world point (x, y, z) is the three.js point (x, z, y).
 * With the default camera (azimuth 45 degrees, on the +x/+y side) that
 * gives the same orientation as the 2.5D view.
 *
 * Interaction goes through the camera object (camera.js "3d" mode,
 * which forwards to the `projector` this file installs): screenToWorld(
 * sx, sy, z) is a raycast onto the horizontal plane at height z, so
 * hit-testing, drag-to-reassign and the hover card get the same world
 * coordinates in 3D as in 2D/iso, via the same MapRender.hitTestWorld.
 *
 * Availability: if WebGL can't be created, or three.js fails to load,
 * window.MapView3D.available is false (or MapView3D is never set) and
 * the dashboard simply doesn't offer "3D". Announced with a
 * "mapview3d-ready" event on window once the answer is known.
 */
(async function () {
  "use strict";

  function webglAvailable() {
    try {
      const c = document.createElement("canvas");
      const gl = c.getContext("webgl2") || c.getContext("webgl");
      if (!gl) return false;
      const lose = gl.getExtension("WEBGL_lose_context");
      if (lose) lose.loseContext();
      return true;
    } catch (e) {
      return false;
    }
  }

  function announce(available) {
    window.dispatchEvent(new CustomEvent("mapview3d-ready", { detail: { available } }));
  }

  let THREE;
  if (!webglAvailable() || !window.MapRender || !window.DashboardCamera) {
    window.MapView3D = { available: false };
    announce(false);
    return;
  }
  try {
    THREE = await import("/static/three/three.module.js");
  } catch (e) {
    console.warn("map-view3d: three.js failed to load; 3D view disabled", e);
    window.MapView3D = { available: false };
    announce(false);
    return;
  }

  // three.js's own fat-line addon (vendored unmodified, see static/three/
  // README.md) for route lines; falls back to plain 1px lines if it
  // can't load.
  let L2 = null;
  try {
    const [a, b, c] = await Promise.all([import("/static/three/addons/Line2.js"), import("/static/three/addons/LineGeometry.js"), import("/static/three/addons/LineMaterial.js")]);
    L2 = { Line2: a.Line2, LineGeometry: b.LineGeometry, LineMaterial: c.LineMaterial };
  } catch (e) {
    console.warn("map-view3d: line addon failed to load; routes drawn as plain lines", e);
  }

  // Hazard effects (flames, smoke, water, debris, haze, dust): a separate
  // module so the core view still works if it can't load.
  let FXMOD = null;
  try { FXMOD = await import("/static/dashboard/map-view3d-fx.js"); }
  catch (e) { console.warn("map-view3d: effects module failed to load; no hazard effects", e); }

  // Procedural buildings with damage states (intact / cracked / partial
  // collapse / rubble); falls back to plain boxes if it can't load.
  let BLDMOD = null;
  try { BLDMOD = await import("/static/dashboard/map-view3d-buildings.js"); }
  catch (e) { console.warn("map-view3d: buildings module failed to load; plain boxes used", e); }

  const MR = window.MapRender;
  const GLYPH_PX = 128;          // marker glyph canvas size
  const GLYPH_TEAM_R = 26;       // team glyph radius on that canvas (px)
  // Incident pins (3D only): a compact beacon on a short stem. Urgency is
  // shown by motion (a pulsing ring for P1, a steady one for P2), not size.
  const PIN_R_PX = 22;           // pin head radius on its glyph canvas (px)
  const PIN_R_WORLD = 0.22;      // ... and in world units
  const PIN_Z = 0.62;            // head height above the footing
  const PIN_HIT_R = 0.26;        // pick radius (world units, before MARKER_BOOST)
  const GROUND_TEX_PX_PER_CELL = 48;
  const FOV = 35;
  // Perspective makes the 3D map's markers read smaller than in 2D/2.5D at the same world size; scale sprites and hit areas up a little.
  const MARKER_BOOST = 1.4;

  const SETTINGS_KEY = "dashboard.map3dSettings";
  const DEFAULT_SETTINGS = { effects: "low", reduceMotion: false, lighting: "day", autoQuality: true };
  function loadSettings() {
    const s = Object.assign({}, DEFAULT_SETTINGS);
    try { s.reduceMotion = !!(window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches); } catch (e) { /* default */ }
    try {
      const saved = JSON.parse(localStorage.getItem(SETTINGS_KEY) || "{}");
      if (["off", "low", "high"].indexOf(saved.effects) >= 0) s.effects = saved.effects;
      if (typeof saved.reduceMotion === "boolean") s.reduceMotion = saved.reduceMotion;
      if (["day", "night"].indexOf(saved.lighting) >= 0) s.lighting = saved.lighting;
      if (typeof saved.autoQuality === "boolean") s.autoQuality = saved.autoQuality;
    } catch (e) { /* storage unavailable or corrupt - defaults */ }
    return s;
  }
  function saveSettings(s) {
    try { localStorage.setItem(SETTINGS_KEY, JSON.stringify(s)); } catch (e) { /* not remembered, still works */ }
  }

  function create(canvas) {
    const renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: false });
    renderer.shadowMap.enabled = true;
    renderer.shadowMap.type = THREE.PCFShadowMap;
    const maxAniso = renderer.capabilities.getMaxAnisotropy();

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x05080c);
    const camera = new THREE.PerspectiveCamera(FOV, 1, 0.5, 400);

    let lost = false;
    canvas.addEventListener("webglcontextlost", (e) => { e.preventDefault(); lost = true; });
    canvas.addEventListener("webglcontextrestored", () => { lost = false; });

    const settings = loadSettings();
    const fx = FXMOD ? FXMOD.createFx(THREE, { scene, MR }) : null;
    if (fx) { fx.setLevel(settings.effects); fx.setMotion(!settings.reduceMotion); }
    function applyLighting() {
      const L = LIGHTING[settings.lighting] || LIGHTING.day;
      scene.background.set(L.bg); scene.fog.color.set(L.fog);
      hemi.color.set(L.sky); hemi.groundColor.set(L.ground); hemi.intensity = L.hemi;
      sun.color.set(L.sun); sun.intensity = L.sunI; sunOff = L.sunOff;
      if (bld) bld.setNight(L.night);
      if (fx) fx.setNight(L.night);
    }
    const bld = BLDMOD ? BLDMOD.createBuildings(THREE, { scene, MR, maxAniso }) : null;
    let autoReduced = null;     // effects level the auto-quality fallback dropped to, if it did
    const _v2 = new THREE.Vector2();

    // ---------- lighting presets (day / night) ----------
    // Soft ambient (hemisphere) + one directional light with soft shadows,
    // and light distance fog so the far side of the map recedes. Markers,
    // routes and highlights ignore fog so they stay readable at any distance.
    const LIGHTING = {
      day:   { bg: 0x05080c, fog: 0x0b1119, sky: 0xc9d8ee, ground: 0x1b2430, hemi: 1.15, sun: 0xfff3df, sunI: 2.3, sunOff: [-22, 38, -14], night: false },
      night: { bg: 0x03050a, fog: 0x070b16, sky: 0x4a5c92, ground: 0x0b0f18, hemi: 0.75, sun: 0x9bb2ff, sunI: 0.9, sunOff: [16, 34, -22], night: true },
    };
    let sunOff = LIGHTING.day.sunOff;
    scene.fog = new THREE.Fog(LIGHTING.day.fog, 60, 200);

    // ---------- lights ----------
    const hemi = new THREE.HemisphereLight(0xc9d8ee, 0x1b2430, 1.15);
    scene.add(hemi);
    const sun = new THREE.DirectionalLight(0xfff3df, 2.3);
    sun.castShadow = true;
    sun.shadow.mapSize.set(1536, 1536);
    const sc = sun.shadow.camera;
    sc.left = -28; sc.right = 28; sc.top = 28; sc.bottom = -28; sc.near = 1; sc.far = 140;
    sun.shadow.radius = 4;      // blur the shadow edges (soft shadows)
    sun.shadow.bias = -0.0004;
    sun.shadow.normalBias = 0.03;
    scene.add(sun);
    scene.add(sun.target);
    applyLighting();

    // ---------- view state / projector ----------
    const view = { w: 1, h: 1, mapW: 36, mapH: 36, tx: 18, tz: 18, az: Math.PI / 4, el: 50 * Math.PI / 180, dist: 80 };
    const EL_MIN = 15 * Math.PI / 180, EL_MAX = Math.PI / 2; // ~15 degrees above the ground .. straight down
    const raycaster = new THREE.Raycaster();
    const tmpV = new THREE.Vector3();

    // Distance at which the whole map (its four ground corners, plus
    // headroom for tall buildings) just fits the canvas at the current
    // azimuth/elevation: start from a rough guess and rescale by how
    // much the projected corners over- or under-fill the view.
    function fitDistance() {
      const aspect = view.w / Math.max(1, view.h);
      const half = Math.tan((FOV * Math.PI) / 360);
      view.dist = (Math.hypot(view.mapW, view.mapH) / 2 / (half * Math.min(1, aspect))) * 1.1;
      for (let k = 0; k < 6; k++) {
        updateCamera();
        let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
        [[-0.5, -0.5, 0], [view.mapW - 0.5, -0.5, 0], [view.mapW - 0.5, view.mapH - 0.5, 0], [-0.5, view.mapH - 0.5, 0],
         [view.mapW / 2, view.mapH / 2, 2.5]].forEach(([x, y, z]) => {
          const p = worldToScreen(x, y, z);
          minX = Math.min(minX, p.x); maxX = Math.max(maxX, p.x); minY = Math.min(minY, p.y); maxY = Math.max(maxY, p.y);
        });
        const fill = Math.max((maxX - minX) / view.w, (maxY - minY) / view.h) / 0.88;
        if (Math.abs(fill - 1) < 0.01) break;
        view.dist = Math.max(6, Math.min(170, view.dist * fill));
      }
      return view.dist;
    }

    function updateCamera() {
      view.tx = Math.max(-2, Math.min(view.mapW + 2, view.tx));
      view.tz = Math.max(-2, Math.min(view.mapH + 2, view.tz));
      const ce = Math.cos(view.el);
      camera.position.set(view.tx + view.dist * Math.sin(view.az) * ce, view.dist * Math.sin(view.el), view.tz + view.dist * Math.cos(view.az) * ce);
      camera.aspect = view.w / Math.max(1, view.h);
      // Screen-up is "away from the viewer" along the ground, so the
      // camera stays well-defined looking straight down (where the
      // usual world-up vector would be parallel to the view direction).
      camera.up.set(-Math.sin(view.az), 0, -Math.cos(view.az));
      camera.lookAt(view.tx, 0, view.tz);
      camera.updateProjectionMatrix();
      camera.updateMatrixWorld(true);
      sun.position.set(view.tx + sunOff[0], sunOff[1], view.tz + sunOff[2]);
      scene.fog.near = view.dist * 0.9; scene.fog.far = view.dist * 2.6;
      sun.target.position.set(view.tx, 0, view.tz);
    }

    function worldToScreen(wx, wy, z) {
      tmpV.set(wx, z || 0, wy).project(camera);
      return { x: (tmpV.x * 0.5 + 0.5) * view.w, y: (-tmpV.y * 0.5 + 0.5) * view.h };
    }

    // A raycast through the screen point onto the horizontal plane at
    // height z (z = 0 is the ground): the 3D analogue of the iso
    // inverse projection, returning world (x, y).
    function screenToWorld(sx, sy, z) {
      raycaster.setFromCamera({ x: (sx / view.w) * 2 - 1, y: -(sy / view.h) * 2 + 1 }, camera);
      const o = raycaster.ray.origin, d = raycaster.ray.direction;
      if (Math.abs(d.y) < 1e-6) return { x: view.tx, y: view.tz };
      const t = ((z || 0) - o.y) / d.y;
      return { x: o.x + d.x * t, y: o.z + d.z * t };
    }

    const projector = {
      worldToScreen, screenToWorld,
      // Drag the ground: the point that was under the cursor at
      // (sx - dx, sy - dy) ends up under it at (sx, sy). Translating
      // the camera over the ground plane does this exactly.
      pan(dx, dy, sx, sy) {
        cancelCamera();
        const cx = sx === undefined ? view.w / 2 + dx : sx, cy = sy === undefined ? view.h / 2 + dy : sy;
        const a = screenToWorld(cx - dx, cy - dy, 0), b = screenToWorld(cx, cy, 0);
        view.tx += a.x - b.x; view.tz += a.y - b.y;
        updateCamera();
      },
      zoomAt(sx, sy, factor) {
        const before = screenToWorld(sx, sy, 0);
        view.dist = Math.max(6, Math.min(170, view.dist / factor));
        updateCamera();
        const after = screenToWorld(sx, sy, 0);
        view.tx += before.x - after.x; view.tz += before.y - after.y;
        updateCamera();
      },
      orbit(dx, dy) {
        cancelCamera();
        view.az -= dx * 0.008;
        view.el = Math.max(EL_MIN, Math.min(EL_MAX, view.el + dy * 0.006));
        updateCamera();
      },
      // Straight down at the whole map, keeping the current rotation.
      topDown() {
        cancelCamera();
        view.el = EL_MAX;
        view.tx = view.mapW / 2 - 0.5; view.tz = view.mapH / 2 - 0.5;
        view.dist = fitDistance();
        updateCamera();
      },
      reset() {
        cancelCamera();
        view.az = Math.PI / 4; view.el = 50 * Math.PI / 180;
        view.tx = view.mapW / 2 - 0.5; view.tz = view.mapH / 2 - 0.5;
        view.dist = fitDistance();
        updateCamera();
      },
      groundPxPerUnit(wx, wy) {
        const x = wx === undefined ? view.tx : wx, y = wy === undefined ? view.tz : wy;
        const a = worldToScreen(x, y, 0), bx = worldToScreen(x + 1, y, 0), by = worldToScreen(x, y + 1, 0);
        return (Math.hypot(bx.x - a.x, bx.y - a.y) + Math.hypot(by.x - a.x, by.y - a.y)) / 2;
      },
      worldDeltaToScreen(dx, dy, atX, atY, atZ) {
        const x = atX === undefined ? view.tx : atX, y = atY === undefined ? view.tz : atY;
        const a = worldToScreen(x, y, atZ || 0), b = worldToScreen(x + dx, y + dy, atZ || 0);
        return { x: b.x - a.x, y: b.y - a.y };
      },
      heightPx(z) {
        const a = worldToScreen(view.tx, view.tz, 0), b = worldToScreen(view.tx, view.tz, z);
        return Math.abs(b.y - a.y);
      },
    };

    // ---------- static scene (rebuilt only when the map changes) ----------
    const staticGroup = new THREE.Group();
    scene.add(staticGroup);
    let terrainSource = null, envSource = null, terrainParsed = null;
    let buildingMesh = null;
    const outline = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
      new THREE.LineBasicMaterial({ color: 0xffffff, depthTest: false, transparent: true, opacity: 0.9, fog: false }));
    outline.visible = false; outline.renderOrder = 20;
    scene.add(outline);

    function buildGround(terrain) {
      const old = staticGroup.getObjectByName("ground");
      if (old) { staticGroup.remove(old); old.traverse((o) => { if (o.material) { if (o.material.map) o.material.map.dispose(); o.material.dispose(); } if (o.geometry) o.geometry.dispose(); }); }
      if (!terrain) return;
      const W = terrain.width, H = terrain.height;
      const g = new THREE.Group(); g.name = "ground";
      const cx = W / 2 - 0.5, cz = H / 2 - 0.5;
      const slab = new THREE.Mesh(new THREE.BoxGeometry(W, 0.4, H), new THREE.MeshStandardMaterial({ color: 0x0b1118, roughness: 1 }));
      slab.position.set(cx, -0.2, cz);
      slab.receiveShadow = true;
      g.add(slab);
      const tex = new THREE.CanvasTexture(terrain.canvas);
      tex.colorSpace = THREE.SRGBColorSpace;
      tex.anisotropy = maxAniso;
      const plane = new THREE.Mesh(new THREE.PlaneGeometry(W, H),
        new THREE.MeshStandardMaterial({ map: tex, transparent: true, roughness: 1, metalness: 0 }));
      plane.rotation.x = -Math.PI / 2;
      plane.position.set(cx, 0.004, cz);
      plane.receiveShadow = true;
      g.add(plane);
      staticGroup.add(g);
      view.mapW = W; view.mapH = H;
    }

    function buildBuildings(prep) {
      if (buildingMesh) { staticGroup.remove(buildingMesh); buildingMesh.geometry.dispose(); buildingMesh.material.dispose(); buildingMesh = null; }
      if (!prep.list.length) return;
      const mesh = new THREE.InstancedMesh(new THREE.BoxGeometry(1, 1, 1),
        new THREE.MeshStandardMaterial({ roughness: 0.85, metalness: 0.02 }), prep.list.length);
      const m = new THREE.Matrix4(), c = new THREE.Color();
      prep.list.forEach((e, i) => {
        const w = e.x1 - e.x0, d = e.y1 - e.y0;
        m.compose(new THREE.Vector3((e.x0 + e.x1) / 2, e.h / 2, (e.y0 + e.y1) / 2), new THREE.Quaternion(), new THREE.Vector3(w, e.h, d));
        mesh.setMatrixAt(i, m);
        // Same neutral grey as 2D/2.5D, darkened by damage (e.f).
        c.setRGB(139 * e.f / 255, 152 * e.f / 255, 165 * e.f / 255, THREE.SRGBColorSpace);
        mesh.setColorAt(i, c);
      });
      mesh.castShadow = true; mesh.receiveShadow = true;
      staticGroup.add(mesh);
      buildingMesh = mesh;
    }

    // ---------- shared geometry / materials ----------
    const stemGeo = new THREE.CylinderGeometry(0.03, 0.03, 1, 6);
    const discGeo = new THREE.CircleGeometry(1, 20);
    const ringGeo = new THREE.RingGeometry(0.52, 0.62, 28);
    const planeGeo = new THREE.PlaneGeometry(1, 1);
    // Tip toward +Z; rotated about Y to face a team's direction of travel.
    const arrowGeo = new THREE.BufferGeometry();
    arrowGeo.setAttribute("position", new THREE.Float32BufferAttribute([0, 0.04, 0.3, -0.17, 0.04, -0.12, 0.17, 0.04, -0.12], 3));
    const shadowMat = new THREE.MeshBasicMaterial({ color: 0x000000, transparent: true, opacity: 0.4, depthWrite: false, fog: false });
    const selMat = new THREE.MeshBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.8, depthWrite: false, side: THREE.DoubleSide, fog: false });
    const matCache = new Map();
    function colorMat(hex, opacity) {
      const k = hex + "|" + opacity;
      let m = matCache.get(k);
      if (!m) { m = new THREE.MeshBasicMaterial({ color: new THREE.Color(hex), transparent: true, opacity, depthWrite: false, side: THREE.DoubleSide }); matCache.set(k, m); }
      return m;
    }
    const markerMats = new Map(); // hex -> { stem, arrow, ring, ... }
    const l2mats = [];            // fat-line materials: their resolution tracks the canvas size
    function markerMat(hex) {
      let m = markerMats.get(hex);
      if (!m) {
        const c = new THREE.Color(hex);
        m = {
          stem: new THREE.MeshStandardMaterial({ color: c, roughness: 0.6, fog: false }),
          arrow: new THREE.MeshBasicMaterial({ color: c, transparent: true, opacity: 0.95, depthWrite: false, side: THREE.DoubleSide, fog: false }),
          ring: new THREE.MeshBasicMaterial({ color: c, transparent: true, opacity: 0.85, depthWrite: false, side: THREE.DoubleSide, fog: false }),
          solid: new THREE.LineBasicMaterial({ color: c, transparent: true, opacity: 0.85, fog: false }),
          dashed: new THREE.LineDashedMaterial({ color: c, transparent: true, opacity: 0.95, dashSize: 0.22, gapSize: 0.16, fog: false }),
        };
        if (L2) {
          const mk = (dashed) => {
            const lm = new L2.LineMaterial({ color: hex, linewidth: 2.4, transparent: true, opacity: 0.92, dashed, dashSize: 0.22, gapSize: 0.16, worldUnits: false });
            lm.resolution.set(view.w, view.h);
            l2mats.push(lm);
            return lm;
          };
          m.solid2 = mk(false); m.dashed2 = mk(true);
        }
        markerMats.set(hex, m);
      }
      return m;
    }
    const AXIS_X = new THREE.Vector3(1, 0, 0);
    const FLAT = -Math.PI / 2; // lay a plane/disc flat on the ground

    // Per-tick batches (one draw call each): hazard-cell tints and
    // incident stems / ground shadows. Teams are NOT batched - they move
    // every frame, so each has its own small set of objects.
    function makePool(geo, mat, capacity, colored) {
      const mesh = new THREE.InstancedMesh(geo, mat, capacity);
      mesh.count = 0; mesh.frustumCulled = false;
      if (colored) mesh.setColorAt(0, new THREE.Color(1, 1, 1));
      scene.add(mesh);
      const m4 = new THREE.Matrix4(), q = new THREE.Quaternion(), p = new THREE.Vector3(), sc2 = new THREE.Vector3(), col = new THREE.Color();
      return {
        mesh,
        reset() { mesh.count = 0; },
        add(px, py, pz, sx, sy, sz, rotX, hex) {
          if (mesh.count >= capacity) return;
          q.setFromAxisAngle(AXIS_X, rotX || 0);
          m4.compose(p.set(px, py, pz), q, sc2.set(sx, sy, sz));
          mesh.setMatrixAt(mesh.count, m4);
          if (colored && hex !== undefined) mesh.setColorAt(mesh.count, col.set(hex));
          mesh.count++;
        },
        commit() {
          mesh.instanceMatrix.needsUpdate = true;
          if (mesh.instanceColor) mesh.instanceColor.needsUpdate = true;
        },
      };
    }
    const hazardPools = [0.38, 0.24, 0.12].map((o) => makePool(planeGeo, colorMat(0xd29922, o), 2048, false));
    const stemPool = makePool(stemGeo, new THREE.MeshStandardMaterial({ roughness: 0.6, fog: false }), 512, true);
    stemPool.mesh.castShadow = true;
    const blobPool = makePool(discGeo, shadowMat, 512, false);
    const dotPool = makePool(discGeo, new THREE.MeshBasicMaterial({ transparent: true, opacity: 0.95, depthWrite: false, fog: false }), 512, true); // ground dot at each pin's position

    // Marker glyph sprites: the 2D glyph drawn on a small canvas, cached
    // by the fields that change how it looks. Each gets a normal sprite
    // and a "ghost" that only draws where something is IN FRONT of it
    // (depthFunc Greater) - so a marker behind a building is never lost.
    // Markers ignore fog so they stay fully readable at any distance.
    const glyphCache = new Map();
    function glyphSprites(key, drawFn, k) {
      let g = glyphCache.get(key);
      if (!g) {
        const cv = document.createElement("canvas");
        cv.width = cv.height = GLYPH_PX * (k || 1);
        const cx = cv.getContext("2d");
        if (k) cx.scale(k, k);                       // draw in 128-unit coordinates on a sharper canvas
        drawFn(cx);
        const tex = new THREE.CanvasTexture(cv);
        tex.colorSpace = THREE.SRGBColorSpace;
        tex.anisotropy = 4;
        g = {
          solid: new THREE.SpriteMaterial({ map: tex, transparent: true, depthWrite: true, fog: false }),
          ghost: new THREE.SpriteMaterial({ map: tex, transparent: true, opacity: 0.38, depthTest: true, depthWrite: false, depthFunc: THREE.GreaterDepth, fog: false }),
        };
        glyphCache.set(key, g);
        if (glyphCache.size > 400) { // bound memory: drop the oldest
          const first = glyphCache.keys().next().value;
          const old = glyphCache.get(first);
          old.solid.map.dispose(); old.solid.dispose(); old.ghost.dispose();
          glyphCache.delete(first);
        }
      }
      return g;
    }

    // Pin levels of detail (by the pin head's size on screen):
    //   0 far: colour + P-label (+ halo, drawn separately)
    //   1 medium: + assignment (hollow = unassigned, team-colour token, warning if that team is faulted) + group count
    //   2 close / hovered / selected: + progress arc, type icon, victim badge
    const LOD_MED_PX = 12, LOD_CLOSE_PX = 26, HOVER_HEAD_PX = 18;

    // Small vector icons for the incident types (INCIDENT_TYPE_NAMES order).
    function drawTypeIcon(ctx, type, x, y, s) {
      ctx.save(); ctx.translate(x, y); ctx.strokeStyle = "#fff"; ctx.fillStyle = "#fff"; ctx.lineWidth = 1.7; ctx.lineJoin = "round"; ctx.lineCap = "round";
      const poly = (pts, close) => { ctx.beginPath(); pts.forEach((q, i) => (i ? ctx.lineTo(q[0] * s, q[1] * s) : ctx.moveTo(q[0] * s, q[1] * s))); if (close) ctx.closePath(); ctx.stroke(); };
      switch (type) {
        case 0: poly([[-1, 1], [-1, -0.2], [-0.35, -0.7], [0, -0.1], [0.4, -0.8], [1, -0.2], [1, 1]], true); break;                       // collapsed building
        case 1: ctx.strokeRect(-s, -s, 2 * s, 2 * s); ctx.strokeRect(-s * 0.4, -s * 0.4, s * 0.8, s * 0.8); break;                          // confined space
        case 2: ctx.fillRect(-s * 0.3, -s, s * 0.6, 2 * s); ctx.fillRect(-s, -s * 0.3, 2 * s, s * 0.6); break;                              // mass casualty: cross
        case 3: poly([[-0.35, -1], [-0.35, -0.2], [-1, 0.9], [1, 0.9], [0.35, -0.2], [0.35, -1]], false); break;                             // chemical: flask
        case 4: ctx.font = "bold " + Math.round(s * 2.4) + "px sans-serif"; ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText("?", 0, s * 0.1); break;
        case 5: ctx.beginPath(); ctx.arc(0, s * 0.6, s * 0.2, 0, 7); ctx.fill(); ctx.beginPath(); ctx.arc(0, s * 0.6, s * 0.8, -2.3, -0.84); ctx.stroke(); ctx.beginPath(); ctx.arc(0, s * 0.6, s * 1.5, -2.2, -0.94); ctx.stroke(); break; // survivor signal
        case 6: ctx.beginPath(); ctx.arc(-s * 0.45, s * 0.1, s * 0.5, 0, 7); ctx.moveTo(s * 0.95, s * 0.1); ctx.arc(s * 0.45, s * 0.1, s * 0.5, 0, 7); ctx.moveTo(s * 0.5, -s * 0.4); ctx.arc(0, -s * 0.35, s * 0.55, 0, 7); ctx.stroke(); break; // gas cloud
        case 7: ctx.beginPath(); ctx.moveTo(0, -s * 1.1); ctx.bezierCurveTo(s * 0.2, -s * 0.4, s * 0.95, -s * 0.2, s * 0.8, s * 0.4); ctx.bezierCurveTo(s * 0.7, s * 1.0, -s * 0.7, s * 1.0, -s * 0.8, s * 0.4); ctx.bezierCurveTo(-s * 0.9, -s * 0.1, -s * 0.2, -s * 0.5, 0, -s * 1.1); ctx.fill(); break; // flame
        case 8: ctx.strokeRect(-s, -s * 0.6, 2 * s, 1.6 * s); poly([[-1, -0.1], [1, -0.1]], false); break;                                     // cache: box
        case 9: poly([[-1, 0], [0, -1], [1, 0]], false); poly([[-0.7, 0], [-0.7, 0.9], [0.7, 0.9], [0.7, 0]], false); break;                  // safe zone: house
        case 10: ctx.beginPath(); ctx.arc(0, -s * 0.65, s * 0.35, 0, 7); ctx.fill(); poly([[0, -0.2], [0, 0.45], [-0.6, 1], [0, 0.45], [0.6, 1]], false); poly([[-0.65, 0.05], [0.65, 0.05]], false); break; // evacuee
        default: ctx.beginPath(); ctx.arc(0, 0, s * 0.3, 0, 7); ctx.fill();
      }
      ctx.restore();
    }

    // The pin: a round beacon head in the priority colour with the priority
    // label ("P1".."P4") in it. Drawn on a canvas and used as a camera-facing
    // sprite, so it reads from any angle. `d`: { color, inc, members, selected,
    // lod, label, hollow (nobody assigned), ghost (undiscovered, god view),
    // assignColor, fault, progress (0..1 or null), victims }.
    function drawPinGlyph(ctx, d) {
      const cx = GLYPH_PX / 2, cy = GLYPH_PX / 2, r = PIN_R_PX, color = d.color, inc = d.inc;
      const hollow = d.hollow && d.lod >= 1;              // far out every pin is a solid colour dot; hollow = unassigned appears from medium zoom
      ctx.save();
      ctx.globalAlpha = d.ghost ? 0.42 : 1;
      ctx.beginPath(); ctx.arc(cx, cy, r + 3, 0, Math.PI * 2); ctx.fillStyle = "rgba(8,12,18,0.85)"; ctx.fill();   // dark rim for contrast on any ground
      if (hollow || d.ghost) {
        ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.fillStyle = "rgba(10,14,20,0.62)"; ctx.fill();    // hollow: outlined pin, ground shows through
        ctx.beginPath(); ctx.arc(cx, cy, r - 2.5, 0, Math.PI * 2); ctx.strokeStyle = color; ctx.lineWidth = 5;
        if (d.ghost) ctx.setLineDash([6, 5]);
        ctx.stroke();
      } else {
        const grad = ctx.createRadialGradient(cx - r * 0.35, cy - r * 0.4, r * 0.1, cx, cy, r);
        grad.addColorStop(0, "#ffffff"); grad.addColorStop(0.22, color); grad.addColorStop(1, color);
        ctx.beginPath(); ctx.arc(cx, cy, r, 0, Math.PI * 2); ctx.fillStyle = grad; ctx.fill();
      }
      ctx.restore();
      if (d.progress !== null && d.progress !== undefined && d.lod >= 2) {         // work progress: arc filling clockwise from 12 o'clock
        ctx.save(); ctx.globalAlpha = d.ghost ? 0.42 : 1; ctx.lineCap = "butt";
        ctx.beginPath(); ctx.arc(cx, cy, r - 3, 0, Math.PI * 2); ctx.strokeStyle = "rgba(0,0,0,0.38)"; ctx.lineWidth = 4; ctx.stroke();
        if (d.progress > 0) { ctx.beginPath(); ctx.arc(cx, cy, r - 3, -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * d.progress); ctx.strokeStyle = "#3fb950"; ctx.lineWidth = 4; ctx.stroke(); }
        ctx.restore();
      }
      if (d.label) {                                       // the priority, as text: priority never relies on colour alone
        ctx.save(); ctx.globalAlpha = d.ghost ? 0.6 : 1;
        ctx.fillStyle = (hollow || d.ghost) ? color : (inc.priority >= 3 ? "#0d1117" : "#fff");
        ctx.font = "bold 26px sans-serif"; ctx.textAlign = "center"; ctx.textBaseline = "middle";
        if (hollow || d.ghost) { ctx.lineWidth = 4; ctx.strokeStyle = "rgba(8,12,18,0.9)"; ctx.strokeText("P" + inc.priority, cx, cy + 1); }
        ctx.fillText("P" + inc.priority, cx, cy + 1);
        ctx.restore();
      }
      if (d.selected) { ctx.beginPath(); ctx.arc(cx, cy, r + 8, 0, Math.PI * 2); ctx.strokeStyle = "#fff"; ctx.lineWidth = 3; ctx.stroke(); }
      const badge = (x, y, dia, fill, text, fg, font) => {
        ctx.beginPath(); ctx.arc(x, y, dia / 2, 0, Math.PI * 2); ctx.fillStyle = fill; ctx.fill();
        ctx.lineWidth = 2; ctx.strokeStyle = "rgba(8,12,18,0.9)"; ctx.stroke();
        if (text) { ctx.fillStyle = fg; ctx.font = font; ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText(text, x, y + 1); }
      };
      if (d.lod >= 1 && d.members.length > 1) badge(cx - r * 0.95, cy - r * 0.95, 26, "#1f6feb", String(d.members.length), "#fff", "bold 15px sans-serif");
      if (d.lod >= 1 && d.assignColor) {                                // assigned: a ring in the assigned team's colour
        ctx.beginPath(); ctx.arc(cx + r * 0.95, cy + r * 0.95, 10, 0, Math.PI * 2); ctx.fillStyle = "rgba(8,12,18,0.9)"; ctx.fill();
        ctx.beginPath(); ctx.arc(cx + r * 0.95, cy + r * 0.95, 8, 0, Math.PI * 2); ctx.strokeStyle = d.assignColor; ctx.lineWidth = 5; ctx.stroke();
      }
      if (d.lod >= 1 && d.fault) {                                      // the assigned team has an active fault
        const x = cx, y = cy + r * 1.38;
        ctx.beginPath(); ctx.moveTo(x, y - 11); ctx.lineTo(x + 12, y + 9); ctx.lineTo(x - 12, y + 9); ctx.closePath();
        ctx.fillStyle = "#d29922"; ctx.fill(); ctx.lineWidth = 2; ctx.strokeStyle = "rgba(8,12,18,0.95)"; ctx.stroke();
        ctx.fillStyle = "#0d1117"; ctx.font = "bold 14px sans-serif"; ctx.textAlign = "center"; ctx.textBaseline = "middle"; ctx.fillText("!", x, y + 3);
      }
      if (d.lod >= 2) {
        if (d.victims > 0) badge(cx + r * 0.95, cy - r * 0.95, 26, "#f0f3f6", String(d.victims > 99 ? "99+" : d.victims), "#0d1117", "bold 15px sans-serif");
        badge(cx - r * 0.9, cy + r * 0.95, 24, "#0d1117", "", "#fff", "");
        drawTypeIcon(ctx, inc.type, cx - r * 0.9, cy + r * 0.95, 6);
      }
    }

    // Urgency as motion. P1 and P2 get a soft glow halo behind the pin: P1
    // pulses slowly, P2 holds steady. With Reduce motion the halo stays but
    // does not pulse (uMotion = 0). One THREE.Points object holds every halo.
    const pulseU = { uTime: { value: 0 }, uMotion: { value: 1 } };
    pulseU.uMotion.value = settings.reduceMotion ? 0 : 1;
    let haloPts = null;
    function buildHalos() {
      if (haloPts) { dynGroup.remove(haloPts); haloPts.geometry.dispose(); haloPts = null; }
      if (!haloList.length) return;
      const pos = new Float32Array(haloList.length * 3), seed = new Float32Array(haloList.length * 4);
      haloList.forEach((h, i) => { pos.set([h.x, h.y, h.z], i * 3); const c = new THREE.Color(h.hex); seed.set([c.r, c.g, c.b, h.p], i * 4); });
      const g = new THREE.BufferGeometry();
      g.setAttribute("position", new THREE.BufferAttribute(pos, 3)); g.setAttribute("aSeed", new THREE.BufferAttribute(seed, 4));
      const m = new THREE.ShaderMaterial({
        uniforms: { uTime: pulseU.uTime, uMotion: pulseU.uMotion, uScale: { value: 600 } },
        vertexShader: `attribute vec4 aSeed; uniform float uTime, uMotion, uScale; varying vec4 vC; varying float vA;
          void main() { float pulse = aSeed.w < 1.5 ? (uMotion > 0.5 ? 0.5 + 0.5 * sin(uTime * 2.4 + position.x) : 0.6) : 1.0;
            vec4 mv = modelViewMatrix * vec4(position, 1.0); gl_Position = projectionMatrix * mv;
            float size = (aSeed.w < 1.5 ? 1.5 + 0.7 * pulse : 1.35); gl_PointSize = clamp(size * uScale / max(0.1, -mv.z), 22.0 + 6.0 * pulse, 200.0);
            vC = vec4(aSeed.xyz, 1.0); vA = aSeed.w < 1.5 ? 0.35 + 0.35 * (1.0 - pulse) : 0.4; }`,
        fragmentShader: "varying vec4 vC; varying float vA; void main() { vec2 c = gl_PointCoord - 0.5; float d = length(c); float ring = smoothstep(0.5, 0.0, d); gl_FragColor = vec4(vC.rgb, ring * ring * vA); }",
        transparent: true, depthWrite: false, depthTest: false, blending: THREE.AdditiveBlending,
      });
      haloPts = new THREE.Points(g, m); haloPts.frustumCulled = false; haloPts.renderOrder = 4;
      haloPts.onBeforeRender = (r) => { const sz = new THREE.Vector2(); r.getDrawingBufferSize(sz); m.uniforms.uScale.value = sz.y / (2 * Math.tan(FOV * Math.PI / 360)); };
      dynGroup.add(haloPts);
    }

    // Pixels per world unit at a point (for hit radii: a marker's hit
    // area is the disc you see, as in 2.5D).
    function pxPerWorldAt(wx, wy, z) {
      const a = worldToScreen(wx, wy, z), b = worldToScreen(wx + 1, wy, z), c = worldToScreen(wx, wy + 1, z);
      return (Math.hypot(b.x - a.x, b.y - a.y) + Math.hypot(c.x - a.x, c.y - a.y)) / 2;
    }

    // ---------- per-tick scene: incidents, hazards, safe zones ----------
    const dynGroup = new THREE.Group();
    scene.add(dynGroup);
    let dynDisposables = [];

    function addGroundRing(wx, wy, y, scale, mat) {
      const r = new THREE.Mesh(ringGeo, mat);
      r.rotation.x = -Math.PI / 2; r.position.set(wx, y + 0.02, wy); r.scale.set(scale, scale, 1);
      dynGroup.add(r);
    }
    function addDashedLine(points, hex, opacity) {
      const geo = new THREE.BufferGeometry().setFromPoints(points.map((p) => new THREE.Vector3(p[0], p[1], p[2])));
      const mat = new THREE.LineDashedMaterial({ color: new THREE.Color(hex), transparent: true, opacity, dashSize: 0.25, gapSize: 0.18, depthWrite: false });
      const line = new THREE.Line(geo, mat);
      line.computeLineDistances();
      dynGroup.add(line);
      dynDisposables.push(geo, mat);
    }

    let incidentHits = [], safezoneHits = [], pinInfo = [], pinSprites = [], pulseCount = 0, haloList = [], escList = [], escPulses = [];
    const workMax = new Map(), lastPri = new Map();
    let trackTick = null, hoverIncident = null;
    let lastIncidentSig = null;

    function rebuildTickScene(model, o) {
      dynGroup.clear();
      hazardPools.forEach((p) => p.reset()); stemPool.reset(); blobPool.reset(); dotPool.reset();
      dynDisposables.forEach((d) => d.dispose()); dynDisposables = [];
      incidentHits = []; safezoneHits = [];

      if (model.epicenter) {
        const ep = model.epicenter, pts = [];
        for (let i = 0; i <= 48; i++) { const a = (i / 48) * Math.PI * 2; pts.push([ep.x + Math.cos(a) * 1.2, 0.03, ep.y + Math.sin(a) * 1.2]); }
        addDashedLine(pts, 0xa371f7, 0.7);
      }
      model.hazardCells.forEach((c) => {
        hazardPools[c.blocked ? 0 : (c.hazard_flags ? 1 : 2)].add(c.x, 0.02, c.y, 1, 1, 1, FLAT);
      });
      model.safezones.forEach((sz) => {
        const half = 0.42;
        const m = new THREE.Mesh(planeGeo, colorMat(0x3fb950, 0.16));
        m.rotation.x = -Math.PI / 2; m.position.set(sz.x, 0.022, sz.y); m.scale.set(half * 2, half * 2, 1);
        dynGroup.add(m);
        addDashedLine([[sz.x - half, 0.03, sz.y - half], [sz.x + half, 0.03, sz.y - half], [sz.x + half, 0.03, sz.y + half],
                       [sz.x - half, 0.03, sz.y + half], [sz.x - half, 0.03, sz.y - half]], 0x3fb950, 1);
        safezoneHits.push({ type: "safezone", id: sz.id, wx: sz.x, wy: sz.y, wr: 0.5, data: sz });
      });

      pinInfo = []; haloList = []; pinSprites = []; escList = [];
      const tickNow = o.frame ? o.frame.tick : null;
      if (tickNow !== null && trackTick !== null && tickNow < trackTick) { workMax.clear(); lastPri.clear(); }   // time went backwards (replay seek): re-learn
      trackTick = tickNow;
      const teamById = new Map(model.teams.map((mt) => [mt.t.id, mt]));
      const clock = performance.now() / 1000;
      model.incidents.forEach((mi) => {
        const { members, inc, wx, wy, foot, headZ, color, isSel } = mi;
        const victims = members.reduce((a, m) => a + (m.victims || 0), 0);
        // Priority escalation (a lower number than last seen) -> a brief pulse; never on first sight.
        let escalated = false;
        members.forEach((m) => { const pr = lastPri.get(m.id); if (pr !== undefined && m.priority < pr) escalated = true; lastPri.set(m.id, m.priority); });
        if (escalated && !settings.reduceMotion) escPulses.push({ x: wx, y: headZ, z: wy, hex: new THREE.Color(color).getHex(), start: clock });
        // Work progress: work_remaining is sent but the starting amount is not, so the arc
        // measures against the largest value seen for the incident (an estimate).
        let wr = 0, wm = 0;
        members.forEach((m) => { const w = m.work_remaining || 0; workMax.set(m.id, Math.max(workMax.get(m.id) || 0, w)); wr += w; wm += workMax.get(m.id); });
        const progress = wm > 0 ? Math.max(0, Math.min(1, 1 - wr / wm)) : null;
        // Assignment: who is working it (assigned_team 255 = nobody), and whether that team has a fault.
        const assigned = [];
        members.forEach((m) => [m.assigned_team, m.support_team].forEach((tid) => {
          const mt = tid !== undefined && tid !== 255 ? teamById.get(tid) : null;
          if (mt && !assigned.some((a) => a.team === tid)) assigned.push({ team: tid, color: mt.color, fault: !!mt.t.fault_mask });
        }));
        const spec = { color, inc, members, selected: isSel, hollow: assigned.length === 0, ghost: members.every((m) => m.discovered === 0),
                       assignColor: assigned.length ? assigned[0].color : null, fault: assigned.some((a) => a.fault),
                       progress: progress === null ? null : Math.round(progress * 24) / 24, victims };
        const hex = new THREE.Color(color).getHex();
        stemPool.add(wx, foot + (headZ - foot) / 2, wy, 0.8, headZ - foot, 0.8, 0, hex);           // short stem
        blobPool.add(wx, foot + 0.012, wy, 0.2, 0.2, 1, FLAT);                                    // soft ground shadow
        dotPool.add(wx, foot + 0.02, wy, 0.2, 0.2, 1, FLAT, hex);                                 // small ground dot = the exact position
        const scale = (GLYPH_PX / PIN_R_PX) * PIN_R_WORLD;
        const s = new THREE.Sprite(new THREE.SpriteMaterial()); s.position.set(wx, headZ, wy); s.scale.set(scale, scale, 1); s.renderOrder = 5;
        const gh = new THREE.Sprite(new THREE.SpriteMaterial()); gh.position.copy(s.position); gh.scale.copy(s.scale); gh.renderOrder = 6;
        dynGroup.add(s, gh);
        // Urgency as motion: a glow halo behind P1 (pulsing) and P2 (steady); P3/P4 none.
        let ring = "none";
        if (inc.priority <= 2) { haloList.push({ x: wx, y: headZ, z: wy, p: inc.priority, hex }); ring = inc.priority === 1 ? "pulse" : "steady"; }
        if (isSel) addGroundRing(wx, wy, foot, 0.7, selMat);
        const info = { id: inc.id, ids: members.map((m) => m.id), priority: inc.priority, ring, label: false, lod: 0, hollow: spec.hollow, ghost: spec.ghost,
                       assigned, progress, victims, group: members.length, foot, headZ, scale, escalated, type: inc.type };
        pinInfo.push(info);
        const pin = { wx, wy, headZ, s, gh, spec, info, vars: new Map(), cur: -1 };
        pin.variant = (idx) => {                                        // idx = lod * 2 + (label ? 1 : 0); drawn on demand
          let g = pin.vars.get(idx);
          if (!g) {
            const lod = idx >> 1, label = (idx & 1) === 1, sp = Object.assign({}, spec, { lod, label });
            const key = ["P", idx, inc.id, inc.type, inc.priority, victims, members.length, sp.selected ? 1 : 0, color, sp.hollow ? 1 : 0, sp.ghost ? 1 : 0,
                         sp.assignColor || "-", sp.fault ? 1 : 0, sp.progress === null ? "-" : sp.progress].join("|");
            g = glyphSprites(key, (ctx) => drawPinGlyph(ctx, sp), 2);
            pin.vars.set(idx, g);
          }
          return g;
        };
        pinSprites.push(pin);
        incidentHits.push({ type: "incident", id: inc.id, wx, wy, wz: headZ, wr: PIN_HIT_R,
                            data: members.length > 1 ? Object.assign({}, inc, { groupIncidents: members }) : inc });
      });
      escPulses = escPulses.filter((e) => clock - e.start < ESC_SECS);
      buildEscalations();
      updatePinLabels();
      buildHalos();
      pulseCount = pinInfo.filter((p) => p.ring === "pulse").length;
      hazardPools.forEach((p) => p.commit()); stemPool.commit(); blobPool.commit(); dotPool.commit();
    }

    // ---------- teams: persistent objects, smooth movement, routes ----------
    // Every team has its own objects (stem, ground shadow, glyph sprites,
    // heading arrow, selection ring, route line) that live across ticks
    // and are re-posed every animation frame. A tick moves a team one
    // cell; its position is interpolated from where it was to where it
    // is over the time between ticks (measured from how fast frames are
    // actually arriving, so Live pace and Replay speed both fall out of
    // it, and a pause - no new frames - simply lets the glide finish).
    const teamGroup = new THREE.Group();
    scene.add(teamGroup);
    const teamStates = new Map(); // id -> motion state
    const teamViews = new Map();  // id -> scene objects
    let lastTick = null, lastSyncAt = 0, emaGap = 150, fxSynced = false;
    const SNAP_CELLS = 2.6;       // a bigger jump than this is a teleport/seek, not a move

    function ensureTeamView(id) {
      let v = teamViews.get(id);
      if (v) return v;
      const g = new THREE.Group();
      const stem = new THREE.Mesh(stemGeo, markerMat(0xffffff).stem); stem.castShadow = true;
      const blob = new THREE.Mesh(discGeo, shadowMat); blob.rotation.x = FLAT; blob.scale.set(0.34, 0.34, 1);
      const solid = new THREE.Sprite(new THREE.SpriteMaterial()); solid.renderOrder = 5;
      const ghost = new THREE.Sprite(new THREE.SpriteMaterial()); ghost.renderOrder = 6;
      const ring = new THREE.Mesh(ringGeo, selMat); ring.rotation.x = FLAT; ring.visible = false;
      const arrow = new THREE.Mesh(arrowGeo, markerMat(0xffffff).arrow);
      g.add(stem, blob, solid, ghost, ring, arrow);
      teamGroup.add(g);
      v = { id, g, stem, blob, solid, ghost, ring, arrow, route: null, routeObjs: new THREE.Group(), pos: { x: 0, y: 0, foot: 0 }, data: null, color: null, dashed: false };
      teamGroup.add(v.routeObjs);
      teamViews.set(id, v);
      return v;
    }
    function disposeRoute(v) {
      v.routeObjs.children.slice().forEach((c) => { v.routeObjs.remove(c); if (c.geometry && c.userData.ownGeo) c.geometry.dispose(); });
      v.route = null;
    }
    function removeTeamView(id) {
      const v = teamViews.get(id);
      if (!v) return;
      disposeRoute(v);
      teamGroup.remove(v.g); teamGroup.remove(v.routeObjs);
      v.solid.material.dispose(); v.ghost.material.dispose();
      teamViews.delete(id); teamStates.delete(id);
    }

    // Advance each team's motion target for a NEW tick.
    function advanceTeams(model, tick, now, snapAll) {
      const sequential = lastTick !== null && tick === lastTick + 1 && !snapAll;
      const gap = now - lastSyncAt;
      if (sequential) {
        emaGap = gap < 1500 ? (emaGap * 0.6 + gap * 0.4) : emaGap;
      }
      // One tick's glide lasts as long as ticks are actually arriving; a
      // lone manual step (long gap) gets a short fixed glide.
      const dur = sequential ? (gap < 1500 ? Math.max(40, Math.min(1500, emaGap)) : 250) : 0;
      model.teams.forEach((mt) => {
        const { t } = mt;
        let st = teamStates.get(t.id);
        const fresh = !st;
        if (fresh) {
          st = { px: mt.wx, py: mt.wy, pf: mt.foot, cx: mt.wx, cy: mt.wy, cf: mt.foot, t0: now, dur: 0, heading: null,
                 dest: null, rerouteUntil: -1 };
          teamStates.set(t.id, st);
        }
        const moved = Math.hypot(mt.wx - st.cx, mt.wy - st.cy);
        const nowPos = fresh ? { x: mt.wx, y: mt.wy, f: mt.foot } : poseOf(st, now);
        if (!sequential || fresh || moved > SNAP_CELLS) {
          st.px = st.cx = mt.wx; st.py = st.cy = mt.wy; st.pf = st.cf = mt.foot; st.dur = 0;
        } else {
          // Start from where the marker is drawn right now so a tween
          // interrupted by the next tick doesn't jump back.
          st.px = nowPos.x; st.py = nowPos.y; st.pf = nowPos.f;
          st.cx = mt.wx; st.cy = mt.wy; st.cf = mt.foot;
          st.t0 = now; st.dur = dur;
          const dx = st.cx - st.px, dy = st.cy - st.py;
          if (Math.hypot(dx, dy) > 0.05) st.heading = Math.atan2(dx, dy);
        }
        // Rerouting: a team that already had a destination now has a
        // different one.
        const hasDest = !!t.has_dest;
        const dest = hasDest ? [t.dest_x, t.dest_y, t.dest_id] : null;
        if (st.dest && dest && (st.dest[0] !== dest[0] || st.dest[1] !== dest[1])) st.rerouteUntil = tick + 3;
        st.dest = dest;
      });
      lastTick = tick; lastSyncAt = now;
    }

    function poseOf(st, now) {
      const k = st.dur > 0 ? Math.max(0, Math.min(1, (now - st.t0) / st.dur)) : 1;
      return { x: st.px + (st.cx - st.px) * k, y: st.py + (st.cy - st.py) * k, f: st.pf + (st.cf - st.pf) * k, k };
    }

    // Refresh per-team look (colour, glyph, selection) - every render call.
    function syncTeamViews(model) {
      const seen = new Set();
      model.teams.forEach((mt) => {
        const { t, i, color, isSel } = mt;
        seen.add(t.id);
        const v = ensureTeamView(t.id);
        v.data = { ...t, i };
        if (v.color !== color) {
          v.color = color;
          const hex = new THREE.Color(color).getHex();
          const mm = markerMat(hex);
          v.stem.material = mm.stem; v.arrow.material = mm.arrow; v.mm = mm;
        }
        const phase = window._overridePhase && window._overridePhase[t.id];
        const key = ["T", t.id, t.status, t.fault_mask || 0, t.override ? 1 : 0, phase ? phase.kind : "", isSel ? 1 : 0, color].join("|");
        if (v.glyphKey !== key) {
          v.glyphKey = key;
          const mats = glyphSprites(key, (ctx) => MR.drawTeamGlyph(ctx, { x: GLYPH_PX / 2, y: GLYPH_PX / 2 }, GLYPH_TEAM_R, color, t, isSel ? { type: "team", id: t.id } : null));
          v.solid.material = mats.solid; v.ghost.material = mats.ghost;
          const sc3 = (GLYPH_PX / GLYPH_TEAM_R) * MR.MARKER_WORLD_R * MARKER_BOOST;
          v.solid.scale.set(sc3, sc3, 1); v.ghost.scale.set(sc3, sc3, 1);
        }
        v.isSel = isSel;
        v.ring.visible = isSel;
        v.faulted = !!(t.fault_mask || t.degraded);
      });
      Array.from(teamViews.keys()).forEach((id) => { if (!seen.has(id)) removeTeamView(id); });
    }

    // Route for every team with a destination, for the CURRENT tick.
    function rebuildRoutes(model, tick) {
      model.teams.forEach((mt) => {
        const { t } = mt;
        const v = teamViews.get(t.id), st = teamStates.get(t.id);
        if (!v || !st) return;
        disposeRoute(v);
        if (!(t.has_dest && (t.dest_x !== t.x || t.dest_y !== t.y))) return;
        const path = terrainParsed ? window.RoutePlanner.plan(terrainParsed, t.x, t.y, t.dest_x, t.dest_y) : null;
        const cells = path || [[t.x, t.y], [t.dest_x, t.dest_y]]; // no route known: straight line, drawn dashed
        const dashed = v.faulted || tick < st.rerouteUntil || !path;
        const n = cells.length + 1;
        const arr = new Float32Array(n * 3);
        arr[1] = 0.06;
        cells.forEach((c, k) => { arr[(k + 1) * 3] = c[0]; arr[(k + 1) * 3 + 1] = 0.06; arr[(k + 1) * 3 + 2] = c[1]; });
        const mm = v.mm || markerMat(0xffffff);
        let route;
        if (L2) {
          const geo = new L2.LineGeometry();
          geo.setPositions(arr);
          const line = new L2.Line2(geo, dashed ? mm.dashed2 : mm.solid2);
          if (dashed) line.computeLineDistances();
          line.frustumCulled = false; line.userData.ownGeo = true; line.renderOrder = 3;
          v.routeObjs.add(line);
          route = { line, geo, l2: true, dashed, cells };
        } else {
          const geo = new THREE.BufferGeometry();
          const attr = new THREE.BufferAttribute(arr, 3); attr.setUsage(THREE.DynamicDrawUsage);
          geo.setAttribute("position", attr);
          const line = new THREE.Line(geo, dashed ? mm.dashed : mm.solid);
          line.frustumCulled = false; line.userData.ownGeo = true; line.renderOrder = 3;
          v.routeObjs.add(line);
          route = { line, attr, l2: false, dashed, cells };
          if (dashed) line.computeLineDistances();
        }
        const dr = new THREE.Mesh(ringGeo, mm.ring);
        dr.rotation.x = FLAT; dr.position.set(t.dest_x, 0.07, t.dest_y); dr.scale.set(0.5, 0.5, 1);
        v.routeObjs.add(dr);
        v.route = route;
        if (st.heading === null && cells.length > 1) st.heading = Math.atan2(cells[1][0] - cells[0][0], cells[1][1] - cells[0][1]);
      });
    }

    function poseTeams(now) {
      teamViews.forEach((v, id) => {
        const st = teamStates.get(id);
        if (!st) return;
        const p = poseOf(st, now);
        const headZ = p.f + MR.ISO_TEAM_Z;
        v.pos.x = p.x; v.pos.y = p.y; v.pos.foot = p.f;
        v.stem.scale.set(1, headZ - p.f, 1);
        v.stem.position.set(p.x, p.f + (headZ - p.f) / 2, p.y);
        v.blob.position.set(p.x, p.f + 0.012, p.y);
        v.solid.position.set(p.x, headZ, p.y); v.ghost.position.copy(v.solid.position);
        v.ring.position.set(p.x, p.f + 0.02, p.y);
        v.arrow.position.set(p.x, p.f, p.y);
        v.arrow.visible = st.heading !== null;
        if (st.heading !== null) {
          v.arrow.rotation.y = st.heading;
          // Sit just ahead of the stem so it points out from the marker.
          v.arrow.position.x += Math.sin(st.heading) * 0.42;
          v.arrow.position.z += Math.cos(st.heading) * 0.42;
        }
        if (v.route) {
          // First vertex follows the gliding marker. (Fat lines keep
          // segment endpoints in an interleaved buffer; the first
          // segment's start is its first three floats.)
          const buf = v.route.l2 ? v.route.geo.attributes.instanceStart.data : null;
          const a = buf ? buf.array : v.route.attr.array;
          a[0] = p.x; a[1] = p.f + 0.06; a[2] = p.y;
          if (buf) buf.needsUpdate = true; else { v.route.attr.needsUpdate = true; if (v.route.dashed) v.route.line.computeLineDistances(); }
        }
      });
    }

    function teamAnimating(now) {
      for (const st of teamStates.values()) if (st.dur > 0 && now < st.t0 + st.dur) return true;
      return false;
    }

    // ---------- hit targets ----------
    function buildHits() {
      const hits = safezoneHits.slice();
      incidentHits.forEach((h) => {
        hits.push(Object.assign({}, h, { rpx: Math.max(h.wr * MARKER_BOOST * pxPerWorldAt(h.wx, h.wy, h.wz), 9) + 2 }));
      });
      teamViews.forEach((v) => {
        if (!v.data) return;
        const headZ = v.pos.foot + MR.ISO_TEAM_Z;
        hits.push({ type: "team", id: v.data.id, wx: v.pos.x, wy: v.pos.y, wz: headZ, wr: MR.MARKER_WORLD_R + 0.18,
                    rpx: (MR.MARKER_WORLD_R + 0.18) * MARKER_BOOST * pxPerWorldAt(v.pos.x, v.pos.y, headZ), data: v.data });
      });
      return hits;
    }

    // ---------- the frame ----------
    let rafId = 0, lastDraw = 0, curModel = null, curOpts = null;
    const api = { onHits: null };

    // ---------- camera director: follow a team / fly to a spot ----------
    // Both are smooth and both end the moment the user drags the camera
    // (pan / orbit / top-down / reset all call cancelCamera) or presses
    // Esc (the page calls cancelCamera). Wheel zoom does not cancel.
    let director = null; // { mode: "follow", teamId, goalDist } | { mode: "fly", t0, dur, from, to }
    const easeInOut = (k) => (k < 0.5 ? 2 * k * k : 1 - Math.pow(-2 * k + 2, 2) / 2);
    function cameraModeInfo() { return director ? { mode: director.mode, teamId: director.teamId } : null; }
    function notifyCamera() { if (api.onCamera) api.onCamera(cameraModeInfo()); }
    function cancelCamera() {
      if (!director) return false;
      director = null; notifyCamera();
      return true;
    }
    function followTeam(id) {
      const v = teamViews.get(id);
      if (!v) return false;
      director = { mode: "follow", teamId: id, goalDist: Math.min(view.dist, 30) };
      if (settings.reduceMotion) { view.tx = v.pos.x; view.tz = v.pos.y; view.dist = director.goalDist; updateCamera(); }
      notifyCamera(); kick();
      return true;
    }
    // Fly to (x, y): swings the camera ~50 degrees around the spot while
    // moving in, ending closer and tilted down onto it.
    function flyTo(x, y, opts) {
      opts = opts || {};
      const to = { tx: x, tz: y, dist: opts.dist || Math.max(10, Math.min(view.dist, 20)), el: Math.max(EL_MIN, Math.min(EL_MAX, opts.el || 0.75)), az: view.az + (opts.swing === undefined ? 0.9 : opts.swing) };
      if (settings.reduceMotion) { Object.assign(view, to); updateCamera(); director = null; notifyCamera(); return true; }
      director = { mode: "fly", t0: performance.now(), dur: opts.dur || 1400, from: { tx: view.tx, tz: view.tz, dist: view.dist, el: view.el, az: view.az }, to };
      notifyCamera(); kick();
      return true;
    }
    function stepDirector(now, dt) {
      if (!director) return;
      if (director.mode === "follow") {
        const v = teamViews.get(director.teamId);
        if (!v) { cancelCamera(); return; }
        const k = 1 - Math.exp(-dt * 9);
        view.tx += (v.pos.x - view.tx) * k; view.tz += (v.pos.y - view.tz) * k;
        if (view.dist > director.goalDist) view.dist += (director.goalDist - view.dist) * (1 - Math.exp(-dt * 3));
        updateCamera();
      } else {
        const k = Math.max(0, Math.min(1, (now - director.t0) / director.dur)), e = easeInOut(k), f = director.from, t = director.to;
        view.tx = f.tx + (t.tx - f.tx) * e; view.tz = f.tz + (t.tz - f.tz) * e;
        view.dist = f.dist + (t.dist - f.dist) * e; view.el = f.el + (t.el - f.el) * e; view.az = f.az + (t.az - f.az) * e;
        updateCamera();
        if (k >= 1) { director = null; notifyCamera(); }
      }
    }

    // Camera shake (building collapse): a decaying jitter on top of the user's camera.
    let shake = null; // { t0, dur, amp }
    function applyShake(now) {
      if (!shake) return false;
      const k = (now - shake.t0) / shake.dur;
      if (k >= 1) { shake = null; updateCamera(); return false; }
      updateCamera();
      const a = shake.amp * (1 - k) * (1 - k);
      camera.position.x += Math.sin(now * 0.09) * a; camera.position.y += Math.sin(now * 0.13 + 1) * a * 0.7; camera.position.z += Math.cos(now * 0.11) * a;
      camera.updateMatrixWorld(true);
      return true;
    }

    // Pin size on screen. Up close a pin keeps its compact world size; zoomed
    // far out it would shrink to a speck, so its head is held at a minimum
    // pixel size (MIN_HEAD_PX); a hovered / selected pin is also enlarged a
    // little so its details can be read. The "P1".."P4" text is only drawn
    // while the head is big enough to read it (LABEL_MIN_PX, with hysteresis);
    // more detail is added with zoom (see the levels above). Colour, halo and
    // the label stay at every zoom.
    const ESC_SECS = 1.6;
    let tetherCount = 0;
    const MIN_HEAD_PX = 7, LABEL_MIN_PX = 9, LABEL_HYST = 1.5;
    function updatePinLabels() {
      for (const p of pinSprites) {
        const natural = pxPerWorldAt(p.wx, p.wy, p.headZ) * PIN_R_WORLD;       // head radius in px at its world size
        const focus = p.info.ids.includes(hoverIncident) || p.spec.selected;
        const k = Math.max(1, Math.max(MIN_HEAD_PX, focus ? HOVER_HEAD_PX : 0) / Math.max(natural, 0.01));
        const sc = p.info.scale * k;
        p.s.scale.set(sc, sc, 1); p.gh.scale.set(sc, sc, 1);
        const shownPx = natural * k;
        const label = p.info.label ? natural >= LABEL_MIN_PX - LABEL_HYST : natural >= LABEL_MIN_PX + LABEL_HYST;
        const lod = focus || shownPx >= LOD_CLOSE_PX ? 2 : shownPx >= LOD_MED_PX ? 1 : 0;
        const idx = lod * 2 + ((label || (focus && natural >= 5)) ? 1 : 0);
        if (idx !== p.cur) {
          const g = p.variant(idx);
          p.s.material = g.solid; p.gh.material = g.ghost; p.cur = idx;
          p.info.label = (idx & 1) === 1; p.info.lod = lod;
        }
        p.info.headPx = Math.max(natural, MIN_HEAD_PX);
      }
      updateTether();
    }

    // Hover / select an incident: a dashed line from its pin to each assigned team.
    const tetherLines = [];
    function updateTether() {
      let n = 0;
      const now = performance.now();
      const pin = pinSprites.find((p) => p.info.ids.includes(hoverIncident) || p.spec.selected);
      if (pin) {
        for (const a of pin.info.assigned) {
          const st = teamStates.get(a.team), v = teamViews.get(a.team);
          if (!st || !v) continue;
          const tp = poseOf(st, now);
          let ln = tetherLines[n];
          if (!ln) {
            const geo = new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3()]);
            ln = new THREE.Line(geo, new THREE.LineDashedMaterial({ transparent: true, opacity: 0.95, dashSize: 0.22, gapSize: 0.14, depthTest: false, depthWrite: false }));
            ln.renderOrder = 7; ln.frustumCulled = false; scene.add(ln); tetherLines.push(ln);
          }
          ln.material.color.set(a.color);
          const pa = ln.geometry.attributes.position;
          pa.setXYZ(0, pin.wx, pin.headZ, pin.wy); pa.setXYZ(1, tp.x, tp.f + MR.ISO_TEAM_Z * 0.6, tp.y); pa.needsUpdate = true;
          ln.computeLineDistances(); ln.visible = true; n++;
        }
      }
      for (let i = n; i < tetherLines.length; i++) tetherLines[i].visible = false;
      tetherCount = n;
    }

    // Escalation pulse: an expanding, fading ring around the pin for ESC_SECS.
    let escPts = null;
    function buildEscalations() {
      if (escPts) { dynGroup.remove(escPts); escPts.geometry.dispose(); escPts.material.dispose(); escPts = null; }
      escList = settings.reduceMotion ? [] : escPulses.slice();
      if (!escList.length) return;
      const pos = new Float32Array(escList.length * 3), seed = new Float32Array(escList.length * 4);
      escList.forEach((e, i) => { pos.set([e.x, e.y, e.z], i * 3); const c = new THREE.Color(e.hex); seed.set([c.r, c.g, c.b, e.start], i * 4); });
      const g = new THREE.BufferGeometry();
      g.setAttribute("position", new THREE.BufferAttribute(pos, 3)); g.setAttribute("aSeed", new THREE.BufferAttribute(seed, 4));
      const m = new THREE.ShaderMaterial({
        uniforms: { uTime: pulseU.uTime, uScale: { value: 600 }, uSecs: { value: ESC_SECS } },
        vertexShader: `attribute vec4 aSeed; uniform float uTime, uScale, uSecs; varying vec4 vC; varying float vT;
          void main() { float t = clamp((uTime - aSeed.w) / uSecs, 0.0, 1.0); vT = t;
            vec4 mv = modelViewMatrix * vec4(position, 1.0); gl_Position = projectionMatrix * mv;
            gl_PointSize = clamp((0.7 + 2.6 * t) * uScale / max(0.1, -mv.z), 24.0, 260.0); vC = vec4(aSeed.xyz, 1.0); }`,
        fragmentShader: `varying vec4 vC; varying float vT; void main() { float d = length(gl_PointCoord - 0.5) * 2.0;
          float r = 0.25 + 0.75 * vT; float ring = smoothstep(0.12, 0.0, abs(d - r)); gl_FragColor = vec4(mix(vC.rgb, vec3(1.0), 0.35), ring * (1.0 - vT) * (vT >= 1.0 ? 0.0 : 1.0)); }`,
        transparent: true, depthWrite: false, depthTest: false, blending: THREE.AdditiveBlending,
      });
      escPts = new THREE.Points(g, m); escPts.frustumCulled = false; escPts.renderOrder = 8;
      escPts.onBeforeRender = (r) => { const sz = new THREE.Vector2(); r.getDrawingBufferSize(sz); m.uniforms.uScale.value = sz.y / (2 * Math.tan(FOV * Math.PI / 360)); };
      dynGroup.add(escPts);
    }
    function escActive(now) { return !settings.reduceMotion && escList.some((e) => now / 1000 - e.start < ESC_SECS); }

    function drawNow(now, notify) {
      const dt = Math.max(0, Math.min(0.1, (now - lastDraw) / 1000));
      lastDraw = now;
      poseTeams(now);
      stepDirector(now, dt);
      applyShake(now);
      pulseU.uTime.value = now / 1000;
      updatePinLabels();
      if (fx) {
        renderer.getDrawingBufferSize(_v2);
        fx.update(now / 1000, _v2.y / (2 * Math.tan(FOV * Math.PI / 360)));
      }
      renderer.render(scene, camera);
      const hits = buildHits();
      if (notify && api.onHits) api.onHits(hits);
      return hits;
    }

    function ambientOnly(now) { return !teamAnimating(now) && !shake && !director; }
    function needsMotion(now) {
      return teamAnimating(now) || !!shake || !!director || (pulseCount > 0 && !settings.reduceMotion) || escActive(now) || (!!fx && fx.animated(now / 1000));
    }

    // Auto quality: if frames keep taking long at "high"/"low" effects,
    // step the effects down rather than make the whole dashboard sluggish.
    let slowFrames = 0, prevLoopAt = 0;
    function watchFrameRate(now) {
      const dt = prevLoopAt ? now - prevLoopAt : 0;
      prevLoopAt = now;
      if (!settings.autoQuality || lite || dt <= 0 || dt > 250) { slowFrames = 0; return; }
      slowFrames = dt > 26 ? slowFrames + 1 : Math.max(0, slowFrames - 2);
      if (slowFrames > 90) {
        slowFrames = 0;
        if (fx && fx.level !== "off") {
          const next = fx.level === "high" ? "low" : "off";
          fx.setLevel(next); autoReduced = next;
          if (api.onSettings) api.onSettings(api.settingsState());
        } else enterLite();
      }
    }
    // Last resort when frames are still slow with effects off: drop shadows
    // and render at 1x pixel density. Undone by choosing an effects level.
    let lite = false;
    function enterLite() {
      if (lite) return;
      lite = true; autoReduced = "lite";
      renderer.shadowMap.enabled = false;
      renderer.setPixelRatio(1); renderer.setSize(view.w, view.h, false);
      scene.traverse((o) => { if (o.material) o.material.needsUpdate = true; });
      if (api.onSettings) api.onSettings(api.settingsState());
    }
    function leaveLite() {
      if (!lite) return;
      lite = false;
      renderer.shadowMap.enabled = true;
      renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2)); renderer.setSize(view.w, view.h, false);
      scene.traverse((o) => { if (o.material) o.material.needsUpdate = true; });
    }

    function loop(now) {
      rafId = 0;
      if (!active || lost) return;
      // Ambient effects alone (water shimmer, flames) run at ~30 fps;
      // team glides, camera moves and shakes run at the display rate.
      const skip = ambientOnly(now) && now - lastDraw < 30;
      if (!skip) { watchFrameRate(now); drawNow(now, true); }
      if (needsMotion(now)) rafId = requestAnimationFrame(loop); else prevLoopAt = 0;
    }
    let active = true;
    function kick() { if (active && !rafId) rafId = requestAnimationFrame(loop); }

    // o: { env, terrainMap, frame, fogViewMode, selectedMarker, teamColors, outlineBuildingId, snap }
    // Synchronous: updates the scene and draws once, returning the hit
    // targets (same shape as the 2D/iso draw functions'). While teams are
    // still gliding, an animation loop keeps drawing and reports fresh
    // hit targets through api.onHits.
    function render(o) {
      if (lost) return [];
      const now = performance.now();
      const model = MR.buildSceneModel(o);
      curModel = model; curOpts = o;

      let terrainChanged = false;
      if (o.terrainMap && o.terrainMap !== terrainSource) {
        terrainChanged = true;
        const first = terrainSource === null;
        terrainSource = o.terrainMap;
        terrainParsed = MR.parseTerrain(o.terrainMap);
        buildGround(MR.renderTerrainTexture(o.terrainMap, GROUND_TEX_PX_PER_CELL));
        if (first) projector.reset();
      }
      if (model.prep.source !== envSource) {
        envSource = model.prep.source;
        if (bld) bld.build(model.prep.list); else buildBuildings(model.prep);
        detectCollapses(model.prep, o.frame ? o.frame.tick : null);
      }
      // Markers stand on the building as 3D draws it (collapsed ones are lower than in 2.5D).
      if (bld) {
        model.teams.forEach((mt) => { mt.foot = bld.footing(model.prep, mt.t.x, mt.t.y); mt.headZ = mt.foot + MR.ISO_TEAM_Z; });
        model.incidents.forEach((mi) => { mi.foot = bld.footing(model.prep, mi.wx, mi.wy); });
      }
      // Incident pins sit on a short stem (PIN_Z), not the taller 2.5D marker.
      model.incidents.forEach((mi) => { mi.headZ = mi.foot + PIN_Z; });

      const tick = o.frame ? o.frame.tick : null;
      const newTick = tick !== lastTick;
      if (newTick) advanceTeams(model, tick, now, !!o.snap);
      syncTeamViews(model);

      if (fx && (newTick || terrainChanged || !fxSynced)) {
        fx.sync({ terrain: terrainParsed, hazardCells: model.hazardCells, incidents: model.incidents, prep: model.prep, tick: tick || 0,
          footing: bld ? (x, y) => bld.footing(model.prep, x, y) : null });
        fxSynced = true;
      }

      const sig = [tick, o.selectedMarker ? o.selectedMarker.type + ":" + o.selectedMarker.id : "", o.fogViewMode].join("|");
      if (newTick || sig !== lastIncidentSig) { rebuildTickScene(model, o); lastIncidentSig = sig; }
      if (newTick) rebuildRoutes(model, tick);

      outline.visible = false;
      if (o.outlineBuildingId !== undefined && o.outlineBuildingId !== null) {
        const e = model.prep.byId.get(o.outlineBuildingId);
        if (e) {
          const h = bld ? Math.max(0.12, bld.heightOf(e)) : e.h;
          outline.visible = true;
          outline.position.set((e.x0 + e.x1) / 2, h / 2, (e.y0 + e.y1) / 2);
          outline.scale.set(e.x1 - e.x0, h, e.y1 - e.y0);
        }
      }

      const hits = drawNow(now, false);
      kick();
      return hits;
    }

    function setSize(cssW, cssH, dpr) {
      view.w = Math.max(1, cssW); view.h = Math.max(1, cssH);
      renderer.setPixelRatio(Math.min(dpr || 1, 2));
      renderer.setSize(view.w, view.h, false);
      l2mats.forEach((m) => m.resolution.set(view.w, view.h));
      const first = !view._sized;
      view._sized = true;
      if (first) projector.reset(); else { view.dist = Math.min(view.dist, 170); updateCamera(); }
    }

    function settingsState() {
      return Object.assign({}, settings, { effectsApplied: fx ? fx.level : "off", autoReduced, fxAvailable: !!fx });
    }
    function applySettings(partial) {
      Object.assign(settings, partial);
      saveSettings({ effects: settings.effects, reduceMotion: settings.reduceMotion, lighting: settings.lighting, autoQuality: settings.autoQuality });
      if (fx) {
        if (partial.effects !== undefined) { autoReduced = null; slowFrames = 0; leaveLite(); fx.setLevel(settings.effects); }
        if (partial.reduceMotion !== undefined) fx.setMotion(!settings.reduceMotion);
      }
      if (partial.reduceMotion !== undefined) pulseU.uMotion.value = settings.reduceMotion ? 0 : 1;
      if (partial.lighting !== undefined) applyLighting();
      if (shake && settings.reduceMotion) { shake = null; updateCamera(); }
      kick();
      drawNow(performance.now(), false);
      if (api.onSettings) api.onSettings(settingsState());
    }
    // Building-collapse detection. The simulator re-sends the building list when
    // an aftershock destroys buildings ("An aftershock destroyed at least one
    // building this tick" in laptop/main.c), so a new building list in which
    // buildings have newly reached ENV_DAMAGE_DESTROYED is the dashboard's
    // building-collapse event (a gas explosion that destroys a building looks the
    // same in the data, so it is called a collapse, not an aftershock;
    // the simulator doesn't say which). Only while playing along - a seek
    // or scrub across many ticks isn't an "event".
    let prevDamage = null;
    function detectCollapses(prep, tick) {
      const cur = new Map(prep.list.map((e) => [e.id, e.b.damage || 0]));
      if (prevDamage && tick !== null && lastTick !== null && Math.abs(tick - lastTick) <= 2) {
        const cells = [];
        for (const e of prep.list) {
          const was = prevDamage.get(e.id);
          if (was !== undefined && was < 100 && (e.b.damage || 0) >= 100) cells.push({ x: e.b.x, y: e.b.y });
        }
        if (cells.length) {
          const step = Math.max(1, Math.ceil(cells.length / 30));
          api.collapseEvents = (api.collapseEvents || 0) + 1;
          api.lastCollapse = { tick, buildings: cells.length };
          triggerCollapse(cells.filter((_, i) => i % step === 0));
        }
      }
      prevDamage = cur;
    }

    // A building collapse: brief camera shake plus a dust burst over the collapsed
    // cells ([{x, y}]). Both are skipped with "reduce motion". Called by
    // detectCollapses() when a new building list shows buildings destroyed.
    function triggerCollapse(cells) {
      if (settings.reduceMotion) return false;
      const now = performance.now();
      if (fx) fx.burst(cells, now / 1000);
      shake = { t0: now, dur: 650, amp: 0.35 };
      kick();
      return true;
    }

    projector.reset();
    Object.assign(api, {
      setSettings: applySettings, settingsState, triggerCollapse,
      follow: followTeam, flyTo, cancelCamera,
      get fx() { return fx; },
      buildingStats() { return bld ? bld.stats : null; },
      // What each incident pin looks like (ring kind, badges, height), for tests/tooling.
      setHoverIncident(id) { const v = id === undefined ? null : id; if (v !== hoverIncident) { hoverIncident = v; updatePinLabels(); kick(); } },
      escalationState() { const n = performance.now() / 1000; return { active: escList.filter((e) => n - e.start < ESC_SECS).length, total: escList.length, tethers: tetherCount }; },
      incidentPins() { return pinInfo.map((p) => Object.assign({}, p)); },
      pulseState() { return { pulsing: pulseCount, animating: pulseCount > 0 && !settings.reduceMotion, uTime: pulseU.uTime.value, uMotion: pulseU.uMotion.value }; },
      // Current lighting, for tests/tooling.
      lightingState() {
        return { preset: settings.lighting, background: scene.background.getHex(), fog: scene.fog.color.getHex(), fogNear: scene.fog.near, fogFar: scene.fog.far,
                 dist: view.dist, hemi: hemi.intensity, sun: sun.intensity, windowGlow: bld ? bld.glow() : 0, markersFogged: markerMat(0xffffff).stem.fog };
      },
      // Height the 3D view gives a building (where markers stand on it), for tests/tooling.
      buildingHeight(id) { const e = curModel && curModel.prep.byId.get(id); return e && bld ? bld.heightOf(e) : null; },
      projector, render, setSize,
      setActive(on) { active = !!on; if (!active && rafId) { cancelAnimationFrame(rafId); rafId = 0; } else kick(); },
      // Read-only snapshot of one team's drawn state (for tests and tooling).
      teamInfo(id) {
        const v = teamViews.get(id), st = teamStates.get(id);
        if (!v || !st) return null;
        const p = poseOf(st, performance.now());
        return { x: p.x, y: p.y, k: p.k, from: [st.px, st.py], to: [st.cx, st.cy], heading: st.heading,
                 route: v.route ? v.route.cells.map((c) => c.slice()) : null, dashed: v.route ? v.route.dashed : null, faulted: !!v.faulted };
      },
      get state() { return view; },
      // Internals for profiling only: exposed just when the page URL has ?debug=1.
      ...(new URLSearchParams(window.location.search).get("debug") === "1"
        ? { _renderer: renderer, _scene: scene, _camera: camera, _teamViews: teamViews, _teamStates: teamStates } : {}),
    });
    // Live getters. (They can't sit inside Object.assign above: assign would
    // read each getter once and copy the value, freezing it at creation.)
    Object.defineProperties(api, {
      cameraMode: { get: cameraModeInfo },
      settings: { get: () => Object.assign({}, settings) },
      isLost: { get: () => lost },
    });
    return api;
  }

  window.MapView3D = { available: true, create };
  announce(true);
})();

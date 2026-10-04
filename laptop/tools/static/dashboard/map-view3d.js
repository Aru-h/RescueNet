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

  const MR = window.MapRender;
  const GLYPH_PX = 128;          // marker glyph canvas size
  const GLYPH_TEAM_R = 26;       // team glyph radius on that canvas (px)
  const GLYPH_INC_SIZE = 48;     // incident glyph square size on that canvas (px)
  const GROUND_TEX_PX_PER_CELL = 48;
  const FOV = 35;
  // Perspective makes the 3D map's markers read smaller than in 2D/2.5D at the same world size; scale sprites and hit areas up a little.
  const MARKER_BOOST = 1.4;

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

    // ---------- lights ----------
    scene.add(new THREE.HemisphereLight(0xc9d8ee, 0x1b2430, 1.15));
    const sun = new THREE.DirectionalLight(0xfff3df, 2.3);
    sun.castShadow = true;
    sun.shadow.mapSize.set(2048, 2048);
    const sc = sun.shadow.camera;
    sc.left = -32; sc.right = 32; sc.top = 32; sc.bottom = -32; sc.near = 1; sc.far = 140;
    sun.shadow.radius = 4;      // blur the shadow edges (soft shadows)
    sun.shadow.bias = -0.0004;
    sun.shadow.normalBias = 0.03;
    scene.add(sun);
    scene.add(sun.target);

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
      sun.position.set(view.tx - 22, 38, view.tz - 14);
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
        view.az -= dx * 0.008;
        view.el = Math.max(EL_MIN, Math.min(EL_MAX, view.el + dy * 0.006));
        updateCamera();
      },
      // Straight down at the whole map, keeping the current rotation.
      topDown() {
        view.el = EL_MAX;
        view.tx = view.mapW / 2 - 0.5; view.tz = view.mapH / 2 - 0.5;
        view.dist = fitDistance();
        updateCamera();
      },
      reset() {
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
    let terrainSource = null, envSource = null;
    let buildingMesh = null, buildingEntries = [];
    const outline = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1)),
      new THREE.LineBasicMaterial({ color: 0xffffff, depthTest: false, transparent: true, opacity: 0.9 }));
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
      buildingEntries = prep.list;
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

    // ---------- per-frame scene (markers, overlays) ----------
    const dynGroup = new THREE.Group();
    scene.add(dynGroup);
    let dynDisposables = [];

    const stemGeo = new THREE.CylinderGeometry(0.03, 0.03, 1, 6);
    const discGeo = new THREE.CircleGeometry(1, 20);
    const ringGeo = new THREE.RingGeometry(0.52, 0.62, 28);
    const planeGeo = new THREE.PlaneGeometry(1, 1);
    const shadowMat = new THREE.MeshBasicMaterial({ color: 0x000000, transparent: true, opacity: 0.4, depthWrite: false });
    const selMat = new THREE.MeshBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.8, depthWrite: false, side: THREE.DoubleSide });
    const matCache = new Map();
    function colorMat(hex, opacity) {
      const k = hex + "|" + opacity;
      let m = matCache.get(k);
      if (!m) { m = new THREE.MeshBasicMaterial({ color: new THREE.Color(hex), transparent: true, opacity, depthWrite: false, side: THREE.DoubleSide }); matCache.set(k, m); }
      return m;
    }
    // Per-frame batches: hazard cells, marker stems and ground shadows
    // are drawn as InstancedMeshes (one draw call each) instead of one
    // mesh per cell/marker - a mid-run frame has hundreds of hazard
    // cells, and draw calls, not triangles, are what costs on the CPU.
    function makePool(geo, mat, capacity, colored) {
      const mesh = new THREE.InstancedMesh(geo, mat, capacity);
      mesh.count = 0; mesh.frustumCulled = false;
      if (colored) mesh.setColorAt(0, new THREE.Color(1, 1, 1));
      scene.add(mesh);
      const m4 = new THREE.Matrix4(), q = new THREE.Quaternion(), p = new THREE.Vector3(), sc = new THREE.Vector3(), col = new THREE.Color();
      return {
        mesh,
        reset() { mesh.count = 0; },
        add(px, py, pz, sx, sy, sz, rotX, hex) {
          if (mesh.count >= capacity) return;
          q.setFromAxisAngle(AXIS_X, rotX || 0);
          m4.compose(p.set(px, py, pz), q, sc.set(sx, sy, sz));
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
    const AXIS_X = new THREE.Vector3(1, 0, 0);
    const FLAT = -Math.PI / 2; // lay a plane/disc flat on the ground
    const hazardPools = [0.38, 0.24, 0.12].map((o) => makePool(planeGeo, colorMat(0xd29922, o), 2048, false));
    const stemPool = makePool(stemGeo, new THREE.MeshStandardMaterial({ roughness: 0.6 }), 512, true);
    stemPool.mesh.castShadow = true;
    const blobPool = makePool(discGeo, shadowMat, 512, false);

    // Marker glyph sprites: the 2D glyph drawn on a small canvas, cached
    // by the fields that change how it looks. Each gets a normal sprite
    // and a "ghost" that only draws where something is IN FRONT of it
    // (depthFunc Greater) - so a marker behind a building is never lost.
    const glyphCache = new Map();
    function glyphSprites(key, drawFn) {
      let g = glyphCache.get(key);
      if (!g) {
        const cv = document.createElement("canvas");
        cv.width = cv.height = GLYPH_PX;
        drawFn(cv.getContext("2d"));
        const tex = new THREE.CanvasTexture(cv);
        tex.colorSpace = THREE.SRGBColorSpace;
        tex.anisotropy = 4;
        g = {
          solid: new THREE.SpriteMaterial({ map: tex, transparent: true, depthWrite: true }),
          ghost: new THREE.SpriteMaterial({ map: tex, transparent: true, opacity: 0.38, depthTest: true, depthWrite: false, depthFunc: THREE.GreaterDepth }),
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

    function addSprite(mats, wx, headZ, wy, worldScale) {
      const s = new THREE.Sprite(mats.solid);
      s.position.set(wx, headZ, wy); s.scale.set(worldScale, worldScale, 1); s.renderOrder = 5;
      const gh = new THREE.Sprite(mats.ghost);
      gh.position.copy(s.position); gh.scale.copy(s.scale); gh.renderOrder = 6;
      dynGroup.add(s, gh);
    }

    function addMarkerBase(wx, wy, foot, headZ, hex, shadowR) {
      stemPool.add(wx, foot + (headZ - foot) / 2, wy, 1, headZ - foot, 1, 0, new THREE.Color(hex).getHex());
      blobPool.add(wx, foot + 0.012, wy, shadowR, shadowR, 1, FLAT);
    }

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

    // Pixels per world unit at a point (for hit radii: a marker's hit
    // area is the disc you see, as in 2.5D).
    function pxPerWorldAt(wx, wy, z) {
      const a = worldToScreen(wx, wy, z), b = worldToScreen(wx + 1, wy, z), c = worldToScreen(wx, wy + 1, z);
      return (Math.hypot(b.x - a.x, b.y - a.y) + Math.hypot(c.x - a.x, c.y - a.y)) / 2;
    }


    // o: { env, terrainMap, frame, fogViewMode, selectedMarker, teamColors, outlineBuildingId }
    // Returns hit targets (same shape as the 2D/iso draw functions').
    function render(o) {
      const hits = [];
      if (lost) return hits;
      const model = MR.buildSceneModel(o);

      if (o.terrainMap && o.terrainMap !== terrainSource) {
        // First terrain frames the camera; a later STATICMAP update (e.g.
        // flood spreading) only swaps the texture and leaves the user's view alone.
        const first = terrainSource === null;
        terrainSource = o.terrainMap;
        buildGround(MR.renderTerrainTexture(o.terrainMap, GROUND_TEX_PX_PER_CELL));
        if (first) projector.reset();
      }
      if (model.prep.source !== envSource) { envSource = model.prep.source; buildBuildings(model.prep); }

      // Dispose last frame's per-frame objects.
      dynGroup.clear();
      hazardPools.forEach((p) => p.reset()); stemPool.reset(); blobPool.reset();
      dynDisposables.forEach((d) => d.dispose()); dynDisposables = [];

      // Open incident card's building: outline it.
      outline.visible = false;
      if (o.outlineBuildingId !== undefined && o.outlineBuildingId !== null) {
        const e = model.prep.byId.get(o.outlineBuildingId);
        if (e) {
          outline.visible = true;
          outline.position.set((e.x0 + e.x1) / 2, e.h / 2, (e.y0 + e.y1) / 2);
          outline.scale.set(e.x1 - e.x0, e.h, e.y1 - e.y0);
        }
      }

      // Epicenter, hazard cells, safe zones: flat on the ground.
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
        hits.push({ type: "safezone", id: sz.id, wx: sz.x, wy: sz.y, wr: 0.5, data: sz });
      });

      // Incidents.
      model.incidents.forEach((mi) => {
        const { members, inc, wx, wy, worldSize, foot, headZ, color, isSel } = mi;
        const selArg = isSel ? { type: "incident", id: inc.id } : null;
        const key = ["I", inc.id, inc.status, inc.type, inc.priority, inc.victims, members.map((m) => m.id + ":" + m.status).join(","), isSel ? 1 : 0].join("|");
        const mats = glyphSprites(key, (ctx) => MR.drawIncidentGlyph(ctx, { x: GLYPH_PX / 2, y: GLYPH_PX / 2 }, GLYPH_INC_SIZE, color, inc, members, selArg || { type: "none", id: -1 }));
        addMarkerBase(wx, wy, foot, headZ, color, Math.max(0.3, worldSize * 0.6));
        addSprite(mats, wx, headZ, wy, (GLYPH_PX / GLYPH_INC_SIZE) * worldSize * MARKER_BOOST);
        if (isSel) addGroundRing(wx, wy, foot, Math.max(worldSize / 2, 0.4) + 0.25, selMat);
        const wr = Math.max(worldSize / 2, 0.4);
        hits.push({ type: "incident", id: inc.id, wx, wy, wz: headZ, wr,
                    rpx: Math.max(wr * MARKER_BOOST * pxPerWorldAt(wx, wy, headZ), 6) + 2,
                    data: members.length > 1 ? Object.assign({}, inc, { groupIncidents: members }) : inc });
      });

      // Teams.
      model.teams.forEach((mt) => {
        const { t, i, wx, wy, foot, headZ, color, isSel } = mt;
        const phase = window._overridePhase && window._overridePhase[t.id];
        const key = ["T", t.id, t.status, t.fault_mask || 0, t.override ? 1 : 0, phase ? phase.kind : "", isSel ? 1 : 0, color].join("|");
        const mats = glyphSprites(key, (ctx) => MR.drawTeamGlyph(ctx, { x: GLYPH_PX / 2, y: GLYPH_PX / 2 }, GLYPH_TEAM_R, color, t, isSel ? { type: "team", id: t.id } : null));
        if (t.has_dest && (t.dest_x !== t.x || t.dest_y !== t.y)) addDashedLine([[wx, 0.04, wy], [t.dest_x, 0.04, t.dest_y]], color, 0.5);
        addMarkerBase(wx, wy, foot, headZ, color, 0.34);
        addSprite(mats, wx, headZ, wy, (GLYPH_PX / GLYPH_TEAM_R) * MR.MARKER_WORLD_R * MARKER_BOOST);
        if (isSel) addGroundRing(wx, wy, foot, 1, selMat);
        hits.push({ type: "team", id: t.id, wx, wy, wz: headZ, wr: MR.MARKER_WORLD_R + 0.18,
                    rpx: (MR.MARKER_WORLD_R + 0.18) * MARKER_BOOST * pxPerWorldAt(wx, wy, headZ), data: { ...t, i } });
      });

      hazardPools.forEach((p) => p.commit()); stemPool.commit(); blobPool.commit();
      renderer.render(scene, camera);
      return hits;
    }

    function setSize(cssW, cssH, dpr) {
      view.w = Math.max(1, cssW); view.h = Math.max(1, cssH);
      renderer.setPixelRatio(Math.min(dpr || 1, 2));
      renderer.setSize(view.w, view.h, false);
      const first = !view._sized;
      view._sized = true;
      if (first) projector.reset(); else { view.dist = Math.min(view.dist, 170); updateCamera(); }
    }

    projector.reset();
    return {
      projector, render, setSize,
      get state() { return view; },
      get isLost() { return lost; },
      // Internals for profiling only: exposed just when the page URL has ?debug=1.
      ...(new URLSearchParams(window.location.search).get("debug") === "1"
        ? { _renderer: renderer, _scene: scene, _camera: camera } : {}),
    };
  }

  window.MapView3D = { available: true, create };
  announce(true);
})();

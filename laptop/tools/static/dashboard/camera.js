/*
 * laptop/tools/static/dashboard/camera.js
 *
 * A small 2D camera for the Live Ops region map: world <-> screen
 * coordinate conversion, pan, zoom-to-cursor, and a DPI-aware canvas
 * resize helper.
 *
 * Rotation is fixed at 0 in this revision (the map spec calls it out
 * explicitly as "not built now"), but every draw call and every
 * hit-test goes through worldToScreen()/screenToWorld() below, and
 * both already have the one spot a rotation term would go (see the
 * comments inline). Adding rotation later is therefore a change to
 * these two functions, not a rewrite of the map drawing code that
 * calls them.
 *
 * View modes: "2d" is the original top-down projection. "iso" is a 2:1
 * isometric projection (a world cell is a diamond twice as wide as it
 * is tall) with an optional height argument: worldToScreen(wx, wy, z)
 * lifts a point z world-units straight up the screen, and
 * screenToWorld(sx, sy, z) is its exact inverse on the horizontal
 * plane at height z (z = 0 is the ground). In "2d" the z argument is
 * ignored, so callers can pass it unconditionally. Pan, zoom-to-cursor
 * and hit-testing never look at the mode: they only call the two
 * conversion functions, so they behave identically in both.
 *
 * World space: grid units, 0..worldW / 0..worldH (36x36 for the
 * region map, but this file doesn't hardcode that). Screen space: CSS
 * pixels within the canvas's on-page box - NOT device pixels. The
 * canvas context is pre-scaled by devicePixelRatio in
 * resizeCanvasForDPI() below so drawing code never has to think about
 * DPR; it just draws in the same CSS-pixel space mouse events arrive
 * in (via getBoundingClientRect()).
 */
(function (global) {
  "use strict";

  function createCamera(worldW, worldH) {
    const state = {
      worldW: worldW,
      worldH: worldH,
      x: worldW / 2,   // world-space point the camera looks at (center of view)
      y: worldH / 2,
      zoom: 1,         // screen pixels per world unit
      rotation: 0,     // radians; always 0 today - see file header
      viewMode: "2d",  // "2d" (top-down, default) | "iso" (2:1 isometric 2.5D) - see setViewMode()
      viewW: 0,
      viewH: 0,        // last-known CSS pixel size of the canvas box
      minZoom: 0.1,
      maxZoom: 400,
    };

    function fitZoom(viewW, viewH) {
      // Fill the available box with a small margin so edge markers
      // (teams/incidents near the grid boundary) aren't clipped.
      const margin = 0.92;
      const z = Math.min(viewW / state.worldW, viewH / state.worldH) * margin;
      return z > 0 ? z : 1;
    }

    function reset(viewW, viewH) {
      if (viewW !== undefined) state.viewW = viewW;
      if (viewH !== undefined) state.viewH = viewH;
      const fit = fitZoom(state.viewW, state.viewH);
      state.zoom = fit;
      state.minZoom = fit * 0.4;
      state.maxZoom = fit * 12;
      state.x = state.worldW / 2;
      state.y = state.worldH / 2;
    }

    // Called on every resize. Keeps the current pan/zoom on a simple
    // resize (the camera object doesn't reset itself just because the
    // window changed size); only does a full reset the first time a
    // MEANINGFUL size is seen.
    //
    // "Meaningful" matters here: the canvas this camera drives starts
    // out behind a `display: none` screen (the startup gate), and
    // resizeCanvasForDPI() floors a hidden element's 0x0 box to 1x1 so
    // the backing store is never literally empty. A naive "did we ever
    // get a nonzero size before" check treats that 1x1 placeholder as a
    // real prior view, so the FIRST size that actually matters (the
    // canvas's true on-screen box, once its screen goes active) takes
    // the "already had a view, just update the zoom bounds" branch
    // instead of fitting - leaving zoom permanently stuck at whatever
    // the 1x1 box computed. state._hasRealView tracks this separately
    // from viewW/viewH so a placeholder call can't poison it.
    function setViewSize(viewW, viewH) {
      const meaningful = viewW > 8 && viewH > 8;
      state.viewW = viewW;
      state.viewH = viewH;
      if (!meaningful) return;
      if (!state._hasRealView) {
        reset(viewW, viewH);
        state._hasRealView = true;
      } else {
        // Keep zoom bounds sane relative to the new box size.
        const fit = fitZoom(viewW, viewH);
        state.minZoom = fit * 0.4;
        state.maxZoom = fit * 12;
      }
    }

    // Iso constants. At the same camera zoom a world cell is zoom px
    // wide in "2d" and the iso diamond is also ~zoom px wide (half
    // that tall), so fit-zoom and the min/max bounds mean the same
    // thing in both modes. One world unit of HEIGHT is Z_PX_PER_ZOOM *
    // zoom screen px (a cell's edge, drawn at 30 degrees, is ~0.56 *
    // zoom px long, so a 1-unit-tall box looks roughly cubic).
    const ISO_HALF = 0.5;
    const Z_PX_PER_ZOOM = 0.55;

    function isoA() { return state.zoom * ISO_HALF; } // screen px per world unit along each iso axis

    function worldToScreen(wx, wy, z) {
      // Rotation is 0 today (state.rotation is always 0). When it
      // isn't, this becomes a standard rotate-then-translate:
      //   const dx = wx - state.x, dy = wy - state.y;
      //   const cos = Math.cos(state.rotation), sin = Math.sin(state.rotation);
      //   const rx = dx * cos - dy * sin, ry = dx * sin + dy * cos;
      //   return { x: rx * state.zoom + state.viewW / 2, y: ry * state.zoom + state.viewH / 2 };
      const dx = wx - state.x;
      const dy = wy - state.y;
      if (state.viewMode === "iso") {
        const a = isoA();
        return {
          x: (dx - dy) * a + state.viewW / 2,
          y: (dx + dy) * a * 0.5 - (z || 0) * Z_PX_PER_ZOOM * state.zoom + state.viewH / 2,
        };
      }
      return {
        x: dx * state.zoom + state.viewW / 2,
        y: dy * state.zoom + state.viewH / 2,
      };
    }

    function screenToWorld(sx, sy, z) {
      // Inverse of worldToScreen() on the plane at height z (default 0);
      // same note about rotation applies (rotate the delta backwards by
      // -state.rotation before scaling).
      if (state.viewMode === "iso") {
        const a = isoA();
        const u = (sx - state.viewW / 2) / a;                                          // dx - dy
        const v = (sy - state.viewH / 2 + (z || 0) * Z_PX_PER_ZOOM * state.zoom) / (a * 0.5); // dx + dy
        return { x: (u + v) / 2 + state.x, y: (v - u) / 2 + state.y };
      }
      const dx = (sx - state.viewW / 2) / state.zoom;
      const dy = (sy - state.viewH / 2) / state.zoom;
      return { x: dx + state.x, y: dy + state.y };
    }

    // Moves the world point under the screen origin by the screen
    // delta, using the inverse above so it is exact in either mode (a
    // plain "delta / zoom" is only right for the top-down projection).
    function pan(dxScreen, dyScreen) {
      const a = screenToWorld(0, 0);
      const b = screenToWorld(dxScreen, dyScreen);
      state.x -= b.x - a.x;
      state.y -= b.y - a.y;
    }

    function zoomAt(sx, sy, factor) {
      const before = screenToWorld(sx, sy);
      state.zoom = Math.max(state.minZoom, Math.min(state.maxZoom, state.zoom * factor));
      const after = screenToWorld(sx, sy);
      // Keep the world point under the cursor fixed on screen.
      state.x += before.x - after.x;
      state.y += before.y - after.y;
    }

    function setViewMode(mode) {
      state.viewMode = (mode === "iso") ? "iso" : "2d";
    }

    // Screen px per world unit measured along a grid axis on the
    // ground: zoom in "2d"; in "iso" an axis step is (a, a/2) px, whose
    // length is a*sqrt(1.25). Drawing code sizes markers with this
    // rather than state.zoom so a marker's footprint looks the same
    // size in both modes.
    function groundPxPerUnit() {
      return state.viewMode === "iso" ? isoA() * Math.sqrt(1.25) : state.zoom;
    }

    // The linear part of worldToScreen(): the screen-space vector for a
    // WORLD-space delta. Lets hit-testing turn "click minus marker, in
    // world units on the marker's own plane" back into a screen
    // distance, so a billboard marker's hit area is the disc you see
    // rather than a squashed ellipse (a world circle is an ellipse on
    // an iso screen).
    function worldDeltaToScreen(dx, dy) {
      if (state.viewMode === "iso") {
        const a = isoA();
        return { x: (dx - dy) * a, y: (dx + dy) * a * 0.5 };
      }
      return { x: dx * state.zoom, y: dy * state.zoom };
    }

    // Screen px per world unit of height (0 in "2d", where height has
    // no meaning).
    function heightPx(z) {
      return state.viewMode === "iso" ? z * Z_PX_PER_ZOOM * state.zoom : 0;
    }

    // World-unit length of one screen pixel at the current zoom -
    // useful for drag thresholds that should feel the same regardless
    // of zoom level.
    function pxToWorldLength(px) {
      return px / groundPxPerUnit();
    }

    return {
      state: state,
      reset: reset,
      setViewSize: setViewSize,
      worldToScreen: worldToScreen,
      screenToWorld: screenToWorld,
      pan: pan,
      zoomAt: zoomAt,
      setViewMode: setViewMode,
      groundPxPerUnit: groundPxPerUnit,
      worldDeltaToScreen: worldDeltaToScreen,
      heightPx: heightPx,
      pxToWorldLength: pxToWorldLength,
    };
  }

  /* Resizes `canvas`'s backing store to match its CSS box at the
   * current devicePixelRatio, and scales the 2D context so all
   * drawing code can keep working in CSS pixels. Returns
   * {width, height, ctx} in CSS pixels. Safe to call every frame or
   * on every resize event; it only touches canvas.width/height (which
   * clears the canvas) when the size actually changed. */
  function resizeCanvasForDPI(canvas) {
    const rect = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(rect.width));
    const h = Math.max(1, Math.round(rect.height));
    const targetW = Math.max(1, Math.round(w * dpr));
    const targetH = Math.max(1, Math.round(h * dpr));
    const ctx = canvas.getContext("2d");
    if (canvas.width !== targetW || canvas.height !== targetH) {
      canvas.width = targetW;
      canvas.height = targetH;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    return { width: w, height: h, ctx: ctx };
  }

  global.DashboardCamera = { createCamera: createCamera, resizeCanvasForDPI: resizeCanvasForDPI };
})(window);

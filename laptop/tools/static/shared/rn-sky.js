/* RescueNet sky: every page takes its colours from the visitor's clock. The paper drifts minute by minute through night, dawn,
   noon, golden hour and dusk, so no two visits at different times look the same; the text colour switches side when the paper
   crosses from dark to light, so contrast never collapses. One file for the landing page, the simulator and the board screens.
   RNSky.palette(h) -> colours for an hour (0-24). RNSky.apply() writes them to :root as --paper, --ink, --mute, ... */
(function (root) {
  'use strict';
  /* dawn and dusk each take about an hour and a half, in small steps, so the light comes and goes the way it does outside;
     the morning stays a little greyer than noon, which is the brightest moment of the day */
  var KEYS = [
    [0, '#0e1014'], [3.5, '#101318'], [4.5, '#14161e'], [5, '#1b1c27'], [5.4, '#262433'], [5.75, '#383142'], [6.05, '#534655'],
    [6.3, '#75626c'], [6.55, '#9a8586'], [6.8, '#b9a59f'], [7.1, '#cebcb2'], [7.6, '#d8cbc0'], [8.3, '#d9d3c9'], [9.5, '#dbdad4'],
    [11, '#e4e4e1'], [12.5, '#e6e6e3'], [14, '#e6e6e3'], [15.3, '#e5e4df'], [16.4, '#e6d8c5'], [17.1, '#dfc4ab'], [17.5, '#d2b09c'],
    [17.85, '#b8958b'], [18.15, '#95767a'], [18.45, '#6f5868'], [18.75, '#4d3f50'], [19.1, '#352d3e'], [19.6, '#252331'],
    [20.5, '#181a23'], [21.5, '#12141b']
  ];
  function rgb(h) { h = h.replace('#', ''); return [0, 2, 4].map(function (i) { return parseInt(h.slice(i, i + 2), 16) / 255; }); }
  function hex(c) { return '#' + c.map(function (v) { var s = Math.round(Math.max(0, Math.min(1, v)) * 255).toString(16); return s.length < 2 ? '0' + s : s; }).join(''); }
  function mix(a, b, t) { return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t]; }
  function lin(v) { return v <= .04045 ? v / 12.92 : Math.pow((v + .055) / 1.055, 2.4); }
  function lum(c) { return .2126 * lin(c[0]) + .7152 * lin(c[1]) + .0722 * lin(c[2]); }
  function contrast(a, b) { var x = lum(a), y = lum(b); return (Math.max(x, y) + .05) / (Math.min(x, y) + .05); }
  var K = KEYS.map(function (k) { return [k[0], rgb(k[1])]; });
  /* a smooth curve through the keys (Catmull-Rom), so the colour never pauses or lurches at a key */
  function paperAt(h) {
    h = ((h % 24) + 24) % 24;
    var n = K.length, i = 0;
    for (var j = 0; j < n; j++) if (K[j][0] <= h) i = j;
    function at(k) { var m = ((k % n) + n) % n; return [K[m][0] + Math.floor(k / n) * 24, K[m][1]]; }
    var p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2), L = p2[0] - p1[0], t = (h - p1[0]) / L;
    var t2 = t * t, t3 = t2 * t, a = 2 * t3 - 3 * t2 + 1, b = t3 - 2 * t2 + t, c = -2 * t3 + 3 * t2, d = t3 - t2;
    return [0, 1, 2].map(function (k) {
      var m1 = (p2[1][k] - p0[1][k]) / (p2[0] - p0[0]) * L, m2 = (p3[1][k] - p1[1][k]) / (p3[0] - p1[0]) * L;
      return Math.max(0, Math.min(1, a * p1[1][k] + b * m1 + c * p2[1][k] + d * m2));
    });
  }
  var BLACK = [.04, .04, .04], WHITE = [.95, .94, .91];
  /* push a colour away from its background until it reads (WCAG contrast `min`) */
  function ensure(c, bg, min) { var to = lum(bg) > .18 ? [0, 0, 0] : [1, 1, 1]; for (var t = 0; t <= 1; t += .02) { var x = mix(c, to, t); if (contrast(x, bg) >= min) return x; } return to; }
  function palette(h) {
    var paper = paperAt(h), night = lum(paper) < .18;
    /* ink is a near-black (day) or a warm off-white (night), tinted a little by the paper so it belongs to the same hour */
    var ink = night ? mix(paper, WHITE, .9) : mix(paper.map(function (v) { return v * .12; }), BLACK, .55);
    var red = rgb('#e1251b'), teal = night ? rgb('#4fd1c9') : rgb('#0f8f8a');
    ink = ensure(ink, paper, 7);
    /* the deep surface (the connect screens): always dark, but carrying the hour's hue, so it drifts with the page.
       The paper's tint is strengthened and the colour brought down to a fixed darkness. */
    var pl = lum(paper), sm = Math.min(1, pl / .4), boost = 1 + 1.8 * sm * sm * (3 - 2 * sm);   /* night paper is already dark: keep it as it is */
    var avg = (paper[0] + paper[1] + paper[2]) / 3, tint = paper.map(function (v) { return Math.max(0, avg + (v - avg) * boost); });
    var lt = Math.max(1e-4, lum(tint)), sc = Math.pow(.011 / lt, 1 / 2.2), deep = tint.map(function (v) { return Math.min(1, v * sc); });
    var deepFg = ensure(mix(deep, WHITE, .9), deep, 9);
    return {
      hour: ((h % 24) + 24) % 24, night: night, light: lum(paper),
      paper: paper, paper2: mix(paper, ink, .06), ink: ink, mute: ensure(mix(paper, ink, .62), paper, 4.6), rule: mix(paper, ink, .26),
      /* red stays the brand red for the wordmark and fills; small red text gets a darker or lighter version that still reads */
      red: red, redText: ensure(red, paper, 4.6), teal: ensure(teal, paper, 3.2),
      /* colours for surfaces drawn in ink (the ledger, the connect screen), where paper and ink swap roles */
      onInkMute: ensure(mix(ink, paper, .7), ink, 4.6), onInkRule: mix(ink, paper, .22), onInkFaint: ensure(mix(ink, paper, .6), ink, 3.2),
      onInkTeal: ensure(night ? rgb('#0f8f8a') : rgb('#3fb8b2'), ink, 4.6), onInkTeal2: ensure(night ? rgb('#0f8f8a') : rgb('#62c9c4'), ink, 4.6),
      deep: deep, deepFg: deepFg, deepMute: ensure(mix(deep, deepFg, .62), deep, 4.6), deepFaint: ensure(mix(deep, deepFg, .45), deep, 3),
      deepRule: mix(deep, deepFg, .22), deepTeal: ensure(rgb('#4fd1c9'), deep, 4.6)
    };
  }
  var override = null, lapse = null, current = null, listeners = [];
  function clockHour() { var d = new Date(); return d.getHours() + d.getMinutes() / 60 + d.getSeconds() / 3600; }
  function hour() {
    if (lapse) return (lapse.h0 + (performance.now() - lapse.t0) / 1000 * lapse.rate) % 24;
    return override !== null ? override : clockHour();
  }
  function apply() {
    var p = palette(hour()), el = document.documentElement, s = el.style;
    var VARS = { paper: '--paper', paper2: '--paper-2', ink: '--ink', mute: '--mute', rule: '--rule', red: '--red', redText: '--red-text', teal: '--teal',
      onInkMute: '--on-ink-mute', onInkRule: '--on-ink-rule', onInkFaint: '--on-ink-faint', onInkTeal: '--on-ink-teal', onInkTeal2: '--on-ink-teal2',
      deep: '--deep', deepFg: '--deep-fg', deepMute: '--deep-mute', deepFaint: '--deep-faint', deepRule: '--deep-rule', deepTeal: '--deep-teal' };
    Object.keys(VARS).forEach(function (k) { s.setProperty(VARS[k], hex(p[k])); });
    var pc = p.paper.map(function (v) { return Math.round(v * 255); }).join(',');
    s.setProperty('--paper-0', 'rgba(' + pc + ',0)');
    s.colorScheme = p.night ? 'dark' : 'light';
    el.classList.toggle('night', p.night);
    /* for the few minutes the paper is mid-grey at dawn and dusk, blend-mode text would vanish; pages draw it in plain ink instead */
    el.classList.toggle('xover', p.light > .08 && p.light < .3);
    var m = document.querySelector('meta[name="theme-color"]'); if (m) m.setAttribute('content', hex(p.paper));
    current = p; listeners.forEach(function (f) { try { f(p); } catch (e) {} });
    return p;
  }
  var timer = null;
  function start() { apply(); clearInterval(timer); timer = setInterval(apply, lapse ? 60 : 20000); }
  root.RNSky = {
    palette: palette, apply: apply, hour: hour, hex: hex, contrast: contrast, keys: KEYS,
    get current() { return current || apply(); },
    /* preview a fixed hour (null = follow the clock again); never stored, so a reload always follows the clock */
    preview: function (h) { override = (h === null || h === undefined) ? null : +h; lapse = null; start(); },
    /* a whole day in `seconds` (default 24); call again to stop */
    timelapse: function (seconds) { if (lapse) { lapse = null; start(); return false; } lapse = { h0: hour(), t0: performance.now(), rate: 24 / (seconds || 24) }; start(); return true; },
    onChange: function (f) { listeners.push(f); }
  };
  if (typeof document !== 'undefined') start();
})(typeof window !== 'undefined' ? window : globalThis);

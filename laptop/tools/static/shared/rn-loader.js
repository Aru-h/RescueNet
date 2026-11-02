/* RescueNet loading screen for the sim page: what is being loaded, step by step, with a dot-matrix counter that ends on
   the city's number.

     var L = RNLoader.open({ kind: 'Recorded run', name: 'Demo mission' or null, city: 1041,
                             steps: [{ label: 'Reading the recording', weight: 35, worst: 2200 }, ...] });
     L.progress(k, fraction, detail)   step k is that far along (0..1), with a short detail ('23.4 of 59.7 MB')
     L.done(k, detail)                 step k finished
     L.finish().then(...)              everything has loaded: resolves when the screen has played through and been
                                       dismissed (a key or a click on the first run of a visit; on its own after that)
     L.fail(message)                   loading failed: the message, and a way back to the run list

   Honest pacing. The counter is the steps' combined progress, by weight. It never runs ahead of what has really
   loaded, and on the first run of a visit it is also held to each step's worst-case time (worst, ms), so the screen
   always plays through even on a fast laptop, then waits for a key or a click. After that first run, the pacing is a
   quarter of the worst case and the page opens on its own; any key skips the rest once everything has loaded.
   With reduced motion: no animation, the page opens as soon as it is ready.

   Style: the run screens' dark console (thin frame lines, corner ticks, a soft beam, grain), one dot-matrix counter,
   the step log, the city's number at the end. Off-white ink throughout (green means the simulated area on the map). */
(function (root) {
  'use strict';
  var SEEN_KEY = 'rnLoaderSeen';
  var F = {   /* 5 x 7 dot-matrix digits and a few signs */
    '0': '01110100011001110101110011000101110', '1': '00100011000010000100001000010001110', '2': '01110100010000100010001000100011111',
    '3': '11110000010000101110000010000111110', '4': '00010001100101010010111110001000010', '5': '11111100001111000001000011000101110',
    '6': '00110010001000011110100011000101110', '7': '11111000010001000100010000100001000', '8': '01110100011000101110100011000101110',
    '9': '01110100011000101111000010001001100', '%': '11000110010001000100010001001100011', ' ': '00000000000000000000000000000000000',
    '-': '00000000000000011111000000000000000'
  };
  var DIGITS = '0123456789';
  var CSS = '' +
    '.rnl{position:fixed;inset:0;z-index:100;background:#0b0c0e;color:#e6e6e3;overflow:hidden;font-family:"JetBrains Mono",ui-monospace,Menlo,monospace;transition:opacity .45s}' +
    '.rnl.gone{opacity:0;pointer-events:none}' +
    '.rnl .beam{position:absolute;left:50%;top:-10%;width:34%;height:120%;transform:translateX(-50%);background:radial-gradient(ellipse at 50% 0%,rgba(230,230,227,.09),transparent 62%);pointer-events:none;animation:rnlb 5s ease-in-out infinite}' +
    '@keyframes rnlb{50%{opacity:.55}}' +
    '.rnl .grain{position:absolute;inset:-50%;width:200%;height:200%;opacity:.08;pointer-events:none;mix-blend-mode:screen;animation:rnlj .9s steps(3) infinite}' +
    '@keyframes rnlj{33%{transform:translate(-3%,2%)}66%{transform:translate(2%,-3%)}}' +
    '.rnl svg.frame{position:absolute;inset:0;width:100%;height:100%;pointer-events:none}' +
    '.rnl .frame .dr{stroke-dasharray:1;stroke-dashoffset:1;animation:rnld 1.1s cubic-bezier(.6,0,.2,1) forwards}' +
    '@keyframes rnld{to{stroke-dashoffset:0}}' +
    '.rnl .tag{position:absolute;top:12%;font-size:clamp(9px,.8vw,11px);letter-spacing:.16em;text-transform:uppercase;color:#8d8d88}' +
    '.rnl canvas{position:absolute;left:50%;top:46%;transform:translate(-50%,-50%);width:min(30vw,52vh);aspect-ratio:46/13}' +
    '.rnl .under{position:absolute;left:50%;top:56%;transform:translateX(-50%);font-size:clamp(10px,.9vw,12px);letter-spacing:.2em;text-transform:uppercase;color:#8d8d88;white-space:nowrap}' +
    '.rnl .under b{color:#e6e6e3;font-weight:400}' +
    '.rnl .log{position:absolute;left:8%;bottom:13%;font-size:clamp(9px,.8vw,11px);letter-spacing:.06em;text-transform:uppercase;line-height:1.9;color:#8d8d88;max-width:44%}' +
    '.rnl .log div{white-space:nowrap}' +
    '.rnl .log div.now{background:#e6e6e3;color:#0b0c0e}' +
    '.rnl .log .ok{color:#e6e6e3}.rnl .log div.now .ok{color:#0b0c0e}' +
    '.rnl .title{position:absolute;right:8%;bottom:13%;text-align:right;font-family:Archivo,"Helvetica Neue",Arial,sans-serif;font-weight:800;font-stretch:118%;font-size:clamp(15px,2.2vw,30px);line-height:1.05;letter-spacing:.04em;text-transform:uppercase;color:#c9c2b8}' +
    '.rnl .prompt{position:absolute;left:50%;top:64%;transform:translateX(-50%);font-size:clamp(10px,1vw,13px);letter-spacing:.24em;text-transform:uppercase;white-space:nowrap;opacity:0;transition:opacity .4s}' +
    '.rnl .prompt.on{opacity:1;animation:rnlp 1.6s ease-in-out infinite}' +
    '@keyframes rnlp{50%{opacity:.35}}' +
    '.rnl.wait{cursor:pointer}' +
    '.rnl .err{position:absolute;left:50%;top:64%;transform:translateX(-50%);max-width:60ch;text-align:center;font-size:13px;line-height:1.6;color:#e6e6e3}' +
    '.rnl .err a{color:#e6e6e3}' +
    '@media (max-width:700px){.rnl .log{display:none}.rnl canvas{width:70vw}}' +
    '@media (prefers-reduced-motion:reduce){.rnl .beam,.rnl .grain,.rnl .prompt.on{animation:none}.rnl .frame .dr{animation:none;stroke-dashoffset:0}.rnl{transition:none}}';

  function esc(s) { return String(s == null ? '' : s).replace(/[&<>"]/g, function (c) { return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]; }); }
  function seen() { try { return sessionStorage.getItem(SEEN_KEY) === '1'; } catch (e) { return false; } }
  function markSeen() { try { sessionStorage.setItem(SEEN_KEY, '1'); } catch (e) { /* a private window: every run plays in full */ } }
  var reduced = !!(root.matchMedia && root.matchMedia('(prefers-reduced-motion: reduce)').matches);

  function open(opts) {
    if (!document.getElementById('rnlCss')) { var st = document.createElement('style'); st.id = 'rnlCss'; st.textContent = CSS; document.head.appendChild(st); }
    var first = !seen(), steps = opts.steps.slice(), total = steps.reduce(function (a, s) { return a + s.weight; }, 0) || 1;
    var pace = first ? 1 : 0.25;
    var el = document.createElement('div'); el.className = 'rnl'; el.setAttribute('role', 'status'); el.setAttribute('aria-live', 'polite');
    el.innerHTML = '<div class="beam"></div>' +
      '<svg class="grain" aria-hidden="true"><filter id="rnlN"><feTurbulence type="fractalNoise" baseFrequency=".85" numOctaves="2" stitchTiles="stitch"/></filter><rect width="100%" height="100%" filter="url(#rnlN)"/></svg>' +
      '<svg class="frame" viewBox="0 0 1000 625" preserveAspectRatio="none" fill="none" stroke="rgba(230,230,227,.16)" stroke-width="1" aria-hidden="true">' +
      '<path class="dr" pathLength="1" d="M40 60 H960 M40 565 H960"/><path class="dr" pathLength="1" style="animation-delay:.15s" d="M60 40 V585 M940 40 V585"/>' +
      '<path class="dr" pathLength="1" style="animation-delay:.3s" stroke="rgba(230,230,227,.26)" d="M260 120 Q500 70 740 120"/>' +
      '<path d="M18 22 h40 M18 22 v14 M982 22 h-40 M982 22 v14 M18 603 h40 M18 603 v-14 M982 603 h-40 M982 603 v-14" stroke="rgba(230,230,227,.35)"/></svg>' +
      '<span class="tag" style="left:8%">RescueNet</span><span class="tag" style="right:8%">' + esc(opts.kind) + '</span>' +
      '<canvas aria-hidden="true"></canvas><div class="under"></div><div class="prompt">Press any key or click to open</div>' +
      '<div class="log">' + steps.map(function (s) { return '<div>// ' + esc(s.label) + '&hellip; <span class="ok"></span></div>'; }).join('') + '</div>' +
      '<div class="title">' + (opts.name ? esc(opts.name) + '<br>' : '') + (opts.city != null ? 'City ' + esc(opts.city) : '') + '</div>';
    document.body.appendChild(el);
    var cv = el.querySelector('canvas'), cx = cv.getContext('2d'), rows = el.querySelectorAll('.log div'), under = el.querySelector('.under');
    var real = steps.map(function () { return 0; }), shown = steps.map(function () { return 0; }), detail = steps.map(function () { return ''; });
    var started = steps.map(function () { return null; }), finished = false, closed = false, waiting = false, resolveFn = null, raf = 0, endT = 0;

    function dots(text) {
      var d = Math.min(2, root.devicePixelRatio || 1), w = cv.clientWidth, h = cv.clientHeight; if (!w || !h) return;
      if (cv.width !== Math.round(w * d)) { cv.width = Math.round(w * d); cv.height = Math.round(h * d); }
      cx.setTransform(d, 0, 0, d, 0, 0); cx.clearRect(0, 0, w, h);
      var step = w / 41, r = step * .34, y0 = (h - step * 7) / 2 + step / 2, x0 = (w - step * (text.length * 6 - 1)) / 2 + step / 2;
      for (var c = 0; c < text.length; c++) {
        var g = F[text[c]] || F[' '];
        for (var i = 0; i < 35; i++) {
          var on = g[i] === '1';
          cx.beginPath(); cx.arc(x0 + (c * 6 + i % 5) * step, y0 + Math.floor(i / 5) * step, on ? r : r * .32, 0, 6.283);
          cx.fillStyle = on ? '#e6e6e3' : 'rgba(230,230,227,.08)'; cx.fill();
        }
      }
    }
    function pct(v) { v = Math.max(0, Math.min(100, Math.floor(v))); return ('00' + v).slice(-3) + '%'; }
    /* each frame: every step's shown progress follows the real one, held to its worst-case pace (the first run of a visit) */
    function frame() {
      if (closed) return;
      var now = performance.now(), sum = 0, cur = -1;
      for (var k = 0; k < steps.length; k++) {
        if (k > 0 && shown[k - 1] < 1) break;   /* in order: a step shows only once the one before has */
        if (started[k] == null) started[k] = now;
        var cap = reduced ? 1 : Math.min(1, (now - started[k]) / Math.max(1, steps[k].worst * pace));
        shown[k] = Math.min(real[k], cap);
        cur = k;
        if (shown[k] < 1) break;
      }
      for (var j = 0; j < steps.length; j++) sum += shown[j] * steps[j].weight;
      for (var m = 0; m < rows.length; m++) {
        rows[m].className = m === cur && shown[m] < 1 ? 'now' : '';
        rows[m].querySelector('.ok').textContent = shown[m] >= 1 ? '( DONE )' : '';
      }
      var all = shown.every(function (v) { return v >= 1; });
      if (!all) {
        dots(pct(sum / total * 100));
        /* the detail only once the shown progress has caught up with it: never a number ahead of the counter */
        if (cur >= 0) under.innerHTML = '<b>' + esc(steps[cur].label) + '</b>' + (detail[cur] && shown[cur] >= real[cur] - 1e-6 ? ' &middot; ' + esc(detail[cur]) : '');
      } else if (finished && !endT) ending();
      raf = requestAnimationFrame(frame);
    }
    /* the end: the counter scrambles and settles on the city's number, then waits (first run of a visit) or opens */
    function ending() {
      endT = performance.now();
      var target = String(opts.city != null ? opts.city : '').slice(-4), n = 0;
      if (reduced || !target) { dots(target || '100%'); return settle(); }
      while (target.length < 4) target = ' ' + target;
      (function scramble() {
        if (closed) return;
        n++;
        var s = ''; for (var i = 0; i < 4; i++) s += n > 9 + i * 3 ? target[i] : DIGITS[Math.floor(Math.random() * 10)];
        dots(s); under.innerHTML = n > 20 ? '<b>Ready</b>' : '<b>100%</b>';
        if (n < 26) setTimeout(scramble, 55); else settle();
      })();
    }
    function settle() {
      under.innerHTML = '<b>Ready</b>';
      if (first && !reduced) { waiting = true; el.classList.add('wait'); el.querySelector('.prompt').classList.add('on'); }
      else setTimeout(proceed, reduced ? 0 : 350);
    }
    function proceed() {
      if (closed) return;
      closed = true; waiting = false; markSeen(); cancelAnimationFrame(raf);
      document.removeEventListener('keydown', onKey, true);
      el.classList.add('gone'); setTimeout(function () { el.remove(); }, reduced ? 0 : 500);
      if (resolveFn) resolveFn();
    }
    function skipToEnd() { for (var k = 0; k < steps.length; k++) { started[k] = -1e12; } }   /* not the first run: a key jumps to the loaded state */
    function onKey(e) {
      if (e.metaKey || e.ctrlKey || e.altKey) return;
      if (waiting) { e.preventDefault(); e.stopPropagation(); proceed(); }
      else if (!first && finished) { e.preventDefault(); e.stopPropagation(); skipToEnd(); }
    }
    document.addEventListener('keydown', onKey, true);
    el.addEventListener('click', function () { if (waiting) proceed(); else if (!first && finished) skipToEnd(); });
    raf = requestAnimationFrame(frame);

    return {
      progress: function (k, f, d) { if (k < 0 || k >= steps.length) return; real[k] = Math.max(real[k], Math.min(1, f)); if (d != null) detail[k] = d; },
      done: function (k, d) { if (k < 0 || k >= steps.length) return; real[k] = 1; if (d != null) detail[k] = d; },
      finish: function () {
        for (var k = 0; k < steps.length; k++) real[k] = 1;
        finished = true;
        return new Promise(function (res) { resolveFn = res; if (closed) res(); });
      },
      fail: function (msg, backHref) {
        cancelAnimationFrame(raf); closed = true; document.removeEventListener('keydown', onKey, true);
        dots(' -- ');
        under.innerHTML = '<b>Not loaded</b>';
        var e = document.createElement('p'); e.className = 'err';
        e.innerHTML = esc(msg) + (backHref ? ' <a href="' + esc(backHref) + '">Choose another run</a>' : '');
        el.appendChild(e);
      },
      close: proceed,
      element: el
    };
  }
  root.RNLoader = { open: open };
})(typeof window !== 'undefined' ? window : this);

/* RescueNet run store: one run (a recording, or a live run as it arrives) turned into what the sim page draws.

   It reads the simulator's own lines, one at a time and in order:
     RECORDING {...}   the recording's header (seed, ticks, fault profile, radio loss)
     STATICENV {...}   buildings (x, y = footprint CENTRE cell), the epicentre, the exits; repeated when damage changes
     STATICMAP {...}   the terrain grid (water, roads by class); repeated when it changes
     LIVEFRAME {...}   one tick: teams, incidents, safe zones, hazard cells, radio
   A recording is fed line by line (RNRun.fromText); a live run feeds the same lines as they arrive (run.add(tag, obj)),
   so both go through one path. Anything else (OVERRIDE, OVERRIDEACK, ...) is kept out of the way.

   Kept small: each tick keeps its frame as the line it came in (re-parsed only for the tick being looked at) plus the
   few numbers the timeline needs for every tick (team positions, road blocks, worksite states).

   run.at(t) gives the page's view of tick t: the same shape the mockup's data had (laptop/tools/sim page, built from
   tools/build_mock_data.py), worked out from that tick's real frame. Ticks are looked up by the frame's own tick
   number, never by position, so a run that starts later than tick 0 (a take-over) works too.

   Worksites: incidents that can hold a live rescue (INSARAG worksites) are numbered A-1, A-2, ... in the order teams
   find them; the simulated area is sector A. */
(function (root) {
  'use strict';
  var LIVE = { 0: 1, 1: 1, 2: 1, 5: 1 };   /* incident types that can hold a live rescue: collapse, confined space, mass casualty, survivor signal */
  var STATE = ['open', 'assigned', 'working', 'complete'];
  var TICK_S = 36;                           /* one simulator tick is 36 s of mission (TICK_TIME_SEC, core/mission_link.h) */

  function footprint(b) {                    /* centre cell -> top-left cell (core/environment.c: loc_x = x0 + w / 2) */
    var w = Math.max(1, b.w || 1), h = Math.max(1, b.h || 1);
    return { x: b.x - Math.floor(w / 2), y: b.y - Math.floor(h / 2), w: w, h: h };
  }
  function parseMap(m) {                     /* the STATICMAP grid: 8 hex characters per cell, flags in the first two */
    var roads = [], water = [], bridges = [], W = m.width || 36, H = m.height || 36;
    (m.rows || []).forEach(function (row, y) {
      for (var k = 0; k + 2 <= row.length; k += 8) {
        var x = k / 8, fl = parseInt(row.substr(k, 2), 16), rc = (fl >> 2) & 7;
        if (fl & 2) water.push([x, y]);
        if (rc) { roads.push([x, y, rc]); if (fl & 2) bridges.push([x, y]); }
      }
    });
    return { W: W, H: H, roads: roads, water: water, bridges: bridges };
  }

  function Run() {
    this.header = null;
    this.ticks = [];          /* tick numbers in arrival order */
    this.lines = {};          /* tick -> the LIVEFRAME line */
    this.envs = [];           /* [{ from: first tick it applies to, env }] */
    this.maps = [];           /* [{ from, map (parsed) }] */
    this.track = {};          /* tick -> [[x, y, status, stamina]] per team */
    this.blockedTrack = {};   /* tick -> [[x, y]] */
    this.detTrack = {};       /* tick -> [[fault_mask, detected 0/1]] per team (see spans()) */
    this._spans = null;
    this.seen = {};           /* incident id -> worksite id ('A-1', ...) */
    this.nSeen = 0;
    this.events = [];         /* [tick, kind, worksite id, live victims or null] */
    this.wsTrack = {};        /* worksite id -> { id, x, y, found, states: [[tick, status, victims, team]] } */
    this.wsOrder = [];
    this._prev = null;        /* incident id -> status, last frame */
    this._prevOrder = null;   /* incident ids, last frame, in its order */
    this.lastTick = null;
  }
  Run.prototype.add = function (tag, obj, raw) {
    var from = this.lastTick == null ? -Infinity : this.lastTick + 1;
    if (tag === 'RECORDING') { this.header = obj; return; }
    if (tag === 'STATICENV') { this.envs.push({ from: from, env: obj }); return; }
    if (tag === 'STATICMAP') { this.maps.push({ from: from, map: parseMap(obj) }); return; }
    if (tag !== 'LIVEFRAME') return;
    var f = obj, t = f.tick, self = this;
    if (this.lines[t] !== undefined) return;   /* a tick already held (a repeated line) */
    this.lines[t] = raw || JSON.stringify(f);
    this.ticks.push(t); this.lastTick = t;
    this.track[t] = (f.teams || []).map(function (m) { return [m.x, m.y, m.status, Math.round(m.stamina)]; });
    this.blockedTrack[t] = (f.hazard_cells || []).filter(function (c) { return c.blocked; }).map(function (c) { return [c.x, c.y]; });
    this.detTrack[t] = (f.teams || []).map(function (m) {
      var tier = m.consensus_tier || [0, 0];
      return [m.fault_mask || 0, (tier[0] === 2 || tier[1] === 2 || m.degraded) ? 1 : 0];
    });
    this._spans = null;
    /* worksites found this tick: numbered in the order found */
    var cur = {}, found = [];
    (f.incidents || []).forEach(function (i) {
      cur[i.id] = i.status;
      if (LIVE[i.type] && i.discovered && self.seen[i.id] === undefined) {
        var wid = 'A-' + (++self.nSeen); self.seen[i.id] = wid;
        found.push([t, 'found', wid, i.victims]);
      }
    });
    found.forEach(function (e) { self.events.push(e); });
    /* state changes since the last tick, and worksites that left the frame (finished and cleared away) */
    /* (in the frame's own order: an object's number-like keys would come out sorted) */
    var prev = this._prev, prevOrder = this._prevOrder || [];
    if (prev) {
      (f.incidents || []).forEach(function (i) {
        var k = i.id;
        if (self.seen[k] !== undefined && prev[k] !== undefined && cur[k] !== prev[k]) self.events.push([t, STATE[cur[k]] || String(cur[k]), self.seen[k], null]);
      });
      prevOrder.forEach(function (k) {
        if (self.seen[k] !== undefined && cur[k] === undefined && prev[k] !== 3) self.events.push([t, 'complete', self.seen[k], null]);
      });
    }
    /* each worksite's history: its state whenever status, live victims or team change */
    (f.incidents || []).forEach(function (i) {
      var wid = self.seen[i.id]; if (wid === undefined) return;
      var w = self.wsTrack[wid];
      if (!w) { w = self.wsTrack[wid] = { id: wid, x: i.x, y: i.y, found: t, states: [[t, i.status, i.victims, i.assigned_team]] }; self.wsOrder.push(wid); }
      else { var l = w.states[w.states.length - 1]; if (i.status !== l[1] || i.victims !== l[2] || i.assigned_team !== l[3]) w.states.push([t, i.status, i.victims, i.assigned_team]); }
    });
    if (prev) prevOrder.forEach(function (k) {
      var wid = self.seen[k]; if (wid === undefined || cur[k] !== undefined) return;
      var w = self.wsTrack[wid], l = w.states[w.states.length - 1];
      if (l[1] !== 3) w.states.push([t, 3, 0, l[3]]);
    });
    this._prev = cur; this._prevOrder = (f.incidents || []).map(function (i) { return i.id; });
  };
  /* one line of a recording or a live stream */
  Run.prototype.line = function (text) {
    var sp = text.indexOf(' '); if (sp < 0) return;
    var tag = text.slice(0, sp);
    if (tag !== 'LIVEFRAME' && tag !== 'STATICENV' && tag !== 'STATICMAP' && tag !== 'RECORDING') return;
    var body = text.slice(sp + 1), obj;
    try { obj = JSON.parse(body); } catch (e) { return; }
    this.add(tag, obj, tag === 'LIVEFRAME' ? body : undefined);
  };

  Run.prototype.first = function () { return this.ticks.length ? this.ticks[0] : null; };
  Run.prototype.last = function () { return this.lastTick; };
  /* the run's planned length in ticks: the header's, else what has arrived */
  Run.prototype.length = function () { return (this.header && this.header.ticks) || (this.lastTick == null ? 0 : this.lastTick + 1); };
  Run.prototype.has = function (t) { return this.lines[t] !== undefined; };
  /* the nearest tick held at or before t (a live run may not have reached t yet) */
  Run.prototype.clamp = function (t) {
    if (!this.ticks.length) return null;
    t = Math.round(t);
    if (t <= this.ticks[0]) return this.ticks[0];
    if (t >= this.lastTick) return this.lastTick;
    while (t > this.ticks[0] && this.lines[t] === undefined) t--;
    return t;
  };
  Run.prototype.frame = function (t) { var l = this.lines[t]; return l === undefined ? null : JSON.parse(l); };
  function lastFrom(list, t) { var out = null; for (var i = 0; i < list.length; i++) if (list[i].from <= t) out = list[i]; return out || list[0] || null; }
  Run.prototype.envAt = function (t) { var e = lastFrom(this.envs, t); return e ? e.env : null; };
  Run.prototype.mapAt = function (t) { var m = lastFrom(this.maps, t); return m ? m.map : null; };

  /* the page's view of tick t */
  Run.prototype.at = function (t) {
    t = this.clamp(t); if (t == null) return null;
    var f = this.frame(t), env = this.envAt(t) || { buildings: [] }, map = this.mapAt(t) || { W: 36, H: 36, roads: [], water: [], bridges: [] }, self = this;
    /* worksites known by now, in their state at t; and which worksite each team is assigned to */
    var worksites = [], done = [];
    this.wsOrder.forEach(function (wid) {
      var w = self.wsTrack[wid]; if (w.found > t) return;
      var st = w.states[0]; w.states.forEach(function (s) { if (s[0] <= t) st = s; });
      if (st[1] === 3) done.push({ id: w.id, x: w.x, y: w.y });
      else worksites.push({ id: w.id, x: w.x, y: w.y, victims: st[2], status: st[1], team: st[3] });
    });
    var known = {}; Object.keys(this.seen).forEach(function (k) { var w = self.wsTrack[self.seen[k]]; if (w && w.found <= t) known[k] = self.seen[k]; });
    var teams = (f.teams || []).map(function (m) {
      var tier = m.consensus_tier || [0, 0];
      return { id: m.id, x: m.x, y: m.y, stamina: Math.round(m.stamina), status: m.status, spec: m.spec,
        dest: m.has_dest ? [m.dest_x, m.dest_y] : null, site: m.has_dest && known[m.dest_id] !== undefined ? known[m.dest_id] : null,
        tier: tier, fault: m.fault_mask || 0, peers: m.peer_votes || null, heard: m.heard == null ? null : m.heard,
        equip: m.equipment == null ? null : Math.round(m.equipment) };
    });
    var other = (f.incidents || []).filter(function (i) { return known[i.id] === undefined && i.discovered && !LIVE[i.type]; })
      .map(function (i) { return { x: i.x, y: i.y, type: i.type }; });
    var buildings = (env.buildings || []).map(function (b) {
      var p = footprint(b); return { id: b.id, x: p.x, y: p.y, w: p.w, h: p.h, cx: b.x, cy: b.y, type: b.type, floors: b.floors, damage: b.damage };
    });
    return {
      tick: t, first: this.first(), last: this.lastTick, ticks: this.length(),
      W: map.W, H: map.H, roads: map.roads, water: map.water, bridges: map.bridges,
      buildings: buildings, epicenter: env.epicenter || null, gateways: env.gateways || [],
      blocked: this.blockedTrack[t] || [],
      safezones: (f.safezones || []).filter(function (s) { return (s.capacity == null ? 1 : s.capacity) > 0; }),
      teams: teams, worksites: worksites, done: done, other: other,
      events: this.events, vbus: f.vbus || null, resolved: f.incidents_resolved,
      raw_teams: f.teams || [], raw_incidents: f.incidents || [], frame: f
    };
  };
  /* Per team, the stretches of ticks [from, to] when a fault was injected (fault_mask, the injector's own state:
     GROUND TRUTH, never something the teams observed) and when the system had detected one (the same rule as the old
     dashboard's detection timeline: consensus ELEVATED for movement or radio, or the recovery side's degraded flag).
     Kept apart on purpose: the point is to see where they disagree. */
  Run.prototype.spans = function () {
    if (this._spans) return this._spans;
    var out = {}, self = this, open = {};
    this.ticks.forEach(function (t) {
      (self.detTrack[t] || []).forEach(function (d, k) {
        var o = out[k] || (out[k] = { fault: [], flag: [] }), op = open[k] || (open[k] = { fault: null, flag: null });
        [['fault', d[0] !== 0], ['flag', d[1] === 1]].forEach(function (p) {
          var name = p[0];
          if (p[1]) { if (op[name] && op[name][1] === t - 1) op[name][1] = t; else { op[name] = [t, t]; o[name].push(op[name]); } }
        });
      });
    });
    return (this._spans = out);
  };
  /* events up to tick t, newest first */
  Run.prototype.eventsTo = function (t) { var out = []; for (var i = this.events.length - 1; i >= 0; i--) if (this.events[i][0] <= t) out.push(this.events[i]); return out; };
  Run.prototype.wsInfo = function (wid) { return this.wsTrack[wid] || null; };

  function fromText(text) {
    var run = new Run(), i = 0, n = text.length;
    while (i < n) {
      var j = text.indexOf('\n', i); if (j < 0) j = n;
      if (j > i) run.line(text.slice(i, j));
      i = j + 1;
    }
    return run;
  }

  root.RNRun = { create: function () { return new Run(); }, fromText: fromText, footprint: footprint, parseMap: parseMap, TICK_S: TICK_S, LIVE: LIVE };
  if (typeof module !== 'undefined' && module.exports) module.exports = root.RNRun;
})(typeof window !== 'undefined' ? window : globalThis);

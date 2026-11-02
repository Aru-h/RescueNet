/* RescueNet map notation: one place for the colours, classes and symbols every map draws with, so the run list, its
   legend and the sim page cannot drift apart. Each rule follows a field standard (the research doc's "Map notation" tab):
     - building damage: the Copernicus EMS grading colours (red / orange / yellow), measured from the EMSR648 legend
     - road block: the Copernicus "road barrier" diamond
     - people gathering: the Copernicus "spontaneous site" magenta
     - epicentre: the USGS ShakeMap star
     - worksites: INSARAG IDs (sector letter, hyphen, number), victims as L-n, a line through a completed worksite,
       and the ICMS colours (blue active, green complete)
   The standard colours are fixed: they never follow the page's clock palette. Only outlines take the page's ink.
   RNNotation.damageClass(d) -> 'destroyed' | 'moderate' | 'slight' | 'none' for the simulator's 0-100 damage. */
(function (root) {
  'use strict';
  var COLOUR = {
    destroyed: '#ff0000',   /* Copernicus "Destroyed" */
    moderate: '#e69800',    /* Copernicus "Damaged" */
    slight: '#ffff00',      /* Copernicus "Possibly damaged"; here the simulator's own "slight" class (named so in the legend) */
    barrier: '#b60043',     /* Copernicus "Road barrier" */
    gathering: '#ff00c5',   /* Copernicus "Spontaneous site" */
    active: '#2b7de9',      /* ICMS: worksite in progress */
    complete: '#2ea44f',    /* ICMS: worksite complete */
    star: '#ff0000'         /* USGS epicentre star */
  };
  /* the simulator's damage levels (core/environment.h ENV_DAMAGE_*): 25 minor, 50 moderate, 75 severe, 100 destroyed */
  function damageClass(d) { return d >= 75 ? 'destroyed' : d >= 50 ? 'moderate' : d >= 25 ? 'slight' : 'none'; }

  /* sectors: INSARAG letters, skipping I and O so they are never read as 1 and 0 */
  var SECTOR_LETTERS = 'ABCDEFGHJKLMNPQRSTUVWXYZ';

  /* ---- canvas symbols. (cx, cy) is the centre, s the symbol's size in pixels, ink the page's text colour ---- */
  function building(x, px, py, w, h, d, ink, lw) {
    var k = damageClass(d);
    x.lineWidth = lw || 1;
    if (k === 'none') { x.strokeStyle = ink; x.globalAlpha = .7; x.strokeRect(px, py, w, h); x.globalAlpha = 1; return; }
    x.fillStyle = COLOUR[k]; x.fillRect(px, py, w, h);
    x.strokeStyle = ink; x.globalAlpha = .55; x.strokeRect(px, py, w, h); x.globalAlpha = 1;   /* yellow needs an edge on light paper */
  }
  function star(x, cx, cy, s, ink) {
    var R = s / 2, r = R * .45; x.beginPath();
    for (var i = 0; i < 10; i++) { var a = -Math.PI / 2 + i * Math.PI / 5, rr = i % 2 ? r : R; x[i ? 'lineTo' : 'moveTo'](cx + rr * Math.cos(a), cy + rr * Math.sin(a)); }
    x.closePath(); x.fillStyle = COLOUR.star; x.fill(); x.lineWidth = Math.max(1.2, s / 10); x.strokeStyle = ink; x.lineJoin = 'round'; x.stroke();
  }
  function barrier(x, cx, cy, s) {
    var h = s / 2; x.beginPath(); x.moveTo(cx, cy - h); x.lineTo(cx + h, cy); x.lineTo(cx, cy + h); x.lineTo(cx - h, cy); x.closePath();
    x.fillStyle = COLOUR.barrier; x.fill();
    x.fillStyle = '#ffffff'; x.fillRect(cx - h * .5, cy - h * .14, h, h * .28);   /* the white bar of the barrier */
  }
  function assembly(x, cx, cy, s, fill, ink) {   /* a planned assembly point (the simulator's safe zone) */
    var h = s / 2; x.fillStyle = fill; x.fillRect(cx - h, cy - h, s, s); x.lineWidth = Math.max(1, s / 12); x.strokeStyle = ink; x.strokeRect(cx - h, cy - h, s, s);
  }
  function gathering(x, cx, cy, s) {
    x.beginPath(); x.arc(cx, cy, s / 2, 0, 6.283); x.lineWidth = Math.max(1.5, s / 5); x.strokeStyle = COLOUR.gathering; x.stroke();
  }
  function team(x, cx, cy, s, ink) { x.beginPath(); x.arc(cx, cy, s / 2, 0, 6.283); x.fillStyle = ink; x.fill(); }
  /* an INSARAG worksite: a box (the worksite marking), outlined while unassigned, blue while a team works it, green with a
     horizontal line through the whole marking once complete (Vol II Man B 6.2). state: 'open' | 'active' | 'complete' */
  function worksite(x, cx, cy, s, state, ink, bg) {
    var w = s, h = s * .78, l = cx - w / 2, t = cy - h / 2;
    x.lineWidth = Math.max(1.2, s / 9);
    x.fillStyle = state === 'active' ? COLOUR.active : state === 'complete' ? COLOUR.complete : bg;
    x.fillRect(l, t, w, h); x.strokeStyle = ink; x.strokeRect(l, t, w, h);
    if (state === 'complete') { x.beginPath(); x.moveTo(l - s * .25, cy); x.lineTo(l + w + s * .25, cy); x.stroke(); }
  }
  /* a team, by status. There is no common colour standard for team status (ICS gives the words: available, assigned,
     out of service), so the colour reuses this notation's own meaning: blue is "at work on a worksite", as for the
     worksite itself. ready: solid ink; moving: solid ink with a blue ring; working: solid blue; resting: hollow. */
  function teamMarker(x, cx, cy, r, state, label, ink, bg) {
    x.beginPath(); x.arc(cx, cy, r, 0, 6.283);
    var text = bg;
    if (state === 'resting') { x.fillStyle = bg; x.fill(); x.lineWidth = Math.max(1.4, r / 5); x.strokeStyle = ink; x.stroke(); text = ink; }
    else if (state === 'working') { x.fillStyle = COLOUR.active; x.fill(); x.lineWidth = Math.max(1.2, r / 6); x.strokeStyle = ink; x.stroke(); text = '#ffffff'; }
    else { x.fillStyle = ink; x.fill(); }
    if (state === 'moving') { x.beginPath(); x.arc(cx, cy, r + Math.max(2, r * .35), 0, 6.283); x.lineWidth = Math.max(1.6, r / 4); x.strokeStyle = COLOUR.active; x.stroke(); }
    if (label != null) {
      x.font = '600 ' + Math.round(r * 1.15) + 'px "JetBrains Mono", ui-monospace, monospace'; x.textAlign = 'center'; x.textBaseline = 'middle';
      x.fillStyle = text; x.fillText(String(label), cx, cy + .5); x.textAlign = 'start'; x.textBaseline = 'alphabetic';
    }
  }
  /* a task that is not a live-rescue worksite (sector assessment, supplies, evacuation, road clearing): a small open triangle */
  function task(x, cx, cy, s, ink) {
    var h = s / 2; x.beginPath(); x.moveTo(cx, cy - h); x.lineTo(cx + h, cy + h * .8); x.lineTo(cx - h, cy + h * .8); x.closePath();
    x.lineWidth = Math.max(1, s / 8); x.strokeStyle = ink; x.stroke();
  }

  /* ---- the legend, as small inline SVGs drawn from the same constants ---- */
  var LEGEND = {
    destroyed: ['Destroyed or severe', '<rect x="1.5" y="1.5" width="11" height="11" fill="' + COLOUR.destroyed + '" stroke="currentColor" stroke-opacity=".55"/>'],
    moderate: ['Moderate damage', '<rect x="1.5" y="1.5" width="11" height="11" fill="' + COLOUR.moderate + '" stroke="currentColor" stroke-opacity=".55"/>'],
    slight: ['Slight damage', '<rect x="1.5" y="1.5" width="11" height="11" fill="' + COLOUR.slight + '" stroke="currentColor" stroke-opacity=".55"/>'],
    none: ['Little or no damage', '<rect x="1.5" y="1.5" width="11" height="11" fill="none" stroke="currentColor" stroke-opacity=".7"/>'],
    epicentre: ['Epicentre', '<path d="M7 .8l1.6 4.2 4.5.2-3.5 2.8 1.2 4.4L7 9.9l-3.8 2.5 1.2-4.4L.9 5.2l4.5-.2z" fill="' + COLOUR.star + '" stroke="currentColor" stroke-width=".8"/>'],
    assembly: ['Assembly point', '<rect x="2" y="2" width="10" height="10" fill="var(--c-teal, #4fd1c9)" stroke="currentColor"/>'],
    gathering: ['People gathering', '<circle cx="7" cy="7" r="4.5" fill="none" stroke="' + COLOUR.gathering + '" stroke-width="2.2"/>'],
    team: ['Team', '<circle cx="7" cy="7" r="3.5" fill="currentColor"/>'],
    barrier: ['Road blocked', '<path d="M7 .8L13.2 7 7 13.2.8 7z" fill="' + COLOUR.barrier + '"/><rect x="3.9" y="6.1" width="6.2" height="1.8" fill="#fff"/>'],
    wsOpen: ['Worksite, no team yet', '<rect x="1.5" y="3" width="11" height="8.5" fill="none" stroke="currentColor" stroke-width="1.4"/>'],
    wsActive: ['Worksite, team working', '<rect x="1.5" y="3" width="11" height="8.5" fill="' + COLOUR.active + '" stroke="currentColor" stroke-width="1.4"/>'],
    wsDone: ['Worksite complete', '<rect x="2.5" y="3" width="9" height="8.5" fill="' + COLOUR.complete + '" stroke="currentColor" stroke-width="1.4"/><path d="M0 7.2h14" stroke="currentColor" stroke-width="1.4"/>'],
    task: ['Other task', '<path d="M7 2.5l5 8.6H2z" fill="none" stroke="currentColor" stroke-width="1.3"/>'],
    teamReady: ['Team, ready', '<circle cx="7" cy="7" r="4.5" fill="currentColor"/>'],
    teamMoving: ['Team, on the way', '<circle cx="7" cy="7" r="3.6" fill="currentColor"/><circle cx="7" cy="7" r="5.8" fill="none" stroke="' + COLOUR.active + '" stroke-width="1.6"/>'],
    teamWorking: ['Team, working', '<circle cx="7" cy="7" r="4.8" fill="' + COLOUR.active + '" stroke="currentColor"/>'],
    teamResting: ['Team, resting', '<circle cx="7" cy="7" r="4.3" fill="none" stroke="currentColor" stroke-width="1.4"/>']
  };
  function legendItem(key) { var it = LEGEND[key]; return '<li><svg viewBox="0 0 14 14" aria-hidden="true">' + it[1] + '</svg>' + it[0] + '</li>'; }
  function legendHTML(keys) { return keys.map(legendItem).join(''); }

  root.RNNotation = {
    COLOUR: COLOUR, SECTOR_LETTERS: SECTOR_LETTERS, damageClass: damageClass,
    building: building, star: star, barrier: barrier, assembly: assembly, gathering: gathering, team: team, teamMarker: teamMarker, worksite: worksite, task: task,
    LEGEND: LEGEND, legendHTML: legendHTML
  };
})(typeof window !== 'undefined' ? window : globalThis);

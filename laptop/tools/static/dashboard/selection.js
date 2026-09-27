/*
 * laptop/tools/static/dashboard/selection.js
 *
 * One shared "what's selected" store so the map, the consensus
 * network view, the detection timeline and the team/incident tables
 * can all highlight the same thing without four separate copies of
 * click-handling logic. index.html's inline script keeps its own
 * `selectedMarker` variable as the map's source of truth (the map's
 * click/drag code predates this file and other inline code reads
 * that variable directly) - this module is the broadcast layer so
 * every *other* panel finds out when it changes, and can push a
 * change back (e.g. clicking a consensus-network node selects that
 * team on the map too).
 *
 * Selection shape: null, or {type: "team"|"incident", id: <number>}.
 */
(function (global) {
  "use strict";

  let current = null;
  const listeners = [];

  function get() {
    return current;
  }

  function set(sel) {
    current = sel;
    for (const fn of listeners) {
      try {
        fn(current);
      } catch (e) {
        console.error("DashboardSelection listener failed:", e);
      }
    }
  }

  function subscribe(fn) {
    listeners.push(fn);
  }

  function isSelected(type, id) {
    return !!current && current.type === type && current.id === id;
  }

  global.DashboardSelection = { get: get, set: set, subscribe: subscribe, isSelected: isSelected };
})(window);

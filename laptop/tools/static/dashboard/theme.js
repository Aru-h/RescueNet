/*
 * laptop/tools/static/dashboard/theme.js
 *
 * Light/dark theme toggle. index.html's CSS defines the dark palette
 * on :root and a light override on :root[data-theme="light"] (same
 * variable names, so every existing rule that reads var(--bg) etc.
 * just works under either theme with no other CSS changes). This file
 * only flips the attribute and remembers the choice in localStorage
 * for next time the dashboard is opened - it never touches app state.
 */
(function (global) {
  "use strict";

  const KEY = "concord-dashboard-theme";

  function currentTheme() {
    return document.documentElement.getAttribute("data-theme") || "dark";
  }

  // Exactly one toggle button exists (in the merged topbar - a UI pass
  // removed the earlier duplicate that used to also sit in the page
  // header). Kept as a lookup rather than a cached reference so this
  // still works if the button is ever re-templated.
  function apply(theme) {
    document.documentElement.setAttribute("data-theme", theme);
    try { localStorage.setItem(KEY, theme); } catch (e) { /* private mode etc. - not fatal */ }
    const btn = document.getElementById("themeToggleBtn");
    if (btn) btn.textContent = theme === "light" ? "🌙 Dark" : "☀️ Light";
  }

  function toggle() {
    apply(currentTheme() === "dark" ? "light" : "dark");
  }

  function init() {
    let saved = "dark";
    try { saved = localStorage.getItem(KEY) || "dark"; } catch (e) { /* ignore */ }
    apply(saved);
  }

  global.DashboardTheme = { init, toggle };
})(window);

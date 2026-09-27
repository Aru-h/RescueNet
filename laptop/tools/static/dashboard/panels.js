/*
 * laptop/tools/static/dashboard/panels.js
 *
 * Small rendering helpers for the side panels (Team Status tier
 * badges + vote string, the Radio/vbus panel) that index.html's
 * inline updateLiveTables() calls into, rather than growing that
 * function's own HTML-building logic further. Kept dependency-free
 * (no camera, no canvas) since these only ever produce table cell
 * HTML strings or fill one small panel body.
 *
 * Every function here treats a missing/-1 consensus_tier or a missing
 * peer_votes/vbus object as "n/a", not as 0/NONE - board mission mode
 * frames (CONSENSUS_DESIGN.md 5.3) and any build without
 * VIRTUAL_BUS_ENABLED both look like this, and must never be drawn as
 * a false "all clear".
 */
(function (global) {
  "use strict";

  const TIER_LABEL = { "-1": "n/a", 0: "NONE", 1: "LOCAL", 2: "ELEVATED" };
  const TIER_CLASS = { "-1": "tier-na", 0: "tier-none", 1: "tier-local", 2: "tier-elevated" };

  function tierBadge(tier, className) {
    const key = (tier === undefined || tier === null) ? "-1" : String(tier);
    const label = TIER_LABEL[key] !== undefined ? TIER_LABEL[key] : "n/a";
    const cls = TIER_CLASS[key] || "tier-na";
    return `<span class="tier-badge ${cls}" title="${className || ""}">${label}</span>`;
  }

  function voteTriplet(v) {
    if (!v) return "n/a";
    return `${v[0]}F/${v[1]}C/${v[2]}A`;
  }

  function voteString(peerVotes) {
    if (!peerVotes) return "n/a";
    return `M ${voteTriplet(peerVotes.mob)} · C ${voteTriplet(peerVotes.comm)}`;
  }

  function teamConsensusCells(team) {
    const tier = team.consensus_tier;
    const mob = tier ? tier[0] : undefined;
    const comm = tier ? tier[1] : undefined;
    return {
      mobBadge: tierBadge(mob, "Mobility tier"),
      commBadge: tierBadge(comm, "Communication tier"),
      votes: voteString(team.peer_votes),
    };
  }

  function updateRadioPanel(elId, frame) {
    const el = document.getElementById(elId);
    if (!el) return;
    const v = frame && frame.vbus;
    if (!v || (v.sent === 0 && v.delivered === 0 && v.dropped === 0 && v.airtime_ms === 0 && frame.teams &&
               !frame.teams.some((t) => t.peer_votes))) {
      // Genuinely no bus data on this build/mode vs. a bus that simply
      // hasn't sent anything yet are hard to tell apart from zeros
      // alone; either way "n/a"/zero is honest, never fabricated.
    }
    if (!v) {
      el.innerHTML = '<span style="color:var(--muted);">n/a — this frame has no vbus field (board mission mode does not carry it yet)</span>';
      return;
    }
    const dropPct = v.sent ? ((v.dropped / v.sent) * 100).toFixed(1) : "0.0";
    el.innerHTML = `
      <div style="display:flex;gap:18px;flex-wrap:wrap;font-size:13px;">
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;">Sent</div><div style="font-size:20px;font-weight:700;">${v.sent}</div></div>
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;">Delivered</div><div style="font-size:20px;font-weight:700;color:var(--accent);">${v.delivered}</div></div>
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;">Dropped</div><div style="font-size:20px;font-weight:700;color:var(--bad);">${v.dropped}</div></div>
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;">Drop %</div><div style="font-size:20px;font-weight:700;">${dropPct}%</div></div>
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;">Airtime (cum.)</div><div style="font-size:20px;font-weight:700;">${v.airtime_ms} ms</div></div>
      </div>`;
  }

  global.DashboardPanels = { tierBadge, voteString, teamConsensusCells, updateRadioPanel };
})(window);

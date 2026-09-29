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
 * frames (docs/architecture/CONSENSUS_DESIGN.md 5.3) and any build without
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

  // Same numbers as voteString() but without the "F/C/A" letters - for
  // the Teams table's narrow Votes column, where the full spelled-out
  // form (used in the consensus network detail panel, which has more
  // room) would force the table wider than its panel. The letters are
  // still one hover away via the `title` attribute.
  function voteStringCompact(peerVotes) {
    if (!peerVotes) return "n/a";
    const m = peerVotes.mob, c = peerVotes.comm;
    const tri = (v) => v ? `${v[0]}/${v[1]}/${v[2]}` : "n/a";
    return `M ${tri(m)} · C ${tri(c)}`;
  }

  function teamConsensusCells(team) {
    const tier = team.consensus_tier;
    const mob = tier ? tier[0] : undefined;
    const comm = tier ? tier[1] : undefined;
    return {
      mobBadge: tierBadge(mob, "Mobility tier"),
      commBadge: tierBadge(comm, "Communication tier"),
      votes: voteString(team.peer_votes),
      votesCompact: voteStringCompact(team.peer_votes),
    };
  }

  // Per-receiver loss: dropped / (delivered + dropped), NOT
  // dropped / sent. `sent` counts broadcasts (one send reaches up to 9
  // receivers); `delivered`/`dropped` are per-receiver outcomes of
  // those broadcasts (vbus_get_stats()). Dividing dropped by sent
  // understates loss by roughly the fan-out factor. Returns null (not
  // 0) when there is nothing to compute a rate from yet, so callers
  // can render "n/a"/"–" instead of a misleading 0%.
  function radioLossPct(frame) {
    const v = frame && frame.vbus;
    if (!v) return null;
    const denom = v.delivered + v.dropped;
    if (denom <= 0) return null;
    return (v.dropped / denom) * 100;
  }

  // One tick = 36 seconds of mission time (the same constant the
  // board-mission "real time" pace option in index.html uses) - the
  // only place in this file a wall-clock assumption is made, and it's
  // only for this one derived, clearly-labeled stat.
  const TICK_DURATION_MS = 36000;

  // "Channel busy %": average radio airtime per tick, as a fraction of
  // how long a tick actually represents (36 s of mission time) - i.e.
  // "of each tick's real duration, how much of it was the radio
  // actually transmitting". airtime_ms is cumulative, so this is a
  // whole-run average, not an instantaneous reading; both facts are
  // spelled out in the panel's own label/title for a non-expert
  // reader, not just in this comment. Returns null when there's no
  // tick to average over yet.
  function channelBusyPct(frame) {
    const v = frame && frame.vbus;
    if (!v || !frame || !(frame.tick >= 0)) return null;
    const ticksElapsed = frame.tick + 1;
    return (v.airtime_ms / ticksElapsed / TICK_DURATION_MS) * 100;
  }

  function updateRadioPanel(elId, frame) {
    const el = document.getElementById(elId);
    if (!el) return;
    const v = frame && frame.vbus;
    if (!v) {
      el.innerHTML = '<span style="color:var(--muted);">n/a — this frame has no vbus field (board mission mode does not carry it yet)</span>';
      return;
    }
    const lossPct = radioLossPct(frame);
    const lossText = lossPct === null ? "–" : lossPct.toFixed(1) + "%";
    const busyPct = channelBusyPct(frame);
    const busyText = busyPct === null ? "–" : busyPct.toFixed(2) + "%";
    el.innerHTML = `
      <div style="display:flex;gap:18px;flex-wrap:wrap;font-size:13px;">
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;">Sent</div><div style="font-size:20px;font-weight:700;">${v.sent}</div></div>
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;">Delivered</div><div style="font-size:20px;font-weight:700;color:var(--accent);">${v.delivered}</div></div>
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;">Dropped</div><div style="font-size:20px;font-weight:700;color:var(--bad);">${v.dropped}</div></div>
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;" title="dropped / (delivered + dropped) - per-receiver outcomes, not per-broadcast">Per-receiver loss</div><div style="font-size:20px;font-weight:700;">${lossText}</div></div>
        <div><div class="label" style="font-size:11px;color:var(--muted);text-transform:uppercase;" title="Average radio airtime per tick, as a share of the 36s a tick represents in mission time - a whole-run average, not instantaneous">Channel busy %</div><div style="font-size:20px;font-weight:700;">${busyText}</div></div>
      </div>`;
  }

  // The compact topbar's tick/resolved/radio-loss stats (layout
  // rework: these used to live in the now-removed #liveStatsRow line
  // and the map's own tick label). `simTelemetry` is the inline
  // script's running-totals object (cumulative.incidents_resolved) -
  // passed in rather than imported so this file stays framework-free.
  function updateTopBarStats(tickElId, resolvedElId, lossElId, frame, simTelemetry) {
    const tickEl = document.getElementById(tickElId);
    const resolvedEl = document.getElementById(resolvedElId);
    const lossEl = document.getElementById(lossElId);
    if (tickEl) tickEl.textContent = (frame && frame.tick !== undefined) ? String(frame.tick) : "–";
    if (resolvedEl) {
      const r = simTelemetry && simTelemetry.cumulative ? simTelemetry.cumulative.incidents_resolved : undefined;
      resolvedEl.textContent = (r === undefined) ? "–" : String(r);
    }
    if (lossEl) {
      const pct = radioLossPct(frame);
      lossEl.textContent = pct === null ? "n/a" : pct.toFixed(1) + "%";
    }
  }

  global.DashboardPanels = {
    tierBadge, voteString, teamConsensusCells, updateRadioPanel, radioLossPct, channelBusyPct, updateTopBarStats,
  };
})(window);

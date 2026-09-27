/*
 * laptop/tools/static/dashboard/consensus-network.js
 *
 * The consensus network panel: the 10 teams drawn as a ring of nodes,
 * each showing its mobility/communication tier and fast-path flag,
 * with spokes to a center hub whose brightness follows how much of
 * that team's peer bus traffic is currently getting through.
 *
 * Honesty note (see the task's "anything you couldn't show honestly"
 * requirement): LIVEFRAME's peer_votes gives each team's aggregate
 * live/dark/fast counts (how many of the 9 peers currently have an
 * opinion), not which SPECIFIC peer-to-peer beacons were delivered
 * this tick. There is no "team A's beacon to team B arrived" bit
 * anywhere in the frame, so this view cannot honestly draw individual
 * A-to-B edges - a full 10-node mesh with invented per-edge state
 * would be showing data that doesn't exist. What it draws instead is
 * one spoke per team (hub-and-spoke, not mesh), pulsed/faded by that
 * team's OWN live fraction (live / (live + dark)) - real aggregate
 * data, honestly labeled. See CONSENSUS_DESIGN.md 2.6 for the
 * heard-matrix (per-reporter heard_bits) computed on the board side;
 * exposing that per team in LIVEFRAME (or the raw delivered-pairs list
 * behind vbus's per-tick delivery) is exactly the field that would let
 * a future revision draw real per-edge A-to-B state instead of this
 * aggregate proxy - flagged for Part 2+ review, not invented here.
 *
 * consensus_tier is [-1, -1] (not built) whenever the sim wasn't
 * compiled with VIRTUAL_BUS_ENABLED, and the whole field is absent in
 * board mission mode (CONSENSUS_DESIGN.md 5.3's "Not in v1" note).
 * Both cases render as "n/a", never a crash and never a fabricated
 * NONE/grey tier.
 */
(function (global) {
  "use strict";

  const TIER_COLOR = { "-1": "#3a4552", 0: "#8b98a5", 1: "#d29922", 2: "#f85149" };
  const TIER_NAME = { "-1": "n/a", 0: "NONE", 1: "LOCAL", 2: "ELEVATED" };
  const RENDER_INTERVAL_MS = 75; // matches the throttle renderLiveCharts() already uses

  function create() {
    let canvas = null, ctx = null, detailEl = null;
    let nodes = []; // {id, cx, cy, r} in canvas CSS px, set on layout
    let lastFrameTeams = null;
    let pulse = new Array(10).fill(0); // per-team pulse energy, decays each render
    let lastRenderMs = 0;
    let selected = null; // team id or null
    let unsubscribe = null;

    function layout() {
      if (!canvas) return;
      const dims = DashboardCamera.resizeCanvasForDPI(canvas);
      ctx = dims.ctx;
      const w = dims.width, h = dims.height;
      const cx = w / 2, cy = h / 2;
      const ringR = Math.max(30, Math.min(w, h) / 2 - 34);
      const n = (lastFrameTeams && lastFrameTeams.length) || 10;
      nodes = [];
      for (let i = 0; i < n; i++) {
        const angle = (i / n) * Math.PI * 2 - Math.PI / 2;
        nodes.push({
          id: i,
          cx: cx + Math.cos(angle) * ringR,
          cy: cy + Math.sin(angle) * ringR,
          r: Math.max(10, Math.min(w, h) * 0.032),
          hubCx: cx, hubCy: cy,
        });
      }
      return { w, h, cx, cy };
    }

    function tierOf(team, cls) {
      if (!team || !team.consensus_tier) return -1;
      const v = team.consensus_tier[cls];
      return (v === undefined || v === null) ? -1 : v;
    }

    function draw() {
      if (!ctx || !lastFrameTeams) return;
      const w = canvas.getBoundingClientRect().width;
      const h = canvas.getBoundingClientRect().height;
      ctx.clearRect(0, 0, w, h);
      if (!nodes.length || nodes.length !== lastFrameTeams.length) layout();

      const hub = nodes[0] ? { x: nodes[0].hubCx, y: nodes[0].hubCy } : { x: w / 2, y: h / 2 };

      // Spokes first (under the nodes).
      lastFrameTeams.forEach((team, i) => {
        const node = nodes[i];
        if (!node) return;
        const pv = team.peer_votes;
        const live = pv ? pv.live : 0;
        const dark = pv ? pv.dark : 0;
        const total = live + dark;
        const liveFrac = total > 0 ? live / total : (pv ? 0 : null); // null = no consensus data at all
        const energy = pulse[i] || 0;

        if (liveFrac === null) {
          ctx.save();
          ctx.strokeStyle = "rgba(139,152,165,0.15)";
          ctx.lineWidth = 1;
          ctx.setLineDash([2, 4]);
          ctx.beginPath();
          ctx.moveTo(hub.x, hub.y);
          ctx.lineTo(node.cx, node.cy);
          ctx.stroke();
          ctx.restore();
          return;
        }

        const baseAlpha = 0.08 + liveFrac * 0.22;
        const pulseAlpha = energy * 0.55;
        const alpha = Math.min(1, baseAlpha + pulseAlpha);
        const fast = pv && pv.fast;
        ctx.save();
        ctx.strokeStyle = fast ? `rgba(248,81,73,${alpha})` : `rgba(88,166,255,${alpha})`;
        ctx.lineWidth = 1 + energy * 2.5;
        if (liveFrac < 0.4) ctx.setLineDash([4, 3]); // "fading" for mostly-missed
        ctx.beginPath();
        ctx.moveTo(hub.x, hub.y);
        ctx.lineTo(node.cx, node.cy);
        ctx.stroke();
        ctx.restore();

        pulse[i] = energy * 0.82; // decay toward the next real update
      });

      // Hub.
      ctx.beginPath();
      ctx.arc(hub.x, hub.y, 5, 0, Math.PI * 2);
      ctx.fillStyle = "rgba(139,152,165,0.5)";
      ctx.fill();

      // Nodes.
      lastFrameTeams.forEach((team, i) => {
        const node = nodes[i];
        if (!node) return;
        const mob = tierOf(team, 0);
        const comm = tierOf(team, 1);
        const fast = team.peer_votes && team.peer_votes.fast;

        ctx.beginPath();
        ctx.arc(node.cx, node.cy, node.r, 0, Math.PI * 2);
        ctx.fillStyle = TIER_COLOR[String(mob)] || TIER_COLOR["-1"];
        ctx.fill();

        ctx.beginPath();
        ctx.arc(node.cx, node.cy, node.r + 3, 0, Math.PI * 2);
        ctx.strokeStyle = TIER_COLOR[String(comm)] || TIER_COLOR["-1"];
        ctx.lineWidth = 3;
        ctx.stroke();

        if (fast) {
          ctx.fillStyle = "#f0883e";
          ctx.font = `bold ${Math.max(9, node.r)}px sans-serif`;
          ctx.textAlign = "center";
          ctx.textBaseline = "middle";
          ctx.fillText("⚡", node.cx + node.r + 8, node.cy - node.r - 4);
        }

        ctx.fillStyle = "#e6edf3";
        ctx.font = `bold ${Math.max(9, node.r * 0.9)}px sans-serif`;
        ctx.textAlign = "center";
        ctx.textBaseline = "middle";
        ctx.fillText(String(team.id !== undefined ? team.id : i), node.cx, node.cy + 0.5);

        if (selected === (team.id !== undefined ? team.id : i)) {
          ctx.beginPath();
          ctx.arc(node.cx, node.cy, node.r + 7, 0, Math.PI * 2);
          ctx.strokeStyle = "#fff";
          ctx.lineWidth = 2;
          ctx.stroke();
        }
      });
    }

    function voteStr(counts) {
      if (!counts) return "n/a";
      return `${counts[0]}F/${counts[1]}C/${counts[2]}A`;
    }

    function showDetail(teamIdx) {
      if (!detailEl) return;
      const team = lastFrameTeams && lastFrameTeams[teamIdx];
      if (!team) { detailEl.innerHTML = '<span style="color:var(--muted);">click a node to see its vote breakdown</span>'; return; }
      const mobTier = TIER_NAME[String(tierOf(team, 0))];
      const commTier = TIER_NAME[String(tierOf(team, 1))];
      const pv = team.peer_votes;
      if (!pv) {
        detailEl.innerHTML = `<b>Team ${team.id}</b> — consensus n/a for this frame (not built, or this run isn't sending it - e.g. board mission mode).`;
        return;
      }
      detailEl.innerHTML = `
        <b>Team ${team.id}</b> &nbsp;
        <span class="tier-badge" style="background:${TIER_COLOR[String(tierOf(team,0))]}22;color:${TIER_COLOR[String(tierOf(team,0))]};border:1px solid ${TIER_COLOR[String(tierOf(team,0))]};">Mobility ${mobTier}</span>
        <span class="tier-badge" style="background:${TIER_COLOR[String(tierOf(team,1))]}22;color:${TIER_COLOR[String(tierOf(team,1))]};border:1px solid ${TIER_COLOR[String(tierOf(team,1))]};">Comm ${commTier}</span>
        ${pv.fast ? '<span class="tier-badge" style="background:#f0883e22;color:#f0883e;border:1px solid #f0883e;">fast-path</span>' : ""}
        <br>
        <span style="font-family:monospace;">M ${voteStr(pv.mob)} &nbsp;·&nbsp; C ${voteStr(pv.comm)}</span><br>
        <span style="color:var(--muted);font-size:11.5px;">live peers: ${pv.live} &nbsp; dark peers: ${pv.dark}</span>`;
    }

    function handleClick(evt) {
      if (!nodes.length) return;
      const rect = canvas.getBoundingClientRect();
      const px = evt.clientX - rect.left, py = evt.clientY - rect.top;
      let best = null, bestDist = Infinity;
      nodes.forEach((n) => {
        const d = Math.hypot(n.cx - px, n.cy - py);
        if (d <= n.r + 6 && d < bestDist) { best = n; bestDist = d; }
      });
      if (best) {
        selected = best.id;
        showDetail(best.id);
        draw();
        if (global.DashboardSelection) global.DashboardSelection.set({ type: "team", id: best.id });
      }
    }

    function init(canvasEl, detailElArg) {
      canvas = canvasEl;
      detailEl = detailElArg;
      layout();
      canvas.addEventListener("click", handleClick);
      // ResizeObserver (not a plain window "resize" listener) so this
      // panel also picks up the very first real size it gets when the
      // dashboard screen goes from display:none to visible - a window
      // resize event never fires for that transition, only a
      // content-rect change, which ResizeObserver does report.
      if (window.ResizeObserver) {
        new ResizeObserver(() => { layout(); draw(); }).observe(canvas);
      } else {
        window.addEventListener("resize", () => { layout(); draw(); });
      }
      if (global.DashboardSelection) {
        unsubscribe = global.DashboardSelection.subscribe((sel) => {
          const newSel = (sel && sel.type === "team") ? sel.id : null;
          if (newSel !== selected) {
            selected = newSel;
            if (selected !== null) showDetail(selected);
            draw();
          }
        });
      }
      showDetail(null);
    }

    function update(frame) {
      if (!frame || !frame.teams) return;
      lastFrameTeams = frame.teams;
      lastFrameTeams.forEach((team, i) => {
        const pv = team.peer_votes;
        const total = pv ? pv.live + pv.dark : 0;
        pulse[i] = total > 0 ? Math.max(pulse[i] || 0, pv.live / total) : 0;
      });
      if (selected !== null) showDetail(selected);
      const now = performance.now();
      if (now - lastRenderMs >= RENDER_INTERVAL_MS) {
        draw();
        lastRenderMs = now;
      }
    }

    function reset() {
      lastFrameTeams = null;
      pulse = new Array(10).fill(0);
      selected = null;
      if (ctx && canvas) ctx.clearRect(0, 0, canvas.getBoundingClientRect().width, canvas.getBoundingClientRect().height);
      showDetail(null);
    }

    return { init, update, reset, layout: () => { layout(); draw(); } };
  }

  global.ConsensusNetwork = create();
})(window);

/*
 * laptop/tools/static/dashboard/consensus-network.js
 *
 * The consensus network panel: the 10 teams drawn as a ring of nodes,
 * each showing its mobility/communication tier and fast-path flag,
 * with a real 10x10 mesh of edges between every pair, pulsed for a
 * delivered beacon and faded for a missed one.
 *
 * Part 1 originally built this as a hub-and-spoke proxy (one spoke per
 * team, pulsed by that team's own live/dark fraction) because LIVEFRAME
 * only carried each team's AGGREGATE live/dark/fast counts, not which
 * specific peer-to-peer beacons were delivered - see CONSENSUS_
 * DESIGN.md 2.6's heard-matrix note. Part 3 added exactly that field:
 * each team's LIVEFRAME entry now carries `heard`, a 10-bit mask (bit j
 * set = this team, as observer, received target j's beacon on time
 * this tick - PeerObservation_t.rx_on_time via consensus_get_peer_
 * observation(), laptop/debug.c). This file now draws the real mesh
 * from that field instead of the old aggregate proxy.
 *
 * One honesty note that still applies: `heard` is directional (i heard
 * j is not necessarily the same as j heard i - radio conditions aren't
 * always symmetric), but drawing 90 directed edges on a 10-node ring
 * reads as visual noise rather than a clear picture. Each UNDIRECTED
 * pair (i,j) is drawn as delivered this tick if EITHER direction was
 * heard (i heard j OR j heard i) - a deliberate simplification of real,
 * per-tick data, not fabricated data; the per-team vote detail panel
 * (showDetail() below) still shows the literal numbers.
 *
 * consensus_tier is [-1, -1] (not built) whenever the sim wasn't
 * compiled with VIRTUAL_BUS_ENABLED, and the whole field (including
 * `heard`) is absent in board mission mode (CONSENSUS_DESIGN.md 5.3's
 * "Not in v1" note). Both cases render as "n/a" / no edges, never a
 * crash and never a fabricated NONE/grey tier or a fabricated edge.
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
    // Per-UNDIRECTED-pair pulse energy, keyed "i-j" (i<j) - decays each
    // render toward 0, same idea as the old per-team pulse array.
    let edgePulse = {};
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

      // Real mesh: one line per undirected team pair (i<j), under the
      // nodes. `heard` is missing entirely (undefined) whenever
      // consensus isn't built/absent from this frame (see file header) -
      // that pair draws as a faint dashed "no data" line, same visual
      // language the old hub-and-spoke proxy used for that case.
      const n = lastFrameTeams.length;
      for (let i = 0; i < n; i++) {
        const teamI = lastFrameTeams[i];
        const nodeI = nodes[i];
        if (!nodeI) continue;
        for (let j = i + 1; j < n; j++) {
          const teamJ = lastFrameTeams[j];
          const nodeJ = nodes[j];
          if (!nodeJ) continue;
          const key = `${i}-${j}`;
          const hasData = typeof teamI.heard === "number" && typeof teamJ.heard === "number";
          const energy = edgePulse[key] || 0;

          if (!hasData) {
            ctx.save();
            ctx.strokeStyle = "rgba(139,152,165,0.10)";
            ctx.lineWidth = 1;
            ctx.setLineDash([2, 4]);
            ctx.beginPath();
            ctx.moveTo(nodeI.cx, nodeI.cy);
            ctx.lineTo(nodeJ.cx, nodeJ.cy);
            ctx.stroke();
            ctx.restore();
            continue;
          }

          const delivered = !!((teamI.heard & (1 << j)) || (teamJ.heard & (1 << i)));
          const baseAlpha = delivered ? 0.16 : 0.04;
          const alpha = Math.min(1, baseAlpha + energy * 0.6);
          ctx.save();
          ctx.strokeStyle = `rgba(88,166,255,${alpha})`;
          ctx.lineWidth = 1 + energy * 2.2;
          if (!delivered) ctx.setLineDash([4, 3]); // fading for a missed pair this tick
          ctx.beginPath();
          ctx.moveTo(nodeI.cx, nodeI.cy);
          ctx.lineTo(nodeJ.cx, nodeJ.cy);
          ctx.stroke();
          ctx.restore();
        }
      }

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
      const n = lastFrameTeams.length;
      for (let i = 0; i < n; i++) {
        for (let j = i + 1; j < n; j++) {
          const teamI = lastFrameTeams[i], teamJ = lastFrameTeams[j];
          const hasData = typeof teamI.heard === "number" && typeof teamJ.heard === "number";
          const delivered = hasData && !!((teamI.heard & (1 << j)) || (teamJ.heard & (1 << i)));
          const key = `${i}-${j}`;
          edgePulse[key] = delivered ? 1 : (edgePulse[key] || 0) * 0.82; // pulse on delivery, decay otherwise
        }
      }
      if (selected !== null) showDetail(selected);
      const now = performance.now();
      if (now - lastRenderMs >= RENDER_INTERVAL_MS) {
        draw();
        lastRenderMs = now;
      }
    }

    function reset() {
      lastFrameTeams = null;
      edgePulse = {};
      selected = null;
      if (ctx && canvas) ctx.clearRect(0, 0, canvas.getBoundingClientRect().width, canvas.getBoundingClientRect().height);
      showDetail(null);
    }

    return { init, update, reset, layout: () => { layout(); draw(); } };
  }

  global.ConsensusNetwork = create();
})(window);

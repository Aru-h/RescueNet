# Headroom map (Step 5)

**Status: in progress.** Pre-registration: `docs/HEADROOM_MAP_PREREG.md` (commits `1a3a3d3`, `6a08143`, `b94ded0`,
`c577ce7`). Scale audit: `docs/SCALE_AUDIT.md`. Results tables are appended tier by tier as each tier finishes; the
plain-language summary goes at the top when the map is complete. This version holds the parts that need no results: the
description of the road-knowledge propagation model and its design note (Addendum C), written while the runs are
queued.

## A. Road-knowledge propagation: the model as implemented (R1-R3 and Addendum C)

Source: `core/road_knowledge.c`, `core/road_knowledge.h`. Hooks: `core/path.c` (`get_movement_cost`, `can_traverse_cell`
ask the planning view), `core/causal_actions.c` (`causal_actions_crossing_stall` stops a team whose planned step enters
a truly blocked cell; the action layer sees only fleet-known damage), `core/sim_runner.c` (once per tick).

* **One shared table**, not a per-team map. `s_belief[x][y]` holds the believed kind (clear, partial debris, blocked) and
  the tick it was learned. The pathfinder, the CBBA bids, the reachability checks and the rest-zone choice all read it
  (the pathfinder has no team argument, so a per-team picture is not possible without a core change).
* **Observation.** Every tick each team observes the cells within Chebyshev distance `radius` of itself (default 1: its
  own cell and 8 neighbours; the strict variant R1s uses 0). A team that plans a step into a truly blocked cell is
  stopped there and observes it that tick; a team that steps onto unknown partial debris observes it on entry.
* **Report.** If the observed kind differs from the table and no report of that kind is pending, a report is made. It is
  **lost** if the observing team has an active communication-class fault (this flag is the only link to the comms layer)
  or with probability `loss` (default 0.05, the consensus bus default of decision D4; a hash of seed, tick, team and
  cell, never a shared RNG stream). Otherwise it is **delivered at tick + `latency`** (default 2) to every team at once
  (due-tick delivery; one pending slot per cell). R3 sets latency 0, loss 0 and lets communication-faulted teams report.
* **Belief.** The table is overwritten by later reports of either kind (damage or clear), and a believed damage that
  nobody re-observes expires after **2400 ticks** (the longest unattended damage lasts). Stale beliefs (a cell believed
  damaged that has cleared) are therefore possible and counted (`stale_cell_ticks`).
* **R2 prior.** Unknown cells are planned at base cost plus `round(pi x 12)` where `pi` comes from the Omori rate of the
  last 1500 ticks spread over the grid; in this simulator it rounds to 0 to 2 cost units and carries no spatial
  information, because the causal model places damage at hashed epicentres on teams' routes (no intensity or
  building-density structure).
* Laptop-only. The board build links stubs that always return the true map. It does not use `comms_transport`, `vbus`
  or any frame, and it does not change any wire format, enum or schema.

**What it does not model:** per-team maps; hop-by-hop relaying or any dependence on who can hear whom; distance- or
terrain-dependent delay; scouting (sending a team to look); aerial observation; smoke or terrain visibility; reports that
carry a timestamp and are merged by recency; and the road information that already travels on the mission link (see C.5).

Because everybody knows everything after `latency` ticks, **the value of road information measured with it is a lower
bound** (the real propagation is slower and lossier). The Addendum C sensitivity (latency 0-100, loss 0.05-0.5, radius
0-4) measures how much the lower bound moves; its result goes in section B when it has run.

## A2. The shared road picture understates ignorance

Road knowledge is one fleet-wide table in which everybody learns everything `latency` ticks after one team sees it
(section A). Real teams would learn more slowly, unevenly and with errors, so **the model understates how little the
fleet knows, and the measured "value of road information" (omniscient minus the shared-picture setting) is a lower bound**
on what road information is worth. The Addendum C sensitivity measures how much the lower bound moves with latency,
loss and radius.

## P. Provenance and integrity of the results

* **Binaries are frozen per tier** (`build/headroom/frozen/<name>/`) and the runner records the binary's md5 in every
  result and in the CSV. Tier 0 and Tier 1 use **B1** = the tree committed as `ac72fd1` (hooks for D, S, M, K, W, V, TM,
  DQ1 and the road-knowledge module, no recovery-repertoire code), `noai` md5 `63a3f3bfb95f`, `ai` md5 `a6bbe7c30172`,
  built with `-O2`. Results written before the runner recorded the hash are tagged B1 in the CSV because their file time
  is after the B1 build.
* **Build environment of each frozen binary** (so a rebuild can be checked later; all built on this machine, the
  same compiler, flags and OS for both). A rebuild on another machine is only valid if it reproduces the md5.

  | Name | Source commit | `noai` md5 | `ai` md5 | Used for |
  |---|---|---|---|---|
  | B1 | `ac72fd1` | `63a3f3bfb95f94e20ef919d2cadb1ebf` | `a6bbe7c30172177e486f4db63b03dbd1` | Tiers 0-3, Addendum C |
  | B2 | `98fd08c` | `7e33412a46a9...` (full md5 in `build/headroom/frozen/B2_98fd08c/BUILD.txt`) | `d40bd5d785b6...` | Tier 4 |

  Compiler `cc (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`; flags `-std=gnu11 -O2 -w -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS
  -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1 -Icore -Ilaptop` over `core/*.c laptop/main.c laptop/debug.c
  laptop/snapshot.c laptop/mission_feed.c laptop/mission_mirror.c -lm` (the `ai` binary adds `-DFAULT_MODEL_LOADED=1`);
  OS Ubuntu 24.04.4 LTS, kernel Linux 6.18.44-fc-v153 x86_64, glibc 2.39, Python 3.11.15 for the runner and analysis.
  The binaries and their `BUILD.txt` files are kept under `build/headroom/frozen/` (not committed: binaries).
* **Run policy (decided with you):** one machine, exactly 4 worker processes at a time and nothing else running; no
  second session; baselines already run on the same binary are reused (for example PD is also D2 at delay 0, R1 is the
  default of the Addendum C sensitivity), and baselines are re-run only when the binary differs (Tier 4 re-runs P and PD
  on B2); the pre-registered sample sizes are not reduced and there is no early stopping.
* **Checks done on the results written so far (Tier 0 baselines, before the Tier 1 sweep):** (1) run counts per
  condition: 100 per (stress, configuration) for P, F (both stresses) and the no-fault reference, seeds 1001-1100
  contiguous; (2) no duplicates (one file per seed, the `config` field of each file equals its directory); (3) no
  partial runs (no `.tmp` files, every file has incidents, victims and the time budget); (4) **no mixing of binaries:
  54 of the 100 stress-2 P results had been written by an earlier build** (before the W, V, K2, DQ1 and TM1 hooks
  existed). I re-ran 8 of them with B1 (8 of 8 identical in incidents, victims, spawned and the time budget), then
  deleted all 54 and re-ran them with B1, so every Tier 0 result comes from B1; (5) the queue that stalled for about 20
  minutes had run nothing, so no result was affected; (6) the 16 Step 4 reference runs (P, PD, PA, F, seeds 1001-1004,
  stress 2) reproduce the Step 4 numbers exactly with the -O2 build.
* **Full `make check-*` suite before Tier 4 (tree `98fd08c`, 53 targets run, board link targets excluded here and run
  separately).** 47 pass. Six failed to link, and I compared each against the frozen Step 4 tree `c7408be` in a separate
  worktree: **four fail identically at `c7408be`** (`check-consensus-selftest`, `check-fault-predictor-features-selftest`,
  `check-zone-parity`, `check-zone-refresh`: their stub link sets lack symbols added in Step 3.x such as
  `causal_actions_debris_held_at` and `fault_profile_causal_fix`; pre-existing, not touched here). **Two were caused by
  my hooks** (`check-realloc-gate`, `check-recovery-predictor-stress`: their stub link sets lack the new `probe_*`
  symbols); fixed in `laptop/tools/causal_link_stubs.inc` only (tools, no core change), both pass now. The ARM
  single-board compile, link and symbol checks pass (`check-board-single`), with the central allocator compiled out of
  the board build. The multi-board `check-board-link` is the known pre-existing failure (vendored BSP folder missing).
  Byte-identity of the default output against `c7408be`: six profiles, C1 md5 `b248bea72ce2ceb81389762dccc0a749`, and
  the causal variants with the planner (oracle and hazard), recovery actions on, and hold-repair: identical.
* **Per-run tables** are committed in `docs/data/headroom/` (one CSV per tier: stress, configuration, seed, incidents,
  victims lost and seen, spawned, priority-1 outcomes, teams in service, key counters, wall time, binary hash); the
  analysis scripts run from them. The D1 inputs (datasets of 30 seeds) are large and not committed; `detection_delay.py`
  regenerates the table from `headroom_run.py run --configs F --keep`.

## B0. Tier 0 results (read-only; 100 development seeds for the time budget, 30 for D1)

### B0.1 Time budget (TB)

Shares of team-ticks, mean over seeds 1001-1100, planner P and fixed gates F in the v1 world. The categories are
mutually exclusive in the priority order resting, heading to rest, held in a field repair, faulted (mobility or
resource fault active), on scene, en route to a task, available. "In service" = on scene + to task + available, in teams
out of 10. The no-fault reference has no probe diagnostics; the one-seed figures in `docs/SCALE_AUDIT.md` (resting 43.9,
en route 33.7, on scene 17.7, available 4.8 percent) stand in for it and match the brief.

| Condition | resting | to rest | held in repair | faulted | on scene | to task | available | in service (of 10) |
|---|---|---|---|---|---|---|---|---|
| P stress 2 | 49.8% | 10.4% | 0.0% | 3.2% | 9.5% | 24.5% | 2.6% | 3.66 |
| P stress 7 | 53.6% | 15.4% | 0.0% | 6.2% | 7.0% | 15.9% | 1.9% | 2.48 |
| F stress 2 | 49.1% | 10.4% | 0.9% | 2.5% | 9.9% | 24.1% | 3.3% | 3.72 |
| F stress 7 | 53.5% | 15.4% | 1.7% | 4.6% | 6.8% | 15.6% | 2.3% | 2.47 |

Per specialization (P, stress 2 / stress 7), in teams in service of 2: SEARCH 4.32 / 2.81 (per 10), MEDICAL 3.63 / 2.39,
EXTRACTION 3.90 / 2.70, PERIMETER 3.55 / 2.43, SUPPORT 2.91 / 2.08. The full per-specialization table is produced by
`headroom_report.py budget`.

Findings (descriptive):

* **About half of all team time is spent resting in every condition** (49-54 percent), and 10-15 percent more is spent
  travelling to rest: roughly 60 to 69 percent of the fleet's time is rest-related, against 7 to 10 percent on scene doing
  work. The stress-2 world already has fewer teams in service (3.7) than the no-fault world (about 4.8).
* The time-on-scene share falls from 17.7 percent (no faults) to 9.5 percent (stress 2) and 7.0 percent (stress 7).
  Travel to tasks (24.5 percent at stress 2) is two to three times the time spent working. Travel and rest, not work, is
  where the fleet's time goes, which is why the W (work) and V (travel) probes of Tier 2 matter.
* Held in a field repair is 0.0 percent under the planner (it almost never starts one, as in Step 3.6) and 0.9 and 1.7
  percent under the fixed gates at stress 2 and 7. Faulted-but-not-held time is 2-6 percent.
* SUPPORT teams spend the most time resting (60-62 percent) and the least in service.

### B0.2 Gates' detection delay and miss rate (D1)

Fixed-gate system F, stress 2 and 7, development seeds 1001-1030. An episode is one team's consecutive ticks with a
class's fault active; detected at the first tick of the episode at which the gates' logged recommendation is at least
LOCAL_RECOVERY (first row of each pair) or RETURN_TO_SAFE_ZONE (second row). The recommendation can come from another
evaluator, so the last column shows how often it was already set at the onset tick (0-6 percent, so attribution is
mostly clean).

| Stress | Class | Episodes | Mean length (ticks) | Level | Missed | Delay median [p10, p90] (ticks) | Detected within 5 / 10 / 40 ticks | Already recommended at onset |
|---|---|---|---|---|---|---|---|---|
| 2 | mobility | 1571 | 66 | >= LOCAL | 38% | 2 [1, 23] | 67% / 80% / 97% | 3% |
| 2 | mobility | 1571 | 66 | >= RETURN | 40% | 6 [3, 26] | 43% / 72% / 96% | 0% |
| 2 | communication | 3375 | 4 | >= LOCAL | 86% | 3 [0, 12] | 81% / 88% / 100% | 4% |
| 2 | communication | 3375 | 4 | >= RETURN | 93% | 7 [4, 22] | 36% / 74% / 100% | 0% |
| 2 | resource | 1255 | 41 | >= LOCAL | 63% | 11 [0, 57] | 32% / 50% / 81% | 6% |
| 2 | resource | 1255 | 41 | >= RETURN | 71% | 15 [2, 62] | 22% / 38% / 76% | 1% |
| 7 | mobility | 3385 | 70 | >= LOCAL | 43% | 2 [1, 23] | 66% / 80% / 97% | 1% |
| 7 | mobility | 3385 | 70 | >= RETURN | 44% | 6 [3, 27] | 45% / 73% / 96% | 0% |
| 7 | communication | 2292 | 4 | >= LOCAL | 85% | 3 [0, 12] | 73% / 87% / 100% | 2% |
| 7 | communication | 2292 | 4 | >= RETURN | 91% | 7 [4, 19] | 37% / 75% / 100% | 0% |
| 7 | resource | 3208 | 43 | >= LOCAL | 67% | 9 [0, 51] | 34% / 53% / 85% | 5% |
| 7 | resource | 3208 | 43 | >= RETURN | 74% | 13 [3, 55] | 21% / 39% / 83% | 1% |

Findings:

* **When the gates detect a mobility fault they do it fast: median 2 ticks (72 s), 97 percent within 40 ticks.** But
  **38-43 percent of mobility episodes are never flagged at all** before they end (mean length 66-70 ticks).
* **Communication faults are short (mean 4 ticks) and the gates miss 85-86 percent of them** (they act on a window of
  several ticks); the median delay is 3 ticks when they do fire.
* **Resource faults are the slowest and most often missed:** median 9-11 ticks, 63-67 percent never flagged.
* The brief's premise that "gates already confirm and act within a few ticks" holds for the faults they catch; the
  headroom for a better detector is the **missed episodes**, not the delay of the caught ones. Whether catching them
  matters is what D2/D3 and the detector-oracle contrasts of Tier 1 measure.

## B1a. Tier 1, information chunk (screen: 100 development seeds 1001-1100, stress 2 and 7)

Headroom = probe minus matched baseline (P, or P at the same dropout), paired by seed, bootstrap 95% CI (20000
resamples). Victims lost is shown as baseline minus probe (positive = fewer lost). Survival needs a CI lower bound above
0 **and** a point estimate of at least +1.0 incident or 1.0 victim. Per-run data: `docs/data/headroom/tier1a_information.csv`
(binary B1). Team-equivalents are blank until the K1 capacity runs (end of Tier 1) provide the yardstick.

| Probe | Stress | Incidents / seed [95% CI] | Team-equiv. | Victims lost / seed (positive = fewer) [95% CI] | Spawned | Survives |
|---|---|---|---|---|---|---|
| context: shipped AI on the planner (PA minus P) | 2 | -0.05 [-0.60, +0.50] |  | -0.09 [-0.82, +0.67] | -0.2 | (not a probe) |
| context: shipped AI on the planner (PA minus P) | 7 | +0.14 [-0.47, +0.73] |  | +0.07 [-0.81, +0.93] | -0.1 | (not a probe) |
| context: fixed gates minus planner (F minus P) | 2 | -0.26 [-0.92, +0.41] |  | -0.13 [-1.11, +0.85] | -0.2 | (not a probe) |
| context: fixed gates minus planner (F minus P) | 7 | -0.28 [-1.02, +0.48] |  | +0.07 [-1.13, +1.30] | -0.1 | (not a probe) |
| D2 detector oracle, no delay (PD) | 2 | +0.28 [-0.33, +0.89] |  | -0.30 [-1.28, +0.70] | +0.3 | no |
| D2 detector oracle, no delay (PD) | 7 | +0.01 [-0.84, +0.85] |  | -0.57 [-1.69, +0.58] | +0.4 | no |
| D2 detector oracle delayed 5 ticks | 2 | +0.21 [-0.44, +0.86] |  | -0.52 [-1.52, +0.50] | +0.2 | no |
| D2 detector oracle delayed 5 ticks | 7 | -0.07 [-0.88, +0.74] |  | -0.36 [-1.49, +0.77] | -0.0 | no |
| D2 detector oracle delayed 10 ticks | 2 | +0.25 [-0.41, +0.91] |  | -0.66 [-1.62, +0.32] | +0.2 | no |
| D2 detector oracle delayed 10 ticks | 7 | +0.32 [-0.50, +1.14] |  | +0.13 [-0.92, +1.20] | +0.6 | no |
| D2 detector oracle delayed 20 ticks | 2 | +0.05 [-0.69, +0.79] |  | -0.62 [-1.54, +0.31] | +0.2 | no |
| D2 detector oracle delayed 20 ticks | 7 | -0.27 [-1.09, +0.55] |  | -0.38 [-1.62, +0.86] | +0.2 | no |
| D2 detector oracle delayed 40 ticks | 2 | +0.06 [-0.67, +0.78] |  | -0.56 [-1.50, +0.39] | +0.0 | no |
| D2 detector oracle delayed 40 ticks | 7 | -0.53 [-1.36, +0.28] |  | -0.01 [-1.18, +1.16] | +0.0 | no |
| D3a oracle, 30% misses / 1% false alarms | 2 | +0.26 [-0.43, +0.95] |  | -0.46 [-1.48, +0.57] | +0.1 | no |
| D3a oracle, 30% misses / 1% false alarms | 7 | +0.01 [-0.72, +0.73] |  | -0.70 [-1.77, +0.38] | +0.3 | no |
| D3b baseline evidence dropout 25% (effect on P) | 2 | +0.33 [-0.42, +1.05] |  | +0.59 [-0.16, +1.38] | -0.1 | (not a probe) |
| D3b baseline evidence dropout 25% (effect on P) | 7 | -0.08 [-0.79, +0.64] |  | +0.16 [-0.79, +1.11] | +0.2 | (not a probe) |
| D3b oracle headroom at dropout 25% | 2 | -0.25 [-1.03, +0.53] |  | -1.46 [-2.37, -0.56] | +0.4 | no |
| D3b oracle headroom at dropout 25% | 7 | +0.05 [-0.81, +0.91] |  | +0.42 [-0.74, +1.60] | -0.2 | no |
| D3b shipped-AI gain at dropout 25% | 2 | -0.02 [-0.60, +0.57] |  | -0.15 [-0.74, +0.43] | -0.0 | no |
| D3b shipped-AI gain at dropout 25% | 7 | -0.02 [-0.62, +0.57] |  | +0.47 [-0.44, +1.38] | -0.4 | no |
| D3b baseline evidence dropout 50% (effect on P) | 2 | +0.10 [-0.52, +0.72] |  | +0.01 [-0.92, +0.96] | -0.1 | (not a probe) |
| D3b baseline evidence dropout 50% (effect on P) | 7 | -0.03 [-0.73, +0.67] |  | +0.17 [-0.81, +1.15] | -0.3 | (not a probe) |
| D3b oracle headroom at dropout 50% | 2 | +0.25 [-0.52, +1.02] |  | -0.50 [-1.48, +0.49] | +0.2 | no |
| D3b oracle headroom at dropout 50% | 7 | -0.48 [-1.19, +0.24] |  | -0.21 [-1.44, +1.07] | -0.0 | no |
| D3b shipped-AI gain at dropout 50% | 2 | +0.16 [-0.39, +0.69] |  | +0.47 [-0.29, +1.23] | -0.1 | no |
| D3b shipped-AI gain at dropout 50% | 7 | -0.11 [-0.76, +0.53] |  | +0.80 [-0.15, +1.75] | +0.1 | no |
| D3b baseline evidence dropout 75% (effect on P) | 2 | +0.27 [-0.44, +0.97] |  | -0.09 [-0.96, +0.78] | -0.0 | (not a probe) |
| D3b baseline evidence dropout 75% (effect on P) | 7 | +0.24 [-0.42, +0.91] |  | +0.14 [-0.84, +1.13] | +0.2 | (not a probe) |
| D3b oracle headroom at dropout 75% | 2 | -0.33 [-1.10, +0.44] |  | -0.38 [-1.20, +0.45] | -0.0 | no |
| D3b oracle headroom at dropout 75% | 7 | -0.15 [-0.96, +0.65] |  | -0.20 [-1.29, +0.82] | -0.1 | no |
| D3b shipped-AI gain at dropout 75% | 2 | -0.26 [-0.83, +0.30] |  | -0.19 [-0.98, +0.59] | -0.0 | no |
| D3b shipped-AI gain at dropout 75% | 7 | -0.14 [-0.73, +0.44] |  | +0.85 [-0.06, +1.74] | -0.4 | no |

**Result: nothing survives; every incidents CI includes 0.** (The one interval that excludes 0, "oracle headroom at
dropout 25%, stress 2, victims", is **negative**: the perfect detector lost 1.5 more victims than P there; it is not a
benefit and fails the screen.)

* **A perfect detector (PD) adds +0.28 [-0.33, +0.89] incidents at stress 2 and +0.01 [-0.84, +0.85] at stress 7.**
  Step 4 (40 pooled seeds) had measured +0.35 and **+1.75** [+0.47, +2.98]. The stress-7 figure **does not replicate on
  100 seeds**: with 2.5 times the seeds the estimate is 0. The same is true of the shipped AI (Step 4: +1.23 on the planner
  at stress 7; now +0.14 [-0.47, +0.73]). The Step 4 stress-7 headline was a small-sample result and should be
  withdrawn; the honest statement is that the headroom of detection is **indistinguishable from zero at both stresses**,
  with a 95% upper bound of +0.9 incidents (about 2 percent of throughput).
* **Detection delay does not matter** (D2): the oracle delayed 5, 10, 20 and 40 ticks is as good as, or as bad as, the
  undelayed one; all within +-0.5 incidents of P. Detection **earliness** has no measurable value here.
* **Realistic false alarms and misses do not matter** (D3a, 30 percent misses and 1 percent false alarms): +0.26 and
  +0.01, the same as the perfect oracle.
* **Dropping the baseline's evidence does not hurt it** (D3b): removing 25, 50 or 75 percent of the watchdog and twin
  evidence changes P's incidents by +0.1 to +0.3 (no effect). The hook works: go-home events at stress 7 fall from 181 per
  seed to 129 (-29 percent) at 75 percent dropout, with no change in outcome. So **a large share of the gates' go-home
  trips contributes nothing**, and the information channels the baseline reads are not what limits it. With dropout, the
  perfect oracle (-0.3 to +0.25) and the shipped AI (-0.3 to +0.2) are still worth nothing: the AI does not matter when
  the baseline sees less.
* **Context:** the shipped AI on the planner (PA minus P) is -0.05 and +0.14; the fixed gates are indistinguishable from
  the planner (-0.26, -0.28).

**Scorecard (Addendum B, section 10.5, and Addendum A).**

| Prediction | Outcome |
|---|---|
| D1 median delay above 10 ticks, miss rate above 30 percent for communication, resource slowest | Partly: the median delay is 2-3 ticks for mobility and communication (not above 10); communication missed 85-86 percent (held); resource slowest (held) |
| D2 headroom falls as the delay grows (Spearman -0.5 or lower at stress 7) | **Not testable**: headroom is zero at k = 0, so there is nothing to fall |
| D3a keeps at least half of the PD headroom at stress 7 | Vacuous: PD headroom is 0 |
| D3b P falls with dropout (about -0.5 at 25 percent, -2 at 75); oracle headroom and AI gain grow with dropout | **Not held**: P does not fall; oracle headroom and AI gain stay at 0 |
| Overall: no information probe survives at stress 2 | **Held**; and none survives at stress 7 either |

Caveat for all of these: they run on the **planner (P) as baseline**, whose own information use is the thing being
tested. Even so, the fixed gates and the planner are indistinguishable, and so are all oracle variants.

## B1b. Tier 1, supply, demand and decision chunk (screen: 100 development seeds; confirmation: 100 held-out seeds)

Same conventions as B1a. Per-run data: `docs/data/headroom/tier1b_supply_demand_decision.csv` (binary B1).

| Probe | Stress | Incidents / seed [95% CI] | Team-equiv. | Victims lost / seed (positive = fewer) [95% CI] | Spawned | Survives screen |
|---|---|---|---|---|---|---|
| S1 hazard forecast + prevention x1 (level ii) | 2 | +0.02 [-0.31, +0.35] |  | -0.12 [-0.42, +0.19] | -0.1 | no |
| S1 hazard forecast + prevention x1 (level ii) | 7 | -0.19 [-0.68, +0.30] |  | +0.21 [-0.47, +0.88] | -0.1 | no |
| S1 draw-aware forecast + prevention x1 (level i) | 2 | -0.17 [-0.73, +0.40] |  | +0.35 [-0.42, +1.16] | -0.2 | no |
| S1 draw-aware forecast + prevention x1 (level i) | 7 | -0.38 [-1.10, +0.33] |  | +0.18 [-0.70, +1.04] | +0.0 | no |
| S1 hazard forecast + prevention x0.5 (level ii) | 2 | +0.02 [-0.31, +0.35] |  | -0.12 [-0.42, +0.19] | -0.1 | no |
| S1 hazard forecast + prevention x0.5 (level ii) | 7 | -0.19 [-0.68, +0.30] |  | +0.21 [-0.47, +0.88] | -0.1 | no |
| S1 draw-aware forecast + prevention x0.5 (level i) | 2 | +0.37 [-0.28, +1.02] |  | -0.04 [-0.86, +0.79] | +0.0 | no |
| S1 draw-aware forecast + prevention x0.5 (level i) | 7 | -0.54 [-1.23, +0.16] |  | +0.09 [-0.88, +1.04] | -0.1 | no |
| S1 hazard forecast + prevention x0.25 (level ii) | 2 | +0.02 [-0.31, +0.35] |  | -0.12 [-0.42, +0.19] | -0.1 | no |
| S1 hazard forecast + prevention x0.25 (level ii) | 7 | -0.19 [-0.68, +0.30] |  | +0.21 [-0.47, +0.88] | -0.1 | no |
| S1 draw-aware forecast + prevention x0.25 (level i) | 2 | +0.20 [-0.41, +0.82] |  | +0.04 [-0.80, +0.90] | -0.0 | no |
| S1 draw-aware forecast + prevention x0.25 (level i) | 7 | -0.63 [-1.36, +0.09] |  | -0.12 [-1.09, +0.81] | -0.1 | no |
| M1 survival-rate urgency (level ii) | 2 | -0.19 [-0.80, +0.41] |  | +0.27 [-0.55, +1.07] | -0.1 | no |
| M1 survival-rate urgency (level ii) | 7 | -0.35 [-0.92, +0.23] |  | +0.41 [-0.45, +1.30] | -0.4 | no |
| M1 realised-death urgency (level i) | 2 | -0.32 [-0.86, +0.22] |  | -0.21 [-1.01, +0.59] | -0.3 | no |
| M1 realised-death urgency (level i) | 7 | -0.47 [-1.11, +0.16] |  | -0.05 [-1.03, +0.90] | -0.6 | no |
| M2 fire-spread foresight, 50 ticks | 2 | +0.04 [-0.65, +0.73] |  | -0.53 [-1.38, +0.32] | +0.1 | no |
| M2 fire-spread foresight, 50 ticks | 7 | -0.11 [-0.81, +0.58] |  | -0.11 [-1.19, +0.97] | -0.1 | no |
| M3 aftershock-collapse foresight, 50 ticks | 2 | -0.01 [-0.71, +0.68] |  | -0.06 [-0.93, +0.79] | +0.1 | no |
| M3 aftershock-collapse foresight, 50 ticks | 7 | -0.49 [-1.27, +0.28] |  | +0.55 [-0.46, +1.57] | -0.4 | no |
| M4 hazard-arrival foresight, 10 ticks | 2 | +0.52 [-0.04, +1.10] |  | +0.66 [-0.17, +1.51] | +0.1 | no |
| M4 hazard-arrival foresight, 10 ticks | 7 | -0.15 [-0.73, +0.43] |  | +0.22 [-0.64, +1.07] | +0.2 | no |
| M4 hazard-arrival foresight, 50 ticks | 2 | -0.15 [-0.76, +0.46] |  | -0.33 [-1.20, +0.54] | -0.0 | no |
| M4 hazard-arrival foresight, 50 ticks | 7 | +0.15 [-0.57, +0.88] |  | +0.65 [-0.48, +1.79] | +0.1 | no |
| M4 hazard-arrival foresight, 200 ticks | 2 | -0.18 [-0.79, +0.43] |  | -0.26 [-1.12, +0.60] | -0.1 | no |
| M4 hazard-arrival foresight, 200 ticks | 7 | +0.16 [-0.56, +0.89] |  | +0.70 [-0.43, +1.83] | +0.1 | no |
| M5 discovery oracle (no fog of war) | 2 | +1.89 [+1.15, +2.63] |  | +1.82 [+0.93, +2.71] | +0.1 | incidents, victims |
| M5 discovery oracle (no fog of war) | 7 | +3.23 [+2.41, +4.08] |  | +1.79 [+0.83, +2.73] | +0.3 | incidents, victims |
| DQ1 central optimal assignment | 2 | +1.23 [+0.65, +1.82] |  | +0.96 [+0.10, +1.82] | +0.8 | incidents |
| DQ1 central optimal assignment | 7 | +0.82 [+0.17, +1.47] |  | +1.51 [+0.51, +2.52] | +0.5 | victims |
| DQ2 perfect comms (no comm faults, no bus loss) | 2 | +0.70 [+0.10, +1.38] |  | +0.39 [-0.31, +1.10] | +0.1 | no |
| DQ2 perfect comms (no comm faults, no bus loss) | 7 | +0.54 [-0.01, +1.10] |  | +0.51 [-0.28, +1.31] | +0.2 | no |

### Confirmation (held-out seeds 3001-3100, Holm over the whole screened family, m = 112 at this point)

| Probe | Stress | Metric | Screen | Held-out [95% CI] | p | Holm p | Confirmed |
|---|---|---|---|---|---|---|---|
| M5 discovery oracle (no fog of war) | 2 | incidents | +1.89 [+1.15, +2.63] | +1.71 [+1.05, +2.40] | 0.0001 | 0.011 | yes |
| M5 discovery oracle (no fog of war) | 2 | victims | +1.82 [+0.93, +2.71] | +1.37 [+0.42, +2.34] | 0.0058 | 0.632 | no |
| M5 discovery oracle (no fog of war) | 7 | incidents | +3.23 [+2.41, +4.08] | +3.10 [+2.28, +3.95] | 0.0001 | 0.011 | yes |
| M5 discovery oracle (no fog of war) | 7 | victims | +1.79 [+0.83, +2.73] | +1.06 [-0.09, +2.23] | 0.0702 | 1.000 | no |
| DQ1 central optimal assignment | 2 | incidents | +1.23 [+0.65, +1.82] | +1.05 [+0.41, +1.70] | 0.0017 | 0.187 | no |
| DQ1 central optimal assignment | 7 | victims | +1.51 [+0.51, +2.52] | +1.30 [+0.35, +2.31] | 0.0087 | 0.940 | no |

**Result.**

* **M5, the discovery oracle (every incident revealed the tick it spawns), is the only confirmed headroom so far:
  +1.71 [+1.05, +2.40] incidents at stress 2 and +3.10 [+2.28, +3.95] at stress 7 on the held-out seeds, Holm-adjusted
  p = 0.011 for both.** That is 4 percent and 9 percent of throughput. It is not demand-driven (spawned incidents rise by
  only 0.1 and 0.3 per seed against gains of 1.9 and 3.2). The victims-lost gain (about 1.4 to 1.8 fewer per seed in the
  screen) has the right sign on the held-out seeds but does not pass the Holm correction. The Step 3 measurement that
  fog of war dominates the time to first response (spawn to discovery 226 ticks on average) agrees with this.
  **Realisable?** Not by a fault forecaster: it is search, reporting and sensing (more scouts, aerial survey, reports from
  the public, better coverage), i.e. information about *where incidents are*, not about *which team will fail*.
* **DQ1, the central optimal assignment (an upper bound on coordination, laptop-only), shows +1.05 [+0.41, +1.70]
  incidents at stress 2 and 1.30 fewer victims lost at stress 7 on the held-out seeds: the same sign with a CI that
  excludes 0, but it does not pass the Holm correction over this large family (adjusted p 0.19 and 0.94), so it is not
  confirmed.** At stress 2 the incident gain is also flagged demand-driven by the pre-registered rule (spawned
  incidents +0.8 against a gain of +1.2). Reading: the in-process auction (CBBA with bundles, already a converged
  greedy) is within about one incident of an optimal one-to-one assignment; better coordination is worth at most about
  that much.
* **Perfect communications (DQ2)** is worth +0.70 [+0.10, +1.38] and +0.54 [-0.01, +1.10]: positive but below the
  materiality threshold, so it does not survive.
* **Everything else is a null:** cheaper prevention (S1), the survival-based urgency rule (M1, both levels), and foresight
  of fire spread, collapse or hazard arrivals (M2, M3, M4 at 10, 50 and 200 ticks) are all within about +-0.7 incidents
  of 0 with CIs that include 0.
* **S1 at the hazard-rate level is a literal null, not a measurement of cheap prevention:** the three prevention
  prices give identical numbers because the in-field maintenance option (the only one whose price the probe scales) is
  **never chosen** at the hazard-rate forecast, even at a quarter of the cost (0 uses per seed against 9.3 rest-at-base
  preventions that the scaling does not touch). At the perfect-foresight level it is used (5.9 uses per seed at the
  full price, 24.9 at a quarter) and still gives no gain (-0.38 to -0.63 incidents at stress 7).

**Scorecard for this chunk.**

| Prediction (Addendum B, section 10.5) | Outcome |
|---|---|
| S1: no survivor at stress 2 | Held |
| M1 level ii: victims fall 0.5 to 2 at stress 7 | Not supported: +0.41 [-0.45, +1.30], CI includes 0 |
| M2, M3 under +0.5 | Held |
| M4: positive, rising to T = 50, up to +1.5 | Not held: null at every lead |
| M5: +1 to +4 incidents, victims down 1 to 4 | **Held** for incidents (confirmed); victims not confirmed |
| DQ1: CBBA within 1.0 incident of central | Borderline: +1.23 and +0.82 in the screen (above 1.0 at stress 2), not confirmed after Holm |
| DQ2: +0.5 to 2 at stress 7, about 0 at stress 2 | Magnitude held at stress 7 (+0.54, CI touches 0); at stress 2 it is +0.70, not about 0 |

M5 is also run at stress 1 and 3.5 on both seed sets (the pre-registered rule for confirmed survivors); that result follows
in B1c.

## C. Design note (no implementation): how a real road-knowledge version would propagate

### C.1 What is in the code today (budgets found, not assumed)

| Item | Value | Where |
|---|---|---|
| Consensus beacon | 12-byte header + 2 votes x 2 classes x 10 teams = **52 bytes** payload, one broadcast per team per tick | `core/consensus.h` `CONSENSUS_BEACON_SIZE`; sent in `consensus.c` (`vbus_send(... CONSENSUS_FRAME_BEACON ...)`) |
| Frame limit | payload at most **144 bytes**; wire = 7 header + payload + 21 trailer (16-byte tag, 4-byte counter, 1-byte checksum) = 172 bytes at most | `core/lora_frame.h` |
| Beacon on air | 80 bytes on the wire; SF7, BW 125 kHz, CR 4/5 gives about **144 ms**, 0.40 percent of a 36 s tick per team, 4 percent of the channel for 10 teams (computed, `vbus` charges the same formula) | `core/lora_radio_params.h`, `core/vbus.h` |
| Spare room in a beacon frame | 144 - 52 = **92 bytes**, i.e. 23 road reports of 4 bytes | computed |
| Full frame on air | 172 bytes gives about **277 ms**, 0.77 percent of a tick per team, 7.7 percent of the channel for 10 teams (computed) | |
| Bus | broadcast, IID loss default 5 percent, inbox 16 frames, **collisions not modelled** | `core/vbus.h` |
| Map to the boards today | the **true** map, one 37-byte row payload (40 bytes with magic and checksum) per grid row, sent at start, on any change in the row, and periodically | `core/mission_link.h` section 5c |
| Duty cycle | the code says the IN865 conditions are duty-cycle and power limited; the numeric limit is not in the repository (to verify; 1 percent is the usual figure) | `core/lora_radio_params.h` |
| State hash | **none found** (searched core/ and laptop/); the nearest equivalents are the dataset and `make check-evaluate-determinism` | |

### C.2 Route 1: piggyback road reports on the consensus beacon

* **Format.** Append `n` reports after the 52 bytes: `x` (6 bits), `y` (6 bits), `kind` (2 bits), `tick` (low 10 bits, ticks
  are 36 s so 10 bits is 10 hours, ample against the 2400-tick belief expiry once unwrapped by the receiver) = 3-4 bytes.
  A count byte and a version bump (`CONSENSUS_BEACON_VERSION` 2) make old receivers reject the frame rather than
  misread it; this is a **wire change** and so needs your decision.
* **Per-team map.** `kind` (2 bits) plus the learned tick (16 bits) per cell: 36 x 36 x 3 bytes = about **3.9 kB per team,
  39 kB for 10 teams** in RAM (the DK has far more), or 1.3 kB per team if the tick is stored at 8-bit resolution.
  Merge rule: **latest timestamp wins**, ties by team id (commutative, idempotent, so lost and duplicated beacons are
  harmless). A team re-sends the last few reports it received (gossip), so information spreads beyond one radio hop
  even though the bus is a single broadcast domain (here every team hears every beacon with probability 0.95, so the
  spread is the loss, not the range).
* **Budget.** Up to 23 reports per beacon fit with no new frame and no new airtime beyond the longer frame (144 ms to
  277 ms per team per tick, 4 to 7.7 percent of the channel). A world with 10-20 damaged cells per 3000 ticks needs
  far fewer than one report per beacon on average, so the typical beacon grows by a few bytes. The risk is the
  collision rate, which the bus does not model (pure ALOHA at 7.7 percent offered load loses about 14 percent of
  frames against the 5 percent assumed), and the duty-cycle limit (to verify).
* **Determinism.** Everything is a function of the received beacons, which are already deterministic (hash-based
  loss, ascending delivery order). The merge needs no randomness.
* **Code changes.** `core/consensus.c` and `consensus.h` (pack, unpack, static assert on size), a new per-team belief
  table, `core/path.c` (see C.4), `core/causal_actions.c` (the stop-and-observe hook is per team already), the board
  build (the table is static and fits).

### C.3 Route 2: a dedicated road-report message over `comms_transport`

* **Format.** A new frame type (the frame-type enum has room: 0-7 used, bit mask `COMMS_FRAME_TYPE_BIT`), payload
  `n x 4` bytes plus a header; 144 bytes holds 30 reports. Sent only when a team has something new (event driven), so
  the airtime is zero on quiet ticks, against 144 ms every tick for the beacon.
* **Cost.** One frame of 28 + 4 n bytes: 40 bytes on the wire for one report (about 82 ms, computed), a full frame
  277 ms. At 10-20 reports per seed this is under 0.1 percent of the channel. It needs an ACK or a periodic
  re-broadcast (as the mission-link map rows do) because a lost report is not retried by a beacon cadence.
* **Mission link.** The existing map rows carry the true map from the laptop to the boards. In a knowledge version
  the laptop must send the **believed** map to a board (or none, with boards building their own), which changes what the
  map-row frame means (**a semantic change with no format change**) and must be decided with the dashboard.
* **Cost of the route.** A new frame type is an enum and wire addition (your rule: stop and ask).

### C.4 Common requirements for either route

* **`path.c` team argument.** The pathfinder keeps file-scope statics and a cache keyed by (start, destination,
  equipment flags); a per-team view needs `team_id` threaded through `find_path_cost`, `find_next_path_step*` and every
  caller (CBBA bid, allocator reachability, `find_nearest_safe_zone`, the movement code, the aftershock route scan), and
  the path cache key must include the team (or the cache must be per team). That is a core change beyond a hook. The
  planning-view function already exists (`road_knowledge_planning_view`) and would take the team.
* **Per-team memory and merge cost.** 39 kB (or 13 kB) total; a merge per received report, trivial.
* **State hash / determinism.** No state hash exists to extend; the new table would be added to the dataset or sidecar
  (not the 75-feature contract) so the check targets can compare runs.
* **Mission-link wire format.** Unchanged for route 1 (it rides in the beacon, which is a vbus/consensus frame, not a
  mission-link frame). Route 2 adds a frame type. Either way the laptop's world authority must stop sending the true
  map to the boards for the belief to mean anything.
* **Board build.** `ENVIRONMENT_HAS_WORLD_STORAGE` is 0 on the board, so the truth side stays on the laptop; the belief
  table is plain static arrays and fits.
* **Dashboard.** `route.js` rebuilds a route from the true `STATICMAP`. To show what each team knows: emit the believed
  kind and learned tick per changed cell per team (change-only, expected tens of events per seed), a per-team belief
  revision counter, and observation events (team, cell, tick); render a "known vs true" overlay (cells the team plans
  through that are damaged but unknown; stale beliefs), and reconstruct a team's route from its own belief, not the
  global map. The map-render layer needs a team selector.

### C.5 Recommendation and the value that would justify it

**Route 1 (beacon piggyback)** if the sensitivity shows that road information is worth building: it adds no new frame
type, rides a message every team already sends and every team already hears, and gives the merge semantics (latest
timestamp wins) for free. Route 2 is cheaper on airtime but is a new wire message and needs ACK logic.

**What ceiling would justify building it.** The cost is a core change (the team argument in the pathfinder), a beacon
version bump, 39 kB of RAM, 4 to 8 percent more channel load and a dashboard layer. I would not build it for a value
under the materiality threshold of this map (+1.0 incident or -1.0 victim per seed, about 2.5 percent of throughput).
The R1 value (omniscient minus the shared-picture belief) is the upper limit of what any road-information system can add
in the **current** world; if the confirmed value at stress 2 (the realistic setting) is below that threshold, building
a real propagation model is not justified by this world, and the justification would have to come from a world with
more road damage than the v1 world produces (11-18 damaged cells per 3000 ticks at stress 7). The results of
section B decide this; they are not yet in.

# Known Limitations — `tron_snapshot_dataset.csv`

For anyone training on the CSV export. Four known imperfections in the current data, plus one thing that's *not* in the CSV at all.

## 1. M37 — cross-district coverage misattribution (still unfixed)

`search_zone_pick_frontier_cell()` / the passive-coverage tracking in `core/incident.c`'s `search_sweep_step()` computes a team's district once from the team's own current cell, then credits its full sweep radius to that one district. Near a district boundary, some of the covered cells actually belong to a *different* district — coverage gets credited to the wrong `district_id`.

What was fixed this session: the coverage **denominator** (`search_zone_coverage_percent()`) now excludes cells that are genuinely unreachable (boxed-in pockets), so a district's coverage % is no longer artificially capped below 100% by cells nothing can ever reach. That's a real improvement, but it's a different bug from the cross-district credit issue above — the misattribution itself was not touched.

**Net effect:** coverage % values are more honest than before, but are still **not fully correct for districts near a boundary** — some of a district's reported coverage may actually reflect a neighboring district's cells. Don't treat per-district coverage % as ground truth for boundary-adjacent districts.

## 2. Evacuation gateway placement changed during this session

Evacuation gateway positions were fixed twice today: first a connectivity fix (the 3 gateways were landing on non-distinct/degenerate cells), then a separate reachability-model fix (gateway candidates could be chosen on cells that weren't actually road-connected). Before either fix, gateway placement was broken or degenerate in **up to ~95% of generated worlds** (47 of 50 sampled seeds).

**Any CSV data generated before today's fixes has unreliable gateway placement.** Evacuation-related rows/features (gateway location, evacuee routing, anything downstream of `g_evacuation_gateways[]`) from pre-fix runs should not be reused — only regenerate from a build that includes both fixes.

## 3. `EVENT_TICK` is ~97% of all rows, by design

`EVENT_TICK` was added this session to give unconditional per-team, per-tick visibility — previously the CSV only logged a row when one of 11 discrete event types fired (`EVENT_NEW_INCIDENT`, `EVENT_TEAM_ASSIGNED`, `EVENT_TEAM_ON_SCENE`, `EVENT_STAGE_COMPLETE`, `EVENT_TEAM_REST`, `EVENT_TEAM_AVAILABLE`, `EVENT_INCIDENT_RESOLVED`, `EVENT_FAULT_INJECTED`, `EVENT_FAULT_RECOVERED`, `EVENT_WATCHDOG_ANOMALY_CHANGED`, `EVENT_RECOVERY_ACTION_CHANGED`). `EVENT_TICK` fires every tick for every team regardless, so it now dominates the row count (~97% of rows in a measured 50-seed/3000-tick run).

It's meant for coverage/presence checks — e.g. "was this team logged every tick, with no gaps." **Filter `event_type == EVENT_TICK` (12) out before any analysis that looks at event sequences or transitions between meaningful events** — leaving it in will drown out the signal in anything sequence-based.

## 4. `twin_communication_health` was the comm fault label before the leak fix

Before the ground-truth leakage fix (see CHANGELOG.md), `twin_communication_health` was set straight from the injected comm fault severity, so it equalled `fault_active_communication * fault_severity_communication` on every row. The watchdog's communication evidence channel (and the `evidence_communication` feature derived from it) was built on it too.

**CSV data generated before the fix leaks the comm label through these features.** Don't train on it: any comm-fault accuracy it produces is overstated. Regenerate from a build with the fix. `data_prep-2.py` now refuses a dataset where a feature is a near-exact copy of a label, so pre-fix data fails that check.

Mobility detection is now based on observed movement stalls, so a mobility fault that starts and ends while the team isn't travelling can't be seen at all. That is an honest limit, not a bug.

---

`fault_buffer`/`fault_comms` fields are not present in the CSV at all — that subsystem (`core/fault_buffer.c`, `core/fault_comms.c`) has no callers in any build (laptop sim, board firmware, or test harnesses), so there's no fault-broadcast-derived data to look for or expect.

---

# Known Limitations — `core/` (accepted findings from the docs/reports/AUDIT.md triage pass)

Different audience from the CSV section above: this is for anyone reading `core/` source, not consuming the dataset. These are LOW-severity items from `AUDIT.md` that were deliberately triaged as "leave as-is" — each is either protected by an invariant unlikely to break, purely theoretical given current constants, an explicit accepted tradeoff, or genuinely style-only. Not bugs to fix; documented here so nobody re-discovers and re-litigates them from scratch.

1. **`cbba_build_bundle`** (core/cbba.c) — `bundle_size` can advance past what `path_size` reflects when `path[]` fills up first; same underlying issue as `AUDIT.md` top-10 item #9's deliberately-unfixed "orphaned pin" behavior (needs a design decision on what happens when path caps out, not a mechanical fix). Tracked there, not duplicated here.
2. **`chacha20_rotl32`** (core/chacha20.c) — UB only on a rotation amount of 0 or ≥32; every call site uses fixed literals (16/12/8/7).
3. **`abs_i`** (core/environment.c) — UB only at `INT_MIN`; every caller passes small grid-coordinate deltas (max ~72).
4. **`district_infrastructure_density`** (core/environment.c) — computed into `DistrictRecord_t.critical_infrastructure_density`, which is dead data (never read elsewhere); cleanup opportunity, not a bug.
5. **`queue_hazard_event`** (core/environment.c) — silently drops a hazard notification when all `MAX_ENV_PENDING_HAZARDS` slots are full; an explicitly acknowledged bounded-queue tradeoff per its own comment.
6. **`next_road_id`** (core/environment.c) — 16-bit road-ID counter has no wraparound guard; world generation issues a few hundred IDs per run, nowhere near 65,535.
7. **`carve_repair_to_nearest_road`** (core/environment.c) — degenerate self-path fallback requires zero roads existing anywhere on the grid, which `stage_generate_roads()` always prevents by carving the arterial chain first.
8. **`apply_building_damage`** (core/environment.c) — footprint-window reconstruction is fragile against future changes to `place_building_rect()`, but already mitigated by a per-cell `building_id` mismatch check; no live bug today.
9. **`building_incident_score`** (core/environment.c) — no NULL check of its own, but both call sites only invoke it after `building_can_spawn_incident()` has already guaranteed non-NULL.
10. **`propagate_fire`** (core/environment.c) — its `building_id < MAX_ENV_BUILDINGS` guard is redundant (IDs are always in range by construction) but genuinely safe, not buggy.
11. **`lora_crypto_accept_counter`** (core/lora_crypto.c) — the actual floor-regression bug here was fixed via `AUDIT.md` top-10 item #10 (commit `7e733ab`); the residual "non-atomic, no real ISR context" note is the same class of finding as item #4 (documented future-risk, no live ISR anywhere in this codebase today).
12. **`sim_tick`** (core/sim_runner.c) — tick-counter wraparound requires ~4.29 billion ticks and self-heals within one tick regardless (`simulation_step()` re-syncs it).
13. **`mission_link_apply_incident`** (core/mission_link.c) — **not a tradeoff, a still-open defect, listed here so it doesn't get lost.** `spawned_by = 0u;` three lines above the `support_team` sentinel fix (`AUDIT.md` top-10 item #1, commit `5725725`) has the identical sentinel defect: team 0 is a real team id, not a "none" sentinel, so a locally-created incident's `spawned_by` can silently alias team 0 instead of meaning "no spawning team," with the same downstream risk via `complete_incident_stage`'s `EQUIPMENT_CACHE` completion path that `support_team` had. Flagged during the item #1 fix review but deliberately left unfixed as out of scope for that change. Fix is mechanical (initialize to `NO_ASSIGNMENT` like `support_team` now does) — this just hasn't been done yet.

---

# Known Limitations — `board/` (accepted findings from the docs/reports/AUDIT.md board audit pass)

Same audience as the `core/` section above: accepted findings from the board/-specific audit (`board/STM32N6570-DK/*.c`, `board/Raspberry_Pi_Pico/*.c`). Each is either an explicitly intentional stub, cosmetic-only, or a low-confidence/unverified concern with no demonstrated failure — not bugs to fix.

1. **`debug_trace`** (both boards' `dk_debug.c`/`debug.c`) — intentional no-op; documented as a stub with no on-device sink, not a bug.
2. **`sx126x_hal_reset`** (board/STM32N6570-DK/sx1262_hal.c) — no extra settle delay between NRST release and the first BUSY read. Low-confidence, unverified-against-datasheet-timing concern; no demonstrated failure.
3. **`board_led_init`** (board/Raspberry_Pi_Pico/main_board.c) — `gpio_set_pin()`/`gpio_set_val()` return values discarded. Cosmetic LED status indicator; consistent with this codebase's established unchecked-BSP-return convention elsewhere.
4. **`board_led_pulse`** (board/Raspberry_Pi_Pico/main_board.c) — same reasoning as `board_led_init` above.
5. **`fmt_fixed3`** (both boards' `dk_debug.c`/`debug.c`) — residual `tmp[12]`/`int32_t scaled` truncation risk for a value/decimals combination outside the stamina/CBBA-score range this function is actually called with today. Not reachable given current callers; same class as several already-accepted `core/` LOW items above.
6. **`sim_task`** (board/STM32N6570-DK/main_board.c) — two config-fragility issues, neither reachable with shipped defaults: (1) `BOARD_SEED + BOARD_SEED_COUNT` computed as `uint32_t` with no overflow guard — a build config with `BOARD_SEED` near `UINT32_MAX` would wrap the seed loop bound; (2) `tick % BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL` has no zero-guard — a build redefining that macro to `0` divides by zero on the first tick. Both require a non-default build config to trigger (shipped defaults are `1u`/`1u` and `50u`).

---

# Known Limitations — system behaviour, detection, dashboard and demo

Audience: anyone running, presenting or judging the single-board STM32N6570-DK build and the laptop dashboard. These are known behaviours and gaps, not open bugs, unless marked otherwise. Each one says what you will see and why.

## A. Simulation behaviour

1. **Stamina/rest can pull a team off an almost-finished task.** A team whose stamina runs out is sent to rest even if its current incident is nearly done. The incident goes back to `INC_OPEN`, unassigned, and is usually picked up later by a different team. Work already done is partly kept (`work_remaining` stays where it was instead of resetting). Reproducible example: seed 2, `--fault-profile=none`, team 1 on incident 0 is pulled off at tick 80 with `work_remaining = 5` of 137 and rests at tick 98; team 7 takes the incident at tick 92. This is a deliberate strictness of the stamina model, and it applies to every assignment, not only manual overrides. It was left unchanged because changing it would change the training dataset.

2. **All 10 teams run on one board.** The submission build (`DK_SINGLE_BOARD_FLAGS`) puts every team on one STM32N6570-DK, and the handshake coordinator defaults to 1 board. Team-to-team messages go over an in-memory virtual bus, not a radio link. The multi-board LoRa path still exists in the code but is not exercised by this build.

## B. Fault detection

3. **Environment faults cannot be detected from team telemetry.** An environment fault only adds travel cost to one cell on the team's next path, which leaves almost no trace in what a team reports. Rules, consensus and the AI all detect ~0% of them. For that reason, fault-detection scores are reported two ways: macro F1 over all 5 classes and over the 4 detectable classes (excluding environment). Quote both.

4. **Rule-based detection is uneven across fault classes.** Measured with `make evaluate` on training-range seeds at the time of writing: communication ~56%, mobility ~38%, resource ~12%, environment and mission ~0% of faults detected by the rules. Re-run `make evaluate` for current numbers; don't rely on these figures after the code changes.

5. **Peer consensus adds robustness, not accuracy.** In `make evaluate`, consensus is never the first evaluator to flag a fault, and communication detection is slightly lower with consensus on. Consensus is valuable as a cross-check that one team's faulty self-report can't hide (and that a single faulty peer can't fake), not as a way to catch more faults. `CONSENSUS_CAP_LOCAL_EVALUATORS` is 0 (off) in this build. Present the AI as the headline detector and consensus as the safety net.

6. **Mobility faults are only visible while a team is travelling.** (See also the CSV section above.) A mobility fault that starts and ends while the team is stationary leaves no observable stall.

7. **The AI fault predictor is opt-in and off by default.** A trained float32 MLP (75 -> 128 -> 64 -> 5, sigmoid) runs on the CPU only when built with `-DFAULT_MODEL_LOADED=1`. The default build (`make`, `./tron`), the dataset generator and all checks stay AI-off: `fault_predictor_run()` returns all-zero scores and never touches the weights, so the CSV is unchanged. Turning the AI on changes team behaviour and therefore the CSV, so AI numbers must come from the separate AI build.
   - **Run with the AI:** `make tron-ai` builds `./tron_ai` (`./tron` is untouched); for the dashboard: `make tron-ai && TRON_BINARY=./tron_ai SIM_ONLY=1 python laptop/tools/app.py`. The `+ai` row of `make evaluate` uses the same flags. On the board, the CubeIDE DevRam configuration defines `FAULT_MODEL_LOADED=1`; the boot `CONFIG` line shows `model_loaded=1`.
   - **Board start-up:** on the board (`ENVIRONMENT_HAS_WORLD_STORAGE=0`) the AI outputs zero until every map and district row has arrived from the laptop over the mission link. On the laptop the world is local and nothing is waited for.
   - **The AI never acts alone.** Its score escalates past `LOCAL_RECOVERY` (return to safe zone, reallocate, abort) only when backed up by the watchdog anomaly flag, a sustained streak of high scores, or consensus ELEVATED. Because the watchdog threshold (`WATCHDOG_ANOMALY_THRESHOLD`, 0.10) was tuned while the AI returned zeros, a false watchdog alarm can now back up an AI score; measure with `laptop/tools/ai_fp_report.py` before relying on the AI-on behaviour.
   - **The gain is modest by construction of the simulator, not because detection is weak.** Faults are active on 58% of team-ticks (random profile) and clear on a fixed timer in `fault_injector_step()` (duration plus one PAR interval); nothing in the recovery layer can shorten them, so exposure is identical with or without the AI. Incidents resolved (held-out seeds 1001-1020): no faults 47.85, faults without AI 36.65, faults with a perfect ground-truth detector 39.70. The perfect detector recovers 27% of the 11.2 lost incidents, and all of it comes from resource faults (+3.20 of 9.7 lost; mobility +0.05 of 5.1 and communication +0.20 of 3.5, both within noise). Reproduce with `bash laptop/tools/ai_headroom_investigation.sh`. Not established: why mobility and communication faults are unrecoverable by the current recovery actions (needs experiments on the recovery logic).

8. **`eval_*` CSV columns are evaluation output, never model input.** They log what each evaluator concluded. Training on them leaks the answer.

## C. Manual override (dashboard simulation mode)

9. **An override guarantees only the initial commitment.** The pin is released as soon as the incident becomes `INC_ASSIGNED` to the pinned team, usually within a few ticks. From then on the team keeps the task through the simulation's normal state, and the normal rules (for example item 1, stamina/rest) still apply. The dashboard shows this as pending → committed → resolved or interrupted. An interrupted override is not re-pinned automatically; drag again if needed.

10. **Overrides work only in dashboard simulation mode.** They are sent over the `--live` simulator's stdin. They are not available in replay mode or when the dashboard is connected to the real board.

11. **System tasks (sector sweeps, equipment runs) can't be override targets.** The shared pin check in `core/cbba.c` only searches real incidents, so a pin onto a system task would silently clear on the next tick. The simulation path rejects these with reason `system_task`, and the dashboard doesn't offer them as drop targets. **Open gap:** the board's USART1 override path has no such check; a board-side override onto a system task silently clears on the next tick.

12. **Override rejections use only what the system itself knows.** A team is refused as `team_unavailable` only if the system has marked it degraded or peer consensus has raised it to the ELEVATED tier. Hidden (not yet detected) faults are never used, so an override onto a team with an undetected fault is accepted. This is deliberate: the reject reason must not leak ground truth.

13. **Windows stdin support is untested.** The non-blocking stdin reader has a Windows branch (`PeekNamedPipe`), but it is not covered by the test suite. macOS and Linux are the tested platforms.

## D. Dashboard map

14. **Buildings are drawn as dots, not filled footprints.** The world generator lets ~120 building-lot cells overlap road cells, so filled footprints always clash with the road layer. A building is a small dot at its centre, and its footprint outline appears when zoomed in or when its card is open.

15. **Blocked terrain is shown with the closest available flag.** `STATICMAP` has no separate rubble flag, so impassable (not navigable) cells stand in for rubble or blocked terrain. `road_blocked` marks blocked road cells specifically.

16. **The consensus network view is undirected.** Each team records which peers it heard from, which is directional. The network view draws one line per team pair, active if either direction was heard. It simplifies the real data; it does not invent any.

## E. Not yet verified on hardware

17. **The DK build's timing and memory are not yet measured on the board.** Tick duration (DWT counter), stack/heap headroom over a full 3000-tick run, and the AI's inference time on the DK (weights ≈ 74 KB of flash, float32) still have to be measured on the physical board. Until then, treat them as estimates.

18. **Confirmed running on the STM32N6570-DK only via the `DevRam` standalone-SRAM build (Application loaded directly by the debugger, no FSBL involved) — full handshake, world transfer, live telemetry and fault detection observed. Standalone Flash Boot hangs at the FSBL → Application handoff; diagnosis and evidence in [`docs/reports/ST_Support_Report.pdf`](docs/reports/ST_Support_Report.pdf), reported to ST (support case 00269955).** The FSBL prints its boot trace up through "init done, calling BOOT_Application" and then hangs indefinitely with no fault or watchdog reset — 100% reproducible across many rebuilds/reflashes/power cycles. A controlled SWD comparison shows the CPU's own debug access port becomes completely unreachable while hung in Flash Boot but works normally in DEV boot with the same debugger and cabling — pointing to a hardware/boot-ROM-level condition, not an application bug. **The full FSBL→XIP handoff has not yet been confirmed working end-to-end even under DEV boot with a debugger attached** — a real linker-script bug in the Application (linked for the wrong memory region) was found and fixed, but a live retest of that exact signed-FSBL + signed-Appli path has not been completed; do not assume it works without testing it. See [`docs/DK_DEMO_LAUNCH.md`](docs/DK_DEMO_LAUNCH.md) for the verified `DevRam` launch steps and the boot switch positions (DEV: BOOT1 on/BOOT0 off; Flash: both off). The board's mission-link session must be reset between mission runs — relaunching a mission without resetting the board first leaves reporting silently stuck.

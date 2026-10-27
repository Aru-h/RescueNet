# RescueNet architecture (one page)

The long technical reference is [`architecture/ARCHITECTURE.md`](architecture/ARCHITECTURE.md); this page is the short map and the invariants.

Status: reviewed by the owner on Oct 27. Written from the repository and the step reports; nothing here is new design.
Claim status for every statement about hardware or results lives in `CLAIMS.md`.

## What it is
Ten rescue teams (search, medical, extraction, perimeter, support; two of each) divide incidents in a procedurally
generated, earthquake-damaged grid city (36x36 cells of 10 m, about 13 ha; 1 tick = 36 s, so a team moves 1 km/h, a
loaded foot team on rubble; see DECISIONS D15 and `docs/SCALE_AUDIT.md` for the one inconsistency, the radio model's 100 m). There is no central dispatcher: teams allocate work with CBBA (an auction), and each team's
health is judged by a watchdog (digital twin), peer consensus and a small neural network. A recovery manager (and an
optional cost-based planner) decides what to do about a fault. Submitted to TRON Programming Contest 2026 as REMCE.

## Where the code runs
The same `core/` C code runs in two places. **Laptop** (`laptop/`, `./tron`, `./tron_ai`): simulator, dataset writer,
evaluation harness, dashboard server (`laptop/tools/app.py`). **Board** (`board/STM32N6570-DK/`): STM32N6570-DK under
mu-T-Kernel 3.0, laptop-clocked over the USART1 mission link. Opt-in study hooks are laptop-only and compiled out of
the board build (`ENVIRONMENT_HAS_WORLD_STORAGE`).

## Modules in `core/` (by job)
| Job | Files |
|---|---|
| World and incidents | `environment`, `grid`, `path` (A*), `incident`, `safezone`, `search_zones`, `mission_state` |
| Teams and movement | `team`, `simulation`, `scoring`, `random` (shared RNG streams) |
| Allocation | `allocator`, `cbba`, `comms_transport` (CBBA messages) |
| Fault injection | `fault_config`, `fault_injector` (causal profile: hazards, roads, persistence), `fault_comms`, `fault_buffer` |
| Detection | `watchdog`, `mission_execution_twin`, `consensus` + `vbus` (beacons), `feature_pack`, `fault_predictor` (+ generated weights/spec) |
| Recovery | `recovery_manager`, `recovery_planner`, `causal_actions`, `planner_calibration.h` |
| Laptop link and radio | `mission_link*`, `lora_*`, `aead_*`, `sx1262_setup.h` |
| Study hooks (opt-in) | `headroom_probes`, `road_knowledge` |

## One simulation tick (read from `core/sim_runner.c` and `core/simulation.c`)
`sim_runner.c` first runs `fault_injector_step()` and `allocate_all_incidents()` (which calls `cbba_run()`), then
`simulation_step()`: hazard propagation, incident generation, team update and movement, search sweep / starvation
escalation / quarantine release, incident work, passive effects, consensus, execution twin, watchdog, feature update,
`recovery_manager_step()` (which also consumes the MLP scores), snapshot recording, recovery dispatch. The code is the
authority for exact order. Messages: consensus beacons go over `vbus` (simulated radio); CBBA bids over `comms_transport`
(in memory on one board; the LoRa path is unused); laptop to board over USART1 mission-link frames.

## RTOS tasks (branch `feature/rtos-multitask`; compiles for Cortex-M55, NOT yet run on the board)
| Task | Priority | Job |
|---|---|---|
| link | 4 | radio and laptop link; drains the USART1 ring lock-free; applies laptop frames between ticks, in stream order |
| sim compute | 10 | boot init, then `sim_tick()`; the only writer of simulation state |
| ai workers 0, 1 | 11 | placeholders (no inference yet) |
| telemetry | 12 | placeholder; sends nothing yet |
| monitor | 20 | 1 s handler, sim watchdog alarm |
| logger | 30 | all console output, one character at a time; STATEHASH lines use a separate path that waits up to 500 ms instead of dropping |

## Invariants (a change that breaks one needs the owner's explicit approval)
1. Default output is byte-identical to the frozen reference: C1 = `./tron 1 3 1500 --fault-profile=combined`, dataset md5
   `b248bea72ce2ceb81389762dccc0a749`, plus the six existing fault profiles.
2. `sim_compute` is the only writer of simulation state; laptop inputs apply at tick boundaries in stream order; a missing
   STATEHASH line is a failure.
3. Causal-profile draws use a hash of (seed, tick, team, class), never the shared RNG streams.
4. No dataset schema, `RecoveryAction_t` enum or wire-format change without approval.
5. The model contract is `ml/feature_spec.json` (75 inputs, float32, BatchNorm folded); Python and C must agree
   (`make check-model-parity`, 19-vector fixture).
6. `submission/single-board-dk` is frozen. Study results use frozen binaries per tier with the md5 recorded.
7. Commits are authored by the owner with no AI trailers (older history still has them: clean before the repo is public).

## Branches
`submission/single-board-dk` (entry, frozen); `feature/causal-faults` (simulation, planner, study); `feature/rtos-multitask`
(task split); `feature/dashboard-2.5d` (dashboard); `upgrade/bsp-1.00.04`; `production` (stable dashboard copy; GitHub default branch, checked Oct 27);
the combined branch is rebuilt at close and not yet pushed.

# CLAIMS (what is true, how we know, and how sure we are)

Status values: **ON-BOARD** (ran on the STM32N6570-DK), **HOST-VERIFIED** (a test passes on the laptop), **COMPILES-ONLY**
(builds for Cortex-M55, never run), **SIM-ONLY** (a simulator result; says nothing about the real world), **WITHDRAWN**
(was claimed, did not replicate), **UNVERIFIED** (source or figure not checked against the original), **AUDITED** (a read-only code or data audit, not a passing test), **NOT-RUN**.
Rule: a claim's status can only go up with evidence recorded here (command, commit, log). Update this file in the same
commit that changes a claim. Numbers are per run, paired by seed, 95% bootstrap CIs.

## System and board
| ID | Claim | Status | Evidence |
|---|---|---|---|
| C01 | The submitted single-board build ran on the STM32N6570-DK before submission | ON-BOARD (owner-reported; how much of a mission it exercised is not recorded in the repo) | `submission/single-board-dk` |
| C02 | BSP v1.00.04 builds with `-DsN_INTVEC=N_INTVEC` | COMPILES-ONLY; never run on hardware | branch `upgrade/bsp-1.00.04`, issue #1 |
| C03 | Prioritised task split (link 4, sim 10, ...) compiles for Cortex-M55 | COMPILES-ONLY; NOT-RUN on the board | `docs/RTOS_SKELETON_BOARD_CHECK.md` |
| C04 | Task split does not change results (state hash identical to single-task) | NOT-RUN on the board. Host parts only: stream-order rule, `statehash_compare` tool, `make check-state-hash` | board baseline hashes never captured |
| C05 | C forward pass equals PyTorch within 4e-7 | HOST-VERIFIED (19-vector fixture); board parity NOT-RUN | `make check-model-parity` |
| C06 | Inference time, stack use, task latency, preemption benefit on the board | NOT-RUN (no measurements exist) | n/a |

## Original (random-fault) world, submitted version
| ID | Claim | Status | Evidence |
|---|---|---|---|
| C10 | The AI adds +3.05 incidents per run [1.30, 4.80] (seeds 1001-1020) | SIM-ONLY, original fault model only. Does not carry over: see C23 | `docs/evaluation/AI_EVALUATION_BRIEF.md` |
| C11 | A perfect detector does about as well as the real AI (39.7 vs 38.95) | SIM-ONLY, original model | same brief, C10 |
| C12 | Acting on environment/mission classes hurt, because recovery actions targeted teams | SIM-ONLY | same brief, C13 |
| C13 | ABORT_MISSION behaves like RETURN_TO_SAFE_ZONE; REQUEST_TASK_REALLOCATION never releases a task | HOST-VERIFIED; present in the submitted build; fixed only under the causal `semantics` token | `docs/RECOVERY_ACTION_SEMANTICS.md`; KNOWN_LIMITATIONS items "ABORT_MISSION and RETURN_TO_SAFE_ZONE do the same thing" and "REQUEST_TASK_REALLOCATION never releases the team's task" (causal branch) |

## Realistic (causal) world, v1
| ID | Claim | Status | Evidence |
|---|---|---|---|
| C20 | Causal fault model is built from hazards, wear, signal and aftershocks | SIM-ONLY | `docs/CAUSAL_FAULT_MODEL.md` |
| C21 | Fault rates match field-robot MTBF of 6-20 h (stress 1-2) | UNVERIFIED: Carlson and Murphy (2005) figures come from search summaries, not the paper | KNOWN_LIMITATIONS item "The persistent-fault parameters are derived or assumed, not measured" (causal branch) |
| C22 | Persistent faults cost 2.9 incidents (stress 2) and 9.1 (stress 7) vs the Step 3.5 world | SIM-ONLY (40 seeds) | `docs/PERSISTENT_FAULTS_RESULTS.md` |
| C23 | The old model's gain disappears in the realistic world; planner and gates do not differ reliably | SIM-ONLY | `docs/RECOVERY_PLANNER_RESULTS.md`, `docs/HEADROOM_MAP.md` |
| C24 | The grid is 10 m per cell (about 13 ha), so a team moves at 1 km/h; the stamina model is consistent with foot teams at that pace (D15). The causal radio model assumes 100 m per cell, which is inconsistent: v1 communication-fault exposure from distance is overstated (comms faults are about 1% of team-ticks and about 4 ticks long, so the mission effect is small) | AUDITED (owner states the scale; the audit found the inconsistency) | `docs/SCALE_AUDIT.md`, DECISIONS D15 |
| C25 | 7 of 300 gateways are unreachable from some team starts (seeds 1027, 1034, 1041, 1047, 1050, 1051, 1069) | SIM-ONLY | KNOWN_LIMITATIONS item "Some evacuation gateways cannot be reached by some teams" (dashboard branch) |

## Step 5 headroom map (100 dev + 100 held-out seeds, Holm over 376 contrasts)
| ID | Claim | Status | Evidence |
|---|---|---|---|
| C30 | A perfect fault detector adds about 0: +0.28 [-0.33, +0.89] at stress 2, +0.01 [-0.84, +0.85] at stress 7 | SIM-ONLY | `docs/HEADROOM_MAP.md` |
| C31 | Step 4 stress-7 detector headroom +1.75 | **WITHDRAWN** (did not replicate on 100 seeds) | HEADROOM_MAP; still printed in `docs/FINAL_EVALUATION.md` |
| C32a | Shipped AI on the planner (PA minus P) adds +1.23 at stress 7 | **WITHDRAWN** (Step 5: +0.14 [-0.47, +0.73] on 100 seeds) | HEADROOM_MAP |
| C32b | Shipped AI on the fixed gates (FA minus F) adds +1.68 at stress 7 | **UNCONFIRMED**: not re-measured in Step 5; the planner analogue (C32a) did not replicate, so treat as noise until re-run | `docs/FINAL_EVALUATION.md` (Step 4) |
| C33 | Forecasting, urgency rules, cheaper prevention, perfect comms, road information and cheaper responses do not pay | SIM-ONLY. Road information at a 1 h latency at stress 7 (+1.32) did not pass the correction; RR0 did not replicate | HEADROOM_MAP |
| C34 | Finding every incident the tick it appears (M5) gains +1.7 to +3.2 incidents per run at all stresses | SIM-ONLY oracle ceiling; confirmed on incidents; NOT confirmed on victims lost after correction. Whether it can be learned is untested | HEADROOM_MAP |
| C35 | Capacity is the big lever: instant travel +5.6, 25% faster work +4.4 (if no extra stamina cost), 25% shorter rests +1.7 | SIM-ONLY ceilings (removed constraints, not designs); incidents are inflated by demand response, victims lost is the cleaner measure | HEADROOM_MAP |
| C36 | 60-69% of fleet time is rest-related, 16-25% travel to tasks, 7-10% working on scene | SIM-ONLY; depends on the rest rules, which are an assumption | HEADROOM_MAP, TB tables |
| C37 | Default output is byte-identical to the frozen reference (C1 md5 `b248bea7...`) | HOST-VERIFIED at `feature/causal-faults` `b7e28d7` (independently re-checked by the coordinator session) | `make check-state-hash`, rebuild + md5 |

## Dashboard
| ID | Claim | Status | Evidence |
|---|---|---|---|
| C40 | 2D/2.5D/3D map, pins, cards, gateway markers work | HOST-VERIFIED: e2e suite (332 checks) passes on Linux except the known take-over step. Latest commits not confirmed by hand on the owner's Mac | `laptop/tools/dashboard_e2e_selftest.py` |
| C41 | The recorded demo replays identically on every platform | **Not true**: it diverges at frame 88 on Linux x86_64 (maths-library differences) | KNOWN_LIMITATIONS item "The bundled demo recording does not replay exactly on Linux" (dashboard branch) |

## Public wording (how each CV / README sentence maps to claims)
| Sentence | Claim IDs | Safe wording |
|---|---|---|
| "runs entirely on one STM32N6570-DK under mu-T-Kernel" | C01 vs C03 | True for the submitted build only (owner-reported). The post-contest task split is compiles-only |
| "bit-matched to within 4e-7" | C05 | "host-verified"; board parity not run |
| "AI improved incidents resolved by +3.05 per mission" | C10, C23, C30 | "+3 per mission under the original fault model; a later pre-registered study showed the gain vanishes against a realistic baseline" |
| "task split with a determinism proof" | C04 | "determinism check (host-verified; board run pending)" until the board comparison exists |
| "cost-based planner evaluated on held-out scenarios" | C23 | "evaluated" is accurate; do not say it improved results (no reliable difference) |
| "2D/3D dashboard" | C40 | accurate; mention the Linux-only e2e and the replay limitation if asked |

## Housekeeping found while writing this
- KNOWN_LIMITATIONS numbering collided across branches. DONE: renumbered once to 1-24 in the merge. This file still cites items by title.
- DONE (a542105): the withdrawn / unconfirmed Step 4 figures (+1.75, +1.68, +1.23 and the conclusions built on them) are marked in
  `docs/FINAL_EVALUATION.md`, `docs/FINAL_EVALUATION_harness_output.md`, `docs/PERSISTENT_FAULTS_RESULTS.md` and `docs/V1_TRAINING_HANDOFF.md`
  (section 8 rewritten). Some wrapped paragraphs carry one note at the paragraph end, not on every line.

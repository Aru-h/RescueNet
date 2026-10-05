# Step 3.5: cost-based recovery planner (`--recovery=utility`)

**Status: approved with decisions D1-D7 (section 11); implementation follows this
document.** Causal profile only. Every other profile, and `--recovery=fixed` (the
default), stay byte-identical to today.

## 0. What the code does today (read before designing)

* **The gates** (`core/recovery_manager_config.h`): per-stage action floors LOCAL 0.24,
  RETURN 0.36, REALLOC 0.48, ABORT 0.72 (x stage multipliers); AI escalation gates
  0.50 (reallocate) and 0.80 (return / abort); min confidence 0.18; a hard
  8-consecutive-tick AI-persistence streak (`FAULT_PERSISTENCE_*`); per-stage
  persistence and cooldown tables; de-escalation after `TICKS_PER_HOUR / 12` = 8 ticks.
  Action = highest signal clearing its floor, then `recovery_apply_hysteresis()`
  moves one rung at a time.
* **The Simplex rule as implemented** (`evaluate_fault_predictor()`): the AI alone
  caps at LOCAL_RECOVERY. RETURN / REALLOC / ABORT also need `watchdog.anomaly_detected`,
  or the 8-tick streak, or consensus ELEVATED (mobility or communication class only).
  Consensus never emits ABORT ("aborting still needs a local evaluator's severe branch").
* **Two things you listed do not exist in the code.** (a) ABORT_MISSION has **no
  operator-approval gate**: `recovery_dispatch()` applies it immediately. (b) There is
  **no "last specialist" rule** anywhere (`grep` of `core/`, `laptop/`, `docs/`).
  The only operator path is the dashboard override message
  (`core/mission_link_override.h`). The proposal therefore *introduces* both as hard
  constraints (section 5); that is new behaviour, flagged for your decision (D3).
* **The AI scores are not probabilities.** Class thresholds (0.573 / 0.965 / 0.705)
  were chosen on a validation set; I found no calibration anywhere in `ml/` or `docs/`.
  `P(fault | evidence)` needs a calibration layer (section 4).
* **Under the causal profile the shipped AI does not currently help.** Same seeds,
  stress 7, roads on, fixes on, actions off, AI-on minus AI-off incidents resolved:
  dev -1.40 [-4.95, +2.00], held-out -0.90 [-3.20, +1.60], pooled -1.15 [-3.25, +0.95]
  (victims lost pooled +1.45 [-0.38, +3.17]). So "planner+AI beats planner without AI"
  may not hold; the evaluation is built so that either answer is reported (section 6).
* **System-task starvation is structural.** Teams are AVAILABLE about 1-3% of ticks
  (`docs/SPECIALIZATION_LOAD.md`: 1.0-2.7% by specialization) and the allocator only
  hands system tasks to AVAILABLE/RESTING teams (`allocate_all_incidents()`); CLEAR_ROAD
  starts about 11 and REINFORCE about 0 per seed.

## 1. Decision scope

One planner, two layers, run once per tick before `recovery_dispatch()`.

**Layer A, per team-fault.** For each team with any detection, choose among the five
existing actions: none (CONTINUE_MONITORING), LOCAL_RECOVERY, REQUEST_TASK_REALLOCATION,
RETURN_TO_SAFE_ZONE, ABORT_MISSION. Output is the existing `RecoveryStatus_t`
(recommendation, reason, confidence, escalation_level), so `recovery_dispatch()`, the
dataset columns and `RecoveryAction_t` are untouched (no schema or enum change).
Classes: mobility, communication, resource, mission. **Environment is routing-only**
(Step 3.2 fix 2): it never enters Layer A.

**Layer B, per system task / map action.** Candidates, each with a target and a cost:

| Action | Target | Candidate when |
|---|---|---|
| REROUTE | a team | its route touches damage (the Step 3.2 label). Under true-cost A* the overlay is 0, so its benefit is exactly 0 and the planner never picks it (consistent with the REROUTE-only result); it matters only under `--roads-astar=blind` |
| CLEAR_ROAD | a damaged cell | observed crossing rate x remaining damage horizon is non-trivial |
| REINFORCE_INCIDENT | an incident | victims at risk and a support-capable team exists |
| RELAY_REPOSITION | a team | its link margin is negative |
| WAIT | none | default; also the environment label's only no-action response |

Layer B replaces the fixed triggers in `CausalActionParams_t`
(`clear_road_persist_ticks`, `clear_road_teams_threshold`, `relay_min_fault_ticks`,
`reinforce_event_window`, ...) with the cost comparison.

## 2. Preemption (the starvation fix)

A new planner-only primitive `planner_preempt(team, system_task)` releases the team's
current task back to INC_OPEN and assigns the system task directly, instead of waiting for
the team to be AVAILABLE. A preemption is chosen only when

`benefit(Y) > switch_cost(T: X -> Y) + margin`  (all in incident-ticks, section 3).

* `switch_cost` = urgency of X x (delay to X: leg to Y + Y's duration + leg back +
  one allocation cycle) + any work on X that is lost. **To verify before building:** that
  `work_remaining` on an incident survives an abandon (the code suggests it does; if not, a
  lost-work term is added).
* `margin` is the hysteresis margin of section 5, not a free constant.
* Never preempt a team inside the last `W` ticks of a stage (it finishes first; `W` =
  its own remaining stage ticks, known from `work_remaining` / work rate).
* **Scope (D5):** system tasks only (CLEAR_ROAD, REINFORCE, RELAY). Incident-to-incident
  preemption is left to CBBA.
* **Same-board only (D6):** pulling a team hosted on another board needs a new
  mission_link message (a wire change, out of scope). In the laptop simulation every team
  is local, so this costs nothing there; on a multi-board system the planner would only
  preempt teams its own board hosts. Stated, not hidden.

## 3. Costs

**Currency: incident-ticks (IT).** One IT is one tick of delay on one open incident.
This is the throughput metric the evaluation already uses, extended with victims through
the simulator's own survival model.

* **Urgency of an open incident** (IT per tick of delay):
  `u_i = 1 + V * n_i * h(age_i)`, where `n_i` is its live victim count and
  `h(a) = (k/lambda) (a/lambda)^(k-1) * tau_h` is the Weibull death hazard per victim per
  tick (`tau_h = TICK_TIME_SEC/3600`, `k = 1.47`, `lambda = 54.9 h`, MacIntyre 2006 figures
  *to verify*, already in `g_causal_params`). `V` converts one expected death into IT. It
  is the **one free exchange rate** between the two objectives.
* **Price of a team-tick:** `q_s = (sum of u_i over open stages needing spec s) / n_s`
  (live, from the queue; no constant), floored at the mean urgency of any open incident.

Every other term is derived from a constant that already exists:

| Term | Formula | Source |
|---|---|---|
| Delay of an incident by `dt` ticks | `dt * u_i` | definition |
| Stall / detour of a crossing | `delay = crossing_delay_ticks (4)` or detour cells | `g_causal_roads` (assumed 4, documented) |
| Travel to a target | cached A* remaining cost / cost-per-tick | `core/path.c`, `cost_per_tick = 3` |
| Work to finish a stage | `work_remaining / team_effective_work_rate` | `core/incident.c` |
| Rest to repair | `CAUSAL_MOBILITY_REPAIR_REST_TICKS (4)` + leg to the safe zone + stamina recovery `ARDUOUS/MODERATE/LIGHT_RECOVERY_RATE` | `core/mission_state.h`, `core/fault_config.h` |
| Continue-with-fault harm, mobility | `rem_task_ticks * p/(1-p) * u_task`, `p` = observed skip rate (twin progress error), prior `wear_symptom_max` | `docs/CAUSAL_FAULT_MODEL.md` |
| Continue-with-fault harm, resource | probability of forced return x (return leg + rest + re-travel) x `u_task` | same; equipment floor 25 |
| Continue-with-fault harm, communication | direct task effect 0; cost = lost consensus visibility, `c_blind` IT/tick | **assumed**, stated, +-50% |
| Residual fault after an action | `(1 - repair_effectiveness)`; wear repair 0.6 field / 0.3 rest | `g_causal_params` (the one place the planner embeds fault-model knowledge; +-50%) |
| CLEAR_ROAD benefit | `rate_cell * horizon * delay * u_bar`; `rate_cell` = observed route-touches per tick over a window, `horizon` = min(remaining damage, task) with a uniform 600-2400 prior | `route_touch`, `g_causal_roads` |
| CLEAR_ROAD cost | `(work/rate + travel) * q_perimeter + switch_cost` | work 120 / 240 |
| REINFORCE benefit | `(1 - survival_factor) * n_i * h_i * V * dur + (1 - 1/boost) * rem_work * u_i` | `g_causal_actions` (assumed values, documented) |
| RELAY benefit / cost | blind-cost avoided over reposition time / reposition ticks x `q` | `g_causal_actions` |

**Layer A expected cost:** `E(a) = sum_f P_f * C_fault(a, f) + C_action(a)`, `a` over the
five actions, summed over fault classes with a detection. `C_action` = ticks the action
takes the team out of service x `q` (RETURN = leg + rest + leg back; LOCAL = one cycle;
REALLOC = `u` x one allocation cycle + the task's re-bid delay; ABORT = the whole task's
sunk time).

Tuned parameters, all on dev seeds 1001-1020 only: **`V`** (start at the parity value
`1 / (n_med * h(age_med))` computed from the run's own medians, grid x1/3, x1, x3), the
**hysteresis margin fraction** (section 5), and the **calibration tables** (section 4).
Everything else above is derived; the +-50% sensitivity runs cover `V`, `c_blind`,
repair effectiveness, the crossing delay and the margin.
All values live in one `PlannerParams_t` in `core/fault_config.h`, each with a source
comment in the same style as `CausalFaultParams_t`.

## 4. P(fault | evidence)

Sources, all already computed per team per tick: the AI class scores
(`fault_predictor_run()`), the watchdog `anomaly_confidence` + `dominant_failure_mode`,
and the consensus tier and `y` (mobility, communication).

* **Calibration (D4):** a monotone lookup table per source and class (isotonic fit)
  mapping score to the empirical frequency of the ground-truth fault, fitted **on dev
  seeds only** from dataset labels. It lives in `core/` as a small const table, so **`ml/`
  and the model are untouched and nothing is retrained.** (The alternative, using raw
  scores against fixed gates, is what the planner is meant to replace.)
* **Fusion:** per class, `P_f = max` of the calibrated sources. They are not independent
  (the AI consumes the watchdog's evidence vector), so a product or noisy-OR would
  overcount. A fitted logistic stack is the alternative; I recommend `max` first because it
  adds no parameters.
* **Planner without the AI** uses watchdog + consensus only. **Oracle P** is the label
  itself (laptop-only, an upper bound).
* **To verify:** that the AI's training data does not include held-out seeds
  2001-2020 (the model and its data are in `ml/`, which I will read, not change).

## 5. Safety

1. **Simplex floor (D2).** Evidence from the safety controller is binding: if the
   watchdog flag **or** consensus ELEVATED is set, the planner may not choose an action
   *below* what the legacy rule-based evaluators (everything except the AI) recommend
   for that team this tick. It can only choose that action or a higher-ranked one. This
   keeps the safety controller able to force the safe action; the planner cannot talk
   it down.
2. **AI-only evidence stays capped.** With no watchdog flag, no consensus ELEVATED and
   no 8-tick streak, an AI-driven Layer A choice is capped at LOCAL_RECOVERY (the README
   rule). Layer B (map / task actions) is not an unsafe action and is not capped.
3. **Approval-gated actions (D3).** ABORT_MISSION, and any action that removes the last
   available team of a specialization that has open demand (RETURN, REALLOC, preemption),
   are emitted as an *approval request* and not applied. In the simulator the default is
   `--planner-approval=deny`, so the planner never relies on an operator that is not
   there; `--planner-approval=auto` (laptop-only) grants every request, to measure what
   approval is worth. A real operator would approve through the existing dashboard
   override path; no wire change.
4. **Hysteresis and minimum dwell.**
   *Min dwell:* a team-level action is held until it completes its own commitment
   (RETURN: arrival and rest; LOCAL: one cycle). *Switch margin:* the chosen action changes
   only if the new expected cost is lower by more than `max(delta, rho * E_current)`, where
   `delta` = the cost of one reversal (two legs x `q`, derived) and `rho` is tuned on dev.
   *Preemption cooldown* per team, as RELAY already uses (30 ticks, to be re-derived).
   *Chattering metric*, reported against the fixed gates: action changes per team per 1000
   ticks, and reversals within 10 ticks.
5. **Environment stays routing-only.** A unit check (as `check-causal-env-no-rtb`)
   asserts the planner never emits a team-level action for the environment class.

## 6. Baselines and evaluation (after your OK)

Stress 7, roads on, fixes on, 3000 ticks, dev 1001-1020, held-out 2001-2020, pooled;
paired 95% bootstrap CIs; every table also has the REROUTE-only row where relevant.

| # | Condition | Purpose |
|---|---|---|
| B0 | fixed gates, AI off (`./tron`) | context |
| B1 | **fixed gates + AI** (shipped, actions off) | the baseline to beat |
| B1b | fixed gates + AI + Step 3 actions on | the best existing configuration |
| P1 | planner, P from watchdog + consensus only (no AI) | AI-necessary control |
| P2 | planner + AI | the product |
| P3 | planner + oracle P | upper bound |

**"AI is necessary" = P2 minus P1**, reported explicitly with its CI and in both
directions. Given section 0, I expect it may not exclude zero; that is a result, not a
failure. Also reported: incidents resolved, victims lost (no conclusion from a single set),
preemptions, action counts by type, chattering, planner decisions per 1000 ticks, and
whether the **blind-A\* REROUTE victims penalty (+3.00 pooled)** changes. Ablations
`--planner=team,tasks,preempt` separate Layer A, Layer B and preemption. Sensitivity: each
of `V`, `c_blind`, repair effectiveness, crossing delay and the margin at +-50%.

Honest expectation: the planner's largest plausible gain is Layer B plus preemption (the
starvation fix); the earlier headroom measurement (perfect detector adds at most about 0.75
incidents over the AI under the combined profile) suggests the P-quality gap is small. A
first cut can still be harmful, as the first Step 3 actions were; the ablation switches
exist so that can be diagnosed quickly rather than argued.

## 7. Board (M55)

Planner core in `core/` (`recovery_planner.c/.h`), compiled for the board; **laptop-only**:
decision log, calibration fitting, oracle P, `--planner-approval=auto`, tuning scripts.

* **State (estimates, to be confirmed with `sizeof`):** per team about 32 B (last action,
  dwell counter, last-switch tick, pending preempt target, cooldown) x 10 = about 320 B RAM.
  Global: preemption table about 4 x 16 B. Const: `PlannerParams_t` about 100 B, calibration
  tables 5 classes x 16 bins x 2 B x 3 sources = 480 B flash. No heap, no recursion, fixed
  loop bounds. Decision ring (laptop-only): 256 x 48 B = 12 KB, not built for the board.
* **Per tick (op-count estimate, not a measurement):** Layer A about 10 teams x 5 actions x
  up to 4 classes, about 40 flops each; Layer B about 10 candidates x 10 teams, each reading
  an already-cached path cost. The dominant risk is path lookups for preemption switch costs
  (bounded at 30 per tick and served from the existing `path.c` cache). Budget: at most 10%
  of the measured x86 `allocate_all_incidents()` time (about 65 us of 654 us per tick); I
  will measure the x86 figure and report it.
* **No M55 number can be produced here.** The repo has no on-device timing (the
  architecture doc lists it as a TODO), and there is no ARM toolchain in this environment,
  so `check-board*` cannot run. As in Step 3.2, I would substitute a host syntax check of every
  `core/*.c` in the board configuration (`-DENVIRONMENT_HAS_WORLD_STORAGE=0`).

## 8. State hash

`core/state_hash.h` (on `origin/feature/rtos-multitask`) hashes four sub-states: teams,
incidents, faults, recovery. New state to hash once causal and RTOS are merged:

| State | Where | Why |
|---|---|---|
| Planner per-team state (last action, dwell, last-switch tick, pending preempt, cooldown) and preemption table | new | proposed 5th sub-hash `planner` |
| Calibration tables and `PlannerParams_t` | const | not state; hash the build id only |
| Stall counter `s_stall[]` | `core/causal_actions.c` | Step 3.1 integration item, still open |
| Road damage kind per cell `s_road_kind[][]`, label state `s_label_now/prev` | `core/fault_injector.c` | Step 3.1 / 3.2 |
| Hidden wear, hazards, aftershock schedule, per-team truth | `core/fault_injector.c` | causal model state, not hashed today |
| CausalAction state (reroute / relay / clear tables, system-task slot map) | `core/causal_actions.c` | Step 3 |

The decision log, the sidecar and the dataset are outputs and are **not** hashed.

## 9. Decision log

Laptop-only, `tron_planner_log.csv` (one row per non-trivial decision, keyed
`world_seed, tick, team`; not part of the dataset or sidecar), plus a `[PLANNER] {json}`
line on stderr in `--live` mode for the dashboard. A decision is non-trivial when the
chosen action is not WAIT/none, differs from the previous tick's, or the runner-up is
within the switch margin. Fields:

`tick, team, layer (A|B), classes flagged with P per source (AI / watchdog / consensus,
raw and calibrated), chosen action and target, chosen expected cost split into
P*C_fault and C_action, top alternative and its cost, margin used, guard flags
(forced_by_watchdog, forced_by_consensus, ai_only_cap, approval_needed, approval_result),
preempted task and its switch cost`.

The dashboard "why card" is then a direct render of one row. A replay-safe version needs the
dashboard gaps already listed in `docs/CONSEQUENTIAL_ROADS.md` (per-tick map history).

## 10. Plan, files, risks

New: `core/recovery_planner.c/.h`. Changed (all gated on `--recovery=utility`):
`core/recovery_manager.c` (planner supplies the `RecoveryStatus_t`), `core/causal_actions.c`
(planner-driven Layer B), `core/allocator.c` / `core/incident.c` (preempt primitive),
`core/fault_config.[ch]`, `laptop/main.c` (flags: `--recovery=fixed|utility`,
`--planner=team,tasks,preempt`, `--planner-approval=deny|auto`), laptop log writer, tuning and
report scripts, Makefile checks (byte-identity with `--recovery=fixed`, determinism, Simplex
floor, environment never team-level, approval gate, preemption never takes the last
specialist without approval).
Time: about 8-10 hours wall-clock, about 5 of them unattended simulation runs.

Risks: (1) a first-cut planner worse than the fixed gates (mitigated by ablation switches
and dev-only tuning); (2) calibration fit on dev can overfit (held-out reports it);
(3) fault-model knowledge embedded in repair effectiveness (sensitivity runs); (4) chaotic
divergence makes small effects look like noise, so claims need the pooled 40 seeds.

## Decisions I need from you

* **D1 Currency.** Incident-ticks with one exchange rate `V` for victims (recommended), or
  a two-objective formulation with explicit weights.
* **D2 Simplex floor.** "Planner may not choose below the rule-based evaluators' action"
  (recommended), or "watchdog / consensus ELEVATED overrides the planner entirely".
* **D3 Approvals.** Introduce them as constraints (they do not exist today), simulator
  default `deny` (recommended), `auto` as a laptop-only upper bound.
* **D4 Calibration.** Post-hoc isotonic tables in `core/`, fitted on dev seeds, no retrain
  (recommended).
* **D5 Preemption scope.** System tasks only (recommended).
* **D6 Preemption locality.** Same-board only, noted as a multi-board limitation.
* **D7 Baseline B1.** Fixed gates + AI with Step 3 actions off (shipped default), with B1b
  (actions on) alongside (recommended).

## 11. Resolved decisions and pre-implementation verification

### Decisions

* **D1** single exchange rate `V`; results reported across a sweep of x0.5, x1, x2 of the
  derived value (`PlannerParams_t.v_mult`), so the choice is visible.
* **D2** Simplex *floor*.
* **D3** approvals as constraints: deny by default in the simulator, `auto` as a
  laptop-only upper bound; causal + `--recovery=utility` only.
* **D4** calibration tables fitted on dev seeds only; calibration quality (reliability
  bins, ECE) reported on the held-out seeds.
* **D5** preemption of system tasks only. **D6** same-board only.
* **D7** baseline B1 = fixed gates + AI with Step 3 actions off; actions-on alongside.

**Headline framing for this step:** (1) *planner without the AI vs fixed gates*, and (2)
*planner with oracle P vs planner without the AI*, which is the headroom a better AI could
capture (the target for the later retraining step). Planner+AI vs planner without AI is
still reported, and with the current (old-label) model it may not favour the AI.

### Verification 1: which seeds produced the AI's data (read from `ml/`, not from the
checkpoint: PyTorch is not installed here, so the checkpoint could not be inspected)

| Use | Seeds | Source |
|---|---|---|
| Model training data (`--fault-profile=random`, AI off) | **1-300** | `ml/MODEL_REPORT.md` sec. 3, `ml/README.md` |
| Class thresholds: validation best-F1 on the training split; closed-loop dev | **2001-2008** | `ml/MODEL_REPORT.md` sec. 3 ("closed-loop threshold tuning (dev) 2001-2008"), `laptop/tools/ai_threshold_sweep.py` defaults |
| Closed-loop results reported by the ML project | 1001-1020 | `ml/MODEL_REPORT.md` |

**Overlap: yes.** Seeds 2001-2008, which my earlier "held-out" set 2001-2020 contains,
were used to tune the AI's thresholds (not its weights, which saw only 1-300). So for
**AI-on rows** 8 of the 20 old held-out seeds were not clean. AI-off rows were not
affected. **The held-out set for this step and from now on is 3001-3020**: grep found no
use of 3001+ in `ml/`, `docs/`, the tools or my own earlier runs. My dev seeds 1001-1020
are the ML project's reported seeds; they were not used for training or tuning of the
model, so fitting the planner's calibration and costs there is clean. Earlier held-out
tables (Steps 3-3.2) stay as published, with this caveat for their AI-on rows.

### Verification 2: a preempted team's progress on its original task

Code: `abandon_task_for_recovery()` sets the task back to INC_OPEN and clears its owner,
and `work_remaining` is written only at creation and on a stage transition
(`complete_incident_stage()`), never on release. Measured (20 dev seeds, 3000 ticks, AI
on; probe in a scratch copy hooking every abandon and the next assignment of the same
incident): 2,835 abandons, 2,236 of them with work already done, 2,249 re-assignments:

* **2,195 (97.6%) re-assigned with `work_remaining` identical**;
* 54 re-assigned with `work_remaining` *higher*, and **all 54 had advanced to the next
  stage** in between (the required specialization changed), i.e. stage completion, not loss;
* **0** same-stage losses; none lower.

So progress is kept. The preemption primitive reuses this release path and a check will
assert it for planner preemptions.

### Where the docs claim ABORT needs approval (listed, not edited)

No document claims *operator* approval for ABORT. The statements closest to it say abort
is **gated by corroboration** (watchdog flag, 8-tick streak, or consensus), which the code
does implement for the AI's abort path (`evaluate_fault_predictor()`):
`README.md` 141-144 and 186; `KNOWN_LIMITATIONS.md` 39 and 56-59; `docs/DEVELOPER_NOTES.md`
97; `ml/MODEL_REPORT.md` 240; `docs/architecture/ARCHITECTURE.md` 113;
`docs/reports/REMCE_Faculty_Report.md` 155 and 199;
`docs/architecture/CONSENSUS_DESIGN.md` 671 and 1035 (consensus never aborts). The
rule-based evaluators' ABORT branches have no gate at all. A reader could take "abort
needs the watchdog" as "abort is protected", which is only true for the AI's own path.

### Two findings that change the design

* **ABORT_MISSION and RETURN_TO_SAFE_ZONE have the same effect on a team that is working**:
  `recovery_dispatch()` calls `abandon_task_for_recovery()` for both; LOCAL_RECOVERY calls
  it too (only for an AVAILABLE team). **REQUEST_TASK_REALLOCATION never releases the
  team's task** (it re-runs allocation, which only assigns OPEN tasks). So Layer A really
  chooses between "stay" and "go home"; ABORT is dominated by RETURN (same effect, plus an
  approval requirement), and REALLOC is modelled with its true effect (none on the team).
  The planner emits RETURN (or LOCAL for an AVAILABLE team) for "go home".
* The approval gate therefore bites mainly on the **last-specialist rule**.

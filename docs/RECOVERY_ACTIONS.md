# Causal-profile recovery actions (Step 3)

Environment and mission faults used to have no recovery action that acted on
what they damage: every action in `core/recovery_manager.c` (monitor, local
recovery, return to safe zone, task reallocation, abort) acts on the *team*,
while an environment fault degrades a road cell and a mission fault worsens an
incident. That is why `docs/evaluation/AI_EVALUATION_BRIEF.md` claim C13 found
the environment and mission classes harmful to enable even with a perfect
detector.

Step 3 adds four actions that act on the map, the incident or the radio link.
They exist **only under `--fault-profile=causal`**; every other profile, and
the default recovery behaviour, is byte-identical to before.

## They are not `RecoveryAction_t` values

`RecoveryAction_t` is serialized: it is written to the dataset CSV
(`recovery_recommendation`, `laptop/snapshot.c`), compared numerically by
analysis tools (`>= RETURN_TO_SAFE_ZONE` is `>= 2`), named in the dashboard
(`laptop/tools/app.py`), and used with `>` / `>=` ordering inside the recovery
manager's own escalation logic. Appending values after `ABORT_MISSION` would
have changed what every one of those comparisons means. So the enum and
`RECOVERY_ACTION_COUNT` are untouched, and the new actions are a separate
channel, `CausalAction_t` in `core/causal_actions.h`, with their own metrics,
printed as `[DIAG ACTION]` lines (causal runs only) and as an `action_mask`
column in the causal-only sidecar `tron_causal_truth.csv`.

## The actions

| Action | Acts on | What it does |
|---|---|---|
| `REROUTE` | a team's path | While a team's route (up to 8 steps ahead) crosses a faulted or blocked cell, that team's pathfinder sees a small extra cost on such cells. It ends when the route no longer touches one, or after 40 ticks. |
| `CLEAR_ROAD` | a map cell | A system task at a faulted cell. When a team finishes the work, every environment fault on the cell ends and the cell is restored from its pre-fault baseline in one step. |
| `REINFORCE_INCIDENT` | an incident | A deteriorating incident requests a support team. While that team is on scene, work is 1.25x faster and the victim death hazard is halved. |
| `RELAY_REPOSITION` | a team's position | A team whose comms fault has lasted 3+ ticks steps toward the nearest cell where the link margin clears the +3 dB hysteresis with a 1 dB cushion, and resumes its route once the fault clears. |

### Triggers (rule-based, observable state only)

Teams, tasks and the map are not told about fault ground truth; the rules read
what a team could see.

* **REROUTE**: an en-route team whose next 8 route steps cross a cell with an
  active environment fault or a blocked road.
* **CLEAR_ROAD**: a cell that has stayed faulted for more than **N = 5 ticks**
  (half the shortest environment backstop), or that lies on the route of
  **2 or more** teams at once. At most 2 clearing tasks exist at a time; one
  per cell. A task is cancelled if its cell clears on its own.
* **REINFORCE_INCIDENT**: an incident with an assigned team whose reported
  victim count fell or whose priority was raised within the last 60 ticks. The
  request stands for 150 ticks and is served only if a support team is free
  (`allocate_support_team()`, `core/allocator.c`).
* **RELAY_REPOSITION**: a team that is en route (not heading to rest) with a
  communication fault older than 3 ticks and a link margin below 0 dB, with no
  reposition in the last 30 ticks.

### AI / oracle triggering

When the AI (or the oracle in `laptop/tools/causal_actions_oracle.sh`) flags
the environment or mission class for a team, `evaluate_fault_predictor()`
(`core/recovery_manager.c`) no longer folds that class into the team-level
confidence. It calls `causal_actions_detector_flag()` instead:

* environment: reroute that team now (no route scan needed) and request a
  `CLEAR_ROAD` task for its fault cell now (no N-tick wait);
* mission: request reinforcement of the team's assigned incident now.

The model configuration is unchanged: environment and mission thresholds stay
at `FAULT_PREDICTOR_CLASS_OFF`.

## How each action is wired

| Piece | File |
|---|---|
| action state, rules, metrics | `core/causal_actions.c`, `core/causal_actions.h` |
| per-tick call after the injector | `core/sim_runner.c` (`sim_tick()`) |
| per-team path overlay | `core/path.c` (`path_set_extra_cost()`; off, and cache bypassed, unless a rerouting team is moving) |
| overlay set around the two pathfinder calls | `core/simulation.c` (`move_team()`) |
| relay stepping | `core/simulation.c` (`move_team()`, `causal_actions_move_override()`) |
| clearing task type | `core/mission_state.h` (`INCIDENT_CLEAR_ROAD`, appended), archetype and creation/cancel in `core/incident.c` |
| clearing completion | `core/incident.c` (`complete_incident_stage()`), `core/fault_injector.c` (`fault_injector_clear_cell_faults()`) |
| reinforcement allocation | `core/allocator.c` (`allocate_support_team()`) |
| reinforcement effects | `core/incident.c` (`update_incident_work()`), `core/fault_injector.c` (mission survival model) |
| AI/oracle path | `core/recovery_manager.c` (`evaluate_fault_predictor()`) |
| flags and diagnostics | `laptop/main.c` |

### Residual-cost fix

When several teams' environment faults stack on one cell, a non-last owner's
recovery used to subtract only its own severity from a cost that the existing
road-degradation hazard may have raised in the meantime. Under the causal
profile that recovery now rebuilds the cost as the cell's baseline plus the
severities of the faults still active on it (`recover_environment_fault()`),
and `CLEAR_ROAD` restores the baseline for all owners at once. Other profiles
keep the old arithmetic.

## Deviations from the brief, stated plainly

1. **`CLEAR_ROAD` and `REINFORCE_INCIDENT` are not CBBA bundle tasks.** CBBA
   bundles hold only incident-pool tasks (`core/cbba.c`); system tasks such as
   sweeps and equipment caches are allocated by the greedy
   `select_best_team()` loop in `core/allocator.c`, and `cbba.c` documents that
   nothing puts system tasks into bundles. `CLEAR_ROAD` is a system task and
   uses that same allocation as sweeps and equipment-cache runs; reinforcement
   uses the existing `support_team` allocation. Putting them into CBBA proper
   would mean making them incident-pool tasks, which would count them as
   "incidents resolved", change incident-generation load, and change the board
   wire format; that was not done.
2. **`RELAY_REPOSITION` is the team-repositions variant only.** The variant in
   which a free support team moves to a relay position was not built: a team
   that is on scene with no task is pulled off by the team state machine
   (`core/team.c`), so a relay team cannot hold a position.
3. **The recovery action enum was not extended** (see above).

## Parameters (`g_causal_actions`, `core/fault_config.c`)

| Parameter | Value | Meaning | Source |
|---|---|---|---|
| `reroute_cost_penalty` | 3 | extra path cost on a faulted/blocked cell for a rerouting team | = the fault's own cost increment; chosen from a sweep of 3 / 10 / 30 (see Results), assumed |
| `reroute_lookahead` | 8 steps | route scanned for faulted cells | assumed |
| `reroute_max_ticks` | 40 | a reroute never lasts longer | assumed |
| `env_crossing_delay_ticks` | 0 (Phase 2 model) | ticks a team stalls before stepping onto a faulted cell; **sensitivity override only** (`--causal-env-crossing-delay`) | assumed |
| `clear_road_persist_ticks` (N) | 5 | a cell faulted this long gets a clearing task | assumed (half the shortest environment backstop of 5-12 ticks) |
| `clear_road_teams_threshold` | 2 | teams whose route crosses the cell, as the alternative trigger | assumed |
| `clear_road_max_tasks` | 2 | concurrent clearing tasks | assumed |
| `clear_road_work` | 60 work units | work to clear a cell (a team does ~8-12 per tick: about 6 ticks) | assumed |
| clearing task specialization | Perimeter | perimeter teams do structural/debris work; any available team can still win it | assumed |
| `reinforce_event_window` | 60 ticks | a death or escalation this recent counts as deteriorating | assumed |
| `reinforce_hold_ticks` | 150 | a reinforcement request stands this long | assumed |
| `reinforce_work_boost` | 1.25x | work-rate multiplier with a support team on scene | assumed |
| `reinforce_survival_factor` | 0.5 | victim death-hazard multiplier with a support team on scene | assumed |
| `relay_min_fault_ticks` | 3 | a comms fault must be this old before a reposition | assumed; added after a first version without it made 1-2 tick blips trigger chattering repositions |
| `relay_cooldown_ticks` | 30 | no new reposition for a team for this long afterward | assumed; same reason |
| `relay_search_radius` | 8 cells | search radius for a positive-margin cell | assumed |
| `relay_max_ticks` | 20 | a reposition never lasts longer | assumed |

Command-line controls (all causal-only; other profiles reject them):
`--causal-actions=on|off|reroute,clear_road,reinforce,relay` (default on),
and the sensitivity overrides `--causal-env-duration=<min>-<max>`,
`--causal-env-crossing-delay=<ticks>`, `--causal-reroute-penalty=<n>`.

## Findings, stated plainly

1. **Environment faults only add routing cost.** An environment fault raises
   one cell's `travel_cost` by its severity (3) and sets `road_blocked`, and
   that is all: `navigable` is kept on purpose (blocking it cut the graph), a
   team still moves exactly one cell per tick, and travel cost affects only
   which route the pathfinder prefers and what a CBBA bid costs. Nothing slows
   or stops a team that crosses a faulted cell. That bounds what `REROUTE` and
   `CLEAR_ROAD` can achieve in the approved Phase 2 model: there is almost
   nothing for them to save.
2. **`CLEAR_ROAD` and `REINFORCE_INCIDENT` starve.** System tasks and support
   assignments go only to teams that are `AVAILABLE`, and a team is
   `AVAILABLE` on about 3% of ticks (teams are almost always resting or
   working). `CLEAR_ROAD` gets a team to finish about 3 of ~35 tasks per seed
   even when road damage lasts 60-150 ticks; `REINFORCE_INCIDENT` never got
   a support team in 20 seeds. **The Step 3.5 planner should be able to pull a
   team off lower-priority work when the expected benefit is higher**; the
   rule-based version here, which waits for a free team, cannot.
3. **The detour penalty was picked from a 3 / 10 / 30 sweep.** `REROUTE` with
   a penalty of 30-40 made teams walk far to avoid a +3 cost. With persistent,
   costly road faults (below) reroute-only scored 42.35 incidents at penalty 3,
   41.60 at 10 and 37.10 at 30, against 41.20 with the actions off. The default
   is 3 (the fault's own cost increment). It was chosen on the same seeds used
   for evaluation, from three values, so treat the reroute number as
   mildly optimistic.
4. **The first version was harmful and was changed.** With penalty 40 and a
   `RELAY_REPOSITION` that triggered on any comms fault, all four actions on
   gave 34.10 incidents against 44.45 with them off (seeds 1001-1020, stress
   7). Per-action ablation showed reroute-only 38.20 and relay-only 40.95;
   relay was chattering (1-2 tick faults, team steps off route, fault clears,
   team walks back in). Fixes: a small penalty; relay only after the fault has
   lasted 3 ticks, with a 30-tick cooldown. The numbers below are from the
   fixed version.

## Results (causal profile, stress 7, seeds 1001-1020, 3000 ticks, AI off)

Differences are paired by seed; bootstrap 95% CI, 10000 resamples.

### Causal before Step 3 (`--causal-actions=off`) vs with the actions (`on`)

Phase 2 environment model (faults last 5-12 ticks, no crossing cost):

| | actions off | actions on | on - off (95% CI) |
|---|---|---|---|
| incidents resolved / seed | 44.45 | 43.20 | **-1.25 [-2.40, -0.20]** |
| victims lost / seed | 13.0 | 12.1 | -0.90 [-2.55, +0.70] |
| any-fault exposure | 0.107 | 0.106 | |
| mobility exposure / mean duration | 0.044 / 7.6 | 0.044 / 7.6 | |
| communication exposure / mean duration | 0.011 / 3.2 | 0.011 / 2.8 | |
| resource exposure / mean duration | 0.039 / 7.9 | 0.037 / 7.9 | |
| environment exposure / mean duration | 0.019 / 8.5 | 0.019 / 8.5 | |
| mission exposure / mean duration | 0.001 / 6.6 | 0.000 / 7.0 | |

Action counts per seed (on): reroute started 114.5, resolved 88.3, abandoned
26.1; clear_road started 42.1, resolved 0.0, abandoned 42.1 (the fault expired
first); reinforce_incident started 0.0 (3.9 requests expired unserved);
relay_reposition started 9.2, resolved 8.9, abandoned 0.2.

In this model the actions cost a small, significant number of incidents: the
only action with real activity is reroute, and it detours to avoid a cost that
never slowed anyone.

Sensitivity model (what-if, not the default): environment faults last 60-150
ticks (`--causal-env-duration=60-150`) and a team stalls 3 ticks before
stepping onto a faulted cell (`--causal-env-crossing-delay=3`), so road damage
costs real time and persists:

| | off | on (all four) | reroute only | clear_road only |
|---|---|---|---|---|
| incidents resolved / seed | 41.20 | 41.20 | 42.35 | 41.25 |
| paired vs off (95% CI) | | 0.00 [-1.85, +1.70] | +1.15 [-0.30, +2.55] | +0.05 [-1.10, +1.20] |
| victims lost / seed | 12.3 | 12.0 | 13.3 | 11.6 |
| environment exposure / mean duration | 0.174 / 103.5 | 0.173 / 97.3 | 0.181 / 104.9 | 0.172 / 97.5 |
| any-fault exposure | 0.246 | 0.243 | 0.252 | 0.243 |

Actions per seed (on): reroute started 202.7 (139.7 resolved, 62.6 abandoned);
clear_road started 35.3, resolved 2.9, abandoned 31.6; reinforce_incident 0
started; relay_reposition started 9.6, resolved 9.3. Going from the Phase 2
environment model to this one costs 3.25 incidents per seed with the actions
off (44.45 to 41.20; the duration change and the crossing delay are not
separated), and the actions recover none of it significantly.

### C13 re-run under the causal profile (AI_EVALUATION_BRIEF.md claim C13)

Real AI with shipped thresholds for mobility/communication/resource, plus a
**perfect** detector (class score = ground truth, threshold 0.5) for the
environment class, the mission class, or both, with the new actions on or off.
Compared with `ai_ship, actions on` (the shipped AI setup with the new actions).
Script: `laptop/tools/causal_actions_oracle.sh`; inference only, the model is
not retrained and the model configuration is unchanged.

Phase 2 environment model:

| condition | incidents | vs ai_ship/on (95% CI) | victims lost | vs ai_ship/on (95% CI) |
|---|---|---|---|---|
| ai_ship, actions on | 41.70 | | 13.75 | |
| ai_ship, actions off | 42.15 | +0.45 [-1.25, +2.15] | 14.45 | +0.70 [-1.75, +3.05] |
| oracle env, actions on | 42.00 | +0.30 [-1.20, +1.85] | 12.50 | -1.25 [-3.70, +1.30] |
| oracle env, actions off | 42.45 | +0.75 [-0.95, +2.55] | 12.10 | -1.65 [-3.85, +0.45] |
| oracle mission, actions on | 41.70 | +0.00 [+0.00, +0.00] | 13.75 | +0.00 [+0.00, +0.00] |
| oracle mission, actions off | 42.10 | +0.40 [-1.20, +2.05] | 14.30 | +0.55 [-1.95, +2.80] |
| oracle env + mission, actions on | 42.00 | +0.30 [-1.20, +1.85] | 12.50 | -1.25 [-3.70, +1.30] |
| oracle env + mission, actions off | 43.10 | +1.40 [-0.25, +3.30] | 12.80 | -0.95 [-3.15, +1.25] |

Sensitivity model (60-150 tick faults, 3-tick crossing cost):

| condition | incidents | vs ai_ship/on (95% CI) | victims lost | vs ai_ship/on (95% CI) |
|---|---|---|---|---|
| ai_ship, actions on | 40.50 | | 14.90 | |
| ai_ship, actions off | 39.80 | -0.70 [-2.10, +0.75] | 12.50 | -2.40 [-4.15, -0.80] |
| oracle env, actions on | 40.60 | +0.10 [-1.15, +1.35] | 13.15 | -1.75 [-3.75, +0.05] |
| oracle env, actions off | 37.95 | **-2.55 [-4.05, -1.05]** | 13.25 | -1.65 [-3.65, +0.15] |
| oracle mission, actions on | 40.50 | +0.00 [+0.00, +0.00] | 14.90 | +0.00 [+0.00, +0.00] |
| oracle mission, actions off | 39.75 | -0.75 [-2.25, +0.75] | 12.95 | -1.95 [-3.70, -0.35] |
| oracle env + mission, actions on | 40.60 | +0.10 [-1.15, +1.35] | 13.15 | -1.75 [-3.75, +0.05] |
| oracle env + mission, actions off | 38.00 | **-2.50 [-3.80, -1.15]** | 14.25 | -0.65 [-2.70, +1.35] |

Reading it:

* **The C13 harm reproduces when road damage is consequential** (oracle env,
  actions off: -2.55 [-4.05, -1.05]; env + mission: -2.50 [-3.80, -1.15]) and
  the new actions remove it (+0.10 [-1.15, +1.35]). In the approved Phase 2
  model the legacy team-level reaction is not measurably harmful under causal
  (+0.75 [-0.95, +2.55]).
* **No condition with the actions on has a positive CI for incidents
  resolved.** A perfect environment detector gains +0.30 [-1.20, +1.85]
  (Phase 2 model) and +0.10 [-1.15, +1.35] (sensitivity model).
* **The oracle mission class changes nothing with the actions on** (identical
  to the baseline, +0.00 [+0.00, +0.00]): its only action is reinforcement, and
  no support team was ever free. With the actions off it lowers victims lost
  in the sensitivity model (-1.95 [-3.70, -0.35]) without a clear change in
  incidents; that is the old team-level reaction, not a Step 3 effect, and one
  interval of 20 seeds does not say why.
* Victims lost is higher with the actions on in the sensitivity model
  (14.90 vs 12.50 with them off: `ai_ship, actions off` -2.40 [-4.15, -0.80]).
  The actions delay some teams' arrival at incidents; this was not
  investigated further.

### Recommendation on thresholds

**Do not enable the environment or mission classes.** The criterion was a
positive paired CI for incidents resolved against the shipped AI setup, and no
condition meets it. With the new actions on, enabling either class is no
longer harmful, but it is not worth anything either, so the thresholds stay at
`FAULT_PREDICTOR_CLASS_OFF`. The model configuration was not changed.

## Recommendation (not implemented): make blocked roads consequential under causal

Under the causal profile only, I recommend that an active environment fault
actually slows a team that crosses the faulted cell, by a delay of about the
fault's severity (3 ticks), rather than only adding routing cost. Reasons and
expected effect:

* Today the fault has no physical consequence (finding 1), so it cannot carry
  a signal worth recovering from and no action on it can show a benefit.
* Real aftershock damage and debris persist for tens of minutes to hours until
  cleared, not 3-7 minutes; the Phase 2 backstop of 5-12 ticks is
  `combined`'s, not a causal value. A 60-150 tick backstop (36-90 minutes) is
  a more defensible assumption (assumed, no source).
* Measured with the two overrides above, together they cost the mission about
  3 incidents per seed (44.45 to 41.20, about 7%), which is the size of the
  problem the actions would be paid to solve. The rule-based actions recover
  at most about 1 incident of it (reroute only, +1.15 [-0.30, +2.55], not
  significant), so this needs the Step 3.5 planner to show a benefit.
* It restores the C13 signal (the legacy reaction becomes harmful again,
  -2.55 [-4.05, -1.05]), which is the property a trained environment class
  needs to be evaluated against.
* A slow-down is safer than a hard block: setting `navigable = 0` cut the
  graph in the combined profile and left teams with no route to a safe zone.

It is a change to the approved Phase 2 model, so it is not made here; both
parts are already available as sensitivity overrides.


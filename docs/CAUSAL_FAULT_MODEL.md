# Causal fault model (`--fault-profile=causal`)

The existing profiles (`none`, `mobility`, `communication`, `resource`,
`combined`, `random`) are unchanged: their datasets are byte-identical to
the build before this work. Everything here lives behind
`--fault-profile=causal`.

Faults under `causal` are consequences of simulated state rather than
per-tick dice rolls, and they end when their cause is removed. The old
duration ranges remain only as maximum backstops (communication has none).

## Determinism

Every causal draw is a hash of `(seed, tick, team-or-incident, salt)`
(`causal_hash()` in `core/fault_injector.c`, same construction as
`environment.c`'s `deterministic_hash()`). The causal code never calls
`random_*` and never touches `STREAM_FAULTS`, `STREAM_EQUIPMENT` or
`STREAM_WORK_RATE`. `make check-causal-profile` runs a causal run twice and
requires the main dataset and the sidecar to be byte-identical.

## Stress multiplier

`--fault-stress=<x>` (default 1, valid `0 < x <= 1000`, causal only; any other
profile rejects it). 1 means realistic field rates. The value is printed at
start (`[FAULT] profile=causal fault_stress=...`), written to every sidecar
row, folded into the causal `run_id`, and printed by
`laptop/tools/fault_exposure.py`.

It scales **only** the mobility and resource hazards and the Omori rate `K`.
It does not scale the link-margin model (deterministic physics) and it does
not scale the mission clock: incident ageing, victim survival and escalation
always run in real time (1 tick = 36 s), at every stress value.

Real robots fail roughly every 6-24 h (Carlson & Murphy 2005, as specified for
this task). One tick is `TICK_TIME_SEC` = 36 s (`core/mission_state.h:198`),
so 100 ticks = 1 h and a 3000-tick run is 30 h. At stress 1 the mobility and
resource classes together produce about one fault per team per 6.3 h
(about 1200 onsets over 20 seeds x 10 teams x 30 h at the Phase 2.1 constants), the short end of that range.
Per-tick rates in the legacy profiles (7-14% per tick) are orders of magnitude
above this, which is why evaluation runs need a stress value.

**Recommended evaluation stress: 7.** See "Results" for what it does and does
not match.

## Behaviour per class

| Class | Onset | Cleared by |
|---|---|---|
| mobility | hazard `h = base + k_wear * wear + k_rough * roughness(cell)` per tick, `p = 1 - exp(-h * stress)` | field repair (4 ticks RESTING at a safe zone), or the backstop. A field repair removes 60% of wear; a backstop clear removes none. |
| communication | link margin < 0 dB at the team's cell (deterministic) | link margin > +3 dB (hysteresis). Nothing else; no backstop. |
| resource | hazard `h = base + k_batt * (1 - equip/100)^2 + k_rest * min(since_rest/800, 2)`, times 1.5 while a mobility fault is active | resting with equipment >= 90 and stamina >= the soft floor, or the backstop |
| environment | Omori-Utsu aftershock on the team's route | backstop only (no action removes the cause yet) |
| mission | the team's assigned incident loses a victim or escalates | the incident resolves or the team leaves it, or the backstop |

No PAR confirmation delay and no post-repair immunity apply in the causal
profile. A team has at most 3 concurrent faults, and hazards are capped at
0.25 per tick. Communication faults are exempt from the concurrency cap
because they are a state, not an event.

### Communication: link margin

```
margin = tx - body - PL(d) - shadow(cell) - fade(cell, t) - building - rubble - rx_sens
PL(d)  = PL0 + 10 * n * log10(d / d0)
```

`d` is the distance to the nearest receiver (evacuation gateway or safe zone).
`shadow` is zero-mean Gaussian with sigma 7.9 dB, drawn from a hash of
`(seed, cell)`, so dead zones are stable across ticks and visits. `fade` is a
slow time-varying term of +/-2.5 dB: a hash value per (cell, 20-tick window),
interpolated linearly between window boundaries, so it drifts instead of
jumping. A fault starts when the margin falls below 0 dB and clears only when
it is back above +3 dB. Fault severity (heartbeat cadence) is
`1 + deficit_dB / 6`, capped at 4.

Hysteresis and fading did **not** remove short comms faults (see Results and
Known limitations): shadowing is independent from one cell to the next, so a
team crossing one bad cell gets a 1-tick fault.

### Mobility: hidden wear

Wear accumulates per cell crossed (more on rough ground). It is not a feature
and not written to the main dataset. Repair removes a fraction of it rather
than resetting it: a field repair (a mobility fault cleared by 4 ticks resting
at a safe zone) removes 60%, and a safe-zone rest of at least 4 ticks with no
mobility fault open removes 30% (routine maintenance). A fault that only runs
out its backstop leaves wear untouched. Its only visible symptom is an
occasional skipped step once wear exceeds 3 (the AI sees this as a speed
drop).

### Environment: Omori-Utsu

World events at rate `R(t) = K * stress / (c + t)^p`, `t` = ticks since the
mainshock. An event picks a cell by hash; every team whose route (own cell
plus up to 8 path steps) comes within 3 cells takes an environment fault on
the first such cell. This reuses the existing cell-cost/`road_blocked`
mechanism, so the existing road-degradation hazard keeps raising the cost
while the road stays flagged.

### Mission: survival and escalation

Victims in each open incident die at the hazard of a Weibull survival curve
fitted to two MacIntyre 2006 points: 74% alive at 24 h and 22% at 72 h.
**To verify against the paper:** these two figures were supplied with the task
and have not been checked against MacIntyre et al. 2006; the fit
(`lambda = 54.9 h`, `k = 1.47`) is only as good as they are. Priority steps one
level toward CRITICAL every 12 h of incident age (assumed). Both are
irreversible, and the old "restore the original priority and victim count on
recovery" is disabled under causal.

Time-scale mapping: exactly 36 s per tick (`TICK_TIME_SEC`), no compression
and no stress scaling. A 3000-tick run is 30 h, so an incident that waits the
whole run reaches an age of 30 h.

### Resource-drain damping

A resource fault drains stamina and equipment (`core/team.c`). Under causal
the drain stops at stamina 25 and equipment 25 (above the 20 that triggers an
equipment-cache request), so a fault cannot push a team deeper into the
depletion that raises the next resource hazard.

## Calibration table

| Parameter | Value | Source |
|---|---|---|
| tx power | 14 dBm | `LORA_TX_POWER_DBM`, `core/lora_radio_params.h:212` |
| spreading factor / bandwidth | SF7 / 125 kHz | `core/lora_radio_params.h:190-191` |
| rx sensitivity | -123 dBm | task specification for SF7 |
| PL0 (free space at d0 = 1 m, 865 MHz) | 31.2 dB | derived: `20 log10(4 pi d0 / lambda)` |
| path-loss exponent n | 2.35 | task specification (range 2.3-2.4), midpoint |
| shadowing sigma | 7.9 dB | task specification |
| body/antenna loss | 5 dB | assumed |
| building penalty | 20 dB at density 100 | assumed |
| rubble penalty | 10 dB at infrastructure damage 100 | assumed |
| cell size | 100 m | assumed (the grid has no physical scale) |
| comm severity step | 6 dB per level | assumed |
| comm clear hysteresis | +3 dB | assumed |
| slow fading amplitude / window | +/-2.5 dB / 20 ticks (12 min) | assumed |
| wear per cell / extra at roughness 1 | 0.002 / 0.010 | assumed |
| mobility hazard base | 1/3000 per tick | assumed, set so total stress-1 MTBF is ~6 h (Carlson & Murphy 2005 range 6-24 h) |
| mobility hazard per wear / roughness | 0.0008 / 0.002 per tick | assumed; per-wear value tuned so wear is the attributed cause of 30-60% of mobility onsets at the evaluation stress |
| wear removed by field repair / by safe-zone rest | 60% / 30% | assumed |
| wear symptom start / slope / max | 3 / 0.10 / 0.5 | assumed |
| resource hazard base | 1/3000 per tick | assumed (as mobility base) |
| resource hazard battery / since-rest | 0.004 / 0.0005 per tick, horizon 800 ticks | assumed |
| mobility-to-resource cascade | x1.5 | assumed |
| resource drain floors | stamina 25, equipment 25 | assumed |
| Omori p | 1.1 | task specification |
| Omori c | 10 ticks (6 min) | assumed |
| Omori K | 28 events per tick at (c+t) = 1 | assumed; gives ~39 events per 3000-tick run at stress 1 |
| mainshock-to-start offset | 200 ticks (2 h) | assumed |
| aftershock radius / route look-ahead | 3 cells / 8 steps | assumed |
| survival Weibull lambda / k | 54.9 h / 1.47 | fit to the 74% at 24 h / 22% at 72 h points given in the task, attributed to MacIntyre 2006 - **to verify against the paper** |
| escalation interval | 12 h | assumed |
| repair dwell | 4 ticks RESTING | assumed |
| resupply level | equipment >= 90 | assumed (the fault's own drain holds equipment just under 100) |
| max concurrent faults / hazard cap | 3 / 0.25 per tick | assumed |
| fault backstops | mobility/comm/resource 6-14, environment 5-12, mission 5-12 ticks | unchanged from `combined` |

All parameters live in `g_causal_params` (`core/fault_config.c`).

## Causal ground-truth sidecar: `tron_causal_truth.csv`

Written only under `--fault-profile=causal`, in the working directory next to
`tron_snapshot_dataset.csv`. One row per `(world_seed, cycle, team_id)` for
every tick. Join to the main dataset on `(world_seed, cycle, team_id)`; `run_id`
is also present and matches the main dataset's `run_id`. The main dataset
schema is unchanged and nothing in `ml/` reads this file. When a forecaster is
trained on it, add these columns to `GROUND_TRUTH_COLUMNS` in
`ml/data_prep.py` at that point so they cannot become features.

| Column | Meaning |
|---|---|
| `run_id`, `world_seed`, `cycle`, `team_id` | join key |
| `fault_stress` | the `--fault-stress` value used |
| `hidden_wear` | accumulated mobility wear (reset by repair) |
| `link_margin_db` | link margin at the team's cell, dB |
| `hazard_mobility`, `hazard_resource` | per-tick hazards that tick (after stress and cap) |
| `ticks_since_rest` | ticks since the team was last RESTING |
| `aftershock_hit` | 1 if an aftershock fault was raised on this team this tick |
| `onset_cause_<class>` | cause of a fault that started this tick, else 0 |
| `clear_cause_<class>` | how a fault ended this tick: 0 none, 1 cause removed, 2 backstop |

`onset_cause` codes: 0 none, 1 baseline, 2 wear, 3 roughness, 4 battery,
5 time since rest, 6 link margin, 7 aftershock, 8 incident deterioration.
Rows are per class: `mobility`, `communication`, `resource`, `environment`,
`mission`.

## Results (seeds 1001-1020, 3000 ticks, AI off)

Onset is episodes per team per 1000 ticks; exposure is the share of
team-ticks with the class active; durations are in ticks. Causal numbers are
from the Phase 2.1 model.

| Class | combined: onset / exposure / mean dur | causal x7 (recommended): onset / exposure / mean dur | causal x1: onset / exposure / mean dur |
|---|---|---|---|
| mobility | 5.5 / 0.234 / 42.5 | 5.8 / 0.044 / 7.6 | 1.3 / 0.010 / 7.7 |
| communication | 5.5 / 0.235 / 42.5 | 8.1 / 0.012 / 1.5 | 8.8 / 0.013 / 1.5 |
| resource | 5.5 / 0.234 / 42.5 | 4.8 / 0.038 / 7.9 | 0.7 / 0.006 / 8.4 |
| environment | 6.6 / 0.056 / 8.4 | 2.2 / 0.019 / 8.5 | 0.3 / 0.003 / 8.5 |
| mission | 2.9 / 0.119 / 41.4 | 0.1 / 0.000 / 7.1 | 0.1 / 0.001 / 7.4 |
| any fault (exposure) | 0.577 | 0.108 | 0.032 |
| victims lost / seed | n/a (no survival model) | 10.8 of 228.4 (4.7%) | 10.7 of 236.4 (4.5%) |
| incidents resolved / seed | 36.45 | 44.95 | 46.55 |

Victims lost is almost independent of stress because the mission clock is
real time and no longer scales with `--fault-stress`.

### Mobility onset attribution (share of mobility onsets by dominant hazard term)

| Stress | wear | baseline | roughness |
|---|---|---|---|
| 1 | 86% | 6% | 8% |
| 6 | 55% | 25% | 20% |
| 7 (recommended) | 53% | 27% | 21% |
| 8 | 49% | 29% | 22% |

The Phase 2 constants gave wear 10% at stress 8. The per-wear hazard was
raised from 0.0002 to 0.0008 and repair changed from a reset to a 60%/30%
reduction; this was tuned to the 30-60% target, on the first attempt, so it is
a tuned value and not a measured one. Resource onsets: battery 60%, baseline
40% (time since rest never dominates).

### Communication before and after hysteresis and fading (stress 8, seeds 1001-1020)

| | Phase 2 (clear at margin > 0, no fading) | Phase 2.1 (clear at > +3 dB, +/-2.5 dB fading) |
|---|---|---|
| mean fault duration (ticks) | 1.4 | 1.6 |
| share of faults lasting 1-2 ticks | 97% | 94% |
| longest fault (ticks) | 32 | 41 |

The change was not effective. On seed 1001, 243 of 248 faults of 2 ticks or
fewer occurred while the team was moving, and the median link-margin step
between adjacent cells is 7.4 dB. Shadowing is drawn independently per cell,
so a team that crosses one bad cell gets a 1-tick fault. Spatially correlated
shadowing would address the cause; it is not implemented.

### Are the onset levels comparable?

At stress 7, partly.

* Mobility (1.05x) and resource (0.87x) onsets are close to `combined`.
* Communication onsets are 1.5x higher than `combined` and cannot be tuned
  with stress; most are 1-2 tick blips (above).
* Environment (0.33x) and mission (0.03x) onsets are well below `combined`.
  Matching them would push mobility and resource far past `combined`.
* Exposure is not comparable at any stress: causal faults clear in about 8
  ticks, `combined` faults last about 42, so exposure is several times lower
  even where onsets match.

The difference in incidents resolved should not be read as the effect of
"realism": it comes from lower exposure, a different mission model (victims
now die; incidents escalate) and different onset mixes at once.

## Known limitations

* Mission faults are almost absent (0.1 onsets per team per 1000 ticks, vs 2.9
  in `combined`) now that the survival clock runs in real time: a mission
  fault needs a victim death or an escalation on the incident a team is
  working. A model trained on this profile will see very few mission faults.
* Most communication faults are 1-2 tick blips caused by uncorrelated
  per-cell shadowing; hysteresis and fading did not change that.
* The wear hazard constants were tuned to hit a target attribution share.
* Time since rest never dominated a resource onset; battery depletion and
  baseline account for all of them.
* Link-margin shadowing is static per cell (fading aside) and there is no
  multi-hop mesh.
* The cell size, building and rubble penalties, and most hazard constants are
  assumed, not measured.
* The MacIntyre 2006 survival points are unverified against the paper.
* The board build compiles `fault_injector.c`, so the libm-using model is
  compiled out there (`ENVIRONMENT_HAS_WORLD_STORAGE`); the ARM build checks
  could not be run in this environment (no `arm-none-eabi-gcc`).

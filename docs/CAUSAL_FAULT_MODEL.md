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

It scales: mobility and resource hazards, the Omori rate `K`, and the mission
clock (incidents age `x` times faster). It does **not** scale the link-margin
model, which is deterministic physics.

Real robots fail roughly every 6-24 h (Carlson & Murphy 2005, as specified for
this task). One tick is `TICK_TIME_SEC` = 36 s (`core/mission_state.h:198`),
so 100 ticks = 1 h and a 3000-tick run is 30 h. At stress 1 the mobility and
resource classes together produce about one fault per team per 6.3 h
(956 onsets over 20 seeds x 10 teams x 30 h), the short end of that range.
Per-tick rates in the legacy profiles (7-14% per tick) are orders of magnitude
above this, which is why evaluation runs need a stress value.

**Recommended evaluation stress: 8.** See "Results" for what it does and does
not match.

## Behaviour per class

| Class | Onset | Cleared by |
|---|---|---|
| mobility | hazard `h = base + k_wear * wear + k_rough * roughness(cell)` per tick, `p = 1 - exp(-h * stress)` | field repair (4 ticks RESTING at a safe zone), or the backstop. Repair resets wear; a backstop clear does not. |
| communication | link margin < 0 at the team's cell (deterministic) | link margin > 0 again. Nothing else; no backstop. |
| resource | hazard `h = base + k_batt * (1 - equip/100)^2 + k_rest * min(since_rest/800, 2)`, times 1.5 while a mobility fault is active | resting with equipment >= 90 and stamina >= the soft floor, or the backstop |
| environment | Omori-Utsu aftershock on the team's route | backstop only (no action removes the cause yet) |
| mission | the team's assigned incident loses a victim or escalates | the incident resolves or the team leaves it, or the backstop |

No PAR confirmation delay and no post-repair immunity apply in the causal
profile. A team has at most 3 concurrent faults, and hazards are capped at
0.25 per tick. Communication faults are exempt from the concurrency cap
because they are a state, not an event.

### Communication: link margin

```
margin = tx - body - PL(d) - shadow(cell) - building - rubble - rx_sens
PL(d)  = PL0 + 10 * n * log10(d / d0)
```

`d` is the distance to the nearest receiver (evacuation gateway or safe zone).
`shadow` is zero-mean Gaussian with sigma 7.9 dB, drawn from a hash of
`(seed, cell)`, so dead zones are stable across ticks and visits. Fault
severity (heartbeat cadence) is `1 + deficit_dB / 6`, capped at 4.

### Mobility: hidden wear

Wear accumulates per cell crossed (more on rough ground) and is reset only by
repair. It is not a feature and not written to the main dataset. Its only
visible symptom is an occasional skipped step once wear exceeds 3 (the AI
sees this as a speed drop).

### Environment: Omori-Utsu

World events at rate `R(t) = K * stress / (c + t)^p`, `t` = ticks since the
mainshock. An event picks a cell by hash; every team whose route (own cell
plus up to 8 path steps) comes within 3 cells takes an environment fault on
the first such cell. This reuses the existing cell-cost/`road_blocked`
mechanism, so the existing road-degradation hazard keeps raising the cost
while the road stays flagged.

### Mission: survival and escalation

Victims in each open incident die at the hazard of a Weibull survival curve
fitted to MacIntyre 2006: 74% alive at 24 h and 22% at 72 h (the figures given
for this task; not independently re-checked against the paper). That fit gives
`lambda = 54.9 h`, `k = 1.47`. Priority steps one level toward CRITICAL every
12 h of (stress-scaled) age. Both are irreversible, and the old "restore the
original priority and victim count on recovery" is disabled under causal.

Time-scale mapping: ticks to hours is exactly 36 s per tick, with no
compression. Stress `x` makes incidents age `x` times faster, so stress 8
means 30 simulated hours are experienced as 240 h of incident age.

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
| wear per cell / extra at roughness 1 | 0.002 / 0.010 | assumed |
| mobility hazard base | 1/3000 per tick | assumed, set so total stress-1 MTBF is ~6 h (Carlson & Murphy 2005 range 6-24 h) |
| mobility hazard per wear / roughness | 0.0002 / 0.002 per tick | assumed |
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
| survival Weibull lambda / k | 54.9 h / 1.47 | fit to MacIntyre 2006 points given in the task (74% at 24 h, 22% at 72 h) |
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
team-ticks with the class active; durations are in ticks.

| Class | combined: onset / exposure / mean dur | causal x8: onset / exposure / mean dur | causal x1: onset / exposure / mean dur |
|---|---|---|---|
| mobility | 5.5 / 0.234 / 42.5 | 4.9 / 0.038 / 7.8 | 0.9 / 0.007 / 7.6 |
| communication | 5.5 / 0.235 / 42.5 | 8.0 / 0.011 / 1.4 | 7.2 / 0.010 / 1.4 |
| resource | 5.5 / 0.234 / 42.5 | 5.5 / 0.043 / 7.9 | 0.7 / 0.006 / 8.1 |
| environment | 6.6 / 0.056 / 8.4 | 2.6 / 0.022 / 8.5 | 0.3 / 0.003 / 8.7 |
| mission | 2.9 / 0.119 / 41.4 | 0.7 / 0.005 / 7.3 | 0.1 / 0.001 / 7.3 |
| any fault (exposure) | 0.577 | 0.112 | 0.025 |
| incidents resolved / seed | 36.45 | 44.80 | 44.70 |

Under causal, victims lost per seed: 118.3 of 221.7 at x8 (53.4%), 11.2 of
241.1 at x1 (4.6%).

### Are the onset levels comparable?

Only partly.

* Mobility (0.9x) and resource (1.0x) onsets at stress 8 are close to
  `combined`.
* Communication onsets are 1.45x higher than `combined` and cannot be tuned
  with stress; they are set by where the link margin crosses zero. Most
  episodes are 1-2 ticks long (a team crossing a marginal cell).
* Environment (0.4x) and mission (0.24x) onsets are well below `combined`.
  Raising stress enough to match them would push the mobility, resource and
  mission models far past `combined` (stress 16 gives mobility 8.4 and
  resource 9.6 per team per 1000 ticks).
* Exposure is not comparable at any stress: causal faults clear in about 8
  ticks, `combined` faults last about 42, so even at matched onset the causal
  exposure is several times lower.

The difference in incidents resolved should not be read as the effect of
"realism": it comes from lower exposure, a different mission model (victims
now die; incidents escalate) and different onset mixes at once.

## Known limitations

* The wear term rarely triggers a fault with the current constants. At stress
  8 on seeds 1001-1003, 33 of 451 mobility onsets were attributed to wear
  (259 baseline, 159 roughness), because repair keeps resetting wear (maximum
  wear observed 3.5). A forecaster will find mobility faults mostly
  unpredictable from wear; changing that means changing the assumed
  constants, which was not done silently.
* Time since rest never dominated a resource onset; battery depletion
  (291 of 501) and baseline (210) did.
* The 8x mission clock at stress 8 is aggressive: 53% of victims are lost per
  seed. Stress 1 gives 4.6%.
* Link-margin shadowing is per cell and static; there is no temporal fading
  and no multi-hop mesh.
* The cell size, building and rubble penalties, and most hazard constants are
  assumed, not measured.
* The board build compiles `fault_injector.c`, so the libm-using model is
  compiled out there (`ENVIRONMENT_HAS_WORLD_STORAGE`); the ARM build checks
  could not be run in this environment (no `arm-none-eabi-gcc`).

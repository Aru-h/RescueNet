# Scale audit (Step 5, tier 0, SA): what physical size is one grid cell?

> **Owner decision (Oct 27, see `DECISIONS.md` D15):** the design scale is **10 m per cell** (about 13 ha), so a team moves
> 1 km/h, a loaded foot team on rubble. The stamina model agrees: full stamina to the arduous floor at 0.110 per travel
> tick is 727 ticks, 7.3 h, 7.3 km at 10 m (but 73 km at 100 m). This supersedes the audit's recommendation to keep the scale
> abstract. The radio model's 100 m per cell and the 4-tick debris stall (derived from 100 m) are the parts that remain
> inconsistent with it.

Read-only audit. No code or default was changed. Measured figures come from one no-fault run (`--fault-profile=none`,
seed 1001, 3000 ticks) and one v1-world run (stress 2, seed 1001) using the dataset's per-team rows; computed figures
are marked "computed". Anything from outside the repository is marked "to verify" (the network proxy blocked the
primary pages in earlier steps, so I cite from memory of the literature and have not re-fetched).

## 1. The two scales in the code

| Where | Value | Used by |
|---|---|---|
| `core/mission_state.h:196` `CELL_SIZE_METERS` | 10 | **nothing** (grep finds no other use) |
| `core/fault_config.c:20` `.cell_size_m` | 100 | the causal radio model only (`fault_injector.c`, distance to the nearest safe zone or gateway) |
| `GRID_WIDTH` x `GRID_HEIGHT` | 36 x 36 | everything |
| `TICK_TIME_SEC` | 36 s (100 ticks per hour) | everything |

So the same grid is 360 m x 360 m (13 ha, computed) in one reading and 3.6 km x 3.6 km (1300 ha) in the other.

I searched the repository (docs, code, README, dashboard JS, Python tools) for "hectare", "ha", "square km", "metre",
"m/s" and "km". **No "11 hectares" (or any area) claim exists in the repository**; the 13 ha figure above is derived from
10 m x 36 x 36. The only physical-scale statements are in `docs/CONSEQUENTIAL_ROADS.md` (a "100 m cell crossed at about
0.5 m/s", assumed) and the `cell_size_m = 100` radio parameter. The dashboard JS shows no scale bar or metre label.

## 2. What the dynamics imply (measured)

* A team moves **at most one cell per tick**, always: in the no-fault run every recorded step was one cell, and a team
  moved on 81 percent of its en-route ticks (73 percent in the v1 world, the rest are debris stalls and holds).
* Diagonal moves cost the same one tick (8-way), so the map is crossed in about **36 ticks = 21.6 minutes** corner to
  corner along a diagonal and 36 ticks along an edge.
* Trip to an incident: mean 16.9 ticks (10 min), median 15, p90 34 ticks (20 min), n = 162 trips (no-fault run); v1
  world 16.5 mean. Trips to rest are shorter (the team is usually close): mean 4-6 ticks, median 1-2 (many are teams
  already standing at a safe zone).
* Time budget on that run: resting 43.9 percent, en route 33.7, on scene 17.7, available 4.8 (v1 stress 2: 45.4, 37.3,
  13.9, 3.4). The brief's figures (44, 34, 18, 5) are right.

## 3. Implied physical values

Implied speed = cell size / 36 s.

| Reading | Cell | Map | Implied team speed | Trip to incident (mean 17 ticks) | Map crossing (36 ticks) |
|---|---|---|---|---|---|
| A | 10 m | 360 m, 13 ha | **0.28 m/s = 1.0 km/h** | 170 m in 10 min | 360 m in 22 min |
| B | 100 m | 3.6 km, 1300 ha | **2.8 m/s = 10 km/h** | 1.7 km in 10 min | 3.6 km in 22 min |
| C (abstract, calibrate by speed) | undefined | undefined | choose a speed, e.g. 1 km/h loaded foot team on rubble, derive the cell | follows | follows |

Sanity (to verify): a loaded USAR team on foot over rubble moves roughly 1-2 km/h (A is at the slow end); 10 km/h is a
fast jog or a vehicle on a debris-strewn street (B is plausible for vehicles, implausible for foot teams).

## 4. Every quantity that depends on a physical scale

| Quantity | Where | In cells / ticks | At 10 m | At 100 m | Which reading it was built for |
|---|---|---|---|---|---|
| Movement | `simulation.c` `move_team` | 1 cell per tick | 1.0 km/h | 10 km/h | either |
| Debris crossing delay | `fault_config` roads (4 ticks, x3 cost) | 4 extra ticks per partial cell | 144 s extra per 10 m = 0.07 m/s through debris | 144 s extra per 100 m cell, with the doc's "100 m at 0.5 m/s" | **100 m** (the doc says so) |
| Debris clearing | 120 / 240 work units | 12 / 24 ticks per cell | 7 / 14 min per 10 m of lane | 7 / 14 min per 100 m of road | 10 m (a lane through a block in minutes) |
| Radio path loss | `fault_injector.c` | distance cells x `cell_size_m` | link margin stays positive, see below | margin crosses 0 at 18+ cells | **100 m** |
| Search sweep radius | `SEARCH_SWEEP_RADIUS` 3 | 3 cells | 30 m (dogs, acoustic) | 300 m | 10 m |
| Other-team reveal radius | `OTHER_TEAM_REVEAL_RADIUS` 2 | 2 cells | 20 m | 200 m | 10 m |
| Aftershock damage radius | `aftershock_radius_cells` 3 (|dx|+|dy|) | 3 cells | 30 m | 300 m | 10 m (damage to nearby roads) |
| Fire spread | `fire_spread_interval_ticks` 25, to the 4 neighbour cells | 1 cell per 25 ticks (15 min) | 10 m per 15 min between adjacent buildings | 100 m per 15 min | 10 m (building to building) |
| Gas escalation | every 40 ticks | n/a | | | scale free |
| Building footprints | 1-3 cells | | 10-30 m (a house, a block) | 100-300 m (a stadium) | **10 m** |
| Roads | one cell wide | | 10 m street | 100 m wide "road" | **10 m** |
| Field repair | 34-102 ticks held in place | | scale free (time only) | | scale free |
| Go-home repair | a round trip of 40-70 ticks | | 40-70 ticks x 10 m = 400-700 m total | 4-7 km | **depends on the safe-zone spacing** |
| Safe zones, gateways | 4 safe zones, a few gateways | positions on the grid | within 360 m | within 3.6 km | either |
| Dashboard | `laptop/tools/static/dashboard/*.js` | cells | no scale label shown | | not applicable |
| Stamina and equipment | per tick | | scale free | | scale free |

Radio link budget (computed from `fault_config.c`: TX 14 dBm, body loss 5 dB, PL0 31.2 dB at 1 m, exponent 2.35,
sensitivity -123 dBm, building penalty up to 20 dB, rubble up to 10 dB, shadowing sigma 7.9 dB):

| Distance in cells | margin at 10 m (dB, minus 30 dB worst-case penalties) | margin at 100 m (dB, minus 30) |
|---|---|---|
| 5 | 60.9 (30.9) | 37.4 (7.4) |
| 18 | 47.8 (17.8) | 24.3 (-5.7) |
| 36 | 40.7 (10.7) | 17.2 (-12.8) |

At 10 m the margin stays above +10 dB even with all penalties, so communication faults (margin below 0) would be
produced only by shadowing 1.4 sigma or more below the mean in the worst cells; at 100 m they occur in dense, damaged
areas far from a safe zone, which is what the Step 2 model was designed to show. **The communication-fault model of the
v1 world therefore depends on the 100 m reading.** Everything else that has a physical anchor (buildings, roads, fire
spread, search and aftershock radii) points to about 10 m, and the movement speed of 1 cell per tick is physical only
at 10 m for a foot team (1 km/h) and at 100 m for a vehicle (10 km/h).

## 5. Verdict and recommendation

The model is **internally inconsistent in scale** (geometry and spread at 10 m; radio, debris delay derivation and the
vehicle-like speed at 100 m), and `CELL_SIZE_METERS` documents a value no code reads. Nothing in the v1 results
depends on the inconsistency being resolved in either direction, because every result is expressed in ticks and
cells, but any claim in physical units is unsupported.

Options: (1) keep as is and document the scale as abstract with an explicit calibration; (2) adopt 10 m with a
different tick or speed model; (3) adopt 100 m and fix the geometry claims; (4) other.

**Recommendation: (1) now, then (2) for v2.**

* Now (no world change, documentation only): state in the README, `CAUSAL_FAULT_MODEL.md` and `CONSEQUENTIAL_ROADS.md`
  that the grid is **abstract**: one cell per tick is a speed calibration, not a measured length; list per parameter
  which reading it was calibrated for (the table above); and either remove `CELL_SIZE_METERS` or comment that it is
  unused. Do not quote hectares, kilometres or m/s anywhere.
* For v2 (a world change, out of scope here): adopt **10 m** because the geometry (street width, building size, fire
  spread between adjacent buildings, a 30 m search radius) is the part of the model that can be checked against the
  real world. That needs (a) a speed model, since 1 cell per tick is 1 km/h: terrain-dependent speeds or several
  cells per tick for vehicles, which is also what a tool or transport model (V1, v2 equipment tiers) needs; (b) a radio
  model whose distance is the real distance to a relay or the base, with the range set by the radio (a 360 m map is
  within LoRa range, so distance-driven faults vanish and shadowing and building loss remain); (c) a re-derived debris
  delay (partial debris of about 10 m crossed at 0.5 m/s is 20 s, below one tick, so the stall is better modelled as
  a clearing time).

## 6. Statements and files to correct

| File | Statement | Correction |
|---|---|---|
| `core/mission_state.h:196` | `CELL_SIZE_METERS 10` (unused) | comment as unused and abstract, or remove |
| `core/fault_config.c:20`, `fault_config.h:63` | `cell_size_m = 100` | note it is the radio model's own calibration, not a map scale |
| `docs/CONSEQUENTIAL_ROADS.md` (Assumptions) | "a 100 m cell crossed at about 0.5 m/s" | say it assumes the 100 m reading; the geometry does not |
| `docs/CAUSAL_FAULT_MODEL.md` | radio model parameters | state the 100 m assumption beside the link budget |
| `docs/SPECIALIZATION_LOAD.md` and other docs | any "city", "block" or distance wording | use ticks and cells |
| `README.md` | "procedurally generated, earthquake-damaged city" | acceptable; add "abstract scale" when a size is first implied |
| anywhere | "11 hectares" | **not present in the repository**; 13 ha is derived (10 m reading) |

No dashboard string needs changing (it shows no scale).

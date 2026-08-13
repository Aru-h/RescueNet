# TRON Simulator Architecture

## Overview

The simulator now has a procedural environment subsystem that sits beneath the existing incident, routing, CBBA, snapshot, and metrics flows.

The compatibility boundary is unchanged: teams, incidents, safe zones, system tasks, CBBA bids, and snapshots still use grid coordinates. The environment layer enriches those coordinates with district, road, building, infrastructure, safety, travel-cost, and damage metadata.

## World Generation Flow

`generate_world(seed)` performs the run setup in this order:

1. Seed all deterministic random streams.
2. Generate the procedural environment.
3. Initialize incident and system-task pools.
4. Place safe zones on environment-safe candidate cells and register them as infrastructure.
5. Place evacuation gateways and register them as infrastructure.
6. Apply the earthquake event to buildings, roads, and registered infrastructure.
7. Initialize the safety grid from damaged environment base safety.
8. Place teams at generated safe zones.
9. Initialize evacuation state.

This preserves the existing simulator lifecycle while making all downstream behavior environment-aware.

## Environment Subsystem

The subsystem is implemented in `environment.c` and `environment.h`.

It generates:

- Districts: rescue base, residential, industrial, medical, civic, and waterfront.
- Road network: arterial, collector, and local road classes.
- Buildings: typed structures with district and risk metadata.
- Safe-zone candidates: preferred cells near rescue, medical, and arterial-road infrastructure.
- Navigability metadata: per-cell travel costs and route availability.
- Damage state: building damage, road damage/blockage, infrastructure damage, and spawned-incident masks.

The environment is stored in `g_environment[GRID_WIDTH][GRID_HEIGHT]`, with building records in `g_environment_buildings`.

## Earthquake Event

`environment_apply_earthquake(seed)` assigns an epicenter and intensity for each generated world. Damage is deterministic for the world seed and is influenced by:

- District vulnerability: industrial and waterfront districts amplify damage, while rescue and medical districts are more resilient.
- Construction quality: each building has a quality rating generated from its district profile.
- Distance from epicenter: damage attenuates with Manhattan distance from the earthquake epicenter.

The earthquake mutates persistent object state:

- Buildings receive `damage_state`.
- Road cells receive `road_damage` and may be flagged as `road_blocked`.
- Infrastructure cells receive `infrastructure_damage`.

Blocked roads remain technically navigable but carry a high travel cost. This keeps routing and CBBA compatible while making teams avoid blocked corridors when an alternative exists.

## Environment-Aware Routing

`path.c` still exposes the existing `find_next_path_step(...)` function used by movement and CBBA scoring. Internally, it now asks the environment for:

- Whether a cell is navigable.
- How expensive a cell is to enter.

Roads are cheaper than blocks/buildings, so crews naturally prefer the road network while still being able to reach incidents inside built-up areas.

## Environment-Aware Incidents

Generated incidents now choose damaged source objects through `environment_find_incident_site(...)`.

Incident type influences source selection:

- Structural collapse and confined-space incidents originate from severely or moderately damaged buildings, with blocked roads as fallback collapse sources.
- Chemical exposure and gas leaks originate from damaged industrial buildings or industrial infrastructure.
- Mass casualty events originate from damaged residential, medical, civic, or infrastructure objects.
- Fire incidents originate from damaged buildings or severely damaged industrial road/infrastructure cells.
- Search and survivor incidents originate from damaged non-rescue-base buildings or blocked road cells.

When a damaged object emits an incident type, that object records a spawned-incident mask so the same source does not repeatedly emit the same incident type. Incident records are otherwise unchanged, preserving CBBA, snapshots, scoring, and metrics compatibility.

## Environment-Aware Safe Zones And Gateways

Safe zones are generated on environment-selected candidate cells, then teams spawn from those safe zones as before.

Evacuation gateways are now aligned to the generated road network edges when possible. Existing gateway and offboard system-task structures are unchanged.

## Compatibility Notes

- CBBA still consumes `IncidentState_t` and `TaskRef_t`.
- Snapshots keep the same CSV schema.
- Metrics code remains compatible with existing incident/team state transitions.
- Existing simulator entry point remains `./tron`, with an optional fault profile flag: `./tron [first_seed] [seed_count] [ticks_per_seed] [--fault-profile profile]`.
- Legacy coordinate-based code can still use `find_next_path_step(...)`; it is now environment-aware behind the same signature.

## Operational Fault Injection

The simulator includes a generic operational fault subsystem that models effects rather than causes.

Fault profiles are selected with `--fault-profile` and can enable mobility, communication, resource, environment, or mission effects. The profiles are deterministic per seed and use the dedicated fault random stream so that repeated runs are reproducible.

The fault injector updates shared team fault state and may:

- Slow or stall movement.
- Delay heartbeat visibility.
- Degrade stamina or equipment.
- Temporarily alter an environment cell's operational state.
- Temporarily perturb an active incident's priority or victim load.

The watchdog observes the live team state against the mission execution twin and emits recommendations such as local replan, global replan, return to safe zone, or request support. Simulation applies those recommendations; the watchdog itself does not directly mutate the plan.

Fault events are recorded through the existing snapshot event channel, so the original CSV schema remains unchanged.

## Offline Snapshot Labeling

The repository includes a separate offline pipeline for the existing snapshot dataset. It reads `tron_snapshot_dataset.csv` and writes a new labeled CSV without changing the original snapshot export.

The label definition is:

- Group rows by `run_id` and `team_id`.
- For each snapshot, look ahead at most `N` ticks within the same run and team.
- Emit a positive label when a future snapshot is observed with `status == STATUS_AVAILABLE` and `stamina` at or above the ready-stamina threshold for the snapshot's `fitness_tier`.
- Emit a negative label otherwise.

The labeled output keeps the original snapshot columns and appends:

- `recovery_threshold_stamina`
- `recovery_ticks_to_available`
- `recovery_within_<N>_ticks`

Because the original snapshot export does not include `equipment_level`, the offline pipeline uses the observable recovered state (`STATUS_AVAILABLE`) as the recovery proxy while still recording the tier-specific stamina threshold for transparency.

The default workflow is:

```sh
make
make label_dataset N=5
```

You can also run the labeler directly:

```sh
./label_snapshots tron_snapshot_dataset.csv tron_snapshot_dataset_labeled.csv 5
```

## Build And Smoke Test

Build:

```sh
make
```

Smoke test:

```sh
make smoke
```

The smoke test runs seed `1` for `200` ticks and prints an environment, team, incident, and safe-zone summary.

Fault-profile smoke test:

```sh
./tron 1 1 5 --fault-profile mobility
```

## Laptop / Board Platform Split

The codebase is split into three tiers:

- `core/` — the portable simulator (grid, environment, incidents,
  teams, CBBA allocation, A* pathfinding, scoring, watchdog, recovery
  manager, fault injection, random). No stdio/OS/filesystem
  dependencies. Verified freestanding-clean for Cortex-M55 by
  `make check-board`.
- `platform/laptop/` — desktop entry point (`main.c`, CLI parsing),
  printf-based debug sink (`debug.c`), CSV snapshot writer
  (`snapshot.c`).
- `platform/board/` — STM32N6570-DK entry point (`main_board.c`,
  µT-Kernel `usermain()`), `tm_printf`-based debug sink (`debug.c`),
  ring-buffer telemetry stub (`snapshot.c`, pending USB protocol
  design).

Both platforms share `core/sim_runner.h`, which owns the tick sequence
(`fault_injector_step -> allocate_all_incidents -> simulation_step`) as
a single source of truth, so this performance-sensitive ordering can't
drift between laptop and board.

The board files are verified against the real µT-Kernel 3.0 BSP2
headers (`github.com/tron-forum/mtk3_bsp2`, official STM32N6570-DK
support) via `make check-board` — see `vendor/README.md` for how to
fetch the BSP and what's been confirmed against it (FPU defaults,
debug UART routing, `tm_printf`'s no-float limitation, entry-point
shape). Not yet a flashable firmware image: that needs an STM32CubeIDE
project wrapping these sources, which `vendor/README.md` also covers.

# tron

A multi-team disaster-response simulator built for the **TRON
Programming Contest 2026** (September 2026 deadline). Five specialized
response teams — Search, Medical, Extraction, Perimeter, Support —
respond to incidents in a procedurally generated disaster environment,
coordinated entirely through the **Consensus-Based Bundle Algorithm
(CBBA)**: no central dispatcher, just agents broadcasting local beliefs
and reconciling conflicts through consensus. The project targets real
hardware — **µT-Kernel 3.0** on an **STM32N6570-DK** (STM32N657,
Cortex-M55) — and is built to run identically on a laptop for fast
iteration and on the actual board for field deployment.

## What makes this interesting

- **CBBA is implemented as a genuine message-passing protocol, not a
  simulation shortcut.** `CBBAMessage_t` (`core/cbba.c`) is explicitly
  documented in the source as *"wire format only"* — a 116-byte struct
  shaped like something you'd actually put on a radio, with broadcast
  and receive functions that never reach into another agent's live
  state. See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full
  protocol walkthrough and how it maps onto a real LoRa link.
- **The codebase is split for two real targets, not hypothetically
  portable.** `core/` has zero stdio/OS/filesystem dependencies and is
  verified freestanding-clean for Cortex-M55 by actually compiling
  against the official µT-Kernel 3.0 BSP2 headers — not just a
  freestanding-C guess (`make check-board`).
- **Grounded in real incident-command practice, not arbitrary
  numbers.** Team rest/rehab modeling follows NFPA 1584's two-tier
  self-rehab/full-rehab structure; fault-repair confirmation timing is
  built around FEMA's operational-period and PAR (Personnel
  Accountability Report) check-in cadence. See the comments in
  `core/mission_state.h` for the reasoning, not just the constants.
- **A live dashboard you can actually watch.** `laptop/tools/app.py` streams
  the simulation tick-by-tick over SSE to a browser map with live
  hazard/building rendering, per-team status, and an incident journal
  — see [`laptop/tools/README.md`](laptop/tools/README.md).

## Project status

**Working today:**
- Full laptop simulation: procedural world generation, earthquake
  damage propagation, CBBA task allocation across 10 teams (2 per
  specialty), fault injection, watchdog/recovery, fog-of-war search
  dispatch (`core/search_zones.c`).
- `board/STM32N6570-DK/` compiles clean against real µT-Kernel 3.0 BSP2
  headers (`make check-board BSP_DIR=...`) — genuinely verified, not
  assumed.
- Live browser dashboard with real-time map, telemetry, and an
  incident journal.

**In progress / partially stubbed:**
- Board telemetry (`board/STM32N6570-DK/snapshot.c`) is a ring buffer with
  no drain mechanism yet — every USB-related line is a `TODO(usb)`
  marker, not working code.
- Board debug output works over the ST-LINK debug UART
  (`tm_printf`, USART1) but there's no separate data-link transport
  yet.

**Not started / open questions** (see `ARCHITECTURE.md`'s *Open
Architectural Questions* section for the full reasoning):
- No flashable firmware image — `board/STM32N6570-DK/` compiles but there's
  no STM32CubeIDE project, linker script, or HAL init wrapping it yet.
- No real distributed deployment — CBBA's message-passing shape is
  real, but all teams still run in one process today; nothing goes out
  over an actual radio.
- Whether each board should share full world knowledge or only know
  about incidents it discovers locally is still an open design
  question, not a decided one.

## Repo structure

```
core/             portable simulator — grid, environment, CBBA, pathing,
                  scoring, fault injection, watchdog/recovery. No
                  stdio/OS/filesystem dependencies.
laptop/  desktop entry point: CLI, printf debug, CSV writer
board/STM32N6570-DK/   STM32N6570-DK entry point: µT-Kernel task, tm_printf
laptop/tools/            dashboard (Flask + SSE), snapshot labeler, compare scripts
vendor/           notes on fetching the external µT-Kernel BSP (not bundled)
```

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full file-by-file
breakdown with line counts and what each one actually does.

## Build and run

### Laptop

```sh
make                 # builds ./tron and ./label_snapshots
./tron 1 1 200        # first_seed=1, seed_count=1, ticks_per_seed=200
make smoke            # equivalent to: ./tron 1 1 200
```

Optional flags:

```sh
./tron 1 1 5 --fault-profile mobility     # inject a fault profile
./tron 1 1 200 --live --tick-delay-ms=150 # stream JSON frames, paced for humans
```

Fault profiles: `none`, `mobility`, `communication`, `resource`,
`combined`, `random` (`core/fault_config.h`).

### Board (compile-check only — see caveat below)

```sh
make check-board                              # core/ only, no BSP needed
make check-board BSP_DIR=/path/to/mtk3_bsp2   # full check, real BSP headers
```

The second form needs the official µT-Kernel 3.0 BSP2, fetched
separately (not bundled here — released under T-License 2.2, which
isn't redistribution-friendly to vendor verbatim):

```sh
cd vendor
git clone --recurse-submodules https://github.com/tron-forum/mtk3_bsp2.git
```

**This is a compile-correctness check against real BSP headers, not a
firmware build.** There's no linker script, HAL init, or STM32CubeIDE
project yet — see `vendor/README.md` for exactly what's still needed
to get a flashable image.

### Dashboard

```sh
pip install flask --break-system-packages
python3 laptop/tools/app.py
```

Then open <http://127.0.0.1:5050>. Full usage guide:
[`laptop/tools/README.md`](laptop/tools/README.md).

### Offline snapshot labeling

```sh
make label_dataset N=5
# or directly:
./label_snapshots tron_snapshot_dataset.csv tron_snapshot_dataset_labeled.csv 5
```

## Standards and references reflected in the code

These aren't just cited for flavor — the actual constants and timing
in `core/mission_state.h` are derived from them:

- **NFPA 1584** (Standard on the Rehabilitation Process for Members
  During Emergency Operations) — the two-tier self-rehab/full-rehab
  rest model (`MAX_CONSECUTIVE_PARTIAL_REST_CYCLES`).
- **FEMA's National US&R Response System** — operational-period
  structure and Personnel Accountability Report (PAR) check-in cadence
  underpin the fault-repair confirmation timing
  (`FAULT_PAR_INTERVAL_TICKS`, `FAULT_REPAIR_CONFIRM_DELAY_TICKS`,
  `FAULT_POST_REPAIR_IMMUNITY_TICKS`).
- **CBBA** — Choi, Brunet, and How, *"Consensus-Based Decentralized
  Auctions for Robust Task Allocation,"* 2009 — the consensus action
  table implemented in `core/cbba.c` is cited directly against this
  paper (Table I) in the source comments.

## Contest context

Built for the **TRON Programming Contest 2026**, targeting µT-Kernel
3.0 — the real-time OS published by the [TRON Forum](https://github.com/tron-forum),
whose official STM32N6570-DK BSP (`mtk3_bsp2`) this project compiles
and verifies against directly (see `vendor/README.md`). Contest-specific
links, deadlines, and submission requirements aren't tracked in this
repository — check with your contest coordinator for those.

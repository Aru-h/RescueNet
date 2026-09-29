# tron — REMCE rescue-coordination system

A decentralized, fault-tolerant disaster-response coordination system built
for the TRON Programming Contest 2026. Ten specialist response teams
(search, medical, extraction, perimeter, support) are allocated across
incidents in a procedurally generated city using CBBA (Consensus-Based
Bundle Algorithm), with no central coordinator: teams reach a shared plan
by exchanging bids over a peer-to-peer link. A digital-twin watchdog on
each team compares expected vs. actual behaviour to detect faults
(communication, mobility, resource, environment, mission), peer consensus
cross-checks those detections against what other teams observed, and an
AI fault predictor (in progress on a separate branch) is layered in behind
a simplex architecture so it can never make a decision unilaterally. The
same core logic (`core/`) runs identically on a laptop, for development
and dataset generation, and on a real STM32N6570-DK board running
µT-Kernel 3.0.

## Architecture

- **Single board, 10 teams.** The submission build
  (`DK_SINGLE_BOARD_FLAGS` in the `Makefile`) runs all 10 teams on one
  STM32N6570-DK. Team-to-team messages go over an in-memory virtual bus
  (`core/vbus.c`) rather than a radio link; the multi-board LoRa path
  (`core/lora_*.c`, `board/STM32N6570-DK/sx1262_hal.c`,
  `board/Raspberry_Pi_Pico/`) still exists in the code and is exercised by
  its own two-board harness targets, but is not what this build ships.
- **Task allocation: CBBA.** `core/cbba.c` implements the Consensus-Based
  Bundle Algorithm — each team builds a bundle of tasks by bidding and
  reconciles conflicts with peers, with no central allocator.
- **Fault injection.** `core/fault_injector.c` and `core/fault_config.c`
  inject configurable communication/mobility/resource/environment/mission
  faults into a run, driving both the training dataset and the detection
  evaluation.
- **Fault detection, three layers:**
  - *Rules* — a digital-twin watchdog (`core/mission_execution_twin.c`,
    `core/watchdog.c`) compares each team's expected vs. actual state and
    raises evidence across 7 channels.
  - *Consensus* — `core/consensus.c` lets teams corroborate or contest
    each other's fault reports from what their own radio hears, adding
    robustness against a single faulty (or faking) self-report. See
    [`docs/architecture/CONSENSUS_DESIGN.md`](docs/architecture/CONSENSUS_DESIGN.md).
  - *AI* — a float32 MLP fault predictor (`core/fault_predictor.c`),
    gated by the `FAULT_MODEL_LOADED` flag so it stays inert (all-zero
    scores) until a trained model is exported. Being added on a separate
    branch; not active in this build.
- **Evaluation tooling** (`laptop/tools/evaluate.py`, `make evaluate`)
  measures whether each layer actually improves fault detection and
  mission outcome over a rules-only baseline, with paired per-seed
  comparisons and bootstrap confidence intervals.
- **Laptop dashboard** (`laptop/tools/app.py`) drives and visualizes a run
  in three modes — live simulation, replay of a recorded run, or connected
  to the real board — including a drag-and-drop manual override of team
  assignments in simulation mode.

Full technical detail: [`docs/architecture/ARCHITECTURE.md`](docs/architecture/ARCHITECTURE.md).

## Repo layout

| Path | What's there |
|---|---|
| `core/` | Portable C simulation/coordination core — CBBA, consensus, fault injection/detection, LoRa crypto and framing. Builds identically for the laptop and the board. |
| `laptop/` | Laptop-only entry point (`main.c`), dataset snapshot writer (`snapshot.c`), and `laptop/tools/` — the dashboard, evaluation harness, and every `make check-*` selftest. |
| `board/STM32N6570-DK/` | Single-board submission target's board-specific code (mission link task, SX1262 HAL, debug). |
| `board/Raspberry_Pi_Pico/` | Second-board target for the multi-board LoRa path (not used by the submission build, kept for that path's own harness/tests). |
| `tron_board/` | The STM32CubeIDE project for the physical board (`.project`/`.cproject`, HAL/CMSIS drivers, FSBL). |
| `docs/` | Design docs, evaluation results, and research notes — see [`docs/README.md`](docs/README.md) for the index. |
| `data_prep.py`, `generate_feature_spec.py`, `feature_spec.json` | The 74-input feature contract for the AI fault predictor: dataset preparation and the C/Python feature-spec generation, kept in sync by `make check-feature-spec-fresh`. |
| `Makefile` | Every build and `make check-*` target for the laptop, board, and Pico builds. |

## Quick start

### Laptop simulator + dashboard

```
pip install -r requirements.txt
make tron                              # builds ./tron

# Dashboard, simulation-only (no real board needed):
SIM_ONLY=1 python laptop/tools/app.py       # macOS/Linux
python laptop/tools/app.py --no-hardware    # Windows, or as an explicit flag anywhere
```

The dashboard supports live simulation, replay of a recorded run, and a
real-board connection, plus a drag-and-drop manual override of team
assignments while in simulation mode.

### Board bring-up (STM32N6570-DK)

The only currently verified boot path is **DEV boot via the `DevRam`
STM32CubeIDE debug configuration** (the Application loaded straight into
SRAM by the debugger). Standalone Flash Boot hangs at the FSBL →
Application handoff and is open with ST as support case 00269955. See
[`docs/DK_DEMO_LAUNCH.md`](docs/DK_DEMO_LAUNCH.md) for the full checklist:
boot switch positions, the `DevRam` debug configuration, and known rough
edges.

### Evaluation

```
make evaluate                          # seeds 1001-1020, 3000 ticks, rules vs rules+consensus
```

Results are written to `build/evaluate/` and copied to
[`docs/evaluation/RESULTS.md`](docs/evaluation/RESULTS.md). See
[`docs/evaluation/README.md`](docs/evaluation/README.md) for options and
what the harness actually measures.

### Dataset generation

```
make tron_snapshot_dataset.csv         # or: ./tron <seed> <world-count> <ticks> --fault-profile=combined
python data_prep.py tron_snapshot_dataset.csv
```

`data_prep.py` builds the 74-input feature matrix used to train the AI
fault predictor and enforces the ground-truth-leak guards described in
[`docs/ml/FEATURE_CONTRACT_DESIGN.md`](docs/ml/FEATURE_CONTRACT_DESIGN.md). Read
[`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md) before training on any
previously generated CSV — several dataset-affecting fixes landed during
development.

### AI training / export

The AI fault predictor's training and export pipeline is being developed
on a separate branch; this branch ships with `FAULT_MODEL_LOADED=0` (the
predictor returns all-zero scores) so dataset generation stays
unaffected. See [`docs/ml/FEATURE_CONTRACT_DESIGN.md`](docs/ml/FEATURE_CONTRACT_DESIGN.md)
for the feature contract the model is trained against.

## Test / check commands

```
make                                        # build ./tron and label_snapshots
make check-selftests                        # all laptop/tools selftests
make check-live-override-noop-regression    # dashboard override is a byte-identical no-op on the dataset
make check-consensus-shadow-regression      # consensus doesn't change pre-existing CSV columns
make check-single-board-dk-harness          # 10 teams + virtual bus + consensus + mission link, single board
make check-board-single                     # single-board DK compile/link/symbol flags (needs arm-none-eabi-gcc)
make check-cubeide-single-flags             # tron_board's CubeIDE project matches DK_SINGLE_BOARD_FLAGS
make check-dashboard-csrf-selftest          # dashboard API CSRF/content-type hardening
make check-dashboard-e2e                    # browser end-to-end check (needs Playwright + a browser)
```

`check-board-single` needs an ARM cross-compiler (`arm-none-eabi-gcc`),
and `check-dashboard-e2e` needs
[`laptop/tools/requirements-dashboard-dev.txt`](laptop/tools/requirements-dashboard-dev.txt)'s
Playwright plus a fetched browser — see that file and each target's
Makefile comment for setup. Neither runs in every environment.

## Known limitations

See [`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md) for known behaviours
and gaps in the dataset, the core simulation, the board firmware, and the
dashboard — read this before training on the CSV or judging a live demo.

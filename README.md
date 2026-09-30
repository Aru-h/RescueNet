# REMCE — Fault-tolerant rescue coordination on μT-Kernel 3.0

**TRON Programming Contest 2026 · RTOS Application category (Student) · Theme: TRON × AI**
Team: **Rescuenet** — Harush S G Temkar, Harshith Ragi

Ten rescue teams (search, medical, extraction, perimeter, support) respond to
incidents in a procedurally generated, earthquake-damaged city. There is no
central dispatcher: the teams divide the work among themselves with an auction
algorithm (CBBA), and every team checks its own health against a digital twin.
When a team is faulty (a failing radio, a stalled vehicle, missing
equipment…), a rule-based watchdog, the other teams (peer consensus) and a
small neural network running on the microcontroller detect it, and the system
reassigns work so the mission keeps going.

All ten teams, the coordination logic and the AI run on **one STM32N6570-DK
under μT-Kernel 3.0**. A laptop dashboard shows the mission live, lets an
operator overrule the system by dragging a team onto an incident, and can
replay any recorded mission and take it over from any moment.

## Contents

1. [What you need](#1-what-you-need)
2. [Five-minute evaluation without the board](#2-five-minute-evaluation-without-the-board)
3. [Running on the STM32N6570-DK](#3-running-on-the-stm32n6570-dk)
4. [The AI on μT-Kernel](#4-the-ai-on-μt-kernel)
5. [How it works](#5-how-it-works)
6. [Verifying the results](#6-verifying-the-results)
7. [Repository layout](#7-repository-layout)
8. [Known limitations, third-party software, license](#8-known-limitations-third-party-software-license)

## 1. What you need

**Hardware:** one STM32N6570-DK and a USB cable to its ST-LINK connector.
No other hardware. (The laptop-only evaluation in §2 needs no board at all.)

**Software:**

| For | Needs |
|---|---|
| Laptop simulator and dashboard | macOS or Linux (tested), a C compiler (`cc`/gcc/clang), `make`, Python 3 |
| Board | STM32CubeIDE, and the μT-Kernel 3.0 BSP2 from TRON Forum (fetched separately, see §3) |
| Retraining the AI (optional) | PyTorch (installed by `requirements.txt`) |

```
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

## 2. Five-minute evaluation without the board

The laptop build runs exactly the same `core/` code as the board.

**Build and start the dashboard:**

```
make                                   # builds ./tron (AI off)
make tron-ai                           # builds ./tron_ai (AI on)
TRON_BINARY=./tron_ai SIM_ONLY=1 python laptop/tools/app.py
```

Open **http://127.0.0.1:5050** and click **Bypass & Enter Dashboard**. (On
Windows, which is untested, use `python laptop/tools/app.py --no-hardware`.
Leave out `TRON_BINARY=./tron_ai` to run with the AI off.)

**Things to try:**

1. **A live mission.** Choose a seed and a fault profile (for example
   `combined`) and press **▶ Live**. The map shows the teams moving, incidents
   opening and closing, and faults being detected; the side panels show each
   team's state, the peer votes and the virtual radio traffic.
2. **Overrule the system.** Drag a team onto an open incident on the map and
   confirm. The dashboard shows the override's lifecycle: pending → committed
   → resolved or interrupted.
3. **Replay.** Switch the mode selector to **Replay**, choose
   `demo_seed1002_combined_600ticks.jsonl.gz` (a bundled 600-tick mission) and
   press **Load**, then **▶ Play**. Use the scrub bar or **⏭ Next detection**
   to jump to the next fault detection.
4. **Take over a replay.** Pause at any tick and press **⏵ Take over from
   here**. The simulator re-runs the mission to that tick (checking that it
   matches the recording) and continues live, where you can override again.
   The bundled demo was recorded with `./tron` (AI off), so start the dashboard
   without `TRON_BINARY=./tron_ai` to take it over.

**Batch run without the dashboard:**

```
./tron 1001 1 3000 --fault-profile=combined   # seed 1001, one run, 3000 ticks
```

This writes `tron_snapshot_dataset.csv`, one row per team per tick.

## 3. Running on the STM32N6570-DK

The full checklist, including the exact CubeIDE settings, is
[`docs/DK_DEMO_LAUNCH.md`](docs/DK_DEMO_LAUNCH.md). In short:

1. **Fetch the μT-Kernel 3.0 BSP2** (TRON Forum's T-License does not allow us
   to bundle it):
   ```
   cd board/STM32N6570-DK/vendor
   git clone --recurse-submodules --branch v1.00.03 https://github.com/tron-forum/mtk3_bsp2.git
   ```
   Use **v1.00.03**, the version the board build was tested with. The newer
   v1.00.04 does not compile for the STM32N6570-DK (`'sN_INTVEC' undeclared`
   in `sysdepend/stm32_cube/cpu/core/armv8m/interrupt.c`).
2. **Open `tron_board/` in STM32CubeIDE** (File → Import → Existing Projects),
   select the **`DevRam`** build configuration of the `Appli` project, and
   build. `DevRam` includes the AI (`FAULT_MODEL_LOADED=1`) and the
   single-board flags (all 10 teams on one board, virtual bus, consensus on).
3. **Set the boot switches to DEV boot:** BOOT1 (SW1) on, BOOT0 (SW2) off.
   Pressing the reset button should turn LED2 red.
4. **Debug** with a configuration that loads `DevRam/tron_board_Appli.elf`,
   then **Resume**. The serial console (ST-LINK virtual COM port, 115200 8N1)
   prints `TRON Board is online and ready!` and a `CONFIG` line including
   `model_loaded=1`.
5. **Start the dashboard in hardware mode:**
   ```
   python laptop/tools/app.py
   ```
   Enter the board's serial port (macOS: `/dev/cu.usbmodem…`, Linux:
   `/dev/ttyACM0`, Windows: `COMx`), click **Connect to Board**, wait for the
   handshake, then click **Start board mission**. The laptop sends the city
   map and the clock; the board runs all ten teams and reports back.
6. **Reset the board** (CubeIDE Restart, then Resume) **before every new
   mission.**

Standalone Flash Boot (without the debugger) does not work yet; see
[Known limitation 1](KNOWN_LIMITATIONS.md).

## 4. The AI on μT-Kernel

A float32 neural network (75 inputs → 128 → 64 → 5 fault scores, 18,309
weights and biases, about 73 KB) runs **on the board's CPU, inside the
μT-Kernel simulation task**, with no ML runtime: the whole forward pass is a
71-line plain-C file, `core/fault_predictor.c`. Every tick, for each team, it
reads 75 features of live state (digital-twin errors, watchdog evidence,
movement history, map signals) and scores five fault classes: mobility,
communication, resource, environment and mission.

- **Safe by design (simplex pattern):** on its own the AI can only trigger
  local recovery. Reallocating a team's task or sending it back to the safe
  zone also needs the rule-based watchdog, 8 consecutive ticks of AI
  confidence, or peer consensus to agree.
- **Measured effect** (simulator, held-out seeds 1001–1020, 3000 ticks):
  teams resolve **38.95 incidents per run with the AI vs 35.9 without (+3.05,
  95% CI 1.30–4.80)**, with no measurable harm on fault-free runs. The model
  keeps its accuracy when its own decisions change what teams do (macro F1
  0.708 before vs 0.706 while acting).
- **Limits come from the recovery layer, not the model.** Only mobility,
  communication and resource act. Environment and mission faults hit the map
  or the incident, not the team, while every recovery action acts on the team;
  even a detector that is never wrong costs 8.8 incidents per run when those
  classes act, so they are off. And a perfect detector on the three shipped
  classes reaches only 39.7 vs the model's 38.95, because faults clear on a
  fixed timer no recovery action can shorten.
- **Verified C = PyTorch:** `make check-model-parity` checks the C forward pass
  against PyTorch within 1e-5 (worst measured 3.6e-7).

Details: [`ml/MODEL_REPORT.md`](ml/MODEL_REPORT.md) (model, data, thresholds,
closed-loop results) and
[`docs/evaluation/AI_EVALUATION_BRIEF.md`](docs/evaluation/AI_EVALUATION_BRIEF.md)
(every test, command and raw result). Retraining: [`ml/README.md`](ml/README.md).

## 5. How it works

- **Task allocation — CBBA** (`core/cbba.c`): each team bids for incidents
  and resolves conflicts with its peers; no central allocator.
- **One board, ten teams:** each team runs as its own logical node; teams talk
  over an in-memory virtual bus (`core/vbus.c`) that simulates per-link packet
  loss and airtime, standing in for a radio mesh.
- **Fault injection** (`core/fault_injector.c`): communication, mobility,
  resource, environment and mission faults, by profile (`none`, `combined`,
  `random`, single-class profiles).
- **Fault detection in three layers:**
  1. *Rules:* a digital twin (`core/mission_execution_twin.c`) predicts each
     team's state, and a watchdog (`core/watchdog.c`) accumulates evidence on
     seven channels when reality diverges.
  2. *Peer consensus* (`core/consensus.c`): teams vote on each other from what
     they observe over the bus, so a team cannot hide its own fault and one
     faulty peer cannot fake one.
     [`docs/architecture/CONSENSUS_DESIGN.md`](docs/architecture/CONSENSUS_DESIGN.md)
  3. *AI* (§4).
- **Recovery** (`core/recovery_manager.c`): fuses the three layers into one
  action per team — keep monitoring, local recovery, reallocate the task,
  return to the safe zone or abort.
- **Laptop ↔ board mission link** (`core/mission_link.c`,
  `board/STM32N6570-DK/mission_link_task.c`): over USART1 the laptop sends the
  city map, incidents, injected faults and the clock; the board sends team
  status back.
- **Deterministic:** the same seed gives the same mission, which is what makes
  replay, take-over and the byte-identical regression tests possible.

Full design: [`docs/architecture/ARCHITECTURE.md`](docs/architecture/ARCHITECTURE.md).

## 6. Verifying the results

```
make check-selftests                        # 14 selftests: mission link, overrides, CBBA, replay take-over, AI model parity, …
make check-live-override-noop-regression    # overrides change nothing when unused (byte-identical dataset)
make check-consensus-shadow-regression      # consensus does not alter pre-existing behaviour
make check-feature-pack-parity              # C feature vector == Python training features
make check-single-board-dk-harness          # 10 teams + virtual bus + consensus + real board mission-link code, host-side
make check-cubeide-single-flags             # the CubeIDE project uses the single-board flags
make check-dashboard-csrf-selftest          # dashboard API hardening
make check-dashboard-e2e                    # full browser test of the dashboard (needs Playwright, see below)
make evaluate                               # rules vs +consensus vs +AI, paired seeds, bootstrap CIs
```

`check-dashboard-e2e` needs
`pip install -r laptop/tools/requirements-dashboard-dev.txt` and
`python -m playwright install chromium`. `make check-board-single` also
cross-compiles the board code and needs `arm-none-eabi-gcc`.

## 7. Repository layout

| Path | Contents |
|---|---|
| `core/` | Portable C: simulation, CBBA, virtual bus, consensus, fault injection and detection, recovery, AI forward pass and weights. Built unchanged for the laptop and the board. (Also contains LoRa framing/encryption code from an earlier multi-board design; not used by this build.) |
| `board/STM32N6570-DK/` | Board-specific code: μT-Kernel entry point and tasks, mission link over USART1, serial debug output. `vendor/` is where the μT-Kernel BSP2 goes. |
| `tron_board/` | The STM32CubeIDE project (application, FSBL, ST HAL drivers, linker scripts, `DevRam` configuration). |
| `laptop/` | Laptop simulator entry point and dataset writer; `laptop/tools/` holds the dashboard (`app.py`), the evaluation harness and every test. |
| `ml/` | AI pipeline: data preparation, feature contract, training, evaluation, export, trained checkpoint, model report. |
| `docs/` | Architecture, evaluation results, launch checklist, reports; index in [`docs/README.md`](docs/README.md). |
| `Makefile` | Every build and check target. |

## 8. Known limitations, third-party software, license

- **Known limitations:** [`KNOWN_LIMITATIONS.md`](KNOWN_LIMITATIONS.md).
- **Third-party software**, rights holders, licenses and our rights statement:
  [`THIRD_PARTY.md`](THIRD_PARTY.md). Third-party components keep their own
  licenses.
- **Originality:** this program is the original work of the entrants. It has
  not been published, and it has not been entered in any other contest.
- **License:** see [`LICENSE`](LICENSE). We intend to release this project as
  open source after the contest.

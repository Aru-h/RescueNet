# Skeleton checkpoint: board test checklist

State of the firmware at this checkpoint (RTOS work, phase 1 skeleton):

| Task | Priority | Does |
|---|---|---|
| link | 4 | owns the radio and the laptop link; wakes on USART1 RX (ISR), a 10 ms poll handler, or the sim finishing a tick |
| sim compute | 10 | boot init, then `sim_tick()` unchanged; only writer of simulation state |
| ai worker 0, 1 | 11 | created, wait on a flag; no inference yet (Phase 2) |
| telemetry | 12 | created, waits on a message buffer; sends nothing yet (Phase 3) |
| monitor | 20 | created, waits on a 1 s handler and the sim watchdog; prints only a watchdog alarm |
| logger | 30 | prints every console line (`BOARD_LOG`), one character at a time |

Kernel objects: 4 event flags, 2 mutexes (comms, UART), 2 message buffers
(log, telemetry), 2 cyclic handlers (10 ms link poll, 1 s monitor), 1 alarm
(sim watchdog, 10 s per tick). USART1 RX is a kernel-managed interrupt
(`tk_def_int`, IRQ 159, level 15) that also sets a flag bit.

Nothing here was run on a board by the author: only the ARM compile
(`make check-board-compile check-board-single-compile`) and the PC checks.

## 1. What to flash

`DevRam` configuration of `tron_board/Appli`, same as before (README section
3). No new compiler defines. Reset the board before each run. Capture the
console as in `docs/BOARD_STATE_HASH_CAPTURE.md` (Linux/macOS and PowerShell
commands are there).

## 2. Expected console

Lines the run must contain (the order of the "task up" lines among themselves,
and relative to the sx1262 `DBG` lines, is not fixed):

    tron: CONFIG teams_per_board=10 vbus=1 consensus_enforce=1 radio=0 model_loaded=1 cpu_hz=... 
    tron: starting simulation task
    tron: link task created (pri 4)
    tron: sim compute task created (pri 10)
    tron: sim compute task entered
    tron: USART1 addr=0x52001000 fifoen=1 ue=1 re=1 rxneie=1 cr1=0x2000002d
    tron: waiting on mission-link handshake
    tron: link task running
    tron: ai worker 0 up (pri 11)
    tron: ai worker 1 up (pri 11)
    tron: telemetry task up (pri 12)
    tron: monitor task up (pri 20)

then, once the dashboard connects and the mission starts:

    tron: handshake ready
    tron: waiting on mission roster, map and sites
    tron: roster, map and sites received, starting seed loop
    tron: seed 1 start
    STATEHASH tick=50 all=... teams=... inc=... flt=... rec=...      (every 50 ticks)
    tron: tick cost last=... cyc (...) max=... cyc (...) @600 MHz     (every 50 ticks)

Must NOT appear: `FAILED` (any of `board_log_start`, `board_rtos_create`, `link
task start`, `sim compute task start`, `cyclic handler start`), `WATCHDOG`,
`log: N lines dropped`, `Default Handler`, `joining at`.

## 3. Hashes to compare

Same seed, ticks and pace as your Phase 0 baseline captures
(`seed 1, 300 ticks, time_scale 10, fault_profile none` in the capture doc):

    python laptop/tools/statehash_compare.py board_run1.log skeleton_run.log

Expected: `MATCH: 6 common ticks (50..300) identical in all five hashes`. The
skeleton changes who runs what, not what the simulation computes, so the hashes
must equal the baseline.

## 4. If something is off: bisect order

Every commit below compiles for the board (checked with `make check-board-compile check-board-single-compile`); whether each one boots is what this table is for (`git checkout <hash>`, rebuild).

| Commit | Adds | If the board breaks here |
|---|---|---|
| `10a7218` | USART1 RX through `tk_def_int` | no RX at all: laptop never connects, `Default Handler` printed |
| `f426417` | logger task, console through a message buffer | no or garbled console text; UART lock |
| `c60c6b1` | boot wait loops yield | (small) |
| `f7898c3`, `82a8a0e` | kernel objects, helper tasks | `board_rtos_create FAILED rc=-N` (-1 flags, -2 mutex, -3 mbf, -4 cyclic, -5 alarm, -6 ai worker, -7 telemetry, -8 monitor) |
| `ece605b` | link task + sim compute split | handshake or roster never completes, ticks do not start, hashes differ |
| `2c512b1` | usermain stack fix (init task has 1 KB) | hang or fault right at boot, before `link task created` |

## 5. Known risks at this checkpoint (not yet verified on hardware)

- The init task that runs `usermain()` has a 1 KB stack (BSP `INITTASK_STKSZ`);
  `usermain()` was reduced to small log buffers for that, but a fault before
  `link task created` would point there first.
- While the logger has not started running (everything printed before the sim
  first blocks) output is direct and unbuffered, as before; after that it is
  queued. A burst longer than the 4 KB buffer is dropped and counted.
- The link task blocks on the comms mutex while the sim computes a tick, so the
  sim inherits priority 4 for that time. USART1 bytes are still captured by the
  ISR ring (512 bytes) meanwhile; the ring is drained at the start of the next
  link pass.
- CBBA's reply wait is now 62 steps of ~20 ms (about 1.24 s). Not measured.
- Laptop frames that arrive after the post-tick pass and before the next tick
  starts may be applied before that tick (the old single task had the same kind
  of race at a different point). If run-to-run hashes differ only on the
  `inc` or `teams` hash, look at this first.

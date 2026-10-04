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
`log: N REQUIRED lines LOST`, `Default Handler`, `joining at`. (`log: N lines
dropped` concerns ordinary text lines only and is not a failure by itself; it
is worth reporting.)

**A missing `STATEHASH` line is a failure, not noise.** The firmware prints one
every 50 ticks and never drops one on purpose: it waits up to 500 ms for room in
the log buffer, and if even that fails it prints `REQUIRED lines LOST`.
`statehash_compare.py` fails on a tick that one log has inside the other log's
range and the other lacks. With seed 1 / 300 ticks you must see exactly six
(ticks 50, 100, ... 300) in every run.

## 3. Hashes to compare

Same seed, ticks and pace as your Phase 0 baseline captures
(`seed 1, 300 ticks, time_scale 10, fault_profile none` in the capture doc):

    python laptop/tools/statehash_compare.py board_run1.log skeleton_run.log

Expected: `MATCH: 6 common ticks (50..300) identical in all five hashes` (a
log that stops early still says MATCH for the ticks it has: check the count is 6). The
skeleton changes who runs what, not what the simulation computes, so the hashes
must equal the baseline.

## 4. Locks, and what the link task touches

Two mutexes, both `TA_INHERIT`. `g_mtx_comms` is held by the sim compute task for
a whole tick and released only inside `cbba_wait_poll()` (for one ~20 ms wait
step); the link task takes it for a pass. `g_uart_mtx` is held for one whole
console line or one whole binary frame.

| What the link task does | Needs `g_mtx_comms`? | Mid-tick |
|---|---|---|
| empty the USART1 ISR ring into its receive buffer (`mission_link_task_usart1_pump`) | **no**: the ring is single-producer (ISR) / single-consumer (link), the buffer is link-private | runs at priority 4 even while the sim computes |
| parse and apply laptop frames, status heartbeat, handshake TX (`mission_link_task_step`) | yes (writes mission-link and simulation state) | only between ticks |
| poll the radio, dispatch queued TX (`sx1262_poll_rx`, `comms_transport_dispatch_pending`) | yes (the comms inbox and TX queues are not safe across tasks, see `core/comms_transport.c`) | only inside the sim's CBBA wait steps, and between ticks |
| write a binary frame / text line to USART1 | `g_uart_mtx` only | any time |

How a pass takes the mutex: the post-tick pass and the boot waits **block** on it
(the sim is waiting for them). Every other wake-up (RX flag, 10 ms poll) uses a
**try-lock** and skips the pass if the sim is computing, so the link never queues
behind a tick; the post-tick pass, or the sim's release in `cbba_wait_poll()`
(which unlocks before waking the link), does the work instead.

What this buys, and what it does not: the ISR ring is emptied promptly during a
tick, which is what the later priority comparison (link above or below the sim)
exercises. It does not make the radio serviceable during a tick's compute
phase: a received LoRa frame waits in the SX1262 until the next CBBA wait step
or the end of the tick, as it did when one task did everything (the DK
single-board build has the radio off).

## 5. Laptop frame order (a rule, not a timing outcome)

The laptop sends a tick's changes (tasks, faults, map rows, zone trust) and
then, **last**, the clock frame that authorises the tick
(`laptop/mission_feed.c`). The board applies the stream in that order and stops
at each authorisation: the receive loops stop consuming once a clock frame has
authorised a tick the sim has not run (`authorised tick > g_current_tick`) and
resume after it ran. So frames before `clock(T)` are applied before tick T,
frames behind it wait, in order, until after tick T, however fast the tasks run.
Host test: `make check-mission-link-stream-order` (real `mission_link_task.c`,
control run with the mode off).

Consequences to know: if the board is slower than the laptop's pace, the laptop's
frames queue in the 1 KB receive buffer (and the 512-byte ring) instead of the
board jumping ahead; when both are full, bytes are dropped and counted (the
`LoRa`/USART1 error line), and the stream resyncs on the next frame. A board that
joins late (first clock frame far ahead of tick 0) still jumps to it once.

## 6. If something is off: bisect order

Every commit below compiles for the board (checked with `make check-board-compile
check-board-single-compile`); whether each one boots is what this table is for
(`git checkout <hash>`, rebuild).

| Commit | Adds | If the board breaks here |
|---|---|---|
| `10a7218` | USART1 RX through `tk_def_int` | no RX at all: laptop never connects, `Default Handler` printed |
| `f426417` | logger task, console through a message buffer | no or garbled console text; UART lock |
| `c60c6b1` | boot wait loops yield | (small) |
| `f7898c3`, `82a8a0e` | kernel objects, helper tasks | `board_rtos_create FAILED rc=-N` (-1 flags, -2 mutex, -3 mbf, -4 cyclic, -5 alarm, -6 ai worker, -7 telemetry, -8 monitor) |
| `ece605b` | link task + sim compute split | handshake or roster never completes, ticks do not start, hashes differ |
| `2c512b1` | usermain stack fix (init task has 1 KB) | hang or fault right at boot, before `link task created` |
| `8caeb4a` | stream order: stop at each tick authorisation | ticks never start after the first (a clock frame is held), or hashes differ from baseline |
| `9c9e5f2` | lock-free ring drain, try-lock passes | RX overruns, slow or stuck link passes |
| `e51c761` | STATEHASH through the reliable log path | missing STATEHASH lines, `REQUIRED lines LOST` |

## 7. Known risks at this checkpoint (not yet verified on hardware)

- The init task that runs `usermain()` has a 1 KB stack (BSP `INITTASK_STKSZ`);
  `usermain()` was reduced to small log buffers for that, but a fault before
  `link task created` would point there first.
- While the logger has not started running (everything printed before the sim
  first blocks) output is direct and unbuffered, as before; after that it is
  queued. A burst of ordinary lines longer than the 4 KB buffer is dropped and
  counted; STATEHASH lines wait instead.
- CBBA's reply wait is now 62 steps of ~20 ms (about 1.24 s). Not measured.
- The USART1 ISR wakes the link task once 32 bytes wait; a shorter frame is
  picked up by the 10 ms poll, so the ring drain latency is up to 10 ms.
- The sim inherits priority 4 only if the link task blocks on the comms mutex,
  which now happens only for the post-tick pass and the boot waits (when the
  sim is not holding it).

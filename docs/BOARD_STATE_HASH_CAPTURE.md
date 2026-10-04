# Capturing board state hashes (Phase 0 of the RTOS work)

Goal: show that two board runs with the same seed and pace print the same
`STATEHASH` lines, so later phases can be checked against this capture.

Board hashes are **not** expected to equal the PC standalone hashes
(`tron --state-hash=N`): the board runs only the teams (`SIM_ROLE_BOARD`, no
fault injector, incidents arrive over the mission link), the laptop owns the
world. The reference is the board against itself.

## One run

1. Build `DevRam` in CubeIDE (README section 3). `BOARD_STATE_HASH_INTERVAL`
   defaults to 50; override with `-DBOARD_STATE_HASH_INTERVAL=N` in the project's
   compiler defines (0 turns it off).
2. SW1 on, SW2 off (DEV boot). Debug `DevRam/tron_board_Appli.elf`, Resume.
3. **Reset the board before every run** (CubeIDE Restart, then Resume).
4. Start the dashboard with its output kept:
   `python laptop/tools/app.py 2>&1 | tee board_run1.log`
   (the board's console text arrives on the same serial port and the bridge
   prints it as `[Board Log] ...`).
5. Connect to the board, wait for the handshake, then start the mission with a
   fixed seed, ticks and pace. Through the UI, or:
   `curl -X POST localhost:<port>/api/board/launch -H 'Content-Type: application/json' -d '{"seed":1,"ticks":300,"time_scale":10,"fault_profile":"none"}'`
6. When the log shows `STATEHASH tick=300`, stop (Ctrl+C), reset the board and
   repeat steps 4-6 into `board_run2.log`.
7. `python laptop/tools/statehash_compare.py board_run1.log board_run2.log`

## Expected console output

At boot, after `tron: CONFIG ...`:

    tron: USART1 addr=0x52001000 fifoen=1 ue=1 re=1 rxneie=1 cr1=0x2000002d

`fifoen=0` or a different address means the FIFO enable did not reach the
secure USART1: report it.

During the run, one line every 50 ticks (tick numbers are the board's tick):

    [Board Log] tron: STATEHASH tick=50 all=<16 hex> teams=<16 hex> inc=<16 hex> flt=<16 hex> rec=<16 hex>

If `tm_printf` prints hex without zero padding (a short hash), tell me; the
format would need `%08lx` replaced.

Expected comparison result: `MATCH: 6 common ticks (50..300) identical in all five hashes`.

## Validity checks on the runs themselves

- Neither log may contain `joining at` ("tick ... too far behind the laptop's"):
  skipped ticks change the state sequence. If it appears, lower the pace
  (`time_scale` 1-5) and capture again.
- `incidents` hashes slots in slot order, and the board fills slots in the order
  incidents arrive over the link. If only `inc` (and what depends on it)
  differs between two otherwise clean runs, that is an arrival-order effect to
  report, not necessarily a sim bug.
- A mismatch prints the first differing tick and which of teams / inc / flt /
  rec diverged.

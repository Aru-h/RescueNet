# STM32N6570-DK bring-up and demo launch

How to get the single-board submission running on the physical
STM32N6570-DK, and how to run a mission demo once it's flashed.

## Boot mode: DEV (DevRam), not Flash

The board is brought up in **DEV boot** using the `DevRam` STM32CubeIDE
debug configuration, which loads and runs the firmware directly into
RAM rather than programming it into flash.

**Flash Boot is currently unresolved** (open with ST as case
**00269955**). Until that's resolved, DEV/DevRam is the only supported
way to run this build on real hardware — don't expect a flash-programmed
boot to come up.

### Boot switches

- **DEV boot (what this project uses):** BOOT1 **on**, BOOT0 **off**.
- **Flash boot (not currently working — see above):** both BOOT0 and
  BOOT1 **off**.

### Launching

1. Set the boot switches to DEV (BOOT1 on, BOOT0 off).
2. Open the `tron_board` project in STM32CubeIDE.
3. Run the `DevRam` debug configuration to load and start the firmware.
4. **Reset the board between mission runs.** DEV/RAM boot does not
   reliably restart a clean mission on its own — power-cycle or reset
   the board before starting a new run, rather than re-running the
   previous session's state.

## Pace / time scale

The dashboard (`laptop/tools/app.py`) and live simulator support a
`time_scale` parameter: `1` is real time (one tick every 36 seconds,
the mission's natural pace), and higher values compress the run for a
demo or rehearsal.

- **10x pace is fully safe** — verified stable across repeated runs.
- **30x pace has been verified working but is not guaranteed** — it has
  run successfully but hasn't been exercised enough to treat as a
  supported baseline. If a demo run at 30x looks off, drop back to 10x.

## Related reading

- [`KNOWN_LIMITATIONS.md`](../KNOWN_LIMITATIONS.md) — known behaviours
  and gaps when running or judging this build, including the
  single-board/virtual-bus setup and detection limitations.
- [`ARCHITECTURE.md`](../ARCHITECTURE.md) — how the single-board build
  (`DK_SINGLE_BOARD_FLAGS`) maps all 10 teams onto one board.

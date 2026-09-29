# STM32N6570-DK demo launch checklist (DEV boot / debug probe)

Audience: whoever is starting the single-board demo on real hardware. This is the **only** supported boot path for the demo right now — standalone Flash Boot hangs at the FSBL→Application handoff (see [Known limitation E.18](../KNOWN_LIMITATIONS.md) and the full diagnosis report). Follow these steps exactly; skipping the loader-init or reset-mode settings below reproduces failures we already hit and fixed once.

## 1. Boot switch position

Set **SW1 (BOOT1) = right/ON, SW2 (BOOT0) = left/OFF** (DEV boot). Verify it: press the black NRST button — **LED2 must turn red**. If it doesn't, the board is not actually in DEV boot and nothing below will work (this exact mistake — an invalid switch combination that is neither DEV boot nor Flash Boot — caused unexplained "Target not responding" debugger failures during bring-up).

Flash Boot (standalone, no debugger) is **both switches left/OFF** — out of scope for the demo; see [Known limitation E.18](../KNOWN_LIMITATIONS.md).

## 1a. Recommended path: standalone SRAM build (`DevRam`), no FSBL

The most reliably verified path this session was running the Application **by itself**, loaded straight into internal SRAM by the debugger, with no FSBL involved at all — this isolates the Application from the still-unresolved FSBL→XIP handoff entirely, and is what actually produced a full confirmed run (handshake, world transfer, live telemetry, fault detection) on real hardware.

1. In CubeIDE, select the **`DevRam`** build configuration for the `Appli` project (uses `STM32N657X0HXQ_LRUN.ld` + defines `APPLI_DEVRAM_STANDALONE`) and build it.
2. Create a debug configuration pointing at `DevRam/tron_board_Appli.elf` directly. No external loader is needed (it's internal SRAM, not external flash) and no signing is needed (the debugger loads the plain `.elf`).
3. Debug, then **Resume**. Expect `TRON Board is online and ready!`, then the kernel/CONFIG boot trace, same as the FSBL path below.
4. **Reset the board (CubeIDE Restart + Resume) before every new mission run.** Stopping and relaunching a mission on an already-connected board session without resetting it first leaves the board's mission-link session state stale, and the board silently stops sending team-status reports (observed directly: `reports_forwarded` stuck at 0 for the rest of that session). A fresh reset is required between runs — this is a known limitation, not yet fixed at the protocol level.
5. Runs at 600 MHz CPU clock, no I/D-cache (both were tried and reverted — see the corresponding git commit for why), `-O2`. Confirmed clean (zero skipped ticks) at 30× real-time pace with `combined` fault injection over 150+ ticks; 50-60× still occasionally skip ticks under heavy fault load.

## 1b. FSBL → XIP path (steps 2 onward below)

This is the originally-intended production boot chain. The linker-script bug in step "3" was found and fixed this session, but a live end-to-end test of this exact path (FSBL signed+flashed, Application signed+flashed, full boot trace observed) was **not completed this session** due to time constraints — do not assume it works from this doc alone until it's actually been run.

## 2. One-time debug configuration setup (STM32CubeIDE)

In **Run → Debug Configurations → STM32 C/C++ Application → `tron_board_FSBL`**:

**Debugger tab:**
- External loaders: `MX66UW1G45G_STM32N6570-DK.stldr` — **Enabled = true, Initialize = true**. (Initialize defaults to false; leaving it false makes flash erase fail with "Error: failed to erase memory".)

**Startup tab → Reset behaviour:**
- **Type = Hardware reset.** (Software reset and "None" both fail here — Software reset leaves the external XSPI flash unable to erase; "None"/attach-without-reset hits a GDB `vMustReplyEmpty: timeout` on this setup.)

**Startup tab → additional load entry for the Application:**
- Program: the **signed** Appli binary — `tron_board/Appli/Debug/tron_board_Appli-Trusted.bin` (not the plain `tron_board_Appli.bin`, which has no signing header and will boot to garbage).
- Download = **true**, Download address = `0x70100000`.
- Load symbols = **false** (a `.bin` has no symbol table; leave that on the FSBL's own entry only).

This lets a single Debug launch flash both the FSBL and the signed Appli together.

## 3. Before every launch: re-sign the Appli if it was rebuilt

CubeIDE's plain build output (`tron_board_Appli.bin`) has no signing header — the FSBL's boot path expects one at a fixed offset. After any Appli rebuild, re-sign it:

```
STM32_SigningTool_CLI.exe -bin tron_board_Appli.bin -nk -of 0x0 -t fsbl -hv 2.3 -la 0x70100000 -align -o tron_board_Appli-Trusted.bin -s
```

Run from `tron_board/Appli/Debug/`. `STM32_SigningTool_CLI.exe` ships with STM32CubeProgrammer (`<CubeProgrammer install>/bin/`). Verify the result with `-dump` before trusting it: `Binary type` should read `0x10` and `Load address` should read `0x70100000`.

The FSBL itself needs no signing — CubeIDE's Debug launch flashes its plain `.elf`/`.bin` output directly.

## 4. Launch

1. Board connected via the onboard ST-LINK USB, boot pins as in step 1.
2. Run the `tron_board_FSBL` debug configuration (Debug button, or the launch icon's dropdown if a previous config is selected).
3. It flashes both binaries, resets, and halts once (a "Break at ... no debug information" stop right after connect — this is the debugger's own halt point before any of our code runs, not a fault).
4. Hit **Resume**. The FSBL's own boot trace over USART1 has been observed reliably. Whether it then successfully jumps to the Application and the Application starts ticking via this exact path (signed FSBL + signed XIP Appli + hardware reset) has **not been confirmed this session** — see section 1b above. Use the `DevRam` path (section 1a) for a demo you can rely on today.
5. If a breakpoint at `BOOT_Application` was left set from earlier debugging, remove it (or just Resume past it) — the demo should run to completion uninterrupted, not stop there.

## 5. Serial output

USART1, 115200 8N1. On Windows this enumerates as a COM port under the ST-LINK's virtual COM port — check Device Manager for the exact number (it moved around during bring-up whenever the ST-LINK re-enumerated, e.g. after a USB replug). Expect to see the FSBL's boot trace, then `tron: CONFIG ...` from the Application, then per-tick output once the simulation starts.

## 6. Known rough edges to expect

- A USB unplug/replug causes ~10-20s of COM-port disappearance; a terminal/logger opened right after replug can miss the first several boot lines. Prefer the physical reset button over a full unplug/replug when you just need to restart the run without losing early output.
- If CubeIDE is left running with an idle/no debug session and CubeProgrammer (standalone) can't connect to the ST-LINK, fully close CubeIDE — it can hold the ST-LINK reserved even without an active debug session.

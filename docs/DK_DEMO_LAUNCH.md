# STM32N6570-DK demo launch checklist (DEV boot / debug probe)

Audience: whoever is starting the single-board demo on real hardware. This is the **only** supported boot path for the demo right now — standalone Flash Boot hangs at the FSBL→Application handoff (see [Known limitation E.18](../KNOWN_LIMITATIONS.md) and the full diagnosis report). Follow these steps exactly; skipping the loader-init or reset-mode settings below reproduces failures we already hit and fixed once.

## 1. Boot switch position

Set **BOOT1 = high, BOOT0 = low** (DEV boot). Both boot pins low (Flash Boot) is out of scope — don't use it for the demo.

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
4. Hit **Resume**. The FSBL runs, prints its boot trace over USART1, jumps to the Application, and the Application starts ticking.
5. If a breakpoint at `BOOT_Application` was left set from earlier debugging, remove it (or just Resume past it) — the demo should run to completion uninterrupted, not stop there.

## 5. Serial output

USART1, 115200 8N1. On Windows this enumerates as a COM port under the ST-LINK's virtual COM port — check Device Manager for the exact number (it moved around during bring-up whenever the ST-LINK re-enumerated, e.g. after a USB replug). Expect to see the FSBL's boot trace, then `tron: CONFIG ...` from the Application, then per-tick output once the simulation starts.

## 6. Known rough edges to expect

- A USB unplug/replug causes ~10-20s of COM-port disappearance; a terminal/logger opened right after replug can miss the first several boot lines. Prefer the physical reset button over a full unplug/replug when you just need to restart the run without losing early output.
- If CubeIDE is left running with an idle/no debug session and CubeProgrammer (standalone) can't connect to the ST-LINK, fully close CubeIDE — it can hold the ST-LINK reserved even without an active debug session.

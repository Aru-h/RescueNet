# board/STM32N6570-DK/vendor/

External dependencies for the STM32N6570-DK board target, fetched
separately, not bundled in this repo. (Moved here from top-level
`vendor/` as part of splitting each physical board target into its
own self-contained `board/<name>/` folder — see `../../../
CHANGELOG.md`'s "Board folder restructure" entry.)

## mtk3_bsp2 (µT-Kernel 3.0 BSP2)

Official BSP from the TRON Forum, confirmed supporting STM32N6570-DK
(Cortex-M55) via STM32Cube IDE:
https://github.com/tron-forum/mtk3_bsp2

Not vendored directly here because it's released under T-License 2.2,
which isn't a redistribution-friendly license to bundle verbatim —
fetch it yourself:

```sh
cd board/STM32N6570-DK/vendor
git clone --recurse-submodules --branch v1.00.04 https://github.com/tron-forum/mtk3_bsp2.git
```

Use **v1.00.04**. That release does not define `sN_INTVEC`, which its
`sysdepend/stm32_cube/cpu/core/armv8m/interrupt.c` uses to size the
interrupt-handler table (indexed by interrupt number), so the CubeIDE project
(`tron_board/Appli/.cproject`) defines `sN_INTVEC=N_INTVEC` (196 on the
STM32N6) in every build configuration. v1.00.04 also corrects the STM32N6 EXTI
register map (v1.00.03 used STM32H7 register offsets). If you already cloned a
different version, switch with `git checkout v1.00.04` inside `mtk3_bsp2/`.

`board/STM32N6570-DK/` and `make check-board` expect it at
`board/STM32N6570-DK/vendor/mtk3_bsp2/` exactly as cloned above. If
you put it somewhere else, override with:

```sh
make check-board BSP_DIR=/path/to/mtk3_bsp2
```

### What we've verified against it

- STM32N6570-DK is real, first-party supported (`include/sys/sysdepend/stm32_cube/discovery_stm32n657/`),
  targeting Cortex-M55 / ARMv8-M, selected via the `_STM32CUBE_DISCOVERY_N657_`
  compiler define (see `include/sys/machine.h`).
- FPU support is on by default (`USE_FPU=1`) and `ALWAYS_FPU_ATR=1` is
  also on by default, meaning every task gets the FPU context saved —
  good, since core/ leans on floats throughout (stamina, scoring,
  path costs, environment noise generation) and we don't want to
  hand-pick which tasks need TA_FPU.
- Debug UART is USART1 at 115200-8-N-1, exposed through the ST-LINK
  micro-USB connector already on the dev kit — no extra wiring needed
  to get `tm_printf` output.
- `tm_printf`/`tm_sprintf` are T-Monitor-compatible but do **not**
  support floating point conversion specifiers. board/STM32N6570-DK/debug.c
  works around this with a small fixed-point formatter rather than
  silently truncating or crashing on `%f`.
- Entry point is `EXPORT INT usermain(void)`, called from the initial
  task after `knl_start_mtkernel()` runs (that call itself goes in the
  STM32CubeMX-generated `Core/Src/main.c`, inside the
  `USER CODE BEGIN 2` / `END 2` markers — that's IDE project
  scaffolding, not something this repo's sources can inject).
- TrustZone note: STM32N657 ships with TrustZone enabled, and this BSP
  version only runs in the secure world. The STM32CubeIDE project
  needs a secure sub-project for the µT-Kernel application; this
  doesn't affect any of the C sources here, only IDE project setup.
- Verified this specific claim by actually compiling a minimal
  `usermain()` (task creation, `tm_printf`, `tk_dly_tsk`) against the
  real headers with:
  ```
  arm-none-eabi-gcc -mcpu=cortex-m55 -mthumb -mfloat-abi=hard -mfpu=auto \
    -ffreestanding -D_STM32CUBE_DISCOVERY_N657_ \
    -Iconfig -Iinclude -I. -c usermain.c
  ```
  Clean compile, no errors — that's the include-path recipe
  `make check-board` uses.
# board/Raspberry_Pi_Pico/vendor/

External dependency for the Raspberry Pi Pico (RP2040) board target.
Same per-board self-contained layout as `board/STM32N6570-DK/vendor/`
— see that folder's own `README.md` and `../../../CHANGELOG.md`'s
"Board folder restructure" entries for why each board owns its BSP
copy instead of sharing one top-level `vendor/`.

## mtk3_bsp (µT-Kernel 3.0 BSP, v1 — NOT the v2 BSP the DK uses)

Official BSP from the TRON Forum:
https://github.com/tron-forum/mtk3_bsp

This is a genuinely different repo from `mtk3_bsp2` (the DK's BSP),
not just a different branch of the same one — confirmed by actually
cloning both. `mtk3_bsp` is organized as one repo with a separate
git branch per supported board; the Pico's is the `pico_rp2040`
branch:

```sh
cd board/Raspberry_Pi_Pico/vendor
git clone --branch pico_rp2040 --single-branch https://github.com/tron-forum/mtk3_bsp.git mtk3_bsp
```

**Use `--branch ... --single-branch`, not a plain clone + checkout.**
A plain `git clone` defaults to `master`, and `master`'s working tree
carries files (e.g. `build_make/makefile`, still defaulting to
`_NUCLEO_H723_`/`_IOTE_STM32L4_` targets) that are NOT the Pico's
actual build path and will sit alongside the real `pico_rp2040`-branch
files if you `git checkout FETCH_HEAD -- .` on top of an existing
`master` checkout rather than cloning the branch directly — this
looks like contamination the first time you hit it (a build_make/
still mentioning STM32/RX targets even though you fetched the Pico
branch) but is actually just `build_make/` being a shared,
not-Pico-specific file present on every branch; cloning the branch
directly avoids the confusion entirely.

The BSP vendored here **is genuinely vendored** (full source tree
present in this repo, `.git` stripped, matching the DK's
`vendor/mtk3_bsp2/` — see that folder's own note on this), not just
documented — same reasoning as the DK: it's real, freely-available
code needed to build/verify this board target, and keeping it out of
the repo would mean `check-<pico-target>` (once that exists) can't
actually compile against real headers, the same gap this project has
consistently avoided for the DK throughout its own build-check work.
Still released under T-License 2.2 like `mtk3_bsp2` — same licensing
character, re-fetch with the command above instead of trusting a stale
copy if this ever needs updating.

## What's confirmed vs. what the original handoff doc assumed

Two things this vendored copy corrects, verified by actually cloning
and compiling against it (not by reading docs about it):

1. **Build system is Eclipse CDT managed-build (`.project`/
   `.cproject`, `org.eclipse.cdt.managedbuilder.core.genmakebuilder`),
   not a raw hand-written Makefile.** The original handoff doc treated
   `github.com/mijinco0/mtk3-pico-gmake` (a community Makefile +
   `elf2uf2` fork) as the working reference because it couldn't find
   the official repo's own build docs at the time. The official repo
   does have its own real project — same class of thing STM32CubeIDE
   was for the DK, different IDE (Eclipse CDT / "GNU ARM Eclipse"
   plugin family, not STM32CubeIDE). `build_make/` also exists as an
   alternate path but is not wired up per-target for Pico (no
   `pico_rp2040.mk`) — the `.cproject` is the one actually configured
   for this target.
2. **No Raspberry Pi Foundation `pico-sdk` dependency at all.**
   `kernel/sysdepend/cpu/rp2040/boot_2nd.c` embeds the RP2040 stage-2
   boot bytes directly; this BSP is bare-metal and self-contained, not
   built on top of `pico-sdk`. Confirmed no `pico-sdk`/`PICO_SDK_PATH`
   reference anywhere in `.cproject`.
3. **No SPI driver anywhere in this BSP** — `device/` only has
   `adc/`, `common/`, `i2c/`, `ser/`. This isn't Pico-specific: the
   DK's already-vendored `mtk3_bsp2` has zero SPI files either. The
   SX1262 HAL's SPI half will need to be written against raw RP2040
   registers (offsets already present in
   `include/sys/sysdepend/cpu/rp2040/sysdef.h` — `SPI0`/`SPI1`,
   `IO_QSPI_BASE`, `RESETS_RESET_SPI0`/`SPI1`), not a vendor SDK call
   — same situation as the DK side, which must be getting SPI from
   ST's own CubeMX-generated HAL (separate from `mtk3_bsp2` itself),
   not from the µT-Kernel BSP either. GPIO does have a real BSP-level
   wrapper (`include/bsp/libbsp.h` → `gpio_set_val()`/`gpio_get_val()`
   style calls, confirmed via `app_program/app_main.c`'s own on-board-
   LED example), unlike SPI.

## Verified toolchain flags

Confirmed by actually compiling `app_program/app_main.c` (the BSP's
own example) against these headers with `arm-none-eabi-gcc`, clean,
no errors:

```sh
arm-none-eabi-gcc -std=gnu11 -Wall -mcpu=cortex-m0plus -mthumb \
  -ffreestanding -fno-builtin -D_PICO_RP2040_ \
  -Iconfig -Iinclude -Ikernel/knlinc \
  -c app_program/app_main.c -o /tmp/app_main_probe.o
```

`-D_PICO_RP2040_` is the ONE flag that matters — it's the only `-D`
this BSP's own `.cproject` defines. Everything else (the `TARGET_DIR`/
`TARGET_CPU_DIR` textual macros that `sysdef.h` and friends key their
own `#include "sysdepend/TARGET_DIR/..."`-style indirection on)
resolves transitively: `_PICO_RP2040_` → `include/sys/machine.h`'s
`#ifdef` chain → includes
`include/sys/sysdepend/pico_rp2040/machine.h` → THAT file is what
actually `#define`s `TARGET_DIR pico_rp2040` and
`TARGET_CPU_DIR rp2040`. Don't try to pass `-DTARGET_DIR=pico_rp2040`
directly — it isn't set that way anywhere in the real project, and
`machine.h` must be included (transitively, via `tk/tkernel.h`) before
anything that depends on `TARGET_DIR` being defined.

`-mcpu=cortex-m0plus` (RP2040 is dual Cortex-M0+), NOT `-mcpu=cortex-m55`
like the DK — a different core family entirely, not just a different
chip in the same family.

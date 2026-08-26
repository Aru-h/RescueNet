# core/vendor/

External dependencies vendored directly into this repo (see
`sx126x_driver/`'s own README, one level down, for the full detail on
that one) — different from `board/STM32N6570-DK/vendor/`'s and
`board/Raspberry_Pi_Pico/vendor/`'s own `vendor/` folders in two ways:

1. **What's here is licensed for redistribution.** The two BSPs are
   T-License 2.2, which isn't redistribution-friendly, so those
   folders only document fetch steps — the actual BSP source is
   pulled separately by whoever builds this project. `sx126x_driver`
   is Clear BSD licensed, which IS redistribution-friendly, so the
   actual source is vendored directly here.
2. **What's here is shared, not per-board.** The two BSPs are real
   per-board differences (different µT-Kernel BSP major version,
   different CPU, different toolchain) — this driver core is meant to
   be identical on both boards; only the four-function HAL underneath
   it (see `sx126x_driver/README.md`) differs per board. That's why
   this lives under `core/` at all, rather than duplicated into (or
   picked arbitrarily between) either board's own `vendor/`.

## sx126x_driver/

Semtech's official SX126X radio driver:
https://github.com/Lora-net/sx126x_driver

`.git` stripped, matching how both BSPs were vendored. Confirmed
genuinely freestanding (no `stdio.h`/`malloc`/`printf` anywhere) and
confirmed by actually cross-compiling `sx126x.c` for both real target
CPUs (`-mcpu=cortex-m55` and `-mcpu=cortex-m0plus`), clean.

`core/vendor/sx126x_driver/src/*.c` does not match `core/*.c`'s
Makefile wildcard (`CORE_SOURCES`), so it's automatically excluded
from the laptop build — correct, the laptop has no radio.

See `sx126x_driver/README.md` for the HAL contract (four functions,
one implementation needed per board), what's confirmed from the real
SmartElex SX1262 HAT (E22-900M22S) datasheet, and what's still
genuinely undecided (GPIO pin assignments per board, RXEN/TXEN
handling) before either board's HAL can actually be written.

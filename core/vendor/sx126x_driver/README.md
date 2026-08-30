# SX126X driver

This package proposes an implementation in C of the driver for **SX126X** radio component.
Please see the [changelog](CHANGELOG.md) for more information.

## Structure

The driver is defined as follows:

- sx126x.c: implementation of the driver functions
- sx126x.h: declarations of the driver functions
- sx126x_regs.h: definitions of all useful registers (address and fields)
- sx126x_hal.h: declarations of the HAL functions (to be implemented by the user - see below)
- lr_fhss_mac.c: Transceiver-independent LR-FHSS implementation
- sx126x_lr_fhss.c: Transceiver-dependent LR-FHSS implementation
- lr_fhss_mac.h: Transceiver-independent LR-FHSS declarations
- sx126x_lr_fhss.h: Transceiver-dependent LR-FHSS declarations
- lr_fhss_v1_base_types.h: LR-FHSS type interface
- sx126x_bpsk.c: implementation of BPSK driver functions
- sx126x_bpsk.h: declaration of BPSK driver functions

## HAL

The HAL (Hardware Abstraction Layer) is a collection of functions the user shall implement to write platform-dependant calls to the host. The list of functions is the following:

- sx126x_hal_reset
- sx126x_hal_wakeup
- sx126x_hal_write
- sx126x_hal_read

## Cmake usage

This driver exposes a cmake configuration allowing to integrate the driver in a cmake ready application.

### Integration

If the driver code resides in a directory of the application using it, it can be integrated by adding the subdirectory to the configuration as follows:

```cmake
add_subdirectory(sx126x_driver) # where sx126x_driver is the name of the folder containing the driver code
```

Alternatively, if the driver code is available through a code archive, it can be included directly by

```cmake
include(FetchContent)
FetchContent_Declare(
    sx126x_driver
    URL "path_to_archive" # Where path_to_archive is to be replaced by the path to the archive driver
)
FetchContent_MakeAvailable(sx126x_driver)
```

### Configuration

By default, LR-FHSS and BPSK are not included in the build of the driver. It can be added by adding the following line in the before including the driver code in the cmake suite:

```cmake
set(SX126X_ENABLE_BPSK ON CACHE BOOL "") # To enable BPSK
set(SX126X_ENABLE_LR_FHSS ON CACHE BOOL "") # To enable LR-FHSS
```

---

## Project-specific notes (added by this project, not upstream)

Everything above this line is Semtech's own README, kept as-is. This
section documents how this driver is actually used in THIS project —
see `core/vendor/README.md` (one level up) for why it's vendored here
at all rather than under either board's own `vendor/`.

### Build integration

This project doesn't use the CMake integration described above — it's
compiled directly as part of `check-board`/`check-pico`'s own compile
step (see the Makefile), same as every other `core/*.c` file. Both
boards now have a real HAL implementation (see below). LR-FHSS and BPSK
(`sx126x_bpsk.c`, `lr_fhss_mac.c`, `sx126x_lr_fhss.c`) are not used by
this project (plain LoRa modulation only, per the original hardware
handoff) and are never added to any `*_SOURCES` list — left in the
tree because splitting them out would diverge from upstream for no
benefit, not because they're needed.

### The HAL contract — confirmed by reading sx126x.c itself

Every one of `sx126x.c`'s 74 public functions delegates entirely to
one of four functions declared in `sx126x_hal.h`:

```c
sx126x_hal_status_t sx126x_hal_write ( const void* context, const uint8_t* command, uint16_t command_length,
                                        const uint8_t* data, uint16_t data_length );
sx126x_hal_status_t sx126x_hal_read  ( const void* context, const uint8_t* command, uint16_t command_length,
                                        uint8_t* data, uint16_t data_length );
sx126x_hal_status_t sx126x_hal_reset ( const void* context );
sx126x_hal_status_t sx126x_hal_wakeup( const void* context );
```

The driver core never touches SPI or GPIO directly — `context` is an
opaque `const void*` it never dereferences itself; each board's HAL
implementation defines its own context struct (SPI peripheral handle +
NSS/BUSY/DIO1/RESET pin identifiers for that board's actual wiring)
and casts it internally. NSS-assert / BUSY-poll / SPI-transfer /
BUSY-poll-again / NSS-deassert is entirely the HAL implementer's job
inside `hal_write`/`hal_read` — the core has no opinion on any of it.
Two separate implementations were needed, one per board; both now
exist (`board/STM32N6570-DK/sx1262_hal.c`,
`board/Raspberry_Pi_Pico/sx1262_hal.c`), each with its own
`sx1262_rx_selftest` under `laptop/tools/`.

### From the real hardware datasheet (SmartElex SX1262 HAT, E22-900M22S)

Confirmed with the user this module supersedes the Waveshare
Core1262-HF from the original hardware plan. Applies to both boards'
future HAL implementations regardless of MCU:

- Poll BUSY low before every SPI transaction — a command sent while
  BUSY is high is silently dropped, no error surfaced.
- After toggling NRST, BUSY stays high ~3ms for internal calibration —
  an extra wait beyond the normal per-transaction check, needed inside
  `sx126x_hal_reset`.
- Max SPI clock 18MHz, SPI mode 0.
- Command structure over SPI is Opcode → Address/Params → Data,
  matching `sx126x_hal_write`/`_read`'s own `command`/`data` split
  exactly — nothing extra needed to bridge the two.
- The module has RXEN/TXEN RF-switch control pins — genuinely
  different from the originally-planned Waveshare module. Both HALs
  drive these directly from the MCU (by snooping SetTx/SetRx opcodes
  in `apply_rf_switch_for_opcode()`) rather than relying on DIO2
  auto-switching, so this is decided at the code level. On the DK it's
  confirmed against the real SmartElex HAT; on the Pico the code was
  ported unchanged but whether the Pico's own physical module actually
  has a matching RXEN/TXEN arrangement is unverified against real
  hardware — see `board/Raspberry_Pi_Pico/sx1262_hal.c`'s "RXEN/TXEN
  note" and `ARCHITECTURE.md`'s Open Architectural Questions.

### GPIO pin assignments

Decided separately per board (the datasheet's own "Raspberry Pi SPI
Wiring" table is for a Raspberry Pi single-board computer running
Linux, and doesn't transfer to either of this project's actual targets
— STM32N6570-DK, or the Pico's bare-metal RP2040). See each board's
own `sx1262_hal.c` for its NSS/BUSY/DIO1/RESET/RXEN/TXEN pin
definitions.

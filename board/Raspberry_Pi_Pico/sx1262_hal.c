/*
 * board/Raspberry_Pi_Pico/sx1262_hal.c
 *
 * SX1262 LoRa radio HAL for the Raspberry Pi Pico (RP2040) board
 * target. Mirror image of board/STM32N6570-DK/'s own sx1262_hal.c
 * (same four sx126x_hal_*() functions core/vendor/sx126x_driver/src/
 * sx126x_hal.h requires, same lora_spi_transmit() override that
 * replaces core/comms_transport.c's weak default), but this board's
 * BSP has no Pico-SDK dependency and no SPI driver of its own
 * (confirmed: board/Raspberry_Pi_Pico/vendor/mtk3_bsp/device/ only
 * has adc/common/i2c/ser - no spi/), so this file talks to raw RP2040
 * SPI0 registers directly, the same way this BSP's own i2c_rp2040.c
 * and adc_rp2040.c talk to their peripherals (RESETS_RESET pulse +
 * poll RESETS_RESET_DONE, out_w/in_w/set_w/clr_w over macros in
 * sys/sysdepend/cpu/rp2040/sysdef.h).
 *
 * PIN ASSIGNMENT AND SPI PERIPHERAL CHOICE ARE STILL PLACEHOLDERS.
 * As of this pass the real wiring of the SX1262 module to this board
 * had not yet been decided - every SX1262_PIN_ / SX1262_SPI_ macro
 * below is a build-time-overridable default, not a confirmed hardware
 * fact, and MUST be revisited (either by overriding the macros at
 * build time or editing the defaults below) once the actual wiring is
 * fixed. Defaults chosen: SPI0 on its usual GP16/18/19 (MISO/SCK/
 * MOSI) function-select pins, NSS/NRESET/BUSY on nearby SIO-mode GPIO
 * (17/20/21) via this BSP's own gpio_set_pin()/gpio_set_val()/
 * gpio_get_val() wrapper (bsp/libbsp.h) rather than raw SIO registers,
 * per the task's own instruction that this board's OWN pins go
 * through libbsp, not the DK's GPIO handling.
 *
 * SPI0_BASE/SPI1_BASE and the PL022 register offsets/fields
 * (SSPCR0/SSPCR1/SSPDR/SSPSR and friends) were NOT present in the
 * vendored sysdef.h - confirmed by reading the file, not assumed from
 * the task brief that referenced them. Added there (RP2040 datasheet
 * values, not board-specific) rather than duplicated locally here, so
 * any other rp2040 code that later needs the SPI block can reuse the
 * same names.
 *
 * No usec-granularity delay primitive was found anywhere in this BSP
 * (checked tk/sysdepend/cpu/rp2040/syslib.h and tk/tkernel.h) - only
 * tk_dly_tsk()'s millisecond ticks, already used elsewhere in this
 * board's main_board.c. Reset/wakeup timing below uses that, which is
 * coarser than the SX1262 datasheet's ~100us reset-pulse minimum but
 * errs on the safe (longer) side; revisit if real hardware bring-up
 * shows it's insufficient.
 *
 * No real radio has been validated against this file yet - same
 * "next concrete step once you're ready to actually flash the board"
 * caveat as the rest of this board's bring-up work.
 *
 * RXEN/TXEN (RF switch control): this module exposes separate
 * active-high RXEN/TXEN pins rather than auto-switching off DIO2, per
 * the SmartElex SX1262 868M HAT's own SPI manual. Ported the DK
 * board's opcode-snoop approach (apply_rf_switch_for_opcode() below,
 * called from sx126x_hal_write()) rather than wiring DIO2 - keeps
 * both boards' HALs consistent. SX1262_PIN_RXEN/SX1262_PIN_TXEN
 * default to GP22/GP26; override at build time if wired differently.
 */

#include <sys/machine.h>
#ifdef CPU_RP2040

#include <stdint.h>
#include <stdbool.h>

#include <tk/tkernel.h>
#include <bsp/libbsp.h>

#include "sx126x.h"
#include "sx126x_hal.h"
#include "lora_frame.h"        /* lora_frame_try_parse(), LORA_FRAME_MAX_*
                                   - RX path below (sx1262_rx_poll()) */
#include "comms_transport.h"   /* comms_transport_deliver_received() -
                                   RX path below (sx1262_rx_poll()) */
#include "lora_radio_params.h" /* LOCKED PHY params shared with the DK HAL -
                                   see that header's top comment (Track 1) */

/* ------------------------------------------------------------------ */
/* Pin assignment - see file header. Override at build time (-D) once
 * real wiring is decided; these are only the fallback defaults. */
#ifndef SX1262_PIN_MISO
#define SX1262_PIN_MISO   16u   /* SPI0 RX */
#endif
#ifndef SX1262_PIN_NSS
#define SX1262_PIN_NSS    17u   /* manual GPIO, NOT SPI0's hardware CSn -
                                  * the sx126x_hal contract needs CS held
                                  * across a whole command+data
                                  * transaction, which this BSP's own
                                  * SPI block does not do for you. */
#endif
#ifndef SX1262_PIN_SCK
#define SX1262_PIN_SCK    18u   /* SPI0 SCK */
#endif
#ifndef SX1262_PIN_MOSI
#define SX1262_PIN_MOSI   19u   /* SPI0 TX */
#endif
#ifndef SX1262_PIN_NRESET
#define SX1262_PIN_NRESET 20u
#endif
#ifndef SX1262_PIN_BUSY
#define SX1262_PIN_BUSY   21u
#endif
#ifndef SX1262_PIN_DIO1
#define SX1262_PIN_DIO1   15u   /* IRQ line, plain GPIO input, polled (see
                                  * sx1262_rx_poll() below) - not a real
                                  * interrupt, matching how SX1262_PIN_BUSY
                                  * above is also just polled on this board.
                                  * Default only, same as every other
                                  * SX1262_PIN_* here: this board's actual
                                  * wiring of DIO1 has not been confirmed
                                  * against real hardware yet, so GP15 is
                                  * a placeholder pick (adjacent to the
                                  * SPI0 MISO/NSS/SCK/MOSI block on
                                  * GP16-19, otherwise unused by this
                                  * file) - override at build time (-D)
                                  * or edit this default once real wiring
                                  * is decided, per this file's own
                                  * top-of-file caveat. */
#endif
#ifndef SX1262_PIN_RXEN
#define SX1262_PIN_RXEN   22u   /* RF switch RX enable, active-high -
                                  * ported from board/STM32N6570-DK/'s
                                  * sx1262_hal.c opcode-snoop approach,
                                  * see apply_rf_switch_for_opcode()
                                  * below. DIO2-automatic-switching is
                                  * NOT used, matching the DK board. */
#endif
#ifndef SX1262_PIN_TXEN
#define SX1262_PIN_TXEN   26u   /* RF switch TX enable, active-high */
#endif

/* Opcodes this file needs to snoop for RXEN/TXEN switching (see
 * apply_rf_switch_for_opcode() below). Not exported by sx126x_hal.h -
 * the HAL boundary is deliberately opaque bytes - so re-declared here,
 * same values as board/STM32N6570-DK/'s own copy of this comment,
 * cross-checked against sx126x.c's private sx126x_commands_e. */
#define SX1262_OPCODE_SET_SLEEP     0x84u
#define SX1262_OPCODE_SET_TX        0x83u
#define SX1262_OPCODE_SET_RX        0x82u

#ifndef SX1262_SPI_BASE
#define SX1262_SPI_BASE       SPI0_BASE
#endif
#ifndef SX1262_SPI_RESET_BIT
#define SX1262_SPI_RESET_BIT  RESETS_RESET_SPI0
#endif

/* SSPCPSR/SSPCR0 SCR bring-up clock setting - a conservative starting
 * point (slow), not a measured/tuned value; this BSP doesn't expose
 * clk_peri's actual running frequency anywhere checked, so an exact
 * bit rate can't be computed from this codebase alone. CPSR must be
 * an even value 2..254 per the PL022 spec. Revisit once real hardware
 * bring-up can measure actual SCLK on a scope/logic analyzer. */
#ifndef SX1262_SPI_CPSR
#define SX1262_SPI_CPSR 62u
#endif
#ifndef SX1262_SPI_SCR
#define SX1262_SPI_SCR  0u
#endif

/* ------------------------------------------------------------------ */

static bool g_sx1262_spi_ready = false;

/*
 * TX-busy tracking for comms_transport.c's dispatcher (lora_radio_tx_
 * busy() below) - set the instant lora_spi_transmit() successfully
 * keys the radio, cleared once sx1262_rx_poll() observes TX_DONE (or
 * Timeout) on a later poll. Previously this bit was read off DIO1/
 * IRQ status and silently discarded (the IRQ mask already includes
 * TX_DONE - see sx1262_hal_init()'s dio_irq_params call below - but
 * nothing consumed it); this is what actually surfaces it.
 *
 * g_lora_tx_busy_poll_count: defensive watchdog, not a real-time
 * deadline. sx126x_set_tx(NULL, 0u) below (unlike the DK's own
 * lora_spi_transmit(), which passes a 1000ms hardware timeout to
 * set_tx() and therefore gets TX_DONE-or-Timeout guaranteed by the
 * radio itself) configures no hardware TX watchdog on this board, so
 * a stuck TX_DONE (radio fault, corrupted state) would otherwise wedge
 * this flag - and therefore the whole TX queue behind it - forever.
 * Counts consecutive sx1262_rx_poll() calls seen while busy without
 * observing TX_DONE; forced clear past SX1262_TX_BUSY_POLL_LIMIT.
 * Deliberately a poll-count, not a tick/ms-based timeout: BOARD_TICK_
 * DELAY_MS is 0 (see main_board.c) so there is no real-time pacing to
 * measure against here - a bring-up-appropriate approximation, same
 * "stated rather than hidden" spirit as comms_transport.c's own
 * COMMS_TRANSPORT_CHANNEL_BYTES_PER_TICK caveat.
 */
static volatile int g_lora_tx_busy = 0;
static uint32_t g_lora_tx_busy_poll_count = 0u;

/* Diagnostic: how many times sx1262_enter_rx()'s post-TX re-arm (in
 * sx1262_rx_poll() below) has failed. This file deliberately doesn't
 * mirror the DK HAL's lazy g_radio_configured retry-gate pattern (see
 * sx1262_hal_init()'s own doc on why), so a failed re-arm here has no
 * automatic retry - counting it at least makes the failure visible
 * instead of silently leaving the radio deaf until the next TX. */
static uint32_t g_sx1262_rx_rearm_failures = 0u;

#ifndef SX1262_TX_BUSY_POLL_LIMIT
#define SX1262_TX_BUSY_POLL_LIMIT 100000u
#endif

static void sx1262_spi_init(void)
{
    if (g_sx1262_spi_ready)
    {
        return;
    }

    /* Bring SPI0 out of reset - same set_w/clr_w-then-poll-
       RESETS_RESET_DONE pattern this BSP's own i2c_rp2040.c and
       adc_rp2040.c already use for their peripherals. */
    set_w(RESETS_RESET, SX1262_SPI_RESET_BIT);
    clr_w(RESETS_RESET, SX1262_SPI_RESET_BIT);
    while ((in_w(RESETS_RESET_DONE) & SX1262_SPI_RESET_BIT) == 0)
    {
        /* spin */
    }

    /* Route SCK/MOSI/MISO through the SPI peripheral function select;
       MISO needs input-enable on its pad, same as this BSP's I2C
       driver does for its own pins. */
    out_w(GPIO_CTRL(SX1262_PIN_SCK),  GPIO_CTRL_FUNCSEL_SPI);
    out_w(GPIO_CTRL(SX1262_PIN_MOSI), GPIO_CTRL_FUNCSEL_SPI);
    out_w(GPIO_CTRL(SX1262_PIN_MISO), GPIO_CTRL_FUNCSEL_SPI);
    out_w(GPIO(SX1262_PIN_MISO), GPIO_IE);

    /* NSS/NRESET/BUSY are plain SIO GPIOs on THIS board's own pins -
       routed through this BSP's own libbsp wrapper, not raw SIO
       registers and not the DK's GPIO handling, per the task's own
       instruction. */
    gpio_set_pin(SX1262_PIN_NSS, GPIO_MODE_OUT);
    gpio_set_val(SX1262_PIN_NSS, 1);        /* idle deasserted (active-low CS) */
    gpio_set_pin(SX1262_PIN_NRESET, GPIO_MODE_OUT);
    gpio_set_val(SX1262_PIN_NRESET, 1);     /* idle out of reset (active-low) */
    gpio_set_pin(SX1262_PIN_BUSY, GPIO_MODE_IN);
    gpio_set_pin(SX1262_PIN_DIO1, GPIO_MODE_IN);   /* IRQ line - see
                                                       SX1262_PIN_DIO1's
                                                       own doc above;
                                                       configured the
                                                       same way as
                                                       SX1262_PIN_BUSY
                                                       just above, since
                                                       both are plain
                                                       polled GPIO
                                                       inputs on this
                                                       board. */

    /* RXEN/TXEN idle low (RF switch off) until a SetTx/SetRx command
       is actually seen - see apply_rf_switch_for_opcode() below. */
    gpio_set_pin(SX1262_PIN_RXEN, GPIO_MODE_OUT);
    gpio_set_val(SX1262_PIN_RXEN, 0);
    gpio_set_pin(SX1262_PIN_TXEN, GPIO_MODE_OUT);
    gpio_set_val(SX1262_PIN_TXEN, 0);

    /* Configure the PL022 block: disable while configuring, Motorola
       SPI format, 8-bit words, mode 0 (SPO=0/SPH=0 - SX126x's own
       datasheet requires CPOL=0/CPHA=0), master mode (MS bit left
       clear), then enable. */
    out_w(SSPCR1(SX1262_SPI_BASE), 0);
    out_w(SSPCPSR(SX1262_SPI_BASE), SX1262_SPI_CPSR);
    out_w(SSPCR0(SX1262_SPI_BASE),
          SSPCR0_DSS_8BIT | SSPCR0_FRF_MOTOROLA |
          (SX1262_SPI_SCR << SSPCR0_SCR_SHIFT));
    out_w(SSPCR1(SX1262_SPI_BASE), SSPCR1_SSE);

    g_sx1262_spi_ready = true;
}

/*
 * Iteration-count bounds (not wall-clock: this BSP exposes no usec-
 * granularity timer here - see this file's header note - and clk_peri's
 * actual running frequency is explicitly unmeasured, per
 * sx1262_spi_init()'s own SX1262_SPI_CPSR comment: "this BSP doesn't
 * expose clk_peri's actual running frequency anywhere checked, so an
 * exact bit rate can't be computed from this codebase alone"). Sized
 * the same way as board/STM32N6570-DK/sx1262_hal.c's identical bounds
 * (see that file's own derivation comment) - against the actual
 * physical worst case, not picked blind: this file's own SX1262_SPI_CPSR
 * (62) / SX1262_SPI_SCR (0) config against the RP2040 datasheet's rated
 * clk_peri ceiling (125MHz) as the fastest plausible clock this bound
 * has to survive without a false timeout, and the SX126x datasheet's
 * own Tswmode (~3.5ms max BUSY-high duration for a mode-switching
 * command) for the BUSY bound:
 *   - SX1262_SPI_XFER_TIMEOUT_ITERS: one byte at CPSR=62 off a 125MHz
 *     clk_peri ceiling takes ~4us; even at a handful of cycles per spin
 *     iteration this bound clears that with well over 100x margin.
 *   - SX1262_WAIT_NOT_BUSY_TIMEOUT_ITERS: covering the ~3.5ms Tswmode
 *     figure at that same 125MHz ceiling needs on the order of tens of
 *     thousands of spin iterations even accounting for this BSP's
 *     libbsp gpio_get_val() wrapper call overhead (more expensive per
 *     iteration than a raw register read); this bound clears that with
 *     generous margin. Deliberately far larger than the SPI transfer
 *     bound - BUSY can legitimately stay high for a whole command's
 *     internal processing, not just one byte's shift time.
 * Both are generous-but-bounded, same "crude, uncalibrated, revisit
 * once real hardware timing is measured" basis this file already uses
 * elsewhere (e.g. SX1262_TX_BUSY_POLL_LIMIT above) - not a precise
 * real-time deadline, just a ceiling a genuinely wedged/disconnected
 * SPI bus or radio can't hide behind forever.
 */
#define SX1262_SPI_XFER_TIMEOUT_ITERS      100000u
#define SX1262_WAIT_NOT_BUSY_TIMEOUT_ITERS 2000000u

/* Blocking single-byte full-duplex transfer. PL022 FIFOs are 8 deep,
 * but this HAL only needs correctness here, not throughput - one
 * byte at a time keeps this file simple and matches how the four
 * sx126x_hal_*() functions below are called (small command/status
 * bursts, not bulk streaming).
 *
 * Bug fix: this used to spin on both FIFO flags with no timeout at
 * all. Returns 0 and writes the received byte to *in on success;
 * returns 1 (leaving *in untouched) if either flag never asserts
 * within SX1262_SPI_XFER_TIMEOUT_ITERS spins - see that macro's own
 * doc above for how this bound was sized. Callers (sx126x_hal_write/
 * read below) are responsible for leaving the bus in a consistent
 * state (deasserting NSS) on a timeout, same as they already do on the
 * success path - this leaf function only ever reports failure, never
 * guesses at recovery. */
static int sx1262_spi_xfer_byte(uint8_t out, uint8_t *in)
{
    uint32_t timeout;

    timeout = SX1262_SPI_XFER_TIMEOUT_ITERS;
    while ((in_w(SSPSR(SX1262_SPI_BASE)) & SSPSR_TNF) == 0)
    {
        /* wait for room in the TX FIFO */
        if (--timeout == 0u)
        {
            return 1;
        }
    }
    out_w(SSPDR(SX1262_SPI_BASE), out);

    timeout = SX1262_SPI_XFER_TIMEOUT_ITERS;
    while ((in_w(SSPSR(SX1262_SPI_BASE)) & SSPSR_RNE) == 0)
    {
        /* wait for the matching RX byte */
        if (--timeout == 0u)
        {
            return 1;
        }
    }
    *in = (uint8_t)in_w(SSPDR(SX1262_SPI_BASE));
    return 0;
}

/* Bug fix: this used to spin forever with no timeout at all, same
 * issue as board/STM32N6570-DK/sx1262_hal.c's identical wait_busy_low()
 * (see that file's own doc on why an unbounded wait here is wrong, not
 * just this file's - a dead/disconnected/miswired radio, a real risk
 * given this file's own "pin assignment still placeholder, no real
 * radio validated yet" caveats, would otherwise freeze this board's
 * entire single-task firmware forever with no diagnostic). Bounded to
 * SX1262_WAIT_NOT_BUSY_TIMEOUT_ITERS instead - returns 1 on timeout,
 * still without ever telling a caller BUSY was low when it wasn't. */
static int sx1262_wait_not_busy(void)
{
    uint32_t timeout = SX1262_WAIT_NOT_BUSY_TIMEOUT_ITERS;

    while (gpio_get_val(SX1262_PIN_BUSY) != 0)
    {
        /* spin - radio deasserts BUSY once ready for the next command */
        if (--timeout == 0u)
        {
            return 1;
        }
    }
    return 0;
}

/* RXEN/TXEN switching: this module's RF switch is driven directly by
 * this HAL (DIO2 left unconnected, matching the DK board's own
 * wiring choice), so hal_write snoops the opcode byte of every
 * command sent and drives the switch accordingly. Anything other
 * than SetTx/SetRx/SetSleep leaves both lines as they were - most
 * other commands are register/FIFO/config accesses issued while
 * already mid-TX or mid-RX, and toggling the switch on those would
 * be wrong. Ported verbatim (same opcodes, same behaviour) from
 * board/STM32N6570-DK/sx1262_hal.c's apply_rf_switch_for_opcode(). */
static void apply_rf_switch_for_opcode(uint8_t opcode)
{
    switch (opcode)
    {
    case SX1262_OPCODE_SET_TX:
        gpio_set_val(SX1262_PIN_RXEN, 0);
        gpio_set_val(SX1262_PIN_TXEN, 1);
        break;
    case SX1262_OPCODE_SET_RX:
        gpio_set_val(SX1262_PIN_TXEN, 0);
        gpio_set_val(SX1262_PIN_RXEN, 1);
        break;
    case SX1262_OPCODE_SET_SLEEP:
        gpio_set_val(SX1262_PIN_RXEN, 0);
        gpio_set_val(SX1262_PIN_TXEN, 0);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* core/vendor/sx126x_driver/src/sx126x_hal.h contract - all four
 * "shall be implemented by the user" functions. `context` is unused:
 * this board has exactly one SX1262, so there is nothing per-instance
 * to look up. */

/* Bug fix: every sx1262_spi_xfer_byte()/sx1262_wait_not_busy() return
 * code below used to be discarded - a SPI or BUSY timeout mid-command
 * would have silently sent a truncated/garbled command and still
 * reported success up through the vendored sx126x driver, which casts
 * this function's return straight through as its own status (see
 * sx126x.c's sx126x_set_*() wrappers). Now checked: a timeout leaves
 * NSS deasserted (same cleanup the success path already does - not a
 * peripheral reset, just finishing the GPIO sequence this function
 * already owns) and returns SX126X_HAL_STATUS_ERROR, which the return-
 * code checks already added elsewhere this session (sx1262_hal_init(),
 * sx1262_enter_rx(), lora_spi_transmit()) now correctly propagate and
 * act on instead of silently ignoring. Same fix as the DK's identical
 * functions. */
sx126x_hal_status_t sx126x_hal_write(const void* context, const uint8_t* command,
                                      const uint16_t command_length,
                                      const uint8_t* data, const uint16_t data_length)
{
    uint16_t i;
    uint8_t discard;

    (void)context;
    sx1262_spi_init();

    if (command_length > 0u)
    {
        apply_rf_switch_for_opcode(command[0]);
    }

    if (sx1262_wait_not_busy() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_set_val(SX1262_PIN_NSS, 0);

    for (i = 0u; i < command_length; i++)
    {
        if (sx1262_spi_xfer_byte(command[i], &discard) != 0)
        {
            gpio_set_val(SX1262_PIN_NSS, 1);
            return SX126X_HAL_STATUS_ERROR;
        }
    }
    for (i = 0u; i < data_length; i++)
    {
        if (sx1262_spi_xfer_byte(data[i], &discard) != 0)
        {
            gpio_set_val(SX1262_PIN_NSS, 1);
            return SX126X_HAL_STATUS_ERROR;
        }
    }

    gpio_set_val(SX1262_PIN_NSS, 1);

    return SX126X_HAL_STATUS_OK;
}

/* Bug fix: same class of fix as sx126x_hal_write() above - every
 * return code below used to be discarded, so a SPI or BUSY timeout
 * mid-read would have silently handed back partial/garbage data while
 * still reporting success. Now checked, with the same "deassert NSS,
 * then report SX126X_HAL_STATUS_ERROR" cleanup on a timeout. */
sx126x_hal_status_t sx126x_hal_read(const void* context, const uint8_t* command,
                                     const uint16_t command_length,
                                     uint8_t* data, const uint16_t data_length)
{
    uint16_t i;
    uint8_t discard;

    (void)context;
    sx1262_spi_init();
    if (sx1262_wait_not_busy() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_set_val(SX1262_PIN_NSS, 0);

    for (i = 0u; i < command_length; i++)
    {
        if (sx1262_spi_xfer_byte(command[i], &discard) != 0)
        {
            gpio_set_val(SX1262_PIN_NSS, 1);
            return SX126X_HAL_STATUS_ERROR;
        }
    }
    for (i = 0u; i < data_length; i++)
    {
        if (sx1262_spi_xfer_byte(SX126X_NOP, &data[i]) != 0)
        {
            gpio_set_val(SX1262_PIN_NSS, 1);
            return SX126X_HAL_STATUS_ERROR;
        }
    }

    gpio_set_val(SX1262_PIN_NSS, 1);

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_reset(const void* context)
{
    (void)context;
    sx1262_spi_init();

    gpio_set_val(SX1262_PIN_NRESET, 0);
    tk_dly_tsk(1);   /* datasheet wants >=100us low; see file header
                        note on this BSP having no finer-grained
                        delay primitive than tk_dly_tsk()'s 1ms tick */
    gpio_set_val(SX1262_PIN_NRESET, 1);
    tk_dly_tsk(1);   /* let the radio's own boot sequence start before
                        the first command is issued */

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void* context)
{
    (void)context;
    sx1262_spi_init();

    /* SX126x reference wakeup sequence: a brief NSS low pulse pulls
       the radio out of sleep, then wait for BUSY to clear before
       issuing any real command.
       Bug fix: this used to pulse NSS low then immediately high with
       NO delay at all between the two calls, and no pre-emptive BUSY
       wait before starting the pulse - unlike this board's own
       sx126x_hal_reset() (same file) and the DK's equivalent wakeup
       function, both of which insert a real delay and wait on BUSY
       first. Back-to-back gpio_set_val() calls give no guaranteed
       minimum NSS-low pulse width, risking a pulse too short for the
       SX126x to register the wake trigger. No usec-granularity delay
       exists on this BSP (see this file's header comment), so
       tk_dly_tsk(1) - the same coarse-but-only-available primitive
       sx126x_hal_reset() already uses - is the best available here
       too. Currently dead code (nothing in this codebase calls
       sx126x_wakeup()/sx126x_set_sleep()), but wrong regardless of
       whether it's exercised today. */
    if (sx1262_wait_not_busy() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    gpio_set_val(SX1262_PIN_NSS, 0);
    tk_dly_tsk(1);
    gpio_set_val(SX1262_PIN_NSS, 1);

    if (sx1262_wait_not_busy() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return SX126X_HAL_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* core/comms_transport.c's lora_spi_transmit() override - see that
 * file's own doc comment on the weak default this replaces for the
 * exact contract (packed frame bytes in, 0 on successful hand-off to
 * the radio, non-zero on failure). Minimal single-shot implementation
 * for bring-up: no fragmentation, no interrupt/DMA-driven completion
 * tracking - just standby -> load Tx buffer -> key the radio. */
int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len)
{
    /* sx126x_write_buffer()'s size parameter is a uint8_t - anything
       past 255 bytes can't be handed to the radio's Tx buffer in one
       call, and this bring-up pass doesn't implement fragmentation. */
    if (frame_len > 255u)
    {
        return -1;
    }

    sx1262_spi_init();

    if (sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC) != SX126X_STATUS_OK)
    {
        return -1;
    }

    /* pld_len_in_bytes has to match this specific send - re-set it
       rather than assume sx1262_hal_init()'s one-time bring-up value
       (0) still applies. Other fields are the same LOCKED values from
       core/lora_radio_params.h used in sx1262_hal_init() above -
       mirrors board/STM32N6570-DK/sx1262_hal.c's own
       lora_spi_transmit(). */
    {
        sx126x_pkt_params_lora_t pkt_params;

        pkt_params.preamble_len_in_symb = LORA_PREAMBLE_LEN_SYMB;
        pkt_params.header_type = LORA_HEADER_MODE;
        pkt_params.pld_len_in_bytes = (uint8_t)frame_len;
        pkt_params.crc_is_on = LORA_CRC_IS_ON;
        pkt_params.invert_iq_is_on = LORA_INVERT_IQ_IS_ON;
        if (sx126x_set_lora_pkt_params(NULL, &pkt_params) != SX126X_STATUS_OK)
        {
            return -1;
        }
    }

    if (sx126x_write_buffer(NULL, 0u, frame_bytes, (uint8_t)frame_len) != SX126X_STATUS_OK)
    {
        return -1;
    }

    /* timeout_in_ms = 0 -> no hardware Tx-timeout, matching the "hand
       off for transmission and return" contract lora_spi_transmit()
       documents - this call doesn't block for the transmission to
       finish, only for the command exchange with the radio itself. */
    if (sx126x_set_tx(NULL, 0u) != SX126X_STATUS_OK)
    {
        return -1;
    }

    g_lora_tx_busy = 1;
    g_lora_tx_busy_poll_count = 0u;

    return 0;
}

/* comms_transport.c's lora_radio_tx_busy() extern - see that file's
   weak default (used before this HAL is linked) and this board's
   g_lora_tx_busy doc above. */
int lora_radio_tx_busy(void)
{
    return g_lora_tx_busy;
}

/* ------------------------------------------------------------------ */
/* RX ingestion - mirrors board/STM32N6570-DK/sx1262_hal.c's own RX
 * work (same DIO1 IRQ mask, same enter/poll contract), so the two
 * boards behave identically here; this board just needed DIO1 pin
 * wiring + IRQ config added first (see SX1262_PIN_DIO1 above and
 * sx1262_hal_init() below), since neither existed on this board
 * before this pass.
 *
 * RXEN/TXEN note: this section does NOT add any new RF-switch
 * handling. RXEN/TXEN opcode-snooping (apply_rf_switch_for_opcode()
 * above) was already present in this file before this pass, ported
 * from the DK's own wiring - it is NOT re-derived or re-justified
 * here, and this pass does not add to it. Whether THIS board's
 * physical SX1262 module actually has an RXEN/TXEN RF-switch
 * arrangement matching the DK's SmartElex HAT is still unconfirmed
 * (open question, not verified against real Pico-side hardware) -
 * flagged here again explicitly so it isn't missed, but resolving it
 * is out of scope for this pass; SetRx below relies only on
 * apply_rf_switch_for_opcode()'s existing SX1262_OPCODE_SET_RX case,
 * whatever it ends up doing once that's confirmed. */

/* Cheap poll: DIO1 is a single SIO register read (gpio_get_val), so
 * checking it costs far less than a full SPI round trip - only fall
 * through to sx126x_get_irq_status() (which does cost one) when DIO1
 * is actually asserted. */
static int sx1262_dio1_is_asserted(void)
{
    return gpio_get_val(SX1262_PIN_DIO1) != 0u;
}

/* sx126x_set_standby() -> sx126x_set_rx_with_timeout_in_rtc_step(NULL,
 * SX126X_RX_CONTINUOUS) puts the radio into true continuous RX - per
 * sx126x.h's own doc, SX126X_RX_CONTINUOUS (0x00FFFFFF) means "the
 * chip stays in RX mode even after reception of a packet", unlike the
 * ms-based sx126x_set_rx(NULL, 0) wrapper this file used before this
 * pass, which always resolves to RX_SINGLE_MODE (stays in RX only
 * until ONE reception, then drops to standby RC). Converged onto the
 * DK's approach (board/STM32N6570-DK/sx1262_hal.c's own
 * sx1262_enter_rx()) rather than the other way around - see
 * ARCHITECTURE.md's Open Architectural Questions / CHANGELOG.md for
 * why this divergence existed and was flagged for reconciliation.
 *
 * Called both to first enter RX (from sx1262_hal_init() below) and to
 * re-arm after a poll that observed TxDone/Timeout on DIO1 (from
 * sx1262_rx_poll() below) - NOT after a successful RxDone, since with
 * continuous mode the chip is already still listening on its own
 * after a reception; see sx1262_rx_poll()'s own doc on that split. */
/* Bug fix: both sx126x_ calls below used to discard their return codes.
 * A failed call here left the radio silently not actually listening,
 * with no diagnostic and no retry - called both from sx1262_hal_init()
 * (bring-up) and sx1262_rx_poll() (after every TX completion/timeout),
 * so a single failed call could silently deafen the radio for the rest
 * of the run. Now returns 0 on success, 1 on the first failure, so
 * callers can decide whether to retry. Same fix as the DK's identical
 * function. */
int sx1262_enter_rx(void)
{
    sx1262_spi_init();
    if (sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC) != SX126X_STATUS_OK)
    {
        return 1;
    }
    if (sx126x_set_rx_with_timeout_in_rtc_step(NULL, SX126X_RX_CONTINUOUS) != SX126X_STATUS_OK)
    {
        return 1;
    }
    return 0;
}

/* Call every tick, same cadence as mission_link_task_step() in
 * board/Raspberry_Pi_Pico/main_board.c's seed loop - see that file.
 * Contract: cheap no-op when nothing has arrived (the common case,
 * one GPIO read); on RxDone, reads the frame out of the radio's Rx
 * FIFO, hands it to lora_frame_try_parse() and, on a successful
 * parse, to comms_transport_deliver_received(); any other outcome
 * (parse failure, buffer-status/read failure) drops the packet
 * silently, per lora_frame.h's own "drop and resync" policy - there
 * is no NACK path at the LoRa layer. Always clears whatever IRQ bits
 * were actually observed, but only re-enters RX on the TxDone/Timeout
 * branch, NOT after a successful RxDone - now mirrors
 * board/STM32N6570-DK/sx1262_hal.c's own sx1262_poll_rx() instead of
 * diverging from it (see sx1262_enter_rx()'s own doc above): with RX
 * now configured continuous, the chip is still listening on its own
 * after a reception, so a fresh standby->set_rx cycle there would
 * only reopen a listening gap this mode exists to avoid, for no
 * benefit. TxDone/Timeout, by contrast, means the chip auto-returned
 * to standby RC on its own (per the datasheet), so an explicit
 * re-arm IS required on that branch or the radio would sit deaf in
 * standby until the next transmit. */
void sx1262_rx_poll(void)
{
    sx126x_irq_mask_t irq = SX126X_IRQ_NONE;
    sx126x_rx_buffer_status_t rx_status;

    /* Watchdog advance has to happen BEFORE the DIO1 early-return
       below, not after it - the exact fault this guards against (a
       stuck TX that never raises TX_DONE) means DIO1 never asserts
       at all, so any watchdog logic placed after the early-return
       would never run in that scenario. Cheap (one flag check, one
       compare) even every poll. */
    if (g_lora_tx_busy)
    {
        g_lora_tx_busy_poll_count++;

        if (g_lora_tx_busy_poll_count > SX1262_TX_BUSY_POLL_LIMIT)
        {
            /* Force-clear rather than wedge the entire TX queue
               behind one stuck send indefinitely - see g_lora_tx_
               busy_poll_count's own doc above. The frame itself is
               simply lost; no retry at this layer, matching this
               project's existing "no NACK path at the LoRa layer"
               policy. */
            g_lora_tx_busy = 0;
            g_lora_tx_busy_poll_count = 0u;
        }
    }

    if (!sx1262_dio1_is_asserted())
    {
        return;
    }

    if (sx126x_get_irq_status(NULL, &irq) != SX126X_STATUS_OK)
    {
        /* Couldn't even read IRQ status over SPI - nothing safe to
           clear or act on; leave RX armed as-is and try again next
           poll rather than forcing a re-enter on a command that just
           failed. */
        return;
    }

    if ((irq & SX126X_IRQ_RX_DONE) != 0u)
    {
        uint8_t raw[LORA_FRAME_MAX_WIRE_SIZE];
        uint8_t out_type = 0u;
        uint8_t out_sender_board_id = 0u;
        uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t out_payload_len = 0u;

        if (sx126x_get_rx_buffer_status(NULL, &rx_status) == SX126X_STATUS_OK &&
            rx_status.pld_len_in_bytes <= sizeof(raw) &&
            sx126x_read_buffer(NULL, rx_status.buffer_start_pointer, raw,
                                rx_status.pld_len_in_bytes) == SX126X_STATUS_OK)
        {
            if (lora_frame_try_parse(raw, rx_status.pld_len_in_bytes, &out_type,
                                      &out_sender_board_id, out_payload,
                                      &out_payload_len) == 1)
            {
                /* out_sender_board_id: no further use here - see
                   comms_transport.h's own "Open question" note on why
                   LoRaFrame_t carries no sender field the receiving
                   board can act on; the payload's own embedded
                   board_id/team_id (if any) is what dispatch sites
                   actually use. */
                (void)out_sender_board_id;
                (void)comms_transport_deliver_received(out_type, out_payload,
                                                         out_payload_len);
            }
            /* parse_rc 0 (incomplete) or -1 (checksum/tag/length
               reject): drop, matching lora_frame_try_parse()'s own
               documented "drop and resync" policy. */
        }
        /* get_rx_buffer_status/read_buffer failure, or an
           implausible pld_len_in_bytes: drop for the same reason -
           nothing safe to hand upward. RX_DONE branch deliberately
           does NOT re-enter RX below - see this function's own doc
           on why (continuous mode, chip is still listening). */
    }

    /* Clear every IRQ bit this poll actually observed (not just
       RX_DONE) every time this function runs past the DIO1 check
       above - Timeout and TxDone are both routed to DIO1 too (see
       sx1262_hal_init()'s dio_irq_params call below), and leaving
       either uncleared would keep DIO1 asserted forever, making
       every future poll look like a fresh RxDone. */
    (void)sx126x_clear_irq_status(NULL, irq);

    if ((irq & SX126X_IRQ_RX_DONE) == 0u)
    {
        /* TxDone and/or Timeout, no RxDone. Per the datasheet, the
           chip auto-returns to standby RC once a TX completes or
           times out - re-arm RX so the radio goes back to listening
           instead of sitting deaf in standby until the next transmit.
           (RxDone can't reach this branch - handled above instead,
           and does NOT need this same re-arm: RX is configured
           continuous, see sx1262_enter_rx()'s own doc, so the chip is
           still listening on its own after a reception.)

           Given the configured IRQ mask (TxDone|RxDone|Timeout only -
           see sx1262_hal_init()), "not RxDone" here means whatever
           lora_spi_transmit() last keyed is no longer in flight
           either way - clears the busy flag comms_transport.c's
           dispatcher gates on. See g_lora_tx_busy's own doc above. */
        g_lora_tx_busy = 0;
        g_lora_tx_busy_poll_count = 0u;
        if (sx1262_enter_rx() != 0)
        {
            g_sx1262_rx_rearm_failures++;
        }
    }
}

/* One-time radio bring-up + IRQ config, called once from
 * board/Raspberry_Pi_Pico/main_board.c before the seed loop starts -
 * mirrors board/STM32N6570-DK/main_board.c's own sx1262_hal_init()
 * call ordering (radio bring-up first, before sim_runner_init() or
 * anything else that might try to use comms_transport). Now DOES
 * configure pkt_type/RF frequency/buffer base/modulation+packet
 * params/sync word/TX power, inline below, before the pre-existing
 * sx1262_enter_rx() call - all values imported from
 * core/lora_radio_params.h (Track 1), the single LOCKED source of
 * truth shared with board/STM32N6570-DK/sx1262_hal.c, not hardcoded
 * here.
 *
 * Deliberately does NOT mirror the DK HAL's shape for this config:
 * the DK puts its equivalent config in a separate, lazily-triggered
 * ensure_radio_configured(), guarded by a g_radio_configured flag,
 * called from lora_spi_transmit() and its RX poll function instead of
 * from its own sx1262_hal_init(). That lazy/guarded pattern exists on
 * the DK for a reason tangled up with a separate, already-flagged
 * DK-vs-Pico RX re-arm difference (see ARCHITECTURE.md) that's out of
 * scope here. This board's sx1262_hal_init() is already only ever
 * called once, at bring-up (confirmed in main_board.c), and already
 * unconditionally calls sx1262_enter_rx() at its own end - so the
 * config calls just go inline here, no guard flag needed since
 * there's only one call site. If a future reader is tempted to
 * refactor this into a DK-style lazy/guarded helper "for consistency
 * between the two boards," that's the RX re-arm issue above to
 * resolve first, not a shape mismatch to paper over here. */
/* Bug fix: every sx126x_ call below used to discard its return code,
 * so a failed call during this one-time bring-up (plausible given
 * this file's own header notes the pin assignments are still
 * placeholders) left the radio incompletely/incorrectly configured
 * with zero diagnostic anywhere - silent total-comms failure for the
 * entire run. Now returns 0 on success, 1 on the first failure, so
 * the caller (main_board.c) can fail loudly at boot the same way it
 * already does for sim_runner_init(), instead of running an entire
 * seed loop against a radio that was never actually configured. */
int sx1262_hal_init(void)
{
    sx1262_spi_init();
    if (sx126x_hal_reset(NULL) != SX126X_HAL_STATUS_OK) return 1;

    /* TxDone | RxDone | Timeout -> DIO1. Same mask as board/
       STM32N6570-DK/sx1262_hal.c's own sx126x_set_dio_irq_params()
       call, for consistency between the two boards' IRQ behavior -
       see that file's ensure_radio_configured(). irq_mask and
       dio1_mask both 0x0203 (TX_DONE|RX_DONE|TIMEOUT = bits 0,1,9 =
       0x001|0x002|0x200 = 0x203); dio2_mask/dio3_mask both 0 - DIO2/
       DIO3 unused by this HAL (DIO2 in particular is deliberately
       NOT used for RF-switch auto-control here, matching the DK's
       own wiring note on that same point). */
    if (sx126x_set_dio_irq_params(NULL, 0x0203u, 0x0203u, 0x0000u, 0x0000u) != SX126X_STATUS_OK) return 1;

    /* LOCKED PHY bring-up - values from core/lora_radio_params.h, the
       single source of truth shared with the DK's own
       ensure_radio_configured(). Order matches the DK's: pkt type,
       RF frequency, buffer base address, then mod params, then pkt
       params, then sync word, then TX power/ramp. */
    if (sx126x_set_pkt_type(NULL, SX126X_PKT_TYPE_LORA) != SX126X_STATUS_OK) return 1;
    if (sx126x_set_rf_freq(NULL, LORA_RF_FREQ_HZ) != SX126X_STATUS_OK) return 1;
    if (sx126x_set_buffer_base_address(NULL, 0u, 0u) != SX126X_STATUS_OK) return 1;

    {
        sx126x_mod_params_lora_t mod_params;

        mod_params.sf = LORA_SPREADING_FACTOR;
        mod_params.bw = LORA_BANDWIDTH;
        mod_params.cr = LORA_CODING_RATE;
        mod_params.ldro = LORA_LDRO;
        if (sx126x_set_lora_mod_params(NULL, &mod_params) != SX126X_STATUS_OK) return 1;
    }

    {
        sx126x_pkt_params_lora_t pkt_params;

        /* pld_len_in_bytes=0 here, same as the DK's own bring-up -
           lora_spi_transmit() below re-sets it per-send to match
           frame_len before actually keying TX. */
        pkt_params.preamble_len_in_symb = LORA_PREAMBLE_LEN_SYMB;
        pkt_params.header_type = LORA_HEADER_MODE;
        pkt_params.pld_len_in_bytes = 0u;
        pkt_params.crc_is_on = LORA_CRC_IS_ON;
        pkt_params.invert_iq_is_on = LORA_INVERT_IQ_IS_ON;
        if (sx126x_set_lora_pkt_params(NULL, &pkt_params) != SX126X_STATUS_OK) return 1;
    }

    if (sx126x_set_lora_sync_word(NULL, LORA_SYNC_WORD) != SX126X_STATUS_OK) return 1;
    if (sx126x_set_tx_params(NULL, LORA_TX_POWER_DBM, LORA_RAMP_TIME) != SX126X_STATUS_OK) return 1;

    if (sx1262_enter_rx() != 0) return 1;

    return 0;
}

#endif /* CPU_RP2040 */
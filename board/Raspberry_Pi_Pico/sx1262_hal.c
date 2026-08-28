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
 */

#include <sys/machine.h>
#ifdef CPU_RP2040

#include <stdint.h>
#include <stdbool.h>

#include <tk/tkernel.h>
#include <bsp/libbsp.h>

#include "sx126x.h"
#include "sx126x_hal.h"

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

/* Blocking single-byte full-duplex transfer. PL022 FIFOs are 8 deep,
 * but this HAL only needs correctness here, not throughput - one
 * byte at a time keeps this file simple and matches how the four
 * sx126x_hal_*() functions below are called (small command/status
 * bursts, not bulk streaming). */
static uint8_t sx1262_spi_xfer_byte(uint8_t out)
{
    while ((in_w(SSPSR(SX1262_SPI_BASE)) & SSPSR_TNF) == 0)
    {
        /* wait for room in the TX FIFO */
    }
    out_w(SSPDR(SX1262_SPI_BASE), out);

    while ((in_w(SSPSR(SX1262_SPI_BASE)) & SSPSR_RNE) == 0)
    {
        /* wait for the matching RX byte */
    }
    return (uint8_t)in_w(SSPDR(SX1262_SPI_BASE));
}

static void sx1262_wait_not_busy(void)
{
    while (gpio_get_val(SX1262_PIN_BUSY) != 0)
    {
        /* spin - radio deasserts BUSY once ready for the next command */
    }
}

/* ------------------------------------------------------------------ */
/* core/vendor/sx126x_driver/src/sx126x_hal.h contract - all four
 * "shall be implemented by the user" functions. `context` is unused:
 * this board has exactly one SX1262, so there is nothing per-instance
 * to look up. */

sx126x_hal_status_t sx126x_hal_write(const void* context, const uint8_t* command,
                                      const uint16_t command_length,
                                      const uint8_t* data, const uint16_t data_length)
{
    uint16_t i;

    (void)context;
    sx1262_spi_init();
    sx1262_wait_not_busy();

    gpio_set_val(SX1262_PIN_NSS, 0);

    for (i = 0u; i < command_length; i++)
    {
        (void)sx1262_spi_xfer_byte(command[i]);
    }
    for (i = 0u; i < data_length; i++)
    {
        (void)sx1262_spi_xfer_byte(data[i]);
    }

    gpio_set_val(SX1262_PIN_NSS, 1);

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void* context, const uint8_t* command,
                                     const uint16_t command_length,
                                     uint8_t* data, const uint16_t data_length)
{
    uint16_t i;

    (void)context;
    sx1262_spi_init();
    sx1262_wait_not_busy();

    gpio_set_val(SX1262_PIN_NSS, 0);

    for (i = 0u; i < command_length; i++)
    {
        (void)sx1262_spi_xfer_byte(command[i]);
    }
    for (i = 0u; i < data_length; i++)
    {
        data[i] = sx1262_spi_xfer_byte(SX126X_NOP);
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
       issuing any real command. */
    gpio_set_val(SX1262_PIN_NSS, 0);
    gpio_set_val(SX1262_PIN_NSS, 1);

    sx1262_wait_not_busy();

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

    return 0;
}

#endif /* CPU_RP2040 */

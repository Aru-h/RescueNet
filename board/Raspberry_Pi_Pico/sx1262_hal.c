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

sx126x_hal_status_t sx126x_hal_write(const void* context, const uint8_t* command,
                                      const uint16_t command_length,
                                      const uint8_t* data, const uint16_t data_length)
{
    uint16_t i;

    (void)context;
    sx1262_spi_init();

    if (command_length > 0u)
    {
        apply_rf_switch_for_opcode(command[0]);
    }

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

/* sx126x_set_standby() -> sx126x_set_rx(NULL, 0) puts the radio into
 * RX_SINGLE_MODE (see sx126x.h's own doc on sx126x_set_rx: timeout 0
 * = SX126X_RX_SINGLE_MODE - "stays in RX mode until a reception
 * occurs, then switch to standby RC"). Called both to first enter RX
 * (from sx1262_hal_init() below) and to re-arm after every poll that
 * observed DIO1 asserted (from sx1262_rx_poll() below) - per that
 * same doc, the radio does NOT re-arm itself after RxDone/Timeout, it
 * drops back to STDBY_RC, so an explicit re-enter is required every
 * time. */
void sx1262_enter_rx(void)
{
    sx1262_spi_init();
    (void)sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
    (void)sx126x_set_rx(NULL, 0u);
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
 * were actually observed and always re-enters RX before returning,
 * so a dropped/failed packet doesn't leave the radio stuck in
 * STDBY_RC waiting for the next call that never re-arms it. */
void sx1262_rx_poll(void)
{
    sx126x_irq_mask_t irq = SX126X_IRQ_NONE;
    sx126x_rx_buffer_status_t rx_status;

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
           nothing safe to hand upward. */
    }

    /* Clear every IRQ bit this poll actually observed (not just
       RX_DONE) every time this function runs past the DIO1 check
       above - Timeout and TxDone are both routed to DIO1 too (see
       sx1262_hal_init()'s dio_irq_params call below), and leaving
       either uncleared would keep DIO1 asserted forever, making
       every future poll look like a fresh RxDone. */
    (void)sx126x_clear_irq_status(NULL, irq);

    /* Re-enter RX unconditionally - see sx1262_enter_rx()'s own doc
       on why this is required after every DIO1 event, not just a
       successful RxDone. */
    sx1262_enter_rx();
}

/* One-time radio bring-up + IRQ config, called once from
 * board/Raspberry_Pi_Pico/main_board.c before the seed loop starts -
 * mirrors board/STM32N6570-DK/main_board.c's own sx1262_hal_init()
 * call ordering (radio bring-up first, before sim_runner_init() or
 * anything else that might try to use comms_transport). Does NOT
 * configure pkt_type/RF frequency/modulation params - lora_spi_
 * transmit() above doesn't either; that gap is pre-existing on this
 * board and out of scope for this pass (see this board's own
 * SPI/pin-assignment placeholder caveats at the top of this file). */
void sx1262_hal_init(void)
{
    sx1262_spi_init();
    (void)sx126x_hal_reset(NULL);

    /* TxDone | RxDone | Timeout -> DIO1. Same mask as board/
       STM32N6570-DK/sx1262_hal.c's own sx126x_set_dio_irq_params()
       call, for consistency between the two boards' IRQ behavior -
       see that file's ensure_radio_configured(). irq_mask and
       dio1_mask both 0x0203 (TX_DONE|RX_DONE|TIMEOUT = bits 0,1,9 =
       0x001|0x002|0x200 = 0x203); dio2_mask/dio3_mask both 0 - DIO2/
       DIO3 unused by this HAL (DIO2 in particular is deliberately
       NOT used for RF-switch auto-control here, matching the DK's
       own wiring note on that same point). */
    (void)sx126x_set_dio_irq_params(NULL, 0x0203u, 0x0203u, 0x0000u, 0x0000u);

    sx1262_enter_rx();
}

#endif /* CPU_RP2040 */
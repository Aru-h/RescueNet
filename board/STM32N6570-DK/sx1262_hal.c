/*
 * sx1262_hal.c - STM32N6570-DK HAL for the vendored sx126x_driver core
 * (core/vendor/sx126x_driver/), plus the strong lora_spi_transmit()
 * override that bridges comms_transport.c's frame-level send to it.
 *
 * -----------------------------------------------------------------------
 * Wiring this file assumes (DECIDED, see the conversation this file was
 * written from - not guessed): SmartElex SX1262 868M LoRa HAT
 * (E22-900M22S module), wired via the DK's STMod+ connector (CN4), not
 * the Arduino Uno V3 headers - the DK has no Raspberry Pi 40-pin header,
 * so a HAT-form-factor module has to be point-to-point wired regardless.
 *
 *   HAT pin   CN4 pin   MCU pin   Peripheral
 *   NSS       1         PA3       SPI5_NSS  (software-controlled here,
 *                                            not SPI5's own hardware NSS -
 *                                            see spi5_init() below)
 *   MOSI      2         PG2       SPI5_MOSI
 *   MISO      3         PH8       SPI5_MISO
 *   SCK       4         PE15      SPI5_SCK  (also drives on-board LED
 *                                            LD6 "Arduino D13" - harmless
 *                                            flicker during SPI traffic)
 *   DIO1      11        PC11      plain GPIO input, IRQ line (polled by
 *                                 this file today - see the DIO1 note
 *                                 in sx1262_hal_wait_irq() below)
 *   RESET     12        PB3       plain GPIO output, active-low NRST
 *   BUSY      14        PC7       plain GPIO input
 *   RXEN      D4        PH5       plain GPIO output, active-high
 *                                 (Arduino header pin, not STMod+/CN4 -
 *                                 free GPIO not used by any other on-
 *                                 board peripheral)
 *   TXEN      D7        PD6       plain GPIO output, active-high
 *                                 (Arduino header pin, not STMod+/CN4 -
 *                                 free GPIO not used by any other on-
 *                                 board peripheral)
 *   VCC       -         -         3.3V from the Arduino connector's CN8
 *                                 pin 4 (+3V3) - CN4 only brings out 5V
 *   GND       5 or 16   -         -
 *
 * REQUIRES a physical solder-bridge rework on the DK: STMod+ ships by
 * default routed to UART2, not SPI5. SB10/SB12/SB14/SB17 must go from
 * ON to OFF, and SB11/SB13/SB15/SB18 from OFF to ON, or CN4 pins 1-4
 * carry UART2 signals instead of SPI5 and this HAL talks to nothing.
 * Also: nothing may be plugged into the Arduino headers at the same
 * time - STMod+ and Arduino share the same underlying MCU pins
 * electrically (confirmed against the DK's own I/O-assignment table).
 *
 * DIO2 (module pin 8, "can control RF TX switch") is deliberately left
 * unconnected in this wiring - RXEN/TXEN are driven directly by this
 * file instead (see sx126x_hal_write() below), so the DIO2-automatic-
 * switching alternative the datasheet mentions isn't available and
 * isn't needed.
 *
 * -----------------------------------------------------------------------
 * Register-level, not STM32 HAL: board/STM32N6570-DK/vendor/mtk3_bsp2
 * (the vendored micro T-Kernel BSP) has device wrappers for I2C and ADC
 * but none for SPI or plain GPIO, and the Makefile's check-board-compile
 * doesn't grant an -I path to tron_board/Drivers/STM32N6xx_HAL_Driver
 * either. This file talks to SPI5/GPIOx registers directly via the
 * CMSIS device header (stm32n657xx.h) instead - confirmed to define
 * both SPI_TypeDef (the newer SPIv3 block: CR1/CR2/CFG1/CFG2/SR/TXDR/
 * RXDR, same family as STM32H7) and GPIO_TypeDef for this chip. This
 * needs one new -I path added to the Makefile's ARM_CFLAGS (not just
 * BOARD_SOURCES) to build under check-board-compile - see the Makefile
 * comment left next to CORE_INCLUDES for the exact line.
 *
 * SPI kept polled (no interrupts, no DMA): the datasheet's own BUSY
 * poll-before-every-transaction requirement makes each transaction a
 * blocking round trip anyway, and comms_transport's frame sends are
 * infrequent enough that this isn't a throughput concern.
 * -----------------------------------------------------------------------
 */

#include <stdint.h>
#include <stddef.h>

#include "stm32n657xx.h"

#include "sx126x.h"
#include "sx126x_hal.h"

#include "lora_frame.h"          /* lora_frame_try_parse(), LORA_FRAME_MAX_WIRE_SIZE */
#include "comms_transport.h"     /* comms_transport_deliver_received() */
#include "lora_radio_params.h"   /* LOCKED PHY params shared with the Pico HAL - see that header's top comment */

/*
 * -----------------------------------------------------------------------
 * --- Pin/peripheral map (see the wiring table above) --------------------
 * -----------------------------------------------------------------------
 */

#define SX1262_SPI                 SPI5

#define SX1262_NSS_PORT             GPIOA
#define SX1262_NSS_PIN              3u    /* PA3 */

#define SX1262_MOSI_PORT            GPIOG
#define SX1262_MOSI_PIN             2u    /* PG2  - AF5 (SPI5) */

#define SX1262_MISO_PORT            GPIOH
#define SX1262_MISO_PIN             8u    /* PH8  - AF5 (SPI5) */

#define SX1262_SCK_PORT              GPIOE
#define SX1262_SCK_PIN               15u  /* PE15 - AF5 (SPI5) */

#define SX1262_SPI_AF               5u    /* AF5 = SPI5 on PG2/PH8/PE15, per DK datasheet */

#define SX1262_BUSY_PORT            GPIOC
#define SX1262_BUSY_PIN             7u    /* PC7 */

#define SX1262_DIO1_PORT            GPIOC
#define SX1262_DIO1_PIN             11u   /* PC11 */

#define SX1262_RESET_PORT           GPIOB
#define SX1262_RESET_PIN            3u    /* PB3 */

#define SX1262_RXEN_PORT            GPIOH
#define SX1262_RXEN_PIN             5u    /* PH5 - Arduino header D4 */

#define SX1262_TXEN_PORT            GPIOD
#define SX1262_TXEN_PIN             6u    /* PD6 - Arduino header D7 */

/* Opcodes this file needs to snoop for RXEN/TXEN switching (see
 * sx126x_hal_write() below). Not exported by sx126x_hal.h - the HAL
 * boundary is deliberately opaque bytes - so re-declared here, values
 * cross-checked against sx126x.c's own private sx126x_commands_e
 * (0x84/0x83/0x82 respectively, unchanged). */
#define SX1262_OPCODE_SET_SLEEP     0x84u
#define SX1262_OPCODE_SET_TX        0x83u
#define SX1262_OPCODE_SET_RX        0x82u

/*
 * -----------------------------------------------------------------------
 * --- Tiny register-level GPIO helpers ------------------------------------
 * -----------------------------------------------------------------------
 */

static void gpio_clock_enable_all(void)
{
    /* GPIOA, B, C, D, E, F, G, H - every port this HAL touches. */
    RCC->AHB4ENR |= (RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN |
                      RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIODEN |
                      RCC_AHB4ENR_GPIOEEN | RCC_AHB4ENR_GPIOFEN |
                      RCC_AHB4ENR_GPIOGEN | RCC_AHB4ENR_GPIOHEN);
    /* RCC needs a couple of cycles before the newly-clocked
       peripherals' registers are safely accessible. */
    (void)RCC->AHB4ENR;
}

static void gpio_set_output(GPIO_TypeDef *port, uint32_t pin)
{
    port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (1u << (pin * 2u));   /* general-purpose output */
    port->OTYPER &= ~(1u << pin);                                            /* push-pull */
    port->OSPEEDR |= (2u << (pin * 2u));                                     /* high speed */
    port->PUPDR &= ~(3u << (pin * 2u));                                      /* no pull */
}

static void gpio_set_input(GPIO_TypeDef *port, uint32_t pin, uint32_t pull /* 0=none,1=up,2=down */)
{
    port->MODER &= ~(3u << (pin * 2u));                                      /* input */
    port->PUPDR = (port->PUPDR & ~(3u << (pin * 2u))) | (pull << (pin * 2u));
}

static void gpio_set_af(GPIO_TypeDef *port, uint32_t pin, uint32_t af)
{
    uint32_t idx = pin >> 3u;             /* AFR[0] = pins 0-7, AFR[1] = pins 8-15 */
    uint32_t shift = (pin & 7u) * 4u;

    port->MODER = (port->MODER & ~(3u << (pin * 2u))) | (2u << (pin * 2u));  /* alternate function */
    port->OSPEEDR |= (2u << (pin * 2u));
    port->PUPDR &= ~(3u << (pin * 2u));
    port->AFR[idx] = (port->AFR[idx] & ~(0xFu << shift)) | (af << shift);
}

static inline void gpio_write(GPIO_TypeDef *port, uint32_t pin, int level)
{
    /* BSRR: lower 16 bits set, upper 16 bits reset - single atomic write. */
    port->BSRR = level ? (1u << pin) : (1u << (pin + 16u));
}

static inline int gpio_read(GPIO_TypeDef *port, uint32_t pin)
{
    return (port->IDR & (1u << pin)) ? 1 : 0;
}

/*
 * -----------------------------------------------------------------------
 * --- SPI5, register level ------------------------------------------------
 * -----------------------------------------------------------------------
 * NSS handled entirely by software (gpio_write on SX1262_NSS_PORT/PIN),
 * not SPI5's own hardware NSS - simpler to reason about against the
 * datasheet's "assert NSS only after BUSY reads low" requirement than
 * fighting the peripheral's automatic NSS management.
 */

static void spi5_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_SPI5EN;
    (void)RCC->APB2ENR;

    gpio_set_af(SX1262_MOSI_PORT, SX1262_MOSI_PIN, SX1262_SPI_AF);
    gpio_set_af(SX1262_MISO_PORT, SX1262_MISO_PIN, SX1262_SPI_AF);
    gpio_set_af(SX1262_SCK_PORT, SX1262_SCK_PIN, SX1262_SPI_AF);

    SX1262_SPI->CR1 = 0u;   /* disable while configuring */

    /* CFG2: master mode, SPI motorola mode (COMM already 0 = full
       duplex by reset default), CPOL=0/CPHA=0 (SPI mode 0, per
       datasheet), software NSS management (SSM on, SSOE off - we drive
       NSS ourselves via GPIO), AFCNTR off (GPIOs stay under our own
       control, not the peripheral's).
       Bug fix: this used to write only SPI_CFG2_MASTER, leaving SSM at
       its reset value of 0 - which is *hardware* NSS management, the
       opposite of this comment's own stated intent, and with NSS routed
       as a plain GPIO output (not the SPI5 AF) rather than a real NSS
       pin, the peripheral's internal hardware NSS input is left
       floating/disconnected. Cross-checked against ST's SPIv3 (this
       family, same block as STM32H7) reference-manual guidance and
       community documentation, not just this comment's stated intent:
       setting SPI_CFG2_SSM alone is not sufficient either - in software
       NSS mode the *internal* NSS level comes from SPI_CR1_SSI, and
       leaving SSI at its reset value of 0 in master mode means the
       internal NSS reads as "selected" (low), which triggers exactly
       the same MODF (mode fault) condition SSM was meant to avoid. Both
       bits are required together for master mode with software-managed
       NSS: SPI_CFG2_SSM=1 (this line) and SPI_CR1_SSI=1 (set below,
       alongside enabling SPE). */
    SX1262_SPI->CFG2 = SPI_CFG2_MASTER | SPI_CFG2_SSM;

    /* CFG1: 8-bit data size, conservative baud-rate prescaler. SPI5 is
       on APB2; exact APB2 clock depends on the DK's final clock tree
       (not yet finalized alongside the rest of this bring-up), so this
       picks the most conservative (largest) prescaler available
       (/256) rather than assuming a specific APB2 frequency - safely
       under the module's 18MHz ceiling regardless, at the cost of
       transfer speed that can be tightened once the clock tree is
       confirmed. */
    SX1262_SPI->CFG1 = (7u << 28u) /* MBR = 0b111 = fPCLK/256 */
                        | (7u << 0u); /* DSIZE = 8 bits (7 = 8-1) */

    SX1262_SPI->CR2 = 0u; /* TSIZE managed per-transfer if needed; 0 = unlimited/continuous, fine for byte-at-a-time polling */

    /* SSI must be set alongside SPE, not after: with SSM=1, the
       internal NSS level is whatever SSI says at the moment mode-fault
       detection evaluates it, so this must never be enabled with SSI
       still 0 - see the CFG2 comment above for why. */
    SX1262_SPI->CR1 = SPI_CR1_SSI | SPI_CR1_SPE; /* enable, NSS forced high internally */
}

/*
 * Iteration-count bounds (not wall-clock: no calibrated timer exists at
 * this call depth, and this file's own SystemCoreClock/APB2 frequency
 * is explicitly unresolved - see spi5_init()'s CFG1 comment above,
 * "exact APB2 clock depends on the DK's final clock tree, not yet
 * finalized"). Sized against the actual physical worst case rather
 * than picked blind: cross-checked the SPI5 config actually chosen
 * above (CFG1's deliberately most-conservative /256 prescaler) and the
 * SX126x datasheet's own documented Tswmode (~3.5ms max BUSY-high
 * duration for a mode-switching command - matches this file's existing
 * "~3ms post-reset BUSY stretch" figure used by crude_delay_cycles()
 * above) against the Cortex-M55's rated maximum core clock for this
 * part (800MHz, STM32N657 datasheet) as the fastest plausible clock
 * this bound has to survive without a false timeout:
 *   - SPI5_XFER_TIMEOUT_ITERS: one byte at /256 off an (unrealistically
 *     generous) 800MHz APB2 takes ~2.6us; even at a handful of cycles
 *     per spin iteration this bound clears that with >100x margin.
 *   - WAIT_BUSY_LOW_TIMEOUT_ITERS: covering the ~3.5ms Tswmode figure
 *     at the same 800MHz ceiling needs on the order of a few hundred
 *     thousand spin iterations; this bound clears that with similar
 *     margin. Deliberately far larger than SPI5_XFER_TIMEOUT_ITERS -
 *     BUSY can legitimately stay high for a whole command's internal
 *     processing, not just one byte's shift time.
 * Both are generous-but-bounded on the same "crude, uncalibrated,
 * revisit once a real timer exists" basis this file already documents
 * for crude_delay_cycles() - not a precise real-time deadline, just a
 * ceiling a genuinely wedged/disconnected SPI bus or radio can't hide
 * behind forever.
 */
#define SPI5_XFER_TIMEOUT_ITERS      100000u
#define WAIT_BUSY_LOW_TIMEOUT_ITERS  2000000u

/* Returns 0 and writes the received byte to *in on success; returns 1
   (leaving *in untouched) if either flag never asserts within
   SPI5_XFER_TIMEOUT_ITERS spins - see that macro's own doc above for
   how this bound was sized. Callers (sx126x_hal_write/read below) are
   responsible for leaving the bus in a consistent state (deasserting
   NSS) on a timeout, same as they already do on the success path -
   this leaf function only ever reports failure, never guesses at
   recovery. */
static int spi5_xfer_byte(uint8_t out, uint8_t *in)
{
    uint32_t timeout;

    /* Kick a single-byte transfer. */
    SX1262_SPI->CR1 |= SPI_CR1_CSTART;

    timeout = SPI5_XFER_TIMEOUT_ITERS;
    while (!(SX1262_SPI->SR & SPI_SR_TXP))
    {
        if (--timeout == 0u)
        {
            return 1;
        }
    }
    *(volatile uint8_t *)&SX1262_SPI->TXDR = out;

    timeout = SPI5_XFER_TIMEOUT_ITERS;
    while (!(SX1262_SPI->SR & SPI_SR_RXP))
    {
        if (--timeout == 0u)
        {
            return 1;
        }
    }
    *in = *(volatile uint8_t *)&SX1262_SPI->RXDR;
    return 0;
}

/*
 * -----------------------------------------------------------------------
 * --- BUSY / RESET timing, per the SmartElex SX1262 HAT datasheet --------
 * -----------------------------------------------------------------------
 */

/* Crude cycle-count busy-wait - no timer/tick source wired up for this
   HAL yet, so this is approximate, not calibrated against a real clock
   frequency. Generous enough to cover the datasheet's ~3ms post-reset
   BUSY stretch at any plausible core clock for this part; genuinely
   imprecise for anything tighter and should be replaced with a real
   timer-backed delay once one exists in this file's reach. */
static void crude_delay_cycles(uint32_t n)
{
    volatile uint32_t i;
    for (i = 0u; i < n; i++)
    {
    }
}

/* Bug fix: this used to spin forever with no timeout at all - "no
   timeout on purpose" per the removed comment, reasoning that per the
   datasheet a command issued while BUSY is high is silently dropped,
   so only looping until BUSY actually clears could honor that. That
   reasoning still holds for what happens on SUCCESS - this function
   still never lets a caller proceed while BUSY reads high - but it
   doesn't justify spinning forever: a dead/disconnected/miswired radio
   (a real risk this file's own wiring notes call out, given the
   required solder-bridge rework) left BUSY permanently high with no
   way to ever recover, freezing the entire single-task firmware
   (mission-link, sim tick, telemetry) with no diagnostic. Bounded to
   WAIT_BUSY_LOW_TIMEOUT_ITERS instead (see that macro's own doc above
   spi5_xfer_byte() for how it was sized against the datasheet's actual
   Tswmode figure, not picked blind) - returns 1 on timeout, still
   without ever telling a caller BUSY was low when it wasn't. */
static int wait_busy_low(void)
{
    uint32_t timeout = WAIT_BUSY_LOW_TIMEOUT_ITERS;

    while (gpio_read(SX1262_BUSY_PORT, SX1262_BUSY_PIN) != 0)
    {
        if (--timeout == 0u)
        {
            return 1;
        }
    }
    return 0;
}

/*
 * -----------------------------------------------------------------------
 * --- One-time init, called once from main_board.c before any sx126x_*
 *     call is made -----------------------------------------------------
 * -----------------------------------------------------------------------
 */

void sx1262_hal_init(void)
{
    gpio_clock_enable_all();

    gpio_set_output(SX1262_NSS_PORT, SX1262_NSS_PIN);
    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);   /* idle high (deselected) */

    gpio_set_input(SX1262_BUSY_PORT, SX1262_BUSY_PIN, 0u);
    gpio_set_input(SX1262_DIO1_PORT, SX1262_DIO1_PIN, 0u);

    gpio_set_output(SX1262_RESET_PORT, SX1262_RESET_PIN);
    gpio_write(SX1262_RESET_PORT, SX1262_RESET_PIN, 1);   /* NRST idle high (not asserted) */

    gpio_set_output(SX1262_RXEN_PORT, SX1262_RXEN_PIN);
    gpio_set_output(SX1262_TXEN_PORT, SX1262_TXEN_PIN);
    gpio_write(SX1262_RXEN_PORT, SX1262_RXEN_PIN, 0);
    gpio_write(SX1262_TXEN_PORT, SX1262_TXEN_PIN, 0);

    spi5_init();
}

/*
 * -----------------------------------------------------------------------
 * --- The four sx126x_hal.h entry points ---------------------------------
 * -----------------------------------------------------------------------
 */

sx126x_hal_status_t sx126x_hal_reset(const void *context)
{
    (void)context;

    gpio_write(SX1262_RESET_PORT, SX1262_RESET_PIN, 0);   /* assert NRST, active-low */
    crude_delay_cycles(10000u);
    gpio_write(SX1262_RESET_PORT, SX1262_RESET_PIN, 1);   /* release */

    /* Per the datasheet: after NRST release, BUSY stays high ~3ms for
       internal calibration - an extra wait beyond the normal
       per-transaction check, done here explicitly rather than relying
       on the next hal_write's own wait_busy_low() to happen to cover
       it. */
    if (wait_busy_low() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    (void)context;

    /* Standard SX126x wake-from-sleep sequence: a brief NSS low/high
       pulse, then wait for BUSY to clear the internal wake-up
       procedure.
       Bug fix: the NSS-low pulse width used to be crude_delay_cycles
       (10u) - an arbitrarily tiny, uncalibrated cycle count with no
       basis shown for satisfying the SX126x's minimum wake-pulse
       timing, and inconsistent with sx126x_hal_reset()'s own reset-
       pulse delay of crude_delay_cycles(10000u) in this same file (a
       1000x difference with no documented reason). Matched to that
       same generous, already-established margin instead of leaving a
       second, smaller "generous margin" placeholder that isn't
       actually generous. Currently dead code (nothing in this
       codebase calls sx126x_wakeup()/sx126x_set_sleep()), but wrong
       regardless of whether it's exercised today. */
    if (wait_busy_low() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }
    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 0);
    crude_delay_cycles(10000u);
    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);
    if (wait_busy_low() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return SX126X_HAL_STATUS_OK;
}

/* RXEN/TXEN switching: this module's RF switch is driven directly by
   this HAL (DIO2 left unconnected - see the wiring note up top), so
   hal_write snoops the opcode byte of every command sent and drives
   the switch accordingly. Anything other than SetTx/SetRx (e.g.
   SetSleep, or all the read-only/config commands) leaves both lines
   low - matches the module's own "active high" RXEN/TXEN definition,
   and leaving both low outside an active TX/RX is the safe default. */
static void apply_rf_switch_for_opcode(uint8_t opcode)
{
    switch (opcode)
    {
    case SX1262_OPCODE_SET_TX:
        gpio_write(SX1262_RXEN_PORT, SX1262_RXEN_PIN, 0);
        gpio_write(SX1262_TXEN_PORT, SX1262_TXEN_PIN, 1);
        break;
    case SX1262_OPCODE_SET_RX:
        gpio_write(SX1262_TXEN_PORT, SX1262_TXEN_PIN, 0);
        gpio_write(SX1262_RXEN_PORT, SX1262_RXEN_PIN, 1);
        break;
    case SX1262_OPCODE_SET_SLEEP:
        gpio_write(SX1262_RXEN_PORT, SX1262_RXEN_PIN, 0);
        gpio_write(SX1262_TXEN_PORT, SX1262_TXEN_PIN, 0);
        break;
    default:
        /* Leave RF switch state as-is for every other command - most
           are register/FIFO/config accesses issued while already
           mid-TX or mid-RX, and toggling the switch on those would be
           wrong. */
        break;
    }
}

/* Bug fix: every spi5_xfer_byte()/wait_busy_low() return code below
 * used to be discarded - a SPI or BUSY timeout mid-command would have
 * silently sent a truncated/garbled command (or none at all) and still
 * reported success up through the vendored sx126x driver, which casts
 * this function's return straight through as its own status (see
 * sx126x.c's sx126x_set_*() wrappers). Now checked: a timeout leaves
 * NSS deasserted (same cleanup the success path already does - not a
 * peripheral reset, just finishing the GPIO sequence this function
 * already owns) and returns SX126X_HAL_STATUS_ERROR, which the return-
 * code checks already added elsewhere this session (ensure_radio_
 * configured(), sx1262_enter_rx(), lora_spi_transmit()) now correctly
 * propagate and act on instead of silently ignoring. */
sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command,
                                      const uint16_t command_length, const uint8_t *data,
                                      const uint16_t data_length)
{
    uint16_t i;

    (void)context;

    if (command_length > 0u)
    {
        apply_rf_switch_for_opcode(command[0]);
    }

    if (wait_busy_low() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 0);

    for (i = 0u; i < command_length; i++)
    {
        uint8_t discard;
        if (spi5_xfer_byte(command[i], &discard) != 0)
        {
            gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);
            return SX126X_HAL_STATUS_ERROR;
        }
    }
    for (i = 0u; i < data_length; i++)
    {
        uint8_t discard;
        if (spi5_xfer_byte(data[i], &discard) != 0)
        {
            gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);
            return SX126X_HAL_STATUS_ERROR;
        }
    }

    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);

    /* SetSleep is the one documented exception where BUSY does NOT
       clear immediately afterward (the chip is, correctly, asleep) -
       skip the trailing wait_busy_low() for it specifically, matching
       every reference sx126x HAL implementation's own handling of this
       opcode. */
    if (!(command_length > 0u && command[0] == SX1262_OPCODE_SET_SLEEP))
    {
        if (wait_busy_low() != 0)
        {
            return SX126X_HAL_STATUS_ERROR;
        }
    }

    return SX126X_HAL_STATUS_OK;
}

/* Bug fix: same class of fix as sx126x_hal_write() above - every
 * spi5_xfer_byte()/wait_busy_low() return code below used to be
 * discarded, so a SPI or BUSY timeout mid-read would have silently
 * handed back partial/garbage data (data[] left partly written from a
 * previous call) while still reporting success. Now checked, with the
 * same "deassert NSS, then report SX126X_HAL_STATUS_ERROR" cleanup on
 * a timeout. */
sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command,
                                     const uint16_t command_length, uint8_t *data,
                                     const uint16_t data_length)
{
    uint16_t i;

    (void)context;

    if (wait_busy_low() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 0);

    for (i = 0u; i < command_length; i++)
    {
        uint8_t discard;
        if (spi5_xfer_byte(command[i], &discard) != 0)
        {
            gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);
            return SX126X_HAL_STATUS_ERROR;
        }
    }
    for (i = 0u; i < data_length; i++)
    {
        if (spi5_xfer_byte(SX126X_NOP, &data[i]) != 0)
        {
            gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);
            return SX126X_HAL_STATUS_ERROR;
        }
    }

    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);

    if (wait_busy_low() != 0)
    {
        return SX126X_HAL_STATUS_ERROR;
    }

    return SX126X_HAL_STATUS_OK;
}

/*
 * -----------------------------------------------------------------------
 * --- lora_spi_transmit() - strong override of comms_transport.c's weak
 *     stub, bridging its frame-level send to the sx126x driver core -----
 * -----------------------------------------------------------------------
 */

static int g_radio_configured;

/*
 * TX-busy tracking for comms_transport.c's dispatcher (lora_radio_tx_
 * busy() below) - set the instant lora_spi_transmit() successfully
 * keys the radio, cleared once sx1262_poll_rx() observes TxDone (or
 * Timeout) on a later poll. No separate software watchdog needed on
 * this board (contrast board/Raspberry_Pi_Pico/sx1262_hal.c's own
 * poll-count watchdog): lora_spi_transmit() below already passes a
 * 1000ms timeout to sx126x_set_tx(), so the radio's own hardware
 * guarantees a Timeout IRQ (already routed to DIO1, same mask this
 * file already used before this flag existed) fires on its own if
 * TxDone never does - this flag is only ever left set for that
 * bounded 1000ms window, never indefinitely.
 */
static volatile int g_lora_tx_busy;

/*
 * Puts the radio into RX and leaves it there. Called once from
 * ensure_radio_configured() below (radio init) and NOT re-called by
 * sx1262_poll_rx() after each received packet - see this function's
 * own doc for why that's a correction against this file's original
 * RX prompt, not an oversight.
 *
 * CORRECTION vs. this file's original RX prompt, which assumed
 * sx126x_set_rx(NULL, 0) (timeout_in_ms=0) means "continuous RX" -
 * checked against the vendored driver instead of assumed, per that
 * prompt's own instruction, and that assumption is wrong. Per
 * core/vendor/sx126x_driver/src/sx126x.c's sx126x_set_rx():
 * timeout_in_ms is converted via sx126x_convert_timeout_in_ms_to_rtc_step(),
 * so timeout_in_ms=0 always converts to rtc_step 0 -
 * SX126X_RX_SINGLE_MODE (sx126x.h) - "the chip stays in RX mode until
 * a reception occurs, then switch to standby RC". That is exactly the
 * one-shot behavior this function needs to NOT have: it would mean
 * sx1262_poll_rx() DOES have to re-arm RX after every single packet
 * (and after every timeout, if a timeout were ever configured, which
 * it isn't here either).
 *
 * True continuous mode is a distinct sentinel, SX126X_RX_CONTINUOUS
 * (0x00FFFFFF, sx126x.h) - "the chip stays in RX mode even after
 * reception of a packet" - reachable only via the *_with_timeout_in_
 * rtc_step() entry point (sx126x_set_rx()'s own ms-based wrapper has
 * no way to express it, since it always runs timeout_in_ms through
 * the ms->rtc_step conversion first). That's what this function uses
 * below, which is also why sx1262_poll_rx() does NOT call this
 * function again after handling a received packet - the chip is
 * already still listening, per the driver's own doc quoted above; a
 * fresh standby->set_rx cycle after every packet would only reopen a
 * listening gap this mode exists to avoid, for no benefit.
 */
/* Bug fix: both calls below used to discard their return codes. A
 * failed call here left the radio silently not actually listening,
 * with no diagnostic and no retry - called both from ensure_radio_
 * configured() (bring-up) and sx1262_poll_rx() (after every TX
 * completion/timeout), so a single failed call could silently deafen
 * the radio for the rest of the run. Now returns 0 on success, 1 on
 * the first failure, so callers can decide whether to retry. */
static int sx1262_enter_rx(void)
{
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

/*
 * Radio bring-up this file can decide with actual certainty: reset,
 * standby, LoRa packet type, 868MHz RF frequency (this module's own
 * name - "SX1262 868M" - decides this one, not a guess), TX/RX FIFO
 * base addresses, and DIO1 routed to TxDone/RxDone/Timeout.
 *
 * SF/BW/CR, preamble length, sync word, CRC/header mode, and TX power
 * are protocol parameters that must MATCH whatever Track 2 (the Pico
 * board) configures its own SX1262 to, or the two boards' radios will
 * simply fail to hear each other - no crash, no damage, just silent
 * non-interop until synced. These are now LOCKED, defined once in
 * core/lora_radio_params.h (SF7/BW125/CR4:5, explicit header, CRC on,
 * sync word 0x34, +14dBm/200us ramp) and pulled in via the macros
 * below rather than hardcoded here - that header is the single source
 * of truth both boards' HALs read from, so this file no longer needs
 * (or gets) its own copy of these values to keep in sync by hand.
 */
/* Bug fix: every call below used to discard its return code, and
 * g_radio_configured was set to 1 unconditionally afterward regardless
 * of whether any of them actually failed - a partial/failed bring-up
 * (bad frequency, an SPI glitch corrupting an opcode byte) was
 * undetectable, and the radio would be believed fully configured while
 * running against whatever partial state actually landed on the chip.
 * Now returns 0 on success, 1 on the first failure - g_radio_
 * configured is only set once every call has actually succeeded, so a
 * failed attempt leaves it false and this function will legitimately
 * retry (rather than skip, believing itself already done) on its next
 * call from lora_spi_transmit()/sx1262_poll_rx(). Mirrors the return-
 * code-checking pattern lora_spi_transmit() below already uses for its
 * own critical calls. */
static int ensure_radio_configured(void)
{
    sx126x_mod_params_lora_t mod_params;
    sx126x_pkt_params_lora_t pkt_params;

    if (g_radio_configured)
    {
        return 0;
    }

    if (sx126x_hal_reset(NULL) != SX126X_HAL_STATUS_OK) return 1;
    if (sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC) != SX126X_STATUS_OK) return 1;
    if (sx126x_set_pkt_type(NULL, SX126X_PKT_TYPE_LORA) != SX126X_STATUS_OK) return 1;
    if (sx126x_set_rf_freq(NULL, LORA_RF_FREQ_HZ) != SX126X_STATUS_OK) return 1;
    if (sx126x_set_buffer_base_address(NULL, 0u, 0u) != SX126X_STATUS_OK) return 1;
    if (sx126x_set_dio_irq_params(NULL, 0x0203u /* TxDone|RxDone|Timeout */,
                                   0x0203u /* -> DIO1 */, 0x0000u, 0x0000u) != SX126X_STATUS_OK) return 1;

    /* LOCKED - see comment above. Values from core/lora_radio_params.h. */
    mod_params.sf = LORA_SPREADING_FACTOR;
    mod_params.bw = LORA_BANDWIDTH;
    mod_params.cr = LORA_CODING_RATE;
    mod_params.ldro = LORA_LDRO;
    if (sx126x_set_lora_mod_params(NULL, &mod_params) != SX126X_STATUS_OK) return 1;

    /* LOCKED - see comment above. pld_len_in_bytes gets overwritten
       per-send in lora_spi_transmit() below (explicit header mode
       means the receiver reads length from the header, but the driver
       still wants the field set to match what's actually being sent). */
    pkt_params.preamble_len_in_symb = LORA_PREAMBLE_LEN_SYMB;
    pkt_params.header_type = LORA_HEADER_MODE;
    pkt_params.pld_len_in_bytes = 0u;
    pkt_params.crc_is_on = LORA_CRC_IS_ON;
    pkt_params.invert_iq_is_on = LORA_INVERT_IQ_IS_ON;
    if (sx126x_set_lora_pkt_params(NULL, &pkt_params) != SX126X_STATUS_OK) return 1;

    /* Sync word and TX power/ramp - both LOCKED in core/lora_radio_params.h
       and, like mod/pkt params above, not previously called anywhere, so
       TX power was whatever the chip's reset default happened to be. Must
       run before g_radio_configured is set, same as mod/pkt params above. */
    if (sx126x_set_lora_sync_word(NULL, LORA_SYNC_WORD) != SX126X_STATUS_OK) return 1;
    if (sx126x_set_tx_params(NULL, LORA_TX_POWER_DBM, LORA_RAMP_TIME) != SX126X_STATUS_OK) return 1;

    g_radio_configured = 1;

    /* Listen by default from the moment radio bring-up finishes.
       Placed here (end of the one-time bring-up path), not as a
       separate call from sx1262_hal_init(), so it can't run before
       set_dio_irq_params()/the mod/pkt params just above it -
       sx1262_poll_rx() below also calls ensure_radio_configured() on
       every poll (cheap after the first call, guarded by
       g_radio_configured) specifically so RX starts even if this
       board never happens to transmit before its first poll.

       lora_spi_transmit() below switches the radio into TX (see
       apply_rf_switch_for_opcode()'s SET_TX handling above) and, per
       the datasheet, the chip auto-returns to standby RC on its own
       once that TX completes or times out - NOT back to RX. This
       function only re-arms RX once, here, at bring-up; getting the
       radio listening again after each TX is sx1262_poll_rx()'s job
       (it already sees TxDone/Timeout on the same DIO1 line RxDone
       uses, since all three share one dio_irq_params mask above - see
       that function's own doc on why it re-arms on those two bits and
       not on RxDone). */
    if (sx1262_enter_rx() != 0)
    {
        g_radio_configured = 0;
        return 1;
    }

    return 0;
}

int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len)
{
    sx126x_pkt_params_lora_t pkt_params;

    if (frame_len == 0u || frame_len > 255u)
    {
        return 1; /* can't represent this frame's length in one LoRa payload byte */
    }

    if (ensure_radio_configured() != 0)
    {
        return 1;
    }

    /* pld_len_in_bytes has to match this specific send - re-set it
       rather than assume ensure_radio_configured()'s one-time value
       still applies. Other fields are the same LOCKED values from
       core/lora_radio_params.h used in ensure_radio_configured() above. */
    pkt_params.preamble_len_in_symb = LORA_PREAMBLE_LEN_SYMB;
    pkt_params.header_type = LORA_HEADER_MODE;
    pkt_params.pld_len_in_bytes = (uint8_t)frame_len;
    pkt_params.crc_is_on = LORA_CRC_IS_ON;
    pkt_params.invert_iq_is_on = LORA_INVERT_IQ_IS_ON;
    if (sx126x_set_lora_pkt_params(NULL, &pkt_params) != SX126X_STATUS_OK)
    {
        return 1;
    }

    if (sx126x_write_buffer(NULL, 0u, frame_bytes, (uint8_t)frame_len) != SX126X_STATUS_OK)
    {
        return 1;
    }

    /* 1000ms timeout - generous; a real deadline can be tightened once
       real-world timing is measured on hardware. */
    if (sx126x_set_tx(NULL, 1000u) != SX126X_STATUS_OK)
    {
        return 1;
    }

    g_lora_tx_busy = 1;

    return 0;
}

/* comms_transport.c's lora_radio_tx_busy() extern - see that file's
   weak default (used before this HAL is linked) and g_lora_tx_busy's
   own doc above. */
int lora_radio_tx_busy(void)
{
    return g_lora_tx_busy;
}

/*
 * -----------------------------------------------------------------------
 * --- sx1262_poll_rx() - RX counterpart to lora_spi_transmit() above,
 *     called every main-loop tick alongside mission_link_task_step()
 *     (see board/STM32N6570-DK/main_board.c) ------------------------------
 * -----------------------------------------------------------------------
 *
 * ensure_radio_configured() is called first, same as lora_spi_transmit()
 * does - guarantees the radio (and therefore RX) has actually been
 * brought up even if this board's first main-loop tick happens before
 * its first transmit. Cheap after the first call (one int compare).
 *
 * Everything past the DIO1 read below must stay cheap: this runs every
 * tick, and DIO1 is low the overwhelming majority of the time (nothing
 * received or sent since the last poll) - the gpio_read() below is the
 * only work done in that common case.
 */
void sx1262_poll_rx(void)
{
    sx126x_irq_mask_t irq_status;
    sx126x_rx_buffer_status_t rx_buf_status;
    uint8_t frame_bytes[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_type;
    uint8_t out_sender_board_id;
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_payload_len;

    if (ensure_radio_configured() != 0)
    {
        /* g_radio_configured stays 0 on failure, so the next poll
           retries the whole bring-up sequence rather than proceeding
           against a possibly-unconfigured radio. */
        return;
    }

    if (gpio_read(SX1262_DIO1_PORT, SX1262_DIO1_PIN) == 0)
    {
        return; /* nothing asserted - the common case, must stay cheap */
    }

    if (sx126x_get_irq_status(NULL, &irq_status) != SX126X_STATUS_OK)
    {
        return; /* SPI error reading status - try again next poll */
    }

    /* Clear whatever's actually set, in every case below, so DIO1
       deasserts and this doesn't false-trigger next poll - see this
       function's own top-of-file doc requirement. Clearing exactly
       irq_status (not the full TxDone|RxDone|Timeout mask) only ever
       clears bits that were actually pending. */
    (void)sx126x_clear_irq_status(NULL, irq_status);

    if ((irq_status & SX126X_IRQ_RX_DONE) == 0u)
    {
        /* TxDone and/or Timeout, no RxDone. Per the datasheet (see
           ensure_radio_configured()'s doc above sx1262_enter_rx()'s
           call site), the chip auto-returns to standby RC once a TX
           completes or times out - re-arm RX so the radio goes back
           to listening instead of sitting deaf in standby until the
           next transmit. (RxDone can't reach this branch - handled
           below instead, and does NOT need this same re-arm: RX is
           configured continuous, see sx1262_enter_rx()'s own doc, so
           the chip is still listening on its own after a reception.)

           Given the configured IRQ mask (TxDone|RxDone|Timeout only -
           see ensure_radio_configured()), "not RxDone" here means
           whatever lora_spi_transmit() last keyed is no longer in
           flight either way - clears the busy flag comms_transport.c's
           dispatcher gates on. See g_lora_tx_busy's own doc above. */
        g_lora_tx_busy = 0;
        if (sx1262_enter_rx() != 0)
        {
            /* Force a full re-bring-up on the next poll rather than
               silently leaving the radio deaf until the next TX -
               reuses ensure_radio_configured()'s own retry gate. */
            g_radio_configured = 0;
        }
        return;
    }

    if (sx126x_get_rx_buffer_status(NULL, &rx_buf_status) != SX126X_STATUS_OK)
    {
        return;
    }

    if (rx_buf_status.pld_len_in_bytes > (uint8_t)sizeof(frame_bytes))
    {
        /* Can't happen for a frame shaped by this project's own
           envelope (LORA_FRAME_MAX_WIRE_SIZE already covers the
           worst case) - guard rather than overrun frame_bytes on a
           value that came off the wire. */
        return;
    }

    if (sx126x_read_buffer(NULL, rx_buf_status.buffer_start_pointer,
                            frame_bytes, rx_buf_status.pld_len_in_bytes) != SX126X_STATUS_OK)
    {
        return;
    }

    /* lora_frame_try_parse() hands back a complete, already-decrypted
       frame in one call - the radio delivered one whole packet, not a
       stream, so rc==0 ("need more bytes") is not a real outcome
       here; treated identically to rc==-1 below regardless. See
       comms_transport_deliver_received()'s own doc on why
       out_sender_board_id has no further use past authenticating this
       frame (COMMS_TRANSPORT_UNKNOWN_SENDER is what actually gets
       recorded) - same handling laptop/tools/two_board_harness.c's
       harness_rx_pump() already uses for the identical situation. */
    if (lora_frame_try_parse(frame_bytes, rx_buf_status.pld_len_in_bytes, &out_type,
                              &out_sender_board_id, out_payload, &out_payload_len) == 1)
    {
        (void)out_sender_board_id;
        (void)comms_transport_deliver_received(out_type, out_payload, out_payload_len);
    }
    /* rc == 0 or -1: drop - a bit-error-corrupted real-RF packet is an
       expected outcome (see lora_frame.h's own doc on this), not
       something to propagate as an error. */
}

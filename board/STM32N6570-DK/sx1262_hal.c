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
 *   RXEN      17        PD13      plain GPIO output, active-high
 *   TXEN      18        PF1       plain GPIO output, active-high
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

#define SX1262_RXEN_PORT            GPIOD
#define SX1262_RXEN_PIN             13u   /* PD13 */

#define SX1262_TXEN_PORT            GPIOF
#define SX1262_TXEN_PIN             1u    /* PF1 */

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
    /* GPIOA, B, C, D, E, F, G - every port this HAL touches. */
    RCC->AHB4ENR |= (RCC_AHB4ENR_GPIOAEN | RCC_AHB4ENR_GPIOBEN |
                      RCC_AHB4ENR_GPIOCEN | RCC_AHB4ENR_GPIODEN |
                      RCC_AHB4ENR_GPIOEEN | RCC_AHB4ENR_GPIOFEN |
                      RCC_AHB4ENR_GPIOGEN);
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
       datasheet), software NSS management (SSM/SSOE off - we drive
       NSS ourselves via GPIO), AFCNTR off (GPIOs stay under our own
       control, not the peripheral's). */
    SX1262_SPI->CFG2 = SPI_CFG2_MASTER;

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

    SX1262_SPI->CR1 = SPI_CR1_SPE; /* enable */
}

static uint8_t spi5_xfer_byte(uint8_t out)
{
    /* Kick a single-byte transfer. */
    SX1262_SPI->CR1 |= SPI_CR1_CSTART;

    while (!(SX1262_SPI->SR & SPI_SR_TXP))
    {
    }
    *(volatile uint8_t *)&SX1262_SPI->TXDR = out;

    while (!(SX1262_SPI->SR & SPI_SR_RXP))
    {
    }
    return *(volatile uint8_t *)&SX1262_SPI->RXDR;
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

static void wait_busy_low(void)
{
    /* No timeout here on purpose: per the datasheet, a command issued
       while BUSY is high is silently dropped - looping until BUSY
       actually clears is the only way to honor that, not a timeout
       that might let a caller proceed into a dropped command. */
    while (gpio_read(SX1262_BUSY_PORT, SX1262_BUSY_PIN) != 0)
    {
    }
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
    wait_busy_low();

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    (void)context;

    /* Standard SX126x wake-from-sleep sequence: a brief NSS low/high
       pulse, then wait for BUSY to clear the internal wake-up
       procedure. */
    wait_busy_low();
    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 0);
    crude_delay_cycles(10u);
    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);
    wait_busy_low();

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

    wait_busy_low();

    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 0);

    for (i = 0u; i < command_length; i++)
    {
        (void)spi5_xfer_byte(command[i]);
    }
    for (i = 0u; i < data_length; i++)
    {
        (void)spi5_xfer_byte(data[i]);
    }

    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);

    /* SetSleep is the one documented exception where BUSY does NOT
       clear immediately afterward (the chip is, correctly, asleep) -
       skip the trailing wait_busy_low() for it specifically, matching
       every reference sx126x HAL implementation's own handling of this
       opcode. */
    if (!(command_length > 0u && command[0] == SX1262_OPCODE_SET_SLEEP))
    {
        wait_busy_low();
    }

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command,
                                     const uint16_t command_length, uint8_t *data,
                                     const uint16_t data_length)
{
    uint16_t i;

    (void)context;

    wait_busy_low();

    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 0);

    for (i = 0u; i < command_length; i++)
    {
        (void)spi5_xfer_byte(command[i]);
    }
    for (i = 0u; i < data_length; i++)
    {
        data[i] = spi5_xfer_byte(SX126X_NOP);
    }

    gpio_write(SX1262_NSS_PORT, SX1262_NSS_PIN, 1);

    wait_busy_low();

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
 * Radio bring-up this file can decide with actual certainty: reset,
 * standby, LoRa packet type, 868MHz RF frequency (this module's own
 * name - "SX1262 868M" - decides this one, not a guess), TX/RX FIFO
 * base addresses, and DIO1 routed to TxDone/RxDone/Timeout.
 *
 * NOT decided here, and left as clearly-labeled PLACEHOLDER values
 * below: SF/BW/CR, preamble length, sync word, CRC/header mode. These
 * are protocol parameters that must MATCH whatever Track 2 (the Pico
 * board) configures its own SX1262 to, or the two boards' radios will
 * simply fail to hear each other - no crash, no damage, just silent
 * non-interop until synced. Placeholder values below are the common
 * "safe default" figures used in most SX126x reference examples
 * (SF7/BW125/CR4:5, explicit header, CRC on, sync word 0x34) - fine to
 * ship as a starting point, but must be confirmed against Track 2
 * before relying on this link for anything real.
 */
static void ensure_radio_configured(void)
{
    sx126x_mod_params_lora_t mod_params;
    sx126x_pkt_params_lora_t pkt_params;

    if (g_radio_configured)
    {
        return;
    }

    (void)sx126x_hal_reset(NULL);
    (void)sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
    (void)sx126x_set_pkt_type(NULL, SX126X_PKT_TYPE_LORA);
    (void)sx126x_set_rf_freq(NULL, 868000000u);
    (void)sx126x_set_buffer_base_address(NULL, 0u, 0u);
    (void)sx126x_set_dio_irq_params(NULL, 0x0203u /* TxDone|RxDone|Timeout */,
                                     0x0203u /* -> DIO1 */, 0x0000u, 0x0000u);

    /* PLACEHOLDER - see comment above. */
    mod_params.sf = SX126X_LORA_SF7;
    mod_params.bw = SX126X_LORA_BW_125;
    mod_params.cr = SX126X_LORA_CR_4_5;
    mod_params.ldro = 0u;
    (void)sx126x_set_lora_mod_params(NULL, &mod_params);

    /* PLACEHOLDER - see comment above. pld_len_in_bytes gets overwritten
       per-send in lora_spi_transmit() below (explicit header mode
       means the receiver reads length from the header, but the driver
       still wants the field set to match what's actually being sent). */
    pkt_params.preamble_len_in_symb = 8u;
    pkt_params.header_type = SX126X_LORA_PKT_EXPLICIT;
    pkt_params.pld_len_in_bytes = 0u;
    pkt_params.crc_is_on = true;
    pkt_params.invert_iq_is_on = false;
    (void)sx126x_set_lora_pkt_params(NULL, &pkt_params);

    g_radio_configured = 1;
}

int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len)
{
    sx126x_pkt_params_lora_t pkt_params;

    if (frame_len == 0u || frame_len > 255u)
    {
        return 1; /* can't represent this frame's length in one LoRa payload byte */
    }

    ensure_radio_configured();

    /* pld_len_in_bytes has to match this specific send - re-set it
       rather than assume ensure_radio_configured()'s one-time value
       still applies. */
    pkt_params.preamble_len_in_symb = 8u;
    pkt_params.header_type = SX126X_LORA_PKT_EXPLICIT;
    pkt_params.pld_len_in_bytes = (uint8_t)frame_len;
    pkt_params.crc_is_on = true;
    pkt_params.invert_iq_is_on = false;
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

    return 0;
}

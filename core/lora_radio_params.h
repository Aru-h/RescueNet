/*
 * lora_radio_params.h - LOCKED LoRa PHY parameters for the TRON two-board
 * link, expressed as compile-time macros (not runtime config).
 *
 * -----------------------------------------------------------------------
 * These values are LOCKED, not placeholders. Both boards' SX1262 HALs
 * (board/STM32N6570-DK/sx1262_hal.c and, in a later pass,
 * board/Raspberry_Pi_Pico/sx1262_hal.c) #include this header and use
 * these macros directly in their sx126x_mod_params_lora_t /
 * sx126x_pkt_params_lora_t structs, instead of each board hardcoding
 * its own literals.
 *
 * WHY THIS MATTERS: SF/BW/CR, preamble length, header mode, CRC, and
 * sync word are radio-layer protocol parameters. If the two boards'
 * SX1262s don't agree on all of them, the radios simply fail to hear
 * each other - no crash, no error, just silent non-interop. Centralizing
 * them here in one header both boards include removes that coordination
 * problem: there is exactly one place to change these values, and
 * changing it changes both boards at once (after each board's next
 * rebuild/reflash).
 *
 * Changing ANY value below requires updating both boards together -
 * that's not a caveat, it's the entire reason this header exists instead
 * of two independent hardcoded copies. Do not let one board's HAL drift
 * from these macros; if a board needs a different value, that's a
 * conversation about updating this file, not about overriding it locally.
 *
 * Scope is deliberately narrow (YAGNI): this is a fixed 2-board contest
 * link, so there is no FSK support here and no runtime-configurable TX
 * power. If a future need for either shows up, add it then.
 * -----------------------------------------------------------------------
 */

#ifndef TRON_CORE_LORA_RADIO_PARAMS_H
#define TRON_CORE_LORA_RADIO_PARAMS_H

#include "sx126x.h"

/* RF center frequency, in Hz. */
#define LORA_RF_FREQ_HZ            868000000u

/* Modulation parameters (sx126x_mod_params_lora_t). */
#define LORA_SPREADING_FACTOR      SX126X_LORA_SF7
#define LORA_BANDWIDTH             SX126X_LORA_BW_125
#define LORA_CODING_RATE           SX126X_LORA_CR_4_5
#define LORA_LDRO                  0u  /* low data-rate optimization off */

/* Packet parameters (sx126x_pkt_params_lora_t). */
#define LORA_PREAMBLE_LEN_SYMB     8u
#define LORA_HEADER_MODE           SX126X_LORA_PKT_EXPLICIT
#define LORA_CRC_IS_ON             true
#define LORA_INVERT_IQ_IS_ON       false

/* Sync word, passed to sx126x_set_lora_sync_word(). */
#define LORA_SYNC_WORD             0x34u

/* TX power/ramp, passed to sx126x_set_tx_params(). */
#define LORA_TX_POWER_DBM          14
#define LORA_RAMP_TIME             SX126X_RAMP_200_US

#endif /* TRON_CORE_LORA_RADIO_PARAMS_H */

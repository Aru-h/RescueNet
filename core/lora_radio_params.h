/*
 * lora_radio_params.h - LOCKED LoRa PHY parameters for the TRON two-board
 * link, expressed as compile-time macros (not runtime config).
 *
 * -----------------------------------------------------------------------
 * These values are LOCKED, not placeholders. Both boards' SX1262 HALs
 * (board/STM32N6570-DK/sx1262_hal.c and board/Raspberry_Pi_Pico/sx1262_hal.c)
 * #include this header and use these macros directly in their
 * sx126x_mod_params_lora_t / sx126x_pkt_params_lora_t structs, instead of
 * each board hardcoding its own literals.
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
 *
 * -----------------------------------------------------------------------
 * REGION SELECTION (added - region-config restructure)
 * -----------------------------------------------------------------------
 * Originally this file hardcoded a single frequency, 868000000u, labeled
 * as the RF center frequency with no region tag. That number is EU868's
 * center-of-band value, NOT anything inside India's IN865-867 MHz ISM
 * band - a grep of this repo for "India"/"IN865" ahead of this change
 * turned up no prior doc actually committing this project to India's
 * band; the 868 MHz figure looks like an EU868 default carried over
 * without a region decision ever being made. That mismatch is corrected
 * below by making 865.0625 MHz (India's IN865 first default channel,
 * per the LoRaWAN Regional Parameters IN865-867 band plan) the new
 * default, not by preserving 868 MHz under a new name.
 *
 * Two region profiles are provided, chosen at COMPILE TIME (not
 * runtime) via a Makefile-supplied macro:
 *
 *   (default, no flag needed)   IN865  - India, 865-867 MHz ISM band.
 *   -DLORA_REGION_JP920         JP920  - Japan, 920 MHz band (ARIB
 *                                         STD-T108), NOT the wider AS923
 *                                         range some vendors lump Japan
 *                                         into.
 *
 * WHY COMPILE-TIME #if, NOT A RUNTIME LoraRegionProfile_t STRUCT: this
 * mirrors core/recovery_manager_config.h's convention (plain #define
 * constants selected/overridden at compile time, e.g. its
 * RECOVERY_MANAGER_* tuning macros), not core/comms_transport.h's
 * runtime board-count pattern. The board-count case is runtime because
 * the SAME board binary needs to answer "how many teams share this
 * board" against a value that can differ per deployment without a
 * rebuild. Region is different: it is a regulatory/certification
 * property of a given radio's physical deployment (which country's
 * ISM band it is legally allowed to transmit in), decided at flash time
 * per this project's existing "-DTHIS_BOARD_ID=0/1 per board recipe"
 * convention (see this Makefile's check-board / check-pico rules), not
 * something a running board should be able to switch between - a board
 * flashed for JP920 has no legitimate reason to ever answer "what if I
 * were IN865" at runtime, and a struct/enum indirection here would only
 * add a footgun (defaulting to the wrong region if a caller forgets to
 * set it) with no corresponding benefit. It also means zero changes are
 * needed in either board's sx1262_hal.c: LORA_RF_FREQ_HZ and friends
 * keep the exact same macro names, so both HALs keep working unmodified
 * - only the value each macro expands to depends on which region was
 * selected at compile time.
 *
 * SF/BW/CR ARE NOT REGION-SPECIFIC HERE: spreading factor, bandwidth,
 * and coding rate are modulation-layer choices orthogonal to which ISM
 * band's frequencies are legal to use - IN865 and JP920 both permit
 * SF7/BW125/CR4:5 (neither region's rules force a different SF/BW/CR
 * the way, say, US915's fixed-DR channel plan can). They are therefore
 * left as shared, non-#if-gated macros below. If a future region
 * actually required a different SF/BW/CR (some sub-bands of some
 * regions do constrain minimum bandwidth or symbol rate), that would
 * need its own #if arm added at that point - not preemptively guessed
 * here.
 * -----------------------------------------------------------------------
 */

#ifndef TRON_CORE_LORA_RADIO_PARAMS_H
#define TRON_CORE_LORA_RADIO_PARAMS_H

#include "sx126x.h"

#if defined(LORA_REGION_JP920)

/* ---------------------------------------------------------------
 * JP920 - Japan, 920 MHz band, ARIB STD-T108.
 * ---------------------------------------------------------------
 * ARIB STD-T108 governs Japan's 920.5-928.0 MHz specified low-power
 * radio band. The sub-band commonly used by LoRa-type wide-area
 * systems sits around 923.2-928.0 MHz with ~200 kHz channel spacing,
 * often enumerated as a 15-channel plan (vendor literature commonly
 * labels these something like CH24-CH38, though numbering varies by
 * source).
 *
 * CONFIDENCE FLAG: the exact channel count/numbering/upper-edge
 * placement above is recalled from general ARIB STD-T108 / Japan
 * 920 MHz LoRa deployment knowledge, NOT verified against the current
 * ARIB STD-T108 spec text or a certification body's channel table in
 * this session. Do not treat LORA_JP920_CHANNEL_PLAN_HZ below as
 * certification-ready; re-derive/verify it against the authoritative
 * ARIB STD-T108 document (or an ARIB-certified module vendor's
 * published channel table for the exact sub-band/class this project
 * will certify under) before this profile is used for anything beyond
 * bring-up/bench testing. LORA_RF_FREQ_HZ below is set to a frequency
 * near the middle of that range (923.6 MHz) precisely because it is
 * unlikely to be wrong regardless of exactly how the 15 channels end
 * up numbered - a middle-of-band single default channel, not a
 * claim about which numbered ARIB channel it corresponds to.
 *
 * This project currently only ever transmits on ONE fixed channel
 * (no frequency-hopping/channel-agility logic exists in core/lora_frame.c
 * or either board's HAL) - same fixed-2-board-link YAGNI scope this
 * header's top comment already states for TX power/FSK. The 15-entry
 * table below is documented for a future channel-agility pass, but
 * LORA_RF_FREQ_HZ is what actually gets programmed into the radio
 * today.
 */
#define LORA_RF_FREQ_HZ            923600000u /* ~923.6 MHz, middle of the
                                                  923.2-928.0 MHz sub-band */

/* Approximate 15-channel, 200 kHz-spaced plan for the 923.2-926.0 MHz
   span, for a future channel-agility implementation. See the
   CONFIDENCE FLAG note above - verify before relying on this list for
   certification. NOT consulted by any code today (single fixed
   channel only); documented here as the region's channel-plan
   reference point. */
#define LORA_JP920_CHANNEL_COUNT   15u
#define LORA_JP920_CHANNEL_SPACING_HZ 200000u
#define LORA_JP920_CHANNEL_BASE_HZ 923200000u /* channel 0 of the table */
/* channel N (0..14) center frequency =
   LORA_JP920_CHANNEL_BASE_HZ + N * LORA_JP920_CHANNEL_SPACING_HZ */

/* ARIB STD-T108 mandates Listen-Before-Talk (channel occupancy sensing
   before transmit) for this band - see this project's LBT gap finding
   (reported separately, not implemented here: core/lora_frame.c has no
   carrier-sense/CCA step before TX). LORA_JP920_REQUIRES_LBT exists so
   any future code that DOES add CCA can gate on region rather than
   re-deriving "does this region need LBT" from scratch, and so this
   fact is discoverable from the header instead of only from a doc. */
#define LORA_JP920_REQUIRES_LBT    1

#else /* default: IN865 */

/* ---------------------------------------------------------------
 * IN865 - India, 865-867 MHz ISM band (WPC de-licensed band).
 * ---------------------------------------------------------------
 * 865.0625 MHz is IN865's first default/mandatory channel under the
 * LoRaWAN Regional Parameters IN865-867 band plan (the other two
 * commonly-cited defaults are 865.4025 MHz and 865.9850 MHz). Chosen
 * as this project's IN865 default because it is the closest
 * standards-anchored analog to the previous unlabeled 868000000u
 * value - not because 868 MHz "becomes" this value, but because this
 * is the correct in-band replacement for what looks like a carried-
 * over EU868 default.
 *
 * India's current point-to-point deployment for this link does not
 * require Listen-Before-Talk the way Japan's ARIB STD-T108 does (see
 * this project's separate LBT gap-finding note) - WPC's de-licensing
 * conditions for this band are duty-cycle/power-limited, not
 * LBT/CCA-gated, for this class of use. LORA_IN865_REQUIRES_LBT is
 * defined as 0 to make that contrast explicit and greppable next to
 * LORA_JP920_REQUIRES_LBT above.
 */
#define LORA_RF_FREQ_HZ            865062500u /* IN865 default channel 1 */

#define LORA_IN865_CHANNEL_COUNT   3u
#define LORA_IN865_CHANNEL_0_HZ    865062500u
#define LORA_IN865_CHANNEL_1_HZ    865402500u
#define LORA_IN865_CHANNEL_2_HZ    865985000u

#define LORA_IN865_REQUIRES_LBT    0

#endif /* LORA_REGION_JP920 */

/* Modulation parameters (sx126x_mod_params_lora_t). Orthogonal to
   region selection above - see this file's top "REGION SELECTION"
   comment for why SF/BW/CR are not #if-gated. */
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

/* TX power/ramp, passed to sx126x_set_tx_params(). Left the same
   across both regions deliberately: 14 dBm is comfortably under both
   IN865's and JP920's respective power limits for this frame size/duty
   cycle, so there is no regulatory reason to make it region-specific
   (unlike frequency, where using EU868's value in Japan or outside
   India's band would be an actual violation, using 14 dBm in either
   region is not) - do not read this as "power limits are identical
   between the two regions", just that 14 dBm clears both. */
#define LORA_TX_POWER_DBM          14
#define LORA_RAMP_TIME             SX126X_RAMP_200_US

#endif /* TRON_CORE_LORA_RADIO_PARAMS_H */

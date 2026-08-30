#include "lora_crypto.h"
#include <string.h> /* memset - see core/chacha20.c's note on this being
                        an already-established freestanding-safe
                        dependency in core/ */

/*
 * Default contest-build PSK - see this file's header for why a
 * static key is the deliberate, documented choice here, and for how
 * to override it (-DLORA_CRYPTO_PSK_INIT=...) before any real
 * deployment. This default is NOT secret (it ships in source
 * control) - treat every LoRa frame as observable/forgeable by
 * anyone with this repo until a real per-build secret replaces it.
 */
#ifndef LORA_CRYPTO_PSK_INIT
#define LORA_CRYPTO_PSK_INIT \
    { 0x54u, 0x52u, 0x4fu, 0x4eu, 0x2du, 0x32u, 0x30u, 0x32u, \
      0x36u, 0x2du, 0x43, 0x42u, 0x42u, 0x41u, 0x2du, 0x64u, \
      0x65u, 0x6du, 0x6fu, 0x2du, 0x62u, 0x75u, 0x69u, 0x6cu, \
      0x64u, 0x2du, 0x70u, 0x73u, 0x6bu, 0x21u, 0x21u, 0x21u }
#endif

const uint8_t g_lora_crypto_psk[LORA_CRYPTO_KEY_BYTES] = LORA_CRYPTO_PSK_INIT;

/* This board's own outgoing counter - see header doc: one counter,
   shared across every frame_type/destination this board sends to. */
static uint32_t g_tx_counter = 0u;

/* Per-sender RX state, indexed by sender_board_id (< LORA_CRYPTO_MAX_SENDERS).
   g_rx_seen[i] is 0 until the first accepted frame from sender i;
   g_rx_last_counter[i] is only meaningful once g_rx_seen[i] is set. */
static uint32_t g_rx_last_counter[LORA_CRYPTO_MAX_SENDERS];
static uint8_t g_rx_seen[LORA_CRYPTO_MAX_SENDERS];

uint32_t lora_crypto_next_tx_counter(void)
{
    uint32_t value = g_tx_counter;
    g_tx_counter++;
    return value;
}

void lora_crypto_reset(void)
{
    g_tx_counter = 0u;
    memset(g_rx_last_counter, 0, sizeof(g_rx_last_counter));
    memset(g_rx_seen, 0, sizeof(g_rx_seen));
}

void lora_crypto_build_nonce(uint8_t sender_board_id, uint32_t counter,
                              uint8_t out_nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES])
{
    uint32_t i;

    out_nonce[0] = sender_board_id;

    for (i = 1u; i < 8u; i++)
    {
        out_nonce[i] = 0u;
    }

    out_nonce[8]  = (uint8_t)(counter & 0xffu);
    out_nonce[9]  = (uint8_t)((counter >> 8) & 0xffu);
    out_nonce[10] = (uint8_t)((counter >> 16) & 0xffu);
    out_nonce[11] = (uint8_t)((counter >> 24) & 0xffu);
}

uint32_t lora_crypto_reconstruct_counter(uint8_t sender_board_id, uint16_t wire_frag)
{
    uint32_t last_accepted;
    uint32_t candidate;
    uint64_t bumped; /* uint64_t so the +0x10000 bump below can never
                         silently wrap a uint32_t and produce a
                         SMALLER candidate than last_accepted - see
                         the overflow check below. */

    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS)
    {
        return (uint32_t)wire_frag;
    }

    if (!g_rx_seen[sender_board_id])
    {
        /* Bootstrap: no prior accepted counter for this sender -
           any starting point is fine, see this function's own
           header doc for why. */
        return (uint32_t)wire_frag;
    }

    last_accepted = g_rx_last_counter[sender_board_id];

    /* Candidate in the same 16-bit "window" as last_accepted. */
    candidate = (last_accepted & 0xffff0000u) | (uint32_t)wire_frag;

    if (candidate <= last_accepted)
    {
        /* wire_frag's window has already been fully consumed - the
           real counter must be in the NEXT window up (mirrors
           LoRaWAN's own frame-counter reconstruction). */
        bumped = (uint64_t)candidate + 0x10000u;

        if (bumped > 0xffffffffu)
        {
            /* This sender's 32-bit counter has wrapped entirely -
               astronomically unlikely for a contest build (would
               require ~4 billion frames from one sender) but handled
               rather than silently truncated: fall back to the
               un-bumped candidate. The AEAD tag verify downstream is
               still the real gate on whether this reconstruction was
               actually correct - a wrong guess here just fails to
               decrypt, it never fails OPEN (see this file's header
               doc on lora_crypto_accept_counter() for the same
               principle applied to the accept side). */
            return candidate;
        }

        candidate = (uint32_t)bumped;
    }

    return candidate;
}

void lora_crypto_accept_counter(uint8_t sender_board_id, uint32_t counter)
{
    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS)
    {
        return;
    }

    g_rx_last_counter[sender_board_id] = counter;
    g_rx_seen[sender_board_id] = 1u;
}

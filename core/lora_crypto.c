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

/* Session epoch state - see lora_crypto.h's top comment and "Session
   epochs + session control" section. */
static uint32_t g_tx_epoch;
static uint8_t g_hello_due;
static uint8_t g_local_board_id;
static uint8_t g_local_board_id_known;
static uint32_t g_challenge_seq;

static uint32_t g_rx_epoch[LORA_CRYPTO_MAX_SENDERS];
static uint32_t g_rx_session_count[LORA_CRYPTO_MAX_SENDERS];
static uint8_t g_challenge_pending[LORA_CRYPTO_MAX_SENDERS];
static uint32_t g_challenge_epoch[LORA_CRYPTO_MAX_SENDERS];
static uint8_t g_challenge_due[LORA_CRYPTO_MAX_SENDERS];
static uint8_t g_challenge_nonce[LORA_CRYPTO_MAX_SENDERS][LORA_CRYPTO_SESSION_NONCE_BYTES];
static uint8_t g_response_due[LORA_CRYPTO_MAX_SENDERS];
static uint8_t g_response_nonce[LORA_CRYPTO_MAX_SENDERS][LORA_CRYPTO_SESSION_NONCE_BYTES];

/* Non-atomic read-then-increment on g_tx_counter, no lock. Safe today for
 * the same reason documented in comms_transport_inbox_push()'s
 * concurrency-invariant comment: both board targets create exactly one
 * simulation task, and every caller of this function (lora_frame_pack(),
 * via comms_transport_send()/comms_transport_dispatch_pending()) runs
 * from that same single task - never from an ISR, never from a second
 * concurrent task. A repeated (key, nonce) pair from a torn read-modify-
 * write here would be a catastrophic AEAD break, so re-verify this
 * invariant before ever calling this from a second task or an ISR. */
uint32_t lora_crypto_next_tx_counter(void)
{
    uint32_t value = g_tx_counter;

    /* Saturate rather than wrap: wrapping back to 0 within the same
       epoch would repeat a (key, nonce) pair. UINT32_MAX itself is
       never handed out for a real frame - lora_frame_pack() checks
       lora_crypto_tx_counter_exhausted() first and refuses to send. */
    if (g_tx_counter != UINT32_MAX)
    {
        g_tx_counter++;
    }
    return value;
}

uint8_t lora_crypto_tx_counter_exhausted(void)
{
    return (uint8_t)(g_tx_counter == UINT32_MAX);
}

void lora_crypto_reset(void)
{
    g_tx_counter = 0u;
    memset(g_rx_last_counter, 0, sizeof(g_rx_last_counter));
    memset(g_rx_seen, 0, sizeof(g_rx_seen));

    g_tx_epoch = 0u;
    g_hello_due = 0u;
    g_challenge_seq = 0u;
    memset(g_rx_epoch, 0, sizeof(g_rx_epoch));
    memset(g_rx_session_count, 0, sizeof(g_rx_session_count));
    memset(g_challenge_pending, 0, sizeof(g_challenge_pending));
    memset(g_challenge_epoch, 0, sizeof(g_challenge_epoch));
    memset(g_challenge_due, 0, sizeof(g_challenge_due));
    memset(g_challenge_nonce, 0, sizeof(g_challenge_nonce));
    memset(g_response_due, 0, sizeof(g_response_due));
    memset(g_response_nonce, 0, sizeof(g_response_nonce));
}

void lora_crypto_set_tx_epoch(uint32_t epoch)
{
    g_tx_epoch = epoch;
    g_hello_due = 1u;
}

uint32_t lora_crypto_tx_epoch(void)
{
    return g_tx_epoch;
}

void lora_crypto_set_local_board_id(uint8_t board_id)
{
    g_local_board_id = board_id;
    g_local_board_id_known = 1u;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

void lora_crypto_build_nonce(uint8_t sender_board_id, uint32_t epoch, uint32_t counter,
                              uint8_t out_nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES])
{
    uint32_t i;

    out_nonce[0] = sender_board_id;
    put_le32(&out_nonce[1], epoch);

    for (i = 5u; i < 8u; i++)
    {
        out_nonce[i] = 0u;
    }

    out_nonce[8]  = (uint8_t)(counter & 0xffu);
    out_nonce[9]  = (uint8_t)((counter >> 8) & 0xffu);
    out_nonce[10] = (uint8_t)((counter >> 16) & 0xffu);
    out_nonce[11] = (uint8_t)((counter >> 24) & 0xffu);
}

int lora_crypto_counter_is_replay(uint8_t sender_board_id, uint32_t counter)
{
    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS || !g_rx_seen[sender_board_id])
    {
        return 0;
    }

    return (counter <= g_rx_last_counter[sender_board_id]) ? 1 : 0;
}

void lora_crypto_accept_counter(uint8_t sender_board_id, uint32_t counter)
{
    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS)
    {
        return;
    }

    /* Never lowers the floor within a session: a lower counter here
     * would reopen the replay window for every counter between the new
     * floor and the old one. lora_frame_try_parse() already rejects
     * counters at or below the floor before this is reached, so this is
     * defense in depth, not a path normal traffic takes. A new session
     * legitimately restarts the floor - that goes through
     * lora_crypto_confirm_session() instead, never through here. */
    if (g_rx_seen[sender_board_id] && counter <= g_rx_last_counter[sender_board_id])
    {
        return;
    }

    g_rx_last_counter[sender_board_id] = counter;
    g_rx_seen[sender_board_id] = 1u;
}

/*
 * ---------------------------------------------------------------
 * Session epochs + session control - see lora_crypto.h.
 * ---------------------------------------------------------------
 */

int lora_crypto_rx_classify(uint8_t sender_board_id, uint32_t epoch)
{
    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS)
    {
        return LORA_CRYPTO_RX_TRUSTED; /* untracked: no floor, no session
                                          state - counter_is_replay()/
                                          accept_counter() ignore it */
    }

    if (!g_rx_seen[sender_board_id])
    {
        return LORA_CRYPTO_RX_BOOTSTRAP;
    }

    return (epoch == g_rx_epoch[sender_board_id]) ? LORA_CRYPTO_RX_TRUSTED
                                                  : LORA_CRYPTO_RX_UNCONFIRMED;
}

void lora_crypto_confirm_session(uint8_t sender_board_id, uint32_t epoch, uint32_t counter)
{
    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS)
    {
        return;
    }

    g_rx_epoch[sender_board_id] = epoch;
    g_rx_last_counter[sender_board_id] = counter;
    g_rx_seen[sender_board_id] = 1u;
    g_rx_session_count[sender_board_id]++;
    g_challenge_pending[sender_board_id] = 0u;
    g_challenge_due[sender_board_id] = 0u;
}

void lora_crypto_note_unconfirmed_session(uint8_t sender_board_id, uint32_t epoch)
{
    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS)
    {
        return;
    }

    /* One outstanding challenge per sender, BOUND to the epoch it was
       issued for. Evidence of any other epoch replaces it with a fresh
       nonce - never re-issues an old one. Found by review: with a nonce
       shared across epochs, a RESPONSE recorded from session E2 (and kept
       from reaching this board) could later be replayed to confirm E2
       after that sender had already moved on to E3, delivering replayed
       E2 frames. Now that recorded RESPONSE names a nonce that is no
       longer outstanding. Cost: an attacker who keeps replaying frames
       of an old epoch can keep replacing the live one's challenge - an
       active DoS, no worse than jamming, never a wrong session. */
    if (!g_challenge_pending[sender_board_id] ||
        g_challenge_epoch[sender_board_id] != epoch)
    {
        /* Unique per challenge for as long as epochs differ across boots
           - see lora_crypto.h's "Session control" doc. */
        put_le32(&g_challenge_nonce[sender_board_id][0], g_tx_epoch);
        put_le32(&g_challenge_nonce[sender_board_id][4], g_challenge_seq);
        g_challenge_seq++;
        g_challenge_epoch[sender_board_id] = epoch;
        g_challenge_pending[sender_board_id] = 1u;
    }

    /* Re-sent once per held frame, so a lost CHALLENGE (or RESPONSE) is
       retried at the rate the rebooted peer keeps transmitting - bounded
       by that peer's own traffic, never self-sustaining. */
    g_challenge_due[sender_board_id] = 1u;
}

int lora_crypto_session_rx(uint8_t sender_board_id, uint32_t epoch, uint32_t counter,
                           int rx_class, const uint8_t *payload, uint8_t payload_len)
{
    int trusted = (rx_class != LORA_CRYPTO_RX_UNCONFIRMED) ? 1 : 0;
    uint8_t subtype;
    uint8_t target;
    const uint8_t *nonce;

    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS ||
        payload_len < LORA_CRYPTO_SESSION_PAYLOAD_SIZE)
    {
        if (!trusted)
        {
            lora_crypto_note_unconfirmed_session(sender_board_id, epoch);
        }
        return trusted;
    }

    subtype = payload[0];
    target = payload[1];
    nonce = &payload[2];

    if (subtype != LORA_CRYPTO_SESSION_HELLO &&
        target != LORA_CRYPTO_SESSION_TARGET_ALL &&
        g_local_board_id_known && target != g_local_board_id)
    {
        /* Addressed to a different board. Still authenticated evidence
           of that sender's session, so an unconfirmed one gets
           challenged like any other held frame. */
        if (!trusted)
        {
            lora_crypto_note_unconfirmed_session(sender_board_id, epoch);
        }
        return trusted;
    }

    if (subtype == LORA_CRYPTO_SESSION_CHALLENGE)
    {
        /* Answering reveals nothing (the response only echoes a nonce
           under this board's own current session), so challenges are
           answered even from an unconfirmed session - otherwise two
           boards that each hold the other's new session would wait on
           each other forever. One pending response per sender: a flood
           of replayed challenges costs at most one response per
           dispatch. */
        memcpy(g_response_nonce[sender_board_id], nonce, LORA_CRYPTO_SESSION_NONCE_BYTES);
        g_response_due[sender_board_id] = 1u;

        if (!trusted)
        {
            lora_crypto_note_unconfirmed_session(sender_board_id, epoch);
        }
        return 1;
    }

    if (subtype == LORA_CRYPTO_SESSION_RESPONSE &&
        g_challenge_pending[sender_board_id] &&
        (trusted || g_challenge_epoch[sender_board_id] == epoch) &&
        memcmp(nonce, g_challenge_nonce[sender_board_id], LORA_CRYPTO_SESSION_NONCE_BYTES) == 0)
    {
        if (trusted)
        {
            /* The already-confirmed session answered - whatever held
               frame triggered the challenge was not from a live new
               session (e.g. a replay). Nothing to switch. */
            g_challenge_pending[sender_board_id] = 0u;
            g_challenge_due[sender_board_id] = 0u;
        }
        else
        {
            lora_crypto_confirm_session(sender_board_id, epoch, counter);
        }
        return 1;
    }

    /* HELLO, a stale/unsolicited RESPONSE, or an unknown subtype. */
    if (!trusted)
    {
        lora_crypto_note_unconfirmed_session(sender_board_id, epoch);
    }
    return trusted;
}

int lora_crypto_take_session_tx(uint8_t *out_payload, uint8_t *out_payload_len)
{
    uint32_t i; /* not uint8_t: `i < LORA_CRYPTO_MAX_SENDERS` never ends
                   if that cap is overridden to 256 (found by review) */

    for (i = 0u; i < LORA_CRYPTO_MAX_SENDERS; i++)
    {
        if (g_response_due[i])
        {
            out_payload[0] = LORA_CRYPTO_SESSION_RESPONSE;
            out_payload[1] = (uint8_t)i;
            memcpy(&out_payload[2], g_response_nonce[i], LORA_CRYPTO_SESSION_NONCE_BYTES);
            *out_payload_len = (uint8_t)LORA_CRYPTO_SESSION_PAYLOAD_SIZE;
            g_response_due[i] = 0u;
            return 1;
        }
    }

    for (i = 0u; i < LORA_CRYPTO_MAX_SENDERS; i++)
    {
        if (g_challenge_due[i] && g_challenge_pending[i])
        {
            out_payload[0] = LORA_CRYPTO_SESSION_CHALLENGE;
            out_payload[1] = (uint8_t)i;
            memcpy(&out_payload[2], g_challenge_nonce[i], LORA_CRYPTO_SESSION_NONCE_BYTES);
            *out_payload_len = (uint8_t)LORA_CRYPTO_SESSION_PAYLOAD_SIZE;
            g_challenge_due[i] = 0u;
            return 1;
        }
    }

    if (g_hello_due)
    {
        out_payload[0] = LORA_CRYPTO_SESSION_HELLO;
        out_payload[1] = LORA_CRYPTO_SESSION_TARGET_ALL;
        memset(&out_payload[2], 0, LORA_CRYPTO_SESSION_NONCE_BYTES);
        *out_payload_len = (uint8_t)LORA_CRYPTO_SESSION_PAYLOAD_SIZE;
        g_hello_due = 0u;
        return 1;
    }

    return 0;
}

uint32_t lora_crypto_peer_session_count(uint8_t sender_board_id)
{
    if (sender_board_id >= LORA_CRYPTO_MAX_SENDERS)
    {
        return 0u;
    }

    return g_rx_session_count[sender_board_id];
}

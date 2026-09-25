#include "lora_frame.h"
#include "aead_chacha20poly1305.h"

/* See lora_frame_auth_fail_count()/lora_frame_accept_count() in
   lora_frame.h. Plain counters, not reset by lora_crypto_reset(): they
   describe what this boot has seen, not crypto state. */
static uint32_t g_lora_frame_auth_fail_count;
static uint32_t g_lora_frame_accept_count;
static uint32_t g_lora_frame_session_hold_count;
static uint32_t g_lora_frame_replay_count;

uint32_t lora_frame_replay_count(void)
{
    return g_lora_frame_replay_count;
}

uint32_t lora_frame_session_hold_count(void)
{
    return g_lora_frame_session_hold_count;
}

uint32_t lora_frame_auth_fail_count(void)
{
    return g_lora_frame_auth_fail_count;
}

uint32_t lora_frame_accept_count(void)
{
    return g_lora_frame_accept_count;
}

/*
 * Additive checksum over `len` bytes at `bytes`, mod 256 - same scheme
 * as TELEMETRY_FRAME_CHECKSUM() in telemetry_frame.h, just expressed
 * as a function here (this file, unlike that header, is allowed to
 * hold logic) instead of a fixed-width macro, since payload length
 * varies per frame here instead of being fixed at 10 bytes. Computed
 * over CIPHERTEXT bytes only - see lora_frame.h's "Encryption"
 * section for why this is a pre-filter ahead of the real (Poly1305)
 * integrity check, not a replacement for one.
 */
static uint8_t lora_frame_checksum(const uint8_t *bytes, uint8_t len)
{
    uint8_t sum = 0u;
    uint8_t i;

    for (i = 0u; i < len; i++)
    {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum;
}

uint32_t lora_frame_pack(uint8_t frame_type, uint8_t sender_board_id,
                          const uint8_t *payload, uint8_t payload_len,
                          uint8_t *out_buf)
{
    uint32_t counter;
    uint8_t nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES];
    uint8_t *ciphertext;
    uint8_t *tag_out;
    uint8_t *counter_frag_out;
    uint8_t *checksum_out;
    uint32_t epoch;

    if (payload_len > LORA_FRAME_MAX_PAYLOAD)
    {
        /* Caller error: more payload than this envelope can carry.
           Write nothing and report zero bytes written rather than
           silently truncating the caller's data. Deliberately does
           NOT call lora_crypto_next_tx_counter() first - nothing is
           being sent, so nothing should consume a counter value. */
        return 0u;
    }

    if (lora_crypto_tx_counter_exhausted())
    {
        /* TX counter saturated: sealing another frame would reuse a
           nonce. Refuse to send, without consuming a counter value. */
        return 0u;
    }

    out_buf[0] = frame_type;
    out_buf[1] = payload_len;
    out_buf[2] = sender_board_id;
    epoch = lora_crypto_tx_epoch();
    out_buf[3] = (uint8_t)(epoch & 0xffu);
    out_buf[4] = (uint8_t)((epoch >> 8) & 0xffu);
    out_buf[5] = (uint8_t)((epoch >> 16) & 0xffu);
    out_buf[6] = (uint8_t)((epoch >> 24) & 0xffu);

    ciphertext = &out_buf[LORA_FRAME_HEADER_SIZE];
    tag_out = &ciphertext[payload_len];
    counter_frag_out = &tag_out[LORA_FRAME_TAG_SIZE];
    checksum_out = &counter_frag_out[LORA_FRAME_COUNTER_FRAG_SIZE];

    counter = lora_crypto_next_tx_counter();
    lora_crypto_build_nonce(sender_board_id, epoch, counter, nonce);

    /* AAD = the clear header bytes just written above - see
       lora_frame.h's "Encryption" section for why these three fields
       specifically are authenticated but not encrypted. Cannot fail
       here (payload_len already checked against LORA_FRAME_MAX_PAYLOAD,
       which lora_frame.h's own _Static_assert ties to
       AEAD_CHACHA20POLY1305_MAX_DATA_BYTES). */
    (void)aead_chacha20poly1305_seal(g_lora_crypto_psk, nonce,
                                      out_buf, LORA_FRAME_HEADER_SIZE,
                                      payload, payload_len,
                                      ciphertext, tag_out);

    counter_frag_out[0] = (uint8_t)(counter & 0xffu);
    counter_frag_out[1] = (uint8_t)((counter >> 8) & 0xffu);
    counter_frag_out[2] = (uint8_t)((counter >> 16) & 0xffu);
    counter_frag_out[3] = (uint8_t)((counter >> 24) & 0xffu);

    *checksum_out = lora_frame_checksum(ciphertext, payload_len);

    return LORA_FRAME_HEADER_SIZE + (uint32_t)payload_len + LORA_FRAME_TRAILER_SIZE;
}

int lora_frame_try_parse(const uint8_t *buf, uint32_t buf_len,
                          uint8_t *out_type, uint8_t *out_sender_board_id,
                          uint8_t *out_payload, uint8_t *out_payload_len)
{
    uint8_t payload_len;
    uint32_t frame_size;
    uint8_t sender_board_id;
    const uint8_t *ciphertext;
    const uint8_t *tag;
    uint8_t expected_checksum;
    uint32_t counter;
    uint32_t epoch;
    int rx_class;
    uint8_t nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES];

    if (buf_len < 2u)
    {
        /* payload_len (byte 1) hasn't arrived yet. */
        return 0;
    }

    payload_len = buf[1];

    if (payload_len > LORA_FRAME_MAX_PAYLOAD)
    {
        /* This declared length can never belong to a valid frame no
           matter how many more bytes show up - treat it the same as
           a checksum/tag failure (drop and resync is the caller's
           call), rather than waiting forever for an unreachable byte
           count. */
        return -1;
    }

    /* Checked only after the payload_len reject above, so an impossible
       length is rejected from byte 1 alone instead of waiting for the
       rest of a header that can never belong to a valid frame. */
    if (buf_len < LORA_FRAME_HEADER_SIZE)
    {
        return 0;
    }

    frame_size = LORA_FRAME_HEADER_SIZE + (uint32_t)payload_len + LORA_FRAME_TRAILER_SIZE;

    if (buf_len < frame_size)
    {
        /* Header is readable but the payload/tag/counter_frag/
           checksum bytes haven't all arrived yet. */
        return 0;
    }

    sender_board_id = buf[2];
    ciphertext = &buf[LORA_FRAME_HEADER_SIZE];
    tag = &ciphertext[payload_len];
    counter = (uint32_t)tag[LORA_FRAME_TAG_SIZE] |
              ((uint32_t)tag[LORA_FRAME_TAG_SIZE + 1u] << 8) |
              ((uint32_t)tag[LORA_FRAME_TAG_SIZE + 2u] << 16) |
              ((uint32_t)tag[LORA_FRAME_TAG_SIZE + 3u] << 24);

    expected_checksum = lora_frame_checksum(ciphertext, payload_len);

    if (tag[LORA_FRAME_TAG_SIZE + LORA_FRAME_COUNTER_FRAG_SIZE] != expected_checksum)
    {
        /* Cheap pre-filter caught it - don't even pay for a Poly1305
           verify on data that's already known-corrupted. */
        return -1;
    }

    epoch = (uint32_t)buf[3] | ((uint32_t)buf[4] << 8) |
            ((uint32_t)buf[5] << 16) | ((uint32_t)buf[6] << 24);
    rx_class = lora_crypto_rx_classify(sender_board_id, epoch);

    /* Replay floor applies to the sender's CONFIRMED session only - a new
       session legitimately restarts its counter (see core/lora_crypto.h).
       Checked before the AEAD verify: a replayed frame would verify. */
    if (rx_class == LORA_CRYPTO_RX_TRUSTED &&
        lora_crypto_counter_is_replay(sender_board_id, counter))
    {
        g_lora_frame_replay_count++;
        return -1;
    }

    lora_crypto_build_nonce(sender_board_id, epoch, counter, nonce);

    if (!aead_chacha20poly1305_open(g_lora_crypto_psk, nonce,
                                     buf, LORA_FRAME_HEADER_SIZE,
                                     ciphertext, payload_len, tag,
                                     out_payload))
    {
        /* Tag didn't verify - either genuine RF corruption the
           checksum pre-filter happened to miss, a replay (see
           core/lora_crypto.h's reconstruction doc), or someone
           without the PSK. Do NOT call lora_crypto_accept_counter()
           here - see that function's own doc on why committing an
           unverified counter would be a self-inflicted problem, not
           a security fix. */
        g_lora_frame_auth_fail_count++;
        return -1;
    }

    if (rx_class == LORA_CRYPTO_RX_UNCONFIRMED)
    {
        /* Authentic, but possibly a replay from an earlier session of
           this sender - held until the sender proves the session is
           live (core/lora_crypto.h "Session control"). Only a matching
           RESPONSE (confirms) or a CHALLENGE (answered) gets through,
           and neither ever reaches the inbox. Nothing is committed
           here: no floor, no epoch. */
        if (buf[0] != LORA_FRAME_TYPE_SESSION ||
            !lora_crypto_session_rx(sender_board_id, epoch, counter, rx_class,
                                    out_payload, payload_len))
        {
            if (buf[0] != LORA_FRAME_TYPE_SESSION)
            {
                lora_crypto_note_unconfirmed_session(sender_board_id, epoch);
            }
            g_lora_frame_session_hold_count++;
            return -1;
        }
    }
    else
    {
        if (rx_class == LORA_CRYPTO_RX_BOOTSTRAP)
        {
            lora_crypto_confirm_session(sender_board_id, epoch, counter);
        }
        else
        {
            lora_crypto_accept_counter(sender_board_id, counter);
        }

        if (buf[0] == LORA_FRAME_TYPE_SESSION)
        {
            (void)lora_crypto_session_rx(sender_board_id, epoch, counter, rx_class,
                                         out_payload, payload_len);
        }
    }

    g_lora_frame_accept_count++;

    *out_type = buf[0];
    *out_sender_board_id = sender_board_id;
    *out_payload_len = payload_len;

    return 1;
}

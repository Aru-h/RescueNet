#include "lora_frame.h"
#include "aead_chacha20poly1305.h"

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

    if (payload_len > LORA_FRAME_MAX_PAYLOAD)
    {
        /* Caller error: more payload than this envelope can carry.
           Write nothing and report zero bytes written rather than
           silently truncating the caller's data. Deliberately does
           NOT call lora_crypto_next_tx_counter() first - nothing is
           being sent, so nothing should consume a counter value. */
        return 0u;
    }

    out_buf[0] = frame_type;
    out_buf[1] = payload_len;
    out_buf[2] = sender_board_id;

    ciphertext = &out_buf[LORA_FRAME_HEADER_SIZE];
    tag_out = &ciphertext[payload_len];
    counter_frag_out = &tag_out[LORA_FRAME_TAG_SIZE];
    checksum_out = &counter_frag_out[LORA_FRAME_COUNTER_FRAG_SIZE];

    counter = lora_crypto_next_tx_counter();
    lora_crypto_build_nonce(sender_board_id, counter, nonce);

    /* AAD = the three clear header bytes just written above - see
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
    uint16_t wire_frag;
    uint8_t expected_checksum;
    uint32_t counter;
    uint8_t nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES];

    if (buf_len < LORA_FRAME_HEADER_SIZE)
    {
        /* Not even frame_type + payload_len + sender_board_id have
           arrived yet. */
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
    wire_frag = (uint16_t)(tag[LORA_FRAME_TAG_SIZE]
                            | ((uint16_t)tag[LORA_FRAME_TAG_SIZE + 1u] << 8));

    expected_checksum = lora_frame_checksum(ciphertext, payload_len);

    if (tag[LORA_FRAME_TAG_SIZE + LORA_FRAME_COUNTER_FRAG_SIZE] != expected_checksum)
    {
        /* Cheap pre-filter caught it - don't even pay for a Poly1305
           verify on data that's already known-corrupted. */
        return -1;
    }

    counter = lora_crypto_reconstruct_counter(sender_board_id, wire_frag);
    lora_crypto_build_nonce(sender_board_id, counter, nonce);

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
        return -1;
    }

    lora_crypto_accept_counter(sender_board_id, counter);

    *out_type = buf[0];
    *out_sender_board_id = sender_board_id;
    *out_payload_len = payload_len;

    return 1;
}

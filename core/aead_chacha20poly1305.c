#include "aead_chacha20poly1305.h"
#include "chacha20.h"
#include "poly1305.h"
#include <string.h> /* memcpy/memset - see core/chacha20.c's own note on this
                        already being an established freestanding-safe
                        dependency in core/ */

/* Upper bound on the mac_data buffer built below: pad16(AAD) +
   pad16(data) + 16-byte length trailer, with AAD/data individually
   bounded by AEAD_CHACHA20POLY1305_MAX_AAD_BYTES/_MAX_DATA_BYTES
   (see aead_chacha20poly1305.h). pad16(8) = 16, pad16(144) = 144
   (already a multiple of 16), so 16 + 144 + 16 = 176 covers the
   worst case with those two bounds; sized generously beyond that
   here (192) so a future small bump to either bound doesn't
   silently overflow this buffer unnoticed. */
#define AEAD_MAC_DATA_MAX_BYTES 192u

static uint32_t aead_pad16(uint32_t n)
{
    uint32_t rem = n % 16u;
    return (rem == 0u) ? 0u : (16u - rem);
}

static void aead_store_le64(uint8_t *p, uint64_t v)
{
    uint32_t i;
    for (i = 0u; i < 8u; i++)
    {
        p[i] = (uint8_t)((v >> (8u * i)) & 0xffu);
    }
}

/*
 * Builds mac_data per RFC 8439 section 2.8.1:
 *   aad || pad16(aad) || ciphertext || pad16(ciphertext)
 *   || le64(aad_len) || le64(ciphertext_len)
 * into `out` (caller-provided, at least AEAD_MAC_DATA_MAX_BYTES).
 * Returns the total length written. Shared between seal() and
 * open() - both need the identical byte string, seal() over the
 * ciphertext it just produced, open() over the ciphertext it was
 * handed (verified before any plaintext is trusted/returned).
 */
static uint32_t aead_build_mac_data(const uint8_t *aad, uint32_t aad_len,
                                     const uint8_t *ciphertext, uint32_t len,
                                     uint8_t *out)
{
    uint32_t offset = 0u;
    uint32_t pad;

    memcpy(&out[offset], aad, aad_len);
    offset += aad_len;

    pad = aead_pad16(aad_len);
    memset(&out[offset], 0, pad);
    offset += pad;

    memcpy(&out[offset], ciphertext, len);
    offset += len;

    pad = aead_pad16(len);
    memset(&out[offset], 0, pad);
    offset += pad;

    aead_store_le64(&out[offset], (uint64_t)aad_len);
    offset += 8u;
    aead_store_le64(&out[offset], (uint64_t)len);
    offset += 8u;

    return offset;
}

/*
 * Derives the one-time Poly1305 key per RFC 8439 section 2.6: the
 * first 32 bytes of the ChaCha20 keystream block at counter 0 for
 * (key, nonce). This block is used for NOTHING else - in particular
 * the actual ciphertext keystream starts at counter 1 (see
 * aead_chacha20poly1305_seal()/_open() below) so this block's
 * bytes 32..63 (thrown away here) are never reused as ciphertext
 * keystream either, per the RFC's own construction.
 */
static void aead_derive_poly1305_key(
    const uint8_t key[AEAD_CHACHA20POLY1305_KEY_BYTES],
    const uint8_t nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES],
    uint8_t out_poly_key[POLY1305_KEY_BYTES])
{
    uint8_t block[CHACHA20_BLOCK_BYTES];

    chacha20_block(key, nonce, 0u, block);
    memcpy(out_poly_key, block, POLY1305_KEY_BYTES);
}

/*
 * Constant-time tag comparison - a data-dependent early-exit compare
 * here would turn "does this tag verify" into a byte-at-a-time
 * timing oracle for an attacker who can measure response latency
 * over many attempts, which is exactly the kind of forgery-by-
 * timing this AEAD's whole point is to prevent. Every byte is
 * compared unconditionally regardless of earlier mismatches.
 */
static int aead_tag_equal(const uint8_t a[AEAD_CHACHA20POLY1305_TAG_BYTES],
                           const uint8_t b[AEAD_CHACHA20POLY1305_TAG_BYTES])
{
    uint8_t diff = 0u;
    uint32_t i;

    for (i = 0u; i < AEAD_CHACHA20POLY1305_TAG_BYTES; i++)
    {
        diff = (uint8_t)(diff | (a[i] ^ b[i]));
    }

    return diff == 0u;
}

int aead_chacha20poly1305_seal(
    const uint8_t key[AEAD_CHACHA20POLY1305_KEY_BYTES],
    const uint8_t nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES],
    const uint8_t *aad, uint32_t aad_len,
    const uint8_t *plaintext, uint32_t len,
    uint8_t *out_ciphertext,
    uint8_t out_tag[AEAD_CHACHA20POLY1305_TAG_BYTES])
{
    uint8_t poly_key[POLY1305_KEY_BYTES];
    uint8_t mac_data[AEAD_MAC_DATA_MAX_BYTES];
    uint32_t mac_data_len;

    if (len > AEAD_CHACHA20POLY1305_MAX_DATA_BYTES
        || aad_len > AEAD_CHACHA20POLY1305_MAX_AAD_BYTES)
    {
        return 0;
    }

    aead_derive_poly1305_key(key, nonce, poly_key);

    /* Ciphertext keystream starts at counter 1 - counter 0 is
       reserved for the Poly1305 key derived just above and must
       never also be used as ciphertext keystream (that would reuse
       the same keystream bytes for two different purposes under one
       (key, nonce) pair, defeating both). */
    chacha20_xor(key, nonce, 1u, plaintext, out_ciphertext, len);

    mac_data_len = aead_build_mac_data(aad, aad_len, out_ciphertext, len, mac_data);
    poly1305_mac(poly_key, mac_data, mac_data_len, out_tag);

    return 1;
}

int aead_chacha20poly1305_open(
    const uint8_t key[AEAD_CHACHA20POLY1305_KEY_BYTES],
    const uint8_t nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES],
    const uint8_t *aad, uint32_t aad_len,
    const uint8_t *ciphertext, uint32_t len,
    const uint8_t tag[AEAD_CHACHA20POLY1305_TAG_BYTES],
    uint8_t *out_plaintext)
{
    uint8_t poly_key[POLY1305_KEY_BYTES];
    uint8_t mac_data[AEAD_MAC_DATA_MAX_BYTES];
    uint32_t mac_data_len;
    uint8_t computed_tag[AEAD_CHACHA20POLY1305_TAG_BYTES];

    if (len > AEAD_CHACHA20POLY1305_MAX_DATA_BYTES
        || aad_len > AEAD_CHACHA20POLY1305_MAX_AAD_BYTES)
    {
        return 0;
    }

    aead_derive_poly1305_key(key, nonce, poly_key);

    mac_data_len = aead_build_mac_data(aad, aad_len, ciphertext, len, mac_data);
    poly1305_mac(poly_key, mac_data, mac_data_len, computed_tag);

    if (!aead_tag_equal(computed_tag, tag))
    {
        /* Deliberately do NOT decrypt on a failed verify - handing
           back plaintext for a forged/corrupted frame, even if the
           caller is "supposed to" check the return value first,
           is an unforced risk (a caller bug that skips the check
           would otherwise silently accept attacker-controlled
           bytes as if authenticated). */
        return 0;
    }

    chacha20_xor(key, nonce, 1u, ciphertext, out_plaintext, len);

    return 1;
}

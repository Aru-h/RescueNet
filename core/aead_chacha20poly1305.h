#ifndef AEAD_CHACHA20POLY1305_H
#define AEAD_CHACHA20POLY1305_H

#include <stdint.h>

/*
 * AEAD_CHACHA20_POLY1305, RFC 8439 section 2.8: composes
 * core/chacha20.h (encryption) and core/poly1305.h (authentication)
 * into a single authenticated-encryption primitive - the actual
 * thing core/lora_crypto.c calls per LoRa frame. This file has no
 * opinion on where the key/nonce come from (that's core/lora_crypto.c's
 * job: static PSK, per-sender counter, wire framing) - it only
 * implements the generic RFC construction, so it can be verified
 * against RFC-independent test vectors on its own (see
 * laptop/tools/aead_selftest.c) without dragging in any LoRa-
 * specific policy.
 *
 * Max plaintext/ciphertext/AAD sizes here are bounded by
 * LORA_FRAME_MAX_PAYLOAD-scale buffers throughout this project (at
 * most a few hundred bytes per frame), NOT by RFC 8439's own
 * theoretical 2^32-block limit - this implementation does not
 * attempt to support multi-gigabyte messages, and callers with
 * larger data should not use it without re-checking that assumption.
 */

#define AEAD_CHACHA20POLY1305_KEY_BYTES   32u
#define AEAD_CHACHA20POLY1305_NONCE_BYTES 12u
#define AEAD_CHACHA20POLY1305_TAG_BYTES   16u

/*
 * Encrypts `len` bytes of `plaintext` into `out_ciphertext` (may be
 * the same buffer for in-place use) under (key, nonce), authenticating
 * both the ciphertext and `aad`/`aad_len` (sent in the clear
 * alongside the frame - see core/lora_frame.h's header fields).
 * Writes the 16-byte tag to out_tag. `len` must not exceed
 * AEAD_CHACHA20POLY1305_MAX_DATA_BYTES (see below) and `aad_len`
 * must not exceed AEAD_CHACHA20POLY1305_MAX_AAD_BYTES; both limits
 * exist only to bound the stack buffer this function builds
 * internally for the Poly1305 mac_data (RFC 8439 section 2.8.1) -
 * see this header's own top comment on why "arbitrary length" was
 * never a design goal here.
 */
#define AEAD_CHACHA20POLY1305_MAX_DATA_BYTES 144u
#define AEAD_CHACHA20POLY1305_MAX_AAD_BYTES  8u

/*
 * Returns 1 on success, 0 if len/aad_len exceed the bounds above
 * (caller error - nothing is written in that case).
 */
int aead_chacha20poly1305_seal(
    const uint8_t key[AEAD_CHACHA20POLY1305_KEY_BYTES],
    const uint8_t nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES],
    const uint8_t *aad, uint32_t aad_len,
    const uint8_t *plaintext, uint32_t len,
    uint8_t *out_ciphertext,
    uint8_t out_tag[AEAD_CHACHA20POLY1305_TAG_BYTES]);

/*
 * Decrypts+verifies. Returns 1 and writes `len` bytes to
 * out_plaintext (may alias `ciphertext`) if the tag verifies.
 * Returns 0 (writes nothing to out_plaintext) if the tag does not
 * verify OR if len/aad_len exceed the bounds above - a caller must
 * not distinguish these two failure cases from the return value
 * alone (that would leak a timing/behavioral oracle about which
 * failure occurred); core/lora_frame.c's caller-facing contract
 * (lora_frame_try_parse()'s -1 "drop and resync" outcome) already
 * treats both identically, matching this.
 */
int aead_chacha20poly1305_open(
    const uint8_t key[AEAD_CHACHA20POLY1305_KEY_BYTES],
    const uint8_t nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES],
    const uint8_t *aad, uint32_t aad_len,
    const uint8_t *ciphertext, uint32_t len,
    const uint8_t tag[AEAD_CHACHA20POLY1305_TAG_BYTES],
    uint8_t *out_plaintext);

#endif /* AEAD_CHACHA20POLY1305_H */

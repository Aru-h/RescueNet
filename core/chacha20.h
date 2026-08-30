#ifndef CHACHA20_H
#define CHACHA20_H

#include <stdint.h>

/*
 * ChaCha20 stream cipher, RFC 8439 section 2.4 (the IETF variant: a
 * 96-bit nonce and a 32-bit block counter, not the original 64-bit-
 * nonce/64-bit-counter Bernstein layout). This is the primitive
 * layer only - key/nonce management, AEAD composition with
 * Poly1305, and the LoRa-frame-specific counter/replay policy all
 * live one level up (core/aead_chacha20poly1305.h and
 * core/lora_crypto.h respectively). This file has no opinion on any
 * of that; it just turns (key, nonce, counter) into keystream bytes
 * and XORs them against a buffer.
 *
 * Written in-repo rather than vendoring an external library: see
 * the project's LoRa-encryption reference doc for why (freestanding,
 * no stdlib, needs to build clean for both Cortex-M55 and
 * Cortex-M0+ with no external dependency/licensing surface to
 * validate). Verified against RFC 8439's own test vectors - see
 * laptop/tools/aead_selftest.c - not just written from the spec and
 * assumed correct.
 */

#define CHACHA20_KEY_BYTES   32u
#define CHACHA20_NONCE_BYTES 12u
#define CHACHA20_BLOCK_BYTES 64u

/*
 * Encrypts (or decrypts - XOR is its own inverse, this is a stream
 * cipher) `len` bytes from `in` into `out` (may alias `in` == `out`
 * for in-place use, same convention core/lora_frame.c's own pack/
 * parse pair does not need but crypto callers generally want).
 *
 * `key` must be CHACHA20_KEY_BYTES (32) bytes. `nonce` must be
 * CHACHA20_NONCE_BYTES (12) bytes. `initial_counter` is the 32-bit
 * block counter the RFC 8439 AEAD construction expects to vary
 * between the one-time-Poly1305-key block (counter 0) and the
 * actual ciphertext (counter 1, incrementing per 64-byte block
 * after that) - this function does not hardcode either use, callers
 * pick the counter that matches which of those two things they're
 * generating.
 */
void chacha20_xor(const uint8_t key[CHACHA20_KEY_BYTES],
                   const uint8_t nonce[CHACHA20_NONCE_BYTES],
                   uint32_t initial_counter,
                   const uint8_t *in, uint8_t *out, uint32_t len);

/*
 * Raw block function: fills out[CHACHA20_BLOCK_BYTES] with the
 * keystream block for (key, nonce, counter), no XOR against
 * anything. Exposed separately because core/aead_chacha20poly1305.c
 * needs exactly one raw block (counter 0) to derive the one-time
 * Poly1305 key, without wanting to XOR it against a real buffer -
 * chacha20_xor() could be made to do this by XORing against a
 * zeroed buffer, but that is more confusing at the call site than
 * naming the actual operation being performed.
 */
void chacha20_block(const uint8_t key[CHACHA20_KEY_BYTES],
                     const uint8_t nonce[CHACHA20_NONCE_BYTES],
                     uint32_t counter,
                     uint8_t out[CHACHA20_BLOCK_BYTES]);

#endif /* CHACHA20_H */

#ifndef POLY1305_H
#define POLY1305_H

#include <stdint.h>

/*
 * Poly1305 one-time message authenticator, RFC 8439 section 2.5.
 * One-shot API only (init/process/finish all in a single call) -
 * core/aead_chacha20poly1305.c is the only caller, and it always has
 * the complete mac_data buffer (AAD + padding + ciphertext + padding
 * + 16-byte length field, per RFC 8439 section 2.8.1) assembled in
 * memory before it needs a tag, so there is no streaming use case in
 * this codebase to justify the extra init/update/finish state-
 * machine surface a general-purpose Poly1305 library would carry.
 *
 * Implementation is the standard 26-bit-limb ("donna-style")
 * algorithm ported to fixed-width uint32_t/uint64_t types rather
 * than `unsigned long`/`unsigned long long` (the original public-
 * domain reference's types) - `unsigned long` is 32 bits on the
 * Cortex-M targets this project builds for but 64 bits on the
 * x86-64 Linux host used for laptop-side selftests, and this
 * algorithm's carry/reduction logic is written assuming a specific
 * limb width, so pinning it to uint32_t/uint64_t explicitly (matching
 * every other file in core/) removes that platform-dependent
 * ambiguity rather than relying on it happening to still work.
 *
 * CRITICAL: Poly1305 keys are one-time by construction (RFC 8439's
 * "one-time authenticator" in its own name) - reusing a Poly1305 key
 * across two different messages breaks the MAC's security
 * guarantees entirely, independent of anything ChaCha20 does. This
 * is why core/aead_chacha20poly1305.c derives a fresh Poly1305 key
 * for every single AEAD seal/open call (via chacha20_block() at
 * counter 0) instead of reusing one - callers of THIS file must
 * never pass the same `key` to two different `poly1305_mac()` calls
 * over different `msg` contents.
 */

#define POLY1305_KEY_BYTES 32u
#define POLY1305_TAG_BYTES 16u

/*
 * Computes the 16-byte Poly1305 tag for `msg` (`len` bytes) under
 * the given one-time `key` (32 bytes: a 16-byte "r" value followed
 * by a 16-byte "s" pad, per RFC 8439 section 2.5.1 - this is exactly
 * the layout core/aead_chacha20poly1305.c's ChaCha20-block-0-derived
 * key already produces, no repacking needed at the call site).
 * Writes the tag to out_tag[POLY1305_TAG_BYTES].
 */
void poly1305_mac(const uint8_t key[POLY1305_KEY_BYTES],
                   const uint8_t *msg, uint32_t len,
                   uint8_t out_tag[POLY1305_TAG_BYTES]);

#endif /* POLY1305_H */

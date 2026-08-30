#ifndef LORA_CRYPTO_H
#define LORA_CRYPTO_H

#include <stdint.h>
#include "aead_chacha20poly1305.h"

/*
 * LoRa-frame-specific crypto POLICY: key management, per-sender
 * counters, nonce construction, and 16-bit-wire-fragment counter
 * reconstruction. core/aead_chacha20poly1305.h (the RFC 8439
 * primitive) has no opinion on any of this - this file supplies it,
 * and core/lora_frame.c is this file's only caller (the choke point
 * every frame_type already passes through - see that file's own
 * doc). Keeping this policy in its own file rather than folding it
 * into lora_frame.c mirrors how core/mission_link_handshake.c is
 * kept separate from core/comms_transport.c despite riding the same
 * envelope: framing (how bytes are laid out) and protocol policy
 * (what the bytes mean / how trust is established) are different
 * concerns even when one calls the other.
 *
 * ---------------------------------------------------------------
 * Key management: static pre-shared key (the reference doc's
 * "pragmatic fallback if time is short" option, not the "full
 * solution" per-boot session-key option).
 * ---------------------------------------------------------------
 * g_lora_crypto_psk below is ONE key, shared by every board, fixed
 * for the life of this build. This sidesteps the doc's "bootstrap
 * paradox" entirely (deriving a session key FROM the handshake,
 * when the handshake itself needs a key to be readable, is
 * circular) - with a static PSK there is no derivation step, so
 * MissionLinkHandshakeReq_t/Reply_t ride the exact same encrypted
 * envelope as every other frame_type with no special-casing.
 *
 * KNOWN, DELIBERATE, DOCUMENTED TRADEOFF (per the reference doc):
 * because the key is static but each board's counter resets to 0 on
 * reboot, a frame captured before a reboot is technically replayable
 * immediately after that reboot (the counter/nonce space restarts
 * while the key does not - see this file's per-sender counter
 * state below). This is accepted for the contest build given the
 * time budget; a real deployment would need the doc's "full
 * solution" (root-secret + per-boot-derived session key) to close
 * this gap. Documenting it here, not hiding it - see this project's
 * own "empirical verification / correctness over assertion" norm:
 * a known gap that's written down is a design decision, one that's
 * quietly true anyway is a bug waiting to be found.
 *
 * Override before any real deployment via -DLORA_CRYPTO_PSK_INIT=...
 * (an 32-element brace-init-list body) at build time - the default
 * below is a fixed, publicly-visible placeholder suitable only for
 * this contest build, not for anything that needs real
 * confidentiality against a motivated attacker with source access.
 * ---------------------------------------------------------------
 */
#define LORA_CRYPTO_KEY_BYTES AEAD_CHACHA20POLY1305_KEY_BYTES
extern const uint8_t g_lora_crypto_psk[LORA_CRYPTO_KEY_BYTES];

/*
 * Upper bound on distinct sender_board_id values this file tracks
 * per-sender counter state for. Deliberately its OWN constant, not
 * a re-use of core/mission_link_handshake.h's HANDSHAKE_MAX_BOARDS -
 * this is core crypto-layer plumbing and should not have to include
 * a higher-level protocol header just to size an array; the two
 * values are expected to stay in practical agreement (both default
 * to a generous upper bound on board count, not the real board
 * count) but are intentionally decoupled headers. sender_board_id
 * values >= this cap are rejected defensively (see
 * lora_crypto_reconstruct_counter()/_accept_counter() below) rather
 * than indexing out of bounds.
 */
#ifndef LORA_CRYPTO_MAX_SENDERS
#define LORA_CRYPTO_MAX_SENDERS 8u
#endif

/*
 * ---------------------------------------------------------------
 * TX side: this board's own outgoing counter.
 * ---------------------------------------------------------------
 * One counter, shared across every frame_type and every destination
 * this board sends to - see this file's own top comment on why a
 * single (sender_board_id, counter) pair is enough to guarantee
 * nonce uniqueness under the shared PSK without needing a
 * per-destination split.
 */

/* Returns the next 32-bit counter value to use for a frame this
   board is about to send, and advances the internal counter so the
   next call returns a strictly greater value. Call exactly once per
   frame actually sealed (core/lora_frame.c's pack path) - calling
   it and then NOT sending the resulting frame just burns a counter
   value, which is harmless (the scheme tolerates gaps, per this
   file's reconstruction algorithm below) but should not be done
   speculatively. */
uint32_t lora_crypto_next_tx_counter(void);

/* Test/reset hook: resets the TX counter to 0 and clears all RX
   per-sender state (see below). Exists for the same reason
   core/mission_link_handshake.h's board_init() does - a fresh state
   for a new test run - not something board bring-up code should
   call after real traffic has started (that would reintroduce the
   exact reboot-replay window this file's top comment already
   documents as a known, one-time-per-real-reboot tradeoff; calling
   it mid-session multiplies that window). */
void lora_crypto_reset(void);

/*
 * ---------------------------------------------------------------
 * Nonce construction (both TX and RX use this - it's a pure
 * function of (sender_board_id, counter), not itself stateful).
 * ---------------------------------------------------------------
 * 12-byte ChaCha20 nonce layout: byte 0 = sender_board_id, bytes
 * 1..7 = zero, bytes 8..11 = counter (little-endian). The
 * sender_board_id byte is what makes this safe under one shared PSK
 * across multiple boards - two different boards can never produce
 * the same nonce even if their counters happen to reach the same
 * value at the same time, because the leading byte differs. Without
 * it, two boards both starting their own counter at 0 under the
 * same static key would produce IDENTICAL (key, nonce) pairs for
 * their very first frame each - a catastrophic AEAD break (plaintext
 * recovery and tag forgery both become possible), not a theoretical
 * one. This is the concrete reason core/lora_frame.h's wire format
 * carries an explicit sender_board_id byte (see that header's own
 * doc) even though the LoRa-encryption reference doc's original
 * byte-count sketch didn't include one - the sketch settled the
 * counter/reconstruction shape but didn't work through the
 * multi-sender-under-one-key implication, which this comment is the
 * record of resolving.
 *
 * TWO LIMITS this scheme depends on, both worth stating explicitly
 * rather than leaving implicit in the byte widths above:
 *
 *   1. Sender-space ceiling: sender_board_id is ONE byte, so this
 *      scheme supports at most 256 distinct boards sharing one PSK
 *      before two would be forced to collide on that leading nonce
 *      byte. Nowhere near a real constraint at this project's scale
 *      (2 boards today, HANDSHAKE_MAX_BOARDS defaults to 8 - see
 *      that macro's own doc) - flagged here only so it's a written-
 *      down design limit, not a silent one, if this project ever
 *      grew toward hundreds of boards on one shared key.
 *
 *   2. Uniqueness is ASSUMED, not enforced by this file (or by
 *      anything else at runtime/compile-time across the two board
 *      targets - see the Makefile's check-board-ids-distinct target,
 *      the one place that actually checks it, since it's the one
 *      place both boards' THIS_BOARD_ID values are visible in the
 *      same build at once). This file has no way to detect a
 *      provisioning mistake (two physical boards flashed with the
 *      same THIS_BOARD_ID) - it can only build a safe nonce GIVEN
 *      that sender_board_id values are actually distinct across
 *      boards, the same precondition core/comms_transport.c's own
 *      board-id-based team routing already depends on for basic
 *      correctness (a collision breaks routing visibly - crypto
 *      would fail silently instead, which is worse, not better -
 *      see check-board-ids-distinct's own Makefile comment).
 */
void lora_crypto_build_nonce(uint8_t sender_board_id, uint32_t counter,
                              uint8_t out_nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES]);

/*
 * ---------------------------------------------------------------
 * RX side: per-sender counter reconstruction + replay rejection.
 * ---------------------------------------------------------------
 * lora_frame.h's wire format only carries the low 16 bits of the
 * 32-bit counter (LORA_FRAME_COUNTER_FRAG_SIZE) - mirrors LoRaWAN's
 * own frame-counter scheme, per the reference doc. These two
 * functions implement that scheme's receive side, keyed per
 * sender_board_id so this board's own loopback traffic (same-board
 * team-to-team sends, see core/comms_transport.c) and a genuine
 * peer's traffic never share reconstruction state even though both
 * ride the same wire format.
 */

/* Reconstructs the full 32-bit counter from `wire_frag` given
   sender_board_id's last-ACCEPTED counter (see
   lora_crypto_accept_counter() below - "accepted" means "this
   board's AEAD open() already verified a frame at that counter
   value", not merely "seen"). Picks the smallest value congruent to
   wire_frag (mod 65536) that is strictly greater than the last
   accepted value for this sender - i.e. the closest plausible next
   counter, tolerating any number of dropped frames in between
   without needing a replay window. If this sender_board_id has
   never had an accepted frame yet, returns wire_frag verbatim (any
   starting point is accepted from a sender's first-ever frame - see
   lora_crypto_accept_counter()'s own doc for why bootstrapping this
   way is safe: an attacker who doesn't hold the PSK still can't
   produce a frame that verifies, no matter what counter value they
   pick).
   sender_board_id must be < LORA_CRYPTO_MAX_SENDERS - out-of-range
   values return wire_frag verbatim as a defensive fallback (the
   caller's subsequent AEAD verify is still the real gate; a bad
   reconstruction here just means that verify fails, not that
   anything is trusted on the strength of this function alone). */
uint32_t lora_crypto_reconstruct_counter(uint8_t sender_board_id, uint16_t wire_frag);

/* Records that a frame from sender_board_id at exactly `counter` has
   been AEAD-verified (aead_chacha20poly1305_open() returned 1) and
   should now be the new floor for that sender's future
   reconstruction/replay checks. MUST be called only after a
   successful verify, never before or on a failed one - committing a
   counter from an unverified frame would let an attacker who cannot
   forge a valid tag still poison this sender's replay-window state
   and cause a legitimate subsequent frame to be reconstructed
   wrong (a self-inflicted denial-of-service on that sender, not a
   confidentiality/integrity break, but avoidable by just not doing
   this). sender_board_id >= LORA_CRYPTO_MAX_SENDERS is silently
   ignored (out of range for this build's tracked-sender cap, same
   policy as core/mission_link_handshake.h's own HANDSHAKE_MAX_BOARDS
   out-of-range handling). */
void lora_crypto_accept_counter(uint8_t sender_board_id, uint32_t counter);

#endif /* LORA_CRYPTO_H */

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
 * Per-boot session epoch (added after the static-PSK design above was
 * found to break at runtime on a reboot): each board picks a random
 * 32-bit epoch at boot (lora_crypto_set_tx_epoch(), fed from the
 * SX1262's own RNG by board code). It rides in every frame header
 * (authenticated AAD, see core/lora_frame.h) and in the nonce. Two
 * reboot problems this closes:
 *
 *   1. NONCE REUSE. Before this, a reboot restarted the TX counter
 *      under the same static key, so the first frames after every
 *      reboot reused (key, nonce) pairs from before it - a real
 *      ChaCha20-Poly1305 break (keystream reuse, Poly1305 key reuse),
 *      not only the replay risk this comment used to document. A new
 *      epoch gives a fresh nonce space per boot.
 *   2. LOCKOUT. The peer that stayed up kept its replay floor for the
 *      rebooted sender, so every post-reboot frame reconstructed above
 *      that floor, failed AEAD, and was silently dropped until the
 *      rebooted board had re-sent more frames than it ever sent before.
 *      Measured: floor 200 -> first 201 genuine frames lost; a board 0
 *      reboot hung boot outright (its ~40 relay Reqs never got through).
 *
 * A frame under a NEW epoch from a sender already known under another
 * epoch is not trusted on its own: a replayed frame from any earlier
 * session also verifies (same static key). So the receiver holds such
 * frames (drops them, schedules a challenge) and only switches that
 * sender to the new epoch once the sender answers a fresh challenge
 * nonce from the new epoch (see "Session control" below). A replay of an
 * old session cannot answer, so it can neither switch the receiver back
 * to that session nor disturb the currently confirmed one.
 *
 * REMAINING, DOCUMENTED TRADEOFF: a receiver with NO state for a sender
 * (its own first contact, e.g. right after the receiver itself booted)
 * still trusts the first authenticated frame it sees from that sender
 * (bootstrap). An attacker replaying a captured old-session frame at
 * exactly that moment gets it accepted once; the genuine sender's next
 * frame (new epoch) then triggers a challenge and the receiver moves to
 * the genuine session. Closing that last window needs a challenge on
 * first contact as well, which delays every boot by a round trip - not
 * done here. Documenting it here, not hiding it - see this project's
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
 * lora_crypto_counter_is_replay()/_accept_counter() below) rather
 * than indexing out of bounds.
 */
#ifndef LORA_CRYPTO_MAX_SENDERS
#define LORA_CRYPTO_MAX_SENDERS 8u
#endif
_Static_assert(LORA_CRYPTO_MAX_SENDERS <= 256u,
               "sender_board_id is one byte on the wire - more senders cannot be addressed");

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

/* Nonzero once the TX counter has reached UINT32_MAX: the counter
   saturates there instead of wrapping (which would reuse an AEAD
   nonce), so no further frame may be sealed until lora_crypto_reset(). */
uint8_t lora_crypto_tx_counter_exhausted(void);

/* Test/reset hook: resets the TX counter to 0 and clears all RX
   per-sender state (see below). Exists for the same reason
   core/mission_link_handshake.h's board_init() does - a fresh state
   for a new test run - not something board bring-up code should
   call after real traffic has started (that would reintroduce the
   exact reboot-replay window this file's top comment already
   documents as a known, one-time-per-real-reboot tradeoff; calling
   it mid-session multiplies that window). */
void lora_crypto_reset(void);

/* Sets this board's session epoch for every frame it seals from now on,
   and schedules one LORA_CRYPTO_SESSION_HELLO frame (sent by core/comms_
   transport.c's dispatcher) so peers that stayed up notice the new
   session promptly instead of waiting for this board's next real frame.
   Call once at boot, before the first frame is sent, with a value that
   differs from every previous boot's (board code uses the SX1262 RNG).
   Defaults to 0 until set, which is fine for single-process laptop
   builds and tests; two boots of a real board with the same epoch get
   the old lockout/nonce-reuse behavior back. lora_crypto_reset() sets
   it back to 0 and cancels the pending HELLO. */
void lora_crypto_set_tx_epoch(uint32_t epoch);
uint32_t lora_crypto_tx_epoch(void);

/* This board's own id, so session control frames addressed to another
   board are ignored. Configuration, like the PSK: NOT cleared by
   lora_crypto_reset(). core/comms_transport.c's comms_transport_init()
   sets it from g_comms_transport_this_board_id. Until set, control frames
   are accepted regardless of their target byte. */
void lora_crypto_set_local_board_id(uint8_t board_id);

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
void lora_crypto_build_nonce(uint8_t sender_board_id, uint32_t epoch, uint32_t counter,
                              uint8_t out_nonce[AEAD_CHACHA20POLY1305_NONCE_BYTES]);
/* ^ As of the per-boot session epoch (see this file's top comment), the
   layout is: byte 0 = sender_board_id, bytes 1..4 = epoch (LE), bytes
   5..7 = zero, bytes 8..11 = counter (LE). Everything said above about
   the sender byte still holds; the epoch bytes are what keep a rebooted
   board's restarted counter from reusing a nonce under the static key. */

/*
 * ---------------------------------------------------------------
 * RX side: per-sender replay rejection.
 * ---------------------------------------------------------------
 * lora_frame.h's wire format carries the full 32-bit counter
 * (LORA_FRAME_COUNTER_FRAG_SIZE, 4 bytes). These two functions
 * implement the receive side's replay floor, keyed per
 * sender_board_id so this board's own loopback traffic (same-board
 * team-to-team sends, see core/comms_transport.c) and a genuine
 * peer's traffic never share reconstruction state even though both
 * ride the same wire format.
 */

/* 1 if `counter` is at or below sender_board_id's replay floor for its
   CONFIRMED session (see lora_crypto_accept_counter() below), 0 otherwise
   - including for a sender with no state yet or an out-of-range id.
   lora_frame_try_parse() checks this before paying for an AEAD verify on
   a frame of the sender's confirmed session.

   History: the wire used to carry only the low 16 bits of the counter
   and this was lora_crypto_reconstruct_counter(), which guessed the
   upper bits as "the smallest value above the floor". That guess has no
   floor to work from for a new session or a first contact, so once a
   peer's counter passed 65535 a board that rebooted could never decode
   it again (found by review, reproduced in a two-process harness: board
   0 had sent ~700k frames, the rebooted peer then hung). The full 32-bit
   counter now rides the wire (+2 bytes/frame), so nothing is guessed. */
int lora_crypto_counter_is_replay(uint8_t sender_board_id, uint32_t counter);

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

/*
 * ---------------------------------------------------------------
 * Session epochs + session control (reboot recovery).
 * ---------------------------------------------------------------
 * core/lora_frame.c's lora_frame_try_parse() is the only caller of the
 * RX functions below, in this order: classify the frame's (sender,
 * epoch); reject TRUSTED frames at or below the floor
 * (lora_crypto_counter_is_replay()); verify AEAD; then commit via confirm_session() (BOOTSTRAP),
 * accept_counter() (TRUSTED), or hold the frame (UNCONFIRMED).
 *
 * Session control frames ride LORA_FRAME_TYPE_SESSION (core/lora_frame.h)
 * with a fixed LORA_CRYPTO_SESSION_PAYLOAD_SIZE payload:
 *   byte 0     subtype  HELLO / CHALLENGE / RESPONSE
 *   byte 1     target   board the frame is addressed to (0xFF = all)
 *   bytes 2-9  nonce    CHALLENGE: fresh value from the challenger;
 *                       RESPONSE: the challenge nonce echoed back
 * Exchange after board A reboots while board B stays up:
 *   A -> HELLO (or any frame) under new epoch E2 -> B holds it, schedules
 *   B -> CHALLENGE(target A, nonce N) under B's confirmed epoch
 *   A -> RESPONSE(target B, nonce N) under E2
 *   B: nonce matches its outstanding challenge -> A confirmed at E2, floor
 *      = that response's counter; lora_crypto_peer_session_count(A) bumps.
 * Challenge nonces are this board's own epoch plus a per-boot sequence
 * number, so they never repeat across boots as long as epochs don't.
 * They only need to be unique, not secret: nobody without the key can
 * seal a RESPONSE, and a recorded RESPONSE carries an old nonce.
 */
#define LORA_CRYPTO_SESSION_NONCE_BYTES   8u
#define LORA_CRYPTO_SESSION_PAYLOAD_SIZE  (2u + LORA_CRYPTO_SESSION_NONCE_BYTES)
#define LORA_CRYPTO_SESSION_HELLO         ((uint8_t)0u)
#define LORA_CRYPTO_SESSION_CHALLENGE     ((uint8_t)1u)
#define LORA_CRYPTO_SESSION_RESPONSE      ((uint8_t)2u)
#define LORA_CRYPTO_SESSION_TARGET_ALL    ((uint8_t)0xFFu)

#define LORA_CRYPTO_RX_TRUSTED      0 /* epoch already confirmed for this sender
                                         (or sender out of tracked range) */
#define LORA_CRYPTO_RX_BOOTSTRAP    1 /* no state for this sender yet */
#define LORA_CRYPTO_RX_UNCONFIRMED  2 /* known sender, different epoch */

int lora_crypto_rx_classify(uint8_t sender_board_id, uint32_t epoch);

/* Commits a new session for sender_board_id: epoch becomes its confirmed
   epoch and `counter` its replay floor (may be LOWER than the previous
   session's floor - that's the point). Clears any outstanding challenge
   and increments lora_crypto_peer_session_count(). Only after AEAD has
   verified, and for a new epoch only on a matching challenge RESPONSE. */
void lora_crypto_confirm_session(uint8_t sender_board_id, uint32_t epoch, uint32_t counter);

/* Records an AEAD-verified frame of `epoch` from a not-yet-confirmed
   session: schedules a CHALLENGE to that sender for that epoch. Reuses the
   outstanding nonce only if it was issued for this same epoch; otherwise
   issues a fresh one, and only a RESPONSE under that epoch echoing that
   nonce confirms it (see lora_crypto_note_unconfirmed_session() in the .c
   for the replay this binding closes). */
void lora_crypto_note_unconfirmed_session(uint8_t sender_board_id, uint32_t epoch);

/* Handles an AEAD-verified LORA_FRAME_TYPE_SESSION payload. rx_class is
   lora_crypto_rx_classify()'s result for the frame; BOOTSTRAP/TRUSTED
   frames must already have been committed by the caller. Returns 1 if
   the frame is acceptable as delivered (the caller returns 1 from
   lora_frame_try_parse(); comms_transport_deliver_received() then drops
   it, control frames never reach the inbox), 0 if it must be held like
   any other unconfirmed-session frame (a challenge has been scheduled). */
int lora_crypto_session_rx(uint8_t sender_board_id, uint32_t epoch, uint32_t counter,
                           int rx_class, const uint8_t *payload, uint8_t payload_len);

/* Pops the next due session control payload (RESPONSEs first, then
   CHALLENGEs, then this board's boot HELLO) into out_payload (at least
   LORA_CRYPTO_SESSION_PAYLOAD_SIZE bytes). Returns 1 if one was written,
   0 if nothing is due. core/comms_transport.c's dispatcher sends it as a
   LORA_FRAME_TYPE_SESSION frame ahead of every TX queue. */
int lora_crypto_take_session_tx(uint8_t *out_payload, uint8_t *out_payload_len);

/* Number of sessions ever confirmed for sender_board_id this boot: 1 after
   first contact, +1 per confirmed new epoch. A value above 1 means that
   sender rebooted (or otherwise started a new session) while this board
   stayed up - board 0's mission-link handshake uses the change to re-run
   that peer's handshake leg. 0 for an out-of-range sender. */
uint32_t lora_crypto_peer_session_count(uint8_t sender_board_id);

#endif /* LORA_CRYPTO_H */

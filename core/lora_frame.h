#ifndef LORA_FRAME_H
#define LORA_FRAME_H

#include <stdint.h>
#include "lora_crypto.h"

/*
 * On-air envelope for every LoRa packet, regardless of whether it
 * carries a CBBA consensus message (core/cbba.c), a relayed
 * mission-link payload, or a mission-link handshake message. Every
 * frame_type rides the SAME envelope and therefore the SAME
 * encryption - see this header's "Encryption" section below - which
 * is what resolves the LoRa-encryption reference doc's "bootstrap
 * paradox" concern about the handshake needing to be readable before
 * any session key could come from it: this project uses a static
 * pre-shared key (core/lora_crypto.h's g_lora_crypto_psk), not a
 * key derived from the handshake, so there is no ordering problem -
 * MissionLinkHandshakeReq_t/Reply_t get no special treatment, they
 * are just another payload under this envelope.
 *
 * This is a different framing problem from telemetry_frame.h's
 * magic-byte scheme. That scheme exists because USART1 carries free-
 * running ASCII debug text as well as binary frames, so a reader
 * attaching mid-stream has to scan for a frame boundary on a byte
 * boundary it doesn't otherwise know. LoRa has no such problem: the
 * radio driver hands the receiver one already-delimited packet at a
 * time (no interleaved text, no torn stream to resync within), so
 * there's nothing to scan for. A type+length header is enough - the
 * length says how many payload bytes are real, and the type says how
 * to interpret them.
 *
 * ---------------------------------------------------------------
 * Encryption (AEAD_CHACHA20_POLY1305, RFC 8439 - see
 * core/aead_chacha20poly1305.h for the primitive and
 * core/lora_crypto.h for the key/nonce/counter policy this file
 * calls into). Layout (all fields little-endian on this target
 * where multi-byte; the single-byte fields make endianness moot for
 * them):
 *
 *   byte 0            frame_type       LORA_FRAME_TYPE_* (AAD - sent
 *                                       in clear, authenticated but
 *                                       not encrypted, since a
 *                                       receiver needs it to even
 *                                       know how to route/interpret
 *                                       the frame before decrypting)
 *   byte 1            payload_len      bytes of real plaintext (AAD,
 *                                       same reasoning as frame_type)
 *   byte 2            sender_board_id  which board sent this frame
 *                                       (AAD) - see core/lora_crypto.h's
 *                                       "Nonce construction" doc for
 *                                       why this field exists: with
 *                                       one shared static key across
 *                                       every board, the nonce must
 *                                       be partitioned by sender or
 *                                       two different boards could
 *                                       reuse the same (key, nonce)
 *                                       pair - a catastrophic AEAD
 *                                       break, not a theoretical one.
 *                                       This is a deliberate, +1-byte
 *                                       deviation from the LoRa-
 *                                       encryption reference doc's
 *                                       original 165-byte sketch (see
 *                                       LORA_FRAME_MAX_WIRE_SIZE
 *                                       below) - that sketch settled
 *                                       the counter/reconstruction
 *                                       shape but didn't work through
 *                                       the multi-sender-under-one-
 *                                       key implication.
 *   byte 3..6         sender_epoch     sender's per-boot session epoch
 *                                       (LE, AAD) - see core/lora_
 *                                       crypto.h's top comment. Part of
 *                                       the nonce, and how a receiver
 *                                       tells a rebooted sender's new
 *                                       session from its old one.
 *   byte 7..N-1       ciphertext       payload_len bytes, ChaCha20
 *                                       keystream XOR plaintext (N =
 *                                       7 + payload_len)
 *   byte N..N+15      poly1305_tag     16-byte RFC 8439 Poly1305 tag
 *                                       over AAD (bytes 0..6) +
 *                                       ciphertext
 *   byte N+16..N+19   counter          the full 32-bit per-sender
 *                                       counter used to build the AEAD
 *                                       nonce (LE). Was a 16-bit
 *                                       fragment the receiver had to
 *                                       reconstruct - see core/lora_
 *                                       crypto.h lora_crypto_counter_
 *                                       is_replay() for why that
 *                                       stopped being safe.
 *   byte N+20         checksum         additive sum mod 256 over the
 *                                       ciphertext bytes only (NOT
 *                                       the tag/counter_frag) - a
 *                                       cheap pre-filter to reject
 *                                       obviously-corrupted frames
 *                                       BEFORE paying for a full
 *                                       Poly1305 verify, not a
 *                                       substitute for one (the tag
 *                                       is the real integrity/
 *                                       authenticity check; this
 *                                       checksum passing proves
 *                                       nothing on its own).
 *
 * Only frame_type + payload_len + sender_board_id + the *used*
 * payload bytes + tag + counter_frag + checksum go out over the air -
 * lora_frame_pack() does not transmit the full LORA_FRAME_MAX_PAYLOAD
 * every time, only what payload_len says is real. Total on-air size
 * for a given frame is therefore
 * (LORA_FRAME_HEADER_SIZE + payload_len + LORA_FRAME_TRAILER_SIZE)
 * bytes, which varies per frame; see LORA_FRAME_MAX_WIRE_SIZE below
 * for the worst case.
 *
 * On a checksum OR tag failure, treat it as an expected-under-real-RF
 * outcome, not a fatal error - see lora_frame_try_parse()'s -1 case
 * below, which follows the same "drop and resync" policy
 * laptop/tools/telemetry_bridge.py already applies on its checksum
 * mismatches for the USART1 case.
 */

#define LORA_FRAME_TYPE_CBBA          0u
#define LORA_FRAME_TYPE_MISSION_LINK  1u
#define LORA_FRAME_TYPE_SESSION       6u /* core/lora_crypto.h's session
                                             control (HELLO/CHALLENGE/
                                             RESPONSE) - handled inside
                                             lora_frame_try_parse()/core/
                                             comms_transport.c, never
                                             delivered to the inbox. */
#define LORA_FRAME_TYPE_HANDSHAKE     2u /* core/mission_link_handshake.h's
                                             DK<->board-1 liveness handshake.
                                             Canonical home for this value
                                             is here (folded in per that
                                             header's own long-standing
                                             flag asking whoever wires the
                                             handshake's LoRa hop to do
                                             this) — mission_link_handshake.h
                                             no longer defines its own
                                             copy, it includes this header
                                             instead. Rides the same
                                             encrypted envelope as every
                                             other frame_type - see this
                                             header's "Encryption" section
                                             above for why that's safe
                                             (static PSK, no handshake-
                                             derived-key circularity). */
#define LORA_FRAME_TYPE_FAULT_BROADCAST 3u /* core/fault_comms.c (Prompt 1):
                                             board->board, one team_id +
                                             a FAULT_BUFFER_REMOTE_EVIDENCE_
                                             FEATURES-shaped evidence
                                             vector per locally-hosted
                                             team, every tick. Feeds
                                             core/fault_buffer.c's remote
                                             evidence ring
                                             (fault_buffer_record_remote_
                                             evidence()) on the receiving
                                             side. */
#define LORA_FRAME_TYPE_FAULT_VOTE      4u /* core/fault_comms.c (Prompt 1):
                                             board->board, one vote per
                                             board per target team (not
                                             per locally-hosted team -
                                             see fault_comms.c's sender
                                             doc on why), carrying a
                                             class_mask + weight. Feeds
                                             core/fault_buffer.c's vote
                                             tally (fault_buffer_record_
                                             remote_vote()) on the
                                             receiving side. */
#define LORA_FRAME_TYPE_OVERRIDE        5u /* core/mission_link_override.c:
                                             DK->hosting board relay of a
                                             laptop override (5-byte
                                             MissionLinkOverride_t). Was
                                             3 in mission_link_override.h,
                                             colliding with FAULT_
                                             BROADCAST. Applied by the
                                             hosting board's
                                             mission_link_task.c. */
#define LORA_FRAME_TYPE_OVERRIDE_ACK    7u /* hosting board -> board 0:
                                             6-byte MissionLinkOverrideAck_t
                                             (core/mission_link_override.h
                                             section 3), forwarded to the
                                             laptop over USART1. */

/* Every LORA_FRAME_TYPE_* above must be distinct and < 32 (comms_
   transport.h's COMMS_FRAME_TYPE_BIT() mask). Two equal values would
   OR to fewer bits than they sum to, so a duplicate fails here. */
#define LORA_FRAME_TYPE_BIT_(t) ((uint32_t)1u << (t))
_Static_assert((LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_CBBA) |
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_MISSION_LINK) |
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_HANDSHAKE) |
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_FAULT_BROADCAST) |
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_FAULT_VOTE) |
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_OVERRIDE) |
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_OVERRIDE_ACK) |
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_SESSION)) ==
               (LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_CBBA) +
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_MISSION_LINK) +
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_HANDSHAKE) +
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_FAULT_BROADCAST) +
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_FAULT_VOTE) +
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_OVERRIDE) +
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_OVERRIDE_ACK) +
                LORA_FRAME_TYPE_BIT_(LORA_FRAME_TYPE_SESSION)),
               "LORA_FRAME_TYPE_* values must be distinct");

/*
 * CBBAMessage_t (core/cbba.c, private to that file) is the largest
 * payload this envelope needs to carry. Its size depends on
 * NUM_TEAMS and NUM_INCIDENTS (core/mission_state.h), currently 10
 * and 20:
 *
 *   sender_id                    1  byte
 *   winner[NUM_INCIDENTS]        20 bytes (uint8_t * 20)
 *   winning_bid[NUM_INCIDENTS]   80 bytes (float * 20, 4-byte aligned)
 *   timestamp[NUM_TEAMS]         40 bytes (uint32_t * 10)
 *                                = 144 bytes with natural struct
 *                                  alignment/padding.
 *
 * Confirmed at 144 by compiling a sizeof() probe with
 * `arm-none-eabi-gcc -mcpu=cortex-m55` and reading the byte back out
 * of the resulting .rodata via objdump - not hand-calculated. If
 * NUM_TEAMS/NUM_INCIDENTS ever change, re-run that probe and update
 * this constant to match; don't let it silently drift out of sync
 * with the real CBBAMessage_t. Also double-checked against
 * core/aead_chacha20poly1305.h's own AEAD_CHACHA20POLY1305_MAX_DATA_BYTES
 * (also 144) - lora_frame_pack()/lora_frame_try_parse() rely on this
 * envelope's max payload never exceeding what the AEAD layer was
 * sized for; a _Static_assert below enforces the two stay in sync
 * rather than relying on this comment alone.
 */
#define LORA_FRAME_MAX_PAYLOAD 144u

_Static_assert(LORA_FRAME_MAX_PAYLOAD <= AEAD_CHACHA20POLY1305_MAX_DATA_BYTES,
               "LORA_FRAME_MAX_PAYLOAD must not exceed what core/aead_chacha20poly1305.h was sized for");

/* frame_type + payload_len + sender_board_id + sender_epoch(4) - see
   this header's "Encryption" section above for why sender_board_id and
   then sender_epoch had to join the two original fields. */
#define LORA_FRAME_HEADER_SIZE ((uint32_t)7u)

/* poly1305_tag + counter_frag + checksum - see this header's
   "Encryption" section above for each field's size/purpose. */
#define LORA_FRAME_TAG_SIZE          16u
#define LORA_FRAME_COUNTER_FRAG_SIZE 4u /* full counter - name kept */
#define LORA_FRAME_CHECKSUM_SIZE     1u
#define LORA_FRAME_TRAILER_SIZE \
    ((uint32_t)(LORA_FRAME_TAG_SIZE + LORA_FRAME_COUNTER_FRAG_SIZE + LORA_FRAME_CHECKSUM_SIZE))

/* Smallest possible on-air frame: header + trailer, zero payload
   bytes. Unlike the pre-encryption envelope, this is NOT small
   (28 bytes, not 7) - AEAD overhead (tag + counter_frag + checksum)
   is paid even on an empty payload, since the tag authenticates the
   AAD header fields regardless of payload size. */
#define LORA_FRAME_MIN_WIRE_SIZE (LORA_FRAME_HEADER_SIZE + LORA_FRAME_TRAILER_SIZE)

/* Largest possible on-air frame: header + a full LORA_FRAME_MAX_PAYLOAD
   payload (as ciphertext, same length as plaintext - ChaCha20 is a
   stream cipher) + trailer. 7 + 144 + 21 = 172 bytes (was 166 before
   the 4-byte sender_epoch joined the header and the counter went from
   16 to 32 bits on the wire) - see this
   header's "Encryption" section above on why this is 166, not the
   LoRa-encryption reference doc's original 165-byte sketch (the
   +1 is the sender_board_id field that sketch didn't account for).
   Useful for sizing TX/RX radio buffers - still comfortably under
   the SX1262's 256-byte FIFO and lora_spi_transmit()'s 255-byte
   runtime cap. */
#define LORA_FRAME_MAX_WIRE_SIZE \
    (LORA_FRAME_HEADER_SIZE + LORA_FRAME_MAX_PAYLOAD + LORA_FRAME_TRAILER_SIZE)

/*
 * In-memory representation of one frame. This is sized for the worst
 * case (a full LORA_FRAME_MAX_PAYLOAD payload) for callers that want
 * a fixed-size value to build a frame into before packing; it is not
 * the on-air layout itself, which is variable-length (see
 * LORA_FRAME_MIN_WIRE_SIZE / LORA_FRAME_MAX_WIRE_SIZE above) and is
 * produced by lora_frame_pack(), not by taking sizeof(LoRaFrame_t)
 * bytes of this struct. Holds PLAINTEXT payload (the caller's actual
 * data) - the on-air ciphertext is never materialized into this
 * struct, only into lora_frame_pack()'s out_buf.
 */
typedef struct __attribute__((packed))
{
    uint8_t frame_type;      /* LORA_FRAME_TYPE_* */
    uint8_t sender_board_id; /* which board this frame is FROM */
    uint8_t payload_len;     /* actual bytes used in payload[] below */
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
} LoRaFrame_t;

/*
 * Packs frame_type + sender_board_id + payload into out_buf using
 * the on-air layout documented above, encrypting payload under
 * core/lora_crypto.h's static PSK and this board's next TX counter
 * (lora_crypto_next_tx_counter() - called exactly once per
 * successful pack, see that function's own doc on why speculative
 * calls should be avoided) - only the bytes actually used, not a
 * full LORA_FRAME_MAX_PAYLOAD every time. out_buf must have room for
 * at least (LORA_FRAME_HEADER_SIZE + payload_len + LORA_FRAME_TRAILER_SIZE)
 * bytes (LORA_FRAME_MAX_WIRE_SIZE covers the worst case for any
 * payload_len).
 *
 * sender_board_id is normally this board's own id
 * (g_comms_transport_this_board_id, from the one caller,
 * core/comms_transport.c's comms_transport_send() - a board only
 * ever packs frames it is itself originating, whether the
 * destination ends up being the same board (loopback, see
 * comms_transport_send()'s same-board path) or a different one).
 *
 * Returns the total number of bytes written to out_buf on success.
 * Returns 0 and writes nothing if payload_len exceeds
 * LORA_FRAME_MAX_PAYLOAD - that is a caller error (data too big for
 * this envelope), not something to silently truncate. Does NOT
 * advance the TX counter in this failure case (nothing was sent).
 */
uint32_t lora_frame_pack(uint8_t frame_type, uint8_t sender_board_id,
                          const uint8_t *payload, uint8_t payload_len,
                          uint8_t *out_buf);

/*
 * Parses one frame out of buf (buf_len bytes available): checks the
 * cheap checksum pre-filter, then AEAD-decrypts+verifies (see this
 * header's "Encryption" section above), rejecting a counter at or below
 * the sender's replay floor (core/lora_crypto.h lora_crypto_counter_is_
 * replay()) and, on a successful verify, committing it via
 * lora_crypto_accept_counter() - or, for a sender's new session, holding
 * the frame until that session is confirmed (see lora_crypto.h).
 *
 * Returns:
 *    1  a complete, checksum-valid, AEAD-verified frame was found.
 *       out_type, out_sender_board_id, out_payload (payload_len
 *       bytes copied in, already decrypted) and out_payload_len are
 *       filled in.
 *    0  buf does not yet contain a complete frame (fewer than
 *       payload_len + LORA_FRAME_HEADER_SIZE + LORA_FRAME_TRAILER_SIZE
 *       bytes available, where payload_len is read from buf[1]).
 *       Nothing is written to the out_* parameters.
 *   -1  a complete frame-shaped span was found but either the
 *       checksum pre-filter or the AEAD tag did not verify (or its
 *       declared payload_len exceeds LORA_FRAME_MAX_PAYLOAD, which
 *       can never be a valid frame regardless of how many more bytes
 *       arrive). Nothing is written to the out_* parameters, and the
 *       sender's counter state is NOT advanced (see
 *       lora_crypto_accept_counter()'s own doc on why that matters).
 *
 * -1 is an expected outcome under real RF conditions (bit errors,
 * partial packets) - the same policy question
 * laptop/tools/telemetry_bridge.py already answers for the USART1
 * case: drop the bad frame and resync rather than treating it as an
 * error to propagate. Since each LoRa packet is already a whole unit
 * off the radio (unlike the USART1 stream), "resync" here just means
 * the caller discards this packet and waits for the next one - there
 * is no partial-byte rescan to do within a single received packet.
 *
 * out_payload must have room for at least LORA_FRAME_MAX_PAYLOAD bytes.
 */
int lora_frame_try_parse(const uint8_t *buf, uint32_t buf_len,
                          uint8_t *out_type, uint8_t *out_sender_board_id,
                          uint8_t *out_payload, uint8_t *out_payload_len);

/* Monotonic count of frames lora_frame_try_parse() rejected because the
   AEAD tag did not verify (checksum and length rejects are NOT counted).
   That branch is silent by design. (It used to also swallow every frame
   from a peer that rebooted while this board stayed up - see core/
   lora_crypto.h's top comment for that lockout and its fix.) A rising
   count means something is sending frames this board cannot verify:
   RF corruption that slips past the 8-bit checksum, a sender
   using a different key, and a stream parser retrying misaligned bytes
   (laptop/tools/two_board_harness.c) all land in the same branch.
   Nothing in core/ acts on it; board code reports it. */
uint32_t lora_frame_auth_fail_count(void);

/* Monotonic count of frames lora_frame_try_parse() accepted (returned 1).
   Lets a board stuck waiting tell "the radio hears authenticated peer
   traffic" apart from "nothing has arrived at all". */
uint32_t lora_frame_accept_count(void);

/* Monotonic count of AEAD-VALID frames lora_frame_try_parse() held back
   (returned -1) because they came from a sender's not-yet-confirmed new
   session - see core/lora_crypto.h. Nonzero briefly after a peer reboots
   (until its challenge RESPONSE arrives); a count that keeps climbing
   means the challenge exchange is not completing. */
uint32_t lora_frame_session_hold_count(void);

/* Monotonic count of frames lora_frame_try_parse() rejected because their
   counter was at or below the sender's replay floor for its confirmed
   session (a duplicate or a replay). Not counted as an auth failure. */
uint32_t lora_frame_replay_count(void);

#endif /* LORA_FRAME_H */

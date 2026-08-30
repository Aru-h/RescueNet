/*
 * Standalone self-test for core/lora_frame.c (no hardware/board
 * required - desktop gcc only, like laptop/tools/telemetry_bridge_selftest.py
 * is for the USART1/Python side).
 *
 * Exercises:
 *   1. Happy path: pack a frame, parse it back, fields round-trip
 *      (frame_type, sender_board_id, payload_len, payload - and now
 *      that packing means AEAD-encrypting, "round-trip" also proves
 *      decryption is actually the inverse of encryption, not just
 *      that bytes come back unchanged).
 *   2. Max-size payload round-trip, since LORA_FRAME_MAX_PAYLOAD (144)
 *      is a load-bearing constant (sized to CBBAMessage_t) and is the
 *      case most likely to break silently if it ever drifts.
 *   3. The checksum-reject path: pack a frame, corrupt one ciphertext
 *      byte post-pack (as real RF bit-flip corruption would), confirm
 *      lora_frame_try_parse() returns -1 (either the cheap checksum
 *      pre-filter or the Poly1305 tag catches it - this test doesn't
 *      care which, both paths return -1 identically, see lora_frame.h).
 *   4. An oversized declared payload_len in the header (a shape no
 *      valid frame can ever have) also returns -1, not an infinite
 *      "wait for more bytes" (0).
 *   5. A short/incomplete buffer returns 0, not a false accept or a
 *      crash.
 *   6. Tampering with sender_board_id or frame_type (both AAD, not
 *      encrypted, but authenticated) is caught by the tag - proves
 *      the AAD is actually bound into the tag, not just carried
 *      alongside it unauthenticated.
 *   7. Replaying an already-accepted frame verbatim is rejected -
 *      core/lora_crypto.h's per-sender counter state is what's
 *      actually being exercised here, not lora_frame.c in isolation.
 *
 * Every test calls lora_crypto_reset() first so tests don't leak
 * TX-counter/RX-replay-state into each other via the crypto layer's
 * global state (core/lora_crypto.c) - each test wants a fresh
 * "no sender has ever sent anything yet" world.
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore \
 *       -o lora_frame_selftest laptop/tools/lora_frame_selftest.c \
 *       core/lora_frame.c core/lora_crypto.c core/aead_chacha20poly1305.c \
 *       core/chacha20.c core/poly1305.c
 * Run:
 *   ./lora_frame_selftest
 */

#include "lora_frame.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

static void test_happy_path(void)
{
    uint8_t payload[5] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_sender = 0xFFu;
    uint8_t out_len = 0xFFu;
    uint32_t packed_len;
    int rc;

    lora_crypto_reset();

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_MISSION_LINK, 0u, payload,
                                  (uint8_t)sizeof(payload), wire_buf);

    /* header(3) + 5 payload bytes + trailer(19) = 27 */
    CHECK(packed_len == LORA_FRAME_HEADER_SIZE + sizeof(payload) + LORA_FRAME_TRAILER_SIZE);

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, &out_sender,
                               out_payload, &out_len);

    CHECK(rc == 1);
    CHECK(out_type == LORA_FRAME_TYPE_MISSION_LINK);
    CHECK(out_sender == 0u);
    CHECK(out_len == sizeof(payload));
    CHECK(memcmp(out_payload, payload, sizeof(payload)) == 0);
}

static void test_max_payload_round_trip(void)
{
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_sender = 0xFFu;
    uint8_t out_len = 0u;
    uint32_t packed_len;
    uint32_t i;
    int rc;

    lora_crypto_reset();

    for (i = 0u; i < LORA_FRAME_MAX_PAYLOAD; i++)
    {
        payload[i] = (uint8_t)(i & 0xFFu);
    }

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, 1u, payload,
                                  LORA_FRAME_MAX_PAYLOAD, wire_buf);

    CHECK(packed_len == LORA_FRAME_MAX_WIRE_SIZE);
    CHECK(LORA_FRAME_MAX_WIRE_SIZE == 166u); /* see lora_frame.h's own doc
                                                 on why 166, not the
                                                 reference doc's 165 */

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, &out_sender,
                               out_payload, &out_len);

    CHECK(rc == 1);
    CHECK(out_type == LORA_FRAME_TYPE_CBBA);
    CHECK(out_sender == 1u);
    CHECK(out_len == LORA_FRAME_MAX_PAYLOAD);
    CHECK(memcmp(out_payload, payload, LORA_FRAME_MAX_PAYLOAD) == 0);
}

/* The case that actually matters once real RF is involved: a single
   corrupted ciphertext byte must be rejected (-1), not silently
   accepted or crash the parser. */
static void test_corrupted_byte_is_rejected(void)
{
    uint8_t payload[6] = { 0xDEu, 0xADu, 0xBEu, 0xEFu, 0x01u, 0x02u };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_sender = 0xFFu;
    uint8_t out_len = 0xFFu;
    uint32_t packed_len;
    int rc;

    lora_crypto_reset();

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, 0u, payload,
                                  (uint8_t)sizeof(payload), wire_buf);
    CHECK(packed_len == LORA_FRAME_HEADER_SIZE + sizeof(payload) + LORA_FRAME_TRAILER_SIZE);

    /* Flip a bit in one ciphertext byte, as an RF bit-error would -
       ciphertext starts right after the 3-byte header. */
    wire_buf[LORA_FRAME_HEADER_SIZE + 3] ^= 0x01u;

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, &out_sender,
                               out_payload, &out_len);

    CHECK(rc == -1);
}

/* AAD (frame_type/payload_len/sender_board_id) is authenticated, not
   just carried alongside the tag - tampering with it must be caught
   the same way tampering with ciphertext is. */
static void test_tampered_sender_id_is_rejected(void)
{
    uint8_t payload[4] = { 0xAAu, 0xBBu, 0xCCu, 0xDDu };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_sender = 0xFFu;
    uint8_t out_len = 0xFFu;
    uint32_t packed_len;
    int rc;

    lora_crypto_reset();

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_MISSION_LINK, 3u, payload,
                                  (uint8_t)sizeof(payload), wire_buf);

    /* Flip the sender_board_id byte (AAD, byte index 2) after
       packing, as if a corrupted/forged frame claimed to be from a
       different board. */
    wire_buf[2] ^= 0x01u;

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, &out_sender,
                               out_payload, &out_len);

    CHECK(rc == -1);
}

/* A declared payload_len bigger than LORA_FRAME_MAX_PAYLOAD can never
   be a valid frame - confirm this is reported as -1 immediately, not
   as "waiting for more data" (0), which would never resolve. */
static void test_oversized_header_is_rejected(void)
{
    uint8_t buf[4] = { LORA_FRAME_TYPE_CBBA, 0xFFu /* payload_len = 255 */,
                        0x00u, 0x00u };
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_sender = 0xFFu;
    uint8_t out_len = 0xFFu;
    int rc;

    lora_crypto_reset();

    rc = lora_frame_try_parse(buf, sizeof(buf), &out_type, &out_sender,
                               out_payload, &out_len);

    CHECK(rc == -1);
}

/* A buffer that doesn't yet hold a full frame must return 0, not a
   false accept and not out-of-bounds reads. */
static void test_incomplete_buffer_returns_zero(void)
{
    uint8_t payload[10] = { 0 };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_sender = 0xFFu;
    uint8_t out_len = 0xFFu;
    uint32_t packed_len;
    int rc;

    lora_crypto_reset();

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_MISSION_LINK, 0u, payload,
                                  (uint8_t)sizeof(payload), wire_buf);

    /* One byte short of the full frame - checksum byte hasn't "arrived". */
    rc = lora_frame_try_parse(wire_buf, packed_len - 1u, &out_type,
                               &out_sender, out_payload, &out_len);

    CHECK(rc == 0);

    /* Not even the 3-byte header has "arrived" yet. */
    rc = lora_frame_try_parse(wire_buf, 1u, &out_type, &out_sender,
                               out_payload, &out_len);

    CHECK(rc == 0);
}

/* lora_frame_pack() must refuse (return 0, write nothing) rather than
   silently truncate a payload larger than it can carry. */
static void test_pack_rejects_oversized_payload(void)
{
    uint8_t payload[1] = { 0x42u };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint32_t packed_len;

    lora_crypto_reset();

    /* payload_len is a uint8_t so it can claim up to 255, which is
       larger than LORA_FRAME_MAX_PAYLOAD (144) - exercise that path
       without needing a 256-byte source buffer: pack() only reads
       payload_len bytes from payload[] when it proceeds, and it must
       not proceed here at all. */
    packed_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, 0u, payload, 255u,
                                  wire_buf);

    CHECK(packed_len == 0u);
}

/* Replaying an already-accepted frame verbatim must be rejected on
   the second receipt - this is core/lora_crypto.h's per-sender
   counter/replay state doing its job, not a lora_frame.c-only
   property. */
static void test_replay_is_rejected(void)
{
    uint8_t payload[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_sender = 0xFFu;
    uint8_t out_len = 0xFFu;
    uint32_t packed_len;
    int rc;

    lora_crypto_reset();

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, 2u, payload,
                                  (uint8_t)sizeof(payload), wire_buf);

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, &out_sender,
                               out_payload, &out_len);
    CHECK(rc == 1);

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, &out_sender,
                               out_payload, &out_len);
    CHECK(rc == -1);
}

int main(void)
{
    test_happy_path();
    test_max_payload_round_trip();
    test_corrupted_byte_is_rejected();
    test_tampered_sender_id_is_rejected();
    test_oversized_header_is_rejected();
    test_incomplete_buffer_returns_zero();
    test_pack_rejects_oversized_payload();
    test_replay_is_rejected();

    if (g_failures == 0)
    {
        printf("lora_frame_selftest: all checks passed\n");
        return 0;
    }

    printf("lora_frame_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

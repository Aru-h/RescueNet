/*
 * Standalone self-test for core/lora_frame.c (no hardware/board
 * required - desktop gcc only, like tools/telemetry_bridge_selftest.py
 * is for the USART1/Python side).
 *
 * Exercises:
 *   1. Happy path: pack a frame, parse it back, fields round-trip.
 *   2. Max-size payload round-trip, since LORA_FRAME_MAX_PAYLOAD (144)
 *      is a load-bearing constant (sized to CBBAMessage_t) and is the
 *      case most likely to break silently if it ever drifts.
 *   3. The checksum-reject path: pack a frame, corrupt one payload
 *      byte post-pack (as real RF bit-flip corruption would), confirm
 *      lora_frame_try_parse() returns -1. This is the case that
 *      actually matters once real RF is involved - a parser that only
 *      handles well-formed input is not proven to be safe to point at
 *      a radio.
 *   4. An oversized declared payload_len in the header (a shape no
 *      valid frame can ever have) also returns -1, not an infinite
 *      "wait for more bytes" (0).
 *   5. A short/incomplete buffer returns 0, not a false accept or a
 *      crash.
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore \
 *       -o lora_frame_selftest tools/lora_frame_selftest.c core/lora_frame.c
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
    uint8_t out_len = 0xFFu;
    uint32_t packed_len;
    int rc;

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_MISSION_LINK, payload,
                                  (uint8_t)sizeof(payload), wire_buf);

    /* header(2) + 5 payload bytes + checksum(1) = 8 */
    CHECK(packed_len == 8u);

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, out_payload,
                               &out_len);

    CHECK(rc == 1);
    CHECK(out_type == LORA_FRAME_TYPE_MISSION_LINK);
    CHECK(out_len == sizeof(payload));
    CHECK(memcmp(out_payload, payload, sizeof(payload)) == 0);
}

static void test_max_payload_round_trip(void)
{
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_len = 0u;
    uint32_t packed_len;
    uint32_t i;
    int rc;

    for (i = 0u; i < LORA_FRAME_MAX_PAYLOAD; i++)
    {
        payload[i] = (uint8_t)(i & 0xFFu);
    }

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, payload,
                                  LORA_FRAME_MAX_PAYLOAD, wire_buf);

    CHECK(packed_len == LORA_FRAME_MAX_WIRE_SIZE);

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, out_payload,
                               &out_len);

    CHECK(rc == 1);
    CHECK(out_type == LORA_FRAME_TYPE_CBBA);
    CHECK(out_len == LORA_FRAME_MAX_PAYLOAD);
    CHECK(memcmp(out_payload, payload, LORA_FRAME_MAX_PAYLOAD) == 0);
}

/* The case that actually matters once real RF is involved: a single
   corrupted byte must be rejected (-1), not silently accepted or
   crash the parser. */
static void test_corrupted_byte_is_rejected(void)
{
    uint8_t payload[6] = { 0xDEu, 0xADu, 0xBEu, 0xEFu, 0x01u, 0x02u };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_type = 0xFFu;
    uint8_t out_len = 0xFFu;
    uint32_t packed_len;
    int rc;

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, payload,
                                  (uint8_t)sizeof(payload), wire_buf);
    CHECK(packed_len == 2u + sizeof(payload) + 1u);

    /* Flip a bit in one payload byte, as an RF bit-error would - not
       the checksum byte itself, since the point is that the checksum
       *catches* payload corruption it didn't originally cover. */
    wire_buf[2 + 3] ^= 0x01u; /* was payload[3] == 0xEF */

    rc = lora_frame_try_parse(wire_buf, packed_len, &out_type, out_payload,
                               &out_len);

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
    uint8_t out_len = 0xFFu;
    int rc;

    rc = lora_frame_try_parse(buf, sizeof(buf), &out_type, out_payload,
                               &out_len);

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
    uint8_t out_len = 0xFFu;
    uint32_t packed_len;
    int rc;

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_MISSION_LINK, payload,
                                  (uint8_t)sizeof(payload), wire_buf);

    /* One byte short of the full frame - checksum byte hasn't "arrived". */
    rc = lora_frame_try_parse(wire_buf, packed_len - 1u, &out_type,
                               out_payload, &out_len);

    CHECK(rc == 0);

    /* Not even the 2-byte header has "arrived" yet. */
    rc = lora_frame_try_parse(wire_buf, 1u, &out_type, out_payload,
                               &out_len);

    CHECK(rc == 0);
}

/* lora_frame_pack() must refuse (return 0, write nothing) rather than
   silently truncate a payload larger than it can carry. */
static void test_pack_rejects_oversized_payload(void)
{
    uint8_t payload[1] = { 0x42u };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint32_t packed_len;

    /* payload_len is a uint8_t so it can claim up to 255, which is
       larger than LORA_FRAME_MAX_PAYLOAD (144) - exercise that path
       without needing a 256-byte source buffer: pack() only reads
       payload_len bytes from payload[] when it proceeds, and it must
       not proceed here at all. */
    packed_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, payload, 255u,
                                  wire_buf);

    CHECK(packed_len == 0u);
}

int main(void)
{
    test_happy_path();
    test_max_payload_round_trip();
    test_corrupted_byte_is_rejected();
    test_oversized_header_is_rejected();
    test_incomplete_buffer_returns_zero();
    test_pack_rejects_oversized_payload();

    if (g_failures == 0)
    {
        printf("lora_frame_selftest: all checks passed\n");
        return 0;
    }

    printf("lora_frame_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

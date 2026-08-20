/*
 * Standalone self-test for core/mission_link_handshake.c (no hardware
 * required - desktop gcc only, same pattern as
 * tools/mission_link_selftest.c and tools/lora_frame_selftest.c).
 *
 * Covers the board-side half of the handshake (the laptop-side
 * aggregation logic - the literal "fakes both replies arriving out of
 * order" acceptance test - lives in tools/handshake_selftest.py, since
 * that's genuinely where HandshakeCoordinator lives; see that file's
 * own top comment). This file proves the board-side building blocks
 * that file's Python counterpart depends on producing/consuming
 * correctly:
 *   - Req/Reply codec round-trip on both channels (USART1 magic-byte
 *     framing and the bare comms_transport/LoRa payload), matching
 *     tools/mission_link_selftest.c's coverage for mission_link.h's
 *     four types.
 *   - resync/reject behavior (wrong magic, corrupted checksum,
 *     incomplete buffer).
 *   - the local-readiness latch: build_local_reply() returns nothing
 *     until BOTH mark_local_ready() and note_request_received() have
 *     happened, in either order.
 *   - the DK-side NUCLEO retry/timeout policy: send-on-boot,
 *     retry-on-interval, stop-after-a-real-reply, and the synthetic
 *     ready=0 relay once the timeout fires.
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore \
 *       -o handshake_selftest tools/handshake_selftest.c \
 *       core/mission_link_handshake.c
 * Run:
 *   ./handshake_selftest
 */

#include "mission_link_handshake.h"

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

static void test_req_both_channels_agree(void)
{
    MissionLinkHandshakeReq_t original;
    uint8_t usart1_wire[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE];
    MissionLinkHandshakeReq_t via_usart1;
    uint8_t relay_payload[MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE];
    MissionLinkHandshakeReq_t via_relay;
    uint32_t packed_len;
    int rc;

    original.board_id = 1u;
    original.protocol_version = MISSION_LINK_PROTOCOL_VERSION;

    packed_len = mission_link_handshake_pack_req(&original, usart1_wire);
    CHECK(packed_len == MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE);

    rc = mission_link_handshake_parse_req(usart1_wire, packed_len, &via_usart1);
    CHECK(rc == 1);

    mission_link_handshake_pack_req_payload(&original, relay_payload);
    mission_link_handshake_parse_req_payload(relay_payload, &via_relay);

    CHECK(memcmp(&via_usart1, &original, sizeof(MissionLinkHandshakeReq_t)) == 0);
    CHECK(memcmp(&via_relay, &original, sizeof(MissionLinkHandshakeReq_t)) == 0);
}

static void test_reply_both_channels_agree(void)
{
    MissionLinkHandshakeReply_t original;
    uint8_t usart1_wire[MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE];
    MissionLinkHandshakeReply_t via_usart1;
    uint8_t relay_payload[MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE];
    MissionLinkHandshakeReply_t via_relay;
    uint32_t packed_len;
    int rc;

    original.board_id = 0u;
    original.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
    original.ready = 1u;

    packed_len = mission_link_handshake_pack_reply(&original, usart1_wire);
    CHECK(packed_len == MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE);

    rc = mission_link_handshake_parse_reply(usart1_wire, packed_len, &via_usart1);
    CHECK(rc == 1);

    mission_link_handshake_pack_reply_payload(&original, relay_payload);
    mission_link_handshake_parse_reply_payload(relay_payload, &via_relay);

    CHECK(memcmp(&via_usart1, &original, sizeof(MissionLinkHandshakeReply_t)) == 0);
    CHECK(memcmp(&via_relay, &original, sizeof(MissionLinkHandshakeReply_t)) == 0);
}

static void test_wrong_magic_is_rejected(void)
{
    /* Build something that is NOT a valid handshake frame (plain
       text) and confirm parse_req/parse_reply both reject it rather
       than misreading it. */
    const uint8_t not_a_frame[MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE] =
        {'t', 'r', 'o', 'n', ':', ' '};
    MissionLinkHandshakeReq_t req_out;
    MissionLinkHandshakeReply_t reply_out;
    int rc;

    rc = mission_link_handshake_parse_req(not_a_frame, sizeof(not_a_frame), &req_out);
    CHECK(rc == -1);

    rc = mission_link_handshake_parse_reply(not_a_frame, sizeof(not_a_frame), &reply_out);
    CHECK(rc == -1);
}

static void test_corrupted_checksum_is_rejected(void)
{
    MissionLinkHandshakeReply_t original;
    uint8_t wire[MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE];
    MissionLinkHandshakeReply_t out;
    int rc;

    original.board_id = 1u;
    original.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
    original.ready = 1u;

    mission_link_handshake_pack_reply(&original, wire);
    wire[3] ^= 0x01u; /* flip a payload bit, as USART1 line noise would */

    rc = mission_link_handshake_parse_reply(wire, sizeof(wire), &out);
    CHECK(rc == -1);
}

static void test_incomplete_buffer_returns_zero(void)
{
    MissionLinkHandshakeReq_t original;
    uint8_t wire[MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE];
    MissionLinkHandshakeReq_t out;
    int rc;

    original.board_id = 0u;
    original.protocol_version = MISSION_LINK_PROTOCOL_VERSION;

    mission_link_handshake_pack_req(&original, wire);

    rc = mission_link_handshake_parse_req(wire, sizeof(wire) - 1u, &out);
    CHECK(rc == 0);

    rc = mission_link_handshake_parse_req(wire, 1u, &out);
    CHECK(rc == 0);
}

/* Local-readiness latch: build_local_reply() must return 0 until BOTH
   mark_local_ready() and note_request_received() have happened, in
   either order, and 1 (repeatably) after both have. */
static void test_local_reply_latch_both_orders(void)
{
    MissionLinkHandshakeReply_t reply;
    int rc;

    /* Order 1: request arrives first, then readiness. */
    mission_link_handshake_board_init(0u);
    CHECK(mission_link_handshake_build_local_reply(&reply) == 0);

    mission_link_handshake_note_request_received();
    rc = mission_link_handshake_build_local_reply(&reply);
    CHECK(rc == 0); /* not ready yet, even though requested */

    mission_link_handshake_mark_local_ready();
    CHECK(mission_link_handshake_local_is_ready() == 1);
    rc = mission_link_handshake_build_local_reply(&reply);
    CHECK(rc == 1);
    CHECK(reply.board_id == 0u);
    CHECK(reply.protocol_version == MISSION_LINK_PROTOCOL_VERSION);
    CHECK(reply.ready == 1u);

    /* Repeat call: still returns 1 (not "consumed"), per this
       function's documented contract. */
    CHECK(mission_link_handshake_build_local_reply(&reply) == 1);

    /* Order 2: readiness first, then request. */
    mission_link_handshake_board_init(1u);
    mission_link_handshake_mark_local_ready();
    CHECK(mission_link_handshake_build_local_reply(&reply) == 0); /* nobody asked yet */

    mission_link_handshake_note_request_received();
    rc = mission_link_handshake_build_local_reply(&reply);
    CHECK(rc == 1);
    CHECK(reply.board_id == 1u);
}

/* DK-side NUCLEO retry/timeout policy. */
static void test_nucleo_retry_and_timeout_policy(void)
{
    MissionLinkHandshakeReply_t relay;
    int rc;

    mission_link_handshake_board_init(0u);

    /* Send-on-boot: due on the very first check, at tick 0. */
    CHECK(mission_link_handshake_should_send_nucleo_req(0u) == 1);
    /* Immediately after, not due again (timer was just reset). */
    CHECK(mission_link_handshake_should_send_nucleo_req(1u) == 0);
    CHECK(mission_link_handshake_should_send_nucleo_req(
              HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS - 1u) == 0);
    /* Due again once the retry interval elapses. */
    CHECK(mission_link_handshake_should_send_nucleo_req(
              HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS) == 1);

    /* Not yet timed out. */
    CHECK(mission_link_handshake_nucleo_timed_out(HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS) == 0);
    CHECK(mission_link_handshake_get_relay_reply(HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS, &relay) == 0);

    /* A real reply arrives: stop retrying, relay it as-is. */
    {
        MissionLinkHandshakeReply_t nucleo_reply;
        nucleo_reply.board_id = 1u;
        nucleo_reply.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
        nucleo_reply.ready = 1u;
        mission_link_handshake_note_remote_reply(&nucleo_reply);
    }
    CHECK(mission_link_handshake_should_send_nucleo_req(1000000u) == 0);
    rc = mission_link_handshake_get_relay_reply(1000000u, &relay);
    CHECK(rc == 1);
    CHECK(relay.board_id == 1u);
    CHECK(relay.ready == 1u);

    /* Fresh board: no reply ever arrives -> timeout fires, synthetic
       ready=0 relay is produced. */
    mission_link_handshake_board_init(0u);
    CHECK(mission_link_handshake_should_send_nucleo_req(0u) == 1); /* seeds the clock at now=0 */
    CHECK(mission_link_handshake_nucleo_timed_out(HANDSHAKE_NUCLEO_TIMEOUT_TICKS - 1u) == 0);
    CHECK(mission_link_handshake_get_relay_reply(HANDSHAKE_NUCLEO_TIMEOUT_TICKS - 1u, &relay) == 0);

    CHECK(mission_link_handshake_nucleo_timed_out(HANDSHAKE_NUCLEO_TIMEOUT_TICKS) == 1);
    /* Sticky. */
    CHECK(mission_link_handshake_nucleo_timed_out(HANDSHAKE_NUCLEO_TIMEOUT_TICKS + 1u) == 1);
    /* No more Reqs into the void. */
    CHECK(mission_link_handshake_should_send_nucleo_req(HANDSHAKE_NUCLEO_TIMEOUT_TICKS + 1u) == 0);

    rc = mission_link_handshake_get_relay_reply(HANDSHAKE_NUCLEO_TIMEOUT_TICKS, &relay);
    CHECK(rc == 1);
    CHECK(relay.board_id == 1u);
    CHECK(relay.ready == 0u); /* synthetic "presumed absent" */

    /* A late real reply after the timeout is still recorded and
       still relayed (real data beats the synthetic placeholder),
       per this file's documented policy. */
    {
        MissionLinkHandshakeReply_t late_reply;
        late_reply.board_id = 1u;
        late_reply.protocol_version = MISSION_LINK_PROTOCOL_VERSION;
        late_reply.ready = 1u;
        mission_link_handshake_note_remote_reply(&late_reply);
    }
    rc = mission_link_handshake_get_relay_reply(HANDSHAKE_NUCLEO_TIMEOUT_TICKS + 500u, &relay);
    CHECK(rc == 1);
    CHECK(relay.ready == 1u);
}

int main(void)
{
    test_req_both_channels_agree();
    test_reply_both_channels_agree();
    test_wrong_magic_is_rejected();
    test_corrupted_checksum_is_rejected();
    test_incomplete_buffer_returns_zero();
    test_local_reply_latch_both_orders();
    test_nucleo_retry_and_timeout_policy();

    if (g_failures == 0)
    {
        printf("handshake_selftest: all checks passed\n");
        return 0;
    }

    printf("handshake_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

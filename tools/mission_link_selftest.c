/*
 * Standalone self-test for core/mission_link.c (no hardware/board
 * required - desktop gcc only, like tools/lora_frame_selftest.c is
 * for lora_frame.c).
 *
 * The thing most likely to silently drift if the payload-only split
 * (mission_link_pack_X() = magic bytes + mission_link_pack_X_payload()
 * + checksum) isn't done cleanly: the two channels this protocol
 * spans - USART1's magic-byte framing and comms_transport/LoRa's
 * sub-type + bare payload - must decode the exact same bytes to the
 * exact same struct contents. This file proves that for all four
 * message types, plus exercises the resync/reject paths every wire
 * format in this codebase is expected to have (checksum corruption,
 * wrong magic bytes, incomplete buffers).
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore \
 *       -o mission_link_selftest tools/mission_link_selftest.c \
 *       core/mission_link.c
 * Run:
 *   ./mission_link_selftest
 */

#include "mission_link.h"
#include "mission_link_codec.h"

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

static MissionLinkRoster_t make_roster(void)
{
    MissionLinkRoster_t r;

    r.team_id = 3u;
    r.specialization = 1u;      /* SPEC_MEDICAL */
    r.equipment_flags = 0x25u;  /* EQ_ROPE | EQ_MEDICAL_KIT | EQ_HAZMAT_SUIT */
    r.fitness_tier = 2u;        /* LIGHT */
    r.loc_x = -1200;
    r.loc_y = 340;
    r.stamina = 87u;

    return r;
}

static MissionLinkIncident_t make_incident(void)
{
    MissionLinkIncident_t inc;

    inc.incident_id = 0xDEADBEEFu;
    inc.loc_x = 512;
    inc.loc_y = -30;
    inc.work_remaining = 4200u;
    inc.priority = 2u;        /* PRIORITY_HIGH */
    inc.incident_type = 3u;   /* INCIDENT_CHEMICAL_EXPOSURE */
    inc.req_capability = 0u;  /* SPEC_SEARCH */
    inc.status = 1u;          /* INC_ASSIGNED */
    inc.victim_count = 7u;
    inc.discovered = 1u;

    return inc;
}

static MissionLinkFault_t make_fault(void)
{
    MissionLinkFault_t f;
    int i;

    f.team_id = 5u;
    for (i = 0; i < NUM_OPERATIONAL_FAULT_CLASSES; i++)
    {
        f.active[i] = (uint8_t)(i % 2);
        f.severity[i] = (uint8_t)(10u * (uint8_t)(i + 1));
    }

    return f;
}

static MissionLinkStatus_t make_status(void)
{
    MissionLinkStatus_t s;

    s.team_id = 7u;
    s.status = 2u; /* STATUS_ON_SCENE */
    s.equipment_level = 64u;

    return s;
}

/* ---------------------------------------------------------------
 * The acceptance-criterion test: a roster message round-trips
 * through both the direct-USART1 path and the sub-type+bare-payload
 * (comms_transport/LoRa) path and produces identical parsed
 * MissionLinkRoster_t contents either way.
 * ---------------------------------------------------------------
 */
static void test_roster_both_channels_agree(void)
{
    MissionLinkRoster_t original = make_roster();

    /* Channel 1: USART1 magic-byte framing. */
    uint8_t usart1_wire[MISSION_LINK_ROSTER_WIRE_SIZE];
    MissionLinkRoster_t via_usart1;
    uint32_t packed_len;
    int rc;

    packed_len = mission_link_pack_roster(&original, usart1_wire);
    CHECK(packed_len == MISSION_LINK_ROSTER_WIRE_SIZE);

    rc = mission_link_parse_roster(usart1_wire, packed_len, &via_usart1);
    CHECK(rc == 1);

    /* Channel 2: comms_transport/LoRa relay - sub-type byte + bare
       payload, no magic bytes, no checksum byte of its own (that's
       lora_frame.h's job on the real hop; this test exercises just
       the mission_link.c codec half of that channel, same as
       mission_link_task.c's relay functions do before handing bytes
       to comms_transport_send()). */
    uint8_t relay_payload[MISSION_LINK_ROSTER_PAYLOAD_SIZE];
    MissionLinkRoster_t via_relay;

    mission_link_pack_roster_payload(&original, relay_payload);
    mission_link_parse_roster_payload(relay_payload, &via_relay);

    /* Both paths must agree with each other AND with the original. */
    CHECK(memcmp(&via_usart1, &original, sizeof(MissionLinkRoster_t)) == 0);
    CHECK(memcmp(&via_relay, &original, sizeof(MissionLinkRoster_t)) == 0);
    CHECK(memcmp(&via_usart1, &via_relay, sizeof(MissionLinkRoster_t)) == 0);
}

/* Same round-trip-parity check for the other three message types,
   since the payload-only split is shared code across all four - if
   it drifts for one type it's likely to drift for all of them, but
   each type's struct layout (different field widths/counts) is worth
   confirming independently rather than assuming roster's pass
   generalizes silently. */
static void test_incident_both_channels_agree(void)
{
    MissionLinkIncident_t original = make_incident();
    uint8_t usart1_wire[MISSION_LINK_INCIDENT_WIRE_SIZE];
    MissionLinkIncident_t via_usart1;
    uint8_t relay_payload[MISSION_LINK_INCIDENT_PAYLOAD_SIZE];
    MissionLinkIncident_t via_relay;
    uint32_t packed_len;
    int rc;

    packed_len = mission_link_pack_incident(&original, usart1_wire);
    CHECK(packed_len == MISSION_LINK_INCIDENT_WIRE_SIZE);

    rc = mission_link_parse_incident(usart1_wire, packed_len, &via_usart1);
    CHECK(rc == 1);

    mission_link_pack_incident_payload(&original, relay_payload);
    mission_link_parse_incident_payload(relay_payload, &via_relay);

    CHECK(memcmp(&via_usart1, &original, sizeof(MissionLinkIncident_t)) == 0);
    CHECK(memcmp(&via_relay, &original, sizeof(MissionLinkIncident_t)) == 0);
}

static void test_fault_both_channels_agree(void)
{
    MissionLinkFault_t original = make_fault();
    uint8_t usart1_wire[MISSION_LINK_FAULT_WIRE_SIZE];
    MissionLinkFault_t via_usart1;
    uint8_t relay_payload[MISSION_LINK_FAULT_PAYLOAD_SIZE];
    MissionLinkFault_t via_relay;
    uint32_t packed_len;
    int rc;

    packed_len = mission_link_pack_fault(&original, usart1_wire);
    CHECK(packed_len == MISSION_LINK_FAULT_WIRE_SIZE);

    rc = mission_link_parse_fault(usart1_wire, packed_len, &via_usart1);
    CHECK(rc == 1);

    mission_link_pack_fault_payload(&original, relay_payload);
    mission_link_parse_fault_payload(relay_payload, &via_relay);

    CHECK(memcmp(&via_usart1, &original, sizeof(MissionLinkFault_t)) == 0);
    CHECK(memcmp(&via_relay, &original, sizeof(MissionLinkFault_t)) == 0);
}

static void test_status_both_channels_agree(void)
{
    MissionLinkStatus_t original = make_status();
    uint8_t usart1_wire[MISSION_LINK_STATUS_WIRE_SIZE];
    MissionLinkStatus_t via_usart1;
    uint8_t relay_payload[MISSION_LINK_STATUS_PAYLOAD_SIZE];
    MissionLinkStatus_t via_relay;
    uint32_t packed_len;
    int rc;

    packed_len = mission_link_pack_status(&original, usart1_wire);
    CHECK(packed_len == MISSION_LINK_STATUS_WIRE_SIZE);

    rc = mission_link_parse_status(usart1_wire, packed_len, &via_usart1);
    CHECK(rc == 1);

    mission_link_pack_status_payload(&original, relay_payload);
    mission_link_parse_status_payload(relay_payload, &via_relay);

    CHECK(memcmp(&via_usart1, &original, sizeof(MissionLinkStatus_t)) == 0);
    CHECK(memcmp(&via_relay, &original, sizeof(MissionLinkStatus_t)) == 0);
}

/* ---------------------------------------------------------------
 * USART1 channel resync/reject behavior, mirroring
 * lora_frame_selftest.c's coverage for the analogous cases.
 * ---------------------------------------------------------------
 */

static void test_wrong_magic_is_rejected(void)
{
    MissionLinkRoster_t original = make_roster();
    uint8_t wire[MISSION_LINK_ROSTER_WIRE_SIZE];
    MissionLinkFault_t out;
    int rc;

    mission_link_pack_roster(&original, wire);

    /* A roster frame's magic bytes must not be mistaken for a fault
       frame - mission_link_parse_fault() should reject it outright
       (-1), not misinterpret roster payload bytes as fault fields. */
    rc = mission_link_parse_fault(wire, sizeof(wire), &out);
    CHECK(rc == -1);
}

static void test_corrupted_checksum_is_rejected(void)
{
    MissionLinkStatus_t original = make_status();
    uint8_t wire[MISSION_LINK_STATUS_WIRE_SIZE];
    MissionLinkStatus_t out;
    int rc;

    mission_link_pack_status(&original, wire);

    /* Flip a bit in the payload, as USART1 line noise or a torn frame
       would - the checksum must catch it. */
    wire[2] ^= 0x01u;

    rc = mission_link_parse_status(wire, sizeof(wire), &out);
    CHECK(rc == -1);
}

static void test_incomplete_buffer_returns_zero(void)
{
    MissionLinkIncident_t original = make_incident();
    uint8_t wire[MISSION_LINK_INCIDENT_WIRE_SIZE];
    MissionLinkIncident_t out;
    int rc;

    mission_link_pack_incident(&original, wire);

    /* One byte short of the full frame - checksum byte hasn't
       "arrived" yet. */
    rc = mission_link_parse_incident(wire, sizeof(wire) - 1u, &out);
    CHECK(rc == 0);

    /* Not even both magic bytes have "arrived" yet. */
    rc = mission_link_parse_incident(wire, 1u, &out);
    CHECK(rc == 0);
}

/* Simulates the resync loop mission_link_task.c's RX side runs: a
   buffer with leading debug text before a valid frame must be
   recoverable by dropping one byte at a time, same policy
   tools/telemetry_bridge.py's TelemetryFrameParser already applies. */
static void test_resync_past_leading_text(void)
{
    MissionLinkRoster_t original = make_roster();
    uint8_t frame[MISSION_LINK_ROSTER_WIRE_SIZE];
    uint8_t stream[16 + MISSION_LINK_ROSTER_WIRE_SIZE];
    uint32_t stream_len = 0u;
    uint32_t offset = 0u;
    int rc;
    MissionLinkRoster_t out;

    mission_link_pack_roster(&original, frame);

    memcpy(stream, "tron: hello\n", 12u);
    stream_len += 12u;
    memcpy(&stream[stream_len], frame, sizeof(frame));
    stream_len += (uint32_t)sizeof(frame);

    while (offset < stream_len)
    {
        rc = mission_link_parse_roster(&stream[offset], stream_len - offset, &out);

        if (rc == 1)
        {
            break;
        }

        if (rc == -1)
        {
            offset++; /* drop one byte, resync, same as telemetry_bridge.py */
            continue;
        }

        /* rc == 0: genuinely not enough bytes left to ever complete a
           frame in this fixed test buffer - that would be a test bug,
           not an expected outcome here. */
        CHECK(0);
        return;
    }

    CHECK(rc == 1);
    CHECK(memcmp(&out, &original, sizeof(MissionLinkRoster_t)) == 0);
}

int main(void)
{
    test_roster_both_channels_agree();
    test_incident_both_channels_agree();
    test_fault_both_channels_agree();
    test_status_both_channels_agree();

    test_wrong_magic_is_rejected();
    test_corrupted_checksum_is_rejected();
    test_incomplete_buffer_returns_zero();
    test_resync_past_leading_text();

    if (g_failures == 0)
    {
        printf("mission_link_selftest: all checks passed\n");
        return 0;
    }

    printf("mission_link_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

#include "mission_link_override.h"
#include "mission_state.h"
#include "comms_transport.h"

/*
 * Implements the codec + apply logic declared in
 * mission_link_override.h. Portable core file: no platform includes
 * (no tm_printf, no stdio, no OS calls), same constraint every other
 * core .c file compiles under (see Makefile's check-board target).
 * Struct fields are copied byte-by-byte via a (const uint8_t *) cast
 * rather than memcpy(), matching the convention core/mission_link.c's
 * own top-of-file comment already establishes for this project: every
 * struct this file touches is __attribute__((packed)) on a
 * little-endian-only target (Cortex-M55 and the x86_64 host tools
 * build both are LE), so a flat byte-for-byte copy is safe and
 * matches the "all fields little-endian" wire contract this project's
 * mission-link-family headers document.
 */

/* Additive checksum over `len` bytes at `bytes`, mod 256 - identical
   scheme to mission_link_checksum() (mission_link.c),
   TELEMETRY_FRAME_CHECKSUM() (telemetry_frame.h), and
   lora_frame_checksum() (lora_frame.c). Kept as this file's own
   private copy rather than shared, same self-contained-per-file
   precedent already established across this project's mission-link
   family (mission_link.c, mission_link_handshake.c each keep their
   own copy too). */
static uint8_t override_checksum(const uint8_t *bytes, uint32_t len)
{
    uint8_t sum = 0u;
    uint32_t i;

    for (i = 0u; i < len; i++)
    {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum;
}

/* ---------------------------------------------------------------
 * Channel 2 core: bare-payload pack/parse
 * ---------------------------------------------------------------
 */

void mission_link_override_pack_payload(const MissionLinkOverride_t *msg,
                                         uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_OVERRIDE_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_override_parse_payload(const uint8_t *payload,
                                          MissionLinkOverride_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_OVERRIDE_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

/* ---------------------------------------------------------------
 * Channel 1: USART1 magic-byte framing
 * ---------------------------------------------------------------
 */

uint32_t mission_link_override_pack(const MissionLinkOverride_t *msg,
                                     uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = MISSION_LINK_OVERRIDE_MAGIC0;
    out_buf[1] = MISSION_LINK_OVERRIDE_MAGIC1;

    mission_link_override_pack_payload(msg, payload_start);

    out_buf[2u + MISSION_LINK_OVERRIDE_PAYLOAD_SIZE] =
        override_checksum(payload_start, MISSION_LINK_OVERRIDE_PAYLOAD_SIZE);

    return MISSION_LINK_OVERRIDE_WIRE_SIZE;
}

int mission_link_override_parse(const uint8_t *buf, uint32_t buf_len,
                                 MissionLinkOverride_t *out)
{
    uint8_t expected_checksum;

    if (buf_len < 2u)
    {
        return 0;
    }

    if (buf[0] != MISSION_LINK_OVERRIDE_MAGIC0 ||
        buf[1] != MISSION_LINK_OVERRIDE_MAGIC1)
    {
        return -1;
    }

    if (buf_len < MISSION_LINK_OVERRIDE_WIRE_SIZE)
    {
        return 0;
    }

    expected_checksum =
        override_checksum(&buf[2], MISSION_LINK_OVERRIDE_PAYLOAD_SIZE);

    if (buf[2u + MISSION_LINK_OVERRIDE_PAYLOAD_SIZE] != expected_checksum)
    {
        return -1;
    }

    mission_link_override_parse_payload(&buf[2], out);

    return 1;
}

/* ---------------------------------------------------------------
 * Apply: sets the CBBA pin core/cbba.c's cbba_apply_override_pin()
 * (called from cbba_build_bundle()) reads on the next allocation
 * pass. See mission_link_override.h's own doc for why this file does
 * NOT check comms_transport_board_id_for_team() itself.
 * ---------------------------------------------------------------
 */
void mission_link_override_apply(const MissionLinkOverride_t *msg)
{
    if (msg->team_id >= NUM_TEAMS)
    {
        /* Malformed or stale frame - ignore, don't crash. */
        return;
    }

    g_cbba_agent_state[msg->team_id].override_active = 1u;
    g_cbba_agent_state[msg->team_id].override_incident_id = msg->incident_id;
}

/* ---------------------------------------------------------------
 * Routing: local apply vs LoRa relay to whichever board hosts
 * msg->team_id. See mission_link_override.h's own doc for the full
 * contract.
 * ---------------------------------------------------------------
 */
int mission_link_override_route_or_apply(const MissionLinkOverride_t *msg)
{
    uint8_t payload[MISSION_LINK_OVERRIDE_PAYLOAD_SIZE];

    if (msg->team_id >= NUM_TEAMS)
    {
        /* Same "ignore, don't crash" policy as mission_link_override_
           apply() - and nothing meaningful to route a team_id this
           large to anyway. */
        return 0;
    }

    if (comms_transport_board_id_for_team(msg->team_id) ==
        g_comms_transport_this_board_id)
    {
        mission_link_override_apply(msg);
        return 0;
    }

    mission_link_override_pack_payload(msg, payload);

    /* sender_team_id = 0u, not a "this came from team 0" claim -
       matches board/STM32N6570-DK/mission_link_task.c's own established
       convention for every other laptop-originated relay (see
       mission_link_route_or_apply_roster()/_fault() there): this
       directive came from the laptop operator over USART1, not from
       any team, and comms_transport.h's own doc says this parameter
       is local bookkeeping only, never placed on the wire, on
       exactly this (relay, not same-board) path - so there's no
       "right" team_id to put here, and 0u is what every sibling
       message type in this project already uses for that same
       situation rather than inventing a different placeholder. */
    return comms_transport_send(0u, msg->team_id,
                                 LORA_FRAME_TYPE_OVERRIDE, payload,
                                 (uint8_t)MISSION_LINK_OVERRIDE_PAYLOAD_SIZE);
}

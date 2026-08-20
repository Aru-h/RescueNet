#include <stddef.h>

#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_state.h"

/*
 * Implements the codec + apply logic declared in mission_link_codec.h.
 * See that header for the two-channel split (USART1 magic-byte frames
 * vs. bare comms_transport/LoRa payloads) and mission_link.h for the
 * wire format itself.
 *
 * Portable core file: no platform includes (no tm_printf, no stdio,
 * no OS calls) - the same constraint every other core .c file compiles
 * under (see Makefile's check-board target). Struct fields are copied
 * byte-by-byte via an (const uint8_t *) cast rather than memcpy(),
 * matching the convention already established in core/lora_frame.c
 * and platform/board/snapshot.c's ring drain (which does exactly this
 * for TelemetryFrame_t) - this project avoids libc calls in
 * freestanding-checked files, and every struct this file touches is
 * __attribute__((packed)) on a little-endian-only target (Cortex-M55
 * and the x86_64 host tools build both are LE), so a flat byte-for-
 * byte copy is safe and matches the "all fields little-endian" wire
 * contract mission_link.h documents.
 */

/*
 * Additive checksum over `len` bytes at `bytes`, mod 256 - identical
 * scheme to TELEMETRY_FRAME_CHECKSUM() (telemetry_frame.h) and
 * lora_frame_checksum() (lora_frame.c). Expressed as a function here
 * for the same reason lora_frame.c does: payload length varies per
 * message type in this file, unlike telemetry_frame.h's fixed 10-byte
 * case.
 */
static uint8_t mission_link_checksum(const uint8_t *bytes, uint32_t len)
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
 * Flat byte-for-byte copies - see top-of-file comment for why a cast
 * + loop is used instead of memcpy(). Every *_payload() pair below is
 * mechanically identical modulo the struct/size involved.
 */

void mission_link_pack_roster_payload(const MissionLinkRoster_t *msg,
                                       uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_ROSTER_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_parse_roster_payload(const uint8_t *payload,
                                        MissionLinkRoster_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_ROSTER_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

void mission_link_pack_incident_payload(const MissionLinkIncident_t *msg,
                                         uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_INCIDENT_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_parse_incident_payload(const uint8_t *payload,
                                          MissionLinkIncident_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_INCIDENT_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

void mission_link_pack_fault_payload(const MissionLinkFault_t *msg,
                                      uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_FAULT_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_parse_fault_payload(const uint8_t *payload,
                                       MissionLinkFault_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_FAULT_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

void mission_link_pack_status_payload(const MissionLinkStatus_t *msg,
                                       uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_STATUS_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_parse_status_payload(const uint8_t *payload,
                                        MissionLinkStatus_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_STATUS_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

/* ---------------------------------------------------------------
 * Channel 1: USART1 magic-byte framing
 * ---------------------------------------------------------------
 * Each pack_*() is: magic0, magic1, payload (via the payload codec
 * above), checksum-over-payload-only. Each parse_*() is the reverse,
 * following lora_frame_try_parse()'s exact 1/0/-1 contract - see
 * mission_link_codec.h for what each return value means.
 */

uint32_t mission_link_pack_roster(const MissionLinkRoster_t *msg,
                                   uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = MISSION_LINK_ROSTER_MAGIC0;
    out_buf[1] = MISSION_LINK_ROSTER_MAGIC1;

    mission_link_pack_roster_payload(msg, payload_start);

    out_buf[2u + MISSION_LINK_ROSTER_PAYLOAD_SIZE] =
        mission_link_checksum(payload_start, MISSION_LINK_ROSTER_PAYLOAD_SIZE);

    return MISSION_LINK_ROSTER_WIRE_SIZE;
}

int mission_link_parse_roster(const uint8_t *buf, uint32_t buf_len,
                               MissionLinkRoster_t *out)
{
    uint8_t expected_checksum;

    if (buf_len < 2u)
    {
        return 0;
    }

    if (buf[0] != MISSION_LINK_ROSTER_MAGIC0 ||
        buf[1] != MISSION_LINK_ROSTER_MAGIC1)
    {
        return -1;
    }

    if (buf_len < MISSION_LINK_ROSTER_WIRE_SIZE)
    {
        return 0;
    }

    expected_checksum = mission_link_checksum(&buf[2], MISSION_LINK_ROSTER_PAYLOAD_SIZE);

    if (buf[2u + MISSION_LINK_ROSTER_PAYLOAD_SIZE] != expected_checksum)
    {
        return -1;
    }

    mission_link_parse_roster_payload(&buf[2], out);

    return 1;
}

uint32_t mission_link_pack_incident(const MissionLinkIncident_t *msg,
                                     uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = MISSION_LINK_INCIDENT_MAGIC0;
    out_buf[1] = MISSION_LINK_INCIDENT_MAGIC1;

    mission_link_pack_incident_payload(msg, payload_start);

    out_buf[2u + MISSION_LINK_INCIDENT_PAYLOAD_SIZE] =
        mission_link_checksum(payload_start, MISSION_LINK_INCIDENT_PAYLOAD_SIZE);

    return MISSION_LINK_INCIDENT_WIRE_SIZE;
}

int mission_link_parse_incident(const uint8_t *buf, uint32_t buf_len,
                                 MissionLinkIncident_t *out)
{
    uint8_t expected_checksum;

    if (buf_len < 2u)
    {
        return 0;
    }

    if (buf[0] != MISSION_LINK_INCIDENT_MAGIC0 ||
        buf[1] != MISSION_LINK_INCIDENT_MAGIC1)
    {
        return -1;
    }

    if (buf_len < MISSION_LINK_INCIDENT_WIRE_SIZE)
    {
        return 0;
    }

    expected_checksum = mission_link_checksum(&buf[2], MISSION_LINK_INCIDENT_PAYLOAD_SIZE);

    if (buf[2u + MISSION_LINK_INCIDENT_PAYLOAD_SIZE] != expected_checksum)
    {
        return -1;
    }

    mission_link_parse_incident_payload(&buf[2], out);

    return 1;
}

uint32_t mission_link_pack_fault(const MissionLinkFault_t *msg,
                                  uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = MISSION_LINK_FAULT_MAGIC0;
    out_buf[1] = MISSION_LINK_FAULT_MAGIC1;

    mission_link_pack_fault_payload(msg, payload_start);

    out_buf[2u + MISSION_LINK_FAULT_PAYLOAD_SIZE] =
        mission_link_checksum(payload_start, MISSION_LINK_FAULT_PAYLOAD_SIZE);

    return MISSION_LINK_FAULT_WIRE_SIZE;
}

int mission_link_parse_fault(const uint8_t *buf, uint32_t buf_len,
                              MissionLinkFault_t *out)
{
    uint8_t expected_checksum;

    if (buf_len < 2u)
    {
        return 0;
    }

    if (buf[0] != MISSION_LINK_FAULT_MAGIC0 ||
        buf[1] != MISSION_LINK_FAULT_MAGIC1)
    {
        return -1;
    }

    if (buf_len < MISSION_LINK_FAULT_WIRE_SIZE)
    {
        return 0;
    }

    expected_checksum = mission_link_checksum(&buf[2], MISSION_LINK_FAULT_PAYLOAD_SIZE);

    if (buf[2u + MISSION_LINK_FAULT_PAYLOAD_SIZE] != expected_checksum)
    {
        return -1;
    }

    mission_link_parse_fault_payload(&buf[2], out);

    return 1;
}

uint32_t mission_link_pack_status(const MissionLinkStatus_t *msg,
                                   uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = MISSION_LINK_STATUS_MAGIC0;
    out_buf[1] = MISSION_LINK_STATUS_MAGIC1;

    mission_link_pack_status_payload(msg, payload_start);

    out_buf[2u + MISSION_LINK_STATUS_PAYLOAD_SIZE] =
        mission_link_checksum(payload_start, MISSION_LINK_STATUS_PAYLOAD_SIZE);

    return MISSION_LINK_STATUS_WIRE_SIZE;
}

int mission_link_parse_status(const uint8_t *buf, uint32_t buf_len,
                               MissionLinkStatus_t *out)
{
    uint8_t expected_checksum;

    if (buf_len < 2u)
    {
        return 0;
    }

    if (buf[0] != MISSION_LINK_STATUS_MAGIC0 ||
        buf[1] != MISSION_LINK_STATUS_MAGIC1)
    {
        return -1;
    }

    if (buf_len < MISSION_LINK_STATUS_WIRE_SIZE)
    {
        return 0;
    }

    expected_checksum = mission_link_checksum(&buf[2], MISSION_LINK_STATUS_PAYLOAD_SIZE);

    if (buf[2u + MISSION_LINK_STATUS_PAYLOAD_SIZE] != expected_checksum)
    {
        return -1;
    }

    mission_link_parse_status_payload(&buf[2], out);

    return 1;
}

/* ---------------------------------------------------------------
 * Apply logic - what a received message does to local board state.
 * ---------------------------------------------------------------
 */

void mission_link_apply_roster(const MissionLinkRoster_t *msg)
{
    TeamState_t *team;

    if (msg->team_id >= NUM_TEAMS)
    {
        /* Out-of-range team_id: nothing on this board to bootstrap.
           Defensive only - mission generation on the laptop side
           guarantees valid team_id values in practice. */
        return;
    }

    team = &g_teams[msg->team_id];

    team->team_id = msg->team_id;
    team->specialization = msg->specialization;
    team->equipment_flags = msg->equipment_flags;
    team->fitness_tier = msg->fitness_tier;
    team->loc_x = msg->loc_x;
    team->loc_y = msg->loc_y;

    /* stamina is quantized to uint8_t (0-100) on the wire (see
       mission_link.h's MissionLinkRoster_t doc); widen back to the
       float TeamState_t.stamina expects. No precision to recover -
       this is exactly what was sent, not a lossy round-trip on this
       side. */
    team->stamina = (float)msg->stamina;
}

/* Finds the local slot already tracking incident_id, if any. Matched
   by incident_id, not array position - this board's g_incidents[]
   slot layout has no reason to mirror the laptop's, since incidents
   arrive here in discovery order over the wire, not in the laptop's
   own allocation order. */
static IncidentState_t *mission_link_find_incident_slot(uint32_t incident_id)
{
    int i;

    for (i = 0; i < NUM_INCIDENTS; i++)
    {
        if (g_incidents[i].slot_occupied && g_incidents[i].incident_id == incident_id)
        {
            return &g_incidents[i];
        }
    }

    return NULL;
}

static IncidentState_t *mission_link_find_free_incident_slot(void)
{
    int i;

    for (i = 0; i < NUM_INCIDENTS; i++)
    {
        if (!g_incidents[i].slot_occupied)
        {
            return &g_incidents[i];
        }
    }

    return NULL;
}

void mission_link_apply_incident(const MissionLinkIncident_t *msg)
{
    IncidentState_t *incident = mission_link_find_incident_slot(msg->incident_id);

    if (incident == NULL)
    {
        incident = mission_link_find_free_incident_slot();

        if (incident == NULL)
        {
            /* Local incident table is full - this board's partial
               view just can't hold this one right now. Not fatal:
               the laptop remains the authoritative source and will
               keep resending on subsequent field changes, same as
               any other dropped/torn frame under this link's
               documented resync policy. */
            return;
        }

        incident->slot_occupied = 1u;
        incident->incident_id = msg->incident_id;

        /* Fields this message doesn't carry (see mission_link.h's
           MissionLinkIncident_t doc for the deliberately-excluded
           list) - sane defaults for a freshly-created local partial
           view, not a claim that these mirror the laptop's real
           values. */
        incident->hazard_flags = 0u;
        incident->reported_at_tick = 0u;
        incident->structure_type = 0u;
        incident->assigned_team = NO_ASSIGNMENT;
        incident->passive_effect = 0u;
        incident->passive_timer = 0u;
        incident->spawned_by = 0u;
        incident->support_team = 0u;
        incident->mission_fault_active_count = 0u;
        incident->mission_fault_original_priority = 0u;
        incident->mission_fault_original_victim_count = 0u;
    }

    incident->loc_x = msg->loc_x;
    incident->loc_y = msg->loc_y;
    incident->work_remaining = msg->work_remaining;
    incident->priority = (Priority_t)msg->priority;
    incident->incident_type = (IncidentType_t)msg->incident_type;
    incident->req_capability = (TeamSpecialization_t)msg->req_capability;
    incident->status = msg->status;
    incident->victim_count = msg->victim_count;
    incident->discovered = msg->discovered;
}

void mission_link_apply_fault(const MissionLinkFault_t *msg)
{
    TeamFaultState_t *fault_state;
    int i;

    if (msg->team_id >= NUM_TEAMS)
    {
        return;
    }

    fault_state = &g_teams[msg->team_id].fault_state;

    for (i = 0; i < NUM_OPERATIONAL_FAULT_CLASSES; i++)
    {
        fault_state->active[i] = msg->active[i];
        fault_state->severity[i] = msg->severity[i];
    }
}

void mission_link_build_status(uint8_t team_id, MissionLinkStatus_t *out)
{
    out->team_id = team_id;

    if (team_id >= NUM_TEAMS)
    {
        /* Defensive default for an out-of-range id - callers are
           expected to only pass local team ids, which are always
           valid, but this keeps *out fully initialized regardless. */
        out->status = STATUS_AVAILABLE;
        out->equipment_level = 0u;
        return;
    }

    out->status = g_teams[team_id].status;
    out->equipment_level = g_teams[team_id].equipment_level;
}

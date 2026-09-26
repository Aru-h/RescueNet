#include <stdbool.h>
#include <stddef.h>

#include "debug.h"          /* debug_trace() - eviction logging only */
#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_state.h"
#include "incident.h"        /* team_effective_work_rate() - see
                                 mission_link_build_status() below */
#include "cbba.h"            /* CBBA_ROUND_TIMEOUT_MS - static-assert
                                 only, see below. Safe to include from a
                                 .c file (unlike mission_link.h itself -
                                 see that header's own comment on why IT
                                 can't include cbba.h): this file has no
                                 downstream includers to cycle through. */
#include "recovery_manager_config.h" /* RECOVERY_MANAGER_*_THRESHOLD/
                                 SCALE constants - see the three
                                 mission_link_*_precedence() functions
                                 at the end of this file, which reuse
                                 these exact thresholds rather than
                                 inventing separate ones. */

/* MISSION_LINK_HEARTBEAT_INTERVAL_MS (mission_link.h) is defined as an
 * independent literal rather than derived from CBBA_ROUND_TIMEOUT_MS by
 * #include, to avoid a circular include from mission_link.h itself (see
 * that header's section 5a comment). This is the check that keeps the
 * two from silently drifting apart: if CBBA_ROUND_TIMEOUT_MS ever
 * changes and this stops being a whole multiple, fix the multiplier in
 * mission_link.h, not this assert. */
_Static_assert(
    (MISSION_LINK_HEARTBEAT_INTERVAL_MS % CBBA_ROUND_TIMEOUT_MS) == 0u,
    "MISSION_LINK_HEARTBEAT_INTERVAL_MS must stay a whole multiple of "
    "CBBA_ROUND_TIMEOUT_MS - see mission_link.h section 5a");

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
 * and board/STM32N6570-DK/snapshot.c's ring drain (which does exactly this
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
 * Mission clock (mission_link.h section 5b)
 * ---------------------------------------------------------------
 */

void mission_link_pack_clock_payload(const MissionLinkClock_t *msg,
                                     uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_CLOCK_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_parse_clock_payload(const uint8_t *payload,
                                      MissionLinkClock_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_CLOCK_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

uint32_t mission_link_pack_clock(const MissionLinkClock_t *msg, uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = MISSION_LINK_CLOCK_MAGIC0;
    out_buf[1] = MISSION_LINK_CLOCK_MAGIC1;

    mission_link_pack_clock_payload(msg, payload_start);

    out_buf[2u + MISSION_LINK_CLOCK_PAYLOAD_SIZE] =
        mission_link_checksum(payload_start, MISSION_LINK_CLOCK_PAYLOAD_SIZE);

    return MISSION_LINK_CLOCK_WIRE_SIZE;
}

int mission_link_parse_clock(const uint8_t *buf, uint32_t buf_len, MissionLinkClock_t *out)
{
    if (buf_len < 2u)
    {
        return 0;
    }

    if (buf[0] != MISSION_LINK_CLOCK_MAGIC0 ||
        buf[1] != MISSION_LINK_CLOCK_MAGIC1)
    {
        return -1;
    }

    if (buf_len < MISSION_LINK_CLOCK_WIRE_SIZE)
    {
        return 0;
    }

    if (buf[2u + MISSION_LINK_CLOCK_PAYLOAD_SIZE] !=
        mission_link_checksum(&buf[2], MISSION_LINK_CLOCK_PAYLOAD_SIZE))
    {
        return -1;
    }

    mission_link_parse_clock_payload(&buf[2], out);

    return 1;
}

/* ---------------------------------------------------------------
 * Route map and sites (mission_link.h sections 5c/5d)
 * ---------------------------------------------------------------
 */

_Static_assert(MISSION_LINK_MAP_WIDTH == GRID_WIDTH && MISSION_LINK_MAP_HEIGHT == GRID_HEIGHT,
               "mission_link.h's map dimensions must match mission_state.h's grid");
_Static_assert(MISSION_LINK_MAX_SAFE_ZONES == MAX_SAFE_ZONES &&
               MISSION_LINK_NUM_GATEWAYS == NUM_EVACUATION_GATEWAYS,
               "mission_link.h's site counts must match mission_state.h");

void mission_link_pack_map_row_payload(const MissionLinkMapRow_t *msg, uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_MAP_ROW_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_parse_map_row_payload(const uint8_t *payload, MissionLinkMapRow_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_MAP_ROW_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

uint32_t mission_link_pack_map_row(const MissionLinkMapRow_t *msg, uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = MISSION_LINK_MAP_ROW_MAGIC0;
    out_buf[1] = MISSION_LINK_MAP_ROW_MAGIC1;
    mission_link_pack_map_row_payload(msg, payload_start);
    out_buf[2u + MISSION_LINK_MAP_ROW_PAYLOAD_SIZE] =
        mission_link_checksum(payload_start, MISSION_LINK_MAP_ROW_PAYLOAD_SIZE);

    return MISSION_LINK_MAP_ROW_WIRE_SIZE;
}

int mission_link_parse_map_row(const uint8_t *buf, uint32_t buf_len, MissionLinkMapRow_t *out)
{
    if (buf_len < 2u)
    {
        return 0;
    }
    if (buf[0] != MISSION_LINK_MAP_ROW_MAGIC0 || buf[1] != MISSION_LINK_MAP_ROW_MAGIC1)
    {
        return -1;
    }
    if (buf_len < MISSION_LINK_MAP_ROW_WIRE_SIZE)
    {
        return 0;
    }
    if (buf[2u + MISSION_LINK_MAP_ROW_PAYLOAD_SIZE] !=
        mission_link_checksum(&buf[2], MISSION_LINK_MAP_ROW_PAYLOAD_SIZE))
    {
        return -1;
    }
    mission_link_parse_map_row_payload(&buf[2], out);
    return 1;
}

void mission_link_pack_sites_payload(const MissionLinkSites_t *msg, uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_SITES_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_parse_sites_payload(const uint8_t *payload, MissionLinkSites_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_SITES_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

uint32_t mission_link_pack_sites(const MissionLinkSites_t *msg, uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = MISSION_LINK_SITES_MAGIC0;
    out_buf[1] = MISSION_LINK_SITES_MAGIC1;
    mission_link_pack_sites_payload(msg, payload_start);
    out_buf[2u + MISSION_LINK_SITES_PAYLOAD_SIZE] =
        mission_link_checksum(payload_start, MISSION_LINK_SITES_PAYLOAD_SIZE);

    return MISSION_LINK_SITES_WIRE_SIZE;
}

int mission_link_parse_sites(const uint8_t *buf, uint32_t buf_len, MissionLinkSites_t *out)
{
    if (buf_len < 2u)
    {
        return 0;
    }
    if (buf[0] != MISSION_LINK_SITES_MAGIC0 || buf[1] != MISSION_LINK_SITES_MAGIC1)
    {
        return -1;
    }
    if (buf_len < MISSION_LINK_SITES_WIRE_SIZE)
    {
        return 0;
    }
    if (buf[2u + MISSION_LINK_SITES_PAYLOAD_SIZE] !=
        mission_link_checksum(&buf[2], MISSION_LINK_SITES_PAYLOAD_SIZE))
    {
        return -1;
    }
    mission_link_parse_sites_payload(&buf[2], out);
    return 1;
}

uint8_t mission_link_map_encode_cell(uint8_t navigable, uint8_t travel_cost)
{
    if (!navigable)
    {
        return MISSION_LINK_MAP_BLOCKED;
    }
    /* A passable cell must never encode as "blocked". */
    return (travel_cost == 0u) ? 1u : travel_cost;
}

/* The board's copy of the laptop's route map - 1296 bytes, plus which
   rows have arrived. */
static uint8_t g_mission_link_map[MISSION_LINK_MAP_WIDTH][MISSION_LINK_MAP_HEIGHT];
static uint8_t g_mission_link_map_row_known[MISSION_LINK_MAP_HEIGHT];
static uint32_t g_mission_link_map_rows_received;
static uint8_t g_mission_link_sites_received;

uint8_t mission_link_apply_map_row(const MissionLinkMapRow_t *msg)
{
    uint32_t x;

    if (msg->row >= MISSION_LINK_MAP_HEIGHT)
    {
        return 0u;
    }

    for (x = 0u; x < MISSION_LINK_MAP_WIDTH; x++)
    {
        g_mission_link_map[x][msg->row] = msg->cells[x];
    }

    if (!g_mission_link_map_row_known[msg->row])
    {
        g_mission_link_map_row_known[msg->row] = 1u;
        g_mission_link_map_rows_received++;
    }

    return 1u;
}

uint8_t mission_link_map_cell(int x, int y, uint8_t *cell_out)
{
    if (x < 0 || y < 0 || x >= (int)MISSION_LINK_MAP_WIDTH || y >= (int)MISSION_LINK_MAP_HEIGHT ||
        !g_mission_link_map_row_known[y])
    {
        return 0u;
    }

    *cell_out = g_mission_link_map[x][y];
    return 1u;
}

uint32_t mission_link_map_rows_received(void)
{
    return g_mission_link_map_rows_received;
}

uint8_t mission_link_sites_received(void)
{
    return g_mission_link_sites_received;
}

void mission_link_map_reset(void)
{
    uint32_t y;

    for (y = 0u; y < MISSION_LINK_MAP_HEIGHT; y++)
    {
        g_mission_link_map_row_known[y] = 0u;
    }
    g_mission_link_map_rows_received = 0u;
    g_mission_link_sites_received = 0u;
}

uint8_t mission_link_apply_sites(const MissionLinkSites_t *msg)
{
    uint32_t i;

    if (msg->num_safe_zones > MISSION_LINK_MAX_SAFE_ZONES)
    {
        return 0u;
    }
    for (i = 0u; i < msg->num_safe_zones; i++)
    {
        const MissionLinkSafeZoneSite_t *site = &msg->safe_zones[i];

        if (site->loc_x < 0 || site->loc_x >= GRID_WIDTH || site->loc_y < 0 || site->loc_y >= GRID_HEIGHT)
        {
            return 0u;
        }
    }
    for (i = 0u; i < MISSION_LINK_NUM_GATEWAYS; i++)
    {
        if (msg->gateway_x[i] < 0 || msg->gateway_x[i] >= GRID_WIDTH ||
            msg->gateway_y[i] < 0 || msg->gateway_y[i] >= GRID_HEIGHT)
        {
            return 0u;
        }
    }

    for (i = 0u; i < msg->num_safe_zones; i++)
    {
        g_safezones[i].safezone_id = (uint8_t)i;
        g_safezones[i].loc_x = msg->safe_zones[i].loc_x;
        g_safezones[i].loc_y = msg->safe_zones[i].loc_y;
        g_safezones[i].capacity = msg->safe_zones[i].capacity;
    }
    g_num_safe_zones = msg->num_safe_zones;

    for (i = 0u; i < MISSION_LINK_NUM_GATEWAYS; i++)
    {
        g_evacuation_gateways[i].gateway_id = (uint8_t)i;
        g_evacuation_gateways[i].loc_x = msg->gateway_x[i];
        g_evacuation_gateways[i].loc_y = msg->gateway_y[i];
    }

    g_mission_link_sites_received = 1u;
    return 1u;
}

static uint32_t g_mission_link_authorized_tick;
static uint8_t g_mission_link_clock_received;

void mission_link_apply_clock(const MissionLinkClock_t *msg)
{
    g_mission_link_authorized_tick = msg->tick;
    g_mission_link_clock_received = 1u;
}

uint8_t mission_link_clock_received(void)
{
    return g_mission_link_clock_received;
}

uint32_t mission_link_authorized_tick(void)
{
    return g_mission_link_authorized_tick;
}

void mission_link_clock_reset(void)
{
    g_mission_link_authorized_tick = 0u;
    g_mission_link_clock_received = 0u;
}

uint8_t mission_link_clock_step_plan(uint32_t local_tick, uint32_t authorized_tick,
                                     uint32_t *start_tick)
{
    if (local_tick >= authorized_tick)
    {
        return 0u;
    }

    if (authorized_tick - local_tick > MISSION_LINK_CLOCK_MAX_CATCHUP_TICKS)
    {
        *start_tick = authorized_tick - 1u;
    }
    else
    {
        *start_tick = local_tick;
    }

    return 1u;
}

/* ---------------------------------------------------------------
 * Board-side incident cache (BoardIncidentCache_t, mission_state.h)
 * ---------------------------------------------------------------
 * Local to this file - nothing outside mission_link.c needs the raw
 * struct, only the board_incident_cache_get()/put()/remove() API
 * declared in mission_link.h. Zero-initialized at startup, same as
 * every other file-scope object in this project (no explicit init
 * call needed: in_use == 0 for every slot until the first put()).
 */
static BoardIncidentCache_t g_board_incident_cache;

MissionLinkIncident_t *board_incident_cache_get(uint32_t incident_id)
{
    uint32_t i;

    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        if (g_board_incident_cache.slots[i].in_use &&
            g_board_incident_cache.slots[i].incident.incident_id == incident_id)
        {
            return &g_board_incident_cache.slots[i].incident;
        }
    }

    return NULL;
}

/* Finds the in-use slot holding the cached incident with the
   furthest/lowest priority - i.e. the numerically HIGHEST Priority_t
   value (PRIORITY_LOW == 4 is further/lower priority than
   PRIORITY_CRITICAL == 1; see mission_state.h). This is the same
   ordering IncidentState_t.priority already carries laptop-side -
   nothing new invented here, just read off the existing enum. Ties
   keep the first (lowest-index) match, which is as good as any other
   tiebreak absent a documented one and keeps this deterministic. */
static uint32_t board_incident_cache_find_eviction_index(void)
{
    uint32_t worst_index = 0u;
    uint8_t worst_priority = 0u;
    uint32_t i;

    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        if (g_board_incident_cache.slots[i].in_use &&
            g_board_incident_cache.slots[i].incident.priority > worst_priority)
        {
            worst_priority = g_board_incident_cache.slots[i].incident.priority;
            worst_index = i;
        }
    }

    return worst_index;
}

void board_incident_cache_put(const MissionLinkIncident_t *inc)
{
    uint32_t i;
    uint32_t target_index;
    int have_target = 0;

    /* Update-in-place if already cached. */
    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        if (g_board_incident_cache.slots[i].in_use &&
            g_board_incident_cache.slots[i].incident.incident_id == inc->incident_id)
        {
            g_board_incident_cache.slots[i].incident = *inc;
            return;
        }
    }

    /* Not present - insert into the first free slot, if any. */
    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        if (!g_board_incident_cache.slots[i].in_use)
        {
            target_index = i;
            have_target = 1;
            break;
        }
    }

    /* Cache is full: evict the furthest/lowest-priority incident to
       make room. Logged unconditionally (not gated behind a trace
       flag like simulation.c's per-tick trace calls) since eviction
       only happens under real load-spike pressure, not every tick -
       silent eviction here would be a genuinely confusing bug to
       chase later, exactly the scenario this line exists for. */
    if (!have_target)
    {
        target_index = board_incident_cache_find_eviction_index();

        debug_trace("mission_link: incident cache full, evicting incident_id=%lu "
                    "(priority=%u) for incident_id=%lu (priority=%u)\n",
                    (unsigned long)g_board_incident_cache.slots[target_index].incident.incident_id,
                    (unsigned)g_board_incident_cache.slots[target_index].incident.priority,
                    (unsigned long)inc->incident_id,
                    (unsigned)inc->priority);
    }

    g_board_incident_cache.slots[target_index].incident = *inc;
    g_board_incident_cache.slots[target_index].in_use = 1u;
}

void board_incident_cache_remove(uint32_t incident_id)
{
    uint32_t i;

    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        if (g_board_incident_cache.slots[i].in_use &&
            g_board_incident_cache.slots[i].incident.incident_id == incident_id)
        {
            g_board_incident_cache.slots[i].in_use = 0u;
            return;
        }
    }
}

/* ---------------------------------------------------------------
 * Apply logic - what a received message does to local board state.
 * ---------------------------------------------------------------
 */

/* Counts whole-message rejects below. Both reject paths are silent by
   design (no logging in portable core/), which leaves a board that gates
   its boot on mission_link_task_any_roster_applied() unable to tell "a
   roster was rejected" apart from "no roster ever arrived" - the two look
   identical from outside, and both boards' roster-wait loops block forever
   on either. This counter is what lets platform code surface that
   difference; nothing in core/ acts on it. */
static uint32_t g_roster_reject_count;

uint32_t mission_link_roster_reject_count(void)
{
    return g_roster_reject_count;
}

void mission_link_apply_roster(const MissionLinkRoster_t *msg)
{
    TeamState_t *team;

    if (msg->team_id >= NUM_TEAMS)
    {
        /* Out-of-range team_id: nothing on this board to bootstrap.
           Defensive only - mission generation on the laptop side
           guarantees valid team_id values in practice. */
        g_roster_reject_count++;
        return;
    }

    if (msg->loc_x < 0 || msg->loc_x >= GRID_WIDTH ||
        msg->loc_y < 0 || msg->loc_y >= GRID_HEIGHT)
    {
        /* Out-of-range location: same defensive-only posture and same
           whole-message reject as the team_id check above (mission
           generation on the laptop side guarantees valid loc_x/loc_y in
           practice) - added because loc_x/loc_y is written straight to
           team->loc_x/loc_y below with no other validation anywhere
           downstream, and every core/path.c pathfinding call keys off
           team->loc_x/loc_y as its start coordinate. Without this, a
           corrupted roster frame (bit-flip surviving the checksum, or
           any future bug upstream) would silently become an out-of-
           bounds array index the first time this team tries to path
           anywhere. */
        g_roster_reject_count++;
        return;
    }

    if (msg->specialization >= NUM_TEAM_SPECIALIZATIONS ||
        msg->fitness_tier > (uint8_t)LIGHT)
    {
        /* Same whole-message reject (review finding H4): specialization
           indexes CAPABILITY_MATRIX (core/scoring.c) and the per-
           specialization diagnostics, and fitness_tier selects the
           FITNESS_* speed/stamina multipliers - only ARDUOUS/MODERATE/
           LIGHT exist. */
        g_roster_reject_count++;
        return;
    }

    if (msg->equipment_level > EQ_MAX_LEVEL)
    {
        /* Same whole-message reject: a level above the gauge's maximum
           can only be corruption, and the work-rate/abort logic in
           core/team.c and core/incident.c assumes 0-EQ_MAX_LEVEL. */
        g_roster_reject_count++;
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
    team->equipment_level = msg->equipment_level;
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

/* Counts incidents that could not be held because the local table was
   full: an incoming incident dropped for want of a slot, plus every
   incident evicted to make room for a more urgent one. Silent by design
   in portable core/ (no logging), same reasoning as g_roster_reject_
   count above - this is what makes the loss visible. */
static uint32_t g_incident_drop_count;

uint32_t mission_link_incident_drop_count(void)
{
    return g_incident_drop_count;
}

/* When the table is full, the slot of the incident it is safe to give up
   for msg, or NULL. Only an incident nobody is working is a candidate:
   OPEN, no assigned or support team, and no team on this board heading
   for it. Among those, the least urgent (numerically highest Priority_t,
   PRIORITY_CRITICAL == 1 is the most urgent) goes, and only if msg is
   STRICTLY more urgent - so a steady stream of equal-priority re-sends
   can never churn the table. Ties are broken by incident_id (the
   newest, highest id goes), never by slot position: both boards see the
   same incident frames but lay their tables out differently, and the
   choice must depend only on what they both hold, so they evict the
   same incident for the same input (review finding H3). */
static IncidentState_t *mission_link_find_eviction_victim(const MissionLinkIncident_t *msg)
{
    IncidentState_t *victim = NULL;
    int i;

    for (i = 0; i < NUM_INCIDENTS; i++)
    {
        IncidentState_t *candidate = &g_incidents[i];
        uint8_t t;
        uint8_t in_use = 0u;

        if (!candidate->slot_occupied ||
            candidate->status != INC_OPEN ||
            candidate->assigned_team != NO_ASSIGNMENT ||
            candidate->support_team != NO_ASSIGNMENT)
        {
            continue;
        }

        for (t = 0u; t < NUM_TEAMS; t++)
        {
            if (g_teams[t].assigned_task_pool == TASK_POOL_INCIDENT &&
                g_teams[t].assigned_task_slot == (uint8_t)i)
            {
                in_use = 1u;
                break;
            }
        }
        if (in_use)
        {
            continue;
        }

        if (victim == NULL ||
            (uint8_t)candidate->priority > (uint8_t)victim->priority ||
            ((uint8_t)candidate->priority == (uint8_t)victim->priority &&
             candidate->incident_id > victim->incident_id))
        {
            victim = candidate;
        }
    }

    if (victim == NULL || msg->priority >= (uint8_t)victim->priority)
    {
        return NULL;
    }

    return victim;
}

/*
 * Who owns which incident field once the laptop mirrors board reports
 * (see mission_link.h's MissionLinkStatus_t doc):
 *
 *   laptop  creation, location, priority, type, victims, discovery, the
 *           stage (req_capability plus that stage's starting work) and
 *           resolution;
 *   board   execution - which team has it (assigned_team), OPEN ->
 *           ASSIGNED -> IN_PROGRESS, and work_remaining while a team is
 *           on it.
 *
 * The laptop re-sends incidents periodically, and its copy of an
 * execution field can be a report or two behind. Copying its status and
 * work_remaining over a board's live assignment would turn an incident
 * a team is standing on back to OPEN (CBBA would hand it out again) and
 * undo the work done since the last report. So for an incident this
 * board already has:
 *
 *   RESOLVED       the laptop closed it (work finished on the last
 *                  stage, a sweep met its coverage, ...): release the
 *                  teams on it and free the slot.
 *   new stage      req_capability changed: the laptop branched the
 *                  incident (complete_incident_stage() runs there, not
 *                  here). Take every field; any team still on the old
 *                  stage is released.
 *   same stage     take the laptop's descriptive fields; keep status,
 *                  assigned_team and work_remaining while the incident
 *                  is ASSIGNED or IN_PROGRESS here. If the location
 *                  moved (a search sweep's next frontier cell) and a
 *                  team was already on scene, it heads for the new
 *                  cell.
 */
/* core/cbba.c - public but not declared in cbba.h (see its comment). */
extern bool cbba_team_is_local(uint8_t team_id);

static void mission_link_retarget_on_scene_teams(IncidentState_t *incident)
{
    uint8_t slot = (uint8_t)(incident - g_incidents);
    uint8_t t;

    for (t = 0u; t < NUM_TEAMS; t++)
    {
        TeamState_t *team = &g_teams[t];

        if (team->assigned_task_pool == TASK_POOL_INCIDENT &&
            team->assigned_task_slot == slot &&
            team->status == STATUS_ON_SCENE)
        {
            team->status = STATUS_EN_ROUTE;
        }
    }

    if (incident->status == INC_IN_PROGRESS)
    {
        incident->status = INC_ASSIGNED;
    }
}

/* Counts incident frames mission_link_apply_incident() rejected whole as
   out of range - see mission_link_incident_is_valid(). Same "make a
   silent reject visible" purpose as g_roster_reject_count. */
static uint32_t g_incident_reject_count;

uint32_t mission_link_incident_reject_count(void)
{
    return g_incident_reject_count;
}

/* Every field of an incident frame that is later used as an array index
   or a grid coordinate, checked against its real range (review finding
   H4). USART1 frames carry only an 8-bit additive checksum - roughly one
   corrupted frame in 256 still passes it - and the LoRa relay is only as
   trustworthy as its public default PSK, so these are not "can't
   happen" values:
     loc_x/loc_y     grid, path and hazard lookups
     incident_type   g_diag_*_by_type[NUM_INCIDENT_TYPES], archetypes
     req_capability  CAPABILITY_MATRIX and the *_by_spec diagnostics
     priority        Priority_t is 1..4; 0 or >4 breaks every
                     urgency comparison (and H3's eviction choice)
     status          IncidentStatus_t is 0..INC_RESOLVED */
static uint8_t mission_link_incident_is_valid(const MissionLinkIncident_t *msg)
{
    return msg->loc_x >= 0 && msg->loc_x < GRID_WIDTH &&
           msg->loc_y >= 0 && msg->loc_y < GRID_HEIGHT &&
           msg->incident_type < (uint8_t)NUM_INCIDENT_TYPES &&
           msg->req_capability < (uint8_t)NUM_TEAM_SPECIALIZATIONS &&
           msg->priority >= (uint8_t)PRIORITY_CRITICAL &&
           msg->priority <= (uint8_t)PRIORITY_LOW &&
           msg->status <= (uint8_t)INC_RESOLVED;
}

void mission_link_apply_incident(const MissionLinkIncident_t *msg)
{
    IncidentState_t *incident;

    if (!mission_link_incident_is_valid(msg))
    {
        /* Whole-message reject: a frame with one impossible field can't
           be trusted for the rest either. The laptop re-sends open
           incidents periodically, so a real update lost to corruption
           arrives again. */
        g_incident_reject_count++;
        return;
    }

    incident = mission_link_find_incident_slot(msg->incident_id);

    if (incident != NULL)
    {
        /* Only a team hosted HERE makes this board the owner of the
           incident's execution state; an ASSIGNED status copied from
           the laptop (another board's team has it) is not. */
        uint8_t executing = ((incident->status == INC_ASSIGNED ||
                              incident->status == INC_IN_PROGRESS) &&
                             incident->assigned_team < NUM_TEAMS &&
                             cbba_team_is_local(incident->assigned_team));

        if (msg->status == (uint8_t)INC_RESOLVED)
        {
            incident_close_for_authority(incident);
            board_incident_cache_remove(msg->incident_id);
            return;
        }

        if ((uint8_t)incident->req_capability != msg->req_capability)
        {
            if (executing)
            {
                incident_release_teams_for_authority(incident);
            }
            executing = 0u;
        }

        if (executing)
        {
            uint8_t moved = (incident->loc_x != msg->loc_x ||
                             incident->loc_y != msg->loc_y);

            incident->loc_x = msg->loc_x;
            incident->loc_y = msg->loc_y;
            incident->priority = (Priority_t)msg->priority;
            incident->incident_type = (IncidentType_t)msg->incident_type;
            incident->victim_count = msg->victim_count;
            incident->discovered = msg->discovered;

            if (moved)
            {
                mission_link_retarget_on_scene_teams(incident);
            }

            {
                /* Cache view: the laptop's descriptive fields, this
                   board's own execution state. */
                MissionLinkIncident_t cached = *msg;
                cached.status = (uint8_t)incident->status;
                cached.work_remaining = incident->work_remaining;
                board_incident_cache_put(&cached);
            }
            return;
        }
    }

    if (incident == NULL)
    {
        if (msg->status == (uint8_t)INC_RESOLVED)
        {
            /* Closed before this board ever saw it (or a re-send of a
               resolution this board already applied): nothing to do,
               and no reason to spend a slot on it. */
            board_incident_cache_remove(msg->incident_id);
            return;
        }

        incident = mission_link_find_free_incident_slot();

        if (incident == NULL && msg->discovered)
        {
            /* Local incident table is full. Make room if this one is
               more urgent than something nobody here is working - see
               mission_link_find_eviction_victim(). An undiscovered
               incident never evicts anything: no team can accept it
               (team_can_accept_incident()), so it isn't worth a slot
               someone could use. */
            IncidentState_t *victim = mission_link_find_eviction_victim(msg);

            if (victim != NULL)
            {
                board_incident_cache_remove(victim->incident_id);
                incident_close_for_authority(victim);
                g_incident_drop_count++;
                incident = victim;
            }
        }

        if (incident == NULL)
        {
            /* Still no room - this board's partial view just can't hold
               this one right now. Not fatal: the laptop remains the
               authoritative source and keeps re-sending open incidents
               (telemetry_bridge.py's refresh), so it lands once a slot
               frees up, same as any other dropped/torn frame under this
               link's documented resync policy. Counted, not silent. */
            g_incident_drop_count++;
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
        incident->support_team = NO_ASSIGNMENT;
        incident->mission_fault_active_count = 0u;
        incident->mission_fault_original_priority = 0u;
        incident->mission_fault_original_victim_count = 0u;

        /* Building and floor aren't on the wire. The first frame for an
           incident carries its untouched first-stage work, which already
           includes the floor scaling, so later stages this board runs
           reset to that same amount. An incident first seen part-way
           through falls back to the ground-floor amount. */
        incident->building_id = INCIDENT_NO_BUILDING;
        incident->floor = 0u;
        incident->stage_work =
            msg->work_remaining > INCIDENT_BASE_STAGE_WORK
                ? msg->work_remaining
                : (uint16_t)INCIDENT_BASE_STAGE_WORK;
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

    /* Keep the board's lean incident cache (mission_state.h's
       BoardIncidentCache_t) in sync with the same frame that just
       updated g_incidents[] above - this is the "incoming MissionLink
       Incident_t frames get unpacked" point both consumers share.
       Cache the latest view first, then immediately drop it again if
       this frame reports the incident resolved: no reason to hold a
       finished incident in a 15-slot cache and let it consume a slot
       (or trigger an eviction) until eviction pressure eventually
       gets around to it. */
    board_incident_cache_put(msg);

    if (msg->status == (uint8_t)INC_RESOLVED)
    {
        board_incident_cache_remove(msg->incident_id);
    }
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

    /* Normalized rather than rejected (review finding H4): active[] is a
       flag every reader tests for non-zero, so any non-zero byte means
       "active"; severity feeds arithmetic (work-rate cuts, stamina and
       equipment drain, movement skipping, the CBBA communication
       cadence) rather than indexing anything, so a corrupted value is
       clamped to MISSION_LINK_FAULT_SEVERITY_MAX instead of dropping the
       whole fault update - losing a real fault onset would be worse than
       applying one at bounded strength. */
    for (i = 0; i < NUM_OPERATIONAL_FAULT_CLASSES; i++)
    {
        uint8_t severity = msg->severity[i];

        if (severity > MISSION_LINK_FAULT_SEVERITY_MAX)
        {
            severity = MISSION_LINK_FAULT_SEVERITY_MAX;
        }

        fault_state->active[i] = (msg->active[i] != 0u) ? 1u : 0u;
        fault_state->severity[i] = severity;
    }
}

/*
 * Finds the laptop-issued incident (g_incidents[]) team_id is currently
 * assigned to, if any - the same pool/slot resolution pattern used at
 * every other assigned_task_pool/assigned_task_slot read site in this
 * project (see e.g. core/team.c's update_team() STATUS_ON_SCENE case,
 * core/simulation.c's move_team()). Returns NULL for TASK_POOL_NONE, a
 * board-local system task (see below) or a stale/cleared slot - all
 * mean "nothing to anchor this report to",
 * which mission_link_build_status() below reports via
 * MISSION_LINK_NO_INCIDENT rather than treating as an error.
 */
static const IncidentState_t *mission_link_team_current_task(const TeamState_t *team)
{
    if (team->assigned_task_pool == TASK_POOL_INCIDENT &&
        team->assigned_task_slot < NUM_INCIDENTS)
    {
        const IncidentState_t *task = &g_incidents[team->assigned_task_slot];
        return task->slot_occupied ? task : NULL;
    }

    /* A system task here is board-local (equipment cache, evacuee run):
       its id comes from this board's own counter and can collide with a
       laptop incident id, so it is not reported - see mission_link.h's
       MissionLinkStatus_t doc (schema v3). Laptop-issued work, search
       sweeps included, always lives in g_incidents[] on a board. */
    return NULL;
}

void mission_link_build_status(uint8_t team_id, MissionLinkStatus_t *out)
{
    const TeamState_t *team;
    const IncidentState_t *task;

    out->team_id = team_id;

    if (team_id >= NUM_TEAMS)
    {
        /* Defensive default for an out-of-range id - callers are
           expected to only pass local team ids, which are always
           valid, but this keeps *out fully initialized regardless. */
        out->status = STATUS_AVAILABLE;
        out->equipment_level = 0u;
        out->assigned_incident_id = MISSION_LINK_NO_INCIDENT;
        out->work_remaining_at_report = 0u;
        out->work_rate = 0u;
        out->stamina_at_report = 0u;
        out->report_tick = g_current_tick;
        out->loc_x = 0;
        out->loc_y = 0;
        return;
    }

    team = &g_teams[team_id];
    task = mission_link_team_current_task(team);

    out->status = team->status;
    out->equipment_level = team->equipment_level;
    out->loc_x = team->loc_x;
    out->loc_y = team->loc_y;

    /* Anchor snapshot - see mission_link.h's MissionLinkStatus_t doc
       (schema v2) for why these are anchor values, not per-tick
       deltas, and why work_rate is board-computed here rather than
       left for the laptop to re-derive. team_effective_work_rate() is
       the exact same function update_incident_work() (core/incident.c)
       applies for this team's own work if it's a board-hosted team -
       sharing the one function is what keeps this report and the
       board's own snapshot log from ever silently disagreeing about
       what was actually applied. Queried unconditionally (even for an
       idle team) since the field documents "currently-effective rate",
       not "rate currently being applied to something". */
    out->work_rate = team_effective_work_rate(team);
    out->report_tick = g_current_tick;

    /* stamina is quantized to uint8_t (0-100) on the wire, same
       convention as MissionLinkRoster_t.stamina - this is that
       field's ongoing counterpart (see mission_link.h). TeamState_t.
       stamina is already clamped to [MIN_STAMINA, MAX_STAMINA] == [0,
       100] by team.c's change_stamina(), so a direct truncating cast
       is safe here without an extra clamp. */
    out->stamina_at_report = (uint8_t)team->stamina;

    if (task != NULL)
    {
        out->assigned_incident_id = task->incident_id;
        out->work_remaining_at_report = task->work_remaining;
    }
    else
    {
        out->assigned_incident_id = MISSION_LINK_NO_INCIDENT;
        out->work_remaining_at_report = 0u;
    }
}

/*
 * ---------------------------------------------------------------
 * Comms precedence classifiers - see the matching declarations in
 * mission_link.h (next to each struct) for the full reasoning behind
 * each cutoff. Grouped here, at the end of the file, rather than
 * next to each struct's other helpers above, since these three are a
 * single cohesive concern (message routing urgency) distinct from
 * the pack/apply/relay logic the rest of this file implements.
 * ---------------------------------------------------------------
 */

CommsPrecedence_t mission_link_incident_precedence(const MissionLinkIncident_t *msg)
{
    if (msg->priority == (uint8_t)PRIORITY_CRITICAL && msg->discovered != 0u)
    {
        return COMMS_PRECEDENCE_FLASH;
    }

    return COMMS_PRECEDENCE_PRIORITY;
}

CommsPrecedence_t mission_link_fault_precedence(const MissionLinkFault_t *msg)
{
    if (msg->active[FAULT_CLASS_COMMUNICATION] &&
        msg->severity[FAULT_CLASS_COMMUNICATION] >= RECOVERY_MANAGER_COMMUNICATION_SEVERE_THRESHOLD)
    {
        return COMMS_PRECEDENCE_IMMEDIATE;
    }

    /* RECOVERY_MANAGER_MOBILITY_SEVERITY_SCALE used to be the divisor
       evaluate_mobility() applied to the injected severity; that read
       was removed (it leaked ground truth into detection), so it now
       only sets this radio-precedence cut-off. This decides how
       urgently the fault frame itself is relayed, not whether the
       recovery pipeline reacts - see MissionLinkFault_t's doc in
       mission_link.h. */
    if (msg->active[FAULT_CLASS_MOBILITY] &&
        (float)msg->severity[FAULT_CLASS_MOBILITY] >= RECOVERY_MANAGER_MOBILITY_SEVERITY_SCALE)
    {
        return COMMS_PRECEDENCE_IMMEDIATE;
    }

    return COMMS_PRECEDENCE_PRIORITY;
}

CommsPrecedence_t mission_link_status_precedence(const MissionLinkStatus_t *msg)
{
    if ((float)msg->stamina_at_report <= RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD ||
        (uint32_t)msg->equipment_level <= RECOVERY_MANAGER_EQUIPMENT_CRITICAL_THRESHOLD)
    {
        return COMMS_PRECEDENCE_FLASH;
    }

    return COMMS_PRECEDENCE_ROUTINE;
}

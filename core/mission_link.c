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
        incident->support_team = NO_ASSIGNMENT;
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

    for (i = 0; i < NUM_OPERATIONAL_FAULT_CLASSES; i++)
    {
        fault_state->active[i] = msg->active[i];
        fault_state->severity[i] = msg->severity[i];
    }
}

/*
 * Finds the IncidentState_t (either pool) team_id is currently assigned
 * to, if any - the same pool/slot resolution pattern already used at
 * every other assigned_task_pool/assigned_task_slot read site in this
 * project (see e.g. core/team.c's update_team() STATUS_ON_SCENE case,
 * core/simulation.c's move_team()). Returns NULL for TASK_POOL_NONE or
 * a stale/cleared slot - both mean "nothing to anchor this report to",
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

    if (team->assigned_task_pool == TASK_POOL_SYSTEM &&
        team->assigned_task_slot < NUM_SYSTEM_TASKS)
    {
        const IncidentState_t *task = &g_system_tasks[team->assigned_task_slot];
        return task->slot_occupied ? task : NULL;
    }

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
        return;
    }

    team = &g_teams[team_id];
    task = mission_link_team_current_task(team);

    out->status = team->status;
    out->equipment_level = team->equipment_level;

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

    /* RECOVERY_MANAGER_MOBILITY_SEVERITY_SCALE is a divisor in
       recovery_manager.c's own evaluate_mobility() (fault_severity =
       severity / SCALE, compared against a stage-dependent confidence
       floor there) - used here as a direct comparison instead, since
       the stage/confidence machinery isn't available at this call
       site. Treats "raw severity at or past the scale value" as the
       escalation point, which is the same order of magnitude that
       function's own division implies, not an independently invented
       number. */
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

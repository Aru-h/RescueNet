/*
 * Standalone self-test for core/mission_link.c (no hardware/board
 * required - desktop gcc only, like laptop/tools/lora_frame_selftest.c is
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
 * Also covers the board-side incident cache (board_incident_cache_
 * get()/put()/remove(), mission_link.h/.c) added alongside the codec:
 * insert/lookup/update by incident_id, eviction under a full cache,
 * and slot reuse after a resolved incident is removed. That cache
 * lives entirely inside mission_link.c (file-scope static), so these
 * tests only exercise it through its public get()/put()/remove() API
 * and mission_link_apply_incident() - same black-box approach as the
 * round-trip tests above.
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore -Ilaptop \
 *       -o mission_link_selftest laptop/tools/mission_link_selftest.c \
 *       core/[a-z]*.c laptop/snapshot.c
 * Run:
 *   ./mission_link_selftest
 *
 * (core/mission_state.c is required to link g_teams[]/g_incidents[] -
 * mission_link_apply_roster()/_incident() reference them even though
 * this test never calls those two apply functions on paths that
 * touch every element, and the linker still needs every extern
 * mission_link.c's object file references resolved regardless of
 * which functions in it actually get called at runtime.
 *
 * This now links the FULL core/ .c set, not just mission_link.c +
 * mission_state.c: mission_link_build_status() started calling
 * core/incident.c's team_effective_work_rate() to fill
 * MissionLinkStatus_t's schema-v2 work_rate anchor field (see
 * mission_link.h) - the same value update_incident_work() applies
 * board-side, shared rather than recomputed so the two can't disagree
 * (see incident.h's declaration comment on that function) - and
 * incident.c itself is deeply interconnected with the rest of core/
 * (search_zones.c, environment.c, team.c, path.c, grid.c, scoring.c,
 * cbba.c for cbba_team_is_local(), ...), unlike mission_link.c/
 * mission_state.c's previously-narrow dependency footprint. Past that
 * point, cherry-picking individual core/ files stopped being worth
 * the upkeep, so this now mirrors the Makefile's own LAPTOP_SOURCES
 * pattern (CORE_SOURCES + a platform's snapshot.c) rather than
 * hand-maintaining a second, narrower file list. laptop/
 * snapshot.c specifically (not debug.c) for the same reason as
 * before: core/'s .c files' snapshot_record()/snapshot_record_resolved()/
 * snapshot_begin_run()/etc. calls need *some* definition to link
 * against, and debug_trace() is stubbed below instead of pulled in
 * from laptop/debug.c to avoid the conflicting-definition
 * clash that would cause (this file already defines its own
 * debug_trace()).
 *
 * debug_print_generator_evaluation()/_selection() get the same
 * no-op-stub treatment below, for the same "avoid pulling in a real
 * platform debug.c just to satisfy one or two symbols" reasoning
 * debug_trace() below already documents - core/incident.c's
 * generate_incidents_if_needed() calls them, and they're real
 * per-platform implementations (laptop/debug.c,
 * board/STM32N6570-DK/debug.c), not something core/ itself defines.
 */

#include "mission_link.h"
#include "mission_link_codec.h"
#include "mission_state.h"
#include "environment.h"
#include "path.h"
#include "simulation.h"
#include "incident.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Minimal stand-in for debug.h's debug_trace() (real implementations:
   laptop/debug.c, board/STM32N6570-DK/debug.c) - board_incident_
   cache_put()'s eviction-logging call site needs *some* definition to
   link against here. Prints to stdout purely so a human running this
   selftest directly can see eviction happen; nothing in this file's
   CHECK()s depends on this output. */
void debug_trace(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/* Minimal no-op stand-ins for debug.h's other two platform-implemented
   hooks (see the Build comment above for why core/incident.c joined
   this test's link list, and why these two - like debug_trace() above
   - get stubbed here rather than pulling in a real platform debug.c).
   Neither is on any path this test's CHECK()s inspect: incident
   generation itself is exercised only incidentally, as a side effect
   of linking core/incident.c in for team_effective_work_rate().
   Signatures copied verbatim from core/debug.h. */
void debug_print_generator_evaluation(uint32_t tick,
                                      uint8_t load,
                                      uint32_t probability,
                                      uint32_t roll,
                                      uint8_t spawned,
                                      uint8_t available_teams,
                                      uint8_t en_route_teams,
                                      uint8_t on_scene_teams,
                                      uint8_t resting_teams,
                                      uint8_t open_incidents,
                                      uint8_t assigned_incidents,
                                      uint8_t in_progress_incidents,
                                      uint8_t active_system_tasks,
                                      uint16_t degraded_cells,
                                      uint32_t next_evaluation_tick)
{
    (void)tick; (void)load; (void)probability; (void)roll; (void)spawned;
    (void)available_teams; (void)en_route_teams; (void)on_scene_teams;
    (void)resting_teams; (void)open_incidents; (void)assigned_incidents;
    (void)in_progress_incidents; (void)active_system_tasks;
    (void)degraded_cells; (void)next_evaluation_tick;
}

void debug_print_generator_selection(const uint32_t *weights,
                                     uint32_t weight_count,
                                     uint32_t total_weight,
                                     uint32_t type_roll,
                                     uint8_t selected_type)
{
    (void)weights; (void)weight_count; (void)total_weight;
    (void)type_roll; (void)selected_type;
}

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
    r.equipment_level = 64u;

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

/* Variant of make_incident() with caller-chosen incident_id/priority/
   work_remaining - what the board_incident_cache_* tests below need
   to build many distinguishable incidents (and to force a specific
   one to be the cache's "furthest/lowest priority" entry for the
   eviction test). loc_x is derived from incident_id too, purely so a
   memcmp() mismatch after eviction/update would show up as more than
   just "some field changed" - it pins down which incident's data
   moved where. */
static MissionLinkIncident_t make_incident_with_id(uint32_t incident_id,
                                                    uint8_t priority,
                                                    uint16_t work_remaining)
{
    MissionLinkIncident_t inc;

    inc.incident_id = incident_id;
    inc.loc_x = (int16_t)(incident_id % 30000u);
    inc.loc_y = -30;
    inc.work_remaining = work_remaining;
    inc.priority = priority;
    inc.incident_type = 0u;   /* INCIDENT_STRUCTURAL_COLLAPSE */
    inc.req_capability = 0u;  /* SPEC_SEARCH */
    inc.status = 1u;          /* INC_ASSIGNED */
    inc.victim_count = 1u;
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

    /* Schema-v2 anchor fields (see mission_link.h's MissionLinkStatus_t
       doc) - given explicit, distinct values (not left uninitialized)
       so the round-trip check below actually exercises them, not just
       the original v1 prefix. */
    s.assigned_incident_id = 4242u;
    s.work_remaining_at_report = 1500u;
    s.work_rate = 9u;
    s.stamina_at_report = 73u;
    s.report_tick = 123456u;

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
   laptop/tools/telemetry_bridge.py's TelemetryFrameParser already applies. */
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

/* ---------------------------------------------------------------
 * Board-side incident cache (board_incident_cache_get()/put()/
 * remove(), mission_link.h/.c). The cache is a single file-scope
 * static in mission_link.c with no reset API (not part of the task
 * this cache was added for), so these tests run in a fixed order
 * against one shared instance and each picks a fresh incident_id
 * range to avoid depending on exactly what an earlier test left
 * behind - see each test's comment for why its range is safe.
 * ---------------------------------------------------------------
 */

/* Runs first, deliberately, while the process-wide cache is still
   empty (static BoardIncidentCache_t zero-initializes in_use to 0 for
   every slot) - this is the one test in this group that needs to
   know the exact starting occupancy, since it has to fill the cache
   to exactly BOARD_INCIDENT_CACHE_SIZE before the 16th insert can
   prove eviction happened rather than "there happened to be a free
   slot already". */
static void test_incident_cache_eviction_on_overflow(void)
{
    uint32_t base_id = 2000u;
    uint32_t low_priority_id = base_id + 7u; /* the one PRIORITY_LOW entry */
    uint32_t new_id = base_id + BOARD_INCIDENT_CACHE_SIZE; /* the 16th, distinct */
    uint32_t i;
    MissionLinkIncident_t inc;
    MissionLinkIncident_t *got;

    CHECK(BOARD_INCIDENT_CACHE_SIZE == 15u);

    /* Fill the cache with BOARD_INCIDENT_CACHE_SIZE distinct incidents.
       Every one is PRIORITY_HIGH except low_priority_id, which is the
       only PRIORITY_LOW (numerically highest = furthest/lowest
       priority) entry - that makes it the unique, deterministic
       eviction target below, not just "whichever happens to lose a
       tiebreak". */
    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        uint8_t priority = (base_id + i == low_priority_id) ? 4u /* PRIORITY_LOW */
                                                              : 2u /* PRIORITY_HIGH */;

        inc = make_incident_with_id(base_id + i, priority, (uint16_t)(100u + i));
        board_incident_cache_put(&inc);
    }

    /* All BOARD_INCIDENT_CACHE_SIZE should be present and intact -
       inserting up to exactly capacity must never evict anything. */
    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        got = board_incident_cache_get(base_id + i);
        CHECK(got != NULL);
        if (got != NULL)
        {
            CHECK(got->incident_id == base_id + i);
            CHECK(got->work_remaining == (uint16_t)(100u + i));
        }
    }

    /* Push a 16th distinct incident into the now-full cache. Must not
       crash, and must evict low_priority_id specifically (the only
       PRIORITY_LOW entry) rather than corrupting some other slot. */
    inc = make_incident_with_id(new_id, 1u /* PRIORITY_CRITICAL */, 999u);
    board_incident_cache_put(&inc);

    got = board_incident_cache_get(low_priority_id);
    CHECK(got == NULL); /* evicted */

    got = board_incident_cache_get(new_id);
    CHECK(got != NULL); /* the new arrival took its place */
    if (got != NULL)
    {
        CHECK(got->work_remaining == 999u);
    }

    /* Every OTHER original entry must have survived untouched - the
       eviction must be surgical (exactly one slot), not have
       corrupted or dropped its neighbors. */
    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        if (base_id + i == low_priority_id)
        {
            continue; /* the one we just confirmed was evicted */
        }

        got = board_incident_cache_get(base_id + i);
        CHECK(got != NULL);
        if (got != NULL)
        {
            CHECK(got->incident_id == base_id + i);
            CHECK(got->work_remaining == (uint16_t)(100u + i));
        }
    }
}

/* Insert/lookup/update-by-incident_id on a fresh id, disjoint from
   the 2000..2015 range test_incident_cache_eviction_on_overflow()
   uses - safe regardless of what that test left cached, and safe
   regardless of whether the cache is currently full (an update to an
   already-cached id never evicts anything; only a genuinely new id
   competes for a slot). */
static void test_incident_cache_insert_lookup_update(void)
{
    uint32_t id = 3000u;
    MissionLinkIncident_t inc = make_incident_with_id(id, 2u /* PRIORITY_HIGH */, 500u);
    MissionLinkIncident_t *got;

    board_incident_cache_put(&inc);

    got = board_incident_cache_get(id);
    CHECK(got != NULL);
    if (got != NULL)
    {
        CHECK(memcmp(got, &inc, sizeof(MissionLinkIncident_t)) == 0);
    }

    /* Same incident_id, changed fields - must update the existing
       slot in place, not create a second entry. */
    inc.work_remaining = 42u;
    inc.priority = 4u; /* PRIORITY_LOW */
    board_incident_cache_put(&inc);

    got = board_incident_cache_get(id);
    CHECK(got != NULL);
    if (got != NULL)
    {
        CHECK(got->work_remaining == 42u);
        CHECK(got->priority == 4u);
    }
}

static void test_incident_cache_get_missing_returns_null(void)
{
    MissionLinkIncident_t *got = board_incident_cache_get(0xFFFFFFFFu);

    CHECK(got == NULL);
}

/* Confirms removing a cached incident actually frees its slot for
   reuse, rather than just marking it unreachable while still
   occupying room. Picks a specific still-surviving id from the
   eviction test's range (2015, the 16th insert from that test, which
   is guaranteed present regardless of tiebreak details since it was
   just inserted there), removes it, then inserts a brand-new id and
   confirms every OTHER previously-cached entry is still present -
   if remove() hadn't actually freed the slot, this insert would have
   had to evict one of those survivors instead. */
static void test_incident_cache_remove_frees_slot_for_reuse(void)
{
    uint32_t freed_id = 2000u + BOARD_INCIDENT_CACHE_SIZE; /* 2015 */
    uint32_t reuse_id = 6000u;
    uint32_t base_id = 2000u;
    uint32_t low_priority_id = base_id + 7u;
    MissionLinkIncident_t inc;
    MissionLinkIncident_t *got;
    uint32_t i;

    got = board_incident_cache_get(freed_id);
    CHECK(got != NULL); /* sanity: still there from the eviction test */

    board_incident_cache_remove(freed_id);

    got = board_incident_cache_get(freed_id);
    CHECK(got == NULL);

    inc = make_incident_with_id(reuse_id, 3u /* PRIORITY_MEDIUM */, 7u);
    board_incident_cache_put(&inc);

    got = board_incident_cache_get(reuse_id);
    CHECK(got != NULL);

    /* Every surviving 2000..2014 entry (all but the one already
       evicted in the overflow test) must still be present - proof
       the reuse_id insert above reused freed_id's now-empty slot
       instead of evicting one of these. */
    for (i = 0u; i < BOARD_INCIDENT_CACHE_SIZE; i++)
    {
        if (base_id + i == low_priority_id || base_id + i == freed_id)
        {
            continue;
        }

        got = board_incident_cache_get(base_id + i);
        CHECK(got != NULL);
    }
}

/* End-to-end: confirms board_incident_cache_put()/_remove() are
   actually wired into mission_link_apply_incident(), not just
   independently correct in isolation - a fresh incident arriving
   over the link should show up in the cache, and one whose status
   arrives as INC_RESOLVED should disappear from it. */
static void test_apply_incident_wires_cache(void)
{
    uint32_t id = 7000u;
    MissionLinkIncident_t inc = make_incident_with_id(id, 2u, 100u);
    MissionLinkIncident_t *got;

    inc.status = (uint8_t)INC_ASSIGNED;
    mission_link_apply_incident(&inc);

    got = board_incident_cache_get(id);
    CHECK(got != NULL);

    inc.status = (uint8_t)INC_RESOLVED;
    mission_link_apply_incident(&inc);

    got = board_incident_cache_get(id);
    CHECK(got == NULL);
}

/* ---------------------------------------------------------------
 * Board-side merge of laptop incident frames (mission_link.c's
 * mission_link_apply_incident() ownership comment). This build is
 * board 0 with five teams per board, so teams 0-4 are local.
 * --------------------------------------------------------------- */

static IncidentState_t *board_slot_for(uint32_t incident_id)
{
    uint32_t i;

    for (i = 0u; i < NUM_INCIDENTS; i++)
    {
        if (g_incidents[i].slot_occupied && g_incidents[i].incident_id == incident_id)
        {
            return &g_incidents[i];
        }
    }
    return NULL;
}

/* A local team working incident_id the way the board's own allocation
   leaves it. */
static IncidentState_t *board_team_working(uint8_t team_id, uint32_t incident_id,
                                           uint16_t laptop_work, uint16_t board_work)
{
    MissionLinkIncident_t msg = make_incident_with_id(incident_id, 2u, laptop_work);
    IncidentState_t *inc;

    msg.status = (uint8_t)INC_OPEN;
    msg.req_capability = 2u; /* SPEC_MEDICAL */
    msg.incident_type = 2u;  /* INCIDENT_MASS_CASUALTY */
    mission_link_apply_incident(&msg);
    inc = board_slot_for(incident_id);
    if (inc == NULL)
    {
        return NULL;
    }

    memset(&g_teams[team_id], 0, sizeof(g_teams[team_id]));
    g_teams[team_id].team_id = team_id;
    g_teams[team_id].stamina = 90.0f;
    g_teams[team_id].equipment_level = 90u;
    g_teams[team_id].status = STATUS_ON_SCENE;
    g_teams[team_id].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[team_id].assigned_task_slot = (uint8_t)(inc - g_incidents);
    g_teams[team_id].resting_zone_id = NO_ASSIGNMENT;
    inc->assigned_team = team_id;
    inc->status = INC_IN_PROGRESS;
    inc->work_remaining = board_work;
    return inc;
}

static void clear_board_incidents(void)
{
    uint32_t i;

    for (i = 0u; i < NUM_INCIDENTS; i++)
    {
        g_incidents[i].slot_occupied = 0u;
    }
}

static void test_apply_incident_keeps_board_execution(void)
{
    MissionLinkIncident_t resend;
    IncidentState_t *inc;

    clear_board_incidents();
    inc = board_team_working(2u, 9100u, 100u, 40u);
    CHECK(inc != NULL);
    if (inc == NULL)
    {
        return;
    }

    /* The laptop's periodic re-send lags the board: still OPEN, old work. */
    resend = make_incident_with_id(9100u, 1u, 100u);
    resend.status = (uint8_t)INC_OPEN;
    resend.req_capability = 2u;
    resend.incident_type = 2u;
    resend.victim_count = 6u;
    mission_link_apply_incident(&resend);

    CHECK(inc->status == INC_IN_PROGRESS);
    CHECK(inc->assigned_team == 2u);
    CHECK(inc->work_remaining == 40u);
    CHECK(inc->priority == 1u && inc->victim_count == 6u); /* laptop's fields do apply */
    CHECK(g_teams[2].status == STATUS_ON_SCENE);
    CHECK(board_incident_cache_get(9100u) != NULL &&
          board_incident_cache_get(9100u)->work_remaining == 40u);
}

static void test_apply_incident_not_local_takes_laptop_status(void)
{
    MissionLinkIncident_t msg;
    IncidentState_t *inc;

    clear_board_incidents();
    /* Another board's team has it: the laptop says ASSIGNED. */
    msg = make_incident_with_id(9200u, 2u, 100u);
    msg.status = (uint8_t)INC_ASSIGNED;
    mission_link_apply_incident(&msg);
    inc = board_slot_for(9200u);
    CHECK(inc != NULL && inc->status == INC_ASSIGNED);

    /* That team gave it up; the laptop re-opens it. No local team held
       it, so the board must follow rather than keep ASSIGNED forever. */
    msg.status = (uint8_t)INC_OPEN;
    msg.work_remaining = 70u;
    mission_link_apply_incident(&msg);
    CHECK(inc != NULL && inc->status == INC_OPEN && inc->work_remaining == 70u);
}

static void test_apply_incident_moved_sends_team_back_en_route(void)
{
    MissionLinkIncident_t moved;
    IncidentState_t *inc;

    clear_board_incidents();
    inc = board_team_working(3u, 9300u, 100u, 55u);
    CHECK(inc != NULL);
    if (inc == NULL)
    {
        return;
    }

    moved = make_incident_with_id(9300u, 2u, 100u);
    moved.status = (uint8_t)INC_IN_PROGRESS;
    moved.req_capability = 2u;
    moved.incident_type = 2u;
    moved.loc_x = (int16_t)(inc->loc_x + 2);
    moved.loc_y = inc->loc_y;
    mission_link_apply_incident(&moved);

    CHECK(inc->loc_x == moved.loc_x);
    CHECK(g_teams[3].status == STATUS_EN_ROUTE);
    CHECK(inc->status == INC_ASSIGNED && inc->assigned_team == 3u);
}

static void test_apply_incident_new_stage_releases_team(void)
{
    MissionLinkIncident_t next_stage;
    IncidentState_t *inc;

    clear_board_incidents();
    inc = board_team_working(1u, 9400u, 100u, 5u);
    CHECK(inc != NULL);
    if (inc == NULL)
    {
        return;
    }

    next_stage = make_incident_with_id(9400u, 2u, 100u);
    next_stage.status = (uint8_t)INC_OPEN;
    next_stage.req_capability = 3u; /* SPEC_EXTRACTION */
    next_stage.incident_type = 2u;
    mission_link_apply_incident(&next_stage);

    CHECK(inc->slot_occupied && inc->req_capability == 3u);
    CHECK(inc->status == INC_OPEN && inc->work_remaining == 100u);
    CHECK(inc->assigned_team == NO_ASSIGNMENT);
    CHECK(g_teams[1].assigned_task_pool == TASK_POOL_NONE);
    CHECK(g_teams[1].status != STATUS_ON_SCENE);
}

static void test_apply_incident_resolved_releases_and_frees(void)
{
    MissionLinkIncident_t resolved;
    IncidentState_t *inc;

    clear_board_incidents();
    inc = board_team_working(4u, 9500u, 100u, 30u);
    CHECK(inc != NULL);
    if (inc == NULL)
    {
        return;
    }

    resolved = make_incident_with_id(9500u, 2u, 0u);
    resolved.status = (uint8_t)INC_RESOLVED;
    resolved.req_capability = 2u;
    resolved.incident_type = 2u;
    mission_link_apply_incident(&resolved);

    CHECK(board_slot_for(9500u) == NULL);
    CHECK(g_teams[4].assigned_task_pool == TASK_POOL_NONE);
    CHECK(g_teams[4].status != STATUS_ON_SCENE);
    CHECK(board_incident_cache_get(9500u) == NULL);

    /* A re-sent resolution for an incident this board never had (or
       already closed) takes no slot. */
    resolved.incident_id = 9501u;
    mission_link_apply_incident(&resolved);
    CHECK(board_slot_for(9501u) == NULL);
}

static void test_clock_codec_and_step_plan(void)
{
    MissionLinkClock_t clock;
    MissionLinkClock_t parsed;
    uint8_t wire[MISSION_LINK_CLOCK_WIRE_SIZE];
    uint8_t payload[MISSION_LINK_CLOCK_PAYLOAD_SIZE];
    uint32_t start = 0xFFFFFFFFu;

    clock.tick = 0x01020304u;
    CHECK(mission_link_pack_clock(&clock, wire) == MISSION_LINK_CLOCK_WIRE_SIZE);
    CHECK(wire[0] == MISSION_LINK_CLOCK_MAGIC0 && wire[1] == MISSION_LINK_CLOCK_MAGIC1);
    CHECK(mission_link_parse_clock(wire, sizeof(wire), &parsed) == 1 && parsed.tick == clock.tick);
    CHECK(mission_link_parse_clock(wire, sizeof(wire) - 1u, &parsed) == 0);
    wire[3] ^= 0x10u;
    CHECK(mission_link_parse_clock(wire, sizeof(wire), &parsed) == -1);
    mission_link_pack_clock_payload(&clock, payload);
    mission_link_parse_clock_payload(payload, &parsed);
    CHECK(parsed.tick == clock.tick);

    mission_link_clock_reset();
    CHECK(!mission_link_clock_received() && mission_link_authorized_tick() == 0u);
    clock.tick = 40u;
    mission_link_apply_clock(&clock);
    clock.tick = 12u;                     /* latest wins, not highest */
    mission_link_apply_clock(&clock);
    CHECK(mission_link_clock_received() && mission_link_authorized_tick() == 12u);

    CHECK(mission_link_clock_step_plan(5u, 5u, &start) == 0u && start == 0xFFFFFFFFu);
    CHECK(mission_link_clock_step_plan(9u, 5u, &start) == 0u);
    CHECK(mission_link_clock_step_plan(4u, 5u, &start) == 1u && start == 4u);
    CHECK(mission_link_clock_step_plan(2u, 2u + MISSION_LINK_CLOCK_MAX_CATCHUP_TICKS, &start) == 1u &&
          start == 2u);
    CHECK(mission_link_clock_step_plan(2u, 3u + MISSION_LINK_CLOCK_MAX_CATCHUP_TICKS, &start) == 1u &&
          start == 2u + MISSION_LINK_CLOCK_MAX_CATCHUP_TICKS);
    CHECK(mission_link_clock_step_plan(0u, 900u, &start) == 1u && start == 899u);
}

static void test_status_reports_position_and_hides_board_system_tasks(void)
{
    MissionLinkStatus_t st;

    memset(&g_teams[2], 0, sizeof(g_teams[2]));
    g_teams[2].team_id = 2u;
    g_teams[2].loc_x = 33;
    g_teams[2].loc_y = 7;
    g_teams[2].stamina = 50.0f;
    g_teams[2].assigned_task_pool = TASK_POOL_SYSTEM;
    g_teams[2].assigned_task_slot = 0u;
    g_system_tasks[0].slot_occupied = 1u;
    g_system_tasks[0].incident_id = 3u; /* board-local id, may collide */

    mission_link_build_status(2u, &st);
    CHECK(st.loc_x == 33 && st.loc_y == 7);
    CHECK(st.assigned_incident_id == MISSION_LINK_NO_INCIDENT);
    g_system_tasks[0].slot_occupied = 0u;
}

/* ---------------------------------------------------------------
 * Route map, sites, and board-side evacuee legs (mission_link.h
 * sections 5c/5d). This process never generates an environment, so it
 * routes exactly like a board.
 * --------------------------------------------------------------- */

static void test_map_row_codec_and_board_routing(void)
{
    MissionLinkMapRow_t row;
    MissionLinkMapRow_t parsed;
    uint8_t wire[MISSION_LINK_MAP_ROW_WIRE_SIZE];
    uint8_t cell = 0u;
    int next_x = -1;
    int next_y = -1;
    int cost;
    uint8_t y;

    CHECK(mission_link_map_encode_cell(0u, 9u) == MISSION_LINK_MAP_BLOCKED);
    CHECK(mission_link_map_encode_cell(1u, 0u) == 1u); /* passable never encodes as blocked */
    CHECK(mission_link_map_encode_cell(1u, 7u) == 7u);

    mission_link_map_reset();
    CHECK(mission_link_map_rows_received() == 0u);
    CHECK(environment_cell_is_navigable(5, 5) == 1u);  /* no map yet: old default */
    CHECK(environment_cell_travel_cost(5, 5) == 1u);

    memset(&row, 0, sizeof(row));
    row.row = 5u;
    memset(row.cells, 2, sizeof(row.cells));
    row.cells[5] = MISSION_LINK_MAP_BLOCKED;
    row.cells[6] = 9u;
    CHECK(mission_link_pack_map_row(&row, wire) == MISSION_LINK_MAP_ROW_WIRE_SIZE);
    CHECK(mission_link_parse_map_row(wire, sizeof(wire), &parsed) == 1);
    CHECK(memcmp(&parsed, &row, sizeof(row)) == 0);
    CHECK(mission_link_parse_map_row(wire, sizeof(wire) - 1u, &parsed) == 0);
    wire[20] ^= 0x40u;
    CHECK(mission_link_parse_map_row(wire, sizeof(wire), &parsed) == -1);

    CHECK(mission_link_apply_map_row(&row) == 1u);
    CHECK(mission_link_apply_map_row(&row) == 1u);      /* repeat: still one row */
    CHECK(mission_link_map_rows_received() == 1u);
    CHECK(mission_link_map_cell(5, 5, &cell) == 1u && cell == MISSION_LINK_MAP_BLOCKED);
    CHECK(mission_link_map_cell(5, 6, &cell) == 0u);    /* row 6 not here yet */
    CHECK(mission_link_map_cell(-1, 5, &cell) == 0u && mission_link_map_cell(36, 5, &cell) == 0u);
    CHECK(environment_cell_is_navigable(5, 5) == 0u);
    CHECK(environment_cell_travel_cost(6, 5) == 9u);
    CHECK(environment_cell_is_navigable(5, 6) == 1u);  /* unmapped row keeps the default */

    row.row = 200u;
    CHECK(mission_link_apply_map_row(&row) == 0u);
    CHECK(mission_link_map_rows_received() == 1u);

    /* A full map: open ground, with a wall across x=10 except a gap at
       y=30. A team crossing from (5,5) to (15,5) must head for the gap
       rather than step straight through the wall. */
    for (y = 0u; y < MISSION_LINK_MAP_HEIGHT; y++)
    {
        memset(&row, 0, sizeof(row));
        row.row = y;
        memset(row.cells, 1, sizeof(row.cells));
        if (y != 30u)
        {
            row.cells[10] = MISSION_LINK_MAP_BLOCKED;
        }
        (void)mission_link_apply_map_row(&row);
    }
    CHECK(mission_link_map_rows_received() == MISSION_LINK_MAP_HEIGHT);

    cost = find_next_path_step(9, 5, 11, 5, 0, &next_x, &next_y);
    CHECK(cost > 2);                               /* not the 2-step straight line */
    CHECK(!(next_x == 10 && next_y == 5));         /* never into the wall */
    CHECK(next_x == 9 && next_y == 6);             /* toward the gap */

    mission_link_map_reset();
    CHECK(environment_cell_is_navigable(10, 5) == 1u); /* reset forgets the map */
}

static void test_sites_codec_and_apply(void)
{
    MissionLinkSites_t sites;
    MissionLinkSites_t parsed;
    uint8_t wire[MISSION_LINK_SITES_WIRE_SIZE];

    memset(&sites, 0, sizeof(sites));
    sites.num_safe_zones = 2u;
    sites.safe_zones[0].loc_x = 3;
    sites.safe_zones[0].loc_y = 4;
    sites.safe_zones[0].capacity = 40u;
    sites.safe_zones[1].loc_x = 30;
    sites.safe_zones[1].loc_y = 31;
    sites.safe_zones[1].capacity = 25u;
    sites.gateway_x[0] = 0;  sites.gateway_y[0] = 17;
    sites.gateway_x[1] = 35; sites.gateway_y[1] = 2;
    sites.gateway_x[2] = 20; sites.gateway_y[2] = 35;

    CHECK(mission_link_pack_sites(&sites, wire) == MISSION_LINK_SITES_WIRE_SIZE);
    CHECK(mission_link_parse_sites(wire, sizeof(wire), &parsed) == 1 &&
          memcmp(&parsed, &sites, sizeof(sites)) == 0);

    mission_link_map_reset();
    g_safezones[1].evacuee_count = 12u;
    CHECK(!mission_link_sites_received());
    CHECK(mission_link_apply_sites(&sites) == 1u);
    CHECK(mission_link_sites_received());
    CHECK(g_num_safe_zones == 2u);
    CHECK(g_safezones[1].loc_x == 30 && g_safezones[1].loc_y == 31 && g_safezones[1].capacity == 25u);
    CHECK(g_safezones[1].evacuee_count == 12u); /* not on the wire: kept */
    CHECK(g_evacuation_gateways[2].loc_x == 20 && g_evacuation_gateways[2].loc_y == 35);

    /* Bad frames change nothing. */
    parsed = sites;
    parsed.num_safe_zones = 5u;
    CHECK(mission_link_apply_sites(&parsed) == 0u);
    parsed = sites;
    parsed.safe_zones[0].loc_x = 36;
    CHECK(mission_link_apply_sites(&parsed) == 0u);
    parsed = sites;
    parsed.gateway_y[1] = -1;
    CHECK(mission_link_apply_sites(&parsed) == 0u);
    CHECK(g_num_safe_zones == 2u && g_safezones[0].loc_x == 3);
}

static void test_board_evacuee_leg_waits_for_laptop(void)
{
    IncidentState_t *inc;
    SimRole_t saved = g_sim_role;

    clear_board_incidents();
    inc = board_team_working(0u, 9600u, 100u, 0u);
    CHECK(inc != NULL);
    if (inc == NULL)
    {
        return;
    }
    inc->incident_type = INCIDENT_EVACUEE_OFFBOARD;
    inc->req_capability = SPEC_EXTRACTION;
    /* A loading leg with people still waiting and a reachable gateway:
       the fast simulation would load them and head for the gateway. */
    inc->victim_count = 0u;
    inc->evac_safezone_id = 0u;
    inc->evac_capacity = 5u;
    g_safezones[0].loc_x = 3;
    g_safezones[0].loc_y = 3;
    g_safezones[0].evacuee_count = 7u;
    g_evacuation_gateways[0].loc_x = 3;
    g_evacuation_gateways[0].loc_y = 8;

    g_sim_role = SIM_ROLE_BOARD;
    complete_incident_stage(inc);
    g_sim_role = saved;

    CHECK(g_safezones[0].evacuee_count == 7u);        /* headcounts stay the laptop's */
    CHECK(board_slot_for(9600u) == NULL);             /* leg over, nothing staged here */
    CHECK(g_teams[0].assigned_task_pool == TASK_POOL_NONE);
    CHECK(g_teams[0].status != STATUS_ON_SCENE);
}

/* A board never runs generate_teams(), so the roster is the only thing
   that sets a team's equipment - without it every board team started at
   0 and rested before it could work. */
static void test_roster_sets_equipment_level(void)
{
    MissionLinkRoster_t r = make_roster();
    uint32_t rejects = mission_link_roster_reject_count();

    r.loc_x = 4;
    r.loc_y = 6;
    g_teams[r.team_id].equipment_level = 0u;
    mission_link_apply_roster(&r);
    CHECK(g_teams[r.team_id].equipment_level == 64u);
    CHECK(mission_link_roster_reject_count() == rejects);

    r.equipment_level = EQ_MAX_LEVEL + 1u;               /* out of range: whole roster rejected */
    r.stamina = 12u;
    mission_link_apply_roster(&r);
    CHECK(mission_link_roster_reject_count() == rejects + 1u);
    CHECK(g_teams[r.team_id].equipment_level == 64u);
    CHECK(g_teams[r.team_id].stamina == 87.0f);
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

    /* Order matters for this group: the cache is one shared static
       instance with no reset API. test_incident_cache_eviction_on_
       overflow() must run first (needs the cache to start empty), and
       test_incident_cache_remove_frees_slot_for_reuse() must run
       immediately after it (it depends on exactly the 2000..2014-
       minus-2007-plus-2015 state that test leaves behind - any test
       in between that inserts a brand-new id into the still-full
       cache would trigger an unrelated eviction and invalidate that
       assumption). The remaining tests only ever touch their own
       fresh incident_id and don't care what else the cache currently
       holds, so their order relative to each other doesn't matter. */
    test_incident_cache_eviction_on_overflow();
    test_incident_cache_remove_frees_slot_for_reuse();
    test_incident_cache_insert_lookup_update();
    test_incident_cache_get_missing_returns_null();
    test_apply_incident_wires_cache();

    test_apply_incident_keeps_board_execution();
    test_apply_incident_not_local_takes_laptop_status();
    test_apply_incident_moved_sends_team_back_en_route();
    test_apply_incident_new_stage_releases_team();
    test_apply_incident_resolved_releases_and_frees();
    test_clock_codec_and_step_plan();
    test_status_reports_position_and_hides_board_system_tasks();
    test_map_row_codec_and_board_routing();
    test_sites_codec_and_apply();
    test_board_evacuee_leg_waits_for_laptop();
    test_roster_sets_equipment_level();

    if (g_failures == 0)
    {
        printf("mission_link_selftest: all checks passed\n");
        return 0;
    }

    printf("mission_link_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

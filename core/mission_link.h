#ifndef MISSION_LINK_H
#define MISSION_LINK_H

#include <stdint.h>
#include "fault_config.h"   /* NUM_OPERATIONAL_FAULT_CLASSES */

/*
 * Wire contract for the PERSISTENT laptop<->board mission link, as
 * distinct from core/telemetry_frame.h (which is a one-way, board->
 * laptop, best-effort EVENT LOG for CSV/dataset generation and does
 * not carry live sim state).
 *
 * This link carries the state a board needs to run its own team
 * without holding a full copy of the world, and the state the laptop
 * needs back to keep incident/world data authoritative. See
 * ARCHITECTURE.md's laptop/board split (or the commit that added this
 * header) for the reasoning behind which fields live on which side -
 * this header only fixes the shape of the bytes, same as
 * telemetry_frame.h does for its frame. No encode/decode logic here;
 * see mission_link.c for pack/unpack + checksum helpers (the structs
 * below are too wide for telemetry_frame.h's hand-unrolled checksum
 * macro to stay readable, so that part is NOT logic-free like
 * telemetry_frame.h - a deliberate, documented departure from that
 * header's convention, not an oversight).
 *
 * Direction and cadence of each message:
 *
 *   MissionLinkRoster_t   laptop -> board, ONCE at mission start.
 *                          generate_teams() runs on the laptop now,
 *                          so the board can no longer re-derive its
 *                          own roster from team_id alone - this is
 *                          the one-shot handoff of that team's
 *                          starting identity/state.
 *
 *   MissionLinkIncident_t laptop -> board, ONGOING, one incident per
 *                          frame, sent on incident creation/discovery
 *                          and on any field change thereafter. Gives
 *                          the board's CBBA bidding and move_team()
 *                          pathing enough incident state to work from
 *                          without holding all of g_incidents[].
 *
 *   MissionLinkFault_t    laptop -> board, ONGOING, sent on any change
 *                          to that team's fault_state. fault_injector_
 *                          step() stays laptop-side (see ARCHITECTURE.md);
 *                          this is what makes that team's active[]/
 *                          severity[] visible to the board's own
 *                          watchdog_step()/recovery_manager_step(),
 *                          which do stay board-side.
 *
 *   MissionLinkStatus_t   board -> laptop, ONGOING, sent on team
 *                          status/equipment_level change. This is the
 *                          ONLY thing update_all_incident_work() needs
 *                          from a team - work_rate is rolled from the
 *                          laptop's own STREAM_WORK_RATE RNG, not
 *                          board-computed, so there is deliberately no
 *                          "work applied" message. Sending on every
 *                          status/equipment_level change (not every
 *                          tick) keeps this cheap: STATUS_ON_SCENE is
 *                          typically held for many ticks in a row.
 *
 * CBBAMessage_t (core/cbba.c) is NOT part of this link - that is
 * board<->board LoRa mesh traffic, orthogonal to this laptop<->board
 * channel.
 */

/* ---------------------------------------------------------------
 * 1. MissionLinkRoster_t - laptop -> board, once at mission start
 * ---------------------------------------------------------------
 * Mirrors the subset of TeamState_t a board needs to bootstrap
 * itself: identity (team_id, specialization), starting capability
 * (equipment_flags, fitness_tier - both otherwise derivable from
 * team_id % 5, but generate_teams() is laptop-side now, so the board
 * takes what it's given rather than re-deriving), and starting
 * physical state (loc_x/y, stamina). Everything else in TeamState_t
 * (incident_queue_*, fault_state, cached_remaining_path_cost, ...) is
 * either derived at runtime by the board itself or belongs to one of
 * the ongoing messages below - this is a one-shot bootstrap, not a
 * live mirror of TeamState_t.
 *
 * stamina is quantized to uint8_t (0-100) on the wire even though
 * TeamState_t.stamina is float - matches the precedent already set
 * for LoRaTwinSummary_t's stamina field, for the same reason (no
 * meaningful precision loss for a 0-100 gauge, half the wire cost).
 */
typedef struct __attribute__((packed))
{
    uint8_t  team_id;
    uint8_t  specialization;    /* TeamSpecialization_t */
    uint8_t  equipment_flags;   /* EQ_* bitmask */
    uint8_t  fitness_tier;      /* Fitness_Tier */
    int16_t  loc_x;
    int16_t  loc_y;
    uint8_t  stamina;           /* 0-100, quantized from TeamState_t's float */
} MissionLinkRoster_t;

#define MISSION_LINK_ROSTER_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkRoster_t)) /* 9 bytes */

/* ---------------------------------------------------------------
 * 2. MissionLinkIncident_t - laptop -> board, ongoing, per-incident
 * ---------------------------------------------------------------
 * Mirrors the subset of IncidentState_t a board's CBBA bidding
 * (req_capability, priority, victim_count for tiebreak) and
 * move_team() pathing (loc_x/y, status) need. Deliberately excludes
 * fields that stay laptop-only bookkeeping: hazard_flags, structure_
 * type, passive_effect/passive_timer, spawned_by, support_team, the
 * mission_fault_original_* backup fields, and reported_at_tick (the
 * board doesn't run age-weighted priority scoring - that's laptop-
 * side, already reflected in the priority value sent here).
 *
 * work_remaining IS included despite update_all_incident_work() being
 * laptop-only: CBBA bid quality depends on knowing how much work is
 * actually left on a task, not just that it's IN_PROGRESS - a board
 * bidding on a nearly-finished vs. freshly-opened incident needs this
 * to bid sanely. It's read-only input to the board's bid calculation,
 * never written back (see MissionLinkStatus_t - the return channel
 * carries team status only, never incident state).
 */
typedef struct __attribute__((packed))
{
    uint32_t incident_id;
    int16_t  loc_x;
    int16_t  loc_y;
    uint16_t work_remaining;
    uint8_t  priority;         /* Priority_t */
    uint8_t  incident_type;    /* IncidentType_t */
    uint8_t  req_capability;   /* TeamSpecialization_t */
    uint8_t  status;           /* IncidentStatus_t */
    uint8_t  victim_count;
    uint8_t  discovered;       /* Fog-of-war reveal flag */
} MissionLinkIncident_t;

#define MISSION_LINK_INCIDENT_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkIncident_t)) /* 16 bytes */

/* ---------------------------------------------------------------
 * 3. MissionLinkFault_t - laptop -> board, ongoing, per fault-state change
 * ---------------------------------------------------------------
 * Only active[]/severity[] per OperationalFaultClass_t - the two
 * fields watchdog_step()/recovery_manager_step() actually branch on.
 * NOT a mirror of TeamFaultState_t (which is 40+ bytes of laptop-only
 * bookkeeping: start_tick[], duration_ticks[], recovery_tick[],
 * target_x/y[], the original_* pre-fault backup fields, pending_
 * repair[]/repair_confirm_tick[]/immune_until_tick[] - all of that is
 * fault_injector_step()'s own state for running the fault lifecycle,
 * which stays laptop-side per ARCHITECTURE.md; the board only needs
 * to know what's active right now and how bad it is).
 */
typedef struct __attribute__((packed))
{
    uint8_t team_id;
    uint8_t active[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t severity[NUM_OPERATIONAL_FAULT_CLASSES];
} MissionLinkFault_t;

#define MISSION_LINK_FAULT_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkFault_t)) /* 11 bytes, NUM_OPERATIONAL_FAULT_CLASSES == 5 */

/* ---------------------------------------------------------------
 * 4. MissionLinkStatus_t - board -> laptop, ongoing, per status change
 * ---------------------------------------------------------------
 * Deliberately minimal: status and equipment_level are the only two
 * fields update_incident_work() reads from a team (see core/
 * incident.c) - status != STATUS_ON_SCENE or equipment_level == 0
 * both short-circuit the work-rate roll before touching anything
 * team-specific. No position, no stamina, no work-applied figure:
 * work_rate is rolled from the laptop's own STREAM_WORK_RATE RNG on
 * receipt of STATUS_ON_SCENE, not computed board-side and reported.
 * If the dashboard later wants live team position, that is a new,
 * separate field addition here - not implied by anything this struct
 * currently needs to carry.
 */
typedef struct __attribute__((packed))
{
    uint8_t team_id;
    uint8_t status;             /* TeamStatus_t */
    uint8_t equipment_level;
} MissionLinkStatus_t;

#define MISSION_LINK_STATUS_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkStatus_t)) /* 3 bytes */

/* ---------------------------------------------------------------
 * 5. On-wire framing - one magic-byte pair per message type
 * ---------------------------------------------------------------
 * Same shared-UART problem telemetry_frame.h solves (USART1 carries
 * free-running tm_printf text and, per that header, TelemetryFrame_t
 * frames too - this link shares the wire with both), plus now FOUR
 * distinct payload shapes instead of one, so a single magic pair
 * is not enough to disambiguate on sight. Each message type gets its
 * own magic pair rather than a shared magic + type byte, so a
 * resyncing reader can identify both "this is a mission-link frame"
 * and "this is specifically a roster/incident/fault/status frame" in
 * the same two-byte scan, with no extra branch - consistent with how
 * telemetry_frame.h already reserved 0xA5 0x5A for its one frame type.
 *
 * What actually needs to hold for the first (scanned-for) byte of
 * each pair is non-ASCII (>0x7E): tm_printf only ever emits plain
 * ASCII text, so a first byte outside that range can never appear in
 * debug output, which is what rules out a false match regardless of
 * the second byte's value - the second byte does NOT need the same
 * property (this corrects an imprecise claim in telemetry_frame.h's
 * own comment, which says "both bytes are non-printable" for its
 * 0xA5 0x5A pair; 0x5A is 'Z', printable - it doesn't break that
 * frame's resync for the reason above, but the stated rationale
 * there is inaccurate and worth fixing there too). All five first
 * bytes here (0xA5, 0xC3, 0xD4, 0xE7, 0xF9) are >0x7E, and all five
 * full pairs are pairwise distinct, so a reader scanning for any of
 * them knows unambiguously which payload shape follows.
 *
 * Layout per frame, all fields little-endian, matches telemetry_
 * frame.h's shape exactly (2 magic + payload + 1 checksum byte):
 *
 *   byte 0-1        magic       one of the *_MAGIC0/1 pairs below
 *   byte 2..(N-2)   payload     the matching Mission Link struct above
 *   byte N-1        checksum    additive checksum over payload only
 *                                (see mission_link.c - too wide for a
 *                                 hand-unrolled macro past the 10-byte
 *                                 case telemetry_frame.h covers)
 *
 * Checksum is the same additive-sum-mod-256 scheme as telemetry_
 * frame.h, for the same reason: cheap resync protection against
 * single-byte corruption/torn frames, not adversarial-grade -
 * consistent with "one fault model for one fault class" already
 * established for FAULT_CLASS_COMMUNICATION elsewhere in this
 * project. Gaps/duplicates are a transport-layer concern for
 * mission_link.c's resync logic to handle, same as telemetry_
 * frame.h section 3 - not something fixed by this header.
 */

#define MISSION_LINK_ROSTER_MAGIC0   ((uint8_t)0xC3u)
#define MISSION_LINK_ROSTER_MAGIC1   ((uint8_t)0x3Cu)

#define MISSION_LINK_INCIDENT_MAGIC0 ((uint8_t)0xD4u)
#define MISSION_LINK_INCIDENT_MAGIC1 ((uint8_t)0x4Du)

#define MISSION_LINK_FAULT_MAGIC0    ((uint8_t)0xE7u)
#define MISSION_LINK_FAULT_MAGIC1    ((uint8_t)0x7Eu)

#define MISSION_LINK_STATUS_MAGIC0   ((uint8_t)0xF9u)
#define MISSION_LINK_STATUS_MAGIC1   ((uint8_t)0x9Fu)

#define MISSION_LINK_ROSTER_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_ROSTER_PAYLOAD_SIZE + 1u))   /* 12 bytes */
#define MISSION_LINK_INCIDENT_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_INCIDENT_PAYLOAD_SIZE + 1u)) /* 19 bytes */
#define MISSION_LINK_FAULT_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_FAULT_PAYLOAD_SIZE + 1u))    /* 14 bytes */
#define MISSION_LINK_STATUS_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_STATUS_PAYLOAD_SIZE + 1u))   /* 6 bytes */

#endif /* MISSION_LINK_H */

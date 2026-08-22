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
 *                          status/equipment_level/work_rate change AND
 *                          on a periodic heartbeat (MISSION_LINK_
 *                          HEARTBEAT_INTERVAL_MS). Schema v2 (see this
 *                          struct's own doc below): a board-hosted
 *                          team's work_rate is now computed board-side
 *                          from that team's own fault-adjusted
 *                          execution state and reported as an anchor
 *                          snapshot (assigned_incident_id,
 *                          work_remaining_at_report, work_rate,
 *                          stamina_at_report, report_tick), not left
 *                          for update_all_incident_work() to roll from
 *                          the laptop's own STREAM_WORK_RATE RNG - that
 *                          RNG roll is now only used for incidents
 *                          assigned to teams no board hosts. Still
 *                          "sent on change, not every tick" - the
 *                          anchor snapshot plus the laptop-side
 *                          extrapolation formula (see below) is what
 *                          keeps this cheap while still letting the
 *                          dashboard display smoothly, not the
 *                          per-status-field change trigger alone.
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
 * 4. MissionLinkStatus_t - board -> laptop, ongoing, status + progress
 *    anchor (SCHEMA v2 - see version note below)
 * ---------------------------------------------------------------
 * v1 of this struct (team_id, status, equipment_level - 3 bytes) was
 * deliberately minimal because work_rate was rolled from the laptop's
 * own STREAM_WORK_RATE RNG on receipt of STATUS_ON_SCENE, never
 * board-computed. That's no longer true for board-hosted teams: a
 * board now computes its own fault-degraded work_rate (core/
 * incident.c's team_effective_work_rate(), same value
 * update_incident_work() itself applies for that team - see that
 * file's rewrite for why the two must never disagree) and this v2
 * struct carries it, plus enough of an anchor snapshot for the laptop
 * to extrapolate smooth progress/stamina between reports without a
 * message every tick:
 *
 *   assigned_incident_id       which incident this report's
 *                               work_remaining_at_report/work_rate
 *                               apply to. MISSION_LINK_NO_INCIDENT
 *                               (the uint32_t-width equivalent of
 *                               NO_ASSIGNMENT's "all bits set"
 *                               sentinel convention - NOT NO_ASSIGNMENT
 *                               itself widened, since 0xFF is a
 *                               plausible real incident_id once
 *                               g_incident_counter has run long enough)
 *                               for an idle/traveling-without-a-task
 *                               team.
 *   work_remaining_at_report   IncidentState_t.work_remaining's value
 *                               at report_tick - an anchor, not a
 *                               running total the laptop keeps
 *                               decrementing on its own.
 *   work_rate                  this team's currently-effective,
 *                               fault-adjusted rate at report_tick,
 *                               transmitted explicitly rather than
 *                               left for the laptop to re-derive (the
 *                               laptop has no visibility into this
 *                               board's live fault_state/equipment_
 *                               level between MissionLinkFault_t
 *                               updates - re-deriving here would mean
 *                               re-deriving from stale/incomplete
 *                               inputs).
 *   stamina_at_report           0-100 quantized, same convention
 *                               MissionLinkRoster_t.stamina already
 *                               uses for the one-shot starting value -
 *                               this is that field's ongoing
 *                               counterpart.
 *   report_tick                 this board's g_current_tick at the
 *                               moment of this snapshot - the anchor
 *                               timestamp the laptop extrapolates
 *                               forward from (see the laptop-side
 *                               extrapolation formula below).
 *
 * Send triggers (all event-driven or timer-driven, never tick-count-
 * driven - see BOARD_TICK_DELAY_MS=0's "runs flat out" note elsewhere
 * in this project for why a per-tick send has no radio budget that
 * survives it):
 *   - status or equipment_level change (v1 behavior, unchanged)
 *   - work_rate change (fault onset/clear on a resource-relevant
 *     class, or a new task assignment resetting the anchor)
 *   - a periodic heartbeat, MISSION_LINK_HEARTBEAT_INTERVAL_MS apart
 *     on a genuine µT-Kernel wall-clock timer (tk_get_tim() or
 *     equivalent, NOT a sim-tick counter), re-anchoring even when
 *     nothing changed - bounds staleness and gives the laptop's
 *     watchdog a liveness signal if a board goes quiet.
 *
 * Laptop-side extrapolation (platform/laptop/snapshot.c): between
 * reports, the displayed progress is
 *     work_remaining_at_report - work_rate * (current_tick - report_tick)
 * clamped at 0 (and the analogous formula for stamina), NOT a second,
 * independently-simulated laptop-side progress figure running in
 * parallel - see that file for why the two must not race each other.
 * Every new MissionLinkStatus_t arrival resets the anchor and
 * overwrites the projection - last-write-wins by arrival, same
 * pattern CBBA already uses for timestamp[] conflict resolution.
 *
 * Still no live team position in this struct (see v1's note on that -
 * unchanged, still a hypothetical future field addition, not implied
 * by anything added here).
 */
typedef struct __attribute__((packed))
{
    uint8_t  team_id;
    uint8_t  status;             /* TeamStatus_t */
    uint8_t  equipment_level;
    uint32_t assigned_incident_id; /* MISSION_LINK_NO_INCIDENT if idle */
    uint16_t work_remaining_at_report; /* anchor - matches IncidentState_t.work_remaining */
    uint8_t  work_rate;          /* board-computed, fault-adjusted */
    uint8_t  stamina_at_report;  /* 0-100, quantized - see MissionLinkRoster_t.stamina */
    uint32_t report_tick;        /* anchor timestamp (board's g_current_tick) */
} MissionLinkStatus_t;

#define MISSION_LINK_STATUS_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkStatus_t)) /* 15 bytes (was 3 in schema v1) */

/* Sentinel for MissionLinkStatus_t.assigned_incident_id - see this
 * struct's own doc above for why this is a fresh, width-matched
 * "all bits set" sentinel rather than NO_ASSIGNMENT (0xFF) widened:
 * incident_id is a monotonically-increasing counter starting at 0
 * (core/incident.c's allocate_incident_id()), so 0xFF is reachable in
 * a long run and would eventually collide with a real incident. */
#define MISSION_LINK_NO_INCIDENT ((uint32_t)0xFFFFFFFFu)

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

/* VERSION NOTE: MissionLinkStatus_t moved to schema v2 (see its own
 * doc, section 4 above) - payload grew from 3 to 15 bytes. The magic
 * pair itself is unchanged: MISSION_LINK_STATUS_WIRE_SIZE below is
 * derived from sizeof(MissionLinkStatus_t), so every parser that uses
 * the wire-size macro rather than a hardcoded byte count picks up the
 * new length automatically, and there is no firmware-in-the-field
 * compatibility burden yet (this link is not wired into a flashed
 * image - see platform/board/main_board.c's own top-of-file note) that
 * would otherwise justify spending a fresh magic pair on the version
 * bump instead of just documenting it here. */
#define MISSION_LINK_STATUS_MAGIC0   ((uint8_t)0xF9u)
#define MISSION_LINK_STATUS_MAGIC1   ((uint8_t)0x9Fu)

#define MISSION_LINK_ROSTER_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_ROSTER_PAYLOAD_SIZE + 1u))   /* 12 bytes */
#define MISSION_LINK_INCIDENT_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_INCIDENT_PAYLOAD_SIZE + 1u)) /* 19 bytes */
#define MISSION_LINK_FAULT_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_FAULT_PAYLOAD_SIZE + 1u))    /* 14 bytes */
#define MISSION_LINK_STATUS_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_STATUS_PAYLOAD_SIZE + 1u))   /* 18 bytes (was 6 in schema v1) */

/* ---------------------------------------------------------------
 * 5a. Heartbeat cadence for MissionLinkStatus_t's periodic re-anchor
 * ---------------------------------------------------------------
 * Not derived from CBBA_ROUND_TIMEOUT_MS (core/cbba.h) by #include:
 * cbba.h includes mission_state.h, which includes THIS header (for
 * MissionLinkIncident_t/BoardIncidentCache_t - see mission_state.h's
 * own include comment), so mission_link.h -> cbba.h -> mission_state.h
 * -> mission_link.h would be a circular include. Kept as an
 * independent literal instead, picked as a small multiple of
 * CBBA_ROUND_TIMEOUT_MS (530 ms) per this prompt's design note - a
 * multiple, not an unrelated number, so heartbeat traffic and CBBA's
 * own per-round mesh timing budget stay on a shared cadence rather
 * than an arbitrary one that could drift into colliding with it once
 * Prompt 2's channel-busy calibration lands. core/mission_link.c
 * carries a _Static_assert (where cbba.h CAN be safely included, since
 * a .c file has no downstream includers to cycle through) that this
 * stays a multiple of CBBA_ROUND_TIMEOUT_MS - if that ever fires,
 * update the multiplier below rather than the assert. 4x round-trip
 * budgets bounds worst-case staleness at ~2.1s while giving CBBA's
 * own traffic several full rounds of headroom between heartbeats. */
#define MISSION_LINK_HEARTBEAT_INTERVAL_MS ((uint32_t)2120u) /* 530ms * 4 */

/* ---------------------------------------------------------------
 * 6. Board-side incident cache
 * ---------------------------------------------------------------
 * NOT wire format - these three operate on BoardIncidentCache_t
 * (core/mission_state.h), a board-local lookup structure keyed by
 * incident_id, not a struct that ever goes on the wire. Declared here
 * rather than in core/mission_state.h anyway because their only
 * parameter/return type is MissionLinkIncident_t, and putting them
 * next to that struct's own pack/parse declarations keeps every
 * MissionLinkIncident_t-shaped operation discoverable from one place.
 * This is the same kind of deliberate, documented departure from this
 * header's usual "wire format only" convention that mission_link.c's
 * own top-of-file comment already calls out for its checksum helper.
 *
 * mission_link_apply_incident() (mission_link_codec.h) is the only
 * caller of board_incident_cache_put()/_remove() today - see that
 * function's definition in mission_link.c for where the cache is kept
 * in sync with each incoming MissionLinkIncident_t frame.
 */

/* Looks up incident_id in the board's local cache. Returns a pointer
   to the cached MissionLinkIncident_t if present, NULL otherwise. The
   returned pointer aliases the cache's own storage - valid only until
   the next board_incident_cache_put()/_remove() call, same lifetime
   contract as mission_link_find_incident_slot()'s return value in
   mission_link.c. */
MissionLinkIncident_t *board_incident_cache_get(uint32_t incident_id);

/* Inserts inc into the cache, or updates the existing slot if
   inc->incident_id is already cached. If the cache is full and
   inc->incident_id is not already present, evicts the cached incident
   with the furthest/lowest priority (see Priority_t in mission_state.h
   - numerically highest value) to make room, logging a debug line
   when this happens. */
void board_incident_cache_put(const MissionLinkIncident_t *inc);

/* Removes incident_id from the cache if present (no-op otherwise),
   freeing its slot for reuse without waiting for eviction pressure.
   Called once a MissionLinkIncident_t.status arrives as resolved -
   see mission_link_apply_incident(). */
void board_incident_cache_remove(uint32_t incident_id);

#endif /* MISSION_LINK_H */

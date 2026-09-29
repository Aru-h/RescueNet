#ifndef MISSION_LINK_H
#define MISSION_LINK_H

#include <stdint.h>
#include "fault_config.h"       /* NUM_OPERATIONAL_FAULT_CLASSES */
#include "comms_precedence.h"   /* CommsPrecedence_t - see mission_link_*_precedence()
                                   declarations below, next to each struct they classify */

/*
 * Wire contract for the PERSISTENT laptop<->board mission link, as
 * distinct from core/telemetry_frame.h (which is a one-way, board->
 * laptop, best-effort EVENT LOG for CSV/dataset generation and does
 * not carry live sim state).
 *
 * This link carries the state a board needs to run its own team
 * without holding a full copy of the world, and the state the laptop
 * needs back to keep incident/world data authoritative. See
 * docs/architecture/ARCHITECTURE.md's laptop/board split (or the commit that added this
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
 *                          step() stays laptop-side (see docs/architecture/ARCHITECTURE.md);
 *                          this is what lets the board's team physics
 *                          (move_team(), update_team(), team_effective_
 *                          work_rate()) apply the fault's effect. The
 *                          board's watchdog_step()/recovery_manager_step()
 *                          must NOT read it - they detect faults only
 *                          from those observable effects.
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
 *   MissionLinkMapRow_t   laptop -> board, route map rows at start, on
 *                          change and periodically - section 5c.
 *
 *   MissionLinkSites_t    laptop -> board, safe zones and gateways, with
 *                          the rosters - section 5d.
 *
 *   MissionLinkClock_t    laptop -> board, ONGOING, after every laptop
 *                          tick plus periodic re-sends. Boards run
 *                          their own ticks only up to it - see
 *                          section 5b.
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
 * physical state (loc_x/y, stamina, equipment_level). Everything else in TeamState_t
 * (incident_queue_*, fault_state, cached_remaining_path_cost, ...) is
 * either derived at runtime by the board itself or belongs to one of
 * the ongoing messages below - this is a one-shot bootstrap, not a
 * live mirror of TeamState_t.
 *
 * stamina is quantized to uint8_t (0-100) on the wire even though
 * TeamState_t.stamina is float - matches the precedent already set
 * for LoRaTwinSummary_t's stamina field, for the same reason (no
 * meaningful precision loss for a 0-100 gauge, half the wire cost).
 *
 * equipment_level (0-EQ_MAX_LEVEL) is carried because nothing on a
 * board would otherwise set it: generate_teams() is the only place that
 * fills it, and a board never runs that, so every board team started at
 * 0 - resting for its first ~14 ticks before it was allowed to work.
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
    uint8_t  equipment_level;   /* 0-EQ_MAX_LEVEL */
} MissionLinkRoster_t;

#define MISSION_LINK_ROSTER_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkRoster_t)) /* 10 bytes */

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

/*
 * COMMS_PRECEDENCE_FLASH if this incident is both PRIORITY_CRITICAL
 * (mission_state.h's Priority_t) and currently marked discovered - a
 * newly-visible, critical-severity incident (mass casualty, active
 * fire, chemical exposure, gas leak - see IncidentType_t) is
 * CONCORD's closest real equivalent to CCEB's "initial contact"
 * FLASH definition. COMMS_PRECEDENCE_PRIORITY otherwise (lower
 * priority, or an ordinary field-update resend of an already-known
 * incident) - see mission_link_apply_and_relay_incident() in
 * board/{Raspberry_Pi_Pico,STM32N6570-DK}/mission_link_task.c, which
 * relays every incident message
 * identically regardless of priority/discovered today, so this is a
 * pure function of fields already on the wire, not new state to
 * track. Pure function of *msg - safe to call from any relay/send
 * site that already has the struct in hand. */
CommsPrecedence_t mission_link_incident_precedence(const MissionLinkIncident_t *msg);

/* ---------------------------------------------------------------
 * 3. MissionLinkFault_t - laptop -> board, ongoing, per fault-state change
 * ---------------------------------------------------------------
 * Only active[]/severity[] per OperationalFaultClass_t - the two
 * fields the board's team physics (move_team(), update_team()'s
 * heartbeat/resource handling, team_effective_work_rate()) branch on.
 * This is ground truth for simulating the fault's effect only: the
 * watchdog and recovery manager never read it and detect faults from
 * the resulting observable behaviour instead.
 * NOT a mirror of TeamFaultState_t (which is 40+ bytes of laptop-only
 * bookkeeping: start_tick[], duration_ticks[], recovery_tick[],
 * target_x/y[], the original_* pre-fault backup fields, pending_
 * repair[]/repair_confirm_tick[]/immune_until_tick[] - all of that is
 * fault_injector_step()'s own state for running the fault lifecycle,
 * which stays laptop-side per docs/architecture/ARCHITECTURE.md; the board only needs
 * to know what's active right now and how bad it is).
 */
typedef struct __attribute__((packed))
{
    uint8_t team_id;
    uint8_t active[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t severity[NUM_OPERATIONAL_FAULT_CLASSES];
} MissionLinkFault_t;

/* Upper bound mission_link_apply_fault() clamps each severity[] entry to
   on receipt. Real fault profiles use 1-3 (core/fault_config.c's
   effect_strength); nothing indexes by severity, so this only bounds the
   arithmetic a corrupted frame could drive (see that function). */
#define MISSION_LINK_FAULT_SEVERITY_MAX ((uint8_t)10u)

#define MISSION_LINK_FAULT_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkFault_t)) /* 11 bytes, NUM_OPERATIONAL_FAULT_CLASSES == 5 */

/*
 * COMMS_PRECEDENCE_IMMEDIATE if any active fault class's severity is
 * at or past the threshold recovery_manager.c itself already uses to
 * escalate a recovery decision for that class - severity[FAULT_CLASS_
 * COMMUNICATION] >= RECOVERY_MANAGER_COMMUNICATION_SEVERE_THRESHOLD,
 * or severity[FAULT_CLASS_MOBILITY] >= RECOVERY_MANAGER_MOBILITY_
 * SEVERITY_SCALE (the only two fault classes with a directly
 * comparable named constant in recovery_manager_config.h).
 * COMMS_PRECEDENCE_PRIORITY otherwise.
 *
 * Deliberately NOT a re-implementation of evaluate_mobility()'s full
 * confidence math (recovery_manager.c) - that function also folds in
 * WatchdogStatus_t, MissionStage_t, and team->degraded, none of which
 * are naturally available at a relay/send call site building just
 * this one struct. This is a lightweight proxy for message routing
 * urgency, calibrated against the same two named thresholds recovery_
 * manager.c uses for its own escalation decision, not a claim that it
 * reproduces that decision exactly. FAULT_CLASS_RESOURCE/ENVIRONMENT/
 * MISSION have no equivalent named threshold in recovery_manager_
 * config.h today, so they're left at PRIORITY regardless of severity
 * rather than guessing an unfounded cutoff for them - a known gap,
 * not a silent omission, if one of those three classes ever needs the
 * same treatment. */
CommsPrecedence_t mission_link_fault_precedence(const MissionLinkFault_t *msg);

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
 * Laptop-side extrapolation (laptop/snapshot.c): between
 * reports, the displayed progress is
 *     work_remaining_at_report - work_rate * (current_tick - report_tick)
 * clamped at 0 (and the analogous formula for stamina), NOT a second,
 * independently-simulated laptop-side progress figure running in
 * parallel - see that file for why the two must not race each other.
 * Every new MissionLinkStatus_t arrival resets the anchor and
 * overwrites the projection - last-write-wins by arrival, same
 * pattern CBBA already uses for timestamp[] conflict resolution.
 *
 * SCHEMA v3 adds loc_x/loc_y, the team's live grid position. Boards
 * move their own teams, so without it the laptop only ever knew each
 * team's roster starting cell. A position change is its own send
 * trigger (mission_link_status_tx_step()); a moving team changes cell
 * at most once per tick, and the laptop's clock frame paces ticks.
 *
 * assigned_incident_id only ever names a laptop-issued task (one that
 * arrived as a MissionLinkIncident_t). A board-local system task
 * (equipment cache, evacuee run) takes its id from the board's own
 * counter, which can collide with laptop ids, so it is reported as
 * MISSION_LINK_NO_INCIDENT.
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
    int16_t  loc_x;              /* schema v3: live position */
    int16_t  loc_y;
} MissionLinkStatus_t;

#define MISSION_LINK_STATUS_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkStatus_t)) /* 19 bytes (15 in v2, 3 in v1) */

/*
 * COMMS_PRECEDENCE_FLASH if stamina_at_report or equipment_level is
 * at or below the exact thresholds recovery_manager.c itself uses to
 * consider RECOVERY_ACTION_ABORT_MISSION (RECOVERY_MANAGER_STAMINA_
 * CRITICAL_THRESHOLD, RECOVERY_MANAGER_EQUIPMENT_CRITICAL_THRESHOLD -
 * both already on this struct's own 0-100 wire scale, no unit
 * conversion needed). COMMS_PRECEDENCE_ROUTINE otherwise - see this
 * struct's own doc above on why routine heartbeat/status-change
 * traffic is designed to tolerate staleness; this carve-out is the
 * one case where that's no longer true. There is currently no
 * separate "why did this change" signal in the send trigger (see
 * mission_link_status_tx_step()/mission_link_send_status() in
 * board/{Raspberry_Pi_Pico,STM32N6570-DK}/mission_link_task.c -
 * status/equipment/work_rate change and
 * the periodic heartbeat all funnel through the same call), so this
 * function is what actually distinguishes an urgent send from a
 * routine one - it must be called on every STATUS send, not just
 * ones that "look" like they might be a recovery event. */
CommsPrecedence_t mission_link_status_precedence(const MissionLinkStatus_t *msg);

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
 * image - see board/STM32N6570-DK/main_board.c's own top-of-file note) that
 * would otherwise justify spending a fresh magic pair on the version
 * bump instead of just documenting it here. */
#define MISSION_LINK_STATUS_MAGIC0   ((uint8_t)0xF9u)
#define MISSION_LINK_STATUS_MAGIC1   ((uint8_t)0x9Fu)

#define MISSION_LINK_ROSTER_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_ROSTER_PAYLOAD_SIZE + 1u))   /* 13 bytes */
#define MISSION_LINK_INCIDENT_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_INCIDENT_PAYLOAD_SIZE + 1u)) /* 19 bytes */
#define MISSION_LINK_FAULT_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_FAULT_PAYLOAD_SIZE + 1u))    /* 14 bytes */
#define MISSION_LINK_STATUS_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_STATUS_PAYLOAD_SIZE + 1u))   /* 22 bytes (18 in v2, 6 in v1) */

/* ---------------------------------------------------------------
 * 5b. MissionLinkClock_t - laptop -> board, the mission clock
 * ---------------------------------------------------------------
 * The laptop is the single clock master, the way distributed
 * simulations (IEEE 1278 DIS / 1516 HLA time management, ROS's /clock
 * with use_sim_time) keep separate simulators on one timeline: no
 * participant free-runs its own tick. After finishing tick N (and
 * sending that tick's incident/fault frames) the laptop sends
 * tick = N, and a board may run its own sim ticks up to N, never past.
 * How fast N advances - 36 real seconds per tick (TICK_TIME_SEC) for a
 * demo, faster for a rehearsal - is purely a laptop setting; boards
 * just follow. The laptop re-sends the current tick periodically, so a
 * lost frame only delays a board, and the latest frame wins (not the
 * highest), so a restarted laptop mission is followed too.
 *
 * A board that falls more than MISSION_LINK_CLOCK_MAX_CATCHUP_TICKS
 * behind (it rebooted mid-mission, or sat out a long radio outage)
 * jumps its tick forward instead of replaying every missed tick: each
 * replayed tick includes a CBBA round with a reply wait, and replaying
 * hours of movement on stale data is not what the team did anyway.
 * See mission_link_clock_step_plan() (mission_link_codec.h).
 */
typedef struct __attribute__((packed))
{
    uint32_t tick;
} MissionLinkClock_t;

#define MISSION_LINK_CLOCK_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkClock_t)) /* 4 bytes */

#define MISSION_LINK_CLOCK_MAGIC0 ((uint8_t)0xCBu)
#define MISSION_LINK_CLOCK_MAGIC1 ((uint8_t)0xBCu)

#define MISSION_LINK_CLOCK_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_CLOCK_PAYLOAD_SIZE + 1u))    /* 7 bytes */

#define MISSION_LINK_CLOCK_MAX_CATCHUP_TICKS 3u

/* ---------------------------------------------------------------
 * 5c. MissionLinkMapRow_t - laptop -> board, the route map
 * ---------------------------------------------------------------
 * A board never generates the environment (the laptop is the world
 * authority), but its CBBA bids, movement, safe-zone choice and
 * execution twin all price routes through core/path.c, which reads
 * two things per cell: passable, and travel cost. Without this a
 * board's map is "everything passable at cost 1" and its teams walk
 * straight through rubble, water and fire.
 *
 * One frame per grid row y, one byte per cell x:
 *   0      not passable
 *   1-255  passable, this travel cost (environment_cell_travel_cost())
 * The laptop sends every row at mission start, again whenever any cell
 * in that row changes (hazard spread, aftershocks, road damage,
 * environment faults), and all rows periodically so a lost frame or a
 * rebooted board catches up. A board uses a row as soon as it has it;
 * until then that row keeps the no-map default.
 *
 * MISSION_LINK_MAP_WIDTH/HEIGHT restate GRID_WIDTH/GRID_HEIGHT because
 * this header cannot include mission_state.h (see section 5a); core/
 * mission_link.c static-asserts that they match.
 */
#define MISSION_LINK_MAP_WIDTH  36u
#define MISSION_LINK_MAP_HEIGHT 36u
#define MISSION_LINK_MAP_BLOCKED ((uint8_t)0u)

typedef struct __attribute__((packed))
{
    uint8_t row;                                /* grid y */
    uint8_t cells[MISSION_LINK_MAP_WIDTH];      /* indexed by grid x */
} MissionLinkMapRow_t;

#define MISSION_LINK_MAP_ROW_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkMapRow_t)) /* 37 bytes */

#define MISSION_LINK_MAP_ROW_MAGIC0 ((uint8_t)0xDAu)
#define MISSION_LINK_MAP_ROW_MAGIC1 ((uint8_t)0xADu)

#define MISSION_LINK_MAP_ROW_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_MAP_ROW_PAYLOAD_SIZE + 1u))  /* 40 bytes */

/* ---------------------------------------------------------------
 * 5d. MissionLinkSites_t - laptop -> board, safe zones and gateways
 * ---------------------------------------------------------------
 * Where teams rest (begin_return_to_safe_zone()) and where evacuee runs
 * go. Placed once by world generation on the laptop and fixed after
 * that; sent with the rosters and re-sent with them. Evacuee counts
 * are NOT here - they are laptop-side world state (evacuee runs are
 * staged by the laptop, see laptop/mission_mirror.h).
 *
 * Counts restated as literals for the same reason as section 5c.
 */
#define MISSION_LINK_MAX_SAFE_ZONES 4u
#define MISSION_LINK_NUM_GATEWAYS   3u

typedef struct __attribute__((packed))
{
    int16_t loc_x;
    int16_t loc_y;
    uint8_t capacity;
} MissionLinkSafeZoneSite_t;

typedef struct __attribute__((packed))
{
    uint8_t num_safe_zones;
    MissionLinkSafeZoneSite_t safe_zones[MISSION_LINK_MAX_SAFE_ZONES];
    int16_t gateway_x[MISSION_LINK_NUM_GATEWAYS];
    int16_t gateway_y[MISSION_LINK_NUM_GATEWAYS];
} MissionLinkSites_t;

#define MISSION_LINK_SITES_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkSites_t)) /* 33 bytes */

#define MISSION_LINK_SITES_MAGIC0 ((uint8_t)0xE5u)
#define MISSION_LINK_SITES_MAGIC1 ((uint8_t)0x5Eu)

#define MISSION_LINK_SITES_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_SITES_PAYLOAD_SIZE + 1u))    /* 36 bytes */

/* ---------------------------------------------------------------
 * 5e. MissionLinkDistrictRow_t - laptop -> board, the district map
 * ---------------------------------------------------------------
 * Feature-contract Decision 1: the trained model keeps team_zone_id and
 * team_zone_trust as inputs (the earlier research doc's recommendation
 * to drop them was wrong about why - see docs/research/feature_contract_
 * research.md's changelog). zone_id_for_location() (core/search_zones.c)
 * needs district_id per cell to answer team_zone_id on a board, the same
 * way MissionLinkMapRow_t (section 5c) lets environment_cell_travel_
 * cost() answer on a board with no world storage of its own.
 *
 * District ids are 0..NUM_DISTRICT_TYPES-1 (9, core/environment.h) - 4
 * bits is enough, so a row packs 2 cells per byte, half the cost of
 * MissionLinkMapRow_t's one-byte-per-cell. One frame per grid row y:
 * cells[i] holds x=2*i in its low nibble, x=2*i+1 in its high nibble.
 * MISSION_LINK_MAP_WIDTH/HEIGHT (section 5c) are reused rather than a
 * second pair of dimension macros - this is the same 36x36 grid.
 *
 * Sent exactly like the route map: every row at mission start and in
 * mission_feed_tick()'s per-tick pass (laptop/mission_feed.c). district_id
 * is fixed once world generation finishes (verified against core/
 * environment.c: every write site is inside generate_environment()'s own
 * call chain - reset_generation_state()/stage_generate_districts()/
 * assign_anchor()/place_building_rect() - never from environment_apply_
 * earthquake() or any hazard-propagation path), so unlike the route map -
 * whose cells genuinely change from hazard spread etc. - the per-tick
 * diff alone would never re-fire for a board that missed the mission-
 * start burst: nothing ever changes to trigger it. mission_feed_tick()
 * therefore also forces an unconditional full resend of every row every
 * MISSION_LINK_ZONE_TRUST_REFRESH_TICKS, so that board still converges
 * within one refresh period - same reasoning as zone trust below.
 */
typedef struct __attribute__((packed))
{
    uint8_t row;                                       /* grid y */
    uint8_t cells[MISSION_LINK_MAP_WIDTH / 2u];         /* 4 bits/cell, 2 cells/byte */
} MissionLinkDistrictRow_t;

#define MISSION_LINK_DISTRICT_ROW_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkDistrictRow_t)) /* 19 bytes */

#define MISSION_LINK_DISTRICT_ROW_MAGIC0 ((uint8_t)0xB6u)
#define MISSION_LINK_DISTRICT_ROW_MAGIC1 ((uint8_t)0x6Bu)

#define MISSION_LINK_DISTRICT_ROW_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_DISTRICT_ROW_PAYLOAD_SIZE + 1u)) /* 22 bytes */

/* ---------------------------------------------------------------
 * 5f. MissionLinkZoneTrust_t - laptop -> board, search-zone trust inputs
 * ---------------------------------------------------------------
 * search_zone_trust() (core/search_zones.c) is a pure function of
 * SearchZoneState_t.last_swept_tick/.aftershock_hits_since_swept and the
 * current tick - it runs unchanged on a board once g_search_zones[] holds
 * real values instead of the zero-initialised defaults search_zones_
 * init() leaves it at. This carries exactly those two fields, for every
 * zone, in one fixed-size frame (NUM_SEARCH_ZONES is 10 - core/
 * search_zones.h - restated here as a literal for the same reason
 * section 5c restates the grid dimensions: this header cannot include
 * search_zones.h without risking a cycle through mission_state.h/cbba.h;
 * core/mission_link.c static-asserts the two stay equal).
 *
 * Deliberately NOT a precomputed trust value: trust decays continuously
 * (1 point per ZONE_TRUST_DECAY_CAP_TICKS/100 ticks) and jumps on a sweep
 * or an aftershock hit, so a periodically-sent trust NUMBER would already
 * be stale by the time a board used it, and would disagree with the
 * training data's own on-the-fly search_zone_trust() calls. Sending the
 * two raw inputs instead means the board computes the exact same trust
 * value, at the exact tick it needs it, that the laptop would.
 *
 * Sent whenever any zone's last_swept_tick or aftershock_hits_since_swept
 * changes, plus periodically (MISSION_LINK_ZONE_TRUST_REFRESH_TICKS) so a
 * rebooted board or a lost frame doesn't wait indefinitely for the next
 * sweep/aftershock to resync.
 */
#define MISSION_LINK_NUM_ZONES 10u

typedef struct __attribute__((packed))
{
    uint32_t last_swept_tick[MISSION_LINK_NUM_ZONES];
    uint8_t  aftershock_hits_since_swept[MISSION_LINK_NUM_ZONES];
} MissionLinkZoneTrust_t;

#define MISSION_LINK_ZONE_TRUST_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkZoneTrust_t)) /* 50 bytes */

#define MISSION_LINK_ZONE_TRUST_MAGIC0 ((uint8_t)0xA8u)
#define MISSION_LINK_ZONE_TRUST_MAGIC1 ((uint8_t)0x8Au)

#define MISSION_LINK_ZONE_TRUST_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_ZONE_TRUST_PAYLOAD_SIZE + 1u)) /* 53 bytes */

#define MISSION_LINK_ZONE_TRUST_REFRESH_TICKS 100u

/* Board route map (section 5c) equivalent for the district map: returns 1
   and writes the cell's district id to *district_out if row y has arrived
   and (x, y) is on the grid, else 0 (caller keeps its "district 0"
   default - see zone_id_for_location(), core/search_zones.c).
   core/search_zones.c is the only caller. */
uint8_t mission_link_district_cell(int x, int y, uint8_t *district_out);

/* Rows received since mission_link_district_reset() (0..MISSION_LINK_MAP_HEIGHT). */
uint32_t mission_link_district_rows_received(void);

/* Forgets the district map - a board calls this at boot. */
void mission_link_district_reset(void);

/* ---------------------------------------------------------------
 * 5a. Heartbeat cadence for MissionLinkStatus_t's periodic re-anchor
 * ---------------------------------------------------------------
 * Not derived from CBBA_ROUND_TIMEOUT_MS (core/cbba.h) by #include:
 * cbba.h includes mission_state.h, which includes THIS header (for
 * MissionLinkIncident_t/BoardIncidentCache_t - see mission_state.h's
 * own include comment), so mission_link.h -> cbba.h -> mission_state.h
 * -> mission_link.h would be a circular include. Kept as an
 * independent literal instead, picked as a small multiple of
 * CBBA_ROUND_TIMEOUT_MS (1250 ms - recalibrated, see that macro's own
 * doc comment in cbba.h) per this prompt's design note - a multiple,
 * not an unrelated number, so heartbeat traffic and CBBA's own
 * per-round mesh timing budget stay on a shared cadence rather than an
 * arbitrary one that could drift into colliding with it once Prompt
 * 2's channel-busy calibration lands. core/mission_link.c carries a
 * _Static_assert (where cbba.h CAN be safely included, since a .c file
 * has no downstream includers to cycle through) that this stays a
 * multiple of CBBA_ROUND_TIMEOUT_MS - if that ever fires, update the
 * multiplier below rather than the assert.
 *
 * Multiplier kept at 4x (unchanged) when CBBA_ROUND_TIMEOUT_MS was
 * recalibrated from 530ms to 1250ms - the resulting ~5s worst-case
 * staleness bound (was ~2.1s) is a deliberate lengthening of the LoRa
 * heartbeat's own cadence (heartbeat traffic alone was ~23% of channel
 * share at any sim speed - see this project's airtime-budget review),
 * achieved via the SAME recalibration as CBBA_ROUND_TIMEOUT_MS rather
 * than a second, unrelated tuning knob. laptop/tools/telemetry_
 * bridge.py's MISSION_LINK_HEARTBEAT_INTERVAL_SEC mirror and
 * TEAM_STATUS_STALE_SEC (3x that) must be updated to match whenever
 * this changes - there is no automatic sync between this C constant
 * and that Python literal. */
#define MISSION_LINK_HEARTBEAT_INTERVAL_MS ((uint32_t)5000u) /* 1250ms * 4 */

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

/* Board route map (section 5c). Returns 1 and writes the cell's map
   byte to *cell_out if row y has arrived and (x, y) is on the grid,
   else 0 - the caller keeps its no-map default. core/environment.c's
   environment_cell_is_navigable()/_travel_cost() are the callers. */
uint8_t mission_link_map_cell(int x, int y, uint8_t *cell_out);

/* Rows received since mission_link_map_reset() (0..MISSION_LINK_MAP_HEIGHT). */
uint32_t mission_link_map_rows_received(void);

/* Forgets the map - a board calls this at boot. */
void mission_link_map_reset(void);

/* 1 once a MissionLinkSites_t has been applied since mission_link_map_reset(). */
uint8_t mission_link_sites_received(void);

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

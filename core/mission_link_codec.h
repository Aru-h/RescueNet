#ifndef MISSION_LINK_CODEC_H
#define MISSION_LINK_CODEC_H

#include <stdint.h>
#include "mission_link.h"

/*
 * Pack/parse + apply logic for core/mission_link.h's four message
 * types. Split out into this header rather than added to
 * mission_link.h itself because that header is finalized (struct
 * layouts verified against real Cortex-M55 sizes) and, per its own
 * documented convention, is a wire-format-only header with "no
 * encode/decode logic" in it - this file is exactly the "see
 * mission_link.c for pack/unpack + checksum helpers" it points to.
 *
 * Two channels share this codec (see mission_link.c's top-of-file
 * comment for the full rationale):
 *
 *   1. USART1 (laptop <-> board 0 only): shares the wire with
 *      free-running debug text, so frames use mission_link.h's own
 *      magic-byte + checksum framing. Use the mission_link_pack_*() /
 *      mission_link_parse_*() functions below.
 *
 *   2. comms_transport / LoRa (board 0 <-> board 1 relay hop): no
 *      shared-text problem on this hop (lora_frame.h's type+length
 *      envelope already disambiguates), so no magic bytes are spent
 *      here - just a 1-byte sub-type (MISSION_LINK_MSG_*) prepended to
 *      the bare payload bytes. Use the mission_link_pack_*_payload() /
 *      mission_link_parse_*_payload() functions below, which are also
 *      the shared core the magic-byte functions above build on.
 */

/* ---------------------------------------------------------------
 * Sub-type byte for the comms_transport/LoRa relay hop (channel 2
 * above). Distinct from mission_link.h's USART1 magic-byte pairs -
 * this is a single byte prepended to the raw payload, not a two-byte
 * magic pair, since LoRa framing (lora_frame.h) already delimits the
 * packet and doesn't need resync protection against interleaved text.
 * ---------------------------------------------------------------
 */
#define MISSION_LINK_MSG_ROSTER   ((uint8_t)0u)
#define MISSION_LINK_MSG_INCIDENT ((uint8_t)1u)
#define MISSION_LINK_MSG_FAULT    ((uint8_t)2u)
#define MISSION_LINK_MSG_STATUS   ((uint8_t)3u)
#define MISSION_LINK_MSG_CLOCK    ((uint8_t)4u)
#define MISSION_LINK_MSG_MAP_ROW  ((uint8_t)5u)
#define MISSION_LINK_MSG_SITES    ((uint8_t)6u)
/* No MISSION_LINK_MSG_* sub-type for the district map / zone trust
   frames (mission_link.h sections 5e/5f): this is a single-board
   submission (all 10 teams on the one DK), so there is no second board
   to relay toward over comms_transport/LoRa - USART1 only, magic-byte
   framing (channel 1 below), same as MissionLinkClock_t. */

/* Wire size of a comms_transport-relayed message: 1 sub-type byte +
   the bare payload (no magic bytes, no checksum byte - lora_frame.h's
   own envelope already carries a checksum over the whole thing,
   sub-type included). */
#define MISSION_LINK_ROSTER_RELAY_SIZE \
    ((uint8_t)(1u + MISSION_LINK_ROSTER_PAYLOAD_SIZE))
#define MISSION_LINK_INCIDENT_RELAY_SIZE \
    ((uint8_t)(1u + MISSION_LINK_INCIDENT_PAYLOAD_SIZE))
#define MISSION_LINK_FAULT_RELAY_SIZE \
    ((uint8_t)(1u + MISSION_LINK_FAULT_PAYLOAD_SIZE))
#define MISSION_LINK_STATUS_RELAY_SIZE \
    ((uint8_t)(1u + MISSION_LINK_STATUS_PAYLOAD_SIZE))
#define MISSION_LINK_CLOCK_RELAY_SIZE \
    ((uint8_t)(1u + MISSION_LINK_CLOCK_PAYLOAD_SIZE))
#define MISSION_LINK_MAP_ROW_RELAY_SIZE \
    ((uint8_t)(1u + MISSION_LINK_MAP_ROW_PAYLOAD_SIZE))
#define MISSION_LINK_SITES_RELAY_SIZE \
    ((uint8_t)(1u + MISSION_LINK_SITES_PAYLOAD_SIZE))

/* ---------------------------------------------------------------
 * Channel 1: USART1 magic-byte framing (laptop <-> board 0 only)
 * ---------------------------------------------------------------
 * mission_link_pack_*() builds a full frame (magic0, magic1, payload
 * bytes, checksum) into out_buf, which must have room for at least
 * MISSION_LINK_*_WIRE_SIZE bytes. Returns the wire size written.
 *
 * mission_link_parse_*() follows the exact same contract as
 * lora_frame_try_parse(): buf[0] is where a candidate frame of THIS
 * type is expected to start (no internal scanning for the magic pair
 * within buf - that scanning/resync loop, across all four message
 * types plus interleaved debug text, is mission_link_task.c's job on
 * the RX side, same division of labor laptop/tools/telemetry_bridge.py's
 * TelemetryFrameParser has relative to a single-frame-type decoder).
 *   1  a complete, checksum-valid frame of this type was found at
 *      buf[0]. *out is filled in.
 *   0  buf does not yet contain a complete frame (fewer than
 *      MISSION_LINK_*_WIRE_SIZE bytes available). Nothing written.
 *  -1  either the magic bytes at buf[0-1] don't match this message
 *      type, or a complete frame-shaped span was found but its
 *      checksum didn't verify. Caller's resync policy (same as
 *      telemetry_bridge.py's): try the next message type at this
 *      same position, and if none match, drop exactly one byte and
 *      retry - never discard the whole buffer, never treat this as
 *      fatal. Nothing written to *out.
 */
uint32_t mission_link_pack_roster(const MissionLinkRoster_t *msg,
                                   uint8_t *out_buf);
uint32_t mission_link_pack_incident(const MissionLinkIncident_t *msg,
                                     uint8_t *out_buf);
uint32_t mission_link_pack_fault(const MissionLinkFault_t *msg,
                                  uint8_t *out_buf);
uint32_t mission_link_pack_status(const MissionLinkStatus_t *msg,
                                   uint8_t *out_buf);

int mission_link_parse_roster(const uint8_t *buf, uint32_t buf_len,
                               MissionLinkRoster_t *out);
int mission_link_parse_incident(const uint8_t *buf, uint32_t buf_len,
                                 MissionLinkIncident_t *out);
int mission_link_parse_fault(const uint8_t *buf, uint32_t buf_len,
                              MissionLinkFault_t *out);
int mission_link_parse_status(const uint8_t *buf, uint32_t buf_len,
                               MissionLinkStatus_t *out);

/* ---------------------------------------------------------------
 * Channel 2 core: bare-payload pack/parse (no magic bytes, no
 * checksum byte - lora_frame.h's envelope already covers that on the
 * comms_transport/LoRa hop). This is the shared core the channel-1
 * functions above are built from (each mission_link_pack_X() /
 * mission_link_parse_X() is exactly this payload codec plus the
 * magic-byte/checksum wrapper), and it's also what mission_link_
 * task.c calls directly when relaying toward/from comms_transport_
 * send()/comms_transport_poll_received() - see that file for how the
 * MISSION_LINK_MSG_* sub-type byte and this payload codec compose
 * into one comms_transport payload.
 *
 * out_payload/payload buffers are exactly MISSION_LINK_*_PAYLOAD_SIZE
 * bytes - no length parameter needed, unlike the channel-1 functions,
 * since there's no variable amount of leading garbage to skip past
 * here (the caller already isolated the payload bytes, either from a
 * validated USART1 frame or a comms_transport_poll_received() call).
 */
void mission_link_pack_roster_payload(const MissionLinkRoster_t *msg,
                                       uint8_t *out_payload);
void mission_link_pack_incident_payload(const MissionLinkIncident_t *msg,
                                         uint8_t *out_payload);
void mission_link_pack_fault_payload(const MissionLinkFault_t *msg,
                                      uint8_t *out_payload);
void mission_link_pack_status_payload(const MissionLinkStatus_t *msg,
                                       uint8_t *out_payload);

void mission_link_parse_roster_payload(const uint8_t *payload,
                                        MissionLinkRoster_t *out);
void mission_link_parse_incident_payload(const uint8_t *payload,
                                          MissionLinkIncident_t *out);
void mission_link_parse_fault_payload(const uint8_t *payload,
                                       MissionLinkFault_t *out);
void mission_link_parse_status_payload(const uint8_t *payload,
                                        MissionLinkStatus_t *out);

/* ---------------------------------------------------------------
 * What a received message actually does to local board state (see
 * mission_link.h's per-type direction/cadence doc for the "why" of
 * each - this is just where that gets carried out). Shared by both
 * channels: whichever channel a message arrived on, applying it to
 * local state is identical, so mission_link_task.c calls these same
 * three functions regardless of whether the message came straight off
 * USART1 (board 0) or out of comms_transport_poll_received() (board 1,
 * or board 0 receiving an uplinked status - though status is never
 * applied, only forwarded, see mission_link.h: board sends it, never
 * receives it).
 * ---------------------------------------------------------------
 */

/* One-time: sets g_teams[msg->team_id]'s starting identity/capability/
   physical-state fields from the roster handoff. team_id is trusted
   to be < NUM_TEAMS (mission-generation guarantees this on the laptop
   side); out-of-range values are ignored defensively rather than
   indexing g_teams[] out of bounds. */
void mission_link_apply_roster(const MissionLinkRoster_t *msg);

/* Monotonic count of rosters mission_link_apply_roster() rejected whole
   (bad team_id, a location outside the grid, an equipment_level above
   EQ_MAX_LEVEL, or a specialization/fitness_tier outside its enum). Both rejects are silent,
   so a board gating boot on a roster arriving cannot otherwise distinguish
   "rejected" from "never sent" - see each board's own main_board.c
   roster-wait loop, which uses this to report the difference. */
uint32_t mission_link_roster_reject_count(void);

/* Ongoing: updates the board's local partial view of this incident
   (creating a new local slot on first sight - matched by incident_id,
   not by array position, since this board's slot layout has no
   reason to mirror the laptop's). The laptop owns what the incident is
   and its stages/resolution; this board keeps its own execution state
   (assignment, status, work_remaining) while one of its teams is on it -
   see the ownership comment above this function in mission_link.c. */
void mission_link_apply_incident(const MissionLinkIncident_t *msg);

/* Monotonic count of incidents mission_link_apply_incident() could not
   keep because the local table was full: a new incident dropped for want
   of a slot, plus each unworked, less urgent incident evicted to make
   room for a strictly more urgent one (review finding H3). */
uint32_t mission_link_incident_drop_count(void);

/* Monotonic count of incident frames mission_link_apply_incident()
   rejected whole because a field was out of range (location off the
   grid, incident_type/req_capability/priority/status outside their
   enums - review finding H4). */
uint32_t mission_link_incident_reject_count(void);

/* Ongoing: mirrors msg->active[]/severity[] into
   g_teams[msg->team_id].fault_state.active[]/severity[] - the two
   fields the board's team physics reads to apply the fault's effect
   (see mission_link.h's MissionLinkFault_t doc). Detection never reads
   them. Everything else in
   TeamFaultState_t is laptop-only bookkeeping this board never
   touches, left untouched here. */
void mission_link_apply_fault(const MissionLinkFault_t *msg);

/* Builds a status message from team_id's current local state (status,
   equipment_level) - the send side of the one message type this board
   originates rather than receives. Does not itself decide whether to
   send; that change-detection lives in mission_link_task.c, since it
   needs per-team "last sent" bookkeeping that belongs to the task
   driving the send cadence, not to this stateless codec file. */
void mission_link_build_status(uint8_t team_id, MissionLinkStatus_t *out);

/* ---------------------------------------------------------------
 * Mission clock (mission_link.h section 5b)
 * ---------------------------------------------------------------
 */
uint32_t mission_link_pack_clock(const MissionLinkClock_t *msg, uint8_t *out_buf);
int mission_link_parse_clock(const uint8_t *buf, uint32_t buf_len, MissionLinkClock_t *out);
void mission_link_pack_clock_payload(const MissionLinkClock_t *msg, uint8_t *out_payload);
void mission_link_parse_clock_payload(const uint8_t *payload, MissionLinkClock_t *out);

/* Records the laptop's latest tick. Latest frame wins, not highest -
   see mission_link.h section 5b. */
void mission_link_apply_clock(const MissionLinkClock_t *msg);

/* 1 once any clock frame has arrived since mission_link_clock_reset(). */
uint8_t mission_link_clock_received(void);

/* The laptop's latest tick (0 before any clock frame). */
uint32_t mission_link_authorized_tick(void);

/* Forgets the clock - a board calls this at boot. */
void mission_link_clock_reset(void);

/* Decides whether a board with local tick local_tick may run one more
   sim tick given the laptop's authorized_tick. Returns 1 if it may and
   writes the tick it should run FROM into *start_tick: local_tick
   normally, or authorized_tick - 1 when the board is more than
   MISSION_LINK_CLOCK_MAX_CATCHUP_TICKS behind and should jump rather
   than replay. Returns 0 (and leaves *start_tick alone) when the board
   is already at or past the laptop's tick. Pure - no state. */
uint8_t mission_link_clock_step_plan(uint32_t local_tick, uint32_t authorized_tick,
                                     uint32_t *start_tick);

/* ---------------------------------------------------------------
 * Route map and sites (mission_link.h sections 5c/5d)
 * ---------------------------------------------------------------
 */
uint32_t mission_link_pack_map_row(const MissionLinkMapRow_t *msg, uint8_t *out_buf);
int mission_link_parse_map_row(const uint8_t *buf, uint32_t buf_len, MissionLinkMapRow_t *out);
void mission_link_pack_map_row_payload(const MissionLinkMapRow_t *msg, uint8_t *out_payload);
void mission_link_parse_map_row_payload(const uint8_t *payload, MissionLinkMapRow_t *out);

uint32_t mission_link_pack_sites(const MissionLinkSites_t *msg, uint8_t *out_buf);
int mission_link_parse_sites(const uint8_t *buf, uint32_t buf_len, MissionLinkSites_t *out);
void mission_link_pack_sites_payload(const MissionLinkSites_t *msg, uint8_t *out_payload);
void mission_link_parse_sites_payload(const uint8_t *payload, MissionLinkSites_t *out);

/* The map byte for one cell, from the environment's own view: 0 if not
   passable, else its travel cost clamped to 1-255. Used by the laptop
   to build rows, so sender and receiver agree on the encoding. */
uint8_t mission_link_map_encode_cell(uint8_t navigable, uint8_t travel_cost);

/* Stores a row for the board's route map. Rows outside the grid are
   ignored. Returns 1 if applied. */
uint8_t mission_link_apply_map_row(const MissionLinkMapRow_t *msg);

/* Copies the sites into g_safezones[]/g_num_safe_zones/
   g_evacuation_gateways[]. A safe zone's evacuee_count is kept (it is
   not on the wire). A count above MAX_SAFE_ZONES or a site off the grid
   rejects the whole frame. Returns 1 if applied. */
uint8_t mission_link_apply_sites(const MissionLinkSites_t *msg);

/* ---------------------------------------------------------------
 * District map and zone trust (mission_link.h sections 5e/5f)
 * ---------------------------------------------------------------
 */
uint32_t mission_link_pack_district_row(const MissionLinkDistrictRow_t *msg, uint8_t *out_buf);
int mission_link_parse_district_row(const uint8_t *buf, uint32_t buf_len, MissionLinkDistrictRow_t *out);
void mission_link_pack_district_row_payload(const MissionLinkDistrictRow_t *msg, uint8_t *out_payload);
void mission_link_parse_district_row_payload(const uint8_t *payload, MissionLinkDistrictRow_t *out);

uint32_t mission_link_pack_zone_trust(const MissionLinkZoneTrust_t *msg, uint8_t *out_buf);
int mission_link_parse_zone_trust(const uint8_t *buf, uint32_t buf_len, MissionLinkZoneTrust_t *out);
void mission_link_pack_zone_trust_payload(const MissionLinkZoneTrust_t *msg, uint8_t *out_payload);
void mission_link_parse_zone_trust_payload(const uint8_t *payload, MissionLinkZoneTrust_t *out);

/* Packs the 2 district ids covering wire cell index i (x = 2*i low
   nibble, x = 2*i + 1 high nibble) into one byte. district ids above
   0xF are truncated to their low 4 bits - callers pass validated
   district_id values (0..NUM_DISTRICT_TYPES-1 = 0..9), so this never
   fires in practice; not asserted, since a build without world storage
   (a board) never calls this encoder at all (it has no district_id to
   encode - only the laptop, the world authority, builds rows). */
uint8_t mission_link_district_encode_byte(uint8_t district_lo, uint8_t district_hi);

/* Stores a row for the board's district map. Rows outside the grid are
   ignored. Returns 1 if applied. */
uint8_t mission_link_apply_district_row(const MissionLinkDistrictRow_t *msg);

/* Copies last_swept_tick[]/aftershock_hits_since_swept[] into
   g_search_zones[] for every zone (core/search_zones.c) - the two
   fields search_zone_trust() actually reads. Nothing else in
   SearchZoneState_t is touched (sweep_count, covered_cells etc. are
   laptop-side sweep-cycle bookkeeping a board never runs). Returns 1
   if applied (always, for a well-formed frame - there is no field to
   reject a fixed-size, unconditioned array on). */
uint8_t mission_link_apply_zone_trust(const MissionLinkZoneTrust_t *msg);

#endif /* MISSION_LINK_CODEC_H */

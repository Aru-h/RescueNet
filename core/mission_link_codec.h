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

/* Ongoing: updates the board's local partial view of this incident
   (creating a new local slot on first sight - matched by incident_id,
   not by array position, since this board's slot layout has no
   reason to mirror the laptop's). work_remaining is read-only input
   here (CBBA bid quality), never written back - see mission_link.h's
   MissionLinkIncident_t doc for why that's deliberate. */
void mission_link_apply_incident(const MissionLinkIncident_t *msg);

/* Ongoing: mirrors msg->active[]/severity[] into
   g_teams[msg->team_id].fault_state.active[]/severity[] - the two
   fields watchdog_step()/recovery_manager_step() actually read (see
   mission_link.h's MissionLinkFault_t doc). Everything else in
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

#endif /* MISSION_LINK_CODEC_H */

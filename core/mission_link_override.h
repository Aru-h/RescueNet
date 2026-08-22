#ifndef MISSION_LINK_OVERRIDE_H
#define MISSION_LINK_OVERRIDE_H

#include <stdint.h>

/*
 * Manual dashboard override: laptop -> DK (USART1), then DK -> whichever
 * board actually hosts team_id (LoRa relay, comms_transport_send(), if
 * that isn't DK itself). One-shot directive, not part of core/
 * mission_link.h's four simulation-data message types (roster/incident/
 * fault/status) and not folded into that header for the same reason
 * core/mission_link_handshake.h gives for its own liveness probe: this
 * message's shape and lifecycle (operator-issued, no reply, no periodic
 * resend) has nothing in common with how those four are produced and
 * consumed. This is a one-shot directive, not something that expects a
 * reply - the laptop side (tools/app.py) only ever packs this, never
 * parses it; only the receiving board parses it.
 *
 * End-to-end path this header/mission_link_override.c serve:
 *
 *   1. Dashboard drag-drop (tools/templates/index.html) already builds
 *      a pendingOverride{team_id, incident_id} object client-side.
 *      confirmPendingOverride() POSTs it to a new Flask route
 *      (tools/app.py).
 *   2. That route packs a MissionLinkOverride_t via pack_override()
 *      (tools/app.py - a Python mirror of mission_link_override_pack()
 *      below, same self-contained-mirror precedent telemetry_bridge.py
 *      already set for pack_handshake_req()) and writes it to the
 *      laptop<->DK USART1 serial link.
 *   3. DK's USART1 RX path (platform/board/mission_link_task.c - NOT
 *      touched here, hook note only, same "don't touch, flag instead"
 *      rule this project already applies to core/mission_link.h/
 *      mission_link_task.c) calls mission_link_override_parse() on
 *      the incoming bytes.
 *   4. Whoever's receiving code owns team_id decides how to get the
 *      parsed MissionLinkOverride_t to the right board: this file's
 *      own mission_link_override_route_or_apply() (see its doc below)
 *      calls comms_transport_board_id_for_team(msg.team_id) to decide
 *      whether that's this board (apply in-process) or a different
 *      one (relay over comms_transport_send() using
 *      LORA_FRAME_TYPE_OVERRIDE below, the same Option-2 pattern
 *      mission_link.h's own doc comment already established for
 *      roster/incident/fault/status, and the same "flag a new
 *      LORA_FRAME_TYPE_* value here rather than editing lora_frame.h
 *      directly" precedent core/mission_link_handshake.h already set
 *      for LORA_FRAME_TYPE_HANDSHAKE).
 *   5. Either way, the board that actually hosts team_id calls
 *      mission_link_override_apply(), which sets that team's
 *      CBBAAgentState_t.override_active/override_incident_id pin (see
 *      core/mission_state.h's doc comment on those two fields - added
 *      there already, not by this file, per this file's own
 *      coordination note below).
 *   6. core/cbba.c's cbba_build_bundle() (via cbba_apply_override_pin(),
 *      called at the top of that function - NOT part of this file's
 *      ownership, but wired to read exactly the two fields this file
 *      writes) force-injects the pinned incident into that team's
 *      bundle/path ahead of normal marginal-bid competition, and
 *      excludes it from every other team's bidding via the existing
 *      CBBA consensus mechanism (a pinned team's winning_bid is set to
 *      CBBA_OVERRIDE_PIN_BID, a sentinel no normal marginal bid can
 *      beat - see that function's own comment) - so the forced
 *      assignment survives both the current round AND the post-
 *      convergence route-relocation pass (cbba_try_best_relocate()),
 *      which is guarded against moving a team's own actively-pinned
 *      task to another team for a route-utility gain.
 *
 * COORDINATION NOTE: CBBAAgentState_t.override_active/override_
 * incident_id (core/mission_state.h) already exist as of this file's
 * writing - added at the END of that struct specifically to minimize
 * collision risk with concurrent edits to that struct's neighboring
 * code (see that struct's own comment), NOT re-added here. Do not
 * duplicate that field pair from this file; this header only defines
 * the WIRE shape that gets applied to those already-existing fields.
 */

/* ---------------------------------------------------------------
 * 1. MissionLinkOverride_t - laptop -> board, one-shot directive
 * ---------------------------------------------------------------
 * Deliberately minimal: which team, which incident. No priority, no
 * position, no "why" - this is a forced assignment, not a new
 * incident report (that's MissionLinkIncident_t's job, laptop -> board
 * already, unaffected by this message). incident_id (not incident
 * slot) because slot indices are per-board-local allocation order
 * (see mission_link.c's mission_link_find_incident_slot() comment for
 * the same reasoning already established for MissionLinkIncident_t) -
 * the laptop has no reason to know or assume a receiving board's slot
 * layout.
 */
typedef struct __attribute__((packed))
{
    uint8_t  team_id;
    uint32_t incident_id;
} MissionLinkOverride_t;

#define MISSION_LINK_OVERRIDE_PAYLOAD_SIZE \
    ((uint32_t)sizeof(MissionLinkOverride_t)) /* 5 bytes */

/* Confirmed 5 bytes (1 + 4, no padding - team_id's single byte plus
 * three bytes of gcc-inserted alignment padding would be the naive
 * "packed struct still limited by _some_ rule" worry, but
 * __attribute__((packed)) exists exactly to suppress that; a uint8_t
 * immediately followed by a uint32_t inside a packed struct costs
 * nothing extra) by compiling this exact _Static_assert with
 * `arm-none-eabi-gcc -mcpu=cortex-m55 -mthumb` and letting the
 * compiler itself reject the build if it ever stops holding - not
 * hand-calculated, and not left as a one-time manual probe like
 * core/lora_frame.h's CBBAMessage_t note (that struct has multi-byte-
 * aligned fields worth re-checking after any layout change; this one
 * doesn't, so a compile-time assert subsumes the probe here the same
 * way core/mission_link_handshake.h's own _Static_assert lines do for
 * its all-uint8_t structs). */
_Static_assert(sizeof(MissionLinkOverride_t) == 5u,
               "MissionLinkOverride_t must be exactly 5 bytes "
               "(packed: 1-byte team_id + 4-byte incident_id, no padding)");

/* ---------------------------------------------------------------
 * 2. On-wire framing - USART1 magic-byte pair
 * ---------------------------------------------------------------
 * Same shared-UART problem every other magic-byte pair in this
 * project solves (see core/telemetry_frame.h, core/mission_link.h
 * section 5, core/mission_link_handshake.h) - USART1 carries
 * free-running tm_printf ASCII text as well as binary frames, so a
 * resyncing reader needs a magic pair whose first byte can never
 * appear in that text. Every pair already assigned on USART1, cross-
 * checked against 0x87 0x78 below:
 *
 *   telemetry_frame.h    TELEMETRY_FRAME_MAGIC0/1     0xA5 0x5A
 *   mission_link.h        ROSTER_MAGIC0/1              0xC3 0x3C
 *   mission_link.h        INCIDENT_MAGIC0/1            0xD4 0x4D
 *   mission_link.h        FAULT_MAGIC0/1               0xE7 0x7E
 *   mission_link.h        STATUS_MAGIC0/1              0xF9 0x9F
 *   mission_link_handshake.h  HANDSHAKE_MAGIC0/1       0xB6 0x6B
 *   mission_link_override.h   OVERRIDE_MAGIC0/1        0x87 0x78  <-- this file
 *
 * 0x87 matches none of the other first bytes above, and 0x87 != 0x78
 * (the property every pair here needs individually). 0x87 is >0x7E
 * (non-ASCII), which is the property that actually matters for
 * resync safety - tm_printf only ever emits plain ASCII, so a first
 * byte outside that range can never appear in debug output (see
 * mission_link.h section 5's note on this, including its correction
 * of telemetry_frame.h's own comment). MAGIC1 = MAGIC0 with the two
 * nibbles swapped (0x87 -> 0x78), same cosmetic-but-consistent
 * derivation every other pair above already uses (0xA5<->0x5A,
 * 0xC3<->0x3C, 0xD4<->0x4D, 0xE7<->0x7E, 0xF9<->0x9F, 0xB6<->0x6B) -
 * MAGIC1 itself has no non-ASCII requirement to satisfy (0x78 is
 * ASCII 'x', and that's fine - only the first, scanned-for byte needs
 * the property, per the same note).
 *
 * AUTHORITATIVE REGISTRY: this comment block (and the equivalent
 * blocks in mission_link.h/mission_link_handshake.h) is the running
 * list this project keeps to avoid a collision. tools/app.py's
 * OVERRIDE_MAGIC0/1 constants are a Python mirror of THIS row (same
 * self-contained-mirror precedent as pack_handshake_req() already
 * set in telemetry_bridge.py) - keep them in sync if this pair ever
 * changes. (An earlier draft of tools/app.py used 0x28/0x82 for this
 * pair - 0x28 is ASCII '(' and would have broken the non-ASCII-first-
 * byte resync guarantee above; corrected to 0x87/0x78 here and in
 * tools/app.py together, not left silently mismatched between the two
 * sides of the wire.)
 */
#define MISSION_LINK_OVERRIDE_MAGIC0 ((uint8_t)0x87u)
#define MISSION_LINK_OVERRIDE_MAGIC1 ((uint8_t)0x78u)

/* Full USART1 frame: magic0, magic1, payload, checksum - same shape
   every other mission-link-family wire format in this project uses. */
#define MISSION_LINK_OVERRIDE_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_OVERRIDE_PAYLOAD_SIZE + 1u)) /* 8 bytes */

/*
 * LoRa relay frame type for the DK<->NUCLEO hop (comms_transport_
 * send() / lora_frame_pack()), same "flag it here, don't edit
 * lora_frame.h directly" pattern core/mission_link_handshake.h
 * already established for LORA_FRAME_TYPE_HANDSHAKE. core/lora_frame.h
 * currently defines LORA_FRAME_TYPE_CBBA=0 and
 * LORA_FRAME_TYPE_MISSION_LINK=1 directly; mission_link_handshake.h
 * claims value 2 (flagged, not yet folded into lora_frame.h as of
 * this file's writing). Value 3 is unused by any of those, so it's
 * picked here - flagged, same as HANDSHAKE's, for whoever owns
 * lora_frame.h to fold both into that header's own LORA_FRAME_TYPE_*
 * list as the single canonical source once these prompt sets
 * converge, so a fourth file doesn't later pick 2 or 3 again for
 * something unrelated.
 *
 * Why a dedicated value instead of reusing LORA_FRAME_TYPE_MISSION_
 * LINK=1 with a fifth MISSION_LINK_MSG_* sub-type byte (verified
 * directly against core/mission_link_codec.h and platform/board/
 * mission_link_task.c's mission_link_route_or_apply_roster()/_fault()
 * before deciding this - see mission_link_override_route_or_apply()'s
 * own doc for the full comparison): that sub-type enum is
 * mission_link_codec.h's, infrastructure that landed as part of the
 * already-finished four-message mission-link work and isn't this
 * file's to touch. A second, dedicated frame_type sidesteps needing
 * to touch it at all - the frame_type field itself disambiguates on
 * receipt, same role MISSION_LINK_MSG_* plays for the other four, just
 * one layer up.
 */
#define LORA_FRAME_TYPE_OVERRIDE ((uint8_t)3u)

/*
 * Packs msg into out_buf using the USART1 magic-byte layout documented
 * above (magic0, magic1, payload, checksum-over-payload-only). out_buf
 * must have room for at least MISSION_LINK_OVERRIDE_WIRE_SIZE bytes.
 * Returns the number of bytes written (always MISSION_LINK_OVERRIDE_
 * WIRE_SIZE - fixed-size payload, unlike lora_frame_pack()'s variable-
 * length case).
 */
uint32_t mission_link_override_pack(const MissionLinkOverride_t *msg,
                                     uint8_t *out_buf);

/*
 * Parses one MissionLinkOverride_t frame out of buf (buf_len bytes
 * available). Same 1/0/-1 contract as every other mission-link-family
 * parse_*() function in this project (see mission_link.h section 5's
 * doc comment for what each return value means):
 *    1  complete, checksum-valid frame found; *out filled in.
 *    0  buf does not yet contain a complete frame.
 *   -1  wrong magic, or a complete frame-shaped span whose checksum
 *       did not verify.
 */
int mission_link_override_parse(const uint8_t *buf, uint32_t buf_len,
                                 MissionLinkOverride_t *out);

/* ---------------------------------------------------------------
 * Channel 2 core: bare-payload pack/parse (no magic bytes, no
 * checksum - lora_frame.h's own envelope covers that on the LoRa
 * hop), same split every other mission-link-family message uses.
 * ---------------------------------------------------------------
 */
void mission_link_override_pack_payload(const MissionLinkOverride_t *msg,
                                         uint8_t *out_payload);
void mission_link_override_parse_payload(const uint8_t *payload,
                                          MissionLinkOverride_t *out);

/*
 * Applies msg to the board's own CBBA state: sets
 * g_cbba_agent_state[msg->team_id].override_active = 1 and
 * .override_incident_id = msg->incident_id (core/mission_state.h).
 * Out-of-range team_id (>= NUM_TEAMS) is ignored, not a crash - a
 * malformed or stale frame shouldn't take the board down. Does NOT
 * itself check whether this board actually hosts msg->team_id -
 * callers (the USART1 RX hook on DK, or the comms_transport RX poll
 * on whichever board a relayed frame was addressed to) are expected
 * to have already routed the frame to the right board via
 * comms_transport_board_id_for_team() before calling this; this
 * function only ever writes team_id's own slot in g_cbba_agent_state,
 * so a call on the wrong board would just misapply a pin no local
 * cbba_build_bundle() call for that team_id will ever run anyway
 * (this board doesn't build bundles for teams it doesn't host - see
 * cbba_team_is_local()) rather than corrupting anything.
 */
void mission_link_override_apply(const MissionLinkOverride_t *msg);

/*
 * Routes a just-parsed MissionLinkOverride_t to wherever it actually
 * needs to go, per this header's top-of-file pipeline doc (step 4):
 * applies it locally (mission_link_override_apply() above) if this
 * board hosts msg->team_id, or relays it over comms_transport_send()
 * (real LoRa once that driver exists, an in-process queue today - see
 * comms_transport.h) to whichever board does, using
 * LORA_FRAME_TYPE_OVERRIDE above. Named and referenced by tools/
 * app.py's /api/board/override route docstring - that route's own job
 * ends at writing the USART1 frame; THIS function is what decides,
 * once DK's USART1 RX path has parsed that frame, whether team_id
 * lives on this board or needs relaying to NUCLEO.
 *
 * Not called from anywhere in this project yet - the USART1 RX hook
 * on DK (platform/board/mission_link_task.c) is what should call this
 * once it has a freshly-parsed MissionLinkOverride_t, same "hook
 * note, don't touch the file directly" rule this header's top comment
 * already states for that file.
 *
 * SHAPE, not byte-for-byte reuse, of core/mission_link.h's own
 * four-message dual-channel pattern (verified directly against
 * platform/board/mission_link_task.c's mission_link_route_or_apply_
 * roster()/_fault() before writing this): same "apply locally if this
 * board hosts team_id, else relay" decision via comms_transport_
 * board_id_for_team(), same payload-only pack function reused for the
 * relay hop. One deliberate difference: those four types share a
 * single LORA_FRAME_TYPE_MISSION_LINK frame type and disambiguate
 * with a MISSION_LINK_MSG_* sub-type byte (core/mission_link_codec.h)
 * prepended to the payload - extending that shared enum with a fifth
 * value would mean editing mission_link_codec.h, infrastructure that
 * landed as part of that same already-finished four-message work and
 * isn't this file's to touch. LORA_FRAME_TYPE_OVERRIDE above is a
 * second, dedicated frame type instead: the frame_type field itself
 * disambiguates on receipt, so no sub-type byte or relay-size macro
 * is needed here - the relay payload is exactly the bare
 * MISSION_LINK_OVERRIDE_PAYLOAD_SIZE bytes mission_link_override_
 * pack_payload() already produces, unlike MISSION_LINK_ROSTER_
 * RELAY_SIZE etc.'s "1 sub-type byte + payload" shape.
 *
 * Out-of-range team_id (>= NUM_TEAMS) is a no-op, not a crash or a
 * pointless relay attempt - same "ignore, don't crash" policy
 * mission_link_override_apply() already documents for itself.
 *
 * sender_team_id passed to comms_transport_send() is 0u, matching
 * mission_link_route_or_apply_roster()/_fault()'s own established
 * convention for this exact situation (a laptop-originated relay, not
 * a same-board apply) - see comms_transport.h's own doc for why that
 * parameter is local bookkeeping only, never placed on the wire, so
 * there's no real sender team_id to give it and no reason to invent a
 * different placeholder than the one every sibling message type in
 * this project already uses.
 *
 * Returns 0 on success (applied locally, or handed to comms_transport_
 * send() which itself queued/sent it). Returns non-zero only on the
 * relay path, mirroring comms_transport_send()'s own return contract
 * (payload_len too large, or the local inbox full) - a local apply
 * can't itself fail short of the out-of-range case above, which is
 * already a silent no-op, not an error.
 */
int mission_link_override_route_or_apply(const MissionLinkOverride_t *msg);

#endif /* MISSION_LINK_OVERRIDE_H */

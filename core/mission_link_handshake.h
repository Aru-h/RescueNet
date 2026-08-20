#ifndef MISSION_LINK_HANDSHAKE_H
#define MISSION_LINK_HANDSHAKE_H

#include <stdint.h>

/*
 * Liveness handshake, distinct from core/mission_link.h's four
 * simulation-data message types (roster/incident/fault/status). This
 * exists to answer one question before ANY of those four types are
 * safe to send: "is the board on the other end of this wire actually
 * alive and past its own init, or would this frame be arriving at
 * nothing?" See this header's companion .c file for the full
 * request/reply sequence across both hops (USART1 laptop<->DK, LoRa
 * DK<->NUCLEO) and the retry/timeout policy.
 *
 * Not folded into mission_link.h: that header's own doc explicitly
 * scopes it to simulation data, and this message's retry/timeout
 * shape (a liveness probe you resend on a timer until you get an
 * answer or give up) has nothing in common with how roster/incident/
 * fault/status are produced and consumed (each sent once, on a state
 * change, with no notion of "waiting for a reply" at all). Reusing
 * one of those four structs, or bolting a fifth into that same
 * doc/magic-byte table, would conflate two protocols that happen to
 * share a wire, not unify one.
 *
 * ---------------------------------------------------------------
 * Magic byte pair distinctness check (required before treating
 * HANDSHAKE_MAGIC0/1 below as final - see this file's own request):
 * every magic pair already assigned on USART1, cross-checked against
 * 0xB6 0x6B:
 *   telemetry_frame.h   TELEMETRY_FRAME_MAGIC0/1    0xA5 0x5A
 *   mission_link.h       ROSTER_MAGIC0/1             0xC3 0x3C
 *   mission_link.h       INCIDENT_MAGIC0/1           0xD4 0x4D
 *   mission_link.h       FAULT_MAGIC0/1              0xE7 0x7E
 *   mission_link.h       STATUS_MAGIC0/1             0xF9 0x9F
 * 0xB6 0x6B matches none of the above (nor each other, which is the
 * property every one of these pairs individually needs: byte0 !=
 * byte1, true here since 0xB6 != 0x6B). Confirmed distinct - treating
 * this pair as final.
 * ---------------------------------------------------------------
 */
#define HANDSHAKE_MAGIC0 ((uint8_t)0xB6u)
#define HANDSHAKE_MAGIC1 ((uint8_t)0x6Bu)

/* Bump whenever any wire struct in this project changes shape (see
   MissionLinkHandshakeReq_t/Reply_t's protocol_version field doc) -
   this is the value both sides fill that field with. A version
   mismatch is reported (see mission_link_handshake_parse_reply()'s
   caller-facing note in the .c file) but is NOT itself treated as
   "not ready": a live board on a different firmware build is still
   live, just possibly wire-incompatible - that's a decision for
   whoever's driving the handshake result (this file surfaces the
   mismatch, doesn't act on it). */
#define MISSION_LINK_PROTOCOL_VERSION ((uint8_t)1u)

typedef struct __attribute__((packed))
{
    uint8_t board_id;         /* which board this request is addressed
                                  to: 0 = DK, 1 = NUCLEO */
    uint8_t protocol_version; /* the requester's own build's
                                  MISSION_LINK_PROTOCOL_VERSION */
} MissionLinkHandshakeReq_t;

typedef struct __attribute__((packed))
{
    uint8_t board_id;         /* which board this reply is FROM - always
                                  equal to the board_id the matching
                                  MissionLinkHandshakeReq_t was
                                  addressed to; a mismatch would mean
                                  the wrong board answered, which the
                                  caller should treat the same as no
                                  reply at all */
    uint8_t protocol_version; /* the replying board's own build's
                                  MISSION_LINK_PROTOCOL_VERSION */
    uint8_t ready;            /* 1 once this board's own init
                                  (fault_injector_init, watchdog_init,
                                  etc. - see mission_link_handshake.c's
                                  mark_local_ready() doc for exactly
                                  which point this is) has actually
                                  completed, not just once the UART
                                  ISR/LoRa radio is up. 0 is a valid,
                                  meaningful reply value in one case
                                  only: DK's synthetic on-behalf-of-
                                  NUCLEO reply after giving up on ever
                                  hearing from it - see the .c file's
                                  timeout policy. A genuinely-alive
                                  board never sends ready=0 for itself;
                                  it simply doesn't reply yet. */
} MissionLinkHandshakeReply_t;

/* All-uint8_t packed structs: no multi-byte field exists to trigger
   architecture-specific alignment padding in the first place (unlike
   e.g. lora_frame.h's CBBAMessage_t sizing note, which has to worry
   about float/uint32_t alignment and so is verified by compiling a
   probe on the real arm-none-eabi-gcc target and reading the size
   back out of the resulting object file - see that comment for the
   precedent this note deliberately does NOT need to repeat). These
   _Static_assert lines are the equivalent verification for THIS
   case: they fail to compile, on any target including the real
   Cortex-M55 build, the moment this invariant stops holding - a
   compile-time check subsumes a one-time manual probe here, since
   there is no architecture-dependent packing behavior to probe for
   in an all-uint8_t struct. */
_Static_assert(sizeof(MissionLinkHandshakeReq_t) == 2u,
               "MissionLinkHandshakeReq_t must be exactly 2 bytes (all-uint8_t packed struct)");
_Static_assert(sizeof(MissionLinkHandshakeReply_t) == 3u,
               "MissionLinkHandshakeReply_t must be exactly 3 bytes (all-uint8_t packed struct)");

#define MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE ((uint32_t)sizeof(MissionLinkHandshakeReq_t))
#define MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE ((uint32_t)sizeof(MissionLinkHandshakeReply_t))

/* Full USART1 frame: magic0, magic1, payload, checksum - same shape
   mission_link_codec.h's *_WIRE_SIZE macros use. */
#define MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE + 1u))
#define MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE \
    ((uint32_t)(2u + MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE + 1u))

/*
 * LoRa relay frame type for the DK<->NUCLEO hop (comms_transport_
 * send() / lora_frame_pack(), same Option-2 pattern mission_link.h's
 * own doc comment already established for roster/incident/fault/
 * status). core/lora_frame.h currently defines LORA_FRAME_TYPE_CBBA=0
 * and LORA_FRAME_TYPE_MISSION_LINK=1; this file does not add a third
 * value there directly (lora_frame.h belongs to the mission-link
 * relay prompt's file set, not this one - same "don't touch, flag
 * instead" rule this file follows for mission_link.h itself). Value
 * 2 is unused by either of those, so it's picked here and used
 * consistently by both sides of this handshake's LoRa hop; flagged
 * for whoever owns lora_frame.h to fold into that header's own
 * LORA_FRAME_TYPE_* list as the single canonical source once these
 * prompt sets converge, so a THIRD file doesn't later pick 2 again
 * for something unrelated. */
#define LORA_FRAME_TYPE_HANDSHAKE ((uint8_t)2u)

/* ---------------------------------------------------------------
 * Channel 1: USART1 magic-byte framing (laptop <-> DK only)
 * Same 1/0/-1 contract as core/mission_link_codec.h's parse_*()
 * functions - see that header for what each return value means.
 * ---------------------------------------------------------------
 */
uint32_t mission_link_handshake_pack_req(const MissionLinkHandshakeReq_t *msg,
                                          uint8_t *out_buf);
int mission_link_handshake_parse_req(const uint8_t *buf, uint32_t buf_len,
                                      MissionLinkHandshakeReq_t *out);

uint32_t mission_link_handshake_pack_reply(const MissionLinkHandshakeReply_t *msg,
                                            uint8_t *out_buf);
int mission_link_handshake_parse_reply(const uint8_t *buf, uint32_t buf_len,
                                        MissionLinkHandshakeReply_t *out);

/* ---------------------------------------------------------------
 * Channel 2 core: bare-payload pack/parse (no magic bytes, no
 * checksum - lora_frame.h's own envelope covers that on the LoRa
 * hop), shared with channel 1 the same way mission_link_codec.h's
 * *_payload() functions are shared by its two channels.
 * ---------------------------------------------------------------
 */
void mission_link_handshake_pack_req_payload(const MissionLinkHandshakeReq_t *msg,
                                              uint8_t *out_payload);
void mission_link_handshake_parse_req_payload(const uint8_t *payload,
                                               MissionLinkHandshakeReq_t *out);

void mission_link_handshake_pack_reply_payload(const MissionLinkHandshakeReply_t *msg,
                                                uint8_t *out_payload);
void mission_link_handshake_parse_reply_payload(const uint8_t *payload,
                                                 MissionLinkHandshakeReply_t *out);

/* ---------------------------------------------------------------
 * Board-side state machine. Portable (no platform includes) - see
 * mission_link_handshake.c's top-of-file comment for the full
 * request/reply/relay/timeout sequence this implements and the
 * handoff notes for wiring it into main_board.c and
 * platform/board/mission_link_task.c's RX/TX loop (neither touched
 * directly by this file - see this header's own top comment).
 * ---------------------------------------------------------------
 */

/* Tick-budget knobs for the retry/timeout policy - see
   mission_link_handshake.c's top comment for the policy these
   implement and why these particular values. Deliberately macros,
   not compiled-in constants, so a board bring-up build can override
   them (-DHANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS=... etc.) the same
   way platform/board/main_board.c's own BOARD_* knobs are already
   overridable, without editing this file. */
#ifndef HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS
#define HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS 500u
#endif
#ifndef HANDSHAKE_NUCLEO_TIMEOUT_TICKS
#define HANDSHAKE_NUCLEO_TIMEOUT_TICKS 20000u
#endif

/* Call once at boot, after this board knows its own id
   (g_comms_transport_this_board_id - see core/comms_transport.h).
   Resets all handshake state; safe to call again to reset for a
   fresh test run (see tools/handshake_selftest.c). Does NOT seed the
   retry/timeout clock (it takes no `now` parameter) - that clock is
   lazily anchored to the first `now` value passed to
   mission_link_handshake_should_send_nucleo_req() or
   mission_link_handshake_nucleo_timed_out(), since board_init() runs
   at boot before this board necessarily has a meaningful tick source
   to offer (see mission_link_handshake.c's top comment for why the
   NUCLEO leg is expected to start close to boot regardless). */
void mission_link_handshake_board_init(uint8_t this_board_id);

/* Call exactly once, at the point this board's own init has actually
   finished - see mission_link_handshake.c's top comment for the
   specific hook point recommended for main_board.c (a handoff note,
   not a direct edit - see this header's own top comment). Idempotent:
   calling it again after the first time is a harmless no-op. */
void mission_link_handshake_mark_local_ready(void);

/* True once mission_link_handshake_mark_local_ready() has been
   called. */
int mission_link_handshake_local_is_ready(void);

/* Records that a request addressed to THIS board has arrived (from
   USART1 for DK, or from a relayed LoRa MissionLinkHandshakeReq_t for
   NUCLEO). Safe to call repeatedly (each retry from the far end just
   re-latches "someone is waiting"). */
void mission_link_handshake_note_request_received(void);

/* If this board is ready AND a request has been seen (via
   mission_link_handshake_note_request_received()), fills *out with
   this board's reply and returns 1 - the caller (USART1 TX for DK,
   LoRa TX for NUCLEO) should send it. Returns 0 otherwise (not ready
   yet, or nothing has asked). Safe to call every step; deliberately
   NOT "consumed" on a successful call (repeat calls keep returning 1
   once ready+requested) so a caller-side retry-until-acknowledged
   loop, if one exists upstream of this file, has something to
   resend - see mission_link_handshake.c's top comment for why this
   file itself does not track "did the caller actually manage to
   transmit it yet". */
int mission_link_handshake_build_local_reply(MissionLinkHandshakeReply_t *out);

/* ---- DK-side bookkeeping for the NUCLEO leg (board 0 only calls
   these - see mission_link_handshake.c's top comment for why this
   isn't itself gated on g_comms_transport_this_board_id inside this
   file: that's the caller's job, same division of responsibility
   platform/board/mission_link_task.c's own board-0-only USART1 step
   already has). ---- */

/* Whether DK should (re)send its Req to NUCLEO on this step, given
   `now` (the caller's own monotonic tick counter - see this header's
   HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS doc for why this file takes
   it as a parameter instead of reading a global). Returns 1 on the
   very first call after mission_link_handshake_board_init() (board 0
   only - see .c file for the "send on boot, not on-laptop-request"
   policy this implements) and again every
   HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS ticks until either a NUCLEO
   reply has been recorded (mission_link_handshake_note_remote_reply())
   or the timeout has been declared (see
   mission_link_handshake_nucleo_timed_out() below) - once timed out,
   this permanently returns 0 (no more Reqs into the void). */
int mission_link_handshake_should_send_nucleo_req(uint32_t now);

/* True once HANDSHAKE_NUCLEO_TIMEOUT_TICKS have elapsed since
   mission_link_handshake_board_init() with no NUCLEO reply recorded.
   Sticky: once true, stays true (does not un-time-out if a very late
   reply eventually shows up - see .c file's policy note for why a
   late reply is still applied to local state even after this point,
   just no longer chased after). */
int mission_link_handshake_nucleo_timed_out(uint32_t now);

/* Records a reply received from NUCLEO over LoRa. DK caches this and
   mission_link_handshake_get_relay_reply() below starts returning it. */
void mission_link_handshake_note_remote_reply(const MissionLinkHandshakeReply_t *reply);

/* Fills *out with the reply DK should forward to the laptop over
   USART1 right now and returns 1, or returns 0 if there's nothing to
   forward yet. Three cases (see .c file for the full decision table):
   an actual NUCLEO reply was recorded (out->ready reflects NUCLEO's
   own value); the timeout fired with no reply ever recorded (out is
   the synthetic board_id=1/ready=0 "presumed absent" reply - see this
   header's MissionLinkHandshakeReply_t.ready doc); or neither has
   happened yet (returns 0, nothing to relay). Like
   mission_link_handshake_build_local_reply(), not "consumed" on
   success - repeat calls keep returning 1 so an upstream retry loop
   can resend if the previous USART1 transmit didn't get
   acknowledged/observed. */
int mission_link_handshake_get_relay_reply(uint32_t now, MissionLinkHandshakeReply_t *out);

#endif /* MISSION_LINK_HANDSHAKE_H */

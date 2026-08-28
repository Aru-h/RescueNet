#include "mission_link_handshake.h"

/*
 * Implements the codec + state machine declared in
 * mission_link_handshake.h. Portable core file: no platform includes,
 * the same constraint every other core .c file compiles under (see
 * Makefile's check-board target) - see core/mission_link.c's
 * top-of-file comment for the fuller rationale, which applies
 * identically here.
 *
 * ---------------------------------------------------------------
 * Full sequence (mirrors the six numbered steps in the prompt this
 * file answers - restated here so the "why" for each design choice
 * below has the sequence it's justifying right next to it):
 * ---------------------------------------------------------------
 *
 * 1. Laptop sends MissionLinkHandshakeReq_t{board_id=0} to DK over
 *    USART1 directly, on its own retry timer (see
 *    laptop/tools/telemetry_bridge.py's HandshakeCoordinator for that side's
 *    policy - this file only implements the board side).
 *
 * 2. DK, on receiving that Req (mission_link_handshake_note_request_
 *    received()), replies with Reply{board_id=0, ready=1} as soon as
 *    - and not before - mission_link_handshake_local_is_ready() is
 *    true. If the Req arrives before DK's own init has finished,
 *    mission_link_handshake_build_local_reply() simply returns 0
 *    (nothing to send yet) until mission_link_handshake_mark_local_
 *    ready() is called, at which point it starts returning 1. This
 *    is deliberately "latch and wait", not "reply immediately with
 *    ready=0 and let the laptop poll": a ready=0 reply from a board
 *    that IS going to come up shortly is indistinguishable, on the
 *    wire, from a board that's dead - better to simply not answer
 *    until there is a true affirmative answer, and let the laptop's
 *    own retry timer (which it needs anyway, for the case where the
 *    first Req is lost to line noise before DK even receives it)
 *    cover the "still booting" case for free.
 *
 * 3. DK sends its own Req{board_id=1} to NUCLEO over LoRa
 *    (comms_transport_send() + LORA_FRAME_TYPE_HANDSHAKE, payload =
 *    mission_link_handshake_pack_req_payload()'s bytes - the same
 *    Option-2 relay pattern mission_link.h's own doc comment
 *    established) ON DK'S OWN BOOT
 *    (mission_link_handshake_should_send_nucleo_req() returns 1 on
 *    the very first call after mission_link_handshake_board_init()),
 *    NOT gated on first receiving the laptop's own Req. Chosen
 *    because the two legs' expected latencies are wildly different
 *    and largely independent: USART1's laptop<->DK round trip is
 *    microseconds once the laptop starts asking, but "once the
 *    laptop starts asking" itself depends on a human opening the
 *    dashboard, which could be seconds after DK powers on - while
 *    the LoRa DK<->NUCLEO round trip depends on NUCLEO's own boot +
 *    init time, comparable to DK's own. Serializing "wait for laptop
 *    Req" before "ask NUCLEO" would needlessly add the (unbounded,
 *    human-paced) first delay on top of the second, for a resource
 *    (radio airtime for one small Req) cheap enough that starting it
 *    unconditionally at boot costs nothing if the laptop happens to
 *    connect quickly and DK was going to send it almost immediately
 *    anyway.
 *
 * 4. NUCLEO's own instance of this same state machine
 *    (mission_link_handshake_note_request_received() +
 *    mission_link_handshake_build_local_reply(), exactly like DK's
 *    own USART1-facing logic in step 2, just carried over LoRa
 *    instead) replies once ITS init is done. DK records that reply
 *    (mission_link_handshake_note_remote_reply()) and relays it to
 *    the laptop over USART1 as a full magic-byte frame
 *    (mission_link_handshake_pack_reply(), board_id=1 as NUCLEO set
 *    it) via mission_link_handshake_get_relay_reply() - the mirror of
 *    the down-link relay pattern board/STM32N6570-DK/mission_link_task.c
 *    already established for mission-link traffic proper (that file
 *    forwards a relayed MissionLinkStatus_t from board 1 out over
 *    USART1 the same way; this does the analogous thing one level
 *    earlier in the boot sequence, for this handshake's own reply
 *    type instead).
 *
 * 5. (Laptop-side aggregation: implemented in
 *    laptop/tools/telemetry_bridge.py's HandshakeCoordinator, not this
 *    file - the laptop only starts sending roster/incident/fault
 *    traffic, and app.py only enables the dashboard's "start
 *    simulation" control, once it holds a reply for both board_id 0
 *    and 1. See that file's own top comment for the out-of-order-
 *    arrival guarantee this depends on.)
 *
 * 6. Timeout/retry policy - stated explicitly, per the prompt's own
 *    requirement not to leave this unstated:
 *      - DK resends its Req to NUCLEO every
 *        HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS ticks
 *        (mission_link_handshake_should_send_nucleo_req()) until
 *        either a real reply arrives or the timeout below fires.
 *      - After HANDSHAKE_NUCLEO_TIMEOUT_TICKS ticks with no NUCLEO
 *        reply ever recorded, DK gives up asking
 *        (mission_link_handshake_nucleo_timed_out() latches true,
 *        should_send_nucleo_req() permanently returns 0 from then
 *        on - no point spending radio airtime on a board that isn't
 *        answering) AND synthesizes a Reply{board_id=1, ready=0} of
 *        its own on NUCLEO's behalf, so the laptop gets a definitive
 *        "NUCLEO is presumed absent" signal instead of silence it
 *        has no way to distinguish from "still trying, give it more
 *        time". A late reply that arrives after the timeout has
 *        already fired is still recorded and still relayed (real
 *        data is always better than the synthetic placeholder, and
 *        there is no harm in updating the laptop's picture even
 *        late) - it just doesn't un-fire the timeout or resume
 *        resending Reqs, since DK has already told the laptop what
 *        it needed to know to make a decision.
 *      - What the LAPTOP does with a ready=0 board_id=1 reply (block
 *        forever vs. time out vs. offer a DK-only path) is that
 *        side's policy call, made in laptop/tools/telemetry_bridge.py's
 *        HandshakeCoordinator - see that file's top comment. Short
 *        version: it does NOT block forever, and it does NOT
 *        silently proceed DK-only either - it surfaces the timeout to
 *        the operator (app.py's /api/status) and requires an explicit
 *        "continue without NUCLEO" action rather than either
 *        extreme, since a demo running silently degraded is worse
 *        than one that makes the operator confirm it.
 */

/* Additive checksum, identical scheme to core/mission_link.c's
   mission_link_checksum() (and telemetry_frame.h / lora_frame.c
   before it) - re-implemented locally rather than shared across a
   translation unit boundary for the same reason mission_link.c gives
   for not sharing with lora_frame.c: it's four lines, and a shared
   helper would cost a header dependency between two otherwise
   unrelated protocol files for no real reuse benefit. */
static uint8_t handshake_checksum(const uint8_t *bytes, uint32_t len)
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
 */

void mission_link_handshake_pack_req_payload(const MissionLinkHandshakeReq_t *msg,
                                              uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_handshake_parse_req_payload(const uint8_t *payload,
                                               MissionLinkHandshakeReq_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

void mission_link_handshake_pack_reply_payload(const MissionLinkHandshakeReply_t *msg,
                                                uint8_t *out_payload)
{
    const uint8_t *src = (const uint8_t *)msg;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE; i++)
    {
        out_payload[i] = src[i];
    }
}

void mission_link_handshake_parse_reply_payload(const uint8_t *payload,
                                                 MissionLinkHandshakeReply_t *out)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t i;

    for (i = 0u; i < MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE; i++)
    {
        dst[i] = payload[i];
    }
}

/* ---------------------------------------------------------------
 * Channel 1: USART1 magic-byte framing
 * ---------------------------------------------------------------
 */

uint32_t mission_link_handshake_pack_req(const MissionLinkHandshakeReq_t *msg,
                                          uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = HANDSHAKE_MAGIC0;
    out_buf[1] = HANDSHAKE_MAGIC1;

    mission_link_handshake_pack_req_payload(msg, payload_start);

    out_buf[2u + MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE] =
        handshake_checksum(payload_start, MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE);

    return MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE;
}

int mission_link_handshake_parse_req(const uint8_t *buf, uint32_t buf_len,
                                      MissionLinkHandshakeReq_t *out)
{
    uint8_t expected_checksum;

    if (buf_len < 2u)
    {
        return 0;
    }

    if (buf[0] != HANDSHAKE_MAGIC0 || buf[1] != HANDSHAKE_MAGIC1)
    {
        return -1;
    }

    if (buf_len < MISSION_LINK_HANDSHAKE_REQ_WIRE_SIZE)
    {
        return 0;
    }

    expected_checksum =
        handshake_checksum(&buf[2], MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE);

    if (buf[2u + MISSION_LINK_HANDSHAKE_REQ_PAYLOAD_SIZE] != expected_checksum)
    {
        return -1;
    }

    mission_link_handshake_parse_req_payload(&buf[2], out);

    return 1;
}

uint32_t mission_link_handshake_pack_reply(const MissionLinkHandshakeReply_t *msg,
                                            uint8_t *out_buf)
{
    uint8_t *payload_start = &out_buf[2];

    out_buf[0] = HANDSHAKE_MAGIC0;
    out_buf[1] = HANDSHAKE_MAGIC1;

    mission_link_handshake_pack_reply_payload(msg, payload_start);

    out_buf[2u + MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE] =
        handshake_checksum(payload_start, MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE);

    return MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE;
}

/*
 * NOTE on Req vs. Reply sharing one magic pair: unlike
 * mission_link.h's four types (each with its own magic pair, since
 * all four can appear interleaved on the same USART1 stream and need
 * to be told apart by magic bytes alone), Req and Reply here share
 * HANDSHAKE_MAGIC0/1. This is safe because, unlike that four-way
 * case, a parser never needs to disambiguate "is this a Req or a
 * Reply" from magic bytes alone in the first place: WIRE_SIZE differs
 * (2 vs. 3 byte payload, so 5 vs. 6 byte total wire size), and more
 * importantly each side of this handshake only ever RECEIVES one of
 * the two shapes (the laptop only ever receives Replies over USART1;
 * DK only ever receives a Req over USART1, from the laptop - it gets
 * NUCLEO's Reply over LoRa, a completely different channel with its
 * own framing, not USART1 at all). A caller that knows which shape
 * it's expecting calls the matching parse function directly; there is
 * no scanning context where both shapes could plausibly appear back
 * to back needing byte-level disambiguation the way mission_link.h's
 * four receivable-on-one-wire types do.
 */
int mission_link_handshake_parse_reply(const uint8_t *buf, uint32_t buf_len,
                                        MissionLinkHandshakeReply_t *out)
{
    uint8_t expected_checksum;

    if (buf_len < 2u)
    {
        return 0;
    }

    if (buf[0] != HANDSHAKE_MAGIC0 || buf[1] != HANDSHAKE_MAGIC1)
    {
        return -1;
    }

    if (buf_len < MISSION_LINK_HANDSHAKE_REPLY_WIRE_SIZE)
    {
        return 0;
    }

    expected_checksum =
        handshake_checksum(&buf[2], MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE);

    if (buf[2u + MISSION_LINK_HANDSHAKE_REPLY_PAYLOAD_SIZE] != expected_checksum)
    {
        return -1;
    }

    mission_link_handshake_parse_reply_payload(&buf[2], out);

    return 1;
}

/* ---------------------------------------------------------------
 * Board-side state machine
 * ---------------------------------------------------------------
 */

static uint8_t g_this_board_id;
static uint8_t g_local_ready;
static uint8_t g_request_seen;

/* Relay-leg bookkeeping, per peer board_id - GENERALIZED from single
   nucleo-only fields to arrays indexed directly by board_id (index
   g_this_board_id's own slot is simply never touched by the relay
   functions, since a board never relays to itself - no relabeling
   needed to go from "the one NUCLEO" to "peer N"). Harmless, unused
   past index 0, if this board isn't the relay initiator (see this
   file's top comment and this header's own doc for why the
   board-0-only gating is the CALLER's job, not enforced internally
   here). Capped at HANDSHAKE_MAX_BOARDS (see that macro's own doc in
   the header) - a peer_board_id >= this cap is out of range for every
   function below and is either silently ignored (note_remote_reply)
   or would read/write out of bounds if a caller passed one anyway
   (the other three assert-worthy but this file has no assert
   facility available in a freestanding build - callers are expected
   to only ever pass values from comms_transport_board_count(), which
   by construction never exceeds this cap unless someone raises
   NUM_TEAMS far past what HANDSHAKE_MAX_BOARDS was sized for, in
   which case raising this cap to match is the fix, not a bounds
   check here). */
static uint8_t g_relay_req_sent_once[HANDSHAKE_MAX_BOARDS];
static uint8_t g_relay_req_last_sent_valid[HANDSHAKE_MAX_BOARDS];
static uint32_t g_relay_req_last_sent_tick[HANDSHAKE_MAX_BOARDS];
static uint8_t g_relay_reply_seen[HANDSHAKE_MAX_BOARDS];
static MissionLinkHandshakeReply_t g_relay_reply[HANDSHAKE_MAX_BOARDS];
static uint8_t g_relay_timed_out[HANDSHAKE_MAX_BOARDS];

/* Shared across every peer, not per-peer: all relay legs are expected
   to start around the same time, at boot (see this file's top
   comment) - a single clock, seeded once, is enough to time out every
   peer independently off the same zero point. */
static uint32_t g_board_init_tick; /* `now` value at
                                       mission_link_handshake_board_
                                       init() - the timeout clock's
                                       zero point */
static uint8_t g_board_init_tick_valid;

void mission_link_handshake_board_init(uint8_t this_board_id)
{
    uint8_t i;

    g_this_board_id = this_board_id;
    g_local_ready = 0u;
    g_request_seen = 0u;

    g_board_init_tick_valid = 0u;
    g_board_init_tick = 0u;

    for (i = 0u; i < HANDSHAKE_MAX_BOARDS; i++)
    {
        g_relay_req_sent_once[i] = 0u;
        g_relay_req_last_sent_valid[i] = 0u;
        g_relay_req_last_sent_tick[i] = 0u;
        g_relay_reply_seen[i] = 0u;
        g_relay_timed_out[i] = 0u;

        g_relay_reply[i].board_id = i;
        g_relay_reply[i].protocol_version = 0u;
        g_relay_reply[i].ready = 0u;
    }
}

void mission_link_handshake_mark_local_ready(void)
{
    g_local_ready = 1u;
}

int mission_link_handshake_local_is_ready(void)
{
    return (int)g_local_ready;
}

void mission_link_handshake_note_request_received(void)
{
    g_request_seen = 1u;
}

int mission_link_handshake_build_local_reply(MissionLinkHandshakeReply_t *out)
{
    if (!g_local_ready || !g_request_seen)
    {
        return 0;
    }

    out->board_id = g_this_board_id;
    out->protocol_version = MISSION_LINK_PROTOCOL_VERSION;
    out->ready = 1u;

    return 1;
}

/* Lazily seeds the timeout clock's zero point from the first `now`
   value either should_send_relay_req() or relay_timed_out() ever
   sees, for ANY peer (the clock is shared - see the field's own doc
   above). Not seeded directly in board_init(): that function takes no
   `now` parameter (board_init() runs at boot, before this board
   necessarily has a meaningful tick source at all - see this file's
   top comment on when DK starts relay legs vs. when the sim tick
   loop itself starts), so the first call that actually has a real
   `now` value to offer is the right place to anchor "elapsed since
   we started trying". */
static void mission_link_handshake_seed_timeout_clock_if_needed(uint32_t now)
{
    if (!g_board_init_tick_valid)
    {
        g_board_init_tick = now;
        g_board_init_tick_valid = 1u;
    }
}

static void mission_link_handshake_note_relay_req_sent(uint8_t peer_board_id, uint32_t now)
{
    g_relay_req_sent_once[peer_board_id] = 1u;
    g_relay_req_last_sent_valid[peer_board_id] = 1u;
    g_relay_req_last_sent_tick[peer_board_id] = now;
}

int mission_link_handshake_should_send_relay_req(uint8_t peer_board_id, uint32_t now)
{
    int due;

    mission_link_handshake_seed_timeout_clock_if_needed(now);

    if (g_relay_reply_seen[peer_board_id] || g_relay_timed_out[peer_board_id])
    {
        return 0;
    }

    if (!g_relay_req_sent_once[peer_board_id])
    {
        due = 1; /* send-on-boot, per this file's top-comment policy */
    }
    else if (!g_relay_req_last_sent_valid[peer_board_id])
    {
        due = 1; /* defensive: sent_once true but no timestamp recorded somehow */
    }
    else
    {
        /* Unsigned subtraction handles `now` wraparound the same way
           g_current_tick-style monotonic counters elsewhere in this
           codebase are already assumed to (tick counters here are
           never expected to wrap in practice, but this is free to
           get right). */
        due = ((now - g_relay_req_last_sent_tick[peer_board_id]) >= HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS) ? 1 : 0;
    }

    if (due)
    {
        /* Per this function's header doc: a 1 return means "send it
           now AND the timer has already been reset for you" - the
           caller is not expected to separately report back that it
           sent the Req. */
        mission_link_handshake_note_relay_req_sent(peer_board_id, now);
    }

    return due;
}

int mission_link_handshake_relay_timed_out(uint8_t peer_board_id, uint32_t now)
{
    if (g_relay_timed_out[peer_board_id])
    {
        return 1;
    }

    mission_link_handshake_seed_timeout_clock_if_needed(now);

    if (g_relay_reply_seen[peer_board_id])
    {
        return 0;
    }

    if ((now - g_board_init_tick) >= HANDSHAKE_NUCLEO_TIMEOUT_TICKS)
    {
        g_relay_timed_out[peer_board_id] = 1u;
        return 1;
    }

    return 0;
}

void mission_link_handshake_note_remote_reply(const MissionLinkHandshakeReply_t *reply)
{
    if (reply->board_id >= HANDSHAKE_MAX_BOARDS)
    {
        /* Out of range for this build's cap - see that macro's own
           doc for why this is silently ignored rather than a bounds
           violation: a reply this file was never sized to track
           can't be "recorded" anywhere safe to later read back. */
        return;
    }

    g_relay_reply[reply->board_id] = *reply;
    g_relay_reply_seen[reply->board_id] = 1u;
}

int mission_link_handshake_get_relay_reply(uint8_t peer_board_id, uint32_t now,
                                            MissionLinkHandshakeReply_t *out)
{
    if (g_relay_reply_seen[peer_board_id])
    {
        *out = g_relay_reply[peer_board_id];
        return 1;
    }

    if (mission_link_handshake_relay_timed_out(peer_board_id, now))
    {
        /* Synthetic "presumed absent" reply - see this file's
           top-comment timeout policy for why board_id=peer_board_id/
           ready=0 here specifically, and why this is safe to keep
           returning on every call (mirrors mission_link_handshake_
           build_local_reply()'s own "not consumed" contract). */
        out->board_id = peer_board_id;
        out->protocol_version = MISSION_LINK_PROTOCOL_VERSION;
        out->ready = 0u;
        return 1;
    }

    return 0;
}

int mission_link_handshake_relay_reply_seen(uint8_t peer_board_id)
{
    /* Deliberately does NOT call mission_link_handshake_relay_timed_
       out() or touch g_board_init_tick/cbba_now_ms() in any way - see
       this function's own doc in the header for exactly why that
       matters. A plain array read, nothing else. */
    return (int)g_relay_reply_seen[peer_board_id];
}

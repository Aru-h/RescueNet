#include "comms_transport.h"
#include "mission_state.h" /* g_current_tick */

/* See comms_transport.h's "Handoff notes" - platform bring-up code
   must set this to this board's real id before calling
   comms_transport_init(). Defaults to 0 so single-board laptop/desktop
   builds and standalone tests need no setup at all. */
uint8_t g_comms_transport_this_board_id = 0u;

/*
 * SX1262 SPI driver hook - NOT implemented here; that's separate,
 * still-pending bring-up work. This weak default lets comms_transport.c
 * (and anything that links against it - this file's own standalone
 * test, check-board-compile/link) build and run before that driver
 * exists. Once a real, strong (non-weak) lora_spi_transmit() is linked
 * in from the SPI driver's own .c file, the linker resolves every call
 * to that definition instead of this stub automatically - nothing here
 * needs to change.
 *
 * Signature: hands over exactly the packed bytes lora_frame_pack()
 * produced (frame_type + payload_len + payload + checksum, per
 * lora_frame.h), ready to key the radio and transmit as-is. Returns 0
 * on successful hand-off to the radio, non-zero on failure (e.g.
 * busy/NACK) - matching comms_transport_send()'s own 0/non-zero
 * convention, so this file can just pass the return value straight
 * through.
 */
extern int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len);

__attribute__((weak)) int lora_spi_transmit(const uint8_t *frame_bytes,
                                             uint32_t frame_len)
{
    (void)frame_bytes;
    (void)frame_len;

    /* No radio yet - treat as "accepted for transmission" so nothing
       above this layer has to special-case "no driver present." */
    return 0;
}

/*
 * ---------------------------------------------------------------
 * Shared channel-busy accounting
 * ---------------------------------------------------------------
 * One value, shared across both the same-board and cross-board paths,
 * so a benchmark run that happens to keep every team on one board
 * can't look artificially cheap next to a real spread-out N-board
 * run - see comms_transport.h's "Channel contention model" note.
 */

#ifndef COMMS_TRANSPORT_CHANNEL_BYTES_PER_TICK
/*
 * How many on-air bytes "fit" in one simulated cost tick. Calibrated
 * against a real SX1262 LoRa time-on-air (ToA) figure - see the
 * derivation below - not hand-picked. Still an approximation (see the
 * caveats at the end), but a calibrated one now, not the "8u,
 * proportional to size, no real number behind it" placeholder this
 * replaced.
 *
 * ToA (Semtech AN1200.13 formula), for SF7 / BW125kHz / coding rate
 * 4/5 / explicit header / payload CRC on / 8-symbol preamble, sized
 * against the largest single frame this project actually sends -
 * LORA_FRAME_MAX_WIRE_SIZE (lora_frame.h): 3 (header) + 144 (a full
 * CBBAMessage_t payload, core/cbba.c) + 19 (AEAD trailer: Poly1305
 * tag + counter fragment + checksum - see lora_frame.h's
 * "Encryption" section) = 166 bytes. RE-DERIVED from the original
 * 147-byte figure after core/lora_frame.c grew an AEAD envelope (see
 * core/lora_crypto.h/core/aead_chacha20poly1305.h) - re-run this same
 * formula again if LORA_FRAME_MAX_WIRE_SIZE ever changes again, same
 * "don't let it silently drift" policy as lora_frame.h's own
 * CBBAMessage_t-size comment:
 *
 *   Tsym          = 2^SF / BW = 2^7 / 125000               = 1.024 ms
 *   Tpreamble     = (8 + 4.25) * Tsym                       = 12.544 ms
 *   payloadSymbNb = 8 + ceil((8*166 - 4*7 + 28 + 16) / (4*7)) * 5
 *                 = 8 + ceil(1344 / 28) * 5 = 8 + 48*5      = 248 symbols
 *   Tpayload      = 248 * Tsym                              = 253.952 ms
 *   Ttotal        = Tpreamble + Tpayload                    = 266.496 ms
 *
 * i.e. ~623 bytes/sec for a 166-byte frame at SF7. Converted to this
 * project's sim-tick unit via TICK_TIME_SEC=36 (mission_state.h - one
 * sim tick represents 36 real seconds, so a sub-second real LoRa
 * round-trip has to come out "fast" against a 36-second tick, not
 * "free"):
 *
 *   BYTES_PER_TICK = (166 bytes / 0.266496 s) * 36 s/tick ~= 22424
 *
 * Caveats, stated rather than hidden: this is still a single linear
 * bytes-per-tick constant standing in for a genuinely non-linear ToA
 * curve (fixed preamble cost + per-symbol quantization, not a
 * straight line through the origin) - exact at the 166-byte point it
 * was calibrated against, an approximation elsewhere. It also bakes
 * in SF7, an 8-symbol preamble, coding rate 4/5, explicit header, and
 * CRC on - the actual register-level radio configuration is still
 * SPI/radio driver work, still pending bring-up (see comms_transport.h's
 * "Open question" note). Whoever owns that driver should re-derive
 * this number, using the same formula above, once those settings are
 * chosen and confirmed on real hardware. +1u below still guarantees
 * every send costs at least one tick even for a tiny payload, so the
 * channel is never free regardless of size. */
#define COMMS_TRANSPORT_CHANNEL_BYTES_PER_TICK 22424u
#endif

static uint32_t g_channel_busy_until_tick;

/* Advances g_channel_busy_until_tick by this send's cost, starting
   from whichever is later - the channel's current occupancy, or right
   now - so back-to-back sends serialize (queue) behind one another
   instead of each independently claiming "free until wire_len/8 ticks
   from now" as if it were the only send happening. */
static void comms_transport_advance_channel_busy(uint32_t wire_len)
{
    uint32_t cost_ticks = 1u + (wire_len / COMMS_TRANSPORT_CHANNEL_BYTES_PER_TICK);
    uint32_t start_tick = (g_channel_busy_until_tick > g_current_tick)
                               ? g_channel_busy_until_tick
                               : g_current_tick;

    g_channel_busy_until_tick = start_tick + cost_ticks;
}

uint32_t comms_transport_channel_busy_until_tick(void)
{
    return g_channel_busy_until_tick;
}

/*
 * ---------------------------------------------------------------
 * Same-board inbox
 * ---------------------------------------------------------------
 * One shared FIFO for anything addressed to any team resident on this
 * board (a board can host up to 5 teams under the team_id/5 rule).
 * Fixed capacity, reject-when-full - unlike the USART1 telemetry
 * ring's overwrite-oldest policy, silently dropping a mission message
 * would be the wrong failure mode here, so a full inbox is reported
 * back to the sender as a failure instead.
 */

#ifndef COMMS_TRANSPORT_INBOX_CAPACITY
#define COMMS_TRANSPORT_INBOX_CAPACITY 16u
#endif

typedef struct
{
    uint8_t sender_team_id;
    uint8_t frame_type;
    uint8_t payload_len;
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
} CommsInboxEntry_t;

static CommsInboxEntry_t g_inbox[COMMS_TRANSPORT_INBOX_CAPACITY];
static uint32_t g_inbox_head;  /* next slot to write */
static uint32_t g_inbox_tail;  /* next slot to read */
static uint32_t g_inbox_count;

static int comms_transport_inbox_push(uint8_t sender_team_id,
                                       uint8_t frame_type,
                                       const uint8_t *payload,
                                       uint8_t payload_len)
{
    CommsInboxEntry_t *slot;
    uint8_t i;

    if (g_inbox_count == COMMS_TRANSPORT_INBOX_CAPACITY)
    {
        return -1; /* queue full */
    }

    slot = &g_inbox[g_inbox_head];
    slot->sender_team_id = sender_team_id;
    slot->frame_type = frame_type;
    slot->payload_len = payload_len;

    for (i = 0u; i < payload_len; i++)
    {
        slot->payload[i] = payload[i];
    }

    g_inbox_head = (g_inbox_head + 1u) % COMMS_TRANSPORT_INBOX_CAPACITY;
    g_inbox_count++;

    return 0;
}

/*
 * ---------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------
 */

void comms_transport_init(void)
{
    g_inbox_head = 0u;
    g_inbox_tail = 0u;
    g_inbox_count = 0u;

    /* Channel starts idle as of right now, not "busy until 0" -
       matters if comms_transport_init() is ever called mid-run
       (e.g. between test phases) rather than only at boot. */
    g_channel_busy_until_tick = g_current_tick;
}

#ifndef COMMS_TRANSPORT_TEAMS_PER_BOARD
/* team_id / this = board_id. Real per-board firmware keeps the
   default of 5 (matches NUM_TEAMS / 2 boards). A laptop build
   simulating all NUM_TEAMS in one process overrides this via
   -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS so every team resolves
   to board 0 and takes the same-board path uniformly - without this,
   teams 5-9 silently route to the stubbed lora_spi_transmit() instead
   of the local inbox the moment anything (CBBA, mission-link) sends
   through this layer in a laptop-only run. */
#define COMMS_TRANSPORT_TEAMS_PER_BOARD 5u
#endif

uint8_t comms_transport_board_id_for_team(uint8_t team_id)
{
    return (uint8_t)(team_id / COMMS_TRANSPORT_TEAMS_PER_BOARD);
}

/*
 * Ceiling division, not floor: a board owning even one team (a
 * partial last group, e.g. NUM_TEAMS=7 with the default 5-per-board
 * -> boards {0..4} and {5,6}, a 2-team partial board) still counts as
 * a real board that needs a roster/handshake/relay slot. Floor
 * division would silently drop that partial board from every loop
 * that calls this, which is a worse failure mode (teams assigned to
 * a board nothing ever iterates to) than the function existing at
 * all. A function, not a header macro like COMMS_TRANSPORT_TEAMS_
 * PER_BOARD itself: NUM_TEAMS and COMMS_TRANSPORT_TEAMS_PER_BOARD are
 * only both in scope together here, in this .c file - NUM_TEAMS
 * comes from mission_state.h, which this file already includes for
 * other reasons, and COMMS_TRANSPORT_TEAMS_PER_BOARD is a #define
 * local to this file (see immediately above) - so this is where the
 * division actually happens once, rather than every caller needing
 * both headers in scope to compute it themselves and risking drift
 * if they use the wrong divisor.
 */
uint8_t comms_transport_board_count(void)
{
    return (uint8_t)((NUM_TEAMS + COMMS_TRANSPORT_TEAMS_PER_BOARD - 1u)
                      / COMMS_TRANSPORT_TEAMS_PER_BOARD);
}

int comms_transport_send(uint8_t sender_team_id, uint8_t receiver_team_id,
                          uint8_t frame_type, const uint8_t *payload,
                          uint8_t payload_len)
{
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint32_t wire_len;
    uint8_t receiver_board_id;

    /* sender_board_id is always THIS board - see lora_frame.h's own
       doc on lora_frame_pack(): a board only ever packs frames it is
       itself originating, whether the destination ends up being the
       same board (loopback, below) or a different one (SPI/radio
       hand-off, below). */
    wire_len = lora_frame_pack(frame_type, g_comms_transport_this_board_id,
                                payload, payload_len, wire_buf);

    if (wire_len == 0u)
    {
        /* payload_len exceeded LORA_FRAME_MAX_PAYLOAD - lora_frame_pack()
           already refused to write anything; nothing was sent, so no
           channel cost either. */
        return -1;
    }

    /* Unconditional: every send pays for channel occupancy, in-process
       or not. This has to happen before either branch below returns,
       so neither path can end up looking free. */
    comms_transport_advance_channel_busy(wire_len);

    receiver_board_id = comms_transport_board_id_for_team(receiver_team_id);

    if (receiver_board_id == g_comms_transport_this_board_id)
    {
        /* Same board: still round-trip through lora_frame_try_parse()
           on the bytes just packed, even though nothing touches a
           radio - keeps the framing code continuously exercised by
           every test run, not just the ones with real hardware
           attached (the same reasoning already applied to the
           existing in-process telemetry design). */
        uint8_t parsed_type;
        uint8_t parsed_sender_board_id;
        uint8_t parsed_payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t parsed_len;
        int parse_rc = lora_frame_try_parse(wire_buf, wire_len, &parsed_type,
                                             &parsed_sender_board_id,
                                             parsed_payload, &parsed_len);

        if (parse_rc != 1)
        {
            /* A frame this function just packed itself, with a
               correct checksum, failing to parse means
               lora_frame_pack()/lora_frame_try_parse() disagree with
               each other - a bug in one of them, not a real-world
               transport failure. Fail loudly rather than silently
               drop the message. */
            return -2;
        }

        /* parsed_sender_board_id is always g_comms_transport_this_board_id
           here (we packed it as such, just above) - this function's
           own sender_team_id parameter, not the board id, is what
           the inbox tracks per-message, so parsed_sender_board_id
           itself has no further use in this branch beyond having
           been required to authenticate/decrypt the loopback frame. */
        (void)parsed_sender_board_id;

        return comms_transport_inbox_push(sender_team_id, parsed_type,
                                           parsed_payload, parsed_len);
    }

    /* Different board: hand off to the (currently stubbed) SPI driver. */
    return lora_spi_transmit(wire_buf, wire_len);
}

int comms_transport_poll_received(uint8_t *out_sender_team_id,
                                   uint8_t *out_frame_type,
                                   uint8_t *out_payload,
                                   uint8_t *out_payload_len)
{
    CommsInboxEntry_t *slot;
    uint8_t i;

    if (g_inbox_count == 0u)
    {
        return 0;
    }

    slot = &g_inbox[g_inbox_tail];

    *out_sender_team_id = slot->sender_team_id;
    *out_frame_type = slot->frame_type;
    *out_payload_len = slot->payload_len;

    for (i = 0u; i < slot->payload_len; i++)
    {
        out_payload[i] = slot->payload[i];
    }

    g_inbox_tail = (g_inbox_tail + 1u) % COMMS_TRANSPORT_INBOX_CAPACITY;
    g_inbox_count--;

    return 1;
}

int comms_transport_deliver_received(uint8_t frame_type, const uint8_t *payload,
                                      uint8_t payload_len)
{
    /* See comms_transport.h's own doc on this function: the real
       physical-RX counterpart to comms_transport_send()'s same-board
       path above (that path: pack -> lora_frame_try_parse() -> push;
       this path: caller already parsed via lora_frame_try_parse()
       itself -> push here). COMMS_TRANSPORT_UNKNOWN_SENDER, not a
       real sender_team_id - see that macro's own doc for why. */
    return comms_transport_inbox_push(COMMS_TRANSPORT_UNKNOWN_SENDER, frame_type,
                                       payload, payload_len);
}

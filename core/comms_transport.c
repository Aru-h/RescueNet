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
/* How many on-air bytes "fit" in one simulated cost tick. A
   deliberately simple placeholder - proportional to message size, not
   calibrated against a real SX1262 airtime-vs-bytes table (that
   calibration is SPI/radio driver work, still pending bring-up).
   +1u below guarantees every send costs at least one tick even for a
   tiny payload, so the channel is never free regardless of size. */
#define COMMS_TRANSPORT_CHANNEL_BYTES_PER_TICK 8u
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

uint8_t comms_transport_board_id_for_team(uint8_t team_id)
{
    return (uint8_t)(team_id / 5u);
}

int comms_transport_send(uint8_t sender_team_id, uint8_t receiver_team_id,
                          uint8_t frame_type, const uint8_t *payload,
                          uint8_t payload_len)
{
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint32_t wire_len;
    uint8_t receiver_board_id;

    wire_len = lora_frame_pack(frame_type, payload, payload_len, wire_buf);

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
        uint8_t parsed_payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t parsed_len;
        int parse_rc = lora_frame_try_parse(wire_buf, wire_len, &parsed_type,
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

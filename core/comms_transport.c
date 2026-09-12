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
 * Real HAL-provided "is the radio still transmitting the last frame"
 * signal, backed by the SX1262's actual TX_DONE IRQ (see whichever
 * board's sx1262_hal.c is linked - both track a busy flag set in
 * their own lora_spi_transmit() and cleared once TX_DONE/Timeout is
 * observed). comms_transport_dispatch_pending() below is the only
 * caller; NOT the same thing as comms_transport_channel_busy_until_
 * tick() (a tick-based cost estimate - see comms_transport.h's
 * "Channel contention model" note on why the two must stay separate).
 * Weak default returns "never busy" so this file keeps building and
 * running standalone (laptop tests, either board before its own HAL
 * work lands) before a real HAL is linked in - same pattern as
 * lora_spi_transmit() immediately above.
 */
extern int lora_radio_tx_busy(void);

__attribute__((weak)) int lora_radio_tx_busy(void)
{
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

/* Concurrency invariant, CURRENT build only: g_inbox_head/g_inbox_tail/
 * g_inbox_count (and every CommsInboxEntry_t slot) are read and/or written
 * by this function, comms_transport_poll_received(), comms_transport_
 * peek_frame_type(), and comms_transport_deliver_received() (all below) -
 * and today that's safe ONLY because none of the four is ever called from
 * a real interrupt. Confirmed against both currently-implemented boards:
 * board/STM32N6570-DK/sx1262_hal.c and board/Raspberry_Pi_Pico/sx1262_hal.c
 * both poll the radio's DIO1 line from an ordinary task/superloop
 * (sx1262_poll_rx(), called out of main_board.c's single sim_task on the
 * DK) rather than servicing it from a real GPIO/EXTI interrupt, and
 * comms_transport_deliver_received()'s own comment below already flags
 * its real IRQ entry point as "still pending bring-up." None of this
 * state is `volatile`, and none of these four functions take a critical
 * section or disable interrupts, because nothing preempts them today.
 *
 * THIS STOPS BEING SAFE the moment DIO1 is rewired to a real interrupt
 * (EXTI on the DK, gpio_set_irq_enabled() on the Pico) instead of being
 * polled from sx1262_poll_rx()/the Pico's equivalent: comms_transport_
 * deliver_received() would then run in interrupt context while
 * comms_transport_poll_received()/peek_frame_type() keep running in task
 * context, and this non-atomic three-field read-modify-write sequence can
 * desynchronize under a mid-poll interrupt (lost/duplicated frames, or the
 * queue wedging at a wrong count). Add a critical section around the
 * read-modify-write in all four functions (or move to a lock-free SPSC
 * ring) BEFORE that rewiring happens - do not assume this file is already
 * safe for it just because it compiles clean for the board today. */
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
 * Tiered cross-board TX queues
 * ---------------------------------------------------------------
 * One fixed-capacity ring buffer per CommsPrecedence_t tier (see
 * comms_precedence.h). Holds UNPACKED entries - frame_type + raw
 * payload bytes, not the on-air packed/encrypted form - because
 * packing advances the per-board crypto TX counter exactly once per
 * successful call (lora_frame_pack()'s own doc), and a message
 * sitting queued behind higher-precedence traffic must not burn that
 * counter until it's actually about to go out. comms_transport_
 * dispatch_pending() is the only place that pops an entry and calls
 * lora_frame_pack() on it.
 *
 * Capacities are sized against real traffic, not arbitrary: PRIORITY
 * has to absorb cbba_broadcast_local_teams()'s per-round burst (up to
 * NUM_TEAMS-on-this-board x other-boards sends in one call - see
 * core/cbba.c), so it gets the largest allowance. FLASH/IMMEDIATE are
 * low-volume message types (fault escalation, override, handshake,
 * critical-incident discovery) - a handful of slots is generous
 * headroom, not a tight fit. ROUTINE (heartbeat/roster) arrives at a
 * much lower rate than CBBA bidding. Same tail-drop-when-full policy
 * the original single g_inbox already used, just applied per tier
 * now instead of to one shared buffer - see this project's design
 * notes on why strict-priority + tail-drop is the standard pairing
 * here, not something invented for this file.
 */

#ifndef COMMS_TRANSPORT_TX_QUEUE_CAPACITY_FLASH
#define COMMS_TRANSPORT_TX_QUEUE_CAPACITY_FLASH 4u
#endif
#ifndef COMMS_TRANSPORT_TX_QUEUE_CAPACITY_IMMEDIATE
#define COMMS_TRANSPORT_TX_QUEUE_CAPACITY_IMMEDIATE 4u
#endif
#ifndef COMMS_TRANSPORT_TX_QUEUE_CAPACITY_PRIORITY
#define COMMS_TRANSPORT_TX_QUEUE_CAPACITY_PRIORITY 12u
#endif
#ifndef COMMS_TRANSPORT_TX_QUEUE_CAPACITY_ROUTINE
#define COMMS_TRANSPORT_TX_QUEUE_CAPACITY_ROUTINE 6u
#endif

typedef struct
{
    uint8_t frame_type;
    uint8_t payload_len;
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
} CommsTxEntry_t;

typedef struct
{
    CommsTxEntry_t *slots;
    uint32_t capacity;
    uint32_t head;  /* next slot to write */
    uint32_t tail;  /* next slot to read */
    uint32_t count;
} CommsTxQueue_t;

static CommsTxEntry_t g_tx_flash_slots[COMMS_TRANSPORT_TX_QUEUE_CAPACITY_FLASH];
static CommsTxEntry_t g_tx_immediate_slots[COMMS_TRANSPORT_TX_QUEUE_CAPACITY_IMMEDIATE];
static CommsTxEntry_t g_tx_priority_slots[COMMS_TRANSPORT_TX_QUEUE_CAPACITY_PRIORITY];
static CommsTxEntry_t g_tx_routine_slots[COMMS_TRANSPORT_TX_QUEUE_CAPACITY_ROUTINE];

/* Indexed by CommsPrecedence_t - COMMS_PRECEDENCE_COUNT entries,
   ROUTINE..FLASH in that numeric order (see comms_precedence.h). */
static CommsTxQueue_t g_tx_queues[COMMS_PRECEDENCE_COUNT];

static void comms_transport_tx_queues_init(void)
{
    g_tx_queues[COMMS_PRECEDENCE_ROUTINE].slots    = g_tx_routine_slots;
    g_tx_queues[COMMS_PRECEDENCE_ROUTINE].capacity  = COMMS_TRANSPORT_TX_QUEUE_CAPACITY_ROUTINE;
    g_tx_queues[COMMS_PRECEDENCE_PRIORITY].slots    = g_tx_priority_slots;
    g_tx_queues[COMMS_PRECEDENCE_PRIORITY].capacity = COMMS_TRANSPORT_TX_QUEUE_CAPACITY_PRIORITY;
    g_tx_queues[COMMS_PRECEDENCE_IMMEDIATE].slots   = g_tx_immediate_slots;
    g_tx_queues[COMMS_PRECEDENCE_IMMEDIATE].capacity = COMMS_TRANSPORT_TX_QUEUE_CAPACITY_IMMEDIATE;
    g_tx_queues[COMMS_PRECEDENCE_FLASH].slots       = g_tx_flash_slots;
    g_tx_queues[COMMS_PRECEDENCE_FLASH].capacity    = COMMS_TRANSPORT_TX_QUEUE_CAPACITY_FLASH;

    for (uint32_t p = 0u; p < (uint32_t)COMMS_PRECEDENCE_COUNT; p++)
    {
        g_tx_queues[p].head = 0u;
        g_tx_queues[p].tail = 0u;
        g_tx_queues[p].count = 0u;
    }
}

static int comms_transport_tx_enqueue(CommsPrecedence_t precedence,
                                       uint8_t frame_type,
                                       const uint8_t *payload,
                                       uint8_t payload_len)
{
    CommsTxQueue_t *q = &g_tx_queues[(uint32_t)precedence];
    CommsTxEntry_t *slot;
    uint8_t i;

    if (q->count == q->capacity)
    {
        return -1; /* tail-drop: that tier's queue is full */
    }

    slot = &q->slots[q->head];
    slot->frame_type = frame_type;
    slot->payload_len = payload_len;

    for (i = 0u; i < payload_len; i++)
    {
        slot->payload[i] = payload[i];
    }

    q->head = (q->head + 1u) % q->capacity;
    q->count++;

    return 0;
}

/* Returns 1 and fills *out_entry if the highest-precedence non-empty
   queue had something to pop, 0 if every queue is empty. Strict
   priority: always checks FLASH first, then IMMEDIATE, PRIORITY,
   ROUTINE - see comms_transport_dispatch_pending()'s own doc
   (comms_transport.h) for why this is non-preemptive head-of-line
   scheduling, not round-robin. */
static int comms_transport_tx_dequeue_highest(CommsTxEntry_t *out_entry)
{
    int p;

    for (p = (int)COMMS_PRECEDENCE_FLASH; p >= (int)COMMS_PRECEDENCE_ROUTINE; p--)
    {
        CommsTxQueue_t *q = &g_tx_queues[(uint32_t)p];

        if (q->count > 0u)
        {
            *out_entry = q->slots[q->tail];
            q->tail = (q->tail + 1u) % q->capacity;
            q->count--;
            return 1;
        }
    }

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

    comms_transport_tx_queues_init();

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
                          uint8_t frame_type, CommsPrecedence_t precedence,
                          const uint8_t *payload, uint8_t payload_len)
{
    uint8_t receiver_board_id;

    if (payload_len > LORA_FRAME_MAX_PAYLOAD)
    {
        /* Same ceiling lora_frame_pack() itself enforces - checked
           explicitly here now, since the cross-board path below no
           longer calls lora_frame_pack() (and therefore doesn't get
           this check for free) until dispatch time. */
        return -1;
    }

    receiver_board_id = comms_transport_board_id_for_team(receiver_team_id);

    if (receiver_board_id == g_comms_transport_this_board_id)
    {
        /* Same board: unchanged from before this file grew tiered TX
           queues - pack and push immediately. No real channel
           contention on a pure in-process loopback, so nothing to
           gain by queueing it; precedence is intentionally unused on
           this path. */
        uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
        uint32_t wire_len;
        uint8_t parsed_type;
        uint8_t parsed_sender_board_id;
        uint8_t parsed_payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t parsed_len;
        int parse_rc;

        (void)precedence;

        /* sender_board_id is always THIS board - see lora_frame.h's
           own doc on lora_frame_pack(): a board only ever packs
           frames it is itself originating. */
        wire_len = lora_frame_pack(frame_type, g_comms_transport_this_board_id,
                                    payload, payload_len, wire_buf);

        if (wire_len == 0u)
        {
            return -1;
        }

        /* Every send pays for channel occupancy, in-process or not -
           charged immediately here since this path has no queue to
           defer it through (contrast the cross-board path below,
           where this now happens at actual dispatch time instead). */
        comms_transport_advance_channel_busy(wire_len);

        /* Still round-trip through lora_frame_try_parse() on the
           bytes just packed, even though nothing touches a radio -
           keeps the framing code continuously exercised by every test
           run, not just the ones with real hardware attached. */
        parse_rc = lora_frame_try_parse(wire_buf, wire_len, &parsed_type,
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

        (void)parsed_sender_board_id;

        return comms_transport_inbox_push(sender_team_id, parsed_type,
                                           parsed_payload, parsed_len);
    }

    /* Different board: don't touch the radio here. Enqueue for
       comms_transport_dispatch_pending() to pack and actually
       transmit once the radio is free and this is the highest-
       precedence thing waiting - see this function's own doc
       (comms_transport.h) for why packing (and the crypto counter
       advance it does) is deliberately deferred to that point instead
       of happening here. sender_team_id has no wire representation
       (see this header's own "Open question" note) and isn't needed
       to pack or dispatch a cross-board frame, so it's intentionally
       not part of the queued entry. */
    return comms_transport_tx_enqueue(precedence, frame_type, payload, payload_len);
}

void comms_transport_dispatch_pending(void)
{
    CommsTxEntry_t entry;
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint32_t wire_len;

    if (lora_radio_tx_busy())
    {
        /* Still transmitting whatever was dispatched last time -
           nothing to do this tick. Re-checked from FLASH downward
           again the next time this returns not-busy, so a higher-
           precedence arrival always gets first look the moment the
           radio actually frees up. */
        return;
    }

    if (!comms_transport_tx_dequeue_highest(&entry))
    {
        return; /* every tier empty - nothing waiting to send */
    }

    wire_len = lora_frame_pack(entry.frame_type, g_comms_transport_this_board_id,
                                entry.payload, entry.payload_len, wire_buf);

    if (wire_len == 0u)
    {
        /* Shouldn't happen - comms_transport_send() already rejected
           payload_len > LORA_FRAME_MAX_PAYLOAD before this entry was
           ever enqueued - but if it somehow does, the entry is
           already popped; drop it rather than leaving the queue
           machinery in a half-dequeued state. No NACK path at this
           layer (see this function's own doc), same policy as every
           other drop case here. */
        return;
    }

    /* Charged at actual dispatch time, not enqueue time, so this
       stays real in-dispatch-order channel occupancy even when a
       higher-precedence message jumps ahead of something enqueued
       earlier - see comms_transport.h's "Channel contention model"
       note. */
    comms_transport_advance_channel_busy(wire_len);

    /* Return value intentionally ignored - see this function's own
       doc (comms_transport.h) on why a failed hand-off here is
       dropped, not retried. */
    (void)lora_spi_transmit(wire_buf, wire_len);
}

/* Shares g_inbox_head/tail/count with comms_transport_inbox_push() - see
 * that function's concurrency-invariant comment above. Not yet safe to
 * call from a real ISR context caller pushing concurrently. */
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

/* Shares g_inbox_head/tail/count with comms_transport_inbox_push() - see
 * that function's concurrency-invariant comment above. Not yet safe to
 * call from a real ISR context caller pushing concurrently. */
int comms_transport_peek_frame_type(uint8_t *out_frame_type)
{
    if(g_inbox_count == 0u)
        return 0;
    *out_frame_type = g_inbox[g_inbox_tail].frame_type;
    return 1;
}

/* Calls comms_transport_inbox_push() - see that function's concurrency-
 * invariant comment above. This is the function core/comms_transport.h's
 * own doc comment on this prototype calls "a real SPI/IRQ RX handler
 * (still pending bring-up)": today it is only ever invoked from polled
 * task context (sx1262_poll_rx() on the DK, the Pico's equivalent), never
 * from a real interrupt - do not add a real ISR caller here without first
 * adding the synchronization core/comms_transport_inbox_push()'s comment
 * above describes. */
int comms_transport_deliver_received(uint8_t frame_type, const uint8_t *payload,
                                      uint8_t payload_len)
{
    if(payload_len > LORA_FRAME_MAX_PAYLOAD)
    {
        return -1;
    }
    return comms_transport_inbox_push(COMMS_TRANSPORT_UNKNOWN_SENDER, frame_type,
                                       payload, payload_len);
}

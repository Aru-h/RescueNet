/*
 * Self-test for core/comms_transport.c's TX queue rules - desktop only,
 * same spirit as lora_frame_selftest.c. Every dispatched frame is
 * captured by a strong lora_spi_transmit() and parsed back with the real
 * lora_frame_try_parse(), so the checks are on what actually went out.
 *
 * Covers:
 *   1. CBBA frames coalesce per sending team: a newer frame replaces the
 *      queued one in place, so a burst leaves room for other PRIORITY
 *      traffic and only each team's latest belief is sent.
 *   2. Other frame types never coalesce.
 *   3. Tail-drop is unchanged for non-CBBA traffic when a tier is full.
 *   4. ROUTINE gets a dispatch after COMMS_TRANSPORT_ROUTINE_STARVATION_
 *      LIMIT (4) PRIORITY dispatches in a row while it waits, even with
 *      PRIORITY refilled before every dispatch.
 *   5. FLASH still goes first even when ROUTINE is due.
 *
 * Build/run: make check-comms-transport-selftest
 */

#include <stdio.h>
#include <string.h>

#include "comms_transport.h"
#include "lora_frame.h"

static int g_failures;

static void check(int cond, const char *what)
{
    if (!cond)
    {
        fprintf(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

#define MAX_CAPTURED 64u

typedef struct
{
    uint8_t frame_type;
    uint8_t tag; /* payload[0] */
} Sent_t;

static Sent_t g_sent[MAX_CAPTURED];
static uint32_t g_sent_count;

int lora_spi_transmit(const uint8_t *frame_bytes, uint32_t frame_len)
{
    uint8_t frame_type;
    uint8_t sender;
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t payload_len;

    if (lora_frame_try_parse(frame_bytes, frame_len, &frame_type, &sender,
                             payload, &payload_len) == 1 &&
        g_sent_count < MAX_CAPTURED)
    {
        g_sent[g_sent_count].frame_type = frame_type;
        g_sent[g_sent_count].tag = payload_len > 0u ? payload[0] : 0u;
        g_sent_count++;
    }
    return 0;
}

/* Team 5 lives on board 1, so every send below takes the queued path. */
#define REMOTE_TEAM 5u

static int send(uint8_t sender_team, uint8_t frame_type, CommsPrecedence_t precedence,
                uint8_t tag)
{
    uint8_t payload[2] = {tag, 0u};
    return comms_transport_send(sender_team, REMOTE_TEAM, frame_type, precedence,
                                payload, (uint8_t)sizeof(payload));
}

static void reset(void)
{
    g_comms_transport_this_board_id = 0u;
    comms_transport_init();
    g_sent_count = 0u;
}

static void drain(void)
{
    uint32_t i;
    for (i = 0u; i < 2u * MAX_CAPTURED; i++)
    {
        comms_transport_dispatch_pending();
    }
}

static void test_cbba_coalesces_per_team(void)
{
    uint8_t round;
    uint8_t team;

    reset();
    /* Three rounds of 5 teams = 15 frames into a 12-slot tier. */
    for (round = 0u; round < 3u; round++)
    {
        for (team = 0u; team < 5u; team++)
        {
            check(send(team, LORA_FRAME_TYPE_CBBA, COMMS_PRECEDENCE_PRIORITY,
                       (uint8_t)(round * 10u + team)) == 0,
                  "1: CBBA send accepted while coalescing");
        }
    }
    check(send(0u, LORA_FRAME_TYPE_MISSION_LINK, COMMS_PRECEDENCE_PRIORITY, 0xAAu) == 0,
          "1: incident/fault relay still fits behind a CBBA burst");

    drain();
    check(g_sent_count == 6u, "1: exactly one CBBA frame per team plus the relay went out");
    for (team = 0u; team < 5u && team < g_sent_count; team++)
    {
        check(g_sent[team].frame_type == LORA_FRAME_TYPE_CBBA &&
                  g_sent[team].tag == (uint8_t)(20u + team),
              "1: each team's latest belief, in first-queued order");
    }
    check(g_sent_count == 6u && g_sent[5].frame_type == LORA_FRAME_TYPE_MISSION_LINK &&
              g_sent[5].tag == 0xAAu,
          "1: relay sent after the coalesced CBBA frames");
}

static void test_other_types_do_not_coalesce_and_tail_drop(void)
{
    uint32_t i;
    uint32_t accepted = 0u;

    reset();
    for (i = 0u; i < 13u; i++)
    {
        if (send(0u, LORA_FRAME_TYPE_MISSION_LINK, COMMS_PRECEDENCE_PRIORITY, (uint8_t)i) == 0)
        {
            accepted++;
        }
    }
    check(accepted == 12u, "2/3: 12 same-sender relays queue separately, the 13th is tail-dropped");
    drain();
    check(g_sent_count == 12u, "2/3: all 12 queued relays went out");
}

static void test_routine_not_starved(void)
{
    uint32_t dispatch;
    int routine_at = -1;

    reset();
    check(send(0u, LORA_FRAME_TYPE_MISSION_LINK, COMMS_PRECEDENCE_ROUTINE, 0x77u) == 0,
          "4: roster relay queued");

    for (dispatch = 0u; dispatch < 10u; dispatch++)
    {
        /* PRIORITY never empties, as under continuous CBBA bidding. */
        (void)send((uint8_t)(dispatch % 5u), LORA_FRAME_TYPE_CBBA, COMMS_PRECEDENCE_PRIORITY,
                   (uint8_t)dispatch);
        comms_transport_dispatch_pending();
        if (routine_at < 0 && g_sent_count > 0u &&
            g_sent[g_sent_count - 1u].frame_type == LORA_FRAME_TYPE_MISSION_LINK)
        {
            routine_at = (int)dispatch;
        }
    }
    check(routine_at == 4, "4: ROUTINE sent on the 5th dispatch, after 4 PRIORITY ones");
}

static void test_flash_still_first(void)
{
    uint32_t i;

    reset();
    check(send(0u, LORA_FRAME_TYPE_MISSION_LINK, COMMS_PRECEDENCE_ROUTINE, 0x77u) == 0,
          "5: roster relay queued");
    for (i = 0u; i < 4u; i++)
    {
        (void)send((uint8_t)i, LORA_FRAME_TYPE_CBBA, COMMS_PRECEDENCE_PRIORITY, (uint8_t)i);
        (void)send((uint8_t)i, LORA_FRAME_TYPE_CBBA, COMMS_PRECEDENCE_PRIORITY, (uint8_t)i);
        comms_transport_dispatch_pending();
    }
    /* ROUTINE is now due - but a FLASH frame must still win. */
    check(send(0u, LORA_FRAME_TYPE_FAULT_VOTE, COMMS_PRECEDENCE_FLASH, 0xF1u) == 0,
          "5: FLASH queued");
    g_sent_count = 0u;
    comms_transport_dispatch_pending();
    check(g_sent_count == 1u && g_sent[0].frame_type == LORA_FRAME_TYPE_FAULT_VOTE,
          "5: FLASH dispatched ahead of a due ROUTINE entry");
    comms_transport_dispatch_pending();
    check(g_sent_count == 2u && g_sent[1].frame_type == LORA_FRAME_TYPE_MISSION_LINK,
          "5: the due ROUTINE entry follows immediately");
}

int main(void)
{
    test_cbba_coalesces_per_team();
    test_other_types_do_not_coalesce_and_tail_drop();
    test_routine_not_starved();
    test_flash_still_first();

    if (g_failures != 0)
    {
        fprintf(stderr, "comms_transport_selftest: %d check(s) FAILED\n", g_failures);
        return 1;
    }
    printf("comms_transport_selftest: all checks passed\n");
    return 0;
}

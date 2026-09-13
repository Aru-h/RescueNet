/*
 * Confirms core/fault_comms.c's fault_comms_broadcast_local_teams()
 * pacing gate (FAULT_COMMS_BROADCAST_INTERVAL_MS=120000) actually
 * throttles real send cadence to ~120s, not the invalidated
 * TICK_TIME_SEC-based assumption - see fault_predictor_architecture_
 * v4.md Section 4's "Concern 2, partial answer" for the derivation
 * this fix implements.
 *
 * Links the real core/ directory (same whole-core pattern as laptop/
 * tools/fault_comms_receive_selftest.c and evidence_vector_parity_
 * selftest.c - fault_comms.c pulls in enough of the rest of the
 * simulator that a hand-picked subset isn't practical), but overrides
 * core/cbba.c's weak-default cbba_now_ms() with a STRONG definition
 * here that returns a fully test-controlled clock (not auto-
 * incrementing), so this test can set exact simulated wall-clock times
 * per call instead of stepping by a fixed amount.
 *
 * Counts actual broadcasts by watching fault_buffer_get_local_sample()'s
 * tick field change - fault_buffer_record_local_evidence() (which sets
 * it) only runs on ticks that clear the pacing gate, so a change is
 * direct, unambiguous proof a real broadcast pass happened this call.
 *
 * Build: see Makefile's check-fault-comms-broadcast-pacing target.
 */
#include "fault_comms.h"
#include "fault_buffer.h"
#include "mission_state.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

/* Strong override of core/cbba.c's __attribute__((weak)) cbba_now_ms() -
   test-controlled, not auto-incrementing, so cadence can be checked at
   exact simulated times. */
static uint32_t g_test_now_ms = 0u;
uint32_t cbba_now_ms(void) { return g_test_now_ms; }

int main(void)
{
    uint32_t last_seen_tick;
    int broadcasts_seen = 0;
    uint32_t broadcast_times_ms[64];

    memset(g_teams, 0, sizeof(g_teams));
    memset(g_incidents, 0, sizeof(g_incidents));
    memset(g_system_tasks, 0, sizeof(g_system_tasks));
    for (uint8_t t = 0; t < NUM_TEAMS; t++)
        g_teams[t].team_id = t;

    fault_comms_init();
    g_current_tick = 0u;
    g_test_now_ms = 0u;

    last_seen_tick = fault_buffer_get_local_sample(0).tick;
    CHECK(fault_buffer_get_local_sample(0).known == 0u); /* nothing recorded yet */

    /* Simulate a flat-out main loop calling this every tick
       (BOARD_TICK_DELAY_MS=0's actual "runs flat out" behavior, per
       each board's own main_board.c) for a real-time span of 10
       minutes, at a real 100ms-per-tick rate (arbitrary but plausible
       flat-out cadence) - should see broadcasts roughly every 120s,
       not every call. */
    for (uint32_t call = 0; call < 6000; call++) /* 6000 * 100ms = 600s = 10 min */
    {
        g_test_now_ms = call * 100u;
        g_current_tick = call;

        fault_comms_broadcast_local_teams();

        FaultBufferLocalSample_t sample = fault_buffer_get_local_sample(0);
        if (sample.known && sample.tick != last_seen_tick)
        {
            last_seen_tick = sample.tick;
            if (broadcasts_seen < 64)
                broadcast_times_ms[broadcasts_seen] = g_test_now_ms;
            broadcasts_seen++;
        }
    }

    printf("Simulated 600s of flat-out per-tick calls (100ms/tick). Broadcasts observed: %d\n",
        broadcasts_seen);
    for (int i = 0; i < broadcasts_seen && i < 64; i++)
        printf("  broadcast #%d at t=%ums\n", i, broadcast_times_ms[i]);

    /* Expect roughly 600000ms / 120000ms = 5 broadcasts (first one right
       around t=0, since s_last_broadcast_ms starts at 0 and the first
       call's now_ms - 0 = 0 < 120000 is false only once now_ms itself
       reaches 120000 - so the very first tick, t=0, is BELOW the
       interval and does NOT fire; the first real broadcast should land
       at or just after t=120000ms). Not every one of the 6000 calls. */
    CHECK(broadcasts_seen >= 4 && broadcasts_seen <= 6);
    CHECK(broadcasts_seen < 6000); /* the actual point of this test */

    for (int i = 1; i < broadcasts_seen; i++)
    {
        uint32_t gap = broadcast_times_ms[i] - broadcast_times_ms[i - 1];
        printf("  gap #%d: %ums\n", i, gap);
        /* Allow +/- one 100ms tick of quantization slop around the
           exact 120000ms interval. */
        CHECK(gap >= 119900u && gap <= 120000u);
    }

    /* Confirm it is NOT the old, invalidated TICK_TIME_SEC-based
       cadence: TICK_TIME_SEC=36 (mission_state.h) would, at 100ms/call,
       have no relationship to a 120000ms real-time gap at all - this
       is really just re-confirming the gaps above are wall-clock-ms
       distances (which they are, by construction, since g_test_now_ms
       is the only thing the gate reads), not a tick-count distance. */
    CHECK(broadcasts_seen > 0);

    printf("\n%s: %d check(s) failed\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}

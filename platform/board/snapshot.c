/*
 * Board implementation of the snapshot.h interface (STM32N6570-DK).
 *
 * The laptop backend (platform/laptop/snapshot.c) writes a 48-column
 * CSV row per snapshot_record() call straight to a file. There is no
 * filesystem assumption we want to make on-device yet, so this backend
 * buffers a compact per-event record in a fixed-size static ring and
 * leaves the actual USB drain to the board's telemetry task
 * (TODO(usb)).
 *
 * Deliberately NOT full column parity with the laptop CSV schema:
 * capturing all 48 fields per event would mean walking TeamState_t /
 * MissionExecutionTwin_t / WatchdogStatus_t / RecoveryStatus_t here,
 * duplicating logic that snapshot.c on the laptop side already does.
 * Once the USB protocol exists, the better fix is a shared row-encode
 * helper in core/ that both backends call, so the schema only lives in
 * one place. Tracked as a follow-up, not done here to avoid changing
 * core/ behavior as part of this first platform-split pass.
 */

#include <stdint.h>
#include <stddef.h>

#include "snapshot.h"

#ifndef BOARD_SNAPSHOT_RING_CAPACITY
#define BOARD_SNAPSHOT_RING_CAPACITY 256u
#endif

typedef struct
{
    uint32_t run_id;
    uint32_t cycle;
    uint8_t  team_id;
    uint8_t  event_type;
} BoardSnapshotEntry_t;

static BoardSnapshotEntry_t g_snapshot_ring[BOARD_SNAPSHOT_RING_CAPACITY];
static uint32_t g_snapshot_ring_head = 0;
static uint32_t g_snapshot_ring_count = 0;
static uint32_t g_run_id = 0;

void snapshot_begin_run(uint32_t seed)
{
    /* TODO(bsp): no time.h wall-clock source assumed on-device yet.
     * Laptop side seeds run_id from process epoch + seed for
     * cross-run uniqueness; on-device, derive run_id from whatever
     * monotonic/RTC source the BSP exposes, or just from seed alone
     * if run uniqueness isn't needed for the board's use case. */
    g_run_id = seed;
    g_snapshot_ring_head = 0;
    g_snapshot_ring_count = 0;
}

void snapshot_record(uint32_t cycle, EventType_t event_type, uint8_t team_id)
{
    BoardSnapshotEntry_t *slot = &g_snapshot_ring[g_snapshot_ring_head];

    slot->run_id = g_run_id;
    slot->cycle = cycle;
    slot->team_id = team_id;
    slot->event_type = (uint8_t)event_type;

    g_snapshot_ring_head = (g_snapshot_ring_head + 1u) % BOARD_SNAPSHOT_RING_CAPACITY;
    if(g_snapshot_ring_count < BOARD_SNAPSHOT_RING_CAPACITY)
        g_snapshot_ring_count++;
    /* TODO(usb): oldest-entry overwrite on overflow is fine as a
     * stub default, but once the telemetry task exists, decide
     * deliberately whether overrun should overwrite (drop oldest,
     * current behavior) or block/backpressure the sim tick. Silent
     * data loss on overflow is acceptable for bring-up, not for the
     * shipped firmware. */
}

void snapshot_end_run(void)
{
    /* TODO(usb): flush/flag end-of-run boundary to the telemetry
     * consumer so it can segment the stream the way the laptop CSV's
     * run_id column lets the dashboard segment history. */
}

void snapshot_close_dataset(void)
{
    /* No dataset file on-device; nothing to close. Present for
     * interface parity with snapshot.h. */
}

/*
 * TODO(usb): telemetry task hook. Not part of snapshot.h's public
 * interface (core/ never calls this) — this is what the board's USB
 * task should call to drain the ring once it exists. Exposed now so
 * the shape of that integration point is visible in this stub.
 */
uint32_t board_snapshot_ring_pending(void)
{
    return g_snapshot_ring_count;
}

void snapshot_record_resolved(uint32_t cycle, TaskPool_t pool, const IncidentState_t *task) {
    (void)pool; // Suppress unused parameter warning
    if (task == NULL)
    {
        return;
    }

    /*
     * Per the board implementation's design, we intentionally drop the wide 
     * 48-column fields (incident_id, priority, capability, victims, etc.) 
     * to save memory, and just log the core event to the ring buffer.
     *
     * Note: EVENT_INCIDENT_RESOLVED is assumed to be part of EventType_t 
     * in snapshot.h based on the laptop implementation.
     */
    snapshot_record(cycle, EVENT_INCIDENT_RESOLVED, task->assigned_team);
}

/*
 * Board implementation of the snapshot.h interface (STM32N6570-DK,
 * µT-Kernel 3.0 BSP2).
 *
 * The laptop backend (laptop/snapshot.c) writes a 48-column
 * CSV row per snapshot_record() call straight to a file. There is no
 * filesystem on-device, so this backend instead buffers a compact
 * per-event record (run_id, cycle, team_id, event_type - the 4 fields
 * telemetry_frame.h's TelemetryFrame_t mirrors) in a fixed-size static
 * ring, and drains that ring out over USART1 as binary, magic-byte-
 * framed wire frames per core/telemetry_frame.h.
 *
 * Deliberately NOT full column parity with the laptop CSV schema:
 * capturing all 48 fields per event would mean walking TeamState_t /
 * MissionExecutionTwin_t / WatchdogStatus_t / RecoveryStatus_t here,
 * duplicating logic the laptop's snapshot.c already does. See
 * telemetry_frame.h section 1 for why this struct doesn't grow past
 * these 4 fields without a schema change there first.
 *
 * USART1 also carries free-running tm_printf debug text, so the
 * drain below writes raw framed bytes via tm_putchar (not tm_printf)
 * - see telemetry_frame.h section 2 for the magic-byte/checksum
 * framing that lets a listener tell binary frames apart from debug
 * text and resync after a glitch.
 */

#include <stdint.h>
#include <stddef.h>

#include <tm/tmonitor.h>

#include "snapshot.h"
#include "telemetry_frame.h"

/* Ring buffer capacity for pending snapshot entries. */
#ifndef BOARD_SNAPSHOT_RING_CAPACITY
#define BOARD_SNAPSHOT_RING_CAPACITY 256u
#endif

/* Maximum number of frames to drain per call, to bound execution time
 * inside any single tick maintenance interval (see the cadence
 * comment on BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL in main_board.c). */
#ifndef BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL
#define BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL 32u
#endif

/*
 * Mirrors TelemetryFrame_t field for field (see telemetry_frame.h
 * section 1). Kept as a distinct, non-packed type for the ring's
 * internal storage - the packed layout only matters once a frame is
 * being written out to the wire, which is where the drain builds a
 * TelemetryFrame_t from one of these before framing it.
 */
typedef struct
{
    uint32_t run_id;
    uint32_t cycle;
    uint8_t  team_id;
    uint8_t  event_type;
} BoardSnapshotEntry_t;

static BoardSnapshotEntry_t g_snapshot_ring[BOARD_SNAPSHOT_RING_CAPACITY];
static volatile uint32_t g_ring_head = 0u;   /* next write slot */
static volatile uint32_t g_ring_tail = 0u;   /* oldest undrained slot */
static volatile uint32_t g_ring_count = 0u;
static uint32_t g_run_id = 0u;

/*
 * Not part of snapshot.h's public interface (core/ never calls this)
 * - forward-declared at its one call site in main_board.c, same as
 * before.
 */
uint32_t board_snapshot_ring_pending(void)
{
    return g_ring_count;
}

void snapshot_begin_run(uint32_t seed)
{
    /* No wall-clock/RTC source assumed on-device: the laptop side
     * seeds run_id from process epoch + seed for cross-run
     * uniqueness, but there's no equivalent monotonic source wired
     * up here yet, so run_id is just the seed. Revisit if the BSP
     * exposes an RTC and cross-run uniqueness turns out to matter. */
    g_run_id = seed;

    /* Reset ring state for the new run. Safe: main_board.c always
     * does a final board_snapshot_ring_drain() at the end of a
     * seed's ticks (see its sim_task loop) before the next
     * snapshot_begin_run() call, so nothing legitimate is pending at
     * this point in normal operation. */
    g_ring_head = 0u;
    g_ring_tail = 0u;
    g_ring_count = 0u;
}

/*
 * Push one entry into the ring. Ring overflow policy per
 * telemetry_frame.h section 3: overwrite the oldest unsent entry,
 * no backpressure on the sim tick. This must never block.
 */
static void board_snapshot_ring_push(uint32_t cycle, uint8_t event_type, uint8_t team_id)
{
    BoardSnapshotEntry_t *slot = &g_snapshot_ring[g_ring_head];

    slot->run_id = g_run_id;
    slot->cycle = cycle;
    slot->team_id = team_id;
    slot->event_type = event_type;

    if(g_ring_count == BOARD_SNAPSHOT_RING_CAPACITY)
    {
        /* Ring is full: the slot we just wrote into (g_ring_head) is
         * the same slot the oldest undrained entry occupied, so it
         * has already been overwritten above. Advance the tail past
         * it so the drain side stops treating it as the oldest
         * pending entry - count stays at capacity, nothing blocks. */
        g_ring_tail = (g_ring_tail + 1u) % BOARD_SNAPSHOT_RING_CAPACITY;
    }
    else
    {
        g_ring_count++;
    }

    g_ring_head = (g_ring_head + 1u) % BOARD_SNAPSHOT_RING_CAPACITY;
}

void snapshot_record(uint32_t cycle, EventType_t event_type, uint8_t team_id)
{
    board_snapshot_ring_push(cycle, (uint8_t)event_type, team_id);
}

/*
 * Same ring entry shape as snapshot_record(), but - matching the
 * laptop implementation's split - team_id comes from task-
 * >assigned_team rather than a live team lookup, since by the time a
 * task's completion is detected the assigned team may already have
 * moved on to a new assignment earlier in the same tick. The wide
 * incident-detail fields (priority, location, victim count, ...)
 * that the laptop CSV also captures for this event are among the 44
 * columns this board backend intentionally drops - see the top-of-
 * file comment.
 */
void snapshot_record_resolved(uint32_t cycle, TaskPool_t pool, const IncidentState_t *task)
{
    (void)pool;

    if(task == NULL)
        return;

    board_snapshot_ring_push(cycle, (uint8_t)EVENT_INCIDENT_RESOLVED, task->assigned_team);
}

void snapshot_end_run(void)
{
    /* No end-of-run boundary marker on the wire today - a laptop-
     * side reader can already segment by run_id per-frame (see
     * telemetry_frame.h section 1), so there's nothing additional to
     * flush or flag here. Present for interface parity with
     * snapshot.h. */
}

void snapshot_close_dataset(void)
{
    /* No dataset file on-device; nothing to close. Present for
     * interface parity with snapshot.h. */
}

/*
 * Drain pending ring entries out over USART1 as binary, magic-byte-
 * framed wire frames (core/telemetry_frame.h): 2 magic bytes + the
 * packed TelemetryFrame_t payload + a 1-byte additive checksum over
 * the payload. Written with tm_putchar rather than tm_printf since
 * this is raw binary, not text - tm_printf's free-running debug
 * lines share the same wire, and the framing is exactly what lets a
 * listener tell the two apart and resync after a glitch (see
 * telemetry_frame.h section 2).
 *
 * Capped by BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL to bound
 * execution time inside any single tick maintenance cycle. Called
 * periodically by sim_task in main_board.c.
 */
uint32_t board_snapshot_ring_drain(void)
{
    uint32_t drained_count = 0u;

    while((g_ring_count > 0u) && (drained_count < BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL))
    {
        const BoardSnapshotEntry_t *entry = &g_snapshot_ring[g_ring_tail];
        TelemetryFrame_t frame;
        const uint8_t *payload_bytes = (const uint8_t *)&frame;
        uint8_t checksum;
        uint32_t i;

        frame.run_id = entry->run_id;
        frame.cycle = entry->cycle;
        frame.team_id = entry->team_id;
        frame.event_type = entry->event_type;

        checksum = TELEMETRY_FRAME_CHECKSUM(payload_bytes);

        (void)tm_putchar((INT)TELEMETRY_FRAME_MAGIC0);
        (void)tm_putchar((INT)TELEMETRY_FRAME_MAGIC1);
        for(i = 0u; i < TELEMETRY_FRAME_PAYLOAD_SIZE; i++)
        {
            (void)tm_putchar((INT)payload_bytes[i]);
        }
        (void)tm_putchar((INT)checksum);

        g_ring_tail = (g_ring_tail + 1u) % BOARD_SNAPSHOT_RING_CAPACITY;
        g_ring_count--;
        drained_count++;
    }

    return drained_count;
}

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

#include <tm/tmonitor.h>

#include "snapshot.h"
#include "telemetry_frame.h"

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

/*
 * Drain bookkeeping, separate from g_snapshot_ring_head/_count above
 * (those only track ring fill level for board_snapshot_ring_pending()
 * and are left untouched by this drain path). These are monotonic
 * "how many entries total have ever been written/sent" counters
 * rather than a second head/tail pair: that lets board_snapshot_ring_
 * drain() below detect "the entry I was about to send has already
 * been overwritten by a newer snapshot_record() call" with simple
 * subtraction, instead of needing snapshot_record() to maintain a
 * separate not-yet-drained flag per slot on its hot path.
 */
static uint32_t g_snapshot_seq_written = 0;
static uint32_t g_snapshot_seq_drained = 0;

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
    g_snapshot_seq_written = 0;
    g_snapshot_seq_drained = 0;
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
    g_snapshot_seq_written++;
    /* Overwrite-oldest-on-overflow is the permanent, documented
     * policy now (see "Ring overflow policy" in
     * core/telemetry_frame.h), not just a stub default -- this TODO
     * is resolved. board_snapshot_ring_drain() below detects and
     * accounts for entries lost this way rather than assuming
     * everything it ever saw written is still present. */
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

#ifndef BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL
/*
 * Upper bound on how many frames a single board_snapshot_ring_drain()
 * call will emit. tm_putchar() writes synchronously to USART1 (the
 * same physical UART tm_printf debug text shares), so each byte sent
 * here is time this call is not returning control to sim_task. With
 * this cap, one drain call is at most 64 * TELEMETRY_FRAME_WIRE_SIZE
 * (832) bytes -- well under 100ms even at the BSP's slowest supported
 * baud rate -- instead of a worst case of the full 256-entry ring
 * (~3.3KB) if a long-idle drain interval let the ring fill
 * completely. Any pending entries beyond the cap are simply left for
 * the next drain call; per the overwrite-oldest policy in
 * core/telemetry_frame.h this is a latency/staleness trade-off, not
 * a correctness one -- snapshot_record() never blocks on drain
 * progress either way.
 */
#define BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL 64u
#endif

/*
 * Encodes one ring entry as a TelemetryFrame_t and writes it to
 * USART1 as sync-prefixed wire bytes (see core/telemetry_frame.h),
 * using tm_putchar() -- the same underlying T-Monitor call
 * tm_printf()/tm_putstring() use for debug text -- so this rides the
 * one physical UART both channels share. Field-by-field assignment
 * into TelemetryFrame_t rather than a raw memcpy of
 * BoardSnapshotEntry_t: the two structs are defined independently
 * (one here, one in core/telemetry_frame.h) and nothing guarantees
 * identical padding/layout between them even though both are
 * currently {uint32, uint32, uint8, uint8} in the same order.
 */
static void board_snapshot_send_frame(const BoardSnapshotEntry_t *entry)
{
    TelemetryFrame_t frame;
    frame.run_id = entry->run_id;
    frame.cycle = entry->cycle;
    frame.team_id = entry->team_id;
    frame.event_type = entry->event_type;

    const uint8_t *payload = (const uint8_t *)&frame;
    uint8_t checksum = TELEMETRY_FRAME_CHECKSUM(payload);

    tm_putchar((INT)TELEMETRY_FRAME_MAGIC0);
    tm_putchar((INT)TELEMETRY_FRAME_MAGIC1);
    for(uint32_t i = 0; i < TELEMETRY_FRAME_PAYLOAD_SIZE; i++)
        tm_putchar((INT)payload[i]);
    tm_putchar((INT)checksum);
}

/*
 * TODO(usb): telemetry task hook, same status as
 * board_snapshot_ring_pending() above -- not part of snapshot.h's
 * public interface (core/ never calls this), this is what the
 * board's telemetry task should call periodically to actually push
 * ring contents out over USART1 now that the wire format exists.
 * Currently called from platform/board/main_board.c's sim_task loop;
 * see the cadence comment there for why that call site and interval
 * were chosen.
 *
 * Drains up to BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL pending
 * entries, oldest first, as framed TelemetryFrame_t bytes. Returns
 * the number of frames actually sent this call (0 if nothing was
 * pending). If the producer has overwritten entries this call never
 * got to (drain running slower than snapshot_record() over a long
 * enough stretch), those entries are skipped -- not resent, not
 * reconstructed -- per the overwrite-oldest policy documented in
 * core/telemetry_frame.h; the drain pointer just jumps forward to
 * the oldest entry still actually present in the ring.
 */
uint32_t board_snapshot_ring_drain(void)
{
    uint32_t pending = g_snapshot_seq_written - g_snapshot_seq_drained;

    if(pending > BOARD_SNAPSHOT_RING_CAPACITY)
    {
        g_snapshot_seq_drained = g_snapshot_seq_written - BOARD_SNAPSHOT_RING_CAPACITY;
        pending = BOARD_SNAPSHOT_RING_CAPACITY;
    }

    uint32_t to_send = pending;
    if(to_send > BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL)
        to_send = BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL;

    for(uint32_t i = 0; i < to_send; i++)
    {
        uint32_t index = g_snapshot_seq_drained % BOARD_SNAPSHOT_RING_CAPACITY;
        board_snapshot_send_frame(&g_snapshot_ring[index]);
        g_snapshot_seq_drained++;
    }

    return to_send;
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
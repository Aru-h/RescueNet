#ifndef TELEMETRY_FRAME_H
#define TELEMETRY_FRAME_H

#include <stdint.h>

/*
 * Wire contract for draining platform/board/snapshot.c's ring buffer
 * to the laptop over USART1 (the same physical UART tm_printf debug
 * text already uses - there is no second UART or USB CDC on this
 * board). Nothing implements the drain yet (TODO(usb) in
 * snapshot.c); this header only fixes the shape of the bytes so the
 * board-side sender and laptop-side reader can be built against the
 * same contract independently.
 *
 * This is a wire-format definition, not a general telemetry API:
 * plain struct/macro defs only, no encode/decode logic here.
 */

/*
 * ---------------------------------------------------------------
 * 1. Frame payload
 * ---------------------------------------------------------------
 * Mirrors BoardSnapshotEntry_t in platform/board/snapshot.c field
 * for field. Do NOT add fields here: the board ring intentionally
 * drops the other 44 CSV columns the laptop-native snapshot.c
 * captures (see the top-of-file comment in platform/board/
 * snapshot.c) to keep the on-device ring cheap. If board-side
 * capture ever grows beyond these 4 fields, that is a schema
 * change to BoardSnapshotEntry_t first, with this struct updated
 * to match - not the other way around.
 */
typedef struct __attribute__((packed))
{
    uint32_t run_id;     /* matches BoardSnapshotEntry_t.run_id */
    uint32_t cycle;      /* matches BoardSnapshotEntry_t.cycle */
    uint8_t  team_id;    /* matches BoardSnapshotEntry_t.team_id */
    uint8_t  event_type; /* matches BoardSnapshotEntry_t.event_type (EventType_t) */
} TelemetryFrame_t;

#define TELEMETRY_FRAME_PAYLOAD_SIZE ((uint32_t)sizeof(TelemetryFrame_t)) /* 10 bytes */

/*
 * ---------------------------------------------------------------
 * 2. On-wire framing (sync + payload + checksum)
 * ---------------------------------------------------------------
 * USART1 already carries free-running tm_printf ASCII debug lines,
 * so a listener that attaches mid-stream must be able to tell a
 * binary frame apart from arbitrary text on a byte boundary, and
 * resynchronize after any drop/glitch without a separate channel.
 *
 * Layout, all fields little-endian:
 *
 *   byte 0-1   magic          TELEMETRY_FRAME_MAGIC0, MAGIC1
 *   byte 2-11  payload        TelemetryFrame_t, packed, 10 bytes
 *   byte 12    checksum       TELEMETRY_FRAME_CHECKSUM() over payload only
 *
 * Total frame size: TELEMETRY_FRAME_WIRE_SIZE bytes.
 *
 * Magic bytes: 0xA5 0x5A. Both bytes are non-printable/non-ASCII
 * (outside 0x20-0x7E), so a two-byte scan for this exact pair
 * cannot be produced by tm_printf's text lines, and a reader can
 * resync by shifting one byte at a time until it sees this pair
 * followed by a payload+checksum that checks out. Reject the
 * candidate frame and keep scanning if the checksum doesn't match -
 * a magic-byte collision inside text or a torn frame is the
 * expected failure mode, not an error condition.
 *
 * Checksum: single-byte additive checksum (sum of all
 * TELEMETRY_FRAME_PAYLOAD_SIZE payload bytes, mod 256), not CRC8.
 * This is deliberately the cheapest option that still catches
 * random single-byte corruption/torn frames for resync purposes;
 * it is not meant to catch adversarial or burst corruption. If a
 * stronger guarantee is ever needed, upgrade this macro to a real
 * CRC8 - the wire size (1 checksum byte) does not need to change.
 */

#define TELEMETRY_FRAME_MAGIC0 ((uint8_t)0xA5u)
#define TELEMETRY_FRAME_MAGIC1 ((uint8_t)0x5Au)

#define TELEMETRY_FRAME_WIRE_SIZE \
    ((uint32_t)(2u + TELEMETRY_FRAME_PAYLOAD_SIZE + 1u))

/*
 * Computes the 1-byte additive checksum over a TELEMETRY_FRAME_PAYLOAD_SIZE-
 * byte payload buffer. `bytes` must point at TELEMETRY_FRAME_PAYLOAD_SIZE
 * readable bytes (e.g. (const uint8_t *)&frame for a TelemetryFrame_t
 * `frame`). Expression-style macro, not a function, to keep this header
 * logic-free; callers needing a real function should wrap it on their
 * own side.
 */
#define TELEMETRY_FRAME_CHECKSUM(bytes) \
    ((uint8_t)( \
        ((const uint8_t *)(bytes))[0] + ((const uint8_t *)(bytes))[1] + \
        ((const uint8_t *)(bytes))[2] + ((const uint8_t *)(bytes))[3] + \
        ((const uint8_t *)(bytes))[4] + ((const uint8_t *)(bytes))[5] + \
        ((const uint8_t *)(bytes))[6] + ((const uint8_t *)(bytes))[7] + \
        ((const uint8_t *)(bytes))[8] + ((const uint8_t *)(bytes))[9] \
    ))

/*
 * ---------------------------------------------------------------
 * 3. Ring overflow policy
 * ---------------------------------------------------------------
 * DECISION (permanent, supersedes the "decide deliberately" TODO in
 * platform/board/snapshot.c's snapshot_record()): overflow of
 * BOARD_SNAPSHOT_RING_CAPACITY overwrites the oldest unsent entry.
 * The ring is a fixed-size circular buffer with no backpressure -
 * snapshot_record() must never block the sim tick to wait for USART1
 * drain.
 *
 * Rationale: the ring is a debug/telemetry side-channel, not a
 * source of truth the sim depends on for correctness; the laptop-
 * side CSV (platform/laptop/snapshot.c) remains the authoritative,
 * lossless record when full fidelity matters. Bounding worst-case
 * drain latency's effect on the sim loop is worth more than
 * guaranteeing delivery of every board-side telemetry event.
 *
 * Consequence for the wire format above: a laptop-side reader
 * cannot assume `cycle` values in consecutive received frames are
 * contiguous - gaps mean frames were overwritten before drain, not
 * a transport error. Do not add gap detection here; that is
 * consumer-side logic and does not belong in this frame contract.
 */

#endif /* TELEMETRY_FRAME_H */

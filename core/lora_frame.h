#ifndef LORA_FRAME_H
#define LORA_FRAME_H

#include <stdint.h>

/*
 * On-air envelope for every LoRa packet, regardless of whether it
 * carries a CBBA consensus message (core/cbba.c) or a relayed
 * mission-link payload. Both message kinds ride the same radio, so
 * a receiver needs a way to tell them apart and to know how many of
 * the received bytes are real payload before handing them off.
 *
 * This is a different framing problem from telemetry_frame.h's
 * magic-byte scheme. That scheme exists because USART1 carries free-
 * running ASCII debug text as well as binary frames, so a reader
 * attaching mid-stream has to scan for a frame boundary on a byte
 * boundary it doesn't otherwise know. LoRa has no such problem: the
 * radio driver hands the receiver one already-delimited packet at a
 * time (no interleaved text, no torn stream to resync within), so
 * there's nothing to scan for. A type+length header is enough - the
 * length says how many payload bytes are real, and the type says how
 * to interpret them.
 *
 * Layout (all fields little-endian on this target; frame_type/
 * payload_len/checksum are single bytes so endianness is moot for
 * them - only the multi-byte fields inside the payload itself, which
 * this header has no opinion on, could care):
 *
 *   byte 0        frame_type    LORA_FRAME_TYPE_*
 *   byte 1        payload_len   bytes of real data in payload[]
 *   byte 2..N-1   payload       payload_len bytes (N = 2 + payload_len)
 *   byte N        checksum      additive sum mod 256 over payload[0..payload_len)
 *
 * Only frame_type + payload_len + the *used* payload bytes + checksum
 * go out over the air - lora_frame_pack() does not transmit the full
 * LORA_FRAME_MAX_PAYLOAD every time, only what payload_len says is
 * real. Total on-air size for a given frame is therefore
 * (2 + payload_len + 1) bytes, which varies per frame; see
 * LORA_FRAME_MAX_WIRE_SIZE below for the worst case.
 *
 * Checksum: same scheme as TELEMETRY_FRAME_CHECKSUM() in
 * telemetry_frame.h - single-byte additive checksum over the payload
 * bytes only, mod 256. Deliberately cheap; catches random single-byte
 * corruption, not adversarial or burst corruption. On a checksum
 * failure, treat it as an expected-under-real-RF outcome, not a fatal
 * error - see lora_frame_try_parse()'s -1 case below, which follows
 * the same "drop and resync" policy tools/telemetry_bridge.py already
 * applies on its checksum mismatches for the USART1 case.
 */

#define LORA_FRAME_TYPE_CBBA          0u
#define LORA_FRAME_TYPE_MISSION_LINK  1u

/*
 * CBBAMessage_t (core/cbba.c, private to that file) is the largest
 * payload this envelope needs to carry. Its size depends on
 * NUM_TEAMS and NUM_INCIDENTS (core/mission_state.h), currently 10
 * and 20:
 *
 *   sender_id                    1  byte
 *   winner[NUM_INCIDENTS]        20 bytes (uint8_t * 20)
 *   winning_bid[NUM_INCIDENTS]   80 bytes (float * 20, 4-byte aligned)
 *   timestamp[NUM_TEAMS]         40 bytes (uint32_t * 10)
 *                                = 144 bytes with natural struct
 *                                  alignment/padding.
 *
 * Confirmed at 144 by compiling a sizeof() probe with
 * `arm-none-eabi-gcc -mcpu=cortex-m55` and reading the byte back out
 * of the resulting .rodata via objdump - not hand-calculated. If
 * NUM_TEAMS/NUM_INCIDENTS ever change, re-run that probe and update
 * this constant to match; don't let it silently drift out of sync
 * with the real CBBAMessage_t.
 */
#define LORA_FRAME_MAX_PAYLOAD 144u

#define LORA_FRAME_HEADER_SIZE ((uint32_t)2u) /* frame_type + payload_len */

/* Smallest possible on-air frame: header + checksum, zero payload bytes. */
#define LORA_FRAME_MIN_WIRE_SIZE (LORA_FRAME_HEADER_SIZE + 1u)

/* Largest possible on-air frame: header + a full LORA_FRAME_MAX_PAYLOAD
   payload + checksum. Useful for sizing TX/RX radio buffers. */
#define LORA_FRAME_MAX_WIRE_SIZE \
    (LORA_FRAME_HEADER_SIZE + LORA_FRAME_MAX_PAYLOAD + 1u)

/*
 * In-memory representation of one frame. This is sized for the worst
 * case (a full LORA_FRAME_MAX_PAYLOAD payload) for callers that want
 * a fixed-size value to build a frame into before packing; it is not
 * the on-air layout itself, which is variable-length (see
 * LORA_FRAME_MIN_WIRE_SIZE / LORA_FRAME_MAX_WIRE_SIZE above) and is
 * produced by lora_frame_pack(), not by taking sizeof(LoRaFrame_t)
 * bytes of this struct.
 */
typedef struct __attribute__((packed))
{
    uint8_t frame_type;      /* LORA_FRAME_TYPE_* */
    uint8_t payload_len;     /* actual bytes used in payload[] below */
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t checksum;        /* additive sum mod 256 over payload[0..payload_len), same scheme as telemetry_frame.h */
} LoRaFrame_t;

/*
 * Packs frame_type + payload into out_buf using the on-air layout
 * documented above: frame_type, payload_len, payload[0..payload_len),
 * checksum - only the bytes actually used, not a full
 * LORA_FRAME_MAX_PAYLOAD every time. out_buf must have room for at
 * least (2 + payload_len + 1) bytes (LORA_FRAME_MAX_WIRE_SIZE covers
 * the worst case for any payload_len).
 *
 * Returns the total number of bytes written to out_buf on success.
 * Returns 0 and writes nothing if payload_len exceeds
 * LORA_FRAME_MAX_PAYLOAD - that is a caller error (data too big for
 * this envelope), not something to silently truncate.
 */
uint32_t lora_frame_pack(uint8_t frame_type, const uint8_t *payload,
                          uint8_t payload_len, uint8_t *out_buf);

/*
 * Parses one frame out of buf (buf_len bytes available).
 *
 * Returns:
 *    1  a complete, checksum-valid frame was found. out_type,
 *       out_payload (payload_len bytes copied in) and out_payload_len
 *       are filled in.
 *    0  buf does not yet contain a complete frame (fewer than
 *       payload_len + 3 bytes available, where payload_len is read
 *       from buf[1]). Nothing is written to the out_* parameters.
 *   -1  a complete frame-shaped span was found but its checksum did
 *       not verify (or its declared payload_len exceeds
 *       LORA_FRAME_MAX_PAYLOAD, which can never be a valid frame
 *       regardless of how many more bytes arrive). Nothing is written
 *       to the out_* parameters.
 *
 * -1 is an expected outcome under real RF conditions (bit errors,
 * partial packets), not a fatal one - the same policy question
 * tools/telemetry_bridge.py already answers for the USART1 case: drop
 * the bad frame and resync rather than treating it as an error to
 * propagate. Since each LoRa packet is already a whole unit off the
 * radio (unlike the USART1 stream), "resync" here just means the
 * caller discards this packet and waits for the next one - there is
 * no partial-byte rescan to do within a single received packet.
 *
 * out_payload must have room for at least LORA_FRAME_MAX_PAYLOAD bytes.
 */
int lora_frame_try_parse(const uint8_t *buf, uint32_t buf_len,
                          uint8_t *out_type, uint8_t *out_payload,
                          uint8_t *out_payload_len);

#endif /* LORA_FRAME_H */

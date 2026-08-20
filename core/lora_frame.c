#include "lora_frame.h"

/*
 * Additive checksum over `len` bytes at `bytes`, mod 256 - same scheme
 * as TELEMETRY_FRAME_CHECKSUM() in telemetry_frame.h, just expressed
 * as a function here (this file, unlike that header, is allowed to
 * hold logic) instead of a fixed-width macro, since payload length
 * varies per frame here instead of being fixed at 10 bytes.
 */
static uint8_t lora_frame_checksum(const uint8_t *bytes, uint8_t len)
{
    uint8_t sum = 0u;
    uint8_t i;

    for (i = 0u; i < len; i++)
    {
        sum = (uint8_t)(sum + bytes[i]);
    }

    return sum;
}

uint32_t lora_frame_pack(uint8_t frame_type, const uint8_t *payload,
                          uint8_t payload_len, uint8_t *out_buf)
{
    uint8_t i;

    if (payload_len > LORA_FRAME_MAX_PAYLOAD)
    {
        /* Caller error: more payload than this envelope can carry.
           Write nothing and report zero bytes written rather than
           silently truncating the caller's data. */
        return 0u;
    }

    out_buf[0] = frame_type;
    out_buf[1] = payload_len;

    for (i = 0u; i < payload_len; i++)
    {
        out_buf[2u + i] = payload[i];
    }

    out_buf[2u + payload_len] = lora_frame_checksum(payload, payload_len);

    return LORA_FRAME_HEADER_SIZE + (uint32_t)payload_len + 1u;
}

int lora_frame_try_parse(const uint8_t *buf, uint32_t buf_len,
                          uint8_t *out_type, uint8_t *out_payload,
                          uint8_t *out_payload_len)
{
    uint8_t payload_len;
    uint32_t frame_size;
    uint8_t expected_checksum;
    uint8_t i;

    if (buf_len < LORA_FRAME_HEADER_SIZE)
    {
        /* Not even frame_type + payload_len have arrived yet. */
        return 0;
    }

    payload_len = buf[1];

    if (payload_len > LORA_FRAME_MAX_PAYLOAD)
    {
        /* This declared length can never belong to a valid frame no
           matter how many more bytes show up - treat it the same as
           a checksum failure (drop and resync is the caller's call),
           rather than waiting forever for an unreachable byte count. */
        return -1;
    }

    frame_size = LORA_FRAME_HEADER_SIZE + (uint32_t)payload_len + 1u;

    if (buf_len < frame_size)
    {
        /* Header is readable but the payload/checksum bytes haven't
           all arrived yet. */
        return 0;
    }

    expected_checksum = lora_frame_checksum(&buf[2], payload_len);

    if (buf[2u + payload_len] != expected_checksum)
    {
        return -1;
    }

    *out_type = buf[0];
    *out_payload_len = payload_len;

    for (i = 0u; i < payload_len; i++)
    {
        out_payload[i] = buf[2u + i];
    }

    return 1;
}

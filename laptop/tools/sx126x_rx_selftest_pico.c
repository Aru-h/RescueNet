/*
 * Standalone self-test for the SX126x RX-ingestion sequence added by
 * board/Raspberry_Pi_Pico/sx1262_hal.c's sx1262_enter_rx()/
 * sx1262_rx_poll() (no hardware/board required - desktop gcc only,
 * same spirit as laptop/tools/lora_frame_selftest.c and
 * laptop/tools/cbba_board_selftest.c).
 *
 * SCOPE: this deliberately does NOT link or call anything under
 * board/Raspberry_Pi_Pico/ or board/STM32N6570-DK/ - both of those
 * need real BSP headers (µT-Kernel, libbsp, CMSIS device headers)
 * this desktop build doesn't have. What it DOES exercise is
 * core/vendor/sx126x_driver/src/sx126x.c (the vendored driver core
 * BOTH boards' HALs sit on top of - genuinely shared, not board-
 * specific) plus core/lora_frame.c and core/comms_transport.c, via a
 * fake sx126x_hal_write()/sx126x_hal_read()/reset()/wakeup()
 * implementation that stands in for either board's real SPI/GPIO
 * code. This is the same reasoning board/Raspberry_Pi_Pico/
 * sx1262_hal.c's own RX-section comment gives for not re-deriving
 * anything board-specific: the RX *algorithm* (enter RX -> poll IRQ
 * status -> on RxDone, read the buffer -> parse -> deliver -> clear
 * IRQ -> re-arm) is identical in both boards' HALs because it's built
 * entirely out of these same driver-core calls, so one fake-HAL test
 * of that sequence covers both, and there is exactly one copy of it
 * in this repo (this file) rather than a per-board duplicate.
 *
 * What this test does NOT cover, and cannot from the desktop: real
 * DIO1 GPIO timing/wiring, real BUSY-pin handshaking, the RXEN/TXEN
 * opcode-snoop RF-switch logic (board-specific GPIO side effects, not
 * driver-core behavior) - those need real hardware bring-up, same
 * "next concrete step once you're ready to actually flash the board"
 * caveat every board HAL file in this repo already carries.
 *
 * Covers:
 *   1. Happy path: fake radio reports RX_DONE via get_irq_status(),
 *      get_rx_buffer_status()/read_buffer() hand back a real AEAD-
 *      encrypted frame (built with the real lora_frame_pack()/
 *      lora_crypto.c, not a hand-rolled fake), lora_frame_try_parse()
 *      accepts it, comms_transport_deliver_received() pushes it, and
 *      it's visible afterward via comms_transport_poll_received().
 *      Also asserts clear_irq_status() was called with exactly the
 *      bits get_irq_status() reported, and that standby+set_rx (the
 *      enter/re-arm sequence) was issued both before AND after -
 *      i.e. re-arm actually happens post-handling, not just on the
 *      first entry.
 *   2. Corrupted frame (one ciphertext byte flipped after the fake
 *      radio "received" it): lora_frame_try_parse() rejects it, nothing
 *      reaches the inbox, but the poll still clears IRQ status and
 *      still re-arms RX (a dropped packet must not leave the radio
 *      stuck in standby).
 *   3. No IRQ pending (get_irq_status() returns SX126X_IRQ_NONE):
 *      the poll does not touch get_rx_buffer_status/read_buffer at
 *      all (nothing to read), but still clears (a no-op clear of an
 *      empty mask) and still re-arms.
 *   4. RX_DONE together with an unrelated bit (TIMEOUT) set at the
 *      same time: the payload still gets delivered, and the clear
 *      covers both bits, not just RX_DONE - proves the poll clears
 *      whatever was actually observed, not a hardcoded RX_DONE-only
 *      mask (see sx1262_rx_poll()'s own comment on why: Timeout is
 *      also routed to DIO1 and must not be left latched).
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore \
 *       -Icore/vendor/sx126x_driver/src \
 *       -o sx126x_rx_selftest laptop/tools/sx126x_rx_selftest_pico.c \
 *       core/vendor/sx126x_driver/src/sx126x.c \
 *       core/lora_frame.c core/lora_crypto.c \
 *       core/aead_chacha20poly1305.c core/chacha20.c core/poly1305.c \
 *       core/comms_transport.c core/mission_state.c -lm
 * Run:
 *   ./sx126x_rx_selftest
 *
 * (core/mission_state.c is pulled in only because core/comms_transport.c
 * references g_current_tick from mission_state.h - same reason
 * laptop/tools/mission_link_selftest.c needs it. Nothing below calls
 * into mission_state.c directly.)
 */

#include "sx126x.h"
#include "sx126x_hal.h"
#include "lora_frame.h"
#include "lora_crypto.h"
#include "comms_transport.h"

#include <stdio.h>
#include <string.h>
#include <assert.h>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/*
 * ---------------------------------------------------------------
 * Fake sx126x_hal.h implementation - a fake radio, not a mock of the
 * driver. Records every command byte-for-byte and serves scripted
 * responses for the handful of opcodes this test's RX sequence
 * actually issues (SetStandby, SetRx, GetIrqStatus, ClrIrqStatus,
 * GetRxBufferStatus, ReadBuffer). Anything else is accepted silently
 * (matching a real radio's tolerance for config commands it doesn't
 * need to react to for this test's purposes).
 * ---------------------------------------------------------------
 */

#define FAKE_OPCODE_SET_STANDBY          0x80u
#define FAKE_OPCODE_SET_RX               0x82u
#define FAKE_OPCODE_READ_BUFFER          0x1Eu
#define FAKE_OPCODE_GET_IRQ_STATUS       0x12u
#define FAKE_OPCODE_CLR_IRQ_STATUS       0x02u
#define FAKE_OPCODE_GET_RX_BUFFER_STATUS 0x13u

static uint16_t g_fake_irq_status;          /* what GetIrqStatus reports next */
static uint16_t g_fake_last_cleared_mask;   /* last ClrIrqStatus argument observed */
static int      g_fake_clear_call_count;
static int      g_fake_standby_call_count;
static int      g_fake_set_rx_call_count;

static uint8_t  g_fake_rx_buf[LORA_FRAME_MAX_WIRE_SIZE];
static uint8_t  g_fake_rx_buf_len;

static void fake_hal_reset_state(void)
{
    g_fake_irq_status = 0u;
    g_fake_last_cleared_mask = 0xFFFFu; /* sentinel: "never cleared" */
    g_fake_clear_call_count = 0;
    g_fake_standby_call_count = 0;
    g_fake_set_rx_call_count = 0;
    g_fake_rx_buf_len = 0u;
    memset(g_fake_rx_buf, 0, sizeof(g_fake_rx_buf));
}

sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command,
                                      const uint16_t command_length,
                                      const uint8_t *data, const uint16_t data_length)
{
    (void)context;
    (void)data;
    (void)data_length;

    if (command_length == 0u)
    {
        return SX126X_HAL_STATUS_OK;
    }

    switch (command[0])
    {
    case FAKE_OPCODE_SET_STANDBY:
        g_fake_standby_call_count++;
        break;
    case FAKE_OPCODE_SET_RX:
        g_fake_set_rx_call_count++;
        break;
    case FAKE_OPCODE_CLR_IRQ_STATUS:
        /* sx126x_clear_irq_status() encodes the 16-bit mask as
           command[1]<<8 | command[2] - see sx126x.c's own
           implementation. */
        CHECK(command_length == 3u);
        g_fake_last_cleared_mask = (uint16_t)(((uint16_t)command[1] << 8) | command[2]);
        g_fake_clear_call_count++;
        break;
    default:
        break;
    }

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command,
                                     const uint16_t command_length,
                                     uint8_t *data, const uint16_t data_length)
{
    (void)context;

    if (command_length == 0u)
    {
        return SX126X_HAL_STATUS_OK;
    }

    switch (command[0])
    {
    case FAKE_OPCODE_GET_IRQ_STATUS:
        /* sx126x_get_irq_status() expects irq_local[0]<<8 | [1]. */
        CHECK(data_length == 2u);
        data[0] = (uint8_t)(g_fake_irq_status >> 8);
        data[1] = (uint8_t)(g_fake_irq_status & 0xFFu);
        break;
    case FAKE_OPCODE_GET_RX_BUFFER_STATUS:
        /* sx126x_get_rx_buffer_status() expects
           [0]=pld_len_in_bytes, [1]=buffer_start_pointer. */
        CHECK(data_length == 2u);
        data[0] = g_fake_rx_buf_len;
        data[1] = 0u; /* buffer_start_pointer - this fake always starts at 0 */
        break;
    case FAKE_OPCODE_READ_BUFFER:
        /* command[1] = offset, per sx126x_read_buffer(). */
        CHECK(command_length == 3u);
        {
            uint8_t offset = command[1];
            uint16_t i;
            for (i = 0u; i < data_length; i++)
            {
                uint16_t src = (uint16_t)offset + i;
                data[i] = (src < g_fake_rx_buf_len) ? g_fake_rx_buf[src] : 0u;
            }
        }
        break;
    default:
        memset(data, 0, data_length);
        break;
    }

    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_reset(const void *context)
{
    (void)context;
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_wakeup(const void *context)
{
    (void)context;
    return SX126X_HAL_STATUS_OK;
}

/*
 * ---------------------------------------------------------------
 * The RX sequence under test - a direct transcription of
 * board/Raspberry_Pi_Pico/sx1262_hal.c's sx1262_enter_rx()/
 * sx1262_rx_poll(), built entirely out of core/vendor/sx126x_driver
 * calls plus core/lora_frame.c/core/comms_transport.c, so it's
 * exercising the exact same shared logic that file calls into - the
 * only thing NOT reproduced here is the board-specific DIO1
 * gpio_get_val() "is anything pending at all" pre-check, which has no
 * driver-core equivalent to fake (see this file's own top-of-file
 * scope note).
 * ---------------------------------------------------------------
 */

static void test_enter_rx(void)
{
    (void)sx126x_set_standby(NULL, SX126X_STANDBY_CFG_RC);
    (void)sx126x_set_rx(NULL, 0u);
}

static void test_rx_poll(void)
{
    sx126x_irq_mask_t irq = SX126X_IRQ_NONE;
    sx126x_rx_buffer_status_t rx_status;

    if (sx126x_get_irq_status(NULL, &irq) != SX126X_STATUS_OK)
    {
        return;
    }

    if ((irq & SX126X_IRQ_RX_DONE) != 0u)
    {
        uint8_t raw[LORA_FRAME_MAX_WIRE_SIZE];
        uint8_t out_type = 0u;
        uint8_t out_sender_board_id = 0u;
        uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
        uint8_t out_payload_len = 0u;

        if (sx126x_get_rx_buffer_status(NULL, &rx_status) == SX126X_STATUS_OK &&
            rx_status.pld_len_in_bytes <= sizeof(raw) &&
            sx126x_read_buffer(NULL, rx_status.buffer_start_pointer, raw,
                                rx_status.pld_len_in_bytes) == SX126X_STATUS_OK)
        {
            if (lora_frame_try_parse(raw, rx_status.pld_len_in_bytes, &out_type,
                                      &out_sender_board_id, out_payload,
                                      &out_payload_len) == 1)
            {
                (void)out_sender_board_id;
                (void)comms_transport_deliver_received(out_type, out_payload,
                                                         out_payload_len);
            }
        }
    }

    (void)sx126x_clear_irq_status(NULL, irq);
    test_enter_rx();
}

/*
 * ---------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------
 */

static void test_happy_path(void)
{
    uint8_t payload[6] = { 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu, 0xFFu };
    uint32_t packed_len;
    uint8_t out_sender_team_id = 0xFFu;
    uint8_t out_frame_type = 0xFFu;
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_payload_len = 0u;

    lora_crypto_reset();
    comms_transport_init();
    fake_hal_reset_state();

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_MISSION_LINK, 3u, payload,
                                  (uint8_t)sizeof(payload), g_fake_rx_buf);
    CHECK(packed_len > 0u && packed_len <= sizeof(g_fake_rx_buf));
    g_fake_rx_buf_len = (uint8_t)packed_len;

    test_enter_rx();
    CHECK(g_fake_standby_call_count == 1);
    CHECK(g_fake_set_rx_call_count == 1);

    g_fake_irq_status = SX126X_IRQ_RX_DONE;
    test_rx_poll();

    CHECK(g_fake_clear_call_count == 1);
    CHECK(g_fake_last_cleared_mask == (uint16_t)SX126X_IRQ_RX_DONE);
    /* re-armed after handling - standby/set_rx each called a second time */
    CHECK(g_fake_standby_call_count == 2);
    CHECK(g_fake_set_rx_call_count == 2);

    CHECK(comms_transport_poll_received(&out_sender_team_id, &out_frame_type,
                                         out_payload, &out_payload_len) == 1);
    CHECK(out_sender_team_id == COMMS_TRANSPORT_UNKNOWN_SENDER);
    CHECK(out_frame_type == LORA_FRAME_TYPE_MISSION_LINK);
    CHECK(out_payload_len == sizeof(payload));
    CHECK(memcmp(out_payload, payload, sizeof(payload)) == 0);

    /* Inbox is now empty - nothing else was delivered. */
    CHECK(comms_transport_poll_received(&out_sender_team_id, &out_frame_type,
                                         out_payload, &out_payload_len) == 0);
}

static void test_corrupted_frame_dropped_but_still_rearms(void)
{
    uint8_t payload[4] = { 0x01u, 0x02u, 0x03u, 0x04u };
    uint32_t packed_len;
    uint8_t out_sender_team_id, out_frame_type, out_payload_len;
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];

    lora_crypto_reset();
    comms_transport_init();
    fake_hal_reset_state();

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, 2u, payload,
                                  (uint8_t)sizeof(payload), g_fake_rx_buf);
    CHECK(packed_len > 0u);
    /* Flip one ciphertext byte - same corruption approach
       laptop/tools/lora_frame_selftest.c's own checksum-reject test
       uses, right after the 3-byte cleartext header so this hits the
       AEAD-protected region, not the AAD. */
    g_fake_rx_buf[LORA_FRAME_HEADER_SIZE] ^= 0xFFu;
    g_fake_rx_buf_len = (uint8_t)packed_len;

    test_enter_rx();
    g_fake_irq_status = SX126X_IRQ_RX_DONE;
    test_rx_poll();

    /* Dropped: nothing in the inbox. */
    CHECK(comms_transport_poll_received(&out_sender_team_id, &out_frame_type,
                                         out_payload, &out_payload_len) == 0);

    /* But the radio still got cleared and re-armed - a dropped packet
       must not leave RX disabled. */
    CHECK(g_fake_clear_call_count == 1);
    CHECK(g_fake_last_cleared_mask == (uint16_t)SX126X_IRQ_RX_DONE);
    CHECK(g_fake_standby_call_count == 2);
    CHECK(g_fake_set_rx_call_count == 2);
}

static void test_no_irq_pending_is_a_cheap_noop(void)
{
    lora_crypto_reset();
    comms_transport_init();
    fake_hal_reset_state();

    test_enter_rx();
    g_fake_irq_status = SX126X_IRQ_NONE;
    test_rx_poll();

    /* Nothing to read, nothing delivered, but clear+re-arm still ran
       (clearing an empty mask is harmless, and re-arming after a
       call that turned out to have nothing pending is correct too -
       this mirrors the Timeout case, which also lands here). */
    CHECK(g_fake_clear_call_count == 1);
    CHECK(g_fake_last_cleared_mask == 0u);
    CHECK(g_fake_standby_call_count == 2);
    CHECK(g_fake_set_rx_call_count == 2);
}

static void test_rx_done_with_timeout_bit_also_set(void)
{
    uint8_t payload[3] = { 0x7Au, 0x7Bu, 0x7Cu };
    uint32_t packed_len;
    uint8_t out_sender_team_id, out_frame_type, out_payload_len;
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];

    lora_crypto_reset();
    comms_transport_init();
    fake_hal_reset_state();

    packed_len = lora_frame_pack(LORA_FRAME_TYPE_HANDSHAKE, 4u, payload,
                                  (uint8_t)sizeof(payload), g_fake_rx_buf);
    CHECK(packed_len > 0u);
    g_fake_rx_buf_len = (uint8_t)packed_len;

    test_enter_rx();
    /* Both bits observed at once - a real radio can legitimately
       report more than one IRQ bit in a single GetIrqStatus read. */
    g_fake_irq_status = (uint16_t)(SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT);
    test_rx_poll();

    CHECK(comms_transport_poll_received(&out_sender_team_id, &out_frame_type,
                                         out_payload, &out_payload_len) == 1);
    CHECK(out_frame_type == LORA_FRAME_TYPE_HANDSHAKE);

    /* Clear covered BOTH bits that were actually set, not a
       hardcoded RX_DONE-only mask. */
    CHECK(g_fake_last_cleared_mask ==
          (uint16_t)(SX126X_IRQ_RX_DONE | SX126X_IRQ_TIMEOUT));
}

int main(void)
{
    test_happy_path();
    test_corrupted_frame_dropped_but_still_rearms();
    test_no_irq_pending_is_a_cheap_noop();
    test_rx_done_with_timeout_bit_also_set();

    if (g_failures == 0)
    {
        printf("sx126x_rx_selftest: all tests passed\n");
        return 0;
    }

    printf("sx126x_rx_selftest: %d failure(s)\n", g_failures);
    return 1;
}

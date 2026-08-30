/*
 * Standalone self-test for the RX ingestion pipeline added to
 * board/STM32N6570-DK/sx1262_hal.c's sx1262_poll_rx() (no hardware/
 * board required - desktop gcc only, same "no real radio in CI"
 * status as laptop/tools/lora_frame_selftest.c and
 * laptop/tools/two_board_harness.c).
 *
 * sx1262_poll_rx() itself can't be linked here - it's defined in
 * board/STM32N6570-DK/sx1262_hal.c, which talks to real STM32N657
 * SPI5/GPIO registers via stm32n657xx.h (see that file's own top
 * comment) and has no meaning off real hardware. What CAN be tested
 * on a desktop is everything sx1262_poll_rx() calls beneath the DIO1/
 * GPIO layer: sx126x_get_irq_status() / sx126x_clear_irq_status() /
 * sx126x_get_rx_buffer_status() / sx126x_read_buffer() (the real
 * vendored driver core, core/vendor/sx126x_driver/src/sx126x.c,
 * unmodified) -> lora_frame_try_parse() (core/lora_frame.c,
 * unmodified) -> comms_transport_deliver_received() (core/
 * comms_transport.c, unmodified).
 *
 * To reach that chain without real hardware, this file provides a
 * second, host-only implementation of the driver's four sx126x_hal_*
 * entry points (sx126x_hal.h) - a fake in-memory radio: a small
 * register/FIFO model that answers exactly the commands
 * sx126x_get_irq_status()/sx126x_clear_irq_status()/
 * sx126x_get_rx_buffer_status()/sx126x_read_buffer() actually issue
 * (opcodes cross-checked against core/vendor/sx126x_driver/src/
 * sx126x.c itself, not guessed - see fake_sx126x_hal_read()/_write()
 * below for the exact opcode values each responds to). This is a
 * SEPARATE translation unit from board/STM32N6570-DK/sx1262_hal.c
 * (which provides the real, strong versions of the same four
 * functions) - the two are never linked together, so there's no
 * multiple-definition conflict; whichever one a given build links
 * against is entirely determined by which .c files are on that
 * build's command line (this file's own "Build:" comment below vs.
 * the Makefile's check-board-compile/-link).
 *
 * simulate_rx_poll() below is this file's own stand-in for
 * sx1262_poll_rx()'s post-DIO1-check logic (get_irq_status ->
 * clear_irq_status -> on RxDone, get_rx_buffer_status -> read_buffer
 * -> lora_frame_try_parse() -> on rc==1, deliver_received()) - kept
 * deliberately in lock-step with that function's real logic (see the
 * comment on simulate_rx_poll() itself for the specific line-by-line
 * correspondence) since it can't just call it directly.
 *
 * Exercises:
 *   1. Happy path: a well-formed packed frame, loaded into the fake
 *      radio's RX FIFO with RxDone pending, round-trips through
 *      simulate_rx_poll() all the way to comms_transport_poll_received()
 *      with the same type/payload/payload_len it was packed with.
 *   2. Corrupted path: same frame with one ciphertext byte flipped
 *      post-pack (real RF bit-flip corruption, same technique
 *      lora_frame_selftest.c's own checksum-reject test uses) is
 *      rejected by lora_frame_try_parse() (rc != 1) and never reaches
 *      comms_transport_deliver_received() - comms_transport_poll_received()
 *      confirms nothing was queued.
 *   3. IRQ bits are actually cleared by simulate_rx_poll() in both
 *      cases (mirrors sx1262_poll_rx()'s own "clear in every case, so
 *      DIO1 deasserts" requirement) - checked by reading irq status
 *      again afterward and confirming it's back to SX126X_IRQ_NONE.
 *   4. Timeout-only (no RxDone) is a no-op as far as delivery is
 *      concerned - nothing queued, matching sx1262_poll_rx()'s "TxDone
 *      and/or Timeout, no RxDone" branch (this test doesn't exercise
 *      that branch's sx1262_enter_rx() re-arm call, which is board-
 *      only - only the "nothing gets delivered" half is host-testable).
 *
 * Every test calls lora_crypto_reset() and comms_transport_init()
 * first, same reasoning as lora_frame_selftest.c's own tests: no
 * leaking TX-counter/replay/inbox state between tests.
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Icore -Ilaptop \
 *       -Icore/vendor/sx126x_driver/src \
 *       -o sx1262_rx_selftest laptop/tools/sx1262_rx_selftest.c \
 *       core/aead_chacha20poly1305.c core/allocator.c core/cbba.c \
 *       core/chacha20.c core/comms_transport.c core/environment.c \
 *       core/fault_config.c core/fault_injector.c core/grid.c \
 *       core/incident.c core/lora_crypto.c core/lora_frame.c \
 *       core/mission_execution_twin.c core/mission_link.c \
 *       core/mission_link_handshake.c core/mission_link_override.c \
 *       core/mission_state.c core/mission_state_utils.c core/path.c \
 *       core/poly1305.c core/random.c core/recovery_manager.c \
 *       core/safezone.c core/scoring.c core/search_zones.c \
 *       core/sim_runner.c core/simulation.c core/team.c \
 *       core/test_metrics.c core/watchdog.c \
 *       laptop/debug.c laptop/snapshot.c \
 *       core/vendor/sx126x_driver/src/sx126x.c -lm
 * Run:
 *   ./sx1262_rx_selftest
 *
 * (Every core/ source file is listed explicitly above, mirroring
 * CORE_SOURCES in the top-level Makefile - same set check-board/
 * check-pico/check-two-board-harness already link, so nothing here
 * needs to guess which subset comms_transport.c/lora_frame.c actually
 * depend on transitively. laptop/debug.c + laptop/snapshot.c supply
 * debug_trace()/snapshot_record() and friends, which several of
 * core/'s .c files call and which core/ itself does not define - same
 * reason the top-level Makefile's own LAPTOP_SOURCES includes them.)
 */

#include "sx126x.h"
#include "sx126x_hal.h"

#include "lora_frame.h"
#include "lora_crypto.h"
#include "comms_transport.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

/*
 * -----------------------------------------------------------------------
 * --- Fake radio: in-memory model of exactly the state
 *     sx126x_get_irq_status()/sx126x_clear_irq_status()/
 *     sx126x_get_rx_buffer_status()/sx126x_read_buffer() touch --------
 * -----------------------------------------------------------------------
 * Opcodes below are the driver's own private sx126x_commands_e values
 * (core/vendor/sx126x_driver/src/sx126x.c) - re-declared here since
 * that enum is private to that .c file, cross-checked against it
 * directly rather than assumed:
 *   SX126X_GET_IRQ_STATUS       = 0x12
 *   SX126X_CLR_IRQ_STATUS       = 0x02
 *   SX126X_GET_RX_BUFFER_STATUS = 0x13
 *   SX126X_READ_BUFFER          = 0x1E
 * Every other opcode this test's own call path issues (SetStandby
 * 0x80, SetRx 0x82) only needs to be accepted, not modeled - neither
 * changes anything simulate_rx_poll() below reads back.
 */
#define FAKE_OPCODE_GET_IRQ_STATUS       0x12u
#define FAKE_OPCODE_CLR_IRQ_STATUS       0x02u
#define FAKE_OPCODE_GET_RX_BUFFER_STATUS 0x13u
#define FAKE_OPCODE_READ_BUFFER          0x1Eu

static uint16_t g_fake_irq_status;
static uint8_t g_fake_rx_fifo[LORA_FRAME_MAX_WIRE_SIZE];
static uint8_t g_fake_rx_pld_len;
static uint8_t g_fake_rx_start_ptr;

/* Loads one "received" packet into the fake radio's FIFO (always at
   offset 0, matching sx126x_set_buffer_base_address(NULL, 0u, 0u) in
   board/STM32N6570-DK/sx1262_hal.c's ensure_radio_configured()) and
   raises RxDone - the fake-radio equivalent of a real DIO1 assertion
   after a reception. */
static void fake_radio_load_rx_packet(const uint8_t *bytes, uint32_t len)
{
    assert(len <= sizeof(g_fake_rx_fifo));

    memcpy(g_fake_rx_fifo, bytes, len);
    g_fake_rx_pld_len = (uint8_t)len;
    g_fake_rx_start_ptr = 0u;
    g_fake_irq_status = (uint16_t)SX126X_IRQ_RX_DONE;
}

sx126x_hal_status_t sx126x_hal_write(const void *context, const uint8_t *command,
                                      const uint16_t command_length, const uint8_t *data,
                                      const uint16_t data_length)
{
    (void)context;
    (void)data;
    (void)data_length;

    if (command_length > 0u && command[0] == FAKE_OPCODE_CLR_IRQ_STATUS)
    {
        /* SX126X_SIZE_CLR_IRQ_STATUS == 3: opcode + 2 mask bytes,
           big-endian (matches sx126x_clear_irq_status()'s own
           packing in sx126x.c). */
        uint16_t mask = (command_length >= 3u)
                             ? (uint16_t)(((uint16_t)command[1] << 8) | (uint16_t)command[2])
                             : 0u;
        g_fake_irq_status = (uint16_t)(g_fake_irq_status & ~mask);
    }

    /* Every other opcode (SetStandby, SetRx, ...) - accept, nothing
       to model; see this file's top comment. */
    return SX126X_HAL_STATUS_OK;
}

sx126x_hal_status_t sx126x_hal_read(const void *context, const uint8_t *command,
                                     const uint16_t command_length, uint8_t *data,
                                     const uint16_t data_length)
{
    (void)context;

    if (command_length == 0u)
    {
        return SX126X_HAL_STATUS_OK;
    }

    switch (command[0])
    {
    case FAKE_OPCODE_GET_IRQ_STATUS:
        /* SX126X_SIZE_GET_IRQ_STATUS data_length == 2, big-endian -
           matches sx126x_get_irq_status()'s own unpacking. */
        if (data_length >= 2u)
        {
            data[0] = (uint8_t)(g_fake_irq_status >> 8);
            data[1] = (uint8_t)(g_fake_irq_status >> 0);
        }
        break;

    case FAKE_OPCODE_GET_RX_BUFFER_STATUS:
        /* data_length == 2: pld_len_in_bytes, buffer_start_pointer -
           matches sx126x_get_rx_buffer_status()'s own unpacking. */
        if (data_length >= 2u)
        {
            data[0] = g_fake_rx_pld_len;
            data[1] = g_fake_rx_start_ptr;
        }
        break;

    case FAKE_OPCODE_READ_BUFFER:
    {
        /* command[1] is the offset byte sx126x_read_buffer() passes
           through (SX126X_READ_BUFFER, offset, NOP) - copy that many
           bytes starting there out of the fake FIFO. */
        uint8_t offset = (command_length >= 2u) ? command[1] : 0u;
        uint16_t i;

        for (i = 0u; i < data_length; i++)
        {
            uint32_t src = (uint32_t)offset + i;
            data[i] = (src < sizeof(g_fake_rx_fifo)) ? g_fake_rx_fifo[src] : 0u;
        }
        break;
    }

    default:
        /* Nothing this test's call path reads besides the three
           above - zero-fill defensively rather than leave data
           uninitialized. */
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
 * -----------------------------------------------------------------------
 * --- simulate_rx_poll() - board/STM32N6570-DK/sx1262_hal.c's
 *     sx1262_poll_rx(), minus the DIO1 GPIO gate at its top (host has
 *     no GPIO - the fake radio's irq status standing at non-zero is
 *     this test's substitute "DIO1 asserted" signal) -----------------
 * -----------------------------------------------------------------------
 * Kept in lock-step with sx1262_poll_rx()'s real logic:
 *   get_irq_status -> clear_irq_status(irq_status) unconditionally ->
 *   if !(irq_status & RX_DONE) return -> get_rx_buffer_status ->
 *   read_buffer -> lora_frame_try_parse() -> on rc==1, deliver_received().
 * Does NOT include sx1262_poll_rx()'s sx1262_enter_rx() re-arm call on
 * the TxDone/Timeout branch - that call is board-only (drives real
 * SPI/GPIO), not something a host build can meaningfully verify; this
 * test only checks the delivery-or-not outcome that branch shares with
 * the real function (nothing gets delivered).
 *
 * Returns lora_frame_try_parse()'s own rc (1/0/-1) when RxDone was
 * seen, or 2 (a sentinel outside that range) for the "no RxDone"
 * case, so tests can tell "correctly did nothing because there was
 * nothing to do" apart from "tried to parse and got 0/-1".
 */
static int simulate_rx_poll(uint8_t *out_type, uint8_t *out_payload, uint8_t *out_payload_len)
{
    sx126x_irq_mask_t irq_status;
    sx126x_rx_buffer_status_t rx_buf_status;
    uint8_t frame_bytes[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t sender_board_id;
    int rc;

    if (sx126x_get_irq_status(NULL, &irq_status) != SX126X_STATUS_OK)
    {
        return 2;
    }

    (void)sx126x_clear_irq_status(NULL, irq_status);

    if ((irq_status & SX126X_IRQ_RX_DONE) == 0u)
    {
        return 2;
    }

    if (sx126x_get_rx_buffer_status(NULL, &rx_buf_status) != SX126X_STATUS_OK)
    {
        return 2;
    }

    if (rx_buf_status.pld_len_in_bytes > (uint8_t)sizeof(frame_bytes))
    {
        return 2;
    }

    if (sx126x_read_buffer(NULL, rx_buf_status.buffer_start_pointer,
                            frame_bytes, rx_buf_status.pld_len_in_bytes) != SX126X_STATUS_OK)
    {
        return 2;
    }

    rc = lora_frame_try_parse(frame_bytes, rx_buf_status.pld_len_in_bytes, out_type,
                               &sender_board_id, out_payload, out_payload_len);
    if (rc == 1)
    {
        (void)sender_board_id;
        (void)comms_transport_deliver_received(*out_type, out_payload, *out_payload_len);
    }

    return rc;
}

/*
 * -----------------------------------------------------------------------
 * --- Tests ----------------------------------------------------------
 * -----------------------------------------------------------------------
 */

static void test_happy_path_round_trip(void)
{
    uint8_t payload[7] = { 0xDEu, 0xADu, 0xBEu, 0xEFu, 0x01u, 0x02u, 0x03u };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_type = 0xFFu;
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_payload_len = 0xFFu;
    uint32_t wire_len;
    int rc;

    uint8_t polled_sender_team_id = 0xFFu;
    uint8_t polled_frame_type = 0xFFu;
    uint8_t polled_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t polled_payload_len = 0xFFu;

    sx126x_irq_mask_t irq_after;

    lora_crypto_reset();
    comms_transport_init();

    wire_len = lora_frame_pack(LORA_FRAME_TYPE_MISSION_LINK, 1u /* sender_board_id */,
                                payload, (uint8_t)sizeof(payload), wire_buf);
    CHECK(wire_len == LORA_FRAME_HEADER_SIZE + sizeof(payload) + LORA_FRAME_TRAILER_SIZE);

    fake_radio_load_rx_packet(wire_buf, wire_len);

    rc = simulate_rx_poll(&out_type, out_payload, &out_payload_len);
    CHECK(rc == 1);

    /* IRQ bits actually cleared (DIO1 would deassert on real
       hardware) - see this file's own doc point 3. */
    CHECK(sx126x_get_irq_status(NULL, &irq_after) == SX126X_STATUS_OK);
    CHECK(irq_after == (sx126x_irq_mask_t)SX126X_IRQ_NONE);

    CHECK(comms_transport_poll_received(&polled_sender_team_id, &polled_frame_type,
                                         polled_payload, &polled_payload_len) == 1);
    CHECK(polled_sender_team_id == COMMS_TRANSPORT_UNKNOWN_SENDER);
    CHECK(polled_frame_type == LORA_FRAME_TYPE_MISSION_LINK);
    CHECK(polled_payload_len == sizeof(payload));
    CHECK(memcmp(polled_payload, payload, sizeof(payload)) == 0);

    /* Nothing else queued behind it. */
    CHECK(comms_transport_poll_received(&polled_sender_team_id, &polled_frame_type,
                                         polled_payload, &polled_payload_len) == 0);
}

static void test_max_size_payload_round_trip(void)
{
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_type = 0xFFu;
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_payload_len = 0xFFu;
    uint32_t wire_len;
    uint32_t i;
    int rc;

    uint8_t polled_sender_team_id = 0xFFu;
    uint8_t polled_frame_type = 0xFFu;
    uint8_t polled_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t polled_payload_len = 0xFFu;

    lora_crypto_reset();
    comms_transport_init();

    for (i = 0u; i < sizeof(payload); i++)
    {
        payload[i] = (uint8_t)(i & 0xFFu);
    }

    wire_len = lora_frame_pack(LORA_FRAME_TYPE_CBBA, 2u, payload, (uint8_t)sizeof(payload), wire_buf);
    CHECK(wire_len == LORA_FRAME_MAX_WIRE_SIZE);

    fake_radio_load_rx_packet(wire_buf, wire_len);

    rc = simulate_rx_poll(&out_type, out_payload, &out_payload_len);
    CHECK(rc == 1);

    CHECK(comms_transport_poll_received(&polled_sender_team_id, &polled_frame_type,
                                         polled_payload, &polled_payload_len) == 1);
    CHECK(polled_frame_type == LORA_FRAME_TYPE_CBBA);
    CHECK(polled_payload_len == sizeof(payload));
    CHECK(memcmp(polled_payload, payload, sizeof(payload)) == 0);
}

static void test_corrupted_frame_rejected(void)
{
    uint8_t payload[5] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u };
    uint8_t wire_buf[LORA_FRAME_MAX_WIRE_SIZE];
    uint8_t out_type = 0xFFu;
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_payload_len = 0xFFu;
    uint32_t wire_len;
    int rc;

    uint8_t polled_sender_team_id = 0xFFu;
    uint8_t polled_frame_type = 0xFFu;
    uint8_t polled_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t polled_payload_len = 0xFFu;

    sx126x_irq_mask_t irq_after;

    lora_crypto_reset();
    comms_transport_init();

    wire_len = lora_frame_pack(LORA_FRAME_TYPE_MISSION_LINK, 1u, payload,
                                (uint8_t)sizeof(payload), wire_buf);
    CHECK(wire_len > 0u);

    /* Flip one ciphertext byte, same technique lora_frame_selftest.c's
       own checksum-reject test uses - the first ciphertext byte is
       right after the 3-byte header (frame_type, sender_board_id,
       payload_len). */
    wire_buf[LORA_FRAME_HEADER_SIZE] ^= 0xFFu;

    fake_radio_load_rx_packet(wire_buf, wire_len);

    rc = simulate_rx_poll(&out_type, out_payload, &out_payload_len);
    CHECK(rc == -1);

    CHECK(sx126x_get_irq_status(NULL, &irq_after) == SX126X_STATUS_OK);
    CHECK(irq_after == (sx126x_irq_mask_t)SX126X_IRQ_NONE);

    /* Nothing delivered - the whole point of this test. */
    CHECK(comms_transport_poll_received(&polled_sender_team_id, &polled_frame_type,
                                         polled_payload, &polled_payload_len) == 0);
}

static void test_timeout_only_delivers_nothing(void)
{
    uint8_t out_type = 0xFFu;
    uint8_t out_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t out_payload_len = 0xFFu;
    int rc;

    uint8_t polled_sender_team_id = 0xFFu;
    uint8_t polled_frame_type = 0xFFu;
    uint8_t polled_payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t polled_payload_len = 0xFFu;

    lora_crypto_reset();
    comms_transport_init();

    /* No fake_radio_load_rx_packet() call - RxDone never set, only
       Timeout, mirroring sx1262_poll_rx()'s "TxDone and/or Timeout,
       no RxDone" branch (minus that branch's board-only re-arm call -
       see simulate_rx_poll()'s own doc). */
    g_fake_irq_status = (uint16_t)SX126X_IRQ_TIMEOUT;

    rc = simulate_rx_poll(&out_type, out_payload, &out_payload_len);
    CHECK(rc == 2);

    CHECK(comms_transport_poll_received(&polled_sender_team_id, &polled_frame_type,
                                         polled_payload, &polled_payload_len) == 0);
}

int main(void)
{
    test_happy_path_round_trip();
    test_max_size_payload_round_trip();
    test_corrupted_frame_rejected();
    test_timeout_only_delivers_nothing();

    if (g_failures == 0)
    {
        printf("sx1262_rx_selftest: all tests passed\n");
        return 0;
    }

    printf("sx1262_rx_selftest: %d failure(s)\n", g_failures);
    return 1;
}

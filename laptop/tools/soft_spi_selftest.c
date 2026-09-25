/*
 * soft_spi_selftest.c - checks core/soft_spi.h against a simulated SPI
 * mode-0 slave. Host-only, no hardware. Run via `make check-soft-spi-selftest`.
 *
 * The slave model is deliberately strict about mode 0 (CPOL=0, CPHA=0):
 *   - it samples MOSI only on a rising SCK edge,
 *   - it changes MISO only on a falling SCK edge,
 *   - it flags MOSI changing while SCK is high, and MISO being sampled
 *     while SCK is low (that would read the next bit, not the current one).
 * A deliberately wrong "mutant" transfer is also run, to prove this
 * harness would actually catch a real timing bug rather than pass vacuously.
 */

#include <stdint.h>
#include <stdio.h>

#include "soft_spi.h"

typedef struct
{
    int sck;
    int mosi;
    int miso;
    uint8_t tx;
    uint8_t rx;
    int bit_index;
    int rising_edges;
    int falling_edges;
    int mosi_at_rise;
    int violations;
    int delay_calls;
} slave_t;

static slave_t g_slave;

static void slave_reset(void)
{
    g_slave.sck = 0;
    g_slave.mosi = 0;
    g_slave.miso = 0;
    g_slave.violations = 0;
}

/* NSS just went low: mode 0 means the slave already presents bit 7. */
static void slave_begin_byte(uint8_t tx)
{
    g_slave.tx = tx;
    g_slave.rx = 0u;
    g_slave.bit_index = 0;
    g_slave.rising_edges = 0;
    g_slave.falling_edges = 0;
    g_slave.delay_calls = 0;
    g_slave.miso = (tx >> 7) & 1;
}

static void ops_set_sck(int level)
{
    level = level ? 1 : 0;
    if (level == g_slave.sck)
    {
        return;
    }

    if (level)
    {
        g_slave.rising_edges++;
        g_slave.rx = (uint8_t)((g_slave.rx << 1) | (g_slave.mosi ? 1u : 0u));
        g_slave.mosi_at_rise = g_slave.mosi;
        g_slave.bit_index++;
    }
    else
    {
        g_slave.falling_edges++;
        if (g_slave.mosi != g_slave.mosi_at_rise)
        {
            g_slave.violations++;
        }
        g_slave.miso = (g_slave.bit_index < 8) ? ((g_slave.tx >> (7 - g_slave.bit_index)) & 1) : 0;
    }
    g_slave.sck = level;
}

static void ops_set_mosi(int level)
{
    level = level ? 1 : 0;
    if (g_slave.sck && level != g_slave.mosi)
    {
        g_slave.violations++;
    }
    g_slave.mosi = level;
}

static int ops_get_miso(void)
{
    if (!g_slave.sck)
    {
        g_slave.violations++;
    }
    return g_slave.miso;
}

static void ops_delay(void)
{
    g_slave.delay_calls++;
}

static const soft_spi_ops_t g_ops = { ops_set_sck, ops_set_mosi, ops_get_miso, ops_delay };

/* Wrong on purpose: reads MISO after the falling edge, i.e. one bit late. */
static uint8_t mutant_sample_after_fall(const soft_spi_ops_t *ops, uint8_t out)
{
    uint8_t in = 0u;
    int bit;

    for (bit = 7; bit >= 0; bit--)
    {
        ops->set_mosi((out >> bit) & 1u);
        ops->half_period_delay();
        ops->set_sck(1);
        ops->half_period_delay();
        ops->set_sck(0);
        in = (uint8_t)((in << 1) | (ops->get_miso() ? 1u : 0u));
    }
    return in;
}

/* Every outgoing byte value against four different slave reply patterns.
 * Returns how many of the individual byte checks failed. */
static int exercise(uint8_t (*xfer)(const soft_spi_ops_t *, uint8_t), int verbose, int *checks)
{
    int bad = 0;
    unsigned pat;
    unsigned out;

    for (pat = 0u; pat < 4u; pat++)
    {
        for (out = 0u; out < 256u; out++)
        {
            uint8_t tx = (pat == 0u) ? (uint8_t)~out
                       : (pat == 1u) ? (uint8_t)(out ^ 0xA5u)
                       : (pat == 2u) ? 0x00u
                                     : 0xFFu;
            uint8_t in;
            int ok = 1;

            slave_reset();
            slave_begin_byte(tx);
            in = xfer(&g_ops, (uint8_t)out);
            (*checks)++;

            if (in != tx)
            {
                ok = 0;
                if (verbose) printf("  out=0x%02X pat=%u: master read 0x%02X, slave sent 0x%02X\n", out, pat, in, tx);
            }
            if (g_slave.rx != (uint8_t)out)
            {
                ok = 0;
                if (verbose) printf("  out=0x%02X pat=%u: slave received 0x%02X\n", out, pat, g_slave.rx);
            }
            if (g_slave.rising_edges != 8 || g_slave.falling_edges != 8)
            {
                ok = 0;
                if (verbose) printf("  out=0x%02X: %d rising / %d falling edges (want 8/8)\n", out, g_slave.rising_edges, g_slave.falling_edges);
            }
            if (g_slave.sck != 0)
            {
                ok = 0;
                if (verbose) printf("  out=0x%02X: SCK left high\n", out);
            }
            if (g_slave.violations != 0)
            {
                ok = 0;
                if (verbose) printf("  out=0x%02X: %d mode-0 timing violation(s)\n", out, g_slave.violations);
            }
            if (g_slave.delay_calls < 16)
            {
                ok = 0;
                if (verbose) printf("  out=0x%02X: only %d delay calls (want >= 16, two per bit)\n", out, g_slave.delay_calls);
            }
            if (!ok)
            {
                bad++;
            }
        }
    }
    return bad;
}

/* An SX126x-shaped exchange: several bytes back to back under one "NSS
 * low", each byte independent of the last. */
static int transaction_test(int *checks)
{
    static const uint8_t cmd[]   = { 0x1Du, 0x07u, 0x40u, 0x00u, 0x00u, 0xFFu, 0x80u };
    static const uint8_t reply[] = { 0xA0u, 0xA1u, 0xA2u, 0x5Au, 0xC3u, 0x00u, 0x7Eu };
    unsigned i;
    int bad = 0;

    slave_reset();
    for (i = 0u; i < sizeof(cmd); i++)
    {
        uint8_t in;

        slave_begin_byte(reply[i]);
        in = soft_spi_xfer_byte(&g_ops, cmd[i]);
        (*checks)++;
        if (in != reply[i] || g_slave.rx != cmd[i] || g_slave.sck != 0 || g_slave.violations != 0)
        {
            bad++;
            printf("  transaction byte %u: master read 0x%02X (want 0x%02X), slave got 0x%02X (want 0x%02X)\n",
                   i, in, reply[i], g_slave.rx, cmd[i]);
        }
    }
    return bad;
}

int main(void)
{
    int checks = 0;
    int bad;
    int mutant_checks = 0;
    int mutant_bad;

    bad = exercise(soft_spi_xfer_byte, 1, &checks);
    bad += transaction_test(&checks);

    /* The harness must reject the wrong implementation, or it proves nothing. */
    mutant_bad = exercise(mutant_sample_after_fall, 0, &mutant_checks);
    if (mutant_bad == 0)
    {
        printf("FAIL: harness did not detect the late-MISO-sample mutant - test is too weak\n");
        bad++;
    }

    if (bad != 0)
    {
        printf("soft_spi_selftest: FAILED (%d failing check(s) of %d)\n", bad, checks);
        return 1;
    }

    printf("soft_spi_selftest: OK (%d byte checks passed; harness rejected the late-sample mutant %d/%d times)\n",
           checks, mutant_bad, mutant_checks);
    return 0;
}

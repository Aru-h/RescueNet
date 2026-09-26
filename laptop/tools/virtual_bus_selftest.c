/* Selftest for core/vbus.c. Build: make check-virtual-bus */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "random.h"
#include "vbus.h"

#if !VIRTUAL_BUS_ENABLED
#error "virtual_bus_selftest must be built with -DVIRTUAL_BUS_ENABLED=1"
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_NEAR(v, want, tol) do { double v_ = (v); if (fabs(v_ - (want)) > (tol)) { \
    printf("FAIL %s:%d: %s = %.4f, want %.4f +/- %.4f\n", __FILE__, __LINE__, #v, v_, (double)(want), (double)(tol)); g_fail++; } } while (0)

static VbusConfig_t cfg_of(VbusLossModel_t m, float loss, uint32_t seed)
{
    VbusConfig_t c;
    memset(&c, 0, sizeof c);
    c.loss_model = m;
    c.link_loss = loss;
    c.seed = seed;
    return c;
}

static int drain(uint8_t node)
{
    uint8_t s, t, l, p[LORA_FRAME_MAX_PAYLOAD];
    int n = 0;
    while (vbus_poll(node, &s, &t, p, &l)) n++;
    return n;
}

static void test_airtime_formula(void)
{
    uint32_t w;
    CHECK(lora_time_on_air_ms_x1000(80u) == 143616u);
    CHECK(lora_time_on_air_ms_x1000(172u) == 276736u);   /* comms_transport.c's derivation */
    for (w = 0u; w <= 172u; w++) {                       /* double-precision reference */
        double tsym = 128.0 / 125000.0;
        double n = ceil((8.0 * w - 28.0 + 28.0 + 16.0) / 28.0);
        double toa_ms = (12.25 + 8.0 + n * 5.0) * tsym * 1000.0;
        if (fabs(toa_ms * 1000.0 - (double)lora_time_on_air_ms_x1000(w)) > 0.5) {
            printf("FAIL airtime mismatch at %u\n", w);
            g_fail++;
            break;
        }
    }
}

static void test_unicast_broadcast_delay(void)
{
    uint8_t pl[4] = {1, 2, 3, 4}, s, t, l, out[LORA_FRAME_MAX_PAYLOAD];
    VbusConfig_t c = cfg_of(VBUS_LOSS_IID, 0.0f, 1u);
    VbusStats_t st;

    vbus_init(4, &c);
    CHECK(vbus_send(0, 2, 7, pl, 4) == 0);
    vbus_step(0);
    CHECK(vbus_poll(1, &s, &t, out, &l) == 0);
    CHECK(vbus_poll(3, &s, &t, out, &l) == 0);
    CHECK(vbus_poll(0, &s, &t, out, &l) == 0);
    CHECK(vbus_poll(2, &s, &t, out, &l) == 1);
    CHECK(s == 0 && t == 7 && l == 4 && memcmp(out, pl, 4) == 0);
    CHECK(vbus_poll(2, &s, &t, out, &l) == 0);

    /* broadcast: everyone but the sender, in per-source order */
    CHECK(vbus_send(1, VBUS_BROADCAST, 9, pl, 2) == 0);
    vbus_step(1);
    CHECK(drain(0) == 1 && drain(1) == 0 && drain(2) == 1 && drain(3) == 1);
    st = vbus_get_stats();
    CHECK(st.sent == 2 && st.delivered == 4 && st.dropped == 0);

    /* invalid sends */
    CHECK(vbus_send(9, 0, 0, pl, 1) == -1);
    CHECK(vbus_send(0, 9, 0, pl, 1) == -1);
    CHECK(vbus_send(0, 0, 0, pl, 1) == -1);
    CHECK(vbus_send(0, 1, 0, pl, LORA_FRAME_MAX_PAYLOAD + 1u) == -1);
    CHECK(vbus_send(0, 1, 0, NULL, 3) == -1);
    CHECK(vbus_send(0, 1, 0, NULL, 0) == 0);      /* empty payload is fine */
    vbus_step(2);
    CHECK(vbus_poll(1, &s, &t, out, &l) == 1 && l == 0);

    /* delay 1: arrives one step later, not this one */
    c.delay_ticks = 1;
    vbus_init(4, &c);
    CHECK(vbus_send(0, 1, 5, pl, 4) == 0);
    vbus_step(0);
    CHECK(drain(1) == 0);
    vbus_step(1);
    CHECK(drain(1) == 1);
    /* a frame sent after step 0 still lands within one further step of its own send */
    CHECK(vbus_send(0, 1, 5, pl, 4) == 0);
    vbus_step(2);
    CHECK(drain(1) == 0);
    vbus_step(3);
    CHECK(drain(1) == 1);

    /* delay > 1 is clamped to 1 */
    c.delay_ticks = 9;
    vbus_init(2, &c);
    vbus_send(0, 1, 0, pl, 1);
    vbus_step(0); vbus_step(1);
    CHECK(drain(1) == 1);

    /* delivery order: by src id regardless of send order */
    c.delay_ticks = 0;
    vbus_init(4, &c);
    vbus_send(3, 0, 0, pl, 1);
    vbus_send(1, 0, 0, pl, 1);
    vbus_send(2, 0, 0, pl, 1);
    vbus_step(0);
    CHECK(vbus_poll(0, &s, &t, out, &l) && s == 1);
    CHECK(vbus_poll(0, &s, &t, out, &l) && s == 2);
    CHECK(vbus_poll(0, &s, &t, out, &l) && s == 3);
}

#define RATE_STEPS 20000

/* Fraction of copies lost for a 5-node all-broadcast workload. */
static double run_rate(const VbusConfig_t *c, uint32_t *all_or_none_steps_src0)
{
    uint32_t k, steps_all_or_none = 0;
    uint8_t n;
    VbusStats_t st;
    vbus_init(5, c);
    for (k = 0; k < RATE_STEPS; k++) {
        int got;
        for (n = 0; n < 5; n++) vbus_send(n, VBUS_BROADCAST, 0, (const uint8_t *)"x", 1);
        vbus_step(k);
        /* copies of src 0's frame that reached the 4 receivers */
        got = 0;
        drain(0);
        for (n = 1; n < 5; n++) {
            uint8_t s, t, l, p[8];
            while (vbus_poll(n, &s, &t, p, &l))
                if (s == 0) got++;
        }
        if (got == 0 || got == 4) steps_all_or_none++;
    }
    st = vbus_get_stats();
    if (all_or_none_steps_src0) *all_or_none_steps_src0 = steps_all_or_none;
    return (double)st.dropped / (double)(st.dropped + st.delivered);
}

static void test_loss_iid(void)
{
    uint32_t aon;
    VbusConfig_t c = cfg_of(VBUS_LOSS_IID, 0.3f, 42u);
    CHECK_NEAR(run_rate(&c, &aon), 0.3, 0.01);
    /* independent per copy: all-or-none for 4 receivers ~ 0.7^4 + 0.3^4 = 0.2482 */
    CHECK_NEAR((double)aon / RATE_STEPS, 0.2482, 0.015);
    c.link_loss = 0.0f;
    CHECK(run_rate(&c, NULL) == 0.0);
    c.link_loss = 1.0f;
    CHECK(run_rate(&c, NULL) == 1.0);
}

static void test_loss_per_source(void)
{
    uint32_t aon;
    VbusConfig_t c = cfg_of(VBUS_LOSS_PER_SOURCE, 0.3f, 43u);
    CHECK_NEAR(run_rate(&c, &aon), 0.3, 0.01);
    CHECK(aon == RATE_STEPS);   /* one draw per sender: all receivers agree, every step */
}

static void test_loss_gilbert_elliott(void)
{
    /* stationary P(bad) = p_gb/(p_gb+p_bg) = 0.2; loss = 0.2*0.9 + 0.8*0.02 = 0.196 */
    VbusConfig_t c = cfg_of(VBUS_LOSS_GILBERT_ELLIOTT, 0.02f, 44u);
    uint32_t k, runs = 0, lost_total = 0, cur = 0, longest = 0;
    c.p_gb = 0.05f; c.p_bg = 0.20f; c.loss_bad = 0.9f;
    CHECK_NEAR(run_rate(&c, NULL), 0.196, 0.012);

    /* bursty: on a single link with loss_bad = 1, loss runs average ~ 1/p_bg = 5 */
    c.link_loss = 0.0f; c.loss_bad = 1.0f;
    vbus_init(2, &c);
    for (k = 0; k < 40000; k++) {
        uint8_t s, t, l, p[4];
        int got;
        vbus_send(0, 1, 0, (const uint8_t *)"x", 1);
        vbus_step(k);
        got = vbus_poll(1, &s, &t, p, &l);
        if (!got) { lost_total++; cur++; if (cur > longest) longest = cur; }
        else if (cur) { runs++; cur = 0; }
    }
    CHECK(runs > 0);
    CHECK_NEAR((double)lost_total / runs, 5.0, 0.6);
    CHECK(longest >= 15);       /* i.i.d. loss at the same rate (~20%) would almost never do this */
}

static void test_loss_shared_burst(void)
{
    VbusConfig_t c = cfg_of(VBUS_LOSS_SHARED_BURST, 0.0f, 45u);
    uint32_t k, burst_steps = 0, mixed = 0;
    c.p_gb = 0.05f; c.p_bg = 0.20f;     /* P(burst) = 0.2 */
    vbus_init(5, &c);
    for (k = 0; k < RATE_STEPS; k++) {
        int got = 0;
        uint8_t n, m;
        for (n = 0; n < 5; n++) vbus_send(n, VBUS_BROADCAST, 0, (const uint8_t *)"x", 1);
        vbus_step(k);
        for (m = 0; m < 5; m++) got += drain(m);
        if (got == 0) burst_steps++;
        else if (got != 20) mixed++;
    }
    CHECK(mixed == 0);                  /* whole channel: every link, every sender, or none */
    CHECK_NEAR((double)burst_steps / RATE_STEPS, 0.2, 0.03);
}

static void test_rx_loss_and_dark(void)
{
    uint8_t s, t, l, p[8];
    uint32_t k;
    uint32_t got[3] = {0, 0, 0};
    VbusConfig_t c = cfg_of(VBUS_LOSS_IID, 0.0f, 46u);
    VbusStats_t st;

    /* extra rx loss on node 1 only */
    vbus_init(3, &c);
    vbus_set_node_rx_loss(1, 0.4f);
    for (k = 0; k < RATE_STEPS; k++) {
        vbus_send(0, VBUS_BROADCAST, 0, (const uint8_t *)"x", 1);
        vbus_step(k);
        got[1] += (uint32_t)drain(1);
        got[2] += (uint32_t)drain(2);
    }
    CHECK_NEAR(1.0 - (double)got[1] / RATE_STEPS, 0.4, 0.015);
    CHECK(got[2] == RATE_STEPS);

    /* dark: no tx and no rx */
    vbus_init(3, &c);
    vbus_set_node_dark(1, 1);
    CHECK(vbus_send(1, VBUS_BROADCAST, 0, (const uint8_t *)"x", 1) == -1);
    CHECK(vbus_get_stats().sent == 0 && vbus_get_stats().airtime_ms_total == 0);
    vbus_send(0, VBUS_BROADCAST, 0, (const uint8_t *)"x", 1);
    vbus_send(0, 1, 0, (const uint8_t *)"x", 1);
    vbus_step(0);
    CHECK(drain(1) == 0);
    CHECK(drain(2) == 1);
    st = vbus_get_stats();
    CHECK(st.sent == 2 && st.delivered == 1 && st.dropped == 2);
    vbus_set_node_dark(1, 0);
    vbus_send(1, 0, 0, (const uint8_t *)"x", 1);
    vbus_send(0, 1, 0, (const uint8_t *)"x", 1);
    vbus_step(1);
    CHECK(vbus_poll(0, &s, &t, p, &l) == 1 && s == 1);
    CHECK(drain(1) == 1);
}

static void test_overflow(void)
{
    uint32_t k, seen = 0;
    uint8_t s, t, l, p[8];
    VbusConfig_t c = cfg_of(VBUS_LOSS_IID, 0.0f, 47u);
    VbusStats_t st;
    vbus_init(2, &c);
    /* fill the inbox with tagged frames across several steps, never polling */
    for (k = 0; k < VBUS_INBOX_DEPTH + 5u; k++) {
        uint8_t tag = (uint8_t)k;
        vbus_send(0, 1, tag, &tag, 1);
        vbus_step(k);
    }
    st = vbus_get_stats();
    CHECK(st.delivered == VBUS_INBOX_DEPTH);
    CHECK(st.dropped == 5);
    /* oldest kept, newest dropped, FIFO order preserved */
    while (vbus_poll(1, &s, &t, p, &l)) { CHECK(t == seen && p[0] == seen); seen++; }
    CHECK(seen == VBUS_INBOX_DEPTH);
    /* draining frees space again */
    vbus_send(0, 1, 99, p, 1);
    vbus_step(100);
    CHECK(vbus_poll(1, &s, &t, p, &l) == 1 && t == 99);

    /* in-flight queue overflow: sends rejected, counted, no airtime charged */
    vbus_init(2, &c);
    for (k = 0; k < VBUS_PENDING_MAX; k++) CHECK(vbus_send(0, 1, 0, p, 1) == 0);
    {
        uint32_t air = vbus_get_stats().airtime_ms_total;
        CHECK(vbus_send(0, 1, 0, p, 1) == -1);
        st = vbus_get_stats();
        CHECK(st.sent == VBUS_PENDING_MAX && st.dropped == 1 && st.airtime_ms_total == air);
    }
}

static uint32_t trace_hash(VbusLossModel_t m, uint32_t seed)
{
    VbusConfig_t c = cfg_of(m, 0.25f, seed);
    uint32_t h = 2166136261u, k;
    uint8_t n;
    c.p_gb = 0.1f; c.p_bg = 0.3f; c.loss_bad = 0.8f; c.delay_ticks = 1;
    vbus_init(6, &c);
    vbus_set_node_rx_loss(2, 0.2f);
    for (k = 0; k < 2000; k++) {
        for (n = 0; n < 6; n++) {
            uint8_t pl[3] = {(uint8_t)k, n, (uint8_t)(k >> 8)};
            vbus_send(n, (k % 3) ? VBUS_BROADCAST : (uint8_t)((n + 1) % 6), n, pl, 3);
        }
        vbus_step(k);
        for (n = 0; n < 6; n++) {
            uint8_t s, t, l, p[LORA_FRAME_MAX_PAYLOAD];
            while (vbus_poll(n, &s, &t, p, &l)) {
                h = (h ^ n) * 16777619u; h = (h ^ s) * 16777619u; h = (h ^ p[0]) * 16777619u; h = (h ^ k) * 16777619u;
            }
        }
    }
    return h;
}

static void test_determinism(void)
{
    VbusLossModel_t m;
    for (m = VBUS_LOSS_IID; m <= VBUS_LOSS_SHARED_BURST; m++) {
        uint32_t a = trace_hash(m, 1234u), b = trace_hash(m, 1234u), d = trace_hash(m, 1235u);
        CHECK(a == b);
        CHECK(a != d);
    }
}

static void test_airtime_accounting(void)
{
    uint8_t buf[LORA_FRAME_MAX_PAYLOAD] = {0};
    VbusConfig_t c = cfg_of(VBUS_LOSS_IID, 1.0f, 48u);   /* everything lost: airtime still charged */
    uint32_t us = 0, k;
    uint8_t len = 80u - (uint8_t)(LORA_FRAME_HEADER_SIZE + LORA_FRAME_TRAILER_SIZE);
    vbus_init(10, &c);
    CHECK(vbus_send(0, VBUS_BROADCAST, 0, buf, len) == 0);
    CHECK(vbus_get_stats().airtime_ms_total == 143u);   /* 143.616 ms, truncated */
    vbus_init(10, &c);
    for (k = 0; k < 10; k++) vbus_send((uint8_t)k, VBUS_BROADCAST, 0, buf, len);   /* one tick, 10 beacons */
    CHECK(vbus_get_stats().airtime_ms_total == 1436u);  /* design section 1.3: 1 436 ms */
    /* sums exact microseconds, not per-frame truncated ms */
    vbus_init(4, &c);
    for (k = 0; k < 50; k++) {
        uint8_t n = (uint8_t)(k * 3u % (LORA_FRAME_MAX_PAYLOAD + 1u));
        vbus_send(1, 2, 0, buf, n);
        us += lora_time_on_air_ms_x1000((uint32_t)n + LORA_FRAME_HEADER_SIZE + LORA_FRAME_TRAILER_SIZE);
    }
    CHECK(vbus_get_stats().airtime_ms_total == us / 1000u);
}

static void test_random_stream_untouched(void)
{
    uint32_t a[64], b[64], i, k;
    random_seed_all(777u);
    for (i = 0; i < 64; i++) a[i] = random_u32() ^ random_u32_stream(STREAM_FAULTS) ^ random_u32_stream(STREAM_CBBA_CONSENSUS);

    random_seed_all(777u);
    {
        VbusConfig_t c = cfg_of(VBUS_LOSS_GILBERT_ELLIOTT, 0.3f, 777u);
        c.p_gb = 0.2f; c.p_bg = 0.2f; c.loss_bad = 0.7f;
        vbus_init(8, &c);
        for (k = 0; k < 3000; k++) {
            uint8_t n;
            for (n = 0; n < 8; n++) vbus_send(n, VBUS_BROADCAST, 0, (const uint8_t *)"x", 1);
            vbus_step(k);
            for (n = 0; n < 8; n++) drain(n);
        }
        CHECK(vbus_get_stats().dropped > 0);   /* the bus really did draw randomness */
    }
    for (i = 0; i < 64; i++) b[i] = random_u32() ^ random_u32_stream(STREAM_FAULTS) ^ random_u32_stream(STREAM_CBBA_CONSENSUS);
    CHECK(memcmp(a, b, sizeof a) == 0);
}

int main(void)
{
    test_airtime_formula();
    test_unicast_broadcast_delay();
    test_loss_iid();
    test_loss_per_source();
    test_loss_gilbert_elliott();
    test_loss_shared_burst();
    test_rx_loss_and_dark();
    test_overflow();
    test_determinism();
    test_airtime_accounting();
    test_random_stream_untouched();
    if (g_fail) { printf("virtual_bus_selftest: %d FAILED\n", g_fail); return 1; }
    printf("virtual_bus_selftest: all checks passed\n");
    return 0;
}

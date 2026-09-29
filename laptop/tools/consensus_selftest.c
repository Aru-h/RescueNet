/* Unit tests for core/consensus.c (docs/architecture/CONSENSUS_DESIGN.md 7.9 and 7.11).
 * Build: make check-consensus-selftest
 *
 * No simulation runs here. The virtual bus is the real one (core/vbus.c);
 * the teams are synthetic: each test writes g_teams[] and decides which
 * nodes "transmit" this tick, then calls consensus_step(). team_radio_emitted()
 * is the only team.c symbol consensus needs, so it is copied below instead of
 * linking the whole simulator. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "consensus.h"
#include "mission_state.h"
#include "recovery_manager_config.h"
#include "team.h"
#include "vbus.h"
#include "watchdog.h"

#if !VIRTUAL_BUS_ENABLED || !CONSENSUS_TEST_HOOKS
#error "consensus_selftest needs -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_TEST_HOOKS=1"
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_NEAR(v, want, tol) do { double v_ = (v); if (fabs(v_ - (want)) > (tol)) { \
    printf("FAIL %s:%d: %s = %.5f, want %.5f +/- %.5f\n", __FILE__, __LINE__, #v, v_, (double)(want), (double)(tol)); g_fail++; } } while (0)

/* Same predicate as core/team.c. */
uint8_t team_radio_emitted(const TeamState_t *team)
{
    return team->last_heartbeat_tick == g_current_tick;
}

/* ---- synthetic world ---- */

static uint8_t g_emit[NUM_TEAMS];        /* does node n transmit this tick */
static uint32_t g_tick;

static void syn_reset(float link_loss, VbusLossModel_t model)
{
    ConsensusBusConfig_t cfg;

    memset(g_teams, 0, sizeof(g_teams));
    for(uint8_t n = 0; n < NUM_TEAMS; n++)
    {
        g_teams[n].team_id = n;
        g_teams[n].status = STATUS_AVAILABLE;
        g_teams[n].assigned_task_pool = TASK_POOL_NONE;
        g_teams[n].assigned_task_slot = NO_ASSIGNMENT;
        g_teams[n].loc_x = (int16_t)(2 + n);
        g_teams[n].loc_y = 5;
        g_emit[n] = 1u;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.loss_model = (uint8_t)model;
    cfg.link_loss = link_loss;
    consensus_configure_bus(&cfg);
    consensus_init(1234u);
    g_tick = 0u;
}

static void syn_tick(void)
{
    g_tick++;
    g_current_tick = g_tick;
    for(uint8_t n = 0; n < NUM_TEAMS; n++)
        g_teams[n].last_heartbeat_tick = g_emit[n] ? g_tick : g_tick - 1u;
    consensus_step(g_tick);
}

static void syn_ticks(uint32_t n)
{
    while(n--)
        syn_tick();
}

static ConsensusResult_t res(uint8_t t, ConsensusClass_t c)
{
    return consensus_get_result(t, c);
}

/* ---- pure helpers ---- */

static void test_beacon_roundtrip(void)
{
    ConsensusBeacon_t a, b;
    uint8_t buf[CONSENSUS_BEACON_SIZE];

    CHECK(CONSENSUS_BEACON_SIZE == 52u);
    memset(&a, 0, sizeof(a));
    a.version = CONSENSUS_BEACON_VERSION;
    a.team_id = 7u;
    a.seq = 0xBEEFu;
    a.status = STATUS_EN_ROUTE;
    a.loc_x = 35u;
    a.loc_y = 1u;
    a.task_pool = 1u;
    a.task_slot = 0xFFu;
    a.leg_flags = 0x05u;
    a.heard_bits = 0x2A5Au;
    for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
    {
        for(uint8_t j = 0; j < NUM_TEAMS; j++)
        {
            a.votes[c][j].verdict = (uint8_t)((c + j) % 3u);
            a.votes[c][j].n_samples = (uint8_t)(j * 6u + c);
            a.votes[c][j].lo_q = (uint8_t)((j + 3u) & 15u);
            a.votes[c][j].hi_q = (uint8_t)((j * 5u + c) & 15u);
        }
    }

    CHECK(consensus_beacon_pack(&a, buf) == CONSENSUS_BEACON_SIZE);
    CHECK(buf[2] == 0xEFu && buf[3] == 0xBEu);              /* little-endian seq */
    CHECK(buf[10] == 0x5Au && buf[11] == 0x2Au);            /* heard_bits */
    CHECK(consensus_beacon_unpack(buf, CONSENSUS_BEACON_SIZE, 7u, &b) == CONSENSUS_UNPACK_OK);
    CHECK(memcmp(&a, &b, sizeof(a)) == 0);

    /* Drops. */
    CHECK(consensus_beacon_unpack(buf, CONSENSUS_BEACON_SIZE, 6u, &b) == CONSENSUS_UNPACK_BAD_SOURCE);
    CHECK(consensus_beacon_unpack(buf, CONSENSUS_BEACON_SIZE - 1u, 7u, &b) == CONSENSUS_UNPACK_BAD_LENGTH);
    buf[0] = 2u;
    CHECK(consensus_beacon_unpack(buf, CONSENSUS_BEACON_SIZE, 7u, &b) == CONSENSUS_UNPACK_BAD_VERSION);
    buf[0] = CONSENSUS_BEACON_VERSION;
    buf[12] = (uint8_t)(buf[12] | 0xC0u);                   /* reserved verdict 3 */
    CHECK(consensus_beacon_unpack(buf, CONSENSUS_BEACON_SIZE, 7u, &b) == CONSENSUS_UNPACK_BAD_VERDICT);
}

static void test_quantization(void)
{
    for(int i = 0; i <= 1000; i++)
    {
        float v = (float)i / 1000.0f;
        uint8_t lq = consensus_quantize_lo(v);
        uint8_t hq = consensus_quantize_hi(v);

        CHECK(lq <= 15u && hq <= 15u);
        CHECK((float)lq <= v * 15.0f + 1e-6f);      /* lo only ever rounds down */
        CHECK((float)hq >= v * 15.0f - 1e-6f);      /* hi only ever rounds up */
        CHECK(hq - lq <= 1u);
    }
    CHECK(consensus_quantize_lo(1.0f) == 15u && consensus_quantize_hi(1.0f) == 15u);
    CHECK(consensus_quantize_lo(0.0f) == 0u && consensus_quantize_hi(0.0f) == 0u);
}

static void test_wilson(void)
{
    const double z = 1.645;

    for(uint8_t n = 1; n <= 6; n++)
    {
        for(uint8_t k = 0; k <= n; k++)
        {
            double p = (double)k / n;
            double d = 1.0 + z * z / n;
            double c = (p + z * z / (2.0 * n)) / d;
            double h = z * sqrt(p * (1.0 - p) / n + z * z / (4.0 * n * n)) / d;
            double lo = c - h, hi = c + h;
            float flo, fhi;

            if(lo < 0.0 || k == 0) lo = 0.0;
            if(hi > 1.0 || k == n) hi = 1.0;
            consensus_wilson(n, k, &flo, &fhi);
            CHECK_NEAR(flo, lo, 1e-4);
            CHECK_NEAR(fhi, hi, 1e-4);
        }
    }
    {
        float lo, hi;

        consensus_wilson(0, 0, &lo, &hi);
        CHECK(lo == 0.0f && hi == 1.0f);
        /* The worked examples in design 2.3. */
        consensus_severity_interval(4, 2, RECOVERY_MANAGER_MOBILITY_STALL_FULL_SCALE, &lo, &hi);
        CHECK_NEAR(lo, 0.365, 0.005);
        CHECK_NEAR(hi, 1.0, 1e-6);
        consensus_severity_interval(6, 4, CONSENSUS_COMM_MISS_FULL_SCALE, &lo, &hi);
        CHECK_NEAR(lo, 0.520, 0.005);
    }
}

static void test_fti(void)
{
    float lo[NUM_TEAMS] = {0.9f, 0.2f, 0.7f, 0.95f, 0.1f, 0.6f, 0.8f, 0.3f, 0.5f};

    CHECK(consensus_fti_lower_bound(lo, 9, CONSENSUS_F) == 0.8f);   /* 3rd largest */
    CHECK(consensus_fti_lower_bound(lo, 2, CONSENSUS_F) == 0.0f);   /* fewer than f+1 */
    CHECK(consensus_fti_lower_bound(lo, 3, CONSENSUS_F) == 0.2f);
    CHECK(consensus_fti_lower_bound(lo, 0, CONSENSUS_F) == 0.0f);

    /* Lipschitz: changing one input by d moves y by at most d. */
    for(uint8_t i = 0; i < 9; i++)
    {
        float base = consensus_fti_lower_bound(lo, 9, CONSENSUS_F);
        float saved = lo[i];

        for(int step = 0; step <= 20; step++)
        {
            float v = (float)step / 20.0f;
            float y;

            lo[i] = v;
            y = consensus_fti_lower_bound(lo, 9, CONSENSUS_F);
            CHECK(fabsf(y - base) <= fabsf(v - saved) + 1e-6f);
        }
        lo[i] = saved;
    }
}

static void test_gate_truth_table(void)
{
    CHECK(CONSENSUS_F == 2u && CONSENSUS_X_MIN == 3u);

    for(uint8_t live = 0; live <= NUM_TEAMS - 1u; live++)
    {
        for(uint8_t nf = 0; nf <= live; nf++)
        {
            for(uint8_t nc = 0; nf + nc <= live; nc++)
            {
                uint8_t verdict[NUM_TEAMS], domain[NUM_TEAMS];
                float lo[NUM_TEAMS];
                ConsensusResult_t r;
                uint8_t i = 0u;
                uint8_t want;

                for(uint8_t a = 0; a < nf; a++, i++) { verdict[i] = PEER_VERDICT_FAULT; lo[i] = 1.0f; domain[i] = i; }
                for(uint8_t a = 0; a < nc; a++, i++) { verdict[i] = PEER_VERDICT_CLEAR; lo[i] = 0.0f; domain[i] = i; }
                for(; i < live; i++) { verdict[i] = PEER_VERDICT_ABSTAIN; lo[i] = 0.0f; domain[i] = i; }

                r = consensus_gate_compute(live, verdict, lo, domain, live, 0u);
                want = (live >= 3u && nf >= 3u && nf >= 2u * nc + 1u);
                CHECK((r.tier == CONSENSUS_TIER_ELEVATED) == want);
                CHECK(r.n_fault == nf && r.n_clear == nc && r.n_abstain == live - nf - nc);
                CHECK(r.n_dark == (NUM_TEAMS - 1u) - live);
                /* Two voters never elevate, however many peers are dark. */
                if(nf <= 2u)
                    CHECK(r.tier != CONSENSUS_TIER_ELEVATED);
            }
        }
    }

    /* Fast path forces ELEVATED regardless of votes. */
    {
        ConsensusResult_t r = consensus_gate_compute(0, NULL, NULL, NULL, 0, 1u);

        CHECK(r.tier == CONSENSUS_TIER_ELEVATED && r.fast_path == 1u);
    }

    /* Uncorrelated disagreement: 3 FAULT against 1 CLEAR is 3 >= 2*1+1. 3
       against 2 is not. */
    {
        uint8_t v1[5] = {2, 2, 2, 1, 0}, v2[5] = {2, 2, 2, 1, 1}, d[5] = {0, 1, 2, 3, 4};
        float lo[5] = {1, 1, 1, 0, 0};

        CHECK(consensus_gate_compute(5, v1, lo, d, 5, 0).tier == CONSENSUS_TIER_ELEVATED);
        CHECK(consensus_gate_compute(5, v2, lo, d, 5, 0).tier != CONSENSUS_TIER_ELEVATED);
    }

    /* Several nodes in one failure domain count once: 4 FAULT voters in two
       domains is x = 2, which never elevates. */
    {
        uint8_t v[4] = {2, 2, 2, 2}, d[4] = {0, 0, 1, 1};
        float lo[4] = {1, 1, 1, 1};

        CHECK(consensus_gate_compute(4, v, lo, d, 4, 0).x == 2u);
        CHECK(consensus_gate_compute(4, v, lo, d, 4, 0).tier != CONSENSUS_TIER_ELEVATED);
    }

    /* LOCAL: at least one FAULT and y at or above the threshold, no quorum. */
    {
        uint8_t v[3] = {2, 2, 2}, d[3] = {0, 1, 2};
        float lo[3] = {1, 1, 1};

        CHECK(consensus_gate_compute(2, v, lo, d, 2, 0).tier == CONSENSUS_TIER_NONE);   /* y = 0: < f+1 opinions */
        CHECK(consensus_gate_compute(3, v, lo, d, 3, 0).tier == CONSENSUS_TIER_ELEVATED);
    }
}

/* ---- synthetic scenarios ---- */

static void test_healthy_zero_votes(void)
{
    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(80);
    for(uint8_t o = 0; o < NUM_TEAMS; o++)
    {
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            PeerObservation_t p;

            if(o == t) continue;
            consensus_get_peer_observation(o, t, &p);
            CHECK(p.vote[CONSENSUS_CLASS_COMMUNICATION].verdict != PEER_VERDICT_FAULT);
            CHECK(p.vote[CONSENSUS_CLASS_MOBILITY].verdict != PEER_VERDICT_FAULT);
            CHECK(p.comm_misses == 0u && p.plaus_fail == 0u);
        }
    }
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        CHECK(res(t, CONSENSUS_CLASS_COMMUNICATION).tier == CONSENSUS_TIER_NONE);
        CHECK(res(t, CONSENSUS_CLASS_MOBILITY).tier == CONSENSUS_TIER_NONE);
        CHECK(res(t, CONSENSUS_CLASS_COMMUNICATION).n_live == NUM_TEAMS - 1u);
        /* Enough full windows for everyone to vote CLEAR. */
        CHECK(res(t, CONSENSUS_CLASS_COMMUNICATION).n_clear == NUM_TEAMS - 1u);
    }
    CHECK(vbus_get_stats().dropped == 0u);                  /* no inbox overflow */
    CHECK(vbus_get_stats().sent == 80u * NUM_TEAMS);
}

/* A comm-faulted node: silent, then a sustained intermittent emitter. */
static void test_fast_path(void)
{
    ConsensusResult_t r;

    /* Two consecutive silent ticks (k = 41, 42). Evidence about tick k is
       delivered with the beacons of tick k+1, seen one tick later. The
       column for 42 is complete at tick 44, so the fast path fires at 44. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(40);
    g_emit[3] = 0u;
    syn_tick();                                             /* 41 */
    syn_tick();                                             /* 42 */
    g_emit[3] = 1u;
    syn_tick();                                             /* 43 */
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).tier != CONSENSUS_TIER_ELEVATED);
    syn_tick();                                             /* 44 */
    r = res(3, CONSENSUS_CLASS_COMMUNICATION);
    CHECK(r.tier == CONSENSUS_TIER_ELEVATED && r.fast_path == 1u);
    CHECK(r.n_reporters == NUM_TEAMS - 1u);
    /* Held through the end of the next round: round 22 is ticks 44 and 45,
       so it holds through 47, and is gone at 48. */
    syn_ticks(3);                                           /* 47 */
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).tier == CONSENSUS_TIER_ELEVATED);
    syn_tick();                                             /* 48 */
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).tier != CONSENSUS_TIER_ELEVATED);
    CHECK(res(3, CONSENSUS_CLASS_MOBILITY).tier == CONSENSUS_TIER_NONE);
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
        if(t != 3) CHECK(res(t, CONSENSUS_CLASS_COMMUNICATION).tier != CONSENSUS_TIER_ELEVATED);

    /* A single silent tick is not enough. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(40);
    g_emit[3] = 0u;
    syn_tick();
    g_emit[3] = 1u;
    syn_ticks(8);
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).tier != CONSENSUS_TIER_ELEVATED);
    CHECK(consensus_get_stats().fast_path_fires == 0u);
}

static void test_fast_path_reporters(void)
{
    /* Two reporters that always claim "missed" about a healthy target 5
       cannot fire the rule: the other honest reporters heard it. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    consensus_test_force_heard_bit(1, 5, 0);
    consensus_test_force_heard_bit(2, 5, 0);
    syn_ticks(60);
    CHECK(res(5, CONSENSUS_CLASS_COMMUNICATION).tier != CONSENSUS_TIER_ELEVATED);
    CHECK(consensus_get_stats().fast_path_fires == 0u);

    /* One reporter that always claims "heard" about a silent target blocks
       the fast path... */
    syn_reset(0.0f, VBUS_LOSS_IID);
    consensus_test_force_heard_bit(1, 3, 1);
    syn_ticks(40);
    g_emit[3] = 0u;
    syn_ticks(12);
    CHECK(consensus_get_stats().fast_path_fires == 0u);
    /* ...but the vote path still elevates on its normal schedule: the
       window fills with misses, everyone else votes FAULT. */
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).tier == CONSENSUS_TIER_ELEVATED);
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).fast_path == 0u);

    /* Reporter exclusion by row count: 4 nodes hear almost nothing (row
       under CONSENSUS_REPORTER_MIN_HEARD), so only 5 valid reporters remain
       out of 9, still enough for 2f+1 = 5. With a fifth excluded, the rule
       cannot fire. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    for(uint8_t r = 4; r <= 8; r++)
    {
        /* Claim: heard nobody except target 3's row is irrelevant; a row of
           misses is an unhealthy reporter. */
        consensus_test_force_heard_bit(r, CONSENSUS_ANY, 0);
    }
    syn_ticks(40);
    g_emit[3] = 0u;
    syn_ticks(6);
    CHECK(consensus_get_stats().fast_path_fires == 0u);     /* only 4 valid reporters */
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).n_reporters == 4u);
    g_emit[3] = 1u;

    syn_reset(0.0f, VBUS_LOSS_IID);
    for(uint8_t r = 5; r <= 8; r++)
        consensus_test_force_heard_bit(r, CONSENSUS_ANY, 0);
    syn_ticks(40);
    g_emit[3] = 0u;
    syn_ticks(6);
    CHECK(consensus_get_stats().fast_path_fires >= 1u);     /* 5 valid reporters */
}

static void test_graded_self_deaf(void)
{
    PeerObservation_t p;
    uint8_t accused_max = 0u;

    /* Three targets go persistently silent (a 3-way concurrent comm fault).
       Every observer would accuse 3 targets, so all abstain (graded rule);
       the fast path is what catches them. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(40);
    g_emit[1] = g_emit[2] = g_emit[3] = 0u;
    syn_ticks(12);
    for(uint8_t t = 1; t <= 3; t++)
    {
        consensus_get_peer_observation(0, t, &p);
        CHECK(p.vote[CONSENSUS_CLASS_COMMUNICATION].verdict == PEER_VERDICT_ABSTAIN);
        CHECK(p.observer_graded_abstain == 1u);
        CHECK(res(t, CONSENSUS_CLASS_COMMUNICATION).tier == CONSENSUS_TIER_ELEVATED);
        CHECK(res(t, CONSENSUS_CLASS_COMMUNICATION).fast_path == 1u);
    }

    /* Two silent targets stay under the limit: FAULT votes are cast. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(40);
    g_emit[1] = g_emit[2] = 0u;
    syn_ticks(8);
    consensus_get_peer_observation(0, 1, &p);
    CHECK(p.vote[CONSENSUS_CLASS_COMMUNICATION].verdict == PEER_VERDICT_FAULT);
    CHECK(p.observer_graded_abstain == 0u);

    /* A degraded receiver (half its packets lost) never accuses 3 or more
       targets in one tick, and does not push anyone to ELEVATED. */
    syn_reset(0.05f, VBUS_LOSS_IID);
    vbus_set_node_rx_loss(3, 0.5f);
    for(uint32_t i = 0; i < 400u; i++)
    {
        uint8_t accused = 0u;

        syn_tick();
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            if(t == 3u) continue;
            consensus_get_peer_observation(3, t, &p);
            if(p.vote[CONSENSUS_CLASS_COMMUNICATION].verdict == PEER_VERDICT_FAULT)
                accused++;
        }
        if(accused > accused_max) accused_max = accused;
    }
    CHECK(accused_max <= CONSENSUS_COMM_MAX_ACCUSED);
}

static void test_self_deaf_after_dark(void)
{
    PeerObservation_t p;

    /* Node 3 goes dark (neither sends nor receives) for 30 ticks and comes
       back: it must cast no communication FAULT in the 6+ ticks after. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(40);
    vbus_set_node_dark(3, 1);
    syn_ticks(30);
    vbus_set_node_dark(3, 0);
    for(uint32_t i = 0; i < 12u; i++)
    {
        syn_tick();
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            if(t == 3u) continue;
            consensus_get_peer_observation(3, t, &p);
            CHECK(p.vote[CONSENSUS_CLASS_COMMUNICATION].verdict != PEER_VERDICT_FAULT);
        }
    }
    /* While dark, the others saw it as silent: FAULT votes about 3 and an
       ELEVATED tier, but it never counted as "no fault". */
    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(40);
    vbus_set_node_dark(3, 1);
    syn_ticks(12);
    consensus_get_peer_observation(0, 3, &p);
    CHECK(p.vote[CONSENSUS_CLASS_COMMUNICATION].verdict == PEER_VERDICT_FAULT);
    /* A dark fuser hears nothing: hold one round, then NONE with n_live 0. */
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).n_live == 0u);
    CHECK(res(3, CONSENSUS_CLASS_COMMUNICATION).tier == CONSENSUS_TIER_NONE);
}

static void test_dark_voter_mid_round(void)
{
    ConsensusResult_t before, after, later;

    /* Peers 1..8 vote FAULT (forced) about target 0; peer 9 is honest and
       abstains on mobility (target 0 is not travelling). Node 4 then goes
       dark right after sending the beacon of the first tick of a round. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    for(uint8_t v = 1; v <= 8; v++)
        consensus_test_force_verdict(v, 0, CONSENSUS_CLASS_MOBILITY, PEER_VERDICT_FAULT);
    syn_ticks(20);
    CHECK(res(0, CONSENSUS_CLASS_MOBILITY).tier == CONSENSUS_TIER_ELEVATED);
    CHECK(res(0, CONSENSUS_CLASS_MOBILITY).n_fault == 8u);
    CHECK(res(0, CONSENSUS_CLASS_MOBILITY).n_live == 9u);

    while((g_tick + 1u) % CONSENSUS_ROUND_TICKS != 0u)
        syn_tick();
    syn_tick();                                             /* first tick of round r; 4 has sent its beacon */
    vbus_set_node_dark(4, 1);
    syn_tick();                                             /* last tick of r: 4's beacon (in flight) arrives */
    before = res(0, CONSENSUS_CLASS_MOBILITY);
    CHECK(before.n_live == 9u && before.n_fault == 8u);     /* still counted in round r */
    syn_tick();                                             /* round r+1, first tick */
    syn_tick();                                             /* round r+1 closes: 4 sent nothing */
    after = res(0, CONSENSUS_CLASS_MOBILITY);
    CHECK(after.n_live == 8u);
    CHECK(after.n_dark == 1u);
    CHECK(after.n_fault == 7u);
    CHECK(after.n_clear == 0u);                             /* dark is not CLEAR */
    CHECK(after.tier == CONSENSUS_TIER_ELEVATED);           /* x = 7 >= 3 and 7 >= 2*0+1 */
    vbus_set_node_dark(4, 0);
    syn_ticks(6);
    later = res(0, CONSENSUS_CLASS_MOBILITY);
    CHECK(later.n_live == 9u && later.n_fault == 8u);
}

static void liar_run(float loss, uint8_t variant, uint32_t *elevated)
{
    syn_reset(loss, VBUS_LOSS_IID);
    consensus_test_force_verdict(4, CONSENSUS_ANY, CONSENSUS_ANY, PEER_VERDICT_FAULT);
    if(variant >= 1u)
        consensus_test_force_verdict(7, CONSENSUS_ANY, CONSENSUS_ANY, PEER_VERDICT_FAULT);
    if(variant == 2u)
    {
        vbus_set_node_dark(1, 1);
        vbus_set_node_dark(2, 1);
        vbus_set_node_dark(3, 1);
    }
    *elevated = 0u;
    for(uint32_t i = 0; i < 600u; i++)
    {
        syn_tick();
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            if(variant == 2u && t >= 1u && t <= 3u)
                continue;                   /* dark by construction */
            for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
                if(res(t, (ConsensusClass_t)c).tier == CONSENSUS_TIER_ELEVATED)
                    (*elevated)++;
        }
    }
}

static void test_liars(void)
{
    /* One liar, two liars, and two liars with three honest peers dark: a
       healthy target must never elevate at q = 0.05 (asserted). At q = 0.20
       honest loss can add false FAULT votes on top of the liars, so that
       case is reported, not asserted (design 7.2). */
    for(uint8_t variant = 0; variant < 3u; variant++)
    {
        uint32_t elevated;

        liar_run(0.05f, variant, &elevated);
        CHECK(elevated == 0u);
        liar_run(0.20f, variant, &elevated);
        printf("  liars variant %u at q=0.20: %u ELEVATED target-ticks (reported)\n",
            variant, (unsigned)elevated);
    }
}

static void test_mobility_mirror(void)
{
    PeerObservation_t p;
    uint8_t n_before;

    /* A beacon sent at tick k is observed at tick k+1, so every check below
       runs one tick after the state it looks at. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(5);
    g_teams[2].status = STATUS_EN_ROUTE;
    g_teams[2].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[2].assigned_task_slot = 3u;
    for(uint8_t i = 0; i < 7u; i++)                         /* ticks 6..12, one cell each */
    {
        g_teams[2].loc_x++;
        syn_tick();
    }
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.mob_window_n == 4u && p.mob_stalls == 0u);
    CHECK(p.vote[CONSENSUS_CLASS_MOBILITY].verdict == PEER_VERDICT_CLEAR);

    /* Ticks 13..15 without moving: beacons 13 and 14 are stalls, seen at 14
       and 15. Two stalls in a full window is FAULT. */
    syn_ticks(3);
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.mob_stalls == 2u);
    CHECK(p.vote[CONSENSUS_CLASS_MOBILITY].verdict == PEER_VERDICT_FAULT);

    /* Tick 16: new leg, still moving. Seen at tick 17: window reset, no
       sample taken on the tick the leg changed. */
    syn_tick();                                             /* 16: sees b15, a third stall */
    g_teams[2].assigned_task_slot = 4u;
    g_teams[2].loc_x++;
    syn_tick();                                             /* 17: hmm, b16 is still the old leg */
    g_teams[2].loc_x++;
    syn_tick();                                             /* 18: sees b17, the first beacon on the new leg */
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.leg_changed == 1u && p.mob_sampled == 0u);
    CHECK(p.mob_window_n == 0u && p.mob_stalls == 0u);
    CHECK(p.vote[CONSENSUS_CLASS_MOBILITY].verdict == PEER_VERDICT_ABSTAIN);

    /* A missed beacon never creates a stall: b20 is not sent, b21 is 2 cells
       from b19. No sample, no reset, no plausibility failure. */
    g_teams[2].loc_x++;
    syn_tick();                                             /* 19: b18 sampled, moved */
    g_teams[2].loc_x++;
    syn_tick();                                             /* 20: b19 sampled, moved */
    consensus_get_peer_observation(0, 2, &p);
    n_before = p.mob_window_n;
    g_emit[2] = 0u;
    g_teams[2].loc_x++;
    syn_tick();                                             /* 21: b20 not emitted */
    g_emit[2] = 1u;
    g_teams[2].loc_x++;
    syn_tick();                                             /* 22: b21 emitted, seen at 22? no: at 23 */
    syn_tick();                                             /* 23: sees b22 */
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.mob_window_n >= n_before);                      /* gap: no reset */
    CHECK(p.mob_stalls == 0u);
    CHECK(p.plaus_fail == 0u);
    CHECK(p.rep_beta == 0u);

    /* Not travelling: the window resets and mobility abstains. */
    g_teams[2].status = STATUS_ON_SCENE;
    syn_ticks(2);
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.mob_window_n == 0u);
    CHECK(p.vote[CONSENSUS_CLASS_MOBILITY].verdict == PEER_VERDICT_ABSTAIN);
}

static void test_plausibility(void)
{
    PeerObservation_t p;

    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(5);
    g_teams[2].status = STATUS_EN_ROUTE;
    g_teams[2].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[2].assigned_task_slot = 3u;
    for(uint8_t i = 0; i < 5u; i++)                         /* ticks 6..10 */
    {
        g_teams[2].loc_x++;
        syn_tick();
    }
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.plaus_fail == 0u && p.rep_alpha >= 2u && p.rep_beta == 0u);

    /* A 2-cell jump in one tick (tick 11), seen at tick 12. */
    g_teams[2].loc_x = (int16_t)(g_teams[2].loc_x + 2);
    syn_ticks(2);
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.plaus_fail == 1u && p.rep_beta == 1u);
    CHECK(p.mob_window_n == 0u);

    /* Outside the grid. */
    g_teams[2].loc_x = (int16_t)(GRID_WIDTH + 3);
    syn_ticks(2);
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.plaus_fail == 1u && p.rep_beta == 2u);

    /* Keep lying: trust drops below 1/2 and mobility abstains for good. */
    for(uint8_t i = 0; i < 16u; i++)
    {
        g_teams[2].loc_x = (int16_t)(2 + (i % 2u) * 6);
        syn_tick();
    }
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.rep_beta > p.rep_alpha);
    CHECK(p.vote[CONSENSUS_CLASS_MOBILITY].verdict == PEER_VERDICT_ABSTAIN);
}

static void test_peer_obs_bounded_counters(void)
{
    PeerObservation_t p;

    /* Reputation counters halve together instead of wrapping. */
    syn_reset(0.0f, VBUS_LOSS_IID);
    g_teams[2].status = STATUS_EN_ROUTE;
    g_teams[2].assigned_task_pool = TASK_POOL_INCIDENT;
    g_teams[2].assigned_task_slot = 3u;
    for(uint32_t i = 0; i < 700u; i++)
    {
        g_teams[2].loc_x = (int16_t)(2 + (i % 20u));      /* 1 cell per tick, wraps back: a 19-cell jump */
        syn_tick();
    }
    consensus_get_peer_observation(0, 2, &p);
    CHECK(p.rep_alpha < 255u && p.rep_beta < 255u);
    CHECK(p.rep_alpha + p.rep_beta > 0u);
}

static void test_airtime_and_stats(void)
{
    VbusStats_t st;

    syn_reset(0.0f, VBUS_LOSS_IID);
    syn_ticks(100);
    st = vbus_get_stats();
    CHECK(st.sent == 100u * NUM_TEAMS);
    /* 80-byte wire frame = 52 payload + 7 header + 21 trailer. */
    CHECK(CONSENSUS_BEACON_SIZE + LORA_FRAME_HEADER_SIZE + LORA_FRAME_TRAILER_SIZE == 80u);
    CHECK(lora_time_on_air_ms_x1000(80u) == 143616u);
    CHECK(st.airtime_ms_total == (uint32_t)((uint64_t)st.sent * 143616u / 1000u));
    CHECK(st.dropped == 0u);
}

int main(void)
{
    test_beacon_roundtrip();
    test_quantization();
    test_wilson();
    test_fti();
    test_gate_truth_table();
    test_healthy_zero_votes();
    test_fast_path();
    test_fast_path_reporters();
    test_graded_self_deaf();
    test_self_deaf_after_dark();
    test_dark_voter_mid_round();
    test_liars();
    test_mobility_mirror();
    test_plausibility();
    test_peer_obs_bounded_counters();
    test_airtime_and_stats();

    if(g_fail)
    {
        printf("consensus_selftest: %d FAILED\n", g_fail);
        return 1;
    }
    printf("consensus_selftest: all passed\n");
    return 0;
}

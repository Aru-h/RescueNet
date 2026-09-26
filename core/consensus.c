#include "consensus.h"

#if VIRTUAL_BUS_ENABLED

#include <string.h>

#include "vbus.h"
#include "mission_state.h"
#include "random.h"
#include "team.h"
#include "watchdog.h"
#include "recovery_manager_config.h"

/*
 * Peer consensus, see CONSENSUS_DESIGN.md. Section numbers in the comments
 * below refer to that document.
 *
 * Hard rule: this file never reads team->fault_state. Its only inputs are
 * received beacons, team_radio_emitted() (physics output) and the team's own
 * observable state, which is what goes into the beacon it sends.
 */

/* ------------------------------------------------------------------ */
/* Wilson table (design 2.3)                                          */
/* ------------------------------------------------------------------ */

/* CONSENSUS_WILSON[n][k] = {lo, hi} of the Wilson score interval (z = 1.645)
   on k events in n samples, n <= 6. k == 0 has lo 0 and k == n has hi 1.
   n == 0 is the uninformative [0, 1]. Precomputed so the board needs no
   sqrtf and laptop and board agree bit for bit. */
static const float CONSENSUS_WILSON[7][7][2] =
{
    { {0.000000f, 1.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f} },
    { {0.000000f, 0.730169f}, {0.269831f, 1.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f} },
    { {0.000000f, 0.575013f}, {0.120852f, 0.879148f}, {0.424987f, 1.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f} },
    { {0.000000f, 0.474240f}, {0.078257f, 0.746490f}, {0.253510f, 0.921743f}, {0.525760f, 1.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f} },
    { {0.000000f, 0.403521f}, {0.057901f, 0.643860f}, {0.182383f, 0.817617f}, {0.356140f, 0.942099f}, {0.596479f, 1.000000f}, {0.000000f, 0.000000f}, {0.000000f, 0.000000f} },
    { {0.000000f, 0.351157f}, {0.045957f, 0.564737f}, {0.142693f, 0.727538f}, {0.272462f, 0.857307f}, {0.435263f, 0.954043f}, {0.648843f, 1.000000f}, {0.000000f, 0.000000f} },
    { {0.000000f, 0.310822f}, {0.038101f, 0.502447f}, {0.117265f, 0.653009f}, {0.221243f, 0.778757f}, {0.346991f, 0.882735f}, {0.497553f, 0.961899f}, {0.689178f, 1.000000f} },
};

void consensus_wilson(uint8_t n, uint8_t k, float *lo, float *hi)
{
    if(n > 6u)
        n = 6u;
    if(k > n)
        k = n;

    *lo = CONSENSUS_WILSON[n][k][0];
    *hi = CONSENSUS_WILSON[n][k][1];
}

static float clamp01f(float v)
{
    if(!(v > 0.0f))
        return 0.0f;
    if(v > 1.0f)
        return 1.0f;
    return v;
}

void consensus_severity_interval(uint8_t n, uint8_t k, float full_scale,
    float *lo, float *hi)
{
    float rate_lo, rate_hi;

    consensus_wilson(n, k, &rate_lo, &rate_hi);
    *lo = clamp01f(rate_lo / full_scale);
    *hi = clamp01f(rate_hi / full_scale);
}

uint8_t consensus_quantize_lo(float lo)
{
    float scaled = clamp01f(lo) * 15.0f;
    uint8_t q = (uint8_t)scaled;    /* truncation = floor for scaled >= 0 */

    return q > 15u ? 15u : q;
}

uint8_t consensus_quantize_hi(float hi)
{
    float scaled = clamp01f(hi) * 15.0f;
    uint8_t q = (uint8_t)scaled;

    if((float)q < scaled)
        q++;
    return q > 15u ? 15u : q;
}

/* ------------------------------------------------------------------ */
/* Beacon pack / unpack (design 1.2)                                  */
/* ------------------------------------------------------------------ */

_Static_assert(CONSENSUS_BEACON_SIZE <= LORA_FRAME_MAX_PAYLOAD,
    "beacon must fit one LoRa frame");

uint8_t consensus_beacon_pack(const ConsensusBeacon_t *b, uint8_t *out)
{
    uint8_t pos = 0u;

    out[pos++] = b->version;
    out[pos++] = b->team_id;
    out[pos++] = (uint8_t)(b->seq & 0xFFu);
    out[pos++] = (uint8_t)(b->seq >> 8);
    out[pos++] = b->status;
    out[pos++] = b->loc_x;
    out[pos++] = b->loc_y;
    out[pos++] = b->task_pool;
    out[pos++] = b->task_slot;
    out[pos++] = b->leg_flags;
    out[pos++] = (uint8_t)(b->heard_bits & 0xFFu);
    out[pos++] = (uint8_t)(b->heard_bits >> 8);

    for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
    {
        for(uint8_t j = 0; j < NUM_TEAMS; j++)
        {
            const ConsensusVote_t *v = &b->votes[c][j];

            out[pos++] = (uint8_t)(((v->verdict & 3u) << 6) | (v->n_samples & 63u));
            out[pos++] = (uint8_t)(((v->lo_q & 15u) << 4) | (v->hi_q & 15u));
        }
    }

    return pos;
}

int consensus_beacon_unpack(const uint8_t *in, uint8_t len, uint8_t vbus_src,
    ConsensusBeacon_t *out)
{
    uint8_t pos = 0u;

    if(len != CONSENSUS_BEACON_SIZE)
        return CONSENSUS_UNPACK_BAD_LENGTH;
    if(in[0] != CONSENSUS_BEACON_VERSION)
        return CONSENSUS_UNPACK_BAD_VERSION;
    if(in[1] != vbus_src)
        return CONSENSUS_UNPACK_BAD_SOURCE;

    out->version = in[pos++];
    out->team_id = in[pos++];
    out->seq = (uint16_t)(in[pos] | ((uint16_t)in[pos + 1u] << 8));
    pos += 2u;
    out->status = in[pos++];
    out->loc_x = in[pos++];
    out->loc_y = in[pos++];
    out->task_pool = in[pos++];
    out->task_slot = in[pos++];
    out->leg_flags = in[pos++];
    out->heard_bits = (uint16_t)(in[pos] | ((uint16_t)in[pos + 1u] << 8));
    pos += 2u;

    for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
    {
        for(uint8_t j = 0; j < NUM_TEAMS; j++)
        {
            ConsensusVote_t *v = &out->votes[c][j];
            uint8_t a = in[pos++];
            uint8_t bb = in[pos++];

            v->verdict = (uint8_t)(a >> 6);
            v->n_samples = (uint8_t)(a & 63u);
            v->lo_q = (uint8_t)(bb >> 4);
            v->hi_q = (uint8_t)(bb & 15u);
            if(v->verdict > PEER_VERDICT_FAULT)
                return CONSENSUS_UNPACK_BAD_VERDICT;
        }
    }

    return CONSENSUS_UNPACK_OK;
}

/* ------------------------------------------------------------------ */
/* Gate (design 3.3)                                                  */
/* ------------------------------------------------------------------ */

float consensus_fti_lower_bound(const float *lo, uint8_t count, uint8_t f)
{
    float sorted[NUM_TEAMS];
    uint8_t n = count > NUM_TEAMS ? (uint8_t)NUM_TEAMS : count;

    if(n < (uint8_t)(f + 1u))
        return 0.0f;

    for(uint8_t i = 0; i < n; i++)
        sorted[i] = lo[i];

    /* Insertion sort, descending; n <= 10. */
    for(uint8_t i = 1; i < n; i++)
    {
        float key = sorted[i];
        int j = (int)i - 1;

        while(j >= 0 && sorted[j] < key)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    return sorted[f];
}

static uint8_t consensus_domain_of(uint8_t node)
{
    /* One team = one node = one failure domain on the virtual bus. */
    return node;
}

ConsensusResult_t consensus_gate_compute(uint8_t n_live, const uint8_t *verdict,
    const float *lo, const uint8_t *domain, uint8_t count, uint8_t is_fast)
{
    ConsensusResult_t r;
    uint16_t fault_domains = 0u;
    uint16_t clear_domains = 0u;
    uint16_t seen_domains = 0u;
    float opinion_lo[NUM_TEAMS];
    uint8_t n_opinion = 0u;
    uint8_t n_fault_domains = 0u;
    uint8_t n_clear_domains = 0u;

    memset(&r, 0, sizeof(r));
    r.n_live = n_live;
    r.n_dark = (uint8_t)(NUM_TEAMS - 1u > n_live ? NUM_TEAMS - 1u - n_live : 0u);

    /* A domain is FAULT if its live FAULT members outnumber its CLEAR ones,
       CLEAR if the reverse, otherwise it abstains. With one member per
       domain (this build) that is just the member's own verdict. */
    for(uint8_t d = 0; d < 16u; d++)
    {
        uint8_t nf = 0u, nc = 0u, members = 0u;

        for(uint8_t i = 0; i < count; i++)
        {
            if(domain[i] != d)
                continue;
            members++;
            if(verdict[i] == PEER_VERDICT_FAULT)
                nf++;
            else if(verdict[i] == PEER_VERDICT_CLEAR)
                nc++;
        }

        if(members == 0u)
            continue;
        seen_domains |= (uint16_t)(1u << d);
        if(nf > nc)
            fault_domains |= (uint16_t)(1u << d);
        else if(nc > nf)
            clear_domains |= (uint16_t)(1u << d);
    }
    (void)seen_domains;

    for(uint8_t d = 0; d < 16u; d++)
    {
        if(fault_domains & (1u << d))
            n_fault_domains++;
        if(clear_domains & (1u << d))
            n_clear_domains++;
    }

    for(uint8_t i = 0; i < count; i++)
    {
        if(verdict[i] == PEER_VERDICT_FAULT)
            r.n_fault++;
        else if(verdict[i] == PEER_VERDICT_CLEAR)
            r.n_clear++;
        else
            r.n_abstain++;

        if(verdict[i] != PEER_VERDICT_ABSTAIN && n_opinion < NUM_TEAMS)
            opinion_lo[n_opinion++] = lo[i];
    }

    r.x = n_fault_domains;
    r.y = (r.n_fault > 0u) ?
        consensus_fti_lower_bound(opinion_lo, n_opinion, (uint8_t)CONSENSUS_F) : 0.0f;

    if(is_fast ||
       (n_live >= CONSENSUS_MIN_LIVE_VOTERS &&
        r.x >= CONSENSUS_X_MIN &&
        n_fault_domains >= 2u * n_clear_domains + 1u) ||
       (r.x == 1u && r.y >= CONSENSUS_TAU_SINGLE_BOARD))
    {
        r.tier = CONSENSUS_TIER_ELEVATED;
    }
    else if(r.x >= 1u && r.y >= CONSENSUS_TAU_LOCAL)
    {
        r.tier = CONSENSUS_TIER_LOCAL;
    }
    else
    {
        r.tier = CONSENSUS_TIER_NONE;
    }

    r.fast_path = is_fast;
    return r;
}

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    /* Mobility mirror of watchdog.c's update_movement_stalls() (design 2.2). */
    uint16_t last_seq;
    uint8_t has_last;
    uint8_t last_status, last_x, last_y, last_pool, last_slot, last_leg_flags;
    uint8_t stall_history, stall_samples;
    uint8_t rep_alpha, rep_beta;

    /* Communication window (design 2.1). */
    uint8_t comm_miss_bits, comm_window_n;

    /* Current verdicts and intervals, sent in the next beacon. */
    ConsensusVote_t vote[CONSENSUS_NUM_VOTED_CLASSES];
    float lo[CONSENSUS_NUM_VOTED_CLASSES], hi[CONSENSUS_NUM_VOTED_CLASSES];

    /* Per-tick log, cleared at the start of every step. */
    uint8_t on_time, late, stale, plaus_fail, leg_changed, mob_sampled,
        mob_stalled, graded, sent_last_tick, dropped_gt;
} PeerObserver_t;

typedef struct
{
    /* Votes about this node from each voter, this round (design 3.2). */
    uint8_t got[NUM_TEAMS];
    uint8_t vote_has[NUM_TEAMS];
    uint16_t vote_seq[NUM_TEAMS];
    ConsensusVote_t vote[NUM_TEAMS][CONSENSUS_NUM_VOTED_CLASSES];

    /* Heard-matrix: reporter p's bitmap about tick k, ring keyed by k. */
    uint16_t hm_bits[NUM_TEAMS][4];
    uint32_t hm_tick_plus1[NUM_TEAMS][4];

    /* Fast path (design 2.6). */
    uint8_t silent_valid;
    uint32_t silent_k;
    uint32_t fast_hold_until;
    uint8_t fast_active;
    uint8_t n_reporters, n_heard;

    /* Vote-path result, held until the next round closes. */
    ConsensusResult_t vote_result[CONSENSUS_NUM_VOTED_CLASSES];
    uint8_t dark_rounds;
} Fuser_t;

static PeerObserver_t g_obs[NUM_TEAMS][NUM_TEAMS];      /* [observer][target] */
static Fuser_t g_fuser[NUM_TEAMS];
static ConsensusResult_t g_result[NUM_TEAMS][CONSENSUS_NUM_VOTED_CLASSES];
static uint16_t g_heard_bits[NUM_TEAMS];                /* row about tick now-1 */
static uint8_t g_self_deaf[NUM_TEAMS];
static uint8_t g_heard_count[NUM_TEAMS];
static uint8_t g_sent_this_tick[NUM_TEAMS];
static uint8_t g_have_prev_step;
static uint32_t g_prev_step_tick;
static ConsensusBusConfig_t g_bus_cfg =
    { (uint8_t)VBUS_LOSS_IID, CONSENSUS_DEFAULT_LINK_LOSS, 0.0f, 0.0f, 0.0f };
static ConsensusStats_t g_stats;

uint32_t g_diag_consensus_capped[NUM_TEAMS];
uint8_t g_consensus_capped_now[NUM_TEAMS];

#if CONSENSUS_TEST_HOOKS
static uint8_t g_force_verdict_on[NUM_TEAMS][NUM_TEAMS][CONSENSUS_NUM_VOTED_CLASSES];
static uint8_t g_force_verdict_val[NUM_TEAMS][NUM_TEAMS][CONSENSUS_NUM_VOTED_CLASSES];
static uint8_t g_force_heard[NUM_TEAMS][NUM_TEAMS];     /* 0 off, 1 heard, 2 missed */
#endif

static uint8_t popcount8(uint8_t v)
{
    uint8_t n = 0u;

    while(v)
    {
        n = (uint8_t)(n + (v & 1u));
        v >>= 1;
    }
    return n;
}

#if CONSENSUS_FAST_PATH
static uint8_t popcount16(uint16_t v)
{
    uint8_t n = 0u;

    while(v)
    {
        n = (uint8_t)(n + (v & 1u));
        v >>= 1;
    }
    return n;
}
#endif

void consensus_configure_bus(const ConsensusBusConfig_t *cfg)
{
    if(cfg != NULL)
        g_bus_cfg = *cfg;
}

void consensus_set_link_loss(float link_loss)
{
    g_bus_cfg.link_loss = link_loss;
}

void consensus_init(uint32_t world_seed)
{
    VbusConfig_t cfg;

    memset(g_obs, 0, sizeof(g_obs));
    memset(g_fuser, 0, sizeof(g_fuser));
    memset(g_result, 0, sizeof(g_result));
    memset(g_heard_bits, 0, sizeof(g_heard_bits));
    memset(g_self_deaf, 0, sizeof(g_self_deaf));
    memset(g_heard_count, 0, sizeof(g_heard_count));
    memset(g_sent_this_tick, 0, sizeof(g_sent_this_tick));
    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_diag_consensus_capped, 0, sizeof(g_diag_consensus_capped));
    memset(g_consensus_capped_now, 0, sizeof(g_consensus_capped_now));
    g_have_prev_step = 0u;
    g_prev_step_tick = 0u;
#if CONSENSUS_TEST_HOOKS
    consensus_test_clear_forced();
#endif

    for(uint8_t n = 0; n < NUM_TEAMS; n++)
    {
        for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
            g_result[n][c].tier = CONSENSUS_TIER_NONE;
    }

    /* The bus has its own RNG stream, seeded from the world seed but never
       drawing from core/random.c, so switching it on cannot move the world.
       Delivery delay is fixed at 0: a beacon sent at tick k is delivered by
       the vbus_step() of tick k+1, which is the on-time deadline (2.1). */
    memset(&cfg, 0, sizeof(cfg));
    cfg.delay_ticks = 0u;
    cfg.loss_model = (VbusLossModel_t)g_bus_cfg.loss_model;
    cfg.link_loss = g_bus_cfg.link_loss;
    cfg.p_gb = g_bus_cfg.p_gb;
    cfg.p_bg = g_bus_cfg.p_bg;
    cfg.loss_bad = g_bus_cfg.loss_bad;
    cfg.seed = (world_seed ^ 0xC0DE5EEDu) * SEED_MIX_CONSTANT + 0x9E3779B9u;
    vbus_init(NUM_TEAMS, &cfg);

#if TEAM_TEST_RANDOM_EMISSION
    team_test_seed_random_emission((world_seed ^ 0x51ED270Bu) * SEED_MIX_CONSTANT + 1u);
#endif
}

ConsensusResult_t consensus_get_result(uint8_t team, ConsensusClass_t cls)
{
    ConsensusResult_t none;

    if(team >= NUM_TEAMS || (uint8_t)cls >= CONSENSUS_NUM_VOTED_CLASSES)
    {
        memset(&none, 0, sizeof(none));
        return none;
    }
    return g_result[team][cls];
}

ConsensusRunInfo_t consensus_get_run_info(void)
{
    ConsensusRunInfo_t info;
    float loss = g_bus_cfg.link_loss;

    info.vbus_link_loss_ppm = (uint32_t)(loss * 1000000.0f + 0.5f);
    info.vbus_loss_model = g_bus_cfg.loss_model;
    info.enforce = (uint8_t)CONSENSUS_ENFORCE;
    return info;
}

ConsensusStats_t consensus_get_stats(void)
{
    return g_stats;
}

/* ------------------------------------------------------------------ */
/* Beacon reception (design 2.1, 2.2, 2.6, 3.2)                       */
/* ------------------------------------------------------------------ */

static uint8_t abs_diff_u8(uint8_t a, uint8_t b)
{
    return a > b ? (uint8_t)(a - b) : (uint8_t)(b - a);
}

static void mobility_observe(PeerObserver_t *o, const ConsensusBeacon_t *b)
{
    uint8_t heading = (uint8_t)(b->leg_flags & 1u);
    uint8_t zone = (uint8_t)(b->leg_flags >> 1);
    uint8_t last_heading = (uint8_t)(o->last_leg_flags & 1u);
    uint8_t last_zone = (uint8_t)(o->last_leg_flags >> 1);
    uint16_t gap = (uint16_t)(b->seq - o->last_seq);
    uint8_t leg_changed;
    uint8_t consecutive;
    uint8_t plaus_fail = 0u;

    if(!o->has_last)
    {
        /* First beacon from this target: nothing to compare against. The
           position still has to be on the map. */
        if(b->loc_x >= GRID_WIDTH || b->loc_y >= GRID_HEIGHT)
        {
            o->plaus_fail = 1u;
            if(o->rep_beta < 255u)
                o->rep_beta++;
        }
        o->stall_history = 0u;
        o->stall_samples = 0u;
        goto store;
    }

    leg_changed = (uint8_t)(b->task_pool != o->last_pool ||
        b->task_slot != o->last_slot ||
        heading != last_heading ||
        (heading && zone != last_zone));
    consecutive = (uint8_t)(gap == 1u);
    o->leg_changed = leg_changed;

    /* Plausibility (design 2.2): on the map, at most one cell per tick
       elapsed, and a single step on a consecutive same-leg EN_ROUTE pair is
       0 or 1 cell. */
    if(b->loc_x >= GRID_WIDTH || b->loc_y >= GRID_HEIGHT)
    {
        plaus_fail = 1u;
    }
    else
    {
        uint8_t dx = abs_diff_u8(b->loc_x, o->last_x);
        uint8_t dy = abs_diff_u8(b->loc_y, o->last_y);
        uint8_t cheb = dx > dy ? dx : dy;

        if(cheb > gap)
            plaus_fail = 1u;
        else if(consecutive && b->status == STATUS_EN_ROUTE &&
                o->last_status == STATUS_EN_ROUTE && !leg_changed && cheb > 1u)
            plaus_fail = 1u;
    }

    if(plaus_fail)
    {
        o->plaus_fail = 1u;
        o->stall_history = 0u;
        o->stall_samples = 0u;
        if(o->rep_beta < 255u)
            o->rep_beta++;
    }
    else
    {
        if(consecutive && o->rep_alpha < 255u)
            o->rep_alpha++;

        if(b->status == STATUS_EN_ROUTE && o->last_status == STATUS_EN_ROUTE &&
           consecutive && !leg_changed)
        {
            uint8_t stalled = (uint8_t)(b->loc_x == o->last_x && b->loc_y == o->last_y);
            uint8_t mask = (uint8_t)((1u << WATCHDOG_STALL_WINDOW_TICKS) - 1u);

            o->stall_history = (uint8_t)(((o->stall_history << 1) | stalled) & mask);
            if(o->stall_samples < WATCHDOG_STALL_WINDOW_TICKS)
                o->stall_samples++;
            o->mob_sampled = 1u;
            o->mob_stalled = stalled;
        }
        else if(b->status != STATUS_EN_ROUTE || leg_changed)
        {
            o->stall_history = 0u;
            o->stall_samples = 0u;
        }
        /* else: a gap in seq while still EN_ROUTE on the same leg. No
           sample and no reset: a missed beacon never makes a stall. */
    }

    if(o->rep_alpha == 255u || o->rep_beta == 255u)
    {
        o->rep_alpha = (uint8_t)(o->rep_alpha >> 1);
        o->rep_beta = (uint8_t)(o->rep_beta >> 1);
    }

store:
    o->has_last = 1u;
    o->last_seq = b->seq;
    o->last_status = b->status;
    o->last_x = b->loc_x;
    o->last_y = b->loc_y;
    o->last_pool = b->task_pool;
    o->last_slot = b->task_slot;
    o->last_leg_flags = b->leg_flags;
}

static void handle_beacon(uint8_t receiver, uint8_t src, const uint8_t *payload,
    uint8_t len, uint32_t now)
{
    ConsensusBeacon_t b;
    PeerObserver_t *o;
    Fuser_t *f;
    int32_t age;

    if(src >= NUM_TEAMS || src == receiver)
        return;

    if(consensus_beacon_unpack(payload, len, src, &b) != CONSENSUS_UNPACK_OK)
    {
        g_stats.rx_bad++;
        return;
    }

    /* d = 0: this is the beacon for tick now-1, on time. d > 0: late. */
    age = (int16_t)((uint16_t)(now - 1u) - b.seq);
    if(age < 0)
    {
        g_stats.rx_bad++;    /* from the future: cannot be genuine */
        return;
    }

    o = &g_obs[receiver][src];

    if(o->has_last && (int16_t)(b.seq - o->last_seq) <= 0)
    {
        o->stale = 1u;
        g_stats.rx_stale++;
        return;
    }

    g_stats.beacons_rx++;
    if(age == 0)
    {
        o->on_time = 1u;
    }
    else
    {
        o->late = 1u;
        g_stats.rx_late++;
    }

    mobility_observe(o, &b);

    /* The receiver is the fuser for votes about itself, and for the heard
       bitmaps that say who heard it. */
    f = &g_fuser[receiver];
    f->got[src] = 1u;

    if(!f->vote_has[src] || (int16_t)(b.seq - f->vote_seq[src]) > 0)
    {
        f->vote_has[src] = 1u;
        f->vote_seq[src] = b.seq;
        for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
            f->vote[src][c] = b.votes[c][receiver];
    }

    {
        uint32_t k = (uint32_t)b.seq - 1u;    /* the tick this bitmap describes */
        uint8_t slot = (uint8_t)(k & 3u);

        f->hm_bits[src][slot] = b.heard_bits;
        f->hm_tick_plus1[src][slot] = k + 1u;
    }
}

/* ------------------------------------------------------------------ */
/* Verdicts (design 2.1, 2.2, 2.3)                                    */
/* ------------------------------------------------------------------ */

static void set_vote(PeerObserver_t *o, uint8_t cls, PeerVerdict_t verdict,
    uint8_t n, float lo, float hi)
{
    o->vote[cls].verdict = (uint8_t)verdict;
    o->vote[cls].n_samples = n;
    o->vote[cls].lo_q = consensus_quantize_lo(lo);
    o->vote[cls].hi_q = consensus_quantize_hi(hi);
    o->lo[cls] = lo;
    o->hi[cls] = hi;
}

static uint8_t trust_too_low(const PeerObserver_t *o)
{
    /* (alpha+1)/(alpha+beta+2) < 1/2 */
    return (uint8_t)(2u * ((uint16_t)o->rep_alpha + 1u) <
        (uint16_t)o->rep_alpha + (uint16_t)o->rep_beta + 2u);
}

static void compute_mobility_vote(PeerObserver_t *o, uint32_t now)
{
    uint8_t n = o->stall_samples;
    uint8_t k = popcount8(o->stall_history);
    float lo, hi;
    PeerVerdict_t verdict = PEER_VERDICT_ABSTAIN;
    uint32_t age = o->has_last ? (uint32_t)((uint16_t)(now - o->last_seq)) : 0xFFFFu;

    consensus_severity_interval(n, k, RECOVERY_MANAGER_MOBILITY_STALL_FULL_SCALE,
        &lo, &hi);

    if(!trust_too_low(o) && age <= CONSENSUS_MOB_MAX_AGE_TICKS)
    {
        if(n >= WATCHDOG_STALL_MIN_SAMPLES && k >= WATCHDOG_STALL_CONFIRM_COUNT)
            verdict = PEER_VERDICT_FAULT;
        else if(n == WATCHDOG_STALL_WINDOW_TICKS && o->stall_history == 0u)
            verdict = PEER_VERDICT_CLEAR;
    }

    set_vote(o, CONSENSUS_CLASS_MOBILITY, verdict, n, lo, hi);
}

static void compute_comm_vote(PeerObserver_t *o, uint8_t self_deaf)
{
    uint8_t n = o->comm_window_n;
    uint8_t mask = (uint8_t)((1u << CONSENSUS_COMM_WINDOW_TICKS) - 1u);
    uint8_t k = popcount8((uint8_t)(o->comm_miss_bits & mask));
    float lo, hi;
    PeerVerdict_t verdict = PEER_VERDICT_ABSTAIN;

    consensus_severity_interval(n, k, CONSENSUS_COMM_MISS_FULL_SCALE, &lo, &hi);

    /* A tick on which the observer heard nobody is not evidence about the
       target (hard self-deaf rule), so it abstains on it too. */
    if(!self_deaf && n == CONSENSUS_COMM_WINDOW_TICKS)
    {
        if(k >= CONSENSUS_COMM_FAULT_MISSES)
            verdict = PEER_VERDICT_FAULT;
        else if(k <= CONSENSUS_COMM_CLEAR_MAX_MISSES)
            verdict = PEER_VERDICT_CLEAR;
    }

    set_vote(o, CONSENSUS_CLASS_COMMUNICATION, verdict, n, lo, hi);
}

static void update_observer(uint8_t p, uint32_t now)
{
    uint8_t heard = 0u;
    uint8_t expect = (uint8_t)(g_have_prev_step && g_prev_step_tick + 1u == now);
    uint8_t accused = 0u;
    uint16_t bits = 0u;

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        if(t == p)
            continue;
        if(g_obs[p][t].on_time)
        {
            heard++;
            bits = (uint16_t)(bits | (1u << t));
        }
    }

    /* Hard self-deaf rule: nobody heard on time for tick now-1, so the
       problem is this node's own radio and the tick counts for no window. */
    g_self_deaf[p] = (uint8_t)(expect && heard == 0u);
    g_heard_count[p] = heard;
    g_heard_bits[p] = expect ? bits : 0u;

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        PeerObserver_t *o = &g_obs[p][t];

        if(t == p)
            continue;

        if(expect && !g_self_deaf[p])
        {
            uint8_t miss = (uint8_t)!o->on_time;

            o->comm_miss_bits = (uint8_t)(((o->comm_miss_bits << 1) | miss) &
                ((1u << CONSENSUS_COMM_WINDOW_TICKS) - 1u));
            if(o->comm_window_n < CONSENSUS_COMM_WINDOW_TICKS)
                o->comm_window_n++;
            /* Ground truth for the log: it left the transmitter but not the
               channel copy for this observer. */
            o->dropped_gt = (uint8_t)(o->sent_last_tick && !o->on_time);
        }

        compute_mobility_vote(o, now);
        compute_comm_vote(o, g_self_deaf[p]);

        if(o->vote[CONSENSUS_CLASS_COMMUNICATION].verdict == PEER_VERDICT_FAULT)
            accused++;
    }

    /* Graded self-deaf rule: a receiver that would accuse more than
       CONSENSUS_COMM_MAX_ACCUSED targets at once is far more likely the
       degraded one than they are. */
    if(accused > CONSENSUS_COMM_MAX_ACCUSED)
    {
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            PeerObserver_t *o = &g_obs[p][t];

            if(t == p)
                continue;
            o->graded = 1u;
            if(o->vote[CONSENSUS_CLASS_COMMUNICATION].verdict == PEER_VERDICT_FAULT)
                o->vote[CONSENSUS_CLASS_COMMUNICATION].verdict = PEER_VERDICT_ABSTAIN;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Beacon emission (design 1.1, 1.4)                                  */
/* ------------------------------------------------------------------ */

static void send_beacon(uint8_t n, uint32_t now)
{
    const TeamState_t *team = &g_teams[n];
    ConsensusBeacon_t b;
    uint8_t buf[CONSENSUS_BEACON_SIZE];
    uint8_t len;

    memset(&b, 0, sizeof(b));
    b.version = CONSENSUS_BEACON_VERSION;
    b.team_id = n;
    b.seq = (uint16_t)now;
    b.status = (uint8_t)team->status;
    b.loc_x = (uint8_t)team->loc_x;
    b.loc_y = (uint8_t)team->loc_y;
    b.task_pool = (uint8_t)team->assigned_task_pool;
    b.task_slot = (uint8_t)team->assigned_task_slot;
    b.leg_flags = (uint8_t)((team->heading_to_rest ? 1u : 0u) |
        (team->heading_to_rest ? (uint8_t)(team->resting_zone_id << 1) : 0u));
    b.heard_bits = g_heard_bits[n];

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        if(t == n)
            continue;
        for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
            b.votes[c][t] = g_obs[n][t].vote[c];
    }

#if CONSENSUS_TEST_HOOKS
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        if(t == n)
            continue;
        for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
        {
            if(!g_force_verdict_on[n][t][c])
                continue;
            b.votes[c][t].verdict = g_force_verdict_val[n][t][c];
            b.votes[c][t].n_samples = 6u;
            b.votes[c][t].lo_q = (b.votes[c][t].verdict == PEER_VERDICT_FAULT) ? 15u : 0u;
            b.votes[c][t].hi_q = (b.votes[c][t].verdict == PEER_VERDICT_FAULT) ? 15u : 0u;
        }
        if(g_force_heard[n][t] == 1u)
            b.heard_bits = (uint16_t)(b.heard_bits | (1u << t));
        else if(g_force_heard[n][t] == 2u)
            b.heard_bits = (uint16_t)(b.heard_bits & ~(1u << t));
    }
#endif

    len = consensus_beacon_pack(&b, buf);
    if(vbus_send(n, VBUS_BROADCAST, CONSENSUS_FRAME_BEACON, buf, len) == 0)
    {
        g_sent_this_tick[n] = 1u;
        g_stats.beacons_sent++;
    }
}

/* ------------------------------------------------------------------ */
/* Communication fast path (design 2.6)                               */
/* ------------------------------------------------------------------ */

#if CONSENSUS_FAST_PATH
static void evaluate_fast_path(uint32_t now)
{
    uint32_t k;

    if(now < 2u)
        return;
    k = now - 2u;    /* bitmaps about tick k arrive in beacons for tick k+1 */

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        Fuser_t *f = &g_fuser[t];
        uint8_t reporters = 0u;
        uint8_t heard_t = 0u;
        uint8_t slot = (uint8_t)(k & 3u);
        uint8_t silent;

        for(uint8_t p = 0; p < NUM_TEAMS; p++)
        {
            uint16_t bits;
            uint16_t others;

            if(p == t || f->hm_tick_plus1[p][slot] != k + 1u)
                continue;

            bits = f->hm_bits[p][slot];
            /* A row that heard fewer than CONSENSUS_REPORTER_MIN_HEARD of the
               other peers is the reporter's own radio, not the target's. */
            others = (uint16_t)(bits & ~(1u << t) & ~(1u << p));
            if(popcount16(others) < CONSENSUS_REPORTER_MIN_HEARD)
                continue;

            reporters++;
            if(bits & (1u << t))
                heard_t++;
        }

        silent = (uint8_t)(reporters >= CONSENSUS_FAST_MIN_REPORTERS && heard_t == 0u);
        f->n_reporters = reporters;
        f->n_heard = heard_t;

        if(silent)
        {
            if(f->silent_valid && f->silent_k + 1u == k)
            {
                uint32_t round = now / CONSENSUS_ROUND_TICKS;

                /* Two consecutive silent ticks: hold for this round and the
                   next. */
                f->fast_hold_until = (round + 2u) * CONSENSUS_ROUND_TICKS - 1u;
                g_stats.fast_path_fires++;
            }
            f->silent_valid = 1u;
            f->silent_k = k;
        }
        else
        {
            f->silent_valid = 0u;
        }
    }
}
#endif

/* ------------------------------------------------------------------ */
/* Round fusion (design 3.2, 3.3, 3.4)                                */
/* ------------------------------------------------------------------ */

static void fuse_round(uint8_t t, uint32_t now)
{
    Fuser_t *f = &g_fuser[t];
    uint8_t n_live = 0u;

    for(uint8_t p = 0; p < NUM_TEAMS; p++)
    {
        if(p != t && f->got[p])
            n_live++;
    }

    if(n_live == 0u)
    {
        /* Heard nobody all round: hold the last result for one round, then
           publish NONE (design 3.4). */
        if(f->dark_rounds < 1u)
        {
            f->dark_rounds++;
        }
        else
        {
            for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
            {
                memset(&f->vote_result[c], 0, sizeof(f->vote_result[c]));
                f->vote_result[c].n_dark = (uint8_t)(NUM_TEAMS - 1u);
                f->vote_result[c].round = now / CONSENSUS_ROUND_TICKS;
            }
        }
    }
    else
    {
        f->dark_rounds = 0u;

        for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
        {
            uint8_t verdict[NUM_TEAMS];
            uint8_t domain[NUM_TEAMS];
            float lo[NUM_TEAMS];
            uint8_t count = 0u;

            for(uint8_t p = 0; p < NUM_TEAMS; p++)
            {
                const ConsensusVote_t *v;

                if(p == t || !f->got[p])
                    continue;

                v = &f->vote[p][c];
                verdict[count] = f->vote_has[p] ? v->verdict : (uint8_t)PEER_VERDICT_ABSTAIN;
                lo[count] = (float)v->lo_q / 15.0f;
                domain[count] = consensus_domain_of(p);
                count++;
            }

            f->vote_result[c] = consensus_gate_compute(n_live, verdict, lo, domain,
                count, 0u);
            f->vote_result[c].round = now / CONSENSUS_ROUND_TICKS;
        }
    }

    /* The round set only ever grows during a round and is cleared here, so
       a voter that goes dark after voting cannot be un-counted mid-round. */
    memset(f->got, 0, sizeof(f->got));
    memset(f->vote_has, 0, sizeof(f->vote_has));
}

static void publish_results(uint32_t now)
{
#if !CONSENSUS_FAST_PATH
    (void)now;
#endif
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        Fuser_t *f = &g_fuser[t];

#if CONSENSUS_FAST_PATH
        f->fast_active = (uint8_t)(f->fast_hold_until != 0u && now <= f->fast_hold_until);
#else
        f->fast_active = 0u;
#endif

        for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
        {
            ConsensusResult_t r = f->vote_result[c];

            if(c == CONSENSUS_CLASS_COMMUNICATION && f->fast_active)
            {
                r.tier = CONSENSUS_TIER_ELEVATED;
                r.fast_path = 1u;
            }
            r.n_reporters = f->n_reporters;
            r.n_heard_target = f->n_heard;
            g_result[t][c] = r;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Per-tick driver (design 3.1)                                       */
/* ------------------------------------------------------------------ */

void consensus_step(uint32_t now)
{
    uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
    uint8_t src, frame_type, len;

    /* 1. deliver everything sent at now-1. */
    vbus_step(now);

    for(uint8_t p = 0; p < NUM_TEAMS; p++)
    {
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            PeerObserver_t *o = &g_obs[p][t];

            o->on_time = o->late = o->stale = 0u;
            o->plaus_fail = o->leg_changed = o->mob_sampled = o->mob_stalled = 0u;
            o->graded = 0u;
            o->sent_last_tick = g_sent_this_tick[t];
            o->dropped_gt = 0u;
        }
    }

    /* 2. drain every inbox until empty (vbus.h notes). */
    for(uint8_t n = 0; n < NUM_TEAMS; n++)
    {
        while(vbus_poll(n, &src, &frame_type, payload, &len))
        {
            if(frame_type == CONSENSUS_FRAME_BEACON)
                handle_beacon(n, src, payload, len, now);
        }
    }

    /* 3. verdicts about every t != n. */
    memset(g_sent_this_tick, 0, sizeof(g_sent_this_tick));
    for(uint8_t n = 0; n < NUM_TEAMS; n++)
        update_observer(n, now);

    /* 4. beacons, only from nodes whose radio actually transmitted. */
    for(uint8_t n = 0; n < NUM_TEAMS; n++)
    {
        if(team_radio_emitted(&g_teams[n]))
            send_beacon(n, now);
    }

#if CONSENSUS_FAST_PATH
    /* 5b. every tick, so the round length adds no latency to it. */
    evaluate_fast_path(now);
#endif

    /* 5. close the round. */
    if((now % CONSENSUS_ROUND_TICKS) == CONSENSUS_ROUND_TICKS - 1u)
    {
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
            fuse_round(t, now);
    }

    publish_results(now);

    g_have_prev_step = 1u;
    g_prev_step_tick = now;
}

/* ------------------------------------------------------------------ */
/* Log view (design 5.2)                                              */
/* ------------------------------------------------------------------ */

void consensus_get_peer_observation(uint8_t observer, uint8_t target,
    PeerObservation_t *out)
{
    const PeerObserver_t *o;

    memset(out, 0, sizeof(*out));
    if(observer >= NUM_TEAMS || target >= NUM_TEAMS || observer == target)
        return;

    o = &g_obs[observer][target];

    out->rx_on_time = o->on_time;
    out->rx_late = o->late;
    out->rx_stale = o->stale;
    if(o->has_last)
    {
        uint32_t age = (uint32_t)((uint16_t)(g_prev_step_tick - o->last_seq));

        out->rx_age_ticks = (uint8_t)(age > 255u ? 255u : age);
    }
    else
    {
        out->rx_age_ticks = 255u;
    }
    out->observer_self_deaf = g_self_deaf[observer];
    out->comm_window_n = o->comm_window_n;
    out->comm_misses = popcount8(o->comm_miss_bits);
    out->rep_status = o->last_status;
    out->rep_loc_x = o->last_x;
    out->rep_loc_y = o->last_y;
    out->rep_task_pool = o->last_pool;
    out->rep_task_slot = o->last_slot;
    out->rep_heading_to_rest = (uint8_t)(o->last_leg_flags & 1u);
    out->rep_resting_zone_id = (uint8_t)(o->last_leg_flags >> 1);
    out->leg_changed = o->leg_changed;
    out->mob_sampled = o->mob_sampled;
    out->mob_stalled = o->mob_stalled;
    out->mob_window_n = o->stall_samples;
    out->mob_stalls = popcount8(o->stall_history);
    out->mob_history = o->stall_history;
    for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
    {
        out->vote[c] = o->vote[c];
        out->lo[c] = o->lo[c];
        out->hi[c] = o->hi[c];
    }
    out->observer_graded_abstain = o->graded;
    out->observer_heard_count = g_heard_count[observer];
    out->reported_heard_target = (uint8_t)((g_heard_bits[observer] >> target) & 1u);
    out->plaus_fail = o->plaus_fail;
    out->rep_alpha = o->rep_alpha;
    out->rep_beta = o->rep_beta;
    out->gt_link_dropped = o->dropped_gt;
    out->gt_loss_model = g_bus_cfg.loss_model;
}

/* ------------------------------------------------------------------ */
/* Test hooks                                                         */
/* ------------------------------------------------------------------ */

#if CONSENSUS_TEST_HOOKS
void consensus_test_force_verdict(uint8_t voter, uint8_t target, uint8_t cls,
    PeerVerdict_t verdict)
{
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        if(target != CONSENSUS_ANY && target != t)
            continue;
        for(uint8_t c = 0; c < CONSENSUS_NUM_VOTED_CLASSES; c++)
        {
            if(cls != CONSENSUS_ANY && cls != c)
                continue;
            if(voter < NUM_TEAMS)
            {
                g_force_verdict_on[voter][t][c] = 1u;
                g_force_verdict_val[voter][t][c] = (uint8_t)verdict;
            }
        }
    }
}

void consensus_test_force_heard_bit(uint8_t reporter, uint8_t target, uint8_t claim)
{
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        if(target != CONSENSUS_ANY && target != t)
            continue;
        if(reporter < NUM_TEAMS)
            g_force_heard[reporter][t] = claim ? 1u : 2u;
    }
}

void consensus_test_clear_forced(void)
{
    memset(g_force_verdict_on, 0, sizeof(g_force_verdict_on));
    memset(g_force_verdict_val, 0, sizeof(g_force_verdict_val));
    memset(g_force_heard, 0, sizeof(g_force_heard));
}
#endif

#else
/* Multi-board builds: nothing. Keeps the translation unit non-empty. */
typedef int consensus_disabled_translation_unit_t;
#endif /* VIRTUAL_BUS_ENABLED */

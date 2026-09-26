#include "vbus.h"

#if VIRTUAL_BUS_ENABLED

#include "lora_frame.h"

typedef struct {
    uint8_t  src;
    uint8_t  dst;
    uint8_t  frame_type;
    uint8_t  len;
    uint32_t due_step;
    uint8_t  payload[LORA_FRAME_MAX_PAYLOAD];
} VbusFrame_t;

typedef struct {
    VbusFrame_t frames[VBUS_INBOX_DEPTH];
    uint8_t head, count;
} VbusInbox_t;

static uint8_t          g_num_nodes;
static VbusConfig_t     g_cfg;
static uint32_t         g_rng;
static uint32_t         g_steps;            /* completed vbus_step() calls */
static VbusFrame_t      g_pending[VBUS_PENDING_MAX];
static uint16_t         g_pending_count;
static VbusInbox_t      g_inbox[VBUS_MAX_NODES];
static uint8_t          g_dark[VBUS_MAX_NODES];
static float            g_rx_loss[VBUS_MAX_NODES];
static uint8_t          g_link_bad[VBUS_MAX_NODES][VBUS_MAX_NODES];
static uint8_t          g_burst;
static uint32_t         g_src_draw_step[VBUS_MAX_NODES]; /* step+1 of last draw */
static uint8_t          g_src_lost[VBUS_MAX_NODES];
static uint64_t         g_airtime_us;
static VbusStats_t      g_stats;

/* splitmix32-style generator: any seed (including 0) is fine. */
static uint32_t vbus_rand_u32(void)
{
    uint32_t z = (g_rng += 0x9E3779B9u);
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    return z ^ (z >> 16);
}

static float clamp01(float p)
{
    if (!(p > 0.0f)) return 0.0f;   /* also catches NaN */
    if (p > 1.0f) return 1.0f;
    return p;
}

/* True with probability p. Consumes no RNG for p <= 0 or p >= 1. */
static uint8_t vbus_chance(float p)
{
    if (p <= 0.0f) return 0u;
    if (p >= 1.0f) return 1u;
    return (float)(vbus_rand_u32() >> 8) * (1.0f / 16777216.0f) < p ? 1u : 0u;
}

uint32_t lora_time_on_air_ms_x1000(uint32_t wire_len)
{
    /* SF7, BW125: Tsym = 1024 us. CRC on, explicit header, LDRO off:
       payloadSymbNb = 8 + ceil((8*PL - 4*7 + 28 + 16) / 28) * 5.
       Preamble is 8 + 4.25 symbols = 49 quarter-symbols. Everything in
       quarter-symbols (256 us) so the result is exact. */
    uint32_t n = (8u * wire_len + 16u + 27u) / 28u;
    uint32_t payload_syms = 8u + n * 5u;
    return (49u + 4u * payload_syms) * 256u;
}

void vbus_init(uint8_t num_nodes, const VbusConfig_t *cfg)
{
    uint32_t i, j;
    g_num_nodes = num_nodes > VBUS_MAX_NODES ? (uint8_t)VBUS_MAX_NODES : num_nodes;
    if (cfg != 0) {
        g_cfg = *cfg;
    } else {
        g_cfg.delay_ticks = 0u;
        g_cfg.loss_model = VBUS_LOSS_IID;
        g_cfg.link_loss = g_cfg.p_gb = g_cfg.p_bg = g_cfg.loss_bad = 0.0f;
        g_cfg.seed = 0u;
    }
    if (g_cfg.delay_ticks > 1u) g_cfg.delay_ticks = 1u;
    g_cfg.link_loss = clamp01(g_cfg.link_loss);
    g_cfg.p_gb = clamp01(g_cfg.p_gb);
    g_cfg.p_bg = clamp01(g_cfg.p_bg);
    g_cfg.loss_bad = clamp01(g_cfg.loss_bad);
    g_rng = g_cfg.seed;
    g_steps = 0u;
    g_pending_count = 0u;
    g_burst = 0u;
    g_airtime_us = 0u;
    g_stats.sent = g_stats.delivered = g_stats.dropped = g_stats.airtime_ms_total = 0u;
    for (i = 0u; i < VBUS_MAX_NODES; i++) {
        g_inbox[i].head = g_inbox[i].count = 0u;
        g_dark[i] = 0u;
        g_rx_loss[i] = 0.0f;
        g_src_draw_step[i] = 0u;
        g_src_lost[i] = 0u;
        for (j = 0u; j < VBUS_MAX_NODES; j++) g_link_bad[i][j] = 0u;
    }
}

int vbus_send(uint8_t src, uint8_t dst, uint8_t frame_type, const uint8_t *payload, uint8_t len)
{
    VbusFrame_t *f;
    uint32_t i;

    if (src >= g_num_nodes) return -1;
    if (dst != VBUS_BROADCAST && (dst >= g_num_nodes || dst == src)) return -1;
    if (len > LORA_FRAME_MAX_PAYLOAD || (len > 0u && payload == 0)) return -1;
    if (g_dark[src]) return -1;
    if (g_pending_count >= VBUS_PENDING_MAX) {
        g_stats.dropped++;
        return -1;
    }

    f = &g_pending[g_pending_count++];
    f->src = src;
    f->dst = dst;
    f->frame_type = frame_type;
    f->len = len;
    /* Sent after step k-1 (g_steps == k-1): the next step is k. Delay 0
       delivers at step k, delay 1 at step k+1. */
    f->due_step = g_steps + 1u + g_cfg.delay_ticks;
    for (i = 0u; i < len; i++) f->payload[i] = payload[i];

    g_stats.sent++;
    g_airtime_us += lora_time_on_air_ms_x1000((uint32_t)len + LORA_FRAME_HEADER_SIZE + LORA_FRAME_TRAILER_SIZE);
    g_stats.airtime_ms_total = (uint32_t)(g_airtime_us / 1000u);
    return 0;
}

static uint8_t link_lost(uint8_t src, uint8_t dst)
{
    switch (g_cfg.loss_model) {
    case VBUS_LOSS_PER_SOURCE:
        return g_src_lost[src];
    case VBUS_LOSS_GILBERT_ELLIOTT: {
        uint8_t *bad = &g_link_bad[src][dst];
        if (*bad) { if (vbus_chance(g_cfg.p_bg)) *bad = 0u; }
        else      { if (vbus_chance(g_cfg.p_gb)) *bad = 1u; }
        return vbus_chance(*bad ? g_cfg.loss_bad : g_cfg.link_loss);
    }
    case VBUS_LOSS_SHARED_BURST:
        return g_burst ? 1u : vbus_chance(g_cfg.link_loss);
    case VBUS_LOSS_IID:
    default:
        return vbus_chance(g_cfg.link_loss);
    }
}

static void deliver_copy(const VbusFrame_t *f, uint8_t dst)
{
    VbusInbox_t *in;
    VbusFrame_t *slot;
    uint32_t i;

    if (g_dark[dst] || link_lost(f->src, dst) || vbus_chance(g_rx_loss[dst])) {
        g_stats.dropped++;
        return;
    }
    in = &g_inbox[dst];
    if (in->count >= VBUS_INBOX_DEPTH) {
        g_stats.dropped++;          /* overflow: newest dropped */
        return;
    }
    slot = &in->frames[(in->head + in->count) % VBUS_INBOX_DEPTH];
    slot->src = f->src;
    slot->dst = f->dst;
    slot->frame_type = f->frame_type;
    slot->len = f->len;
    for (i = 0u; i < f->len; i++) slot->payload[i] = f->payload[i];
    in->count++;
    g_stats.delivered++;
}

void vbus_step(uint32_t now_tick)
{
    uint32_t src, i, kept;
    (void)now_tick;

    g_steps++;

    if (g_cfg.loss_model == VBUS_LOSS_SHARED_BURST) {
        if (g_burst) { if (vbus_chance(g_cfg.p_bg)) g_burst = 0u; }
        else         { if (vbus_chance(g_cfg.p_gb)) g_burst = 1u; }
    }

    for (src = 0u; src < g_num_nodes; src++) {
        for (i = 0u; i < g_pending_count; i++) {
            const VbusFrame_t *f = &g_pending[i];
            if (f->src != src || f->due_step > g_steps) continue;

            if (g_cfg.loss_model == VBUS_LOSS_PER_SOURCE && g_src_draw_step[src] != g_steps) {
                g_src_draw_step[src] = g_steps;
                g_src_lost[src] = vbus_chance(g_cfg.link_loss);
            }
            if (f->dst == VBUS_BROADCAST) {
                uint32_t d;
                for (d = 0u; d < g_num_nodes; d++)
                    if (d != src) deliver_copy(f, (uint8_t)d);
            } else {
                deliver_copy(f, f->dst);
            }
        }
    }

    /* Compact: keep frames not yet due, preserving send order. */
    kept = 0u;
    for (i = 0u; i < g_pending_count; i++) {
        if (g_pending[i].due_step > g_steps) {
            if (kept != i) g_pending[kept] = g_pending[i];
            kept++;
        }
    }
    g_pending_count = (uint16_t)kept;
}

int vbus_poll(uint8_t node, uint8_t *out_src, uint8_t *out_frame_type,
              uint8_t *out_payload, uint8_t *out_len)
{
    VbusInbox_t *in;
    const VbusFrame_t *f;
    uint32_t i;

    if (node >= g_num_nodes) return 0;
    in = &g_inbox[node];
    if (in->count == 0u) return 0;
    f = &in->frames[in->head];
    if (out_src) *out_src = f->src;
    if (out_frame_type) *out_frame_type = f->frame_type;
    if (out_len) *out_len = f->len;
    if (out_payload) for (i = 0u; i < f->len; i++) out_payload[i] = f->payload[i];
    in->head = (uint8_t)((in->head + 1u) % VBUS_INBOX_DEPTH);
    in->count--;
    return 1;
}

void vbus_set_node_dark(uint8_t node, uint8_t dark)
{
    if (node < g_num_nodes) g_dark[node] = dark ? 1u : 0u;
}

void vbus_set_node_rx_loss(uint8_t node, float extra_loss)
{
    if (node < g_num_nodes) g_rx_loss[node] = clamp01(extra_loss);
}

VbusStats_t vbus_get_stats(void)
{
    return g_stats;
}

#else
/* Multi-board builds: nothing. Keeps the translation unit non-empty. */
typedef int vbus_disabled_translation_unit_t;
#endif

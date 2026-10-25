#include <math.h>
#include <string.h>

#include "road_knowledge.h"
#include "environment.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "headroom_probes.h"

RoadKnowledgeCfg_t g_road_know = { 0u, 1u, 2u, 0u, 0.05f };   /* radius 1 cell, 2-tick report latency, 5% loss (consensus D4) */

#if ENVIRONMENT_HAS_WORLD_STORAGE

#define RK_CELLS (GRID_WIDTH * GRID_HEIGHT)
#define RK_EXPIRY_TICKS 2400u   /* the longest an unattended damage lasts */
#define RK_NONE 0xFFu

static uint8_t s_belief[GRID_WIDTH][GRID_HEIGHT];        /* believed kind: 0 clear, 1 partial, 2 blocked */
static uint32_t s_belief_tick[GRID_WIDTH][GRID_HEIGHT];
static uint8_t s_pend_kind[GRID_WIDTH][GRID_HEIGHT];     /* RK_NONE: nothing pending */
static uint32_t s_pend_due[GRID_WIDTH][GRID_HEIGHT];
static uint8_t s_prev_truth[GRID_WIDTH][GRID_HEIGHT];
static uint32_t s_onset[GRID_WIDTH][GRID_HEIGHT];
static uint8_t s_onset_known[GRID_WIDTH][GRID_HEIGHT];
static uint8_t s_prior_add;
static RoadKnowledgeStats_t s_stats;

static uint8_t belief_kind(int x, int y)
{
    uint8_t k = s_belief[x][y];

    if(k != 0u && g_current_tick > s_belief_tick[x][y] + RK_EXPIRY_TICKS)
        return 0u;
    return k;
}

static uint8_t truth_kind(int x, int y)
{
    return fault_injector_causal_road_kind(x, y);
}

void road_knowledge_reset(void)
{
    memset(s_belief, 0, sizeof(s_belief));
    memset(s_belief_tick, 0, sizeof(s_belief_tick));
    memset(s_pend_kind, RK_NONE, sizeof(s_pend_kind));
    memset(s_pend_due, 0, sizeof(s_pend_due));
    memset(s_prev_truth, 0, sizeof(s_prev_truth));
    memset(s_onset, 0, sizeof(s_onset));
    memset(s_onset_known, 0, sizeof(s_onset_known));
    memset(&s_stats, 0, sizeof(s_stats));
    s_prior_add = 0u;
}

static void set_belief(int x, int y, uint8_t kind, uint32_t tick)
{
    s_belief[x][y] = kind;
    s_belief_tick[x][y] = tick;
    if(kind != 0u && kind == truth_kind(x, y) && s_onset[x][y] != 0u && !s_onset_known[x][y])
    {
        uint32_t d = tick > s_onset[x][y] ? tick - s_onset[x][y] : 0u;

        s_onset_known[x][y] = 1u;
        s_stats.known_episodes++;
        s_stats.known_delay_sum += d;
        if(d <= 2u)
            s_stats.known_within_2++;
        if(d > s_stats.known_delay_max)
            s_stats.known_delay_max = d;
    }
}

/* A team at (tx, ty) sees cell (x, y). */
static void observe(const TeamState_t *team, int x, int y, uint32_t tick)
{
    uint8_t tk = truth_kind(x, y);

    if(tk == belief_kind(x, y) || s_pend_kind[x][y] == tk)
        return;
    s_stats.reports_made++;
    if(!g_road_know.instant)
    {
        if(team->fault_state.active[FAULT_CLASS_COMMUNICATION] ||
           probe_unit(tick, (uint32_t)team->team_id * 2477u + (uint32_t)(x * GRID_HEIGHT + y), 31u) < g_road_know.loss)
        {
            s_stats.reports_lost++;
            return;
        }
    }
    if(g_road_know.instant || g_road_know.latency == 0u)
    {
        set_belief(x, y, tk, tick);
        return;
    }
    s_pend_kind[x][y] = tk;
    s_pend_due[x][y] = tick + g_road_know.latency;
}

/* R2 prior: probability that a given cell carries damage, from the Omori rate of aftershock events over the last
 * 1500 ticks (the longest damage lasts about 2400, mean about 1500), one damaged cell per event spread over the
 * whole grid. Planned as pi x 12 (the partial-debris cost), rounded to whole cost units. */
static void update_prior(uint32_t tick)
{
    const CausalFaultParams_t *k = &g_causal_params;
    float sum = 0.0f, pi, add;

    if(g_road_know.mode != 2u || (tick % 50u) != 0u)
        return;
    for(uint32_t s = (tick > 1500u ? tick - 1500u : 0u); s <= tick; s += 10u)
        sum += 10.0f * k->omori_k * g_fault_stress / powf(k->omori_c_ticks + (float)s + k->omori_t_offset_ticks, k->omori_p);
    pi = sum / (float)RK_CELLS;
    if(pi > 1.0f)
        pi = 1.0f;
    add = pi * (float)((uint16_t)g_causal_roads.crossing_delay_ticks * g_causal_roads.cost_per_tick);
    s_prior_add = (uint8_t)(add + 0.5f);
    if(s_prior_add > s_stats.prior_add_max)
        s_stats.prior_add_max = s_prior_add;
}

void road_knowledge_step(void)
{
    uint32_t tick = g_current_tick + 1u;

    if(g_road_know.mode == 0u)
        return;
    update_prior(tick);

    /* Truth bookkeeping for the diagnostics, and delivery of due reports. */
    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            uint8_t tk = truth_kind(x, y);

            if(tk != 0u && s_prev_truth[x][y] == 0u)
            {
                s_stats.damage_episodes++;
                s_onset[x][y] = tick;
                s_onset_known[x][y] = 0u;
            }
            s_prev_truth[x][y] = tk;
            if(s_pend_kind[x][y] != RK_NONE && tick >= s_pend_due[x][y])
            {
                set_belief(x, y, s_pend_kind[x][y], tick);
                s_pend_kind[x][y] = RK_NONE;
            }
            if(tk == 0u && belief_kind(x, y) != 0u)
                s_stats.stale_cell_ticks++;
        }
    }

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        const TeamState_t *team = &g_teams[t];
        int r = g_road_know.radius;

        for(int dx = -r; dx <= r; dx++)
            for(int dy = -r; dy <= r; dy++)
            {
                int x = (int)team->loc_x + dx, y = (int)team->loc_y + dy;

                if(x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT)
                    observe(team, x, y, tick);
            }
    }
}

int road_knowledge_planning_view(int x, int y, int *navigable, int *cost)
{
    const EnvironmentCell_t *cell;
    uint8_t tk, bk, base_cost, base_nav;
    uint16_t c;

    if(g_road_know.mode == 0u || x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT)
        return 0;
    tk = truth_kind(x, y);
    bk = belief_kind(x, y);
    if(tk == 0u && bk == 0u && s_prior_add == 0u)
        return 0;
    if(tk == bk && s_prior_add == 0u)
        return 0;
    cell = &g_environment[x][y];
    if(tk != 0u)
        fault_injector_causal_road_base(x, y, &base_cost, &base_nav);
    else
    {
        base_cost = cell->travel_cost;
        base_nav = cell->navigable;
    }
    if(bk == 2u)
    {
        *navigable = 0;
        *cost = base_cost;
        return 1;
    }
    c = base_cost;
    if(bk == 1u)
        c = (uint16_t)(c + (g_causal_roads.route_cost_visible
                                ? (uint16_t)((uint16_t)g_causal_roads.crossing_delay_ticks * g_causal_roads.cost_per_tick)
                                : 3u));
    else
        c = (uint16_t)(c + s_prior_add);   /* unknown cell: the prior (0 outside mode 2) */
    *navigable = base_nav ? 1 : 0;
    *cost = c > 255u ? 255 : (int)c;
    return 1;
}

int road_knowledge_step_blocked(const TeamState_t *team, int nx, int ny)
{
    int x = team->loc_x, y = team->loc_y;
    uint32_t tick = g_current_tick;
    int stop = 0;

    if(g_road_know.mode == 0u || nx < 0 || nx >= GRID_WIDTH || ny < 0 || ny >= GRID_HEIGHT)
        return 0;
    if(truth_kind(nx, ny) == 2u)
    {
        observe(team, nx, ny, tick);
        stop = 1;
    }
    else if(nx != x && ny != y)
    {
        /* A diagonal step cuts a corner: refused if either orthogonal neighbour is truly blocked. */
        if(truth_kind(nx, y) == 2u) { observe(team, nx, y, tick); stop = 1; }
        if(truth_kind(x, ny) == 2u) { observe(team, x, ny, tick); stop = 1; }
    }
    if(stop)
    {
        s_stats.blocked_stops++;
        return 1;
    }
    if(truth_kind(nx, ny) == 1u && belief_kind(nx, ny) == 0u)
    {
        s_stats.unknown_partial_enter++;
        observe(team, nx, ny, tick);
    }
    return 0;
}

int road_knowledge_believed_damaged(int x, int y)
{
    if(x < 0 || x >= GRID_WIDTH || y < 0 || y >= GRID_HEIGHT)
        return 0;
    return belief_kind(x, y) != 0u;
}

const RoadKnowledgeStats_t *road_knowledge_stats(void)
{
    return &s_stats;
}

#else /* board: no world storage, the planning view is always the true map */

void road_knowledge_reset(void) {}
void road_knowledge_step(void) {}
int road_knowledge_planning_view(int x, int y, int *navigable, int *cost) { (void)x; (void)y; (void)navigable; (void)cost; return 0; }
int road_knowledge_step_blocked(const TeamState_t *team, int next_x, int next_y) { (void)team; (void)next_x; (void)next_y; return 0; }
int road_knowledge_believed_damaged(int x, int y) { (void)x; (void)y; return 0; }
const RoadKnowledgeStats_t *road_knowledge_stats(void) { static RoadKnowledgeStats_t z; return &z; }

#endif

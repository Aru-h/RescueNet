#include <math.h>
#include <string.h>

#include "headroom_probes.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "incident.h"
#include "path.h"
#include "allocator.h"
#include "scoring.h"
#include "recovery_manager_config.h"
#include "cbba.h"

HeadroomProbes_t g_probe;
HeadroomProbeStats_t g_probe_stats;
ProbeIncRecord_t g_probe_inc_log[PROBE_INC_LOG_MAX];
uint32_t g_probe_inc_log_n;
static uint32_t s_arrived_id[NUM_INCIDENTS];
static uint32_t s_arrived_tick[NUM_INCIDENTS];
ProbeEvent_t g_probe_ev_seen[PROBE_MAX_EVENTS];
uint32_t g_probe_ev_seen_n;
ProbeEvent_t g_probe_ev_plan[PROBE_MAX_EVENTS];
uint32_t g_probe_ev_plan_n;
uint32_t g_probe_ev_mismatch;

#define PROBE_HIST 64u
#define PROBE_URGENCY_WINDOW_TICKS 240u
#define PROBE_URGENCY_DIVISOR 3.0f

static uint8_t s_offline[NUM_TEAMS];
static uint32_t s_busy_until[NUM_TEAMS];     /* RR2a: support team committed until this tick */
static uint8_t s_support_repair[NUM_TEAMS];   /* RR2a: this faulted team is being repaired by a support team */
static uint32_t s_stage_id[NUM_INCIDENTS];
static uint8_t s_stage_n[NUM_INCIDENTS];
static uint8_t s_perim[GRID_WIDTH * GRID_HEIGHT];
static uint8_t s_hist[NUM_TEAMS][3][PROBE_HIST];     /* true fault-active flags, indexed by tick % PROBE_HIST */
static float s_urg[NUM_INCIDENTS];
static uint32_t s_urg_id[NUM_INCIDENTS];
static uint32_t s_urg_tick[NUM_INCIDENTS];

uint32_t probe_hash(uint32_t tick, uint32_t a, uint32_t salt)
{
    uint32_t v = g_world_seed ^ (tick * 2654435761u) ^ ((a + 1u) * 40503u * 65537u) ^ ((salt + 7u) * 2246822519u);

    v ^= v >> 15;
    v *= 2246822519u;
    v ^= v >> 13;
    v *= 3266489917u;
    v ^= v >> 16;
    return v;
}

float probe_unit(uint32_t tick, uint32_t a, uint32_t salt)
{
    return (float)(probe_hash(tick, a, salt) >> 8) * (1.0f / 16777216.0f);
}

void probe_reset(void)
{
    memset(s_hist, 0, sizeof(s_hist));
    memset(s_urg, 0, sizeof(s_urg));
    memset(s_urg_id, 0, sizeof(s_urg_id));
    memset(s_urg_tick, 0, sizeof(s_urg_tick));
    memset(&g_probe_stats, 0, sizeof(g_probe_stats));
    g_probe_ev_seen_n = 0u;
    g_probe_ev_mismatch = 0u;
    g_probe_inc_log_n = 0u;
    memset(s_arrived_id, 0, sizeof(s_arrived_id));
    memset(s_stage_id, 0, sizeof(s_stage_id));
    memset(s_stage_n, 0, sizeof(s_stage_n));
    memset(s_perim, 0, sizeof(s_perim));
    memset(s_offline, 0, sizeof(s_offline));
    memset(s_busy_until, 0, sizeof(s_busy_until));
    memset(s_support_repair, 0, sizeof(s_support_repair));
    if(g_probe.offline_k > 0u)
    {
        /* k teams chosen by a hash of the seed (the k smallest hash values). */
        for(uint8_t n = 0; n < g_probe.offline_k && n < NUM_TEAMS; n++)
        {
            int best = -1;
            uint32_t bh = 0xFFFFFFFFu;

            for(uint8_t t = 0; t < NUM_TEAMS; t++)
            {
                uint32_t h = probe_hash(0u, t, 77u);

                if(!s_offline[t] && (best < 0 || h < bh))
                {
                    best = t;
                    bh = h;
                }
            }
            if(best >= 0)
                s_offline[best] = 1u;
        }
    }
}

void probe_hazard_note(uint32_t tick, uint8_t type, int16_t x, int16_t y)
{
    if(!g_probe.any)
        return;
    if(g_probe_ev_seen_n < PROBE_MAX_EVENTS)
    {
        g_probe_ev_seen[g_probe_ev_seen_n].tick = tick;
        g_probe_ev_seen[g_probe_ev_seen_n].type = type;
        g_probe_ev_seen[g_probe_ev_seen_n].x = x;
        g_probe_ev_seen[g_probe_ev_seen_n].y = y;
        g_probe_ev_seen_n++;
    }
    if(g_probe_ev_plan_n > 0u)
    {
        uint8_t found = 0u;

        for(uint32_t i = 0; i < g_probe_ev_plan_n && !found; i++)
            found = (uint8_t)(g_probe_ev_plan[i].tick == tick && g_probe_ev_plan[i].type == type &&
                              g_probe_ev_plan[i].x == x && g_probe_ev_plan[i].y == y);
        if(!found)
            g_probe_ev_mismatch++;   /* an event the recording did not have; recorded events never seen are found by the report */
    }
}

void probe_preposition(void)
{
    uint8_t claimed[NUM_TEAMS];

    if(g_probe.foresight == 0u || g_probe_ev_plan_n == 0u)
        return;
    memset(claimed, 0, sizeof(claimed));
    for(uint32_t i = 0; i < g_probe_ev_plan_n; i++)
    {
        const ProbeEvent_t *e = &g_probe_ev_plan[i];
        int best = -1;
        uint16_t best_cost = 0xFFFFu;
        uint8_t spec;
        uint8_t is_fire = (e->type == INCIDENT_ACTIVE_FIRE), is_collapse = (e->type == INCIDENT_STRUCTURAL_COLLAPSE);

        if(e->tick <= g_current_tick || e->tick > g_current_tick + (uint32_t)g_probe.lead)
            continue;
        if(!((is_fire && (g_probe.foresight & 1u)) || (is_collapse && (g_probe.foresight & 2u)) ||
             (g_probe.foresight == 0xFFu)))
            continue;
        spec = (uint8_t)g_incident_archetypes[e->type].required_specialization;
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            const TeamState_t *team = &g_teams[t];
            uint16_t c;

            if(claimed[t] || team->status != STATUS_AVAILABLE || team->assigned_task_pool != TASK_POOL_NONE ||
               (uint8_t)team->specialization != spec || team->fault_state.active_mask != 0u ||
               team->stamina < get_soft_stamina_floor(team))
                continue;
            c = find_path_cost(team->loc_x, team->loc_y, e->x, e->y);
            if(c < best_cost)
            {
                best_cost = c;
                best = t;
            }
        }
        if(best < 0)
            continue;
        claimed[best] = 1u;
        {
            TeamState_t *team = &g_teams[best];
            int nx = team->loc_x, ny = team->loc_y;

            if(!(team->loc_x == e->x && team->loc_y == e->y) &&
               find_next_path_step(team->loc_x, team->loc_y, e->x, e->y, 1, &nx, &ny) > 0)
            {
                team->loc_x = (uint8_t)nx;   /* free movement: a ceiling, no stamina is charged */
                team->loc_y = (uint8_t)ny;
            }
        }
    }
}

/* Level ii: expected victims lost in the next W ticks from the entrapment model's rate, given age and victim count. */
static float urgency_rate(const IncidentState_t *inc, uint32_t tick)
{
    const CausalFaultParams_t *k = &g_causal_params;
    const float tick_h = (float)TICK_TIME_SEC / 3600.0f;
    float age_h, a1, a2;

    if(tick < inc->reported_at_tick || inc->victim_count == 0u)
        return 0.0f;
    age_h = (float)(tick - inc->reported_at_tick) * tick_h;
    a1 = age_h / k->survival_lambda_h;
    a2 = (age_h + (float)PROBE_URGENCY_WINDOW_TICKS * tick_h) / k->survival_lambda_h;
    /* 1 - S(t+W)/S(t) with Weibull S(t) = exp(-(t/lambda)^k) */
    return (float)inc->victim_count * (1.0f - expf(-(powf(a2, k->survival_k) - powf(a1, k->survival_k))));
}

void probe_step(void)
{
    uint32_t tick = g_current_tick + 1u;

    if(!g_probe.any)
        return;
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
        for(uint8_t f = 0; f < 3u; f++)
            s_hist[t][f][tick % PROBE_HIST] = g_teams[t].fault_state.active[f] ? 1u : 0u;

    if(g_probe.diag)
    {
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            const TeamState_t *team = &g_teams[t];
            uint8_t sp = (uint8_t)team->specialization, k;

            if(sp >= 5u)
                continue;
            if(team->status == STATUS_RESTING)                     k = 0u;
            else if(team->heading_to_rest)                         k = 1u;
            else if(fault_injector_causal_team_held(t))            k = 2u;
            else if(team->fault_state.active[FAULT_CLASS_MOBILITY] || team->fault_state.active[FAULT_CLASS_RESOURCE]) k = 3u;
            else if(team->status == STATUS_ON_SCENE)               k = 4u;
            else if(team->status == STATUS_EN_ROUTE)               k = 5u;
            else                                                   k = 6u;
            g_probe_stats.tb[sp][k]++;
        }
    }

    if(g_probe.urgency != 0u)
    {
        for(uint8_t slot = 0; slot < NUM_INCIDENTS; slot++)
        {
            const IncidentState_t *inc = &g_incidents[slot];
            float u;

            if(!inc->slot_occupied || inc->status == INC_RESOLVED)
                continue;
            u = (g_probe.urgency == 2u)
                    ? fault_injector_causal_future_victim_loss(slot, PROBE_URGENCY_WINDOW_TICKS)
                    : urgency_rate(inc, g_current_tick);
            s_urg[slot] = u;
            s_urg_id[slot] = inc->incident_id;
            s_urg_tick[slot] = g_current_tick;
        }
    }
}

float probe_oracle_p(const TeamState_t *team, uint8_t f)
{
    uint8_t truth;
    uint32_t tick = g_current_tick;

    if(g_probe.oracle_delay == 0u || tick < g_probe.oracle_delay)
        truth = (g_probe.oracle_delay == 0u && f < 3u) ? (team->fault_state.active[f] ? 1u : 0u) : 0u;
    else
        truth = s_hist[team->team_id][f][(tick - g_probe.oracle_delay) % PROBE_HIST];

    if(g_probe.oracle_noise)
    {
        /* One draw per 8-tick block so the oracle does not flicker. */
        float u = probe_unit(tick / 8u, (uint32_t)team->team_id * 4u + f, truth ? 11u : 12u);

        if(truth && u < g_probe.oracle_fn)
        {
            g_probe_stats.oracle_misses++;
            truth = 0u;
        }
        else if(!truth && u < g_probe.oracle_fp)
        {
            g_probe_stats.oracle_false_alarms++;
            truth = 1u;
        }
    }
    return truth ? 1.0f : 0.0f;
}

int probe_evidence_dropped(uint8_t team_id)
{
    if(g_probe.evidence_dropout <= 0.0f)
        return 0;
    if(probe_unit(g_current_tick, team_id, 21u) < g_probe.evidence_dropout)
    {
        g_probe_stats.dropout_team_ticks++;
        return 1;
    }
    return 0;
}

float probe_urgency_score(const IncidentState_t *incident)
{
    uint8_t slot;
    float u;

    if(g_probe.urgency == 0u || !fault_profile_is_causal())
        return -1.0f;
    if(incident < &g_incidents[0] || incident >= &g_incidents[NUM_INCIDENTS])
        return -1.0f;
    slot = (uint8_t)(incident - &g_incidents[0]);
    if(s_urg_id[slot] == incident->incident_id && s_urg_tick[slot] + 1u >= g_current_tick)
        u = s_urg[slot];
    else
        u = urgency_rate(incident, g_current_tick);   /* new this tick: the rate form */
    u /= PROBE_URGENCY_DIVISOR;
    return u > 1.0f ? 1.0f : u;
}

void probe_inc_arrived(const IncidentState_t *inc)
{
    uint8_t slot;

    if(!g_probe.diag || inc < &g_incidents[0] || inc >= &g_incidents[NUM_INCIDENTS])
        return;
    slot = (uint8_t)(inc - &g_incidents[0]);
    if(s_arrived_id[slot] != inc->incident_id)
    {
        s_arrived_id[slot] = inc->incident_id;
        s_arrived_tick[slot] = g_current_tick;
    }
}

void probe_inc_resolved(const IncidentState_t *inc)
{
    uint8_t slot;

    if(!g_probe.diag || inc < &g_incidents[0] || inc >= &g_incidents[NUM_INCIDENTS] || g_probe_inc_log_n >= PROBE_INC_LOG_MAX)
        return;
    slot = (uint8_t)(inc - &g_incidents[0]);
    g_probe_inc_log[g_probe_inc_log_n].id = inc->incident_id;
    g_probe_inc_log[g_probe_inc_log_n].type = (uint8_t)inc->incident_type;
    g_probe_inc_log[g_probe_inc_log_n].prio = (uint8_t)inc->priority;
    g_probe_inc_log[g_probe_inc_log_n].reported = inc->reported_at_tick;
    g_probe_inc_log[g_probe_inc_log_n].arrived = (s_arrived_id[slot] == inc->incident_id) ? s_arrived_tick[slot] : 0u;
    g_probe_inc_log[g_probe_inc_log_n].resolved = g_current_tick;
    g_probe_inc_log_n++;
}

int probe_team_offline(uint8_t team_id)
{
    return g_probe.offline_k > 0u && team_id < NUM_TEAMS && s_offline[team_id];
}

uint8_t probe_team_spec(uint8_t i)
{
    /* counts of SEARCH, MEDICAL, EXTRACTION, PERIMETER, SUPPORT; blocks in that order */
    static const uint8_t mix[3][5] = { {1, 2, 4, 3, 0}, {1, 2, 3, 3, 1}, {1, 1, 3, 3, 2} };
    uint8_t acc = 0u;

    if(g_probe.team_mix == 0u)
        return (uint8_t)(i % 5u);
    for(uint8_t sp = 0; sp < 5u; sp++)
    {
        acc = (uint8_t)(acc + mix[g_probe.team_mix - 1u][sp]);
        if(i < acc)
            return sp;
    }
    return 4u;
}

void probe_stage_note(const IncidentState_t *inc)
{
    uint8_t slot;

    if(g_probe.work_stage == 0u || inc < &g_incidents[0] || inc >= &g_incidents[NUM_INCIDENTS])
        return;
    slot = (uint8_t)(inc - &g_incidents[0]);
    if(s_stage_id[slot] != inc->incident_id)
    {
        s_stage_id[slot] = inc->incident_id;
        s_stage_n[slot] = 0u;
    }
    if(s_stage_n[slot] < 255u)
        s_stage_n[slot]++;
}

float probe_work_mult(const TeamState_t *team, const IncidentState_t *inc)
{
    float m = 1.0f;

    if(g_probe.work_mult > 0.0f)
        m *= g_probe.work_mult;
    if(g_probe.work_spec != 0u && (uint8_t)team->specialization + 1u == g_probe.work_spec)
        m *= g_probe.work_spec_mult;
    if(g_probe.work_stage != 0u && inc >= &g_incidents[0] && inc < &g_incidents[NUM_INCIDENTS])
    {
        uint8_t slot = (uint8_t)(inc - &g_incidents[0]);
        uint8_t n = (s_stage_id[slot] == inc->incident_id) ? s_stage_n[slot] : 0u;

        if((g_probe.work_stage == 1u && n == 0u) || (g_probe.work_stage == 2u && n > 0u))
            m *= g_probe.work_stage_mult;
    }
    return m;
}

float probe_speed(uint8_t spec)
{
    return (spec < 5u && g_probe.speed[spec] > 0.0f) ? g_probe.speed[spec] : 1.0f;
}

const uint8_t *probe_perimeter_overlay(void)
{
    if(g_probe.speed[3] <= 0.0f)
        return NULL;
    for(int x = 0; x < GRID_WIDTH; x++)
        for(int y = 0; y < GRID_HEIGHT; y++)
            s_perim[x * GRID_HEIGHT + y] = fault_injector_causal_road_kind(x, y) != 0u ? 255u : 0u;
    return s_perim;
}

/* DQ1 ------------------------------------------------------------------------------------------------------------ */
#define DQ_MAXN 64
/* Hungarian algorithm (minimisation), rows <= cols, a[1..n][1..m]; returns assignment col -> row in p[]. */
static void hungarian(int n, int m, long a[DQ_MAXN + 1][DQ_MAXN + 1], int p[DQ_MAXN + 1])
{
    long u[DQ_MAXN + 1], v[DQ_MAXN + 1], minv[DQ_MAXN + 1];
    int way[DQ_MAXN + 1];
    uint8_t used[DQ_MAXN + 1];

    memset(u, 0, sizeof(u));
    memset(v, 0, sizeof(v));
    memset(p, 0, sizeof(int) * (DQ_MAXN + 1));
    memset(way, 0, sizeof(way));
    for(int i = 1; i <= n; i++)
    {
        int j0 = 0;

        p[0] = i;
        for(int j = 0; j <= m; j++)
        {
            minv[j] = 0x7FFFFFFFFFFFL;
            used[j] = 0;
        }
        do
        {
            int i0 = p[j0], j1 = 0;
            long delta = 0x7FFFFFFFFFFFL;

            used[j0] = 1;
            for(int j = 1; j <= m; j++)
            {
                if(used[j])
                    continue;
                {
                    long cur = a[i0][j] - u[i0] - v[j];

                    if(cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                    if(minv[j] < delta) { delta = minv[j]; j1 = j; }
                }
            }
            for(int j = 0; j <= m; j++)
            {
                if(used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else minv[j] -= delta;
            }
            j0 = j1;
        } while(p[j0] != 0);
        do
        {
            int j1 = way[j0];

            p[j0] = p[j1];
            j0 = j1;
        } while(j0);
    }
}

void probe_central_assign(void)
{
    static CBBAAgentState_t empty;
    int teams[NUM_TEAMS], slots[NUM_INCIDENTS], nt = 0, ni = 0;
    static long a[DQ_MAXN + 1][DQ_MAXN + 1];
    static float util[NUM_TEAMS][NUM_INCIDENTS];
    int p[DQ_MAXN + 1];
    const long BAD = 100000000L;

    if(!g_probe.central_alloc)
        return;
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
        if(g_teams[t].status == STATUS_AVAILABLE || g_teams[t].status == STATUS_RESTING)
            teams[nt++] = t;
    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        const IncidentState_t *inc = &g_incidents[i];

        if(inc->slot_occupied && inc->status == INC_OPEN && inc->unreachable_streak < INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD)
            slots[ni++] = i;
    }
    /* the auction's own bundles and winners are discarded for every team */
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        g_cbba_agent_state[t].path_size = 0u;
        g_cbba_agent_state[t].bundle_size = 0u;
        for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
            g_cbba_agent_state[t].winner[i] = NO_ASSIGNMENT;
    }
    if(nt == 0 || ni == 0 || nt > DQ_MAXN || ni > DQ_MAXN)
        return;
    path_cache_begin();
    for(int r = 0; r < nt; r++)
        for(int c = 0; c < ni; c++)
        {
            const TeamState_t *team = &g_teams[teams[r]];
            const IncidentState_t *inc = &g_incidents[slots[c]];
            uint8_t pos = 0;
            float b = -1.0f;

            if(team_can_accept_incident(team, inc) && team_has_minimum_equipment(team, inc))
                b = calculate_marginal_bundle_bid((uint8_t)teams[r], &empty, inc, &pos);
            util[r][c] = b;
        }
    path_cache_end();
    {
        int rows = nt <= ni ? nt : ni, cols = nt <= ni ? ni : nt;
        int transposed = nt > ni;

        memset(a, 0, sizeof(a));
        for(int i = 1; i <= rows; i++)
            for(int j = 1; j <= cols; j++)
            {
                float b = transposed ? util[j - 1][i - 1] : util[i - 1][j - 1];

                a[i][j] = (b < 0.0f) ? BAD : -(long)(b * 100000.0f);
            }
        hungarian(rows, cols, a, p);
        for(int j = 1; j <= cols; j++)
        {
            int i = p[j], r, c;

            if(i == 0)
                continue;
            r = transposed ? j - 1 : i - 1;
            c = transposed ? i - 1 : j - 1;
            if(util[r][c] < 0.0f)
                continue;
            {
                CBBAAgentState_t *ag = &g_cbba_agent_state[teams[r]];

                ag->path[0].pool = TASK_POOL_INCIDENT;
                ag->path[0].slot = (uint8_t)slots[c];
                ag->path_size = 1u;
                ag->winner[slots[c]] = (uint8_t)teams[r];
            }
        }
    }
}

/* Tier 4 ------------------------------------------------------------------------------------------------------- */
int probe_team_busy(uint8_t team_id)
{
    return team_id < NUM_TEAMS && s_busy_until[team_id] > g_current_tick;
}

float probe_field_success(uint8_t team_id, float nominal)
{
    /* RR2a: a support team brings parts and tools; success 0.9 instead of the self-repair 2/3 (assumed, to verify). */
    return (team_id < NUM_TEAMS && s_support_repair[team_id]) ? 0.9f : nominal;
}

float probe_capability_factor(const TeamState_t *team)
{
    float work, speed = 1.0f;

    if(!g_probe.rr_degraded)
        return 1.0f;
    if(!team->fault_state.active[FAULT_CLASS_MOBILITY] && !team->fault_state.active[FAULT_CLASS_RESOURCE])
        return 1.0f;
    work = (float)team_effective_work_rate(team) / (float)team_base_work_rate(team->equipment_level);
    if(team->fault_state.active[FAULT_CLASS_MOBILITY])
    {
        uint8_t sev = team->fault_state.severity[FAULT_CLASS_MOBILITY];

        if(sev >= 4u) speed = 1.0f / 3.0f;       /* moves one tick in three (simulation.c) */
        else if(sev >= 3u) speed = 0.5f;         /* moves one tick in two */
    }
    return work * speed;
}

/* Earliest tick count until the team's stamina or equipment reaches the level at which the gates send it home. */
static float time_to_exhaustion(const TeamState_t *t)
{
    float ts = (t->stamina - RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD) / STAMINA_OPERATION_COST;
    float te = ((float)t->equipment_level - (float)RECOVERY_MANAGER_EQUIPMENT_CRITICAL_THRESHOLD) / 3.0f;   /* mean draw 1..5 */

    return ts < te ? ts : te;
}

void probe_rr_step(void)
{
    if(!g_probe.rr_relief && !g_probe.rr_support_repair)
        return;

    if(g_probe.rr_relief)
    {
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            TeamState_t *tired = &g_teams[t];
            IncidentState_t *inc;
            int best = -1;
            float best_score = -1.0f;
            uint16_t best_eta = 0;
            float tte;

            if(tired->status != STATUS_ON_SCENE || tired->assigned_task_pool != TASK_POOL_INCIDENT ||
               tired->assigned_task_slot >= NUM_INCIDENTS || tired->heading_to_rest)
                continue;
            inc = &g_incidents[tired->assigned_task_slot];
            if(!inc->slot_occupied || inc->status != INC_IN_PROGRESS || inc->assigned_team != t)
                continue;
            tte = time_to_exhaustion(tired);

            /* A relief team already on scene as support: swap roles when the tired team is about to leave. */
            if(inc->support_team < NUM_TEAMS && g_teams[inc->support_team].status == STATUS_ON_SCENE && tte <= 8.0f)
            {
                TeamState_t *rel = &g_teams[inc->support_team];

                inc->assigned_team = rel->team_id;
                inc->support_team = NO_ASSIGNMENT;
                tired->assigned_task_pool = TASK_POOL_NONE;
                tired->assigned_task_slot = NO_ASSIGNMENT;
                tired->status = STATUS_AVAILABLE;
                (void)begin_return_to_safe_zone(tired);
                g_probe_stats.rr_relief_swap++;
                continue;
            }
            if(inc->support_team != NO_ASSIGNMENT || inc->work_remaining == 0u)
                continue;
            /* nearest eligible replacement (idle, can accept, route) by the system's own score */
            for(uint8_t r = 0; r < NUM_TEAMS; r++)
            {
                TeamState_t *cand = &g_teams[r];
                uint16_t eta;
                float sc;

                if(r == t || (cand->status != STATUS_AVAILABLE && cand->status != STATUS_RESTING) || cand->assigned_task_pool != TASK_POOL_NONE ||
                   !team_can_accept_incident(cand, inc))
                    continue;
                eta = find_path_cost(cand->loc_x, cand->loc_y, inc->loc_x, inc->loc_y);
                if(eta == 0xFFFFu)
                    continue;
                sc = calculate_score(cand, inc);
                if(sc > best_score)
                {
                    best_score = sc;
                    best = r;
                    best_eta = eta;
                }
            }
            if(best < 0)
                continue;
            {
                uint8_t go = (tte <= (float)best_eta + 1.0f);

                if(!go && g_probe.rr_relief == 2u)
                    go = (uint8_t)(fault_injector_causal_onset_within(t, (uint32_t)best_eta + 1u) != 0u);   /* realised fault coming */
                if(go && allocator_assign_support((uint8_t)best, tired->assigned_task_slot))
                    g_probe_stats.rr_relief_assign++;
            }
        }
    }

    if(g_probe.rr_support_repair)
    {
        for(uint8_t sup = 0; sup < NUM_TEAMS; sup++)
        {
            TeamState_t *st = &g_teams[sup];
            int target = -1;
            uint16_t best_eta = 0xFFFFu;

            if(st->specialization != SPEC_SUPPORT || st->status != STATUS_AVAILABLE || st->assigned_task_pool != TASK_POOL_NONE ||
               probe_team_busy(sup) || probe_team_offline(sup) ||
               st->fault_state.active[FAULT_CLASS_MOBILITY] || st->fault_state.active[FAULT_CLASS_RESOURCE])
                continue;
            for(uint8_t f = 0; f < NUM_TEAMS; f++)
            {
                TeamState_t *ft = &g_teams[f];
                uint16_t eta;

                if(f == sup || s_support_repair[f] || fault_injector_causal_team_held(f) ||
                   (ft->status != STATUS_EN_ROUTE && ft->status != STATUS_ON_SCENE) || ft->heading_to_rest ||
                   !(ft->fault_state.active[FAULT_CLASS_MOBILITY] || ft->fault_state.active[FAULT_CLASS_RESOURCE]))
                    continue;
                eta = find_path_cost(st->loc_x, st->loc_y, ft->loc_x, ft->loc_y);
                if(eta < best_eta)
                {
                    best_eta = eta;
                    target = f;
                }
            }
            if(target < 0 || best_eta == 0xFFFFu || best_eta > 60u)
                continue;
            {
                TeamState_t *ft = &g_teams[target];
                uint8_t kind = ft->fault_state.active[FAULT_CLASS_MOBILITY] ? 1u : 2u;

                s_support_repair[target] = 1u;
                g_probe_stats.rr_repair_try++;
                if(fault_injector_causal_field_start((uint8_t)target, kind))
                {
                    /* The repair holds the faulted team from now (it waits for the helper, then is repaired); the
                     * support team is committed for the helper's travel plus the repair. */
                    st->loc_x = ft->loc_x;
                    st->loc_y = ft->loc_y;
                    g_probe_stats.rr_repair_commit++;
                    s_busy_until[sup] = g_current_tick + best_eta + fault_injector_causal_field_remaining((uint8_t)target) + 1u;
                }
                else
                    s_support_repair[target] = 0u;
            }
        }
        for(uint8_t f = 0; f < NUM_TEAMS; f++)
            if(s_support_repair[f] && !fault_injector_causal_team_held(f))
                s_support_repair[f] = 0u;
    }
}

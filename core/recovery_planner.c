#include "headroom_probes.h"
#include "recovery_planner.h"

#include <stddef.h>

#include "allocator.h"
#include "causal_actions.h"
#include "environment.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "incident.h"
#include "path.h"
#include "fault_predictor.h"
#include "planner_calibration.h"
#include "safezone.h"

/* ----------------------------------------------------------------------
 * Survival hazard
 *
 * Death hazard per victim per tick of the entrapment model the simulator and
 * this planner both use: Weibull with k = 1.47, lambda = 54.9 h (MacIntyre
 * 2006 fit, "to verify", docs/CAUSAL_FAULT_MODEL.md):
 *     h(a) = (k / lambda) * (a / lambda)^(k - 1) * tau_h,   tau_h = 0.01 h/tick
 * Tabulated hourly (0..96 h) and interpolated, so the planner needs no libm
 * (the board links none of powf/logf for this).
 * ---------------------------------------------------------------------- */
static const float s_hazard_per_h[97] = {
    0.000000e+00f, 4.075172e-05f, 5.644560e-05f, 6.829564e-05f, 7.818333e-05f, 8.682842e-05f,
    9.459694e-05f, 1.017049e-04f, 1.082925e-04f, 1.144564e-04f, 1.202669e-04f, 1.257768e-04f,
    1.310271e-04f, 1.360503e-04f, 1.408725e-04f, 1.455154e-04f, 1.499969e-04f, 1.543324e-04f,
    1.585346e-04f, 1.626148e-04f, 1.665828e-04f, 1.704469e-04f, 1.742146e-04f, 1.778927e-04f,
    1.814869e-04f, 1.850026e-04f, 1.884445e-04f, 1.918169e-04f, 1.951238e-04f, 1.983687e-04f,
    2.015547e-04f, 2.046850e-04f, 2.077622e-04f, 2.107888e-04f, 2.137672e-04f, 2.166995e-04f,
    2.195878e-04f, 2.224338e-04f, 2.252394e-04f, 2.280060e-04f, 2.307354e-04f, 2.334288e-04f,
    2.360876e-04f, 2.387131e-04f, 2.413063e-04f, 2.438686e-04f, 2.464008e-04f, 2.489041e-04f,
    2.513792e-04f, 2.538272e-04f, 2.562488e-04f, 2.586449e-04f, 2.610163e-04f, 2.633635e-04f,
    2.656875e-04f, 2.679887e-04f, 2.702678e-04f, 2.725255e-04f, 2.747623e-04f, 2.769787e-04f,
    2.791754e-04f, 2.813527e-04f, 2.835111e-04f, 2.856512e-04f, 2.877734e-04f, 2.898780e-04f,
    2.919656e-04f, 2.940364e-04f, 2.960910e-04f, 2.981296e-04f, 3.001526e-04f, 3.021603e-04f,
    3.041531e-04f, 3.061313e-04f, 3.080952e-04f, 3.100450e-04f, 3.119812e-04f, 3.139038e-04f,
    3.158133e-04f, 3.177099e-04f, 3.195938e-04f, 3.214652e-04f, 3.233244e-04f, 3.251717e-04f,
    3.270072e-04f, 3.288311e-04f, 3.306437e-04f, 3.324452e-04f, 3.342357e-04f, 3.360155e-04f,
    3.377847e-04f, 3.395435e-04f, 3.412921e-04f, 3.430307e-04f, 3.447594e-04f, 3.464783e-04f,
    3.481877e-04f,
};

static float hazard_per_tick(float age_ticks)
{
    float hours = age_ticks * (float)TICK_TIME_SEC / 3600.0f;
    int i;
    float f;

    if(hours <= 0.0f)
        return 0.0f;
    if(hours >= 96.0f)
        return s_hazard_per_h[96];
    i = (int)hours;
    f = hours - (float)i;
    return s_hazard_per_h[i] + f * (s_hazard_per_h[i + 1] - s_hazard_per_h[i]);
}

/* V: incident-ticks per expected death, derived once by parity: a reference
 * incident (n_ref victims, age_ref ticks) has victim urgency equal to the base
 * urgency 1. D1: multiplied by v_mult for the sweep. */
static float planner_v(void)
{
    float h = hazard_per_tick(g_planner.age_ref_ticks);

    if(h <= 0.0f || g_planner.n_ref_victims <= 0.0f)
        return 0.0f;
    return g_planner.v_mult / (g_planner.n_ref_victims * h);
}

static float incident_urgency(const IncidentState_t *inc)
{
    float age = (g_current_tick > inc->reported_at_tick) ? (float)(g_current_tick - inc->reported_at_tick) : 0.0f;

    return 1.0f + planner_v() * (float)inc->victim_count * hazard_per_tick(age);
}

/* ----------------------------------------------------------------------
 * State and per-tick context
 * ---------------------------------------------------------------------- */
typedef struct
{
    uint8_t last_action;
    uint8_t last_prev_action;
    uint32_t last_change_tick;
    uint8_t last_status;
    uint32_t rest_end_tick;        /* tick the team last left RESTING */
    uint32_t preempt_until;        /* tick + 1: no new preemption of this team before */
    uint32_t prevent_until;        /* Step 3.6: no new preventive action for this team before this tick */
    float p[3];                    /* fused P, last Layer A step */
} PlannerTeamState_t;

static PlannerTeamState_t s_team[NUM_TEAMS];

/* Outcome of every job a team takes (a clearing task, or the support slot of an incident), observed in the
 * run: a job's benefit is only earned if the team finishes it, so it is scaled by the Laplace estimate
 * (Beta(1,1) prior) of that kind's success rate. This is what keeps the planner from repeating a job that
 * keeps failing (the rule-based evaluators send a team that stalls at debris home before it can clear it). */
typedef struct
{
    uint8_t active;
    uint8_t kind;          /* CausalAction_t */
    uint8_t slot;
    uint32_t id;           /* incident_id of the job, to survive slot reuse */
} PlannerPreempt_t;
static PlannerPreempt_t s_pre[NUM_TEAMS];

static PlannerStats_t s_stats;

static float __attribute__((unused)) success_rate(uint8_t kind)
{
    return ((float)s_stats.preempt_succeeded[kind] + 1.0f) /
           ((float)s_stats.preempt_succeeded[kind] + (float)s_stats.preempt_failed[kind] + 2.0f);
}
#if ENVIRONMENT_HAS_WORLD_STORAGE
static PlannerCalib_t s_calib;   /* laptop calibration histograms (6.9 KB): not built for the board */
#endif
static void (*s_log_sink)(const PlannerDecision_t *);

static struct
{
    uint32_t tick_plus1;
    float q;       /* IT per team-tick of lost capacity: open-stage urgency per team */
    float ubar;    /* mean urgency of active incidents */
} s_ctx;

static void tick_context(void)
{
    float qsum = 0.0f, usum = 0.0f;
    uint8_t n = 0;

    if(s_ctx.tick_plus1 == g_current_tick + 1u)
        return;
    s_ctx.tick_plus1 = g_current_tick + 1u;

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        const IncidentState_t *inc = &g_incidents[i];
        float u;

        if(!inc->slot_occupied || inc->status == INC_RESOLVED || !inc->discovered)
            continue;
        u = incident_urgency(inc);
        usum += u;
        n++;
        if(inc->status == INC_OPEN)
            qsum += u;
    }
    s_ctx.q = qsum / (float)NUM_TEAMS;
    s_ctx.ubar = (n > 0u) ? usum / (float)n : 1.0f;
}

void planner_reset(void)
{
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        s_team[t].last_action = 0;
        s_team[t].last_prev_action = 0;
        s_team[t].last_change_tick = 0;
        s_team[t].last_status = 0;
        s_team[t].rest_end_tick = 0;
        s_team[t].preempt_until = 0;
        s_team[t].prevent_until = 0;
        s_team[t].p[0] = s_team[t].p[1] = s_team[t].p[2] = 0.0f;
        s_pre[t].active = 0;
    }
    s_ctx.tick_plus1 = 0;
    s_ctx.q = 0.0f;
    s_ctx.ubar = 1.0f;
}

const PlannerStats_t *planner_stats(void) { return &s_stats; }
#if ENVIRONMENT_HAS_WORLD_STORAGE
const PlannerCalib_t *planner_calib(void) { return &s_calib; }
#else
const PlannerCalib_t *planner_calib(void) { return NULL; }
#endif
const float *planner_last_p(uint8_t team_id) { return s_team[team_id < NUM_TEAMS ? team_id : 0].p; }
void planner_set_log_sink(void (*sink)(const PlannerDecision_t *)) { s_log_sink = sink; }

static void log_decision(const PlannerDecision_t *d)
{
    if(g_planner.log_enabled && s_log_sink != NULL)
        s_log_sink(d);
}

void planner_note_recommendation(uint8_t team_id, uint8_t action)
{
    PlannerTeamState_t *s;

    if(team_id >= NUM_TEAMS)
        return;
    s = &s_team[team_id];
    if(action != s->last_action)
    {
        if((action == RECOVERY_ACTION_RETURN_TO_SAFE_ZONE || action == RECOVERY_ACTION_ABORT_MISSION) &&
           g_teams[team_id].status != STATUS_RESTING && !g_teams[team_id].heading_to_rest)
        {
            s_stats.go_home_events++;
            if(causal_actions_debris_held_within(team_id, 12u))
                s_stats.go_home_near_stall++;
        }
        s_stats.action_changes++;
        if(action == s->last_prev_action && (g_current_tick - s->last_change_tick) <= 10u)
            s_stats.reversals10++;
        s->last_prev_action = s->last_action;
        s->last_action = action;
        s->last_change_tick = g_current_tick;
    }
}

/* ----------------------------------------------------------------------
 * Helpers shared by the layers
 * ---------------------------------------------------------------------- */
static float fminf_(float a, float b) { return a < b ? a : b; }
static float fmaxf_(float a, float b) { return a > b ? a : b; }

/* Mean work rate: rates are drawn 1..8 (WORK_RATE_MIN/MAX, core/mission_state.h). */
static float __attribute__((unused)) ref_work_rate(void)
{
    return ((float)WORK_RATE_MIN + (float)WORK_RATE_MAX) / 2.0f;
}

static const IncidentState_t *team_task(const TeamState_t *team)
{
    if(team->assigned_task_pool == TASK_POOL_INCIDENT && team->assigned_task_slot < NUM_INCIDENTS)
        return g_incidents[team->assigned_task_slot].slot_occupied ? &g_incidents[team->assigned_task_slot] : NULL;
    if(team->assigned_task_pool == TASK_POOL_SYSTEM && team->assigned_task_slot < NUM_SYSTEM_TASKS)
        return g_system_tasks[team->assigned_task_slot].slot_occupied ? &g_system_tasks[team->assigned_task_slot] : NULL;
    return NULL;
}

/* Ticks left on the team's task: remaining route steps plus remaining work. */
static void task_remaining(const TeamState_t *team, const IncidentState_t *task, float *travel, float *work)
{
    float rate = (float)team_effective_work_rate(team);

    *travel = 0.0f;
    *work = 0.0f;
    if(task == NULL)
        return;
    if(team->status == STATUS_EN_ROUTE && team->cached_remaining_path_cost_tick + 1u >= g_current_tick)
        *travel = (float)team->cached_remaining_path_cost;
    if(rate < 1.0f)
        rate = 1.0f;
    *work = (float)task->work_remaining / rate;
}

static float rest_ticks_needed(const TeamState_t *team)
{
    float ready, rate, need;

    switch(team->fitness_tier)
    {
        case ARDUOUS: ready = ARDUOUS_READY_STAMINA; rate = ARDUOUS_RECOVERY_RATE; break;
        case MODERATE: ready = MODERATE_READY_STAMINA; rate = MODERATE_RECOVERY_RATE; break;
        default: ready = LIGHT_READY_STAMINA; rate = LIGHT_RECOVERY_RATE; break;
    }
    need = (ready - team->stamina) / rate;
    return fmaxf_(need, (float)CAUSAL_MOBILITY_REPAIR_REST_TICKS);
}

static uint8_t in_service(const TeamState_t *t)
{
    return (t->status == STATUS_AVAILABLE || t->status == STATUS_EN_ROUTE || t->status == STATUS_ON_SCENE) && !t->heading_to_rest;
}

/* Open demand for a specialization: discovered open stages (and open system tasks). */
static uint8_t open_demand(uint8_t spec)
{
    uint8_t n = 0;

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
        if(g_incidents[i].slot_occupied && g_incidents[i].status == INC_OPEN && g_incidents[i].discovered &&
           g_incidents[i].req_capability == spec)
            n++;
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
        if(g_system_tasks[i].slot_occupied && g_system_tasks[i].status == INC_OPEN &&
           g_system_tasks[i].incident_type != INCIDENT_RETURN_TO_SAFE_ZONE && g_system_tasks[i].req_capability == spec)
            n++;
    return n;
}

/* Removing this team from service would leave a specialization with open demand
 * and no other team in service. D3: such a choice needs operator approval. */
static uint8_t is_last_specialist(const TeamState_t *team)
{
    for(uint8_t i = 0; i < NUM_TEAMS; i++)
        if(i != team->team_id && g_teams[i].specialization == team->specialization && in_service(&g_teams[i]))
            return 0;
    return open_demand(team->specialization) > 0u;
}

/* An approval request. deny (default) refuses it; auto grants it (laptop upper bound). */
static uint8_t request_approval(uint8_t *flags)
{
    s_stats.approvals_requested++;
    *flags |= PLANNER_FLAG_APPROVAL_REQ;
    if(g_planner.approval)
    {
        s_stats.approvals_granted++;
        *flags |= PLANNER_FLAG_APPROVAL_OK;
        return 1;
    }
    s_stats.approvals_denied++;
    return 0;
}

#if ENVIRONMENT_HAS_WORLD_STORAGE
/* ----------------------------------------------------------------------
 * Persistent faults (Step 3.6). A mobility or resource fault now lasts until it is remedied, so a team that stays
 * faulted pays the fault's per-tick harm until its next rest (or the long natural-recovery tail), whichever comes
 * first. The harm rates are the simulator's own documented effects (core/simulation.c move_team, core/incident.c
 * team_effective_work_rate), the durations are the world's parameters (docs/PERSISTENT_FAULTS_PROPOSAL.md section
 * 9.1); nothing here is tuned. Escalation uses the class's baseline hazard (the planner does not see hidden
 * wear) and is linearised to avoid libm. Laptop only (the board never runs the causal profile).
 * ---------------------------------------------------------------------- */
typedef struct
{
    float stay, go, fr;     /* total expected cost (IT) of staying, going home, repairing in the field */
    float harm[3];          /* harm of a persistent fault of each class if the team stays (before weighting by P) */
    float u_x;
    uint8_t fr_class;       /* class a field repair would address (0 mobility, 2 resource), 255 none */
} PersistCosts_t;

static float s_leg[NUM_TEAMS];
static uint32_t s_leg_tick_plus1[NUM_TEAMS];

static float persist_leg_ticks(const TeamState_t *team)
{
    uint8_t zone;
    float leg = 0.0f;

    if(s_leg_tick_plus1[team->team_id] == g_current_tick + 1u)
        return s_leg[team->team_id];
    zone = find_nearest_safe_zone(team);
    if(zone != NO_SAFE_ZONE)
    {
        uint16_t steps = find_path_steps(team->loc_x, team->loc_y, g_safezones[zone].loc_x, g_safezones[zone].loc_y);

        leg = (steps == UINT16_MAX) ? 200.0f : (float)steps;
    }
    s_leg[team->team_id] = leg;
    s_leg_tick_plus1[team->team_id] = g_current_tick + 1u;
    return leg;
}

static float persist_rate(uint8_t f, float sev, float base_rate, float t_frac, float w_frac, float u_x)
{
    float slow_work = (base_rate > sev) ? sev / (base_rate - sev) : sev;
    float slow_trav = (sev >= 4.0f) ? 2.0f : (sev >= 3.0f ? 1.0f : 0.0f);

    if(f == 0u)
        return (t_frac * slow_trav + w_frac * slow_work) * u_x;
    return w_frac * slow_work * u_x;
}

/* Expected cost of each response given the fault probabilities p (per class). */
static void persist_costs(const TeamState_t *team, const float p[3], PersistCosts_t *o)
{
    const CausalPersistParams_t *pp = &g_causal_persist_params;
    const IncidentState_t *task = team_task(team);
    float travel, work, remaining, t_frac, w_frac, base_rate, t_rest_work, d_stay, margin;
    float t_fr = ((float)pp->field_min_ticks + (float)pp->field_max_ticks) / 2.0f;
    float dwell = ((float)pp->base_dwell_min_ticks + (float)pp->base_dwell_max_ticks) / 2.0f;

    task_remaining(team, task, &travel, &work);
    if(task == NULL)
    {
        travel = g_planner.ref_travel_ticks;
        work = g_planner.ref_stage_ticks - g_planner.ref_travel_ticks;
    }
    remaining = fmaxf_(travel + work, 1.0f);
    t_frac = travel / remaining;
    w_frac = work / remaining;
    o->u_x = (task != NULL) ? incident_urgency(task) : s_ctx.ubar;
    base_rate = (float)team_base_work_rate(team->equipment_level);
    t_rest_work = fmaxf_((team->stamina - get_soft_stamina_floor(team)) / STAMINA_OPERATION_COST, 1.0f);
    d_stay = fminf_(pp->natural_mean_ticks, t_rest_work);

    o->stay = 0.0f;
    o->fr_class = 255u;
    {
        float best = 0.0f;

        for(uint8_t f = 0; f < 3u; f++)
        {
            float sev = team->fault_state.active[f] ? (float)team->fault_state.severity[f] : (float)g_fault_config.effect_strength[f];
            float h;

            if(f == 1u)
                h = fminf_((float)g_fault_config.duration_max_ticks[f], 10.0f) * g_planner.cblind;
            else
            {
                float lam = ((f == 0u) ? g_causal_params.mob_hazard_base : g_causal_params.res_hazard_base) * g_fault_stress;
                float p_esc = fminf_(1.0f, pp->escalation_mult * lam * d_stay);
                float r0 = persist_rate(f, sev, base_rate, t_frac, w_frac, o->u_x);
                float r1 = persist_rate(f, fminf_(sev + 1.0f, 4.0f), base_rate, t_frac, w_frac, o->u_x);

                h = r0 * d_stay + p_esc * (r1 - r0) * d_stay * 0.5f;
            }
            o->harm[f] = h;
            o->stay += p[f] * h;
            if(f != 1u && p[f] * h > best)
            {
                best = p[f] * h;
                o->fr_class = f;
            }
        }
    }
    o->go = o->fr = o->stay;
    margin = fmaxf_(2.0f * o->u_x, g_planner.margin_frac * o->stay);
    if(o->stay <= margin)
        return;

    {
        float t_leg = persist_leg_ticks(team);
        float t_rest = fmaxf_(rest_ticks_needed(team), p[0] * dwell);
        float act_go = (t_leg + t_rest) * s_ctx.q + ((task != NULL) ? o->u_x * g_planner.reassign_delay : 0.0f);
        float fault_go = 0.0f;

        for(uint8_t f = 0; f < 3u; f++)
        {
            if(f == 1u)
            {
                float rep = g_planner.repair_comm * g_planner.repair_scale;

                fault_go += p[f] * o->harm[f] * (1.0f - (rep > 1.0f ? 1.0f : rep));
            }
            else
            {
                float sev = team->fault_state.active[f] ? (float)team->fault_state.severity[f] : (float)g_fault_config.effect_strength[f];

                fault_go += p[f] * persist_rate(f, sev, base_rate, t_frac, w_frac, o->u_x) * t_leg;
            }
        }
        o->go = fault_go + act_go;
    }
    if(o->fr_class != 255u && fault_profile_causal_field_repair() &&
       (team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE))
    {
        /* The task halts while the team repairs itself (u_x per tick); with probability (1 - success) the fault stays. */
        o->fr = o->stay - p[o->fr_class] * pp->field_success * o->harm[o->fr_class] + t_fr * o->u_x;
    }
}

/* Preventive options (needs a forecast source): returns 0 none, 1 PREVENT (rest at base), 2 PREVENT_FIELD. */
static uint8_t persist_prevent_choice(const TeamState_t *team, float *benefit_out, float *cost_out)
{
    const CausalPersistParams_t *pp = &g_causal_persist_params;
    const IncidentState_t *task = team_task(team);
    float now[2], after_rest[2], after_field[2];
    float best_c[2] = {0.0f, 0.0f};
    float margin_floor;
    float t_fr = ((float)pp->field_min_ticks + (float)pp->field_max_ticks) / 2.0f;
    float u_x = (task != NULL) ? incident_urgency(task) : s_ctx.ubar;
    float cost_rest, cost_field, ben_rest, ben_field;
    uint8_t choice = 0;

    fault_injector_causal_forecast(team->team_id, 0u, now);
    fault_injector_causal_forecast(team->team_id, 1u, after_rest);
    fault_injector_causal_forecast(team->team_id, 2u, after_field);
    s_stats.prevent_considered++;

    /* Cost of a fault of class c that occurs, under the best response (the planner will respond to it). */
    for(uint8_t k = 0; k < 2u; k++)
    {
        float pv[3] = {0.0f, 0.0f, 0.0f};
        PersistCosts_t c;
        float m;

        pv[k == 0u ? 0u : 2u] = 1.0f;
        persist_costs(team, pv, &c);
        m = c.stay;
        if(c.go < m) m = c.go;
        if(c.fr < m) m = c.fr;
        best_c[k] = m;
    }
    ben_rest = (now[0] - after_rest[0]) * best_c[0] + (now[1] - after_rest[1]) * best_c[1];
    ben_field = (now[0] - after_field[0]) * best_c[0];
    margin_floor = 1.0f + g_planner.margin_frac;
    cost_rest = (2.0f * persist_leg_ticks(team) + rest_ticks_needed(team)) * s_ctx.q + ((task != NULL) ? u_x * g_planner.reassign_delay : 0.0f);
    cost_field = t_fr * u_x;
    if(g_probe.prevent_cost > 0.0f)
        cost_field *= g_probe.prevent_cost;   /* Step 5 S1: priced and executed at the same scale (fault_injector.c) */
    *benefit_out = 0.0f;
    *cost_out = 0.0f;

    if((g_planner.prevent & PLANNER_PREVENT_REST) && ben_rest > cost_rest * margin_floor)
    {
        choice = 1;
        *benefit_out = ben_rest;
        *cost_out = cost_rest;
    }
    if((g_planner.prevent & PLANNER_PREVENT_FIELD) && fault_profile_causal_field_repair() && task != NULL &&
       (team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE) &&
       ben_field > cost_field * margin_floor &&
       (choice == 0u || (ben_field - cost_field) > (ben_rest - cost_rest)))
    {
        choice = 2;
        *benefit_out = ben_field;
        *cost_out = cost_field;
    }
    return choice;
}
#endif /* ENVIRONMENT_HAS_WORLD_STORAGE */

/* ----------------------------------------------------------------------
 * P(fault | evidence)
 * ---------------------------------------------------------------------- */
static float cal_lookup(const float *lut, float score)
{
    int b = (int)(score * (float)PLANNER_CAL_BINS);

    if(b < 0) b = 0;
    if(b >= (int)PLANNER_CAL_BINS) b = (int)PLANNER_CAL_BINS - 1;
    return lut[b];
}

static int __attribute__((unused)) cal_bin(float x)
{
    int b = (int)(x * (float)PLANNER_CAL_BINS);

    if(b < 0) b = 0;
    if(b >= (int)PLANNER_CAL_BINS) b = (int)PLANNER_CAL_BINS - 1;
    return b;
}

/* Per class max of the calibrated sources (they are not independent: the AI
 * reads the watchdog's evidence vector). */
static void fuse_probabilities(const TeamState_t *team, const PlannerEvidence_t *ev, float p[3], float p_ai[3],
                               float p_oth[3])
{
    for(uint8_t f = 0; f < 3u; f++)
    {
        float a = 0.0f, w = 0.0f, c = 0.0f;

        if(g_planner.p_source == PLANNER_P_ORACLE || g_planner.p_source == PLANNER_P_BOTH)
        {
            p[f] = g_probe.any ? probe_oracle_p(team, f) : (team->fault_state.active[f] ? 1.0f : 0.0f);
            p_ai[f] = 0.0f;
            p_oth[f] = p[f];
            continue;
        }
        if(g_planner.p_source == PLANNER_P_AI)
            a = cal_lookup(PLANNER_CAL_AI[f], ev->ai_score[f]);
        if(ev->wd_class == f)
            w = cal_lookup(PLANNER_CAL_WD[f], ev->wd_conf);
        if(f < 2u)
            c = cal_lookup(PLANNER_CAL_CONS[f], ev->cons_y[f]);
        p_ai[f] = a;
        p_oth[f] = fmaxf_(w, c);
        p[f] = fmaxf_(a, p_oth[f]);

#if ENVIRONMENT_HAS_WORLD_STORAGE
        if(g_planner.calib_dump)
        {
            for(uint8_t src = 0; src < 3u; src++)
            {
                float raw, cal;
                int b;
                uint8_t label = team->fault_state.active[f] ? 1u : 0u;

                if(src == 0u) { if(g_planner.p_source != PLANNER_P_AI) continue; raw = ev->ai_score[f]; cal = a; }
                else if(src == 1u) { if(ev->wd_class != f) continue; raw = ev->wd_conf; cal = w; }
                else { if(f >= 2u) continue; raw = ev->cons_y[f]; cal = c; }
                b = cal_bin(raw);
                s_calib.n[src][f][b]++;
                s_calib.pos[src][f][b] += label;
                b = cal_bin(cal);
                s_calib.cal_n[src][f][b]++;
                s_calib.cal_pos[src][f][b] += label;
                s_calib.cal_sum[src][f][b] += cal;
            }
            {
                int b = cal_bin(p[f]);

                s_calib.fused_n[f][b]++;
                s_calib.fused_pos[f][b] += team->fault_state.active[f] ? 1u : 0u;
                s_calib.fused_sum[f][b] += p[f];
            }
        }
#endif
    }
}

/* ----------------------------------------------------------------------
 * Layer A
 * ---------------------------------------------------------------------- */
int planner_team_decide(const TeamState_t *team, const PlannerEvidence_t *ev, RecoveryStatus_t *out)
{
    PlannerTeamState_t *s = &s_team[team->team_id];
    float p[3], p_ai[3], p_oth[3];
    uint8_t floor_action = ev->rule_floor;
    uint8_t flags = 0;
    uint8_t floor_goes_home;
    uint8_t planner_go = 0;
    uint8_t planner_fr = 0;    /* Step 3.6: field repair chosen (LOCAL_RECOVERY on a working team) */
    uint8_t prevent = 0;       /* Step 3.6: 1 PREVENT (rest at base), 2 PREVENT_FIELD (in-field maintenance) */
    float prevent_benefit = 0.0f, prevent_cost = 0.0f;
    uint8_t go_action;
    float pmax = 0.0f;
    float cost_stay = 0.0f, cost_go = 0.0f, margin = 0.0f, fault_go = 0.0f, act_go = 0.0f;

    tick_context();
    if(!fault_profile_planner_layer(PLANNER_LAYER_TEAM))
    {
        /* Evidence/calibration-only mode (--planner=none --planner-calib): fuse for the dump, decide nothing. */
        fuse_probabilities(team, ev, p, p_ai, p_oth);
        (void)floor_action;
        return 0;
    }

    if(s->last_status == STATUS_RESTING && team->status != STATUS_RESTING)
        s->rest_end_tick = g_current_tick;
    s->last_status = team->status;

    fuse_probabilities(team, ev, p, p_ai, p_oth);
    for(uint8_t f = 0; f < 3u; f++)
    {
        s->p[f] = p[f];
        pmax = fmaxf_(pmax, p[f]);
    }

    floor_goes_home = (uint8_t)(floor_action == RECOVERY_ACTION_RETURN_TO_SAFE_ZONE ||
                                floor_action == RECOVERY_ACTION_ABORT_MISSION ||
                                (floor_action == RECOVERY_ACTION_LOCAL_RECOVERY && team->status == STATUS_AVAILABLE));
    go_action = (floor_action == RECOVERY_ACTION_ABORT_MISSION) ? RECOVERY_ACTION_ABORT_MISSION
                : (team->status == STATUS_AVAILABLE ? RECOVERY_ACTION_LOCAL_RECOVERY : RECOVERY_ACTION_RETURN_TO_SAFE_ZONE);

    if(ev->wd_flag) flags |= PLANNER_FLAG_FORCED_WD;
    if(ev->cons_elevated) flags |= PLANNER_FLAG_FORCED_CONS;

    if(team->status == STATUS_RESTING || team->heading_to_rest)
    {
        /* Already going or being home: nothing to decide (the rule evaluators' own guards). */
        out->recommendation = (RecoveryAction_t)floor_action;
        out->reason = (RecoveryReason_t)ev->rule_reason;
        out->confidence = 0.0f;
        out->escalation_level = floor_action;
        planner_note_recommendation(team->team_id, out->recommendation);
        return 1;
    }

    if(floor_goes_home)
    {
        /* Simplex floor (D2): the rule-based evaluators alone already ask for the safe action. */
        s_stats.go_home_floor++;
        flags |= PLANNER_FLAG_FORCED_FLOOR;
    }
#if ENVIRONMENT_HAS_WORLD_STORAGE
    else if(fault_profile_causal_persist() && pmax > 0.0f)
    {
        /* Persistent faults: price staying, going home and (with field repair) repairing in place. */
        PersistCosts_t c;
        float gain_go, gain_fr;

        persist_costs(team, p, &c);
        cost_stay = c.stay;
        margin = fmaxf_(2.0f * c.u_x, g_planner.margin_frac * cost_stay);
        cost_go = c.go;
        fault_go = c.go;
        act_go = 0.0f;
        gain_go = cost_stay - c.go;
        gain_fr = cost_stay - c.fr;
        if(cost_stay > margin && g_current_tick >= s->rest_end_tick + g_planner.preempt_cooldown_ticks)
        {
            if(c.fr_class != 255u && fault_profile_causal_field_repair() && gain_fr > margin && gain_fr >= gain_go)
                planner_fr = 1;
            else if(gain_go > margin)
                planner_go = 1;
        }
        if(planner_fr)
        {
            cost_go = c.fr;
            fault_go = c.fr;
        }

        if(planner_go)
        {
            uint8_t ai_only = (uint8_t)(!ev->wd_flag && !ev->cons_elevated && !ev->streak_ok);

            if(ai_only && g_planner.p_source == PLANNER_P_AI && team->status != STATUS_AVAILABLE)
            {
                uint8_t from_ai = 0;

                for(uint8_t f = 0; f < 3u; f++)
                    if(p_ai[f] > p_oth[f] && p[f] >= 0.5f * pmax)
                        from_ai = 1;
                if(from_ai)
                {
                    planner_go = 0;
                    flags |= PLANNER_FLAG_AI_CAP;
                    s_stats.ai_cap_blocks++;
                }
            }
        }
        if(planner_go && is_last_specialist(team))
        {
            if(!request_approval(&flags))
                planner_go = 0;
        }
        if(planner_go)
            s_stats.go_home_planner++;
        else if(planner_fr)
            s_stats.fr_chosen++;
        else if(pmax >= g_planner.p_log_floor)
            s_stats.stay_with_p++;
    }
#endif
    else if(!fault_profile_causal_persist() && (pmax >= g_planner.p_log_floor || pmax > 0.0f))
    {
        const IncidentState_t *task = team_task(team);
        float travel, work, remaining, u_x, d_stay[3], sev[3];
        float harm_stay = 0.0f, harm_go = 0.0f;
        float t_leg = 0.0f, t_rest = 0.0f;
        uint8_t zone;

        task_remaining(team, task, &travel, &work);
        if(task == NULL)
        {
            /* An AVAILABLE team will take a task soon: price its next stage. */
            travel = g_planner.ref_travel_ticks;
            work = g_planner.ref_stage_ticks - g_planner.ref_travel_ticks;
        }
        remaining = fmaxf_(travel + work, 1.0f);
        u_x = (task != NULL) ? incident_urgency(task) : s_ctx.ubar;

        for(uint8_t f = 0; f < 3u; f++)
        {
            d_stay[f] = ((float)g_fault_config.duration_min_ticks[f] + (float)g_fault_config.duration_max_ticks[f]) / 2.0f;
            sev[f] = (float)g_fault_config.effect_strength[f];
        }

        /* C_fault(stay, f): extra ticks the fault costs the remaining task, from its documented effect
         * (core/simulation.c move_team, core/incident.c work rate): mobility severity >= 4 moves one
         * tick in 3, >= 3 one in 2, and cuts the work rate by its severity; resource cuts the work
         * rate by its severity; a communication fault costs consensus visibility (cblind IT/tick). */
        {
            float base_rate = (float)team_base_work_rate(team->equipment_level);
            float w_frac = work / remaining, t_frac = travel / remaining;

            for(uint8_t f = 0; f < 3u; f++)
            {
                float active_ticks = fminf_(remaining, d_stay[f]);
                float slow_work = (base_rate > sev[f]) ? sev[f] / (base_rate - sev[f]) : sev[f];
                float slow_trav = (sev[f] >= 4.0f) ? 2.0f : (sev[f] >= 3.0f ? 1.0f : 0.0f);
                float harm;

                if(f == 0u)
                    harm = active_ticks * (t_frac * slow_trav + w_frac * slow_work) * u_x;
                else if(f == 2u)
                    harm = active_ticks * w_frac * slow_work * u_x;
                else
                    harm = fminf_(d_stay[f], 10.0f) * g_planner.cblind;
                harm_stay += p[f] * harm;
                {
                    float repair = (f == 0u ? g_planner.repair_mobility : (f == 1u ? g_planner.repair_comm : g_planner.repair_resource)) *
                                   g_planner.repair_scale;

                    if(repair > 1.0f) repair = 1.0f;
                    harm_go += p[f] * harm * (1.0f - repair);
                }
            }
        }
        cost_stay = harm_stay;

        margin = fmaxf_(2.0f * u_x, g_planner.margin_frac * cost_stay);
        fault_go = harm_go;
        if(cost_stay > margin)
        {
            /* Only now is the (path-search) price of going home worth computing: it cannot win otherwise. */
            zone = find_nearest_safe_zone(team);
            if(zone != NO_SAFE_ZONE)
            {
                uint16_t steps = find_path_steps(team->loc_x, team->loc_y, g_safezones[zone].loc_x, g_safezones[zone].loc_y);

                t_leg = (steps == UINT16_MAX) ? 200.0f : (float)steps;
            }
            t_rest = rest_ticks_needed(team);
            act_go = (t_leg + t_rest) * s_ctx.q + ((task != NULL) ? u_x * g_planner.reassign_delay : 0.0f);
            cost_go = fault_go + act_go;
            planner_go = (uint8_t)((cost_stay - cost_go) > margin && g_current_tick >= s->rest_end_tick + g_planner.preempt_cooldown_ticks);
        }
        else
            cost_go = cost_stay;   /* going home cannot beat staying by the margin */

        if(planner_go)
        {
            uint8_t ai_only = (uint8_t)(!ev->wd_flag && !ev->cons_elevated && !ev->streak_ok);

            /* AI-only evidence stays capped at LOCAL_RECOVERY (README rule): LOCAL only acts on an AVAILABLE team. */
            if(ai_only && g_planner.p_source == PLANNER_P_AI && team->status != STATUS_AVAILABLE)
            {
                uint8_t from_ai = 0;

                for(uint8_t f = 0; f < 3u; f++)
                    if(p_ai[f] > p_oth[f] && p[f] >= 0.5f * pmax)
                        from_ai = 1;
                if(from_ai)
                {
                    planner_go = 0;
                    flags |= PLANNER_FLAG_AI_CAP;
                    s_stats.ai_cap_blocks++;
                }
            }
        }
        if(planner_go && is_last_specialist(team))
        {
            if(!request_approval(&flags))
                planner_go = 0;
        }
        if(planner_go)
            s_stats.go_home_planner++;
        else if(pmax >= g_planner.p_log_floor)
            s_stats.stay_with_p++;
    }

#if ENVIRONMENT_HAS_WORLD_STORAGE
    /* A field repair in progress is not interrupted by the planner itself (the rule floor still can). */
    if(!floor_goes_home && !planner_go && !planner_fr && fault_injector_causal_field_state(team->team_id) != 0u &&
       fault_injector_causal_field_state(team->team_id) != 3u)
        planner_fr = 1;

    /* Preventive options: only with a forecast source (the hazard oracle) and an option enabled. */
    if(!floor_goes_home && !planner_go && !planner_fr && g_planner.prevent != 0u && fault_profile_causal_persist() &&
       (g_planner.p_source == PLANNER_P_HAZARD || g_planner.p_source == PLANNER_P_BOTH) &&
       in_service(team) && fault_injector_causal_field_state(team->team_id) == 0u &&
       g_current_tick >= s->rest_end_tick + g_planner.preempt_cooldown_ticks && g_current_tick >= s->prevent_until)
    {
        prevent = persist_prevent_choice(team, &prevent_benefit, &prevent_cost);
        if(prevent == 1u && is_last_specialist(team) && !request_approval(&flags))
            prevent = 0;
        if(prevent == 1u)
        {
            s_stats.prevent_rest++;
            s->prevent_until = g_current_tick + (uint32_t)g_causal_persist_params.forecast_horizon_ticks;
            fault_injector_causal_prevent_order(team->team_id);
            planner_go = 1;
        }
        else if(prevent == 2u)
        {
            if(fault_injector_causal_field_start(team->team_id, 3u))
            {
                s_stats.prevent_field++;
                s->prevent_until = g_current_tick + (uint32_t)g_causal_persist_params.forecast_horizon_ticks;
            }
            else
                prevent = 0;
        }
    }
#endif
    (void)prevent_benefit;   /* only the laptop's preventive options use these */
    (void)prevent_cost;
    (void)prevent;

    {
        uint8_t go = (uint8_t)(floor_goes_home || planner_go);
        RecoveryAction_t action;
        RecoveryReason_t reason;

        if(go)
        {
            action = (RecoveryAction_t)go_action;
            reason = floor_goes_home ? (RecoveryReason_t)ev->rule_reason : RECOVERY_REASON_PREDICTED_FAULT;
        }
        else if(planner_fr)
        {
            action = RECOVERY_ACTION_LOCAL_RECOVERY;
            reason = RECOVERY_REASON_PREDICTED_FAULT;
        }
        else if(floor_action == RECOVERY_ACTION_REQUEST_TASK_REALLOCATION)
        {
            action = RECOVERY_ACTION_REQUEST_TASK_REALLOCATION;
            reason = (RecoveryReason_t)ev->rule_reason;
        }
        else
        {
            action = RECOVERY_ACTION_CONTINUE_MONITORING;
            reason = RECOVERY_REASON_NONE;
        }
        out->recommendation = action;
        out->reason = reason;
        out->confidence = floor_goes_home ? 1.0f : pmax;
        out->escalation_level = (uint8_t)action;
        planner_note_recommendation(team->team_id, (uint8_t)action);

        if(action != RECOVERY_ACTION_CONTINUE_MONITORING || pmax >= g_planner.p_log_floor)
        {
            PlannerDecision_t d;

            d.tick = g_current_tick;
            d.team = team->team_id;
            d.layer = 0;
            d.chosen = (uint8_t)action;
            d.alt = (uint8_t)(go ? RECOVERY_ACTION_CONTINUE_MONITORING : go_action);
            for(uint8_t f = 0; f < 3u; f++) { d.p[f] = p[f]; d.p_ai[f] = p_ai[f]; }
            d.cost_fault = go ? fault_go : cost_stay;
            d.cost_action = go ? act_go : 0.0f;
            d.cost_alt = go ? cost_stay : cost_go;
            d.margin = margin;
            d.flags = flags;
            d.target_x = d.target_y = -1;
            d.aux = -1;
            d.aux_cost = 0.0f;
            s_stats.decisions++;
            log_decision(&d);
        }
    }
    return 1;
}

/* ----------------------------------------------------------------------
 * Layer B and preemption
 * ---------------------------------------------------------------------- */
#if ENVIRONMENT_HAS_WORLD_STORAGE

/* Start tracking a team that has just taken a job; settle jobs that ended. */
static void settle_jobs(void)
{
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        const TeamState_t *team = &g_teams[t];
        PlannerPreempt_t *p = &s_pre[t];

        if(!p->active)
        {
            /* a job taken through the normal allocator */
            if(team->assigned_task_pool == TASK_POOL_SYSTEM && team->assigned_task_slot < NUM_SYSTEM_TASKS &&
               g_system_tasks[team->assigned_task_slot].slot_occupied &&
               g_system_tasks[team->assigned_task_slot].incident_type == INCIDENT_CLEAR_ROAD &&
               g_system_tasks[team->assigned_task_slot].assigned_team == t)
            {
                p->active = 1;
                p->kind = CAUSAL_ACTION_CLEAR_ROAD;
                p->slot = team->assigned_task_slot;
                p->id = g_system_tasks[team->assigned_task_slot].incident_id;
            }
            else if(team->assigned_task_pool == TASK_POOL_INCIDENT && team->assigned_task_slot < NUM_INCIDENTS &&
                    g_incidents[team->assigned_task_slot].slot_occupied && g_incidents[team->assigned_task_slot].support_team == t)
            {
                p->active = 1;
                p->kind = CAUSAL_ACTION_REINFORCE_INCIDENT;
                p->slot = team->assigned_task_slot;
                p->id = g_incidents[team->assigned_task_slot].incident_id;
            }
            continue;
        }
        if(p->kind == CAUSAL_ACTION_CLEAR_ROAD)
        {
            const IncidentState_t *job = &g_system_tasks[p->slot];

            if(!job->slot_occupied || job->incident_id != p->id || job->status == INC_RESOLVED)
            {
                s_stats.preempt_succeeded[p->kind]++;
                p->active = 0;
            }
            else if(team->assigned_task_pool != TASK_POOL_SYSTEM || team->assigned_task_slot != p->slot)
            {
                s_stats.preempt_failed[p->kind]++;
                p->active = 0;
            }
        }
        else
        {
            const IncidentState_t *job = &g_incidents[p->slot];

            if(!job->slot_occupied || job->incident_id != p->id || job->status == INC_RESOLVED)
            {
                s_stats.preempt_succeeded[p->kind]++;
                p->active = 0;
            }
            else if(team->assigned_task_pool != TASK_POOL_INCIDENT || team->assigned_task_slot != p->slot ||
                    job->support_team != t)
            {
                /* reinforcement ended: the incident outlived its support team's stay (hold / recovery) */
                s_stats.preempt_failed[p->kind]++;
                p->active = 0;
            }
        }
    }
}

/* Expected remaining life of a damaged cell that has survived `age` ticks, under the uniform
 * persistence range the road model uses (docs/CONSEQUENTIAL_ROADS.md: 600-2400 ticks, assumed). */
static float expected_damage_remaining(float age)
{
    float lo = (float)g_causal_roads.persist_min_ticks, hi = (float)g_causal_roads.persist_max_ticks;

    if(age < lo)
        return (lo + hi) / 2.0f - age;
    return fmaxf_((hi - age) / 2.0f, 0.0f);
}

static void log_b(uint8_t action, const TeamState_t *team, float benefit, float cost, float margin, int x, int y)
{
    PlannerDecision_t d;

    d.tick = g_current_tick;
    d.team = team != NULL ? team->team_id : 255u;
    d.layer = 1;
    d.chosen = action;
    d.alt = 255u;
    d.p[0] = d.p[1] = d.p[2] = 0.0f;
    d.p_ai[0] = d.p_ai[1] = d.p_ai[2] = 0.0f;
    d.cost_fault = benefit;
    d.cost_action = cost;
    d.cost_alt = 0.0f;
    d.margin = margin;
    d.flags = 0;
    d.target_x = (int16_t)x;
    d.target_y = (int16_t)y;
    d.aux = -1;
    d.aux_cost = 0.0f;
    log_decision(&d);
}

static float clear_benefit(uint8_t kind, float touch_rate, float age, float t_clear)
{
    float delay = (kind == 2u) ? g_planner.blocked_detour : (float)g_causal_roads.crossing_delay_ticks * g_planner.crossing_scale;
    float horizon = fmaxf_(0.0f, expected_damage_remaining(age) - t_clear);

    return touch_rate * horizon * delay * s_ctx.ubar;
}

static float clear_work_ticks(uint8_t kind)
{
    float work = (kind == 2u) ? (float)g_causal_roads.clear_work_full : (float)g_causal_roads.clear_work_partial;

    return work / ref_work_rate();
}

int planner_clear_road_wanted(int x, int y, uint8_t kind, float touch_rate, uint32_t faulted_age_ticks)
{
    float t_clear, benefit, cost;
    int wanted;

    if(!fault_profile_planner_layer(PLANNER_LAYER_TASKS))
        return 0;
    tick_context();
    t_clear = g_planner.ref_travel_ticks + clear_work_ticks(kind);
    benefit = success_rate(CAUSAL_ACTION_CLEAR_ROAD) * clear_benefit(kind, touch_rate, (float)faulted_age_ticks, t_clear);
    cost = t_clear * s_ctx.q;
    wanted = benefit > 0.0f && benefit > cost * (1.0f + g_planner.margin_frac);
    if(wanted)
    {
        s_stats.wanted[CAUSAL_ACTION_CLEAR_ROAD]++;
        log_b(CAUSAL_ACTION_CLEAR_ROAD, NULL, benefit, cost, g_planner.margin_frac * cost, x, y);
    }
    else if(benefit > 0.0f)
        s_stats.refused[CAUSAL_ACTION_CLEAR_ROAD]++;
    return wanted;
}

int planner_reinforce_wanted(uint8_t slot)
{
    const IncidentState_t *inc;
    float age, rem_work, hold, t_dur, benefit, cost, rate;

    if(!fault_profile_planner_layer(PLANNER_LAYER_TASKS) || slot >= NUM_INCIDENTS)
        return 0;
    inc = &g_incidents[slot];
    if(!inc->slot_occupied || inc->victim_count == 0u || inc->assigned_team >= NUM_TEAMS)
        return 0;
    tick_context();
    age = (g_current_tick > inc->reported_at_tick) ? (float)(g_current_tick - inc->reported_at_tick) : 0.0f;
    rate = (float)team_effective_work_rate(&g_teams[inc->assigned_team]);
    if(rate < 1.0f)
        rate = 1.0f;
    rem_work = (float)inc->work_remaining / rate;
    hold = (float)g_causal_actions.reinforce_hold_ticks;
    t_dur = fminf_(hold, rem_work);
    benefit = success_rate(CAUSAL_ACTION_REINFORCE_INCIDENT) *
              ((1.0f - g_causal_actions.reinforce_survival_factor) * (float)inc->victim_count * hazard_per_tick(age) * planner_v() * t_dur +
               (1.0f - 1.0f / g_causal_actions.reinforce_work_boost) * rem_work * incident_urgency(inc));
    cost = (g_planner.ref_travel_ticks + t_dur) * s_ctx.q;
    if(benefit > 0.0f && benefit > cost * (1.0f + g_planner.margin_frac))
    {
        s_stats.wanted[CAUSAL_ACTION_REINFORCE_INCIDENT]++;
        log_b(CAUSAL_ACTION_REINFORCE_INCIDENT, &g_teams[inc->assigned_team], benefit, cost, g_planner.margin_frac * cost, inc->loc_x, inc->loc_y);
        return 1;
    }
    s_stats.refused[CAUSAL_ACTION_REINFORCE_INCIDENT]++;
    return 0;
}

int planner_relay_wanted(const TeamState_t *team)
{
    const IncidentState_t *task;
    float benefit, cost, u_x;

    if(!fault_profile_planner_layer(PLANNER_LAYER_TASKS))
        return 0;
    tick_context();
    task = team_task(team);
    u_x = (task != NULL) ? incident_urgency(task) : s_ctx.ubar;
    benefit = s_team[team->team_id].p[1] * g_planner.cblind * fminf_(((float)g_fault_config.duration_min_ticks[1] + (float)g_fault_config.duration_max_ticks[1]) / 2.0f, 10.0f);
    cost = ((float)g_causal_actions.relay_search_radius / 2.0f) * u_x;
    if(benefit > cost * (1.0f + g_planner.margin_frac))
    {
        s_stats.wanted[CAUSAL_ACTION_RELAY_REPOSITION]++;
        log_b(CAUSAL_ACTION_RELAY_REPOSITION, team, benefit, cost, g_planner.margin_frac * cost, team->loc_x, team->loc_y);
        return 1;
    }
    s_stats.refused[CAUSAL_ACTION_RELAY_REPOSITION]++;
    return 0;
}

int planner_reroute_wanted(const TeamState_t *team)
{
    const IncidentState_t *task;
    float gain, u_x;

    if(!fault_profile_planner_layer(PLANNER_LAYER_TASKS))
        return 0;
    /* True-cost A* already sees the damage: the overlay is 0, its benefit exactly 0. */
    if(g_causal_roads.route_cost_visible)
    {
        s_stats.refused[CAUSAL_ACTION_REROUTE]++;
        return 0;
    }
    tick_context();
    task = team_task(team);
    u_x = (task != NULL) ? incident_urgency(task) : s_ctx.ubar;
    gain = causal_actions_reroute_gain(team);   /* stall ticks avoided minus detour steps */
    if(gain > g_planner.margin_frac)
    {
        s_stats.wanted[CAUSAL_ACTION_REROUTE]++;
        log_b(CAUSAL_ACTION_REROUTE, team, gain * u_x, 0.0f, g_planner.margin_frac * u_x, team->loc_x, team->loc_y);
        return 1;
    }
    s_stats.refused[CAUSAL_ACTION_REROUTE]++;
    return 0;
}

static uint16_t cheb(int ax, int ay, int bx, int by)
{
    int dx = ax > bx ? ax - bx : bx - ax, dy = ay > by ? ay - by : by - ay;

    return (uint16_t)(dx > dy ? dx : dy);
}

/* Best busy team to pull onto a job at (tx, ty). Returns the team id or NO_ASSIGNMENT.
 * `benefit` is the job's value, `job_ticks` its work ticks, `support_only` restricts to SUPPORT teams. */
static uint8_t pick_preempt_team(int tx, int ty, float benefit, float job_ticks, uint8_t support_only, float *net_out,
                                 float *switch_out, int *denied_by_approval)
{
    uint8_t cand[NUM_TEAMS];
    uint16_t dist[NUM_TEAMS];
    uint8_t n = 0;
    uint8_t best = NO_ASSIGNMENT;
    float best_net = 0.0f, best_switch = 0.0f;

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        const TeamState_t *team = &g_teams[t];

        if(team->status != STATUS_EN_ROUTE && team->status != STATUS_ON_SCENE)
            continue;
        if(team->heading_to_rest || s_team[t].preempt_until > g_current_tick + 1u)
            continue;
        if(support_only && team->specialization != SPEC_SUPPORT)
            continue;
        cand[n] = t;
        dist[n] = cheb(team->loc_x, team->loc_y, tx, ty);
        n++;
    }
    /* nearest preempt_k by Chebyshev distance (selection, deterministic by id on ties) */
    for(uint8_t k = 0; k < g_planner.preempt_k && k < n; k++)
    {
        uint8_t m = k;

        for(uint8_t i = (uint8_t)(k + 1u); i < n; i++)
            if(dist[i] < dist[m])
                m = i;
        if(m != k)
        {
            uint8_t tc = cand[k]; uint16_t td = dist[k];

            cand[k] = cand[m]; dist[k] = dist[m]; cand[m] = tc; dist[m] = td;
        }
    }
    /* Price every candidate, then take the best whose last-specialist approval (D3) is granted. */
    {
        float nets[NUM_TEAMS], sws[NUM_TEAMS];
        uint8_t ok[NUM_TEAMS];

        for(uint8_t k = 0; k < g_planner.preempt_k && k < n; k++)
        {
            const TeamState_t *team = &g_teams[cand[k]];
            const IncidentState_t *task = team_task(team);
            uint16_t steps;
            float u_x, t_job, switch_cost, margin, net, travel, work;

            ok[k] = 0;
            s_stats.preempt_considered++;
            steps = find_path_steps(team->loc_x, team->loc_y, (int)tx, (int)ty);
            if(steps == UINT16_MAX)
                continue;
            u_x = (task != NULL) ? incident_urgency(task) : s_ctx.ubar;
            task_remaining(team, task, &travel, &work);
            /* Let a team finish first when it would be done before it could get there. */
            if(team->status == STATUS_ON_SCENE && work <= (float)steps)
            {
                s_stats.preempt_blocked_finish++;
                continue;
            }
            t_job = (float)steps + job_ticks;
            switch_cost = u_x * g_planner.reassign_delay + t_job * s_ctx.q;
            margin = fmaxf_(u_x * (float)steps, g_planner.margin_frac * switch_cost);
            net = benefit - switch_cost - margin;
            if(net <= 0.0f)
            {
                s_stats.preempt_blocked_cost++;
                continue;
            }
            ok[k] = 1;
            nets[k] = net;
            sws[k] = switch_cost;
        }
        for(;;)
        {
            int m = -1;

            for(uint8_t k = 0; k < g_planner.preempt_k && k < n; k++)
                if(ok[k] && (m < 0 || nets[k] > nets[m]))
                    m = k;
            if(m < 0)
                break;
            if(is_last_specialist(&g_teams[cand[m]]))
            {
                uint8_t f = 0;

                if(!request_approval(&f))
                {
                    s_stats.preempt_blocked_approval++;
                    if(denied_by_approval != NULL)
                        *denied_by_approval = 1;
                    ok[m] = 0;
                    continue;
                }
            }
            best = cand[m];
            best_net = nets[m];
            best_switch = sws[m];
            break;
        }
    }
    *net_out = best_net;
    *switch_out = best_switch;
    return best;
}

void planner_preempt_step(void)
{
    if(fault_profile_planner_layer(PLANNER_LAYER_TASKS) || fault_profile_planner_layer(PLANNER_LAYER_PREEMPT))
        settle_jobs();   /* learn from every clearing / reinforcement job, whoever started it */
    if(!fault_profile_planner_layer(PLANNER_LAYER_PREEMPT))
        return;
    tick_context();

    /* CLEAR_ROAD tasks nobody free will take. */
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        IncidentState_t *task = &g_system_tasks[i];
        int cx, cy;
        uint8_t kind, who;
        float benefit, net, sw;
        int denied = 0;

        if(!task->slot_occupied || task->status != INC_OPEN || task->incident_type != INCIDENT_CLEAR_ROAD ||
           task->unreachable_streak >= INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD)
            continue;
        if(!causal_actions_clear_target(i, &cx, &cy))
            continue;
        if(select_best_team(task) != NO_ASSIGNMENT)
            continue;   /* a free team will be allocated normally */
        kind = fault_injector_causal_road_kind(cx, cy);
        benefit = success_rate(CAUSAL_ACTION_CLEAR_ROAD) *
                  clear_benefit(kind, causal_actions_cell_touch_rate(cx, cy),
                                (float)causal_actions_faulted_age(cx, cy), g_planner.ref_travel_ticks + clear_work_ticks(kind));
        if(benefit <= 0.0f)
            continue;
        who = pick_preempt_team(task->loc_x, task->loc_y, benefit, clear_work_ticks(kind), 0, &net, &sw, &denied);
        if(who == NO_ASSIGNMENT)
            continue;
        if(planner_release_team_for_preempt(&g_teams[who]) && allocator_assign_system_task(who, i))
        {
            PlannerDecision_t d;

            s_team[who].preempt_until = g_current_tick + 1u + g_planner.preempt_cooldown_ticks;
            s_pre[who].active = 1;
            s_pre[who].kind = CAUSAL_ACTION_CLEAR_ROAD;
            s_pre[who].slot = i;
            s_pre[who].id = task->incident_id;
            s_stats.preempt_done[CAUSAL_ACTION_CLEAR_ROAD]++;
            s_stats.preempt_net_sum_x100 += (uint32_t)(net * 100.0f);
            d.tick = g_current_tick; d.team = who; d.layer = 2; d.chosen = CAUSAL_ACTION_CLEAR_ROAD; d.alt = 255u;
            d.p[0] = d.p[1] = d.p[2] = 0.0f; d.p_ai[0] = d.p_ai[1] = d.p_ai[2] = 0.0f;
            d.cost_fault = benefit; d.cost_action = sw; d.cost_alt = 0.0f; d.margin = net; d.flags = 0;
            d.target_x = (int16_t)cx; d.target_y = (int16_t)cy; d.aux = (int16_t)i; d.aux_cost = sw;
            log_decision(&d);
        }
    }

    /* REINFORCE: an incident whose reinforcement was wanted and which no free SUPPORT team has taken. */
    for(uint8_t slot = 0; slot < NUM_INCIDENTS; slot++)
    {
        IncidentState_t *inc = &g_incidents[slot];
        uint8_t who, free_support = 0;
        float benefit, net, sw, rate, rem_work, t_dur, age;
        int denied = 0;

        if(!inc->slot_occupied || inc->status == INC_RESOLVED || inc->victim_count == 0u || inc->assigned_team >= NUM_TEAMS ||
           inc->support_team != NO_ASSIGNMENT || !causal_actions_reinforce_wanted(slot))
            continue;
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
            if(g_teams[t].specialization == SPEC_SUPPORT && g_teams[t].status == STATUS_AVAILABLE)
                free_support = 1;
        if(free_support)
            continue;
        age = (g_current_tick > inc->reported_at_tick) ? (float)(g_current_tick - inc->reported_at_tick) : 0.0f;
        rate = (float)team_effective_work_rate(&g_teams[inc->assigned_team]);
        if(rate < 1.0f)
            rate = 1.0f;
        rem_work = (float)inc->work_remaining / rate;
        t_dur = fminf_((float)g_causal_actions.reinforce_hold_ticks, rem_work);
        benefit = success_rate(CAUSAL_ACTION_REINFORCE_INCIDENT) *
                  ((1.0f - g_causal_actions.reinforce_survival_factor) * (float)inc->victim_count * hazard_per_tick(age) * planner_v() * t_dur +
                   (1.0f - 1.0f / g_causal_actions.reinforce_work_boost) * rem_work * incident_urgency(inc));
        who = pick_preempt_team(inc->loc_x, inc->loc_y, benefit, t_dur, 1, &net, &sw, &denied);
        if(who == NO_ASSIGNMENT)
            continue;
        if(planner_release_team_for_preempt(&g_teams[who]) && allocator_assign_support(who, slot))
        {
            PlannerDecision_t d;

            s_team[who].preempt_until = g_current_tick + 1u + g_planner.preempt_cooldown_ticks;
            s_pre[who].active = 1;
            s_pre[who].kind = CAUSAL_ACTION_REINFORCE_INCIDENT;
            s_pre[who].slot = slot;
            s_pre[who].id = inc->incident_id;
            s_stats.preempt_done[CAUSAL_ACTION_REINFORCE_INCIDENT]++;
            s_stats.preempt_net_sum_x100 += (uint32_t)(net * 100.0f);
            d.tick = g_current_tick; d.team = who; d.layer = 2; d.chosen = CAUSAL_ACTION_REINFORCE_INCIDENT; d.alt = 255u;
            d.p[0] = d.p[1] = d.p[2] = 0.0f; d.p_ai[0] = d.p_ai[1] = d.p_ai[2] = 0.0f;
            d.cost_fault = benefit; d.cost_action = sw; d.cost_alt = 0.0f; d.margin = net; d.flags = 0;
            d.target_x = inc->loc_x; d.target_y = inc->loc_y; d.aux = (int16_t)slot; d.aux_cost = sw;
            log_decision(&d);
        }
    }
}

#else /* board: no world storage, no Layer B / preemption */

int planner_clear_road_wanted(int x, int y, uint8_t kind, float touch_rate, uint32_t faulted_age_ticks)
{ (void)x; (void)y; (void)kind; (void)touch_rate; (void)faulted_age_ticks; return 0; }
int planner_reinforce_wanted(uint8_t incident_slot) { (void)incident_slot; return 0; }
int planner_relay_wanted(const TeamState_t *team) { (void)team; return 0; }
int planner_reroute_wanted(const TeamState_t *team) { (void)team; return 0; }
void planner_preempt_step(void) {}

#endif

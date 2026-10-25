#include "road_knowledge.h"
#include "causal_actions.h"

#include <stddef.h>

#include "environment.h"
#include "fault_injector.h"
#include "incident.h"
#include "path.h"
#include "recovery_planner.h"

static CausalActionStats_t s_stats;

#if ENVIRONMENT_HAS_WORLD_STORAGE

#define CELLS (GRID_WIDTH * GRID_HEIGHT)

/* REROUTE */
static uint8_t s_reroute_on[NUM_TEAMS];
static uint32_t s_reroute_start[NUM_TEAMS];
static uint8_t s_avoid[CELLS];                 /* per-cell overlay handed to the pathfinder */
static uint8_t s_damaged[CELLS];               /* 1 = faulted or blocked road cell (the overlay may be 0) */
static uint8_t s_blockers[CELLS];              /* teams whose route crosses the cell this tick */
static uint32_t s_faulted_since[CELLS];        /* tick + 1 the cell became faulted; 0 = not faulted */
/* CLEAR_ROAD */
static uint8_t s_clear_requested[CELLS];       /* the detector asked for this cell */
/* CLEAR_ROAD task slot -> the road cell it clears (the task itself stands on a
 * navigable cell, which for a fully blocked road is a neighbour). */
static int16_t s_clear_tx[NUM_SYSTEM_TASKS];
static int16_t s_clear_ty[NUM_SYSTEM_TASKS];
static uint32_t s_clear_id[NUM_SYSTEM_TASKS];
/* REINFORCE_INCIDENT, per incident slot, valid while the id matches */
static uint32_t s_inc_id[NUM_INCIDENTS];
static uint8_t s_prev_victims[NUM_INCIDENTS];
static uint8_t s_prev_priority[NUM_INCIDENTS];
static uint32_t s_deteriorated_at[NUM_INCIDENTS];   /* tick + 1; 0 = never */
static uint32_t s_reinforce_until[NUM_INCIDENTS];   /* tick + 1; 0 = no request */
static uint8_t s_reinforce_counted[NUM_INCIDENTS];
static uint8_t s_reinforce_detector[NUM_INCIDENTS];
/* RELAY_REPOSITION */
static uint8_t s_relay_on[NUM_TEAMS];
static int16_t s_relay_x[NUM_TEAMS];
static int16_t s_relay_y[NUM_TEAMS];
static uint32_t s_relay_start[NUM_TEAMS];
/* Detector requests, consumed by the next causal_actions_step(). */
static uint8_t s_pending_env[NUM_TEAMS];
static uint32_t s_relay_cooldown_until[NUM_TEAMS];  /* tick + 1 */
static uint8_t s_stall[NUM_TEAMS];
static uint32_t s_stall_total;
/* The damaged cell that makes a team's route-touch label true (-1 = none). */
/* Ticks at which each team's step was held by a debris crossing (recent ring), for the stall-aware evaluators. */
#define HOLD_RING 16
static uint32_t s_hold_ring[NUM_TEAMS][HOLD_RING];   /* tick + 1, 0 = empty */
static uint8_t s_hold_head[NUM_TEAMS];
static float s_touch_rate[CELLS];                /* EWMA of teams whose route touches the cell, per tick (planner) */
static int16_t s_label_px[NUM_TEAMS];            /* previous tick's label cell, to count episodes */
static int16_t s_label_py[NUM_TEAMS];
static int16_t s_label_x[NUM_TEAMS];
static int16_t s_label_y[NUM_TEAMS];

static int cell_index(int x, int y)
{
    return x * GRID_HEIGHT + y;
}

static int in_grid(int x, int y)
{
    return x >= 0 && x < GRID_WIDTH && y >= 0 && y < GRID_HEIGHT;
}

void causal_actions_reset(void)
{
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        s_reroute_on[t] = 0;
        s_reroute_start[t] = 0;
        s_relay_on[t] = 0;
        s_relay_start[t] = 0;
        s_pending_env[t] = 0;
        s_relay_cooldown_until[t] = 0;
        s_stall[t] = 0;
        s_label_px[t] = -1;
        s_label_py[t] = -1;
        s_hold_head[t] = 0;
        for(int h = 0; h < HOLD_RING; h++)
            s_hold_ring[t][h] = 0;
    }
    for(int i = 0; i < CELLS; i++)
    {
        s_avoid[i] = 0;
        s_damaged[i] = 0;
        s_blockers[i] = 0;
        s_faulted_since[i] = 0;
        s_clear_requested[i] = 0;
        s_touch_rate[i] = 0.0f;
    }
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        s_clear_tx[i] = 0;
        s_clear_ty[i] = 0;
        s_clear_id[i] = 0;
    }
    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        s_inc_id[i] = 0;
        s_prev_victims[i] = 0;
        s_prev_priority[i] = 0;
        s_deteriorated_at[i] = 0;
        s_reinforce_until[i] = 0;
        s_reinforce_counted[i] = 0;
        s_reinforce_detector[i] = 0;
    }
    /* Statistics are process-lifetime, like the g_diag_* counters. */
}

const CausalActionStats_t *causal_actions_stats(void)
{
    return &s_stats;
}

/* Where the team is heading right now, if anywhere. */
static int team_destination(const TeamState_t *team, int *dx, int *dy)
{
    if(team->status != STATUS_EN_ROUTE)
        return 0;

    if(team->heading_to_rest)
    {
        if(team->resting_zone_id >= MAX_SAFE_ZONES)
            return 0;
        *dx = g_safezones[team->resting_zone_id].loc_x;
        *dy = g_safezones[team->resting_zone_id].loc_y;
        return 1;
    }

    {
        const IncidentState_t *task = NULL;
        if(team->assigned_task_pool == TASK_POOL_INCIDENT && team->assigned_task_slot < NUM_INCIDENTS)
            task = &g_incidents[team->assigned_task_slot];
        else if(team->assigned_task_pool == TASK_POOL_SYSTEM && team->assigned_task_slot < NUM_SYSTEM_TASKS)
            task = &g_system_tasks[team->assigned_task_slot];
        if(task == NULL || !task->slot_occupied)
            return 0;
        *dx = task->loc_x;
        *dy = task->loc_y;
    }
    return 1;
}

/* Walks the team's current (un-penalised) route and returns the first cell
 * carrying an overlay penalty, or 0. */
static int route_crosses_faulted(const TeamState_t *team, int *hit_x, int *hit_y)
{
    int x = team->loc_x, y = team->loc_y, dx, dy;

    if(!team_destination(team, &dx, &dy))
        return 0;

    for(uint8_t step = 0; step < g_causal_actions.reroute_lookahead; step++)
    {
        int nx = x, ny = y;

        if(x == dx && y == dy)
            break;
        if(find_next_path_step(x, y, dx, dy, 0, &nx, &ny) <= 0)
            break;
        x = nx;
        y = ny;
        if(in_grid(x, y) && s_damaged[cell_index(x, y)] != 0)
        {
            *hit_x = x;
            *hit_y = y;
            return 1;
        }
    }
    return 0;
}

static void refresh_cell_state(void)
{
    uint16_t penalty = g_causal_actions.reroute_cost_penalty;

    /* Consequential roads: when A* already sees a damaged cell's true stall
     * cost in its travel_cost (route_cost_visible), a rerouting team needs no
     * extra penalty (it would double-count); when A* is blind to it, a
     * rerouting team gets the cost the stall is worth. */
    if(fault_profile_causal_roads_on())
        penalty = g_causal_roads.route_cost_visible
                      ? 0u
                      : (uint16_t)((uint16_t)g_causal_roads.crossing_delay_ticks * g_causal_roads.cost_per_tick);

    if(penalty > 255u)
        penalty = 255u;

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            const EnvironmentCell_t *cell = &g_environment[x][y];
            int i = cell_index(x, y);
            uint8_t faulted = cell->env_fault_ref_count > 0;

            if(g_road_know.mode != 0u)   /* Step 5: the action layer only knows the cells the fleet knows about */
                faulted = (uint8_t)(faulted && road_knowledge_believed_damaged(x, y));

            s_damaged[i] = (faulted || (cell->road_blocked && g_road_know.mode == 0u)) ? 1u : 0u;
            s_avoid[i] = s_damaged[i] ? (uint8_t)penalty : 0;
            s_blockers[i] = 0;
            if(faulted)
            {
                if(s_faulted_since[i] == 0)
                    s_faulted_since[i] = g_current_tick + 1u;
            }
            else
            {
                s_faulted_since[i] = 0;
                s_clear_requested[i] = 0;
            }
        }
    }
}

static void clear_task_target(uint8_t slot, int *tx, int *ty);

/* Planner: how often teams' routes touch each damaged cell (per tick, one-hour window). */
static void update_touch_rates(void)
{
    static uint8_t touches[CELLS];
    float a = (g_planner.rate_window_ticks > 1.0f) ? 1.0f / g_planner.rate_window_ticks : 1.0f;

    for(int i = 0; i < CELLS; i++)
        touches[i] = 0;
    /* One touch per approach episode (a team's label cell changes to this cell), not one per tick the
     * route stays within the 8-step lookahead of it: that is the crossing rate the clearing pays back. */
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        if(s_label_x[t] >= 0 && in_grid(s_label_x[t], s_label_y[t]) &&
           (s_label_x[t] != s_label_px[t] || s_label_y[t] != s_label_py[t]))
            touches[cell_index(s_label_x[t], s_label_y[t])]++;
        s_label_px[t] = s_label_x[t];
        s_label_py[t] = s_label_y[t];
    }
    for(int i = 0; i < CELLS; i++)
        if(s_damaged[i] != 0 || s_touch_rate[i] > 0.0f)
            s_touch_rate[i] = s_touch_rate[i] * (1.0f - a) + (float)touches[i] * a;
}

float causal_actions_cell_touch_rate(int x, int y)
{
    return in_grid(x, y) ? s_touch_rate[cell_index(x, y)] : 0.0f;
}

uint32_t causal_actions_faulted_age(int x, int y)
{
    uint32_t since;

    if(!in_grid(x, y))
        return 0u;
    since = s_faulted_since[cell_index(x, y)];
    return since != 0u ? g_current_tick + 1u - since : 0u;
}

int causal_actions_clear_target(uint8_t slot, int *x, int *y)
{
    int tx = -1, ty = -1;

    if(slot >= NUM_SYSTEM_TASKS || !g_system_tasks[slot].slot_occupied || g_system_tasks[slot].incident_type != INCIDENT_CLEAR_ROAD)
        return 0;
    clear_task_target(slot, &tx, &ty);
    if(!in_grid(tx, ty))
        return 0;
    *x = tx;
    *y = ty;
    return 1;
}

/* Planner REROUTE: stall ticks the overlay would avoid minus the detour it would cost, walking the
 * team's route with and without the overlay (blind-A* mode only; zero by construction otherwise). */
float causal_actions_reroute_gain(const TeamState_t *team)
{
    int dx, dy;
    unsigned L[2] = { 0, 0 }, Pn[2] = { 0, 0 };

    if(!team_destination(team, &dx, &dy))
        return 0.0f;
    for(int pass = 0; pass < 2; pass++)
    {
        int x = team->loc_x, y = team->loc_y, steps = 0;

        if(pass == 1)
            path_set_extra_cost(s_avoid);
        while(!(x == dx && y == dy) && steps < (int)g_causal_actions.reroute_lookahead * 4)
        {
            int nx = x, ny = y;

            if(find_next_path_step(x, y, dx, dy, 0, &nx, &ny) <= 0)
                break;
            x = nx;
            y = ny;
            steps++;
            if(in_grid(x, y) && fault_injector_causal_road_kind(x, y) == 1u)
                Pn[pass]++;
        }
        L[pass] = (unsigned)steps;
        path_set_extra_cost(NULL);
    }
    return ((float)Pn[0] - (float)Pn[1]) * (float)g_causal_roads.crossing_delay_ticks * g_planner.crossing_scale - ((float)L[1] - (float)L[0]);
}

static void step_reroute(void)
{
    const CausalActionParams_t *k = &g_causal_actions;

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        const TeamState_t *team = &g_teams[t];
        int hx = 0, hy = 0;
        int crossing = route_crosses_faulted(team, &hx, &hy);

        if(crossing)
            s_blockers[cell_index(hx, hy)]++;

        if(s_reroute_on[t])
        {
            s_stats.active_ticks[CAUSAL_ACTION_REROUTE]++;
            if(team->status != STATUS_EN_ROUTE || (g_current_tick - s_reroute_start[t]) > k->reroute_max_ticks)
            {
                s_reroute_on[t] = 0;
                s_stats.abandoned[CAUSAL_ACTION_REROUTE]++;
            }
            else if(!crossing)
            {
                /* The route no longer touches a faulted cell. */
                s_reroute_on[t] = 0;
                s_stats.resolved[CAUSAL_ACTION_REROUTE]++;
            }
        }
        else if(!fault_profile_causal_action_enabled(CAUSAL_ACTION_REROUTE))
        {
            /* disabled: still counted as a blocker above for CLEAR_ROAD */
        }
        else if(crossing && team->status == STATUS_EN_ROUTE &&
                (!fault_profile_planner_layer(PLANNER_LAYER_TASKS) || planner_reroute_wanted(team)))
        {
            s_reroute_on[t] = 1;
            s_reroute_start[t] = g_current_tick;
            s_stats.dispatched[CAUSAL_ACTION_REROUTE]++;
        }
        else if(s_pending_env[t] && team->status == STATUS_EN_ROUTE &&
                (!fault_profile_planner_layer(PLANNER_LAYER_TASKS) || planner_reroute_wanted(team)))
        {
            /* Detector flagged an environment fault on this team: reroute
             * now, without waiting for the route scan to see the cell. */
            s_reroute_on[t] = 1;
            s_reroute_start[t] = g_current_tick;
            s_stats.dispatched[CAUSAL_ACTION_REROUTE]++;
            s_stats.from_detector[CAUSAL_ACTION_REROUTE]++;
        }
    }
}

/* The road cell a CLEAR_ROAD task slot is for. */
static void clear_task_target(uint8_t slot, int *tx, int *ty)
{
    const IncidentState_t *task = &g_system_tasks[slot];

    if(s_clear_id[slot] == task->incident_id)
    {
        *tx = s_clear_tx[slot];
        *ty = s_clear_ty[slot];
    }
    else
    {
        *tx = task->loc_x;
        *ty = task->loc_y;
    }
}

static int clear_task_at(int x, int y)
{
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        const IncidentState_t *task = &g_system_tasks[i];
        int tx, ty;

        if(!task->slot_occupied || task->incident_type != INCIDENT_CLEAR_ROAD)
            continue;
        clear_task_target(i, &tx, &ty);
        if(tx == x && ty == y)
            return 1;
    }
    return 0;
}

static uint8_t clear_task_count(void)
{
    uint8_t n = 0;
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
        if(g_system_tasks[i].slot_occupied && g_system_tasks[i].incident_type == INCIDENT_CLEAR_ROAD)
            n++;
    return n;
}

static void step_clear_road(void)
{
    const CausalActionParams_t *k = &g_causal_actions;

    /* Drop tasks whose cell cleared on its own. */
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        IncidentState_t *task = &g_system_tasks[i];

        int tx, ty;

        if(!task->slot_occupied || task->incident_type != INCIDENT_CLEAR_ROAD)
            continue;
        s_stats.active_ticks[CAUSAL_ACTION_CLEAR_ROAD]++;
        clear_task_target(i, &tx, &ty);
        if(in_grid(tx, ty) && g_environment[tx][ty].env_fault_ref_count == 0)
        {
            s_stats.abandoned[CAUSAL_ACTION_CLEAR_ROAD]++;
            incident_cancel_system_task(task);
        }
    }

    /* A cell gets a clearing task once it has stayed faulted for N ticks, or
     * blocks several teams, or the detector asked. */
    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            int i = cell_index(x, y);
            uint8_t persists;

            if(s_faulted_since[i] == 0)
                continue;
            if(clear_task_count() >= k->clear_road_max_tasks)
                return;

            persists = (g_current_tick + 1u - s_faulted_since[i]) > k->clear_road_persist_ticks;
            if(fault_profile_planner_layer(PLANNER_LAYER_TASKS))
            {
                /* Step 3.5: the cost comparison replaces the fixed persistence / blocker-count / detector triggers. */
                if(!planner_clear_road_wanted(x, y, fault_injector_causal_road_kind(x, y), s_touch_rate[i],
                                              g_current_tick + 1u - s_faulted_since[i]))
                    continue;
            }
            else if(!(persists || s_blockers[i] >= k->clear_road_teams_threshold || s_clear_requested[i]))
                continue;
            if(clear_task_at(x, y))
                continue;

            {
                uint8_t kind = fault_injector_causal_road_kind(x, y);
                uint16_t work = k->clear_road_work;
                int sx = x, sy = y;
                int id;

                if(fault_profile_causal_roads_on())
                    work = (kind == 2u) ? g_causal_roads.clear_work_full : g_causal_roads.clear_work_partial;

                /* A fully blocked cell is not a valid place to stand (a route
                 * destination must be navigable): work from a neighbour. */
                if(!environment_cell_is_navigable(x, y))
                {
                    static const int NX[4] = { 1, -1, 0, 0 };
                    static const int NY[4] = { 0, 0, 1, -1 };
                    int found = 0;

                    for(int d = 0; d < 4 && !found; d++)
                    {
                        if(in_grid(x + NX[d], y + NY[d]) && environment_cell_is_navigable(x + NX[d], y + NY[d]))
                        {
                            sx = x + NX[d];
                            sy = y + NY[d];
                            found = 1;
                        }
                    }
                    if(!found)
                        continue;
                }

                id = create_clear_road_task((int16_t)sx, (int16_t)sy, work);
                if(id != NO_ASSIGNMENT)
                {
                    for(uint8_t t = 0; t < NUM_SYSTEM_TASKS; t++)
                    {
                        if(g_system_tasks[t].slot_occupied && (int)g_system_tasks[t].incident_id == id)
                        {
                            s_clear_tx[t] = (int16_t)x;
                            s_clear_ty[t] = (int16_t)y;
                            s_clear_id[t] = g_system_tasks[t].incident_id;
                        }
                    }
                    s_stats.dispatched[CAUSAL_ACTION_CLEAR_ROAD]++;
                    if(s_clear_requested[i])
                        s_stats.from_detector[CAUSAL_ACTION_CLEAR_ROAD]++;
                }
            }
        }
    }
}

static void step_reinforce(void)
{
    const CausalActionParams_t *k = &g_causal_actions;

    for(uint8_t slot = 0; slot < NUM_INCIDENTS; slot++)
    {
        const IncidentState_t *inc = &g_incidents[slot];

        if(!inc->slot_occupied || inc->status == INC_RESOLVED)
        {
            if(s_reinforce_counted[slot] && s_inc_id[slot] != 0 && inc->status == INC_RESOLVED &&
               inc->incident_id == s_inc_id[slot])
                s_stats.resolved[CAUSAL_ACTION_REINFORCE_INCIDENT]++;
            s_reinforce_until[slot] = 0;
            s_reinforce_counted[slot] = 0;
            s_reinforce_detector[slot] = 0;
            s_inc_id[slot] = 0;
            continue;
        }

        if(s_inc_id[slot] != inc->incident_id)
        {
            s_inc_id[slot] = inc->incident_id;
            s_prev_victims[slot] = inc->victim_count;
            s_prev_priority[slot] = (uint8_t)inc->priority;
            s_deteriorated_at[slot] = 0;
            s_reinforce_until[slot] = 0;
            s_reinforce_counted[slot] = 0;
            s_reinforce_detector[slot] = 0;
        }

        /* Observable deterioration: the reported victim count fell or the
         * priority was raised. */
        if(inc->victim_count < s_prev_victims[slot] || (uint8_t)inc->priority < s_prev_priority[slot])
            s_deteriorated_at[slot] = g_current_tick + 1u;
        s_prev_victims[slot] = inc->victim_count;
        s_prev_priority[slot] = (uint8_t)inc->priority;

        if(s_reinforce_until[slot] != 0 && s_reinforce_until[slot] <= g_current_tick + 1u)
        {
            if(s_reinforce_counted[slot] == 0 && s_reinforce_until[slot] != 0)
                s_stats.abandoned[CAUSAL_ACTION_REINFORCE_INCIDENT]++;
            s_reinforce_until[slot] = 0;
            s_reinforce_counted[slot] = 0;
            s_reinforce_detector[slot] = 0;
        }

        if(s_reinforce_until[slot] == 0 && inc->victim_count > 0 && inc->assigned_team != NO_ASSIGNMENT &&
           inc->support_team == NO_ASSIGNMENT &&
           (fault_profile_planner_layer(PLANNER_LAYER_TASKS)
                ? planner_reinforce_wanted(slot)   /* Step 3.5: cost, not the fixed event window */
                : ((s_deteriorated_at[slot] != 0 &&
                    (g_current_tick + 1u - s_deteriorated_at[slot]) <= k->reinforce_event_window) ||
                   s_reinforce_detector[slot])))
        {
            s_reinforce_until[slot] = g_current_tick + 1u + k->reinforce_hold_ticks;
            if(s_reinforce_detector[slot])
                s_stats.from_detector[CAUSAL_ACTION_REINFORCE_INCIDENT]++;
        }

        if(s_reinforce_until[slot] != 0)
        {
            s_stats.active_ticks[CAUSAL_ACTION_REINFORCE_INCIDENT]++;
            if(inc->support_team != NO_ASSIGNMENT && !s_reinforce_counted[slot])
            {
                s_reinforce_counted[slot] = 1;
                s_stats.dispatched[CAUSAL_ACTION_REINFORCE_INCIDENT]++;
            }
        }
    }
}

/* Nearest cell (by Manhattan ring) where the link margin clears the
 * hysteresis threshold with a 1 dB cushion. */
static int find_relay_cell(const TeamState_t *team, int16_t *out_x, int16_t *out_y)
{
    float need = g_causal_params.comm_clear_hysteresis_db + 1.0f;

    for(int r = 1; r <= (int)g_causal_actions.relay_search_radius; r++)
    {
        for(int dx = -r; dx <= r; dx++)
        {
            int rem = r - (dx < 0 ? -dx : dx);
            int dys[2] = { rem, -rem };

            for(int n = 0; n < (rem == 0 ? 1 : 2); n++)
            {
                int x = team->loc_x + dx, y = team->loc_y + dys[n];

                if(!in_grid(x, y) || !environment_cell_is_navigable(x, y))
                    continue;
                if(fault_injector_causal_link_margin_at(x, y) < need)
                    continue;
                if(find_path_cost(team->loc_x, team->loc_y, x, y) == UINT16_MAX)
                    continue;
                *out_x = (int16_t)x;
                *out_y = (int16_t)y;
                return 1;
            }
        }
    }
    return 0;
}

static void step_relay(void)
{
    const CausalActionParams_t *k = &g_causal_actions;

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        const TeamState_t *team = &g_teams[t];
        uint8_t comm_fault = team->fault_state.active[FAULT_CLASS_COMMUNICATION];

        if(s_relay_on[t])
        {
            s_stats.active_ticks[CAUSAL_ACTION_RELAY_REPOSITION]++;
            if(!comm_fault)
            {
                s_relay_on[t] = 0;
                s_relay_cooldown_until[t] = g_current_tick + 1u + k->relay_cooldown_ticks;
                s_stats.resolved[CAUSAL_ACTION_RELAY_REPOSITION]++;
            }
            else if(team->status != STATUS_EN_ROUTE || team->heading_to_rest ||
                    (g_current_tick - s_relay_start[t]) > k->relay_max_ticks)
            {
                s_relay_on[t] = 0;
                s_relay_cooldown_until[t] = g_current_tick + 1u + k->relay_cooldown_ticks;
                s_stats.abandoned[CAUSAL_ACTION_RELAY_REPOSITION]++;
            }
            continue;
        }

        if(team->status == STATUS_EN_ROUTE && !team->heading_to_rest &&
           s_relay_cooldown_until[t] <= g_current_tick + 1u &&
           (fault_profile_planner_layer(PLANNER_LAYER_TASKS)
                ? planner_relay_wanted(team)   /* Step 3.5: P(comm fault) x blindness cost vs reposition cost */
                : (comm_fault &&
                   (g_current_tick - team->fault_state.start_tick[FAULT_CLASS_COMMUNICATION]) >= k->relay_min_fault_ticks &&
                   fault_injector_causal_truth(t)->link_margin_db < 0.0f)) &&
           find_relay_cell(team, &s_relay_x[t], &s_relay_y[t]))
        {
            s_relay_on[t] = 1;
            s_relay_start[t] = g_current_tick;
            s_stats.dispatched[CAUSAL_ACTION_RELAY_REPOSITION]++;
        }
    }
}

void causal_actions_step(void)
{
    if(!fault_profile_causal_actions_on())
        return;

    refresh_cell_state();
    if(fault_profile_planner_layer(PLANNER_LAYER_TASKS))
        update_touch_rates();
    step_reroute();      /* also fills s_blockers for CLEAR_ROAD; each step honours its own enable bit */
    if(fault_profile_causal_action_enabled(CAUSAL_ACTION_CLEAR_ROAD))
        step_clear_road();
    if(fault_profile_causal_action_enabled(CAUSAL_ACTION_REINFORCE_INCIDENT))
        step_reinforce();
    if(fault_profile_causal_action_enabled(CAUSAL_ACTION_RELAY_REPOSITION))
        step_relay();

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
        s_pending_env[t] = 0;

    /* Step 3.5: pull busy teams onto system tasks nobody free will take (no-op unless --recovery=utility). */
    planner_preempt_step();
}

void causal_actions_detector_flag(uint8_t team_id, OperationalFaultClass_t fault_class)
{
    const TeamState_t *team;

    if(team_id >= NUM_TEAMS)
        return;
    team = &g_teams[team_id];

    if(fault_class == FAULT_CLASS_ENVIRONMENT &&
       (fault_profile_causal_fix(CAUSAL_FIX_LABEL) ? fault_injector_causal_env_label(team_id) != 0u
                                                   : team->fault_state.active[FAULT_CLASS_ENVIRONMENT]))
    {
        /* Label fix: the cell is the damaged cell the team's route touches;
         * otherwise the cell the team's own environment fault sits on. */
        int x = fault_profile_causal_fix(CAUSAL_FIX_LABEL) ? s_label_x[team_id]
                                                           : team->fault_state.target_x[FAULT_CLASS_ENVIRONMENT];
        int y = fault_profile_causal_fix(CAUSAL_FIX_LABEL) ? s_label_y[team_id]
                                                           : team->fault_state.target_y[FAULT_CLASS_ENVIRONMENT];

        s_pending_env[team_id] = 1;
        if(in_grid(x, y) && fault_profile_causal_action_enabled(CAUSAL_ACTION_CLEAR_ROAD))
            s_clear_requested[cell_index(x, y)] = 1;
    }
    else if(fault_class == FAULT_CLASS_MISSION &&
            team->assigned_task_pool == TASK_POOL_INCIDENT && team->assigned_task_slot < NUM_INCIDENTS &&
            fault_profile_causal_action_enabled(CAUSAL_ACTION_REINFORCE_INCIDENT))
    {
        s_reinforce_detector[team->assigned_task_slot] = 1;
    }
}

int causal_actions_move_override(TeamState_t *team)
{
    uint8_t t = team->team_id;
    int nx = team->loc_x, ny = team->loc_y;

    if(!fault_profile_causal_action_enabled(CAUSAL_ACTION_RELAY_REPOSITION) || t >= NUM_TEAMS || !s_relay_on[t])
        return 0;

    if(team->loc_x == s_relay_x[t] && team->loc_y == s_relay_y[t])
        return 1;   /* in position, waiting for the margin to clear */

    if(find_next_path_step(team->loc_x, team->loc_y, s_relay_x[t], s_relay_y[t], 0, &nx, &ny) <= 0)
    {
        s_relay_on[t] = 0;
        s_stats.abandoned[CAUSAL_ACTION_RELAY_REPOSITION]++;
        return 0;
    }

    team->loc_x = (int16_t)nx;
    team->loc_y = (int16_t)ny;
    return 1;
}

int causal_actions_crossing_stall(const TeamState_t *team, int next_x, int next_y)
{
    uint8_t t = team->team_id;
    uint8_t delay = g_causal_actions.env_crossing_delay_ticks;
    int stalls_here;

    if(fault_profile_causal_roads_on())
        delay = g_causal_roads.crossing_delay_ticks;

    if(!fault_profile_is_causal() || delay == 0 || t >= NUM_TEAMS)
        return 0;

    /* Consequential roads: only partial debris stalls a team (a fully blocked
     * cell is not navigable, so A* never steps onto it). Otherwise any cell
     * with an active fault does, as in the Step 3 sensitivity override. */
    if(g_road_know.mode != 0u && road_knowledge_step_blocked(team, next_x, next_y))
    {
        /* Step 5 R1-R3: the planned step enters a cell that is truly blocked; the team stops and counts as held. */
        s_stall_total++;
        s_hold_ring[t][s_hold_head[t]] = g_current_tick + 1u;
        s_hold_head[t] = (uint8_t)((s_hold_head[t] + 1u) % HOLD_RING);
        return 1;
    }

    stalls_here = in_grid(next_x, next_y) &&
                  (fault_profile_causal_roads_on()
                       ? fault_injector_causal_road_kind(next_x, next_y) == 1u
                       : g_environment[next_x][next_y].env_fault_ref_count > 0);

    if(stalls_here && (next_x != team->loc_x || next_y != team->loc_y))
    {
        if(s_stall[t] < delay)
        {
            s_stall[t]++;
            s_stall_total++;
            s_hold_ring[t][s_hold_head[t]] = g_current_tick + 1u;
            s_hold_head[t] = (uint8_t)((s_hold_head[t] + 1u) % HOLD_RING);
            return 1;
        }
    }
    s_stall[t] = 0;
    return 0;
}

uint32_t causal_actions_stall_ticks(void)
{
    return s_stall_total;
}

void causal_actions_note_hold(uint8_t team_id)
{
    if(team_id >= NUM_TEAMS)
        return;
    s_hold_ring[team_id][s_hold_head[team_id]] = g_current_tick + 1u;
    s_hold_head[team_id] = (uint8_t)((s_hold_head[team_id] + 1u) % HOLD_RING);
}

int causal_actions_debris_held_at(uint8_t team_id, uint32_t tick)
{
    if(team_id >= NUM_TEAMS)
        return 0;
    for(int h = 0; h < HOLD_RING; h++)
        if(s_hold_ring[team_id][h] == tick + 1u)
            return 1;
    return 0;
}

int causal_actions_debris_held_within(uint8_t team_id, uint32_t window)
{
    if(team_id >= NUM_TEAMS)
        return 0;
    for(int h = 0; h < HOLD_RING; h++)
    {
        uint32_t v = s_hold_ring[team_id][h];

        if(v != 0u && v - 1u <= g_current_tick && g_current_tick - (v - 1u) <= window)
            return 1;
    }
    return 0;
}

int causal_actions_overlay_active(const TeamState_t *team)
{
    return fault_profile_causal_action_enabled(CAUSAL_ACTION_REROUTE) && team->team_id < NUM_TEAMS && s_reroute_on[team->team_id];
}

void causal_actions_path_begin(const TeamState_t *team)
{
    if(fault_profile_causal_action_enabled(CAUSAL_ACTION_REROUTE) && team->team_id < NUM_TEAMS && s_reroute_on[team->team_id])
        path_set_extra_cost(s_avoid);
}

void causal_actions_path_end(void)
{
    path_set_extra_cost(NULL);
}

int causal_actions_reinforce_wanted(uint8_t slot)
{
    if(!fault_profile_causal_action_enabled(CAUSAL_ACTION_REINFORCE_INCIDENT) || slot >= NUM_INCIDENTS)
        return 0;
    return s_reinforce_until[slot] > g_current_tick + 1u;
}

static int reinforced_on_scene(const IncidentState_t *incident)
{
    uint8_t slot = (uint8_t)(incident - g_incidents);

    return slot < NUM_INCIDENTS && s_reinforce_until[slot] != 0 &&
           s_inc_id[slot] == incident->incident_id &&
           incident->support_team < NUM_TEAMS &&
           g_teams[incident->support_team].status == STATUS_ON_SCENE;
}

uint8_t causal_actions_boost_work(const IncidentState_t *incident, uint8_t work_rate)
{
    if(!reinforced_on_scene(incident))
        return work_rate;

    {
        float boosted = (float)work_rate * g_causal_actions.reinforce_work_boost;
        return boosted > 255.0f ? 255u : (uint8_t)boosted;
    }
}

float causal_actions_survival_factor(uint8_t slot)
{
    if(!fault_profile_causal_action_enabled(CAUSAL_ACTION_REINFORCE_INCIDENT) || slot >= NUM_INCIDENTS)
        return 1.0f;
    return reinforced_on_scene(&g_incidents[slot]) ? g_causal_actions.reinforce_survival_factor : 1.0f;
}

void causal_actions_road_cleared(const IncidentState_t *task)
{
    uint8_t slot = (uint8_t)(task - g_system_tasks);
    int tx, ty;

    if(slot >= NUM_SYSTEM_TASKS)
        return;
    clear_task_target(slot, &tx, &ty);
    if(fault_injector_clear_cell_faults(tx, ty) > 0)
        s_stats.resolved[CAUSAL_ACTION_CLEAR_ROAD]++;
}

/* Sidecar truth: does this team's route (next 8 steps) cross partial debris
 * (bit 0) or pass within one cell of a fully blocked cell (bit 1)? */
uint8_t causal_actions_route_touch(const TeamState_t *team)
{
    int x = team->loc_x, y = team->loc_y, dx, dy;
    uint8_t bits = 0;
    int cx = -1, cy = -1;
    int have_partial = 0;

    if(team->team_id < NUM_TEAMS)
    {
        s_label_x[team->team_id] = -1;
        s_label_y[team->team_id] = -1;
    }
    if(!fault_profile_causal_roads_on() || !team_destination(team, &dx, &dy))
        return 0;

    for(uint8_t step = 0; step < 8; step++)
    {
        int nx = x, ny = y;

        if(x == dx && y == dy)
            break;
        if(find_next_path_step(x, y, dx, dy, 0, &nx, &ny) <= 0)
            break;
        x = nx;
        y = ny;
        if(fault_injector_causal_road_kind(x, y) == 1u)
        {
            bits |= 1u;
            if(!have_partial)
            {
                have_partial = 1;
                cx = x;
                cy = y;
            }
        }
        for(int ax = -1; ax <= 1; ax++)
            for(int ay = -1; ay <= 1; ay++)
                if(fault_injector_causal_road_kind(x + ax, y + ay) == 2u)
                {
                    bits |= 2u;
                    if(cx < 0)
                    {
                        cx = x + ax;
                        cy = y + ay;
                    }
                }
    }
    if(team->team_id < NUM_TEAMS)
    {
        s_label_x[team->team_id] = (int16_t)cx;
        s_label_y[team->team_id] = (int16_t)cy;
    }
    return bits;
}

uint8_t causal_actions_team_mask(uint8_t t)
{
    uint8_t mask = 0;
    const TeamState_t *team;

    if(!fault_profile_causal_actions_on() || t >= NUM_TEAMS)
        return 0;
    team = &g_teams[t];

    if(s_reroute_on[t])
        mask |= (uint8_t)(1u << CAUSAL_ACTION_REROUTE);
    if(s_relay_on[t])
        mask |= (uint8_t)(1u << CAUSAL_ACTION_RELAY_REPOSITION);
    if(team->assigned_task_pool == TASK_POOL_SYSTEM && team->assigned_task_slot < NUM_SYSTEM_TASKS &&
       g_system_tasks[team->assigned_task_slot].slot_occupied &&
       g_system_tasks[team->assigned_task_slot].incident_type == INCIDENT_CLEAR_ROAD)
        mask |= (uint8_t)(1u << CAUSAL_ACTION_CLEAR_ROAD);
    if(team->assigned_task_pool == TASK_POOL_INCIDENT && team->assigned_task_slot < NUM_INCIDENTS &&
       g_incidents[team->assigned_task_slot].support_team == t &&
       s_reinforce_until[team->assigned_task_slot] != 0)
        mask |= (uint8_t)(1u << CAUSAL_ACTION_REINFORCE_INCIDENT);

    return mask;
}

#else /* !ENVIRONMENT_HAS_WORLD_STORAGE: the board never runs these actions */

void causal_actions_reset(void) {}
void causal_actions_step(void) {}
void causal_actions_detector_flag(uint8_t team_id, OperationalFaultClass_t fault_class) { (void)team_id; (void)fault_class; }
int causal_actions_move_override(TeamState_t *team) { (void)team; return 0; }
int causal_actions_crossing_stall(const TeamState_t *team, int next_x, int next_y) { (void)team; (void)next_x; (void)next_y; return 0; }
uint32_t causal_actions_stall_ticks(void) { return 0; }
void causal_actions_note_hold(uint8_t team_id) { (void)team_id; }
int causal_actions_debris_held_at(uint8_t team_id, uint32_t tick) { (void)team_id; (void)tick; return 0; }
int causal_actions_debris_held_within(uint8_t team_id, uint32_t window) { (void)team_id; (void)window; return 0; }
void causal_actions_path_begin(const TeamState_t *team) { (void)team; }
void causal_actions_path_end(void) {}
int causal_actions_reinforce_wanted(uint8_t incident_slot) { (void)incident_slot; return 0; }
uint8_t causal_actions_boost_work(const IncidentState_t *incident, uint8_t work_rate) { (void)incident; return work_rate; }
float causal_actions_survival_factor(uint8_t incident_slot) { (void)incident_slot; return 1.0f; }
void causal_actions_road_cleared(const IncidentState_t *task) { (void)task; }
uint8_t causal_actions_route_touch(const TeamState_t *team) { (void)team; return 0; }
int causal_actions_overlay_active(const TeamState_t *team) { (void)team; return 0; }
float causal_actions_cell_touch_rate(int x, int y) { (void)x; (void)y; return 0.0f; }
uint32_t causal_actions_faulted_age(int x, int y) { (void)x; (void)y; return 0u; }
int causal_actions_clear_target(uint8_t slot, int *x, int *y) { (void)slot; (void)x; (void)y; return 0; }
float causal_actions_reroute_gain(const TeamState_t *team) { (void)team; return 0.0f; }
uint8_t causal_actions_team_mask(uint8_t team_id) { (void)team_id; return 0; }
const CausalActionStats_t *causal_actions_stats(void) { return &s_stats; }

#endif

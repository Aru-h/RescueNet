#include "fault_predictor_features.h"

#include <string.h>

static FaultPredictorTeamState_t g_fault_predictor_team_state[NUM_TEAMS];
static FaultPredictorIncidentBaseline_t g_fault_predictor_incident_baseline[NUM_INCIDENTS];

void fault_predictor_features_init(void)
{
    memset(g_fault_predictor_team_state, 0, sizeof(g_fault_predictor_team_state));
    memset(g_fault_predictor_incident_baseline, 0, sizeof(g_fault_predictor_incident_baseline));
}

/*
 * Raw movement check plus the idle-at-base contamination guard from
 * data_prep.py:
 *
 *   df["moved"] = df["moved"] | ((df["assigned_incident"] == -1) &
 *                                 (df["twin_remaining_path_cost"] == 0))
 *
 * "assigned_incident == -1" here is "no incident currently assigned"
 * (assigned_task_pool != TASK_POOL_INCIDENT or assigned_task_slot ==
 * NO_ASSIGNMENT - see laptop/snapshot.c's snapshot_record() for the
 * same check, used there to decide whether to log incident context at
 * all). "twin_remaining_path_cost" is g_mission_execution_twin[team_id]
 * .remaining_path_cost.
 */
static uint8_t compute_moved_this_tick(
    uint8_t team_id,
    const TeamState_t *team,
    const FaultPredictorTeamState_t *state)
{
    uint8_t raw_moved;
    uint8_t no_incident_assigned;
    uint16_t remaining_path_cost;

    if(!state->has_last_loc)
    {
        /* First tick this team has ever been observed this run -
         * nothing to compare against yet. Matches data_prep.py
         * exactly, not just approximately: df.groupby(...)[["loc_x",
         * "loc_y"]].diff() gives NaN for both columns on a group's
         * first row, and pandas' sum(axis=1) with the default
         * skipna=True treats an all-NaN row as 0.0, so "0.0 > 0" is
         * False - i.e. the raw (pre-idle-guard) value for a group's
         * first row is "stalled", not "moved". The idle-at-base guard
         * below still runs unconditionally after this and will
         * correctly override this to 1 for the common case (a new
         * team with nothing assigned yet); only a team whose very
         * first observed tick already has an incident assigned with
         * nonzero remaining_path_cost would actually see this branch
         * matter. */
        raw_moved = 0;
    }
    else
    {
        raw_moved = (team->loc_x != state->last_loc_x ||
                     team->loc_y != state->last_loc_y) ? 1 : 0;
    }

    no_incident_assigned = (team->assigned_task_pool != TASK_POOL_INCIDENT ||
                             team->assigned_task_slot == NO_ASSIGNMENT) ? 1 : 0;
    remaining_path_cost = g_mission_execution_twin[team_id].remaining_path_cost;

    if(no_incident_assigned && remaining_path_cost == 0)
        raw_moved = 1;

    return raw_moved;
}

void fault_predictor_features_update_tick(uint8_t team_id)
{
    TeamState_t *team;
    FaultPredictorTeamState_t *state;
    uint8_t moved;

    if(team_id >= NUM_TEAMS)
        return;

    team = &g_teams[team_id];
    state = &g_fault_predictor_team_state[team_id];

    moved = compute_moved_this_tick(team_id, team, state);

    state->moved_history[state->history_head] = moved;
    state->history_head = (uint8_t)((state->history_head + 1) %
        FAULT_PREDICTOR_STALL_WINDOW_LONG);
    if(state->history_count < FAULT_PREDICTOR_STALL_WINDOW_LONG)
        state->history_count++;

    state->last_loc_x = team->loc_x;
    state->last_loc_y = team->loc_y;
    state->has_last_loc = 1;

    /* 5.2: capture this incident's baseline the first time ANY team
     * is observed assigned to it - see the header's note on why this
     * is keyed per-slot/globally, not per-team.
     *
     * Also (re)capture if this slot's incident_id doesn't match what
     * we last captured for it - a slot recycled for a brand-new
     * incident (see the header's note; confirmed against
     * core/incident.c's find_free_incident_slot()/
     * generate_incident_at()) must not silently keep using the
     * PREVIOUS incident's baseline. */
    if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
       team->assigned_task_slot != NO_ASSIGNMENT)
    {
        FaultPredictorIncidentBaseline_t *baseline =
            &g_fault_predictor_incident_baseline[team->assigned_task_slot];
        const IncidentState_t *inc = &g_incidents[team->assigned_task_slot];

        if(!baseline->captured || baseline->incident_id != inc->incident_id)
        {
            baseline->incident_id = inc->incident_id;
            baseline->baseline_priority = (uint8_t)inc->priority;
            baseline->baseline_victim_count = inc->victim_count;
            baseline->captured = 1;
        }
    }
}

float fault_predictor_stall_frac(uint8_t team_id, uint8_t window)
{
    const FaultPredictorTeamState_t *state;
    uint8_t n, i, idx, not_moved_count;

    if(team_id >= NUM_TEAMS)
        return 0.0f;
    if(window != FAULT_PREDICTOR_STALL_WINDOW_SHORT &&
       window != FAULT_PREDICTOR_STALL_WINDOW_LONG)
        return 0.0f;

    state = &g_fault_predictor_team_state[team_id];

    /* min_periods=1 equivalent: divide by ticks actually observed so
     * far, capped at the requested window - not always by `window`. */
    n = (state->history_count < window) ? state->history_count : window;
    if(n == 0)
        return 0.0f;

    not_moved_count = 0;
    for(i = 0; i < n; i++)
    {
        /* Walk backward from the most-recently-written slot. history_
         * head already points PAST the last write, so the last
         * written slot is (history_head - 1), the one before that is
         * (history_head - 2), etc. - modulo arithmetic done with an
         * added FAULT_PREDICTOR_STALL_WINDOW_LONG to stay positive
         * with unsigned wraparound. */
        idx = (uint8_t)((state->history_head + FAULT_PREDICTOR_STALL_WINDOW_LONG - 1 - i) %
                          FAULT_PREDICTOR_STALL_WINDOW_LONG);
        if(!state->moved_history[idx])
            not_moved_count++;
    }

    return (float)not_moved_count / (float)n;
}

float fault_predictor_delta_priority(uint8_t team_id)
{
    const TeamState_t *team;
    const FaultPredictorIncidentBaseline_t *baseline;
    const IncidentState_t *inc;

    if(team_id >= NUM_TEAMS)
        return 0.0f;

    team = &g_teams[team_id];
    if(team->assigned_task_pool != TASK_POOL_INCIDENT ||
       team->assigned_task_slot == NO_ASSIGNMENT)
        return 0.0f;

    baseline = &g_fault_predictor_incident_baseline[team->assigned_task_slot];
    if(!baseline->captured)
        return 0.0f; /* shouldn't happen if update_tick always runs
                       * first for this team this tick, but fail safe
                       * rather than reading an uninitialized baseline */

    inc = &g_incidents[team->assigned_task_slot];
    return (float)inc->priority - (float)baseline->baseline_priority;
}

float fault_predictor_delta_victim_count(uint8_t team_id)
{
    const TeamState_t *team;
    const FaultPredictorIncidentBaseline_t *baseline;
    const IncidentState_t *inc;

    if(team_id >= NUM_TEAMS)
        return 0.0f;

    team = &g_teams[team_id];
    if(team->assigned_task_pool != TASK_POOL_INCIDENT ||
       team->assigned_task_slot == NO_ASSIGNMENT)
        return 0.0f;

    baseline = &g_fault_predictor_incident_baseline[team->assigned_task_slot];
    if(!baseline->captured)
        return 0.0f;

    inc = &g_incidents[team->assigned_task_slot];
    return (float)inc->victim_count - (float)baseline->baseline_victim_count;
}

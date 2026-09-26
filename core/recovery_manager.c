#include "recovery_manager.h"

#include <limits.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
#include "environment.h"
#include "fault_buffer.h"
#include "fault_predictor.h"
#include "incident.h"
#include "mission_execution_twin.h"
#include "recovery_manager_config.h"
#include "watchdog.h"

typedef struct
{
    RecoveryAction_t action;
    RecoveryReason_t reason;
    float confidence;
    uint8_t escalation_level;
} RecoverySignal_t;

typedef struct
{
    RecoveryStatus_t status;
    RecoveryAction_t pending_action;
    RecoveryReason_t pending_reason;
    uint8_t pending_streak;
    uint8_t cooldown_ticks;
    uint8_t last_assigned_task_pool;
    uint8_t last_assigned_task_slot;
    uint8_t last_status;
    /* evaluate_fault_predictor()'s "sustained AI persistence" streak - see
       core/recovery_manager_config.h's FAULT_PERSISTENCE_FLOOR/_TRIP_TICKS
       doc for the mechanism and why it's a hard-consecutive-tick counter,
       not a cumulative up/down one. Separate from pending_streak above,
       which debounces action-candidate arbitration, not AI confidence. */
    uint8_t predictor_persistence_streak;
} RecoveryManagerTeamState_t;

typedef struct
{
    uint8_t active;
    RecoveryAction_t action;
    uint32_t start_tick;
    RecoveryAction_t last_recommendation;
    uint8_t last_recommendation_valid;
} RecoveryMetricsTeamState_t;

typedef struct
{
    RecoveryAction_t last_action;
    uint8_t last_escalation_level;
    uint64_t last_signature;
    uint32_t last_tick;
} RecoveryDispatchState_t;

typedef RecoverySignal_t (*RecoveryEvaluatorFn)(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog);

static RecoveryManagerTeamState_t g_recovery_state[NUM_TEAMS];
static RecoveryDispatchState_t g_recovery_dispatch_state[NUM_TEAMS];
static RecoveryMetricsTeamState_t g_recovery_metrics_state[NUM_TEAMS];
static RecoveryMetrics_t g_recovery_metrics;
/* M5: tick on which REQUEST_TASK_REALLOCATION last ran
 * allocate_all_incidents(), shared by ALL teams. This used to be a
 * per-team allocated_this_tick flag, but each team is dispatched at most
 * once per tick, so the flag never deduplicated anything: N teams on
 * REALLOC meant N full reallocations per tick. On a board each one is a
 * full cbba_run() (LoRa broadcast plus a blocking wait of up to
 * CBBA_ROUND_TIMEOUT_MS). One reallocation per tick already re-auctions
 * every open task for every team, so later REALLOC teams in the same
 * tick have nothing left to gain from another. */
static uint32_t g_realloc_done_tick = UINT32_MAX;

static float recovery_clamp01(float value)
{
    if(value < 0.0f)
        return 0.0f;

    if(value > 1.0f)
        return 1.0f;

    return value;
}

static float recovery_positive(float value)
{
    return value > 0.0f ? value : 0.0f;
}

static uint64_t recovery_team_signature(const TeamState_t *team)
{
    uint64_t signature = 0;

    signature ^= (uint64_t)team->status;
    signature ^= (uint64_t)team->assigned_task_pool << 4;
    signature ^= (uint64_t)team->assigned_task_slot << 8;
    signature ^= (uint64_t)team->loc_x << 16;
    signature ^= (uint64_t)team->loc_y << 24;
    signature ^= (uint64_t)((uint32_t)(team->stamina * 100.0f)) << 32;
    signature ^= (uint64_t)team->equipment_level << 48;
    /* No fault_state.active_mask term: that is the injector's ground
     * truth, not something the recovery pipeline can observe. Letting it
     * into the signature meant a fault switching on or off counted as a
     * state change for recovery_dispatch()'s change detection even when
     * nothing observable about the team had changed. */

    return signature;
}

static void recovery_metrics_reset(void)
{
    memset(&g_recovery_metrics, 0, sizeof(g_recovery_metrics));
    memset(g_recovery_metrics_state, 0, sizeof(g_recovery_metrics_state));
}

static void recovery_metrics_note_recommendation(
    uint8_t team_id,
    RecoveryAction_t recommendation)
{
    RecoveryMetricsTeamState_t *state;

    if(team_id >= NUM_TEAMS)
        return;

    state = &g_recovery_metrics_state[team_id];

    if(state->last_recommendation_valid &&
       state->last_recommendation > RECOVERY_ACTION_CONTINUE_MONITORING &&
       recommendation > RECOVERY_ACTION_CONTINUE_MONITORING &&
       state->last_recommendation != recommendation)
    {
        g_recovery_metrics.recommendation_reversals++;
    }

    state->last_recommendation = recommendation;
    state->last_recommendation_valid = 1u;
}

static void recovery_metrics_note_dispatch(
    uint8_t team_id,
    RecoveryAction_t action)
{
    RecoveryMetricsTeamState_t *state;

    if(team_id >= NUM_TEAMS || action == RECOVERY_ACTION_CONTINUE_MONITORING)
        return;

    state = &g_recovery_metrics_state[team_id];

    if(state->active && state->action == action)
        return;

    g_recovery_metrics.dispatched[action]++;
    state->active = 1u;
    state->action = action;
    state->start_tick = g_current_tick;
}

static void recovery_metrics_update_resolution(
    uint8_t team_id,
    RecoveryAction_t recommendation)
{
    RecoveryMetricsTeamState_t *state;
    uint32_t elapsed;

    if(team_id >= NUM_TEAMS)
        return;

    state = &g_recovery_metrics_state[team_id];
    if(!state->active)
        return;

    if(recommendation > RECOVERY_ACTION_CONTINUE_MONITORING &&
       recommendation >= state->action)
    {
        if(g_current_tick >= state->start_tick &&
           (g_current_tick - state->start_tick) >= RECOVERY_EVALUATION_WINDOW)
        {
            g_recovery_metrics.timed_out[state->action]++;
            state->active = 0u;
        }
        return;
    }

    if(g_current_tick < state->start_tick)
        elapsed = 0u;
    else
        elapsed = g_current_tick - state->start_tick;

    g_recovery_metrics.resolved[state->action]++;
    g_recovery_metrics.resolution_ticks_sum[state->action] += elapsed;
    state->active = 0u;
}

static MissionStage_t recovery_stage_for_team(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin)
{
    (void)team;

    return twin->mission_stage;
}

static float recovery_stage_multiplier_progress(MissionStage_t stage)
{
    switch(stage)
    {
        case MISSION_STAGE_TRAVEL:
            return RECOVERY_MANAGER_STAGE_TRAVEL_PROGRESS_MULT;

        case MISSION_STAGE_WORK:
            return RECOVERY_MANAGER_STAGE_WORK_PROGRESS_MULT;

        case MISSION_STAGE_RECOVERY:
            return RECOVERY_MANAGER_STAGE_RECOVERY_PROGRESS_MULT;

        case MISSION_STAGE_IDLE:
        default:
            return RECOVERY_MANAGER_STAGE_IDLE_PROGRESS_MULT;
    }
}

static float recovery_stage_multiplier_stamina(MissionStage_t stage)
{
    switch(stage)
    {
        case MISSION_STAGE_TRAVEL:
            return RECOVERY_MANAGER_STAGE_TRAVEL_STAMINA_MULT;

        case MISSION_STAGE_WORK:
            return RECOVERY_MANAGER_STAGE_WORK_STAMINA_MULT;

        case MISSION_STAGE_RECOVERY:
            return RECOVERY_MANAGER_STAGE_RECOVERY_STAMINA_MULT;

        case MISSION_STAGE_IDLE:
        default:
            return RECOVERY_MANAGER_STAGE_IDLE_STAMINA_MULT;
    }
}

static float recovery_stage_multiplier_mobility(MissionStage_t stage)
{
    switch(stage)
    {
        case MISSION_STAGE_TRAVEL:
            return RECOVERY_MANAGER_STAGE_TRAVEL_MOBILITY_MULT;

        case MISSION_STAGE_WORK:
            return RECOVERY_MANAGER_STAGE_WORK_MOBILITY_MULT;

        case MISSION_STAGE_RECOVERY:
            return RECOVERY_MANAGER_STAGE_RECOVERY_MOBILITY_MULT;

        case MISSION_STAGE_IDLE:
        default:
            return RECOVERY_MANAGER_STAGE_IDLE_MOBILITY_MULT;
    }
}

static float recovery_stage_multiplier_communication(MissionStage_t stage)
{
    switch(stage)
    {
        case MISSION_STAGE_TRAVEL:
            return RECOVERY_MANAGER_STAGE_TRAVEL_COMMUNICATION_MULT;

        case MISSION_STAGE_WORK:
            return RECOVERY_MANAGER_STAGE_WORK_COMMUNICATION_MULT;

        case MISSION_STAGE_RECOVERY:
            return RECOVERY_MANAGER_STAGE_RECOVERY_COMMUNICATION_MULT;

        case MISSION_STAGE_IDLE:
        default:
            return RECOVERY_MANAGER_STAGE_IDLE_COMMUNICATION_MULT;
    }
}

static float recovery_stage_multiplier_equipment(MissionStage_t stage)
{
    switch(stage)
    {
        case MISSION_STAGE_TRAVEL:
            return RECOVERY_MANAGER_STAGE_TRAVEL_EQUIPMENT_MULT;

        case MISSION_STAGE_WORK:
            return RECOVERY_MANAGER_STAGE_WORK_EQUIPMENT_MULT;

        case MISSION_STAGE_RECOVERY:
            return RECOVERY_MANAGER_STAGE_RECOVERY_EQUIPMENT_MULT;

        case MISSION_STAGE_IDLE:
        default:
            return RECOVERY_MANAGER_STAGE_IDLE_EQUIPMENT_MULT;
    }
}

static float recovery_stage_action_floor(MissionStage_t stage, RecoveryAction_t action)
{
    float floor = RECOVERY_MANAGER_RECOMMENDATION_MIN_CONFIDENCE;

    switch(action)
    {
        case RECOVERY_ACTION_LOCAL_RECOVERY:
            floor = RECOVERY_MANAGER_ACTION_FLOOR_LOCAL;
            break;

        case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
            floor = RECOVERY_MANAGER_ACTION_FLOOR_RETURN;
            break;

        case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
            floor = RECOVERY_MANAGER_ACTION_FLOOR_REALLOC;
            break;

        case RECOVERY_ACTION_ABORT_MISSION:
            floor = RECOVERY_MANAGER_ACTION_FLOOR_ABORT;
            break;

        case RECOVERY_ACTION_CONTINUE_MONITORING:
        default:
            floor = RECOVERY_MANAGER_RECOMMENDATION_MIN_CONFIDENCE;
            break;
    }

    switch(stage)
    {
        case MISSION_STAGE_TRAVEL:
            if(action == RECOVERY_ACTION_RETURN_TO_SAFE_ZONE)
                floor *= RECOVERY_MANAGER_STAGE_TRAVEL_RETURN_FLOOR_MULT;
            else if(action == RECOVERY_ACTION_REQUEST_TASK_REALLOCATION)
                floor *= RECOVERY_MANAGER_STAGE_TRAVEL_REALLOC_FLOOR_MULT;
            break;

        case MISSION_STAGE_WORK:
            if(action == RECOVERY_ACTION_REQUEST_TASK_REALLOCATION)
                floor *= RECOVERY_MANAGER_STAGE_WORK_REALLOC_FLOOR_MULT;
            else if(action == RECOVERY_ACTION_ABORT_MISSION)
                floor *= RECOVERY_MANAGER_STAGE_WORK_ABORT_FLOOR_MULT;
            break;

        case MISSION_STAGE_RECOVERY:
            if(action == RECOVERY_ACTION_LOCAL_RECOVERY)
                floor *= RECOVERY_MANAGER_STAGE_RECOVERY_LOCAL_FLOOR_MULT;
            break;

        case MISSION_STAGE_IDLE:
        default:
            floor *= RECOVERY_MANAGER_STAGE_IDLE_FLOOR_MULT;
            break;
    }

    return floor;
}

static uint8_t recovery_action_cooldown(MissionStage_t stage, RecoveryAction_t action)
{
    switch(stage)
    {
        case MISSION_STAGE_TRAVEL:
            switch(action)
            {
                case RECOVERY_ACTION_LOCAL_RECOVERY:
                    return RECOVERY_MANAGER_STAGE_TRAVEL_LOCAL_COOLDOWN;
                case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
                    return RECOVERY_MANAGER_STAGE_TRAVEL_RETURN_COOLDOWN;
                case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
                    return RECOVERY_MANAGER_STAGE_TRAVEL_REALLOC_COOLDOWN;
                case RECOVERY_ACTION_ABORT_MISSION:
                    return RECOVERY_MANAGER_STAGE_TRAVEL_ABORT_COOLDOWN;
                case RECOVERY_ACTION_CONTINUE_MONITORING:
                default:
                    return 0u;
            }

        case MISSION_STAGE_WORK:
            switch(action)
            {
                case RECOVERY_ACTION_LOCAL_RECOVERY:
                    return RECOVERY_MANAGER_STAGE_WORK_LOCAL_COOLDOWN;
                case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
                    return RECOVERY_MANAGER_STAGE_WORK_RETURN_COOLDOWN;
                case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
                    return RECOVERY_MANAGER_STAGE_WORK_REALLOC_COOLDOWN;
                case RECOVERY_ACTION_ABORT_MISSION:
                    return RECOVERY_MANAGER_STAGE_WORK_ABORT_COOLDOWN;
                case RECOVERY_ACTION_CONTINUE_MONITORING:
                default:
                    return 0u;
            }

        case MISSION_STAGE_RECOVERY:
            switch(action)
            {
                case RECOVERY_ACTION_LOCAL_RECOVERY:
                    return RECOVERY_MANAGER_STAGE_RECOVERY_LOCAL_COOLDOWN;
                case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
                    return RECOVERY_MANAGER_STAGE_RECOVERY_RETURN_COOLDOWN;
                case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
                    return RECOVERY_MANAGER_STAGE_RECOVERY_REALLOC_COOLDOWN;
                case RECOVERY_ACTION_ABORT_MISSION:
                    return RECOVERY_MANAGER_STAGE_RECOVERY_ABORT_COOLDOWN;
                case RECOVERY_ACTION_CONTINUE_MONITORING:
                default:
                    return 0u;
            }

        case MISSION_STAGE_IDLE:
        default:
            switch(action)
            {
                case RECOVERY_ACTION_LOCAL_RECOVERY:
                    return RECOVERY_MANAGER_STAGE_IDLE_LOCAL_COOLDOWN;
                case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
                    return RECOVERY_MANAGER_STAGE_IDLE_RETURN_COOLDOWN;
                case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
                    return RECOVERY_MANAGER_STAGE_IDLE_REALLOC_COOLDOWN;
                case RECOVERY_ACTION_ABORT_MISSION:
                    return RECOVERY_MANAGER_STAGE_IDLE_ABORT_COOLDOWN;
                case RECOVERY_ACTION_CONTINUE_MONITORING:
                default:
                    return 0u;
            }
    }
}

static uint8_t recovery_action_persistence(MissionStage_t stage, RecoveryAction_t action)
{
    switch(stage)
    {
        case MISSION_STAGE_TRAVEL:
            switch(action)
            {
                case RECOVERY_ACTION_LOCAL_RECOVERY:
                    return RECOVERY_MANAGER_STAGE_TRAVEL_LOCAL_PERSISTENCE;
                case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
                    return RECOVERY_MANAGER_STAGE_TRAVEL_RETURN_PERSISTENCE;
                case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
                    return RECOVERY_MANAGER_STAGE_TRAVEL_REALLOC_PERSISTENCE;
                case RECOVERY_ACTION_ABORT_MISSION:
                    return RECOVERY_MANAGER_STAGE_TRAVEL_ABORT_PERSISTENCE;
                case RECOVERY_ACTION_CONTINUE_MONITORING:
                default:
                    return 0u;
            }

        case MISSION_STAGE_WORK:
            switch(action)
            {
                case RECOVERY_ACTION_LOCAL_RECOVERY:
                    return RECOVERY_MANAGER_STAGE_WORK_LOCAL_PERSISTENCE;
                case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
                    return RECOVERY_MANAGER_STAGE_WORK_RETURN_PERSISTENCE;
                case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
                    return RECOVERY_MANAGER_STAGE_WORK_REALLOC_PERSISTENCE;
                case RECOVERY_ACTION_ABORT_MISSION:
                    return RECOVERY_MANAGER_STAGE_WORK_ABORT_PERSISTENCE;
                case RECOVERY_ACTION_CONTINUE_MONITORING:
                default:
                    return 0u;
            }

        case MISSION_STAGE_RECOVERY:
            switch(action)
            {
                case RECOVERY_ACTION_LOCAL_RECOVERY:
                    return RECOVERY_MANAGER_STAGE_RECOVERY_LOCAL_PERSISTENCE;
                case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
                    return RECOVERY_MANAGER_STAGE_RECOVERY_RETURN_PERSISTENCE;
                case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
                    return RECOVERY_MANAGER_STAGE_RECOVERY_REALLOC_PERSISTENCE;
                case RECOVERY_ACTION_ABORT_MISSION:
                    return RECOVERY_MANAGER_STAGE_RECOVERY_ABORT_PERSISTENCE;
                case RECOVERY_ACTION_CONTINUE_MONITORING:
                default:
                    return 0u;
            }

        case MISSION_STAGE_IDLE:
        default:
            switch(action)
            {
                case RECOVERY_ACTION_LOCAL_RECOVERY:
                    return RECOVERY_MANAGER_STAGE_IDLE_LOCAL_PERSISTENCE;
                case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
                    return RECOVERY_MANAGER_STAGE_IDLE_RETURN_PERSISTENCE;
                case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
                    return RECOVERY_MANAGER_STAGE_IDLE_REALLOC_PERSISTENCE;
                case RECOVERY_ACTION_ABORT_MISSION:
                    return RECOVERY_MANAGER_STAGE_IDLE_ABORT_PERSISTENCE;
                case RECOVERY_ACTION_CONTINUE_MONITORING:
                default:
                    return 0u;
            }
    }
}

static RecoverySignal_t recovery_make_signal(
    RecoveryAction_t action,
    RecoveryReason_t reason,
    float confidence)
{
    RecoverySignal_t signal;

    signal.action = action;
    signal.reason = reason;
    signal.confidence = recovery_clamp01(confidence);
    signal.escalation_level = (uint8_t)action;
    return signal;
}

static float recovery_aggregate_signal_confidence(
    const RecoverySignal_t *signals,
    size_t signal_count,
    RecoveryAction_t action)
{
    float direct_support = 0.0f;
    float fallback_support = 0.0f;

    for(size_t i = 0; i < signal_count; i++)
    {
        const RecoverySignal_t *signal = &signals[i];

        if(signal->action == action)
        {
            if(signal->confidence > direct_support)
                direct_support = signal->confidence;
        }
        else if(signal->action > action)
        {
            float discounted = signal->confidence * RECOVERY_MANAGER_ACTION_CONFIDENCE_DECAY;

            if(discounted > fallback_support)
                fallback_support = discounted;
        }
    }

    if(direct_support > 0.0f)
        return direct_support;

    return fallback_support;
}

static RecoveryReason_t recovery_reason_for_action(
    const RecoverySignal_t *signals,
    size_t signal_count,
    RecoveryAction_t action)
{
    RecoveryReason_t reason = RECOVERY_REASON_NONE;
    float best_confidence = 0.0f;

    for(size_t i = 0; i < signal_count; i++)
    {
        const RecoverySignal_t *signal = &signals[i];

        if(signal->action < action)
            continue;

        if(signal->confidence > best_confidence)
        {
            best_confidence = signal->confidence;
            reason = signal->reason;
        }
    }

    if(reason == RECOVERY_REASON_NONE)
    {
        for(size_t i = 0; i < signal_count; i++)
        {
            const RecoverySignal_t *signal = &signals[i];

            if(signal->confidence > best_confidence)
            {
                best_confidence = signal->confidence;
                reason = signal->reason;
            }
        }
    }

    return reason;
}
static RecoveryAction_t recovery_downgrade_to_achievable_action(RecoveryAction_t desired_action,MissionStage_t stage,float confidence)
{
    RecoveryAction_t action = desired_action;

    while(action > RECOVERY_ACTION_CONTINUE_MONITORING &&
          confidence < recovery_stage_action_floor(stage, action))
    {
        action = (RecoveryAction_t)((int)action - 1);
    }

    return action;
}

static RecoverySignal_t evaluate_progress(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog)
{
    MissionStage_t stage = recovery_stage_for_team(team, twin);
    float gap = recovery_positive(twin->progress_error);
    float threshold = RECOVERY_MANAGER_PROGRESS_ERROR_SCALE *
        recovery_stage_multiplier_progress(stage);
    float confidence = threshold > 0.0f ? recovery_clamp01(gap / threshold) : 0.0f;

    (void)watchdog;

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_LOCAL_RECOVERY))
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    if(stage == MISSION_STAGE_TRAVEL)
        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE, stage, confidence),
            RECOVERY_REASON_PROGRESS_DEVIATION,
            confidence);

    if(stage == MISSION_STAGE_WORK)
        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_REQUEST_TASK_REALLOCATION, stage, confidence),
            RECOVERY_REASON_PROGRESS_DEVIATION,
            confidence);

    if(stage == MISSION_STAGE_RECOVERY)
        return recovery_make_signal(RECOVERY_ACTION_LOCAL_RECOVERY,
            RECOVERY_REASON_PROGRESS_DEVIATION,
            confidence);

    return recovery_make_signal(RECOVERY_ACTION_LOCAL_RECOVERY,
        RECOVERY_REASON_PROGRESS_DEVIATION,
        confidence);
}

static RecoverySignal_t evaluate_stamina(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog)
{
    MissionStage_t stage = recovery_stage_for_team(team, twin);
    float gap = recovery_positive(twin->stamina_error);
    float threshold = RECOVERY_MANAGER_STAMINA_ERROR_SCALE *
        recovery_stage_multiplier_stamina(stage);
    float confidence = threshold > 0.0f ? recovery_clamp01(gap / threshold) : 0.0f;

    if(team->stamina <= RECOVERY_MANAGER_STAMINA_LOW_THRESHOLD)
        confidence = recovery_clamp01(confidence + RECOVERY_MANAGER_STAMINA_LOW_BONUS);

    if(team->stamina <= RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD)
        confidence = recovery_clamp01(confidence + RECOVERY_MANAGER_STAMINA_CRITICAL_BONUS);

    /* Fix for a real dead-zone bug (see the RETURN_TO_SAFE_ZONE branch
     * below for the full writeup): the two fixed-threshold bonuses
     * above (20.0, 10.0) are both well below get_soft_stamina_floor()
     * (core/allocator.c) - 20.0 to 40.0 depending on fitness tier -
     * which is the threshold team_can_accept_incident() actually
     * refuses new work at. Without this, a team whose stamina sits
     * between its own tier floor and 20.0 gets neither bonus, so
     * `confidence` could stay under recovery_stage_action_floor()
     * below and this function would return CONTINUE_MONITORING before
     * ever reaching the branch that would route it to rest - even
     * though it's already ineligible for new work.
     *
     * 0.30, not the 0.20 either existing bonus uses: verified against
     * the actual floors this needs to clear, not guessed. With zero
     * base confidence (twin->stamina_error near 0, which is the
     * common idle-team case this bug hits), the relevant gate is
     * RECOVERY_MANAGER_ACTION_FLOOR_LOCAL (0.24, at
     * MISSION_STAGE_IDLE's 1.00x multiplier) - clearing just this one
     * is enough, since recovery_dispatch()'s RECOVERY_ACTION_
     * LOCAL_RECOVERY case already calls abandon_task_for_recovery()
     * for a STATUS_AVAILABLE team (same safe-zone-return machinery
     * RETURN_TO_SAFE_ZONE uses - see that dispatch case's own
     * comment), so reaching LOCAL_RECOVERY's lower floor is
     * sufficient; this doesn't need to clear RETURN_TO_SAFE_ZONE's
     * own higher 0.36 floor directly. 0.30 clears 0.24 with margin
     * for float rounding, without being as aggressive as the
     * CRITICAL_BONUS magnitude. */
    if(team->stamina < get_soft_stamina_floor(team))
        confidence = recovery_clamp01(confidence + 0.30f);

    (void)watchdog;

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_LOCAL_RECOVERY))
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    /* Bug fix: evaluate_mobility()/evaluate_communication()/evaluate_
     * equipment() all short-circuit here (their own comments cite this
     * function as the precedent for doing so), but this function itself
     * had no such guard - the gap the audit that found this flagged.
     * The `else if` branch below fires RETURN_TO_SAFE_ZONE whenever
     * `team->stamina < get_soft_stamina_floor(team)`, which is exactly
     * the state a team sits in while it is RESTING *because* its
     * stamina is low. The moment update_team()'s STATUS_RESTING handler
     * (core/team.c) promotes RESTING -> AVAILABLE mid-tick (stamina/
     * equipment-driven, no notion of "the recovery pipeline already has
     * an opinion"), this function re-fires RETURN_TO_SAFE_ZONE,
     * recovery_dispatch()'s only guard for that action
     * (`status != STATUS_RESTING`) no longer blocks it, and
     * abandon_task_for_recovery() sends the team straight back to the
     * safe zone it just left - re-entering RESTING immediately and
     * repeating for as long as stamina stays low. Same RESTING<->
     * AVAILABLE bounce documented and fixed for mobility/communication/
     * equipment above; a team already RESTING, or already routed toward
     * its safe zone (heading_to_rest), is by definition already doing
     * what this function would otherwise tell it to do. */
    if(team->status == STATUS_RESTING || team->heading_to_rest)
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    if(team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE)
    {
        if(team->degraded || team->stamina <= RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD)
        {
            return recovery_make_signal(
                recovery_downgrade_to_achievable_action(RECOVERY_ACTION_ABORT_MISSION, stage, confidence),
                RECOVERY_REASON_LOW_STAMINA,
                confidence);
        }
    }
    else if(team->degraded || team->stamina <= RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD ||
            team->stamina < get_soft_stamina_floor(team))
    {
        /* No active mission to abort - route to rest instead, same as the
         * TRAVEL/WORK branch below does for a less severe deviation.
         *
         * The get_soft_stamina_floor() clause is the fix for a real
         * dead-zone bug: without it, this branch only fired at
         * RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD (10.0), while
         * team_can_accept_incident() (core/allocator.c) already refuses
         * new work below get_soft_stamina_floor() - 20.0 to 40.0
         * depending on fitness tier, always higher than 10.0. An idle
         * team between those two thresholds was ineligible for new
         * work AND not yet recommended to rest: recovery_dispatch()
         * (core/simulation.c, called every tick for every team
         * regardless of status) had nothing to act on, so the team
         * just sat at STATUS_AVAILABLE indefinitely, invisible to both
         * the incident-bidding pipeline and the recovery pipeline.
         * Confirmed empirically (laptop/tools/perimeter_probe.c): a team
         * showing STATUS_AVAILABLE 94% of the time was only genuinely
         * eligible for new work 61% of the time, with almost no
         * overlap with when a CRITICAL incident it was capable of was
         * open and discovered - it was stuck in this exact dead zone,
         * not actually available in any useful sense. */
        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE, stage, confidence),
            RECOVERY_REASON_LOW_STAMINA,
            confidence);
    }

    if(stage == MISSION_STAGE_TRAVEL || stage == MISSION_STAGE_WORK)
    {
        if(confidence >= recovery_stage_action_floor(stage, RECOVERY_ACTION_RETURN_TO_SAFE_ZONE))
        {
            return recovery_make_signal(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE,
                RECOVERY_REASON_LOW_STAMINA,
                confidence);
        }
    }

    return recovery_make_signal(RECOVERY_ACTION_LOCAL_RECOVERY,
        RECOVERY_REASON_LOW_STAMINA,
        confidence);
}

static RecoverySignal_t evaluate_mobility(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog)
{
    MissionStage_t stage = recovery_stage_for_team(team, twin);
    float severity = 0.0f;
    float confidence;

    if(watchdog->dominant_failure_mode == WATCHDOG_FAILURE_MOBILITY_DEGRADATION)
        severity = watchdog->anomaly_confidence;

    /* Direct observable signal: how often the team failed to move while
     * travelling (watchdog_get_mobility_stall_fraction(), core/
     * watchdog.c). RECOVERY_MANAGER_MOBILITY_STALL_FULL_SCALE of stalls
     * (every other tick - what a severity-3 fault produces) counts as
     * full severity.
     *
     * This replaces a read of team->fault_state.active/severity[
     * FAULT_CLASS_MOBILITY] - the injector's ground truth - which let
     * this evaluator see the answer (zero false positives, flagged on
     * the exact tick of injection). */
    {
        float stall_severity = recovery_clamp01(
            watchdog_get_mobility_stall_fraction(team->team_id) /
            RECOVERY_MANAGER_MOBILITY_STALL_FULL_SCALE);

        if(stall_severity > severity)
            severity = stall_severity;
    }

    confidence = recovery_clamp01(severity * recovery_stage_multiplier_mobility(stage));

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_RETURN_TO_SAFE_ZONE))
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    /* Bug fix: unlike evaluate_stamina() above (which explicitly branches
     * on STATUS_RESTING), this function had no notion of "the team is
     * already doing what I'm about to recommend." A mobility fault stays
     * active[] for its full duration regardless of the team's status, so
     * every tick that duration is up, this function re-derives the exact
     * same RETURN_TO_SAFE_ZONE (or, if team->degraded, ABORT_MISSION/
     * RETURN_TO_SAFE_ZONE) signal from scratch - correctly, in isolation,
     * since the fault genuinely hasn't cleared yet.
     *
     * The problem is what happens once the team is actually RESTING:
     * update_team()'s STATUS_RESTING handler (core/team.c) promotes
     * RESTING -> AVAILABLE purely on stamina/equipment, with no idea a
     * mobility fault is still open. The moment that promotion fires,
     * this function's fresh (not stale - the fault is still active right
     * now) RETURN_TO_SAFE_ZONE recommendation clears recovery_dispatch()'s
     * only guard for that action (`status != STATUS_RESTING`, which an
     * AVAILABLE team fails) and abandon_task_for_recovery() sends the
     * team straight back out - to the safe zone it's already standing
     * in or already en route to, which it reaches almost immediately,
     * re-enters RESTING, gets re-promoted the moment stamina/equipment
     * allow it again, and repeats for as long as the fault lasts.
     * Confirmed empirically: this accounts for the large majority of the
     * RESTING<->EN_ROUTE bounce still observed under `--fault-profile
     * combined` after the two fixes above (which only address stale
     * escalation state, not a fresh recommendation clashing with a
     * fault-blind promotion). A team already RESTING, or already routed
     * toward its safe zone (heading_to_rest), is by definition already
     * doing what this function would otherwise tell it to do, so there
     * is nothing new to recommend - same reasoning evaluate_stamina()
     * already applies for the low-stamina case. */
    if(team->status == STATUS_RESTING || team->heading_to_rest)
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    if(team->degraded)
    {
        if(team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE)
        {
            return recovery_make_signal(
                recovery_downgrade_to_achievable_action(RECOVERY_ACTION_ABORT_MISSION, stage, confidence),
                RECOVERY_REASON_TEAM_DEGRADED,
                confidence);
        }

        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE, stage, confidence),
            RECOVERY_REASON_TEAM_DEGRADED,
            confidence);
    }

    if(stage == MISSION_STAGE_WORK)
    {
        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_REQUEST_TASK_REALLOCATION, stage, confidence),
            RECOVERY_REASON_MOBILITY_FAILURE,
            confidence);
    }

    return recovery_make_signal(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE,
        RECOVERY_REASON_MOBILITY_FAILURE,
        confidence);
}

static RecoverySignal_t evaluate_communication(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog)
{
    MissionStage_t stage = recovery_stage_for_team(team, twin);
    float confidence = 0.0f;
    float scaled_health;


    if(watchdog->dominant_failure_mode == WATCHDOG_FAILURE_COMMUNICATION_FAILURE ||
       twin->communication_health >= RECOVERY_MANAGER_COMMUNICATION_MINOR_THRESHOLD)
    {
        scaled_health = (float)twin->communication_health / RECOVERY_MANAGER_COMMUNICATION_ERROR_SCALE;
        confidence = recovery_clamp01(scaled_health *
            recovery_stage_multiplier_communication(stage));
    }

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_REQUEST_TASK_REALLOCATION))
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    /* Same fix as evaluate_mobility() above, same reasoning: communication_
     * health is tracked independently of team_set_available()'s stamina/
     * equipment gate (core/team.c), so a team can clear that gate and get
     * promoted RESTING -> AVAILABLE while still sitting on a severe comm
     * fault. Without this, the SEVERE branch below re-fires RETURN_TO_
     * SAFE_ZONE the instant that promotion happens, and recovery_dispatch()
     * has no guard against it (status is AVAILABLE, not RESTING) -
     * abandon_task_for_recovery() sends the team right back, it arrives
     * almost immediately, re-enters RESTING, gets re-promoted, repeats for
     * as long as the fault lasts. A team already RESTING or already
     * routed toward its safe zone (heading_to_rest) has nothing further
     * to be told here. */
    if(team->status == STATUS_RESTING || team->heading_to_rest)
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    if(twin->communication_health >= RECOVERY_MANAGER_COMMUNICATION_SEVERE_THRESHOLD)
    {
        /* ABORT_MISSION only makes sense for a team that actually has a
         * mission to abort. An idle/available (or resting) team with a
         * severe comm fault has nothing to abort - it needs to be routed
         * to rest instead, same as evaluate_stamina/evaluate_equipment
         * already do for their own critical-threshold branches. Without
         * this, a severe comm fault on an idle team produces an
         * unactionable ABORT_MISSION recommendation forever, and the team
         * never gets sent to recover. */
        if(team->status == STATUS_EN_ROUTE ||
           team->status == STATUS_ON_SCENE)
        {
            return recovery_make_signal(
                recovery_downgrade_to_achievable_action(RECOVERY_ACTION_ABORT_MISSION, stage, confidence),
                RECOVERY_REASON_COMMUNICATION_FAILURE,
                confidence);
        }

        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE, stage, confidence),
            RECOVERY_REASON_COMMUNICATION_FAILURE,
            confidence);
    }

    if(stage == MISSION_STAGE_RECOVERY)
        return recovery_make_signal(RECOVERY_ACTION_LOCAL_RECOVERY,
            RECOVERY_REASON_COMMUNICATION_FAILURE,
            confidence);

    return recovery_make_signal(RECOVERY_ACTION_REQUEST_TASK_REALLOCATION,
        RECOVERY_REASON_COMMUNICATION_FAILURE,
        confidence);
}

static RecoverySignal_t evaluate_equipment(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog)
{
    MissionStage_t stage = recovery_stage_for_team(team, twin);
    float confidence = 0.0f;
    float deficit = 0.0f;
    uint8_t equipment_level = team->equipment_level;

    (void)watchdog;

    if(equipment_level < RECOVERY_MANAGER_EQUIPMENT_LOW_THRESHOLD)
    {
        deficit = (float)(RECOVERY_MANAGER_EQUIPMENT_LOW_THRESHOLD - equipment_level) /
            RECOVERY_MANAGER_EQUIPMENT_ERROR_SCALE;
        confidence = recovery_clamp01(deficit *
            recovery_stage_multiplier_equipment(stage));
    }

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_LOCAL_RECOVERY))
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    /* Same fix as evaluate_mobility()/evaluate_communication() above, same
     * reasoning: an active FAULT_CLASS_RESOURCE fault keeps draining
     * equipment_level even while RESTING (core/team.c's STATUS_RESTING
     * handler applies the drain before the replenish), so a team can dip
     * back under threshold again shortly after team_set_available()
     * promoted it on a momentarily-full reading. Without this, the
     * CRITICAL branch below re-fires RETURN_TO_SAFE_ZONE the instant
     * that promotion happens, with no dispatch-level guard against it
     * (status is AVAILABLE, not RESTING). A team already RESTING or
     * already routed toward its safe zone (heading_to_rest) has nothing
     * further to be told here. */
    if(team->status == STATUS_RESTING || team->heading_to_rest)
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    if(equipment_level <= RECOVERY_MANAGER_EQUIPMENT_CRITICAL_THRESHOLD)
    {
        if((team->degraded &&
            (team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE)) ||
           stage == MISSION_STAGE_WORK)
        {
            return recovery_make_signal(
                recovery_downgrade_to_achievable_action(RECOVERY_ACTION_ABORT_MISSION, stage, confidence),
                RECOVERY_REASON_EQUIPMENT_FAULT,
                confidence);
        }

        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE, stage, confidence),
            RECOVERY_REASON_EQUIPMENT_FAULT,
            confidence);
    }

    if(stage == MISSION_STAGE_WORK || stage == MISSION_STAGE_TRAVEL)
    {
        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_REQUEST_TASK_REALLOCATION, stage, confidence),
            RECOVERY_REASON_EQUIPMENT_FAULT,
            confidence);
    }

    return recovery_make_signal(RECOVERY_ACTION_LOCAL_RECOVERY,
        RECOVERY_REASON_EQUIPMENT_FAULT,
        confidence);
}

/*
 * Sixth evaluator: cross-board Byzantine-fault-tolerant consensus on
 * this team's fault state, fed by core/fault_buffer.c's per-class vote
 * tally + dynamic-quorum Brooks-Iyengar fusion over incoming FAULT_
 * BROADCAST/FAULT_VOTE frames (Prompt 1). Fusion runs independently
 * per OperationalFaultClass_t (core/fault_config.h), but RecoverySignal_t
 * carries one confidence/reason - so this evaluator reports whichever
 * class currently has the highest weight/quorum ratio, using that
 * ratio directly as confidence (no separate confidence formula) and
 * RECOVERY_REASON_CONSENSUS_FAULT as the reason regardless of which
 * class won.
 *
 * Known simplification, flagged not blocking: if two classes both
 * clear quorum in the same round (plausible given fault_injector.c's
 * confirmed concurrent-fault injection - fault_injector_step() has no
 * mutual-exclusivity check between FAULT_CLASS_t values), only the
 * higher-ratio class surfaces through this evaluator this tick. The
 * other isn't lost - fault_buffer_fuse()'s class_weight[] still has
 * it, and it'll surface as best_class on a later tick if it's
 * genuinely still active - simultaneous-dual-alarm handling just
 * isn't attempted here.
 *
 * Action mapping, scaled by weight/quorum ratio: 0 -> CONTINUE_
 * MONITORING; 0 < ratio < 1 -> LOCAL_RECOVERY, confidence scaling
 * toward 1.0; ratio == 1 (weight == quorum) -> REQUEST_TASK_
 * REALLOCATION; ratio > 1 (weight above quorum, up to N_live) ->
 * propose higher, same degraded/en-route-vs-idle split the other
 * evaluators already use to choose ABORT_MISSION vs RETURN_TO_
 * SAFE_ZONE for a team with nothing to abort. If N_live < 3,
 * fault_buffer_fuse() skips fusion for the round and this evaluator
 * reports confidence 0 - the other five evaluators' arbitration
 * stands on its own that tick.
 *
 * PARKED: compiled out by RECOVERY_MANAGER_ENABLE_CONSENSUS_FAULT
 * (recovery_manager_config.h, 0u by default) - see that macro's own
 * comment for why. Not registered in evaluators[] below regardless of
 * this guard; re-adding both is a two-line change once the underlying
 * weighting-scheme concern is actually fixed.
 */
#if RECOVERY_MANAGER_ENABLE_CONSENSUS_FAULT
static RecoverySignal_t evaluate_consensus_fault(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog)
{
    MissionStage_t stage = recovery_stage_for_team(team, twin);
    FaultBufferFusionResult_t fusion = fault_buffer_fuse(team->team_id);
    float confidence;
    /* Weight accumulates as whole votes (one team contributes one
       weight unit per class it flags, in the common unweighted case),
       so round to the nearest whole vote before comparing against the
       integer quorum - avoids float-equality fragility on "weight ==
       quorum" while still reflecting exact vote counts. */
    uint8_t weight_rounded;

    (void)watchdog;

    if(fusion.fusion_skipped)
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            0.0f);

    confidence = fusion.best_ratio;
    weight_rounded = (uint8_t)(fusion.class_weight[fusion.best_class] + 0.5f);

    if(weight_rounded == 0u)
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_LOCAL_RECOVERY))
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    if(weight_rounded > fusion.quorum)
    {
        if(team->degraded &&
           (team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE))
        {
            return recovery_make_signal(
                recovery_downgrade_to_achievable_action(RECOVERY_ACTION_ABORT_MISSION, stage, confidence),
                RECOVERY_REASON_CONSENSUS_FAULT,
                confidence);
        }

        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE, stage, confidence),
            RECOVERY_REASON_CONSENSUS_FAULT,
            confidence);
    }

    if(weight_rounded == fusion.quorum)
    {
        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_REQUEST_TASK_REALLOCATION, stage, confidence),
            RECOVERY_REASON_CONSENSUS_FAULT,
            confidence);
    }

    /* 0 < weight < quorum: below the Byzantine-safe bound - stay at the
       lowest actionable tier and let confidence (the weight/quorum
       ratio) do the scaling. */
    return recovery_make_signal(RECOVERY_ACTION_LOCAL_RECOVERY,
        RECOVERY_REASON_CONSENSUS_FAULT,
        confidence);
}
#endif /* RECOVERY_MANAGER_ENABLE_CONSENSUS_FAULT */

/*
 * Sixth evaluator (current): single-team AI fault prediction, replacing
 * evaluate_consensus_fault above in the registered lineup while cross-
 * board consensus is parked (v4 architecture doc, Section 4/9). Runs
 * core/fault_predictor.c's model against this team's own live evidence
 * vector - watchdog_get_evidence_vector() for the 7 accumulated_evidence
 * channels + confirmed_mobility_impairment, team->equipment_level/100.0f,
 * plus incident_priority/victim_count and env_raw_travel_cost - the exact
 * same 12-feature shape core/fault_comms.c assembles for FAULT_BROADCAST,
 * so this team's own inference input matches what gets broadcast about it.
 *
 * fault_predictor_run() returns 5 independent per-class scores (not a
 * softmax - see fault_predictor.h). Per the doc's Section 2 ("collapse 5
 * scores into one RecoverySignal_t: max score = confidence, its class =
 * reason"), this reports the highest-scoring class's score directly as
 * confidence - no separate formula - but uses one reason
 * (RECOVERY_REASON_PREDICTED_FAULT) for all 5 classes rather than a
 * per-class mapping, the same simplification evaluate_consensus_fault
 * makes with RECOVERY_REASON_CONSENSUS_FAULT: RESOURCE and ENVIRONMENT
 * don't have a clean existing RecoveryReason_t counterpart, and forcing
 * one risks conflating them with MOBILITY/EQUIPMENT's existing reasons
 * for a class that isn't actually either of those.
 *
 * Tiered trust (Section 3, thresholds explicitly still open per Section
 * 8 item 2 - RECOVERY_MANAGER_PREDICTOR_REALLOCATE_CONFIDENCE/_ESCALATE_
 * CONFIDENCE are placeholders, not tuned numbers): CONTINUE_MONITORING
 * and LOCAL_RECOVERY are fine on the AI's word alone. Anything at or
 * above REQUEST_TASK_REALLOCATION additionally requires watchdog
 * corroboration (watchdog->anomaly_detected) OR sustained AI persistence
 * (predictor_persistence_streak_update() below, gated at
 * FAULT_PERSISTENCE_TRIP_TICKS consecutive ticks >= FAULT_PERSISTENCE_
 * FLOOR - see core/recovery_manager_config.h for the mechanism choice and
 * calibration this constant is cited to). An uncorroborated, not-yet-
 * sustained high-confidence prediction is still capped at LOCAL_RECOVERY
 * rather than silently escalating.
 */
uint8_t recovery_manager_persistence_streak_step(uint8_t streak, float confidence)
{
    if(confidence >= FAULT_PERSISTENCE_FLOOR)
    {
        if(streak < UINT8_MAX)
            streak++;
    }
    else
    {
        streak = 0u;
    }

    return streak;
}

/*
 * EXPERIMENTAL - MEASURED AND NOT ADOPTED. NOT called from predictor_
 * persistence_streak_update()/evaluate_fault_predictor() below; this
 * function exists purely so the variant it implements is real, tested
 * code rather than a claim, exposed the same way recovery_manager_
 * persistence_streak_step() above is exposed for its own selftest.
 *
 * Bounded-forgiveness variant: tolerates exactly ONE isolated below-
 * floor tick without resetting the streak, but still hard-resets on
 * two CONSECUTIVE misses - explicitly narrower than the proportional
 * up/down decay counter recovery_manager_config.h:154-170 already
 * documents as designed, measured, and REJECTED (that variant forgave
 * every bad tick a little, in proportion, which let it forgive bursty
 * noise just as generously as a real fault's brief dip - the exact
 * failure mode this narrower variant was designed to avoid by only
 * ever forgiving a single isolated miss, never a run of them).
 *
 * *was_last_miss is the caller-owned extra bit of state this variant
 * needs beyond the plain streak counter (recovery_manager_persistence_
 * streak_step() above needs none) - true if the immediately preceding
 * tick was itself a miss. Caller must persist it across calls the same
 * way the streak itself is persisted; there is no packed single-uint8_t
 * form of this state, deliberately, so this stays as easy to reason
 * about and unit-test as the sibling function above.
 *
 * MEASURED RESULT (laptop/tools/persistence_bounded_forgiveness_
 * ablation.py, make check-persistence-bounded-forgiveness - real
 * simulator data, evidence_channels.py's recomputed_anomaly_confidence
 * against real fault_active_* ground truth, same methodology
 * FAULT_PERSISTENCE_TRIP_TICKS=8 was itself tuned against, not a
 * trained model):
 *
 *   - DOES fully close the specific dip-every-7th-tick adversarial
 *     pattern found and confirmed exploitable against the hard-reset
 *     mechanism this session (that pattern never lets the hard-reset
 *     streak exceed 6; this variant reaches H=8 by tick 9 against the
 *     identical input, every time).
 *   - Does NOT meaningfully improve the general false-reject/false-
 *     accept tradeoff on real simulator data at H=8: false-reject
 *     46.7% vs. the shipped mechanism's 48.0% (a 1.3-point
 *     improvement) but false-accept 10.8% vs. 7.5% (a 3.3-point
 *     regression) - a worse trade than the improvement it buys, by
 *     the exact same standard (matched-false-reject false-accept
 *     comparison) recovery_manager_config.h already used to reject
 *     the up/down variant. Smaller-magnitude version of the same
 *     problem: bounding the forgiveness to one isolated tick instead
 *     of unlimited proportional decay reduces how much noise gets
 *     forgiven, but does not eliminate it - real noise under FAULT_
 *     PROFILE_COMBINED's concurrent-fault-class background churn
 *     still produces enough isolated single-tick blips for this to
 *     cost more false-accept than it saves false-reject.
 *
 * Conclusion: closes the one specific exploit it was aimed at, but is
 * NOT a net improvement on this dataset - left here as tested,
 * documented, and explicitly not shipped, rather than silently
 * discarded (same disposition as the up/down variant gets in recovery_
 * manager_config.h) or adopted anyway despite the numbers.
 */
uint8_t recovery_manager_persistence_streak_step_bounded_forgiveness(
    uint8_t streak, float confidence, uint8_t *was_last_miss)
{
    if(confidence >= FAULT_PERSISTENCE_FLOOR)
    {
        if(streak < UINT8_MAX)
            streak++;
        *was_last_miss = 0u;
    }
    else
    {
        if(*was_last_miss)
            streak = 0u; /* second consecutive miss - hard reset */
        /* else: first isolated miss - forgiven, streak unchanged */
        *was_last_miss = 1u;
    }

    return streak;
}

static uint8_t predictor_persistence_streak_update(uint8_t team_id, float confidence)
{
    RecoveryManagerTeamState_t *state = &g_recovery_state[team_id];

    state->predictor_persistence_streak =
        recovery_manager_persistence_streak_step(state->predictor_persistence_streak, confidence);

    return state->predictor_persistence_streak;
}

void recovery_manager_build_predictor_evidence(
    uint8_t team_id, float evidence_out[/* FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES */])
{
    const TeamState_t *team = &g_teams[team_id];
    uint8_t confirmed_mobility_impairment = 0u;

    watchdog_get_evidence_vector(team_id, evidence_out, &confirmed_mobility_impairment);
    evidence_out[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS] = (float)team->equipment_level / 100.0f;
    evidence_out[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 1u] = (float)confirmed_mobility_impairment;

    {
        const IncidentState_t *task = NULL;

        if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
           team->assigned_task_slot < NUM_INCIDENTS)
            task = &g_incidents[team->assigned_task_slot];
        else if(team->assigned_task_pool == TASK_POOL_SYSTEM &&
                team->assigned_task_slot < NUM_SYSTEM_TASKS)
            task = &g_system_tasks[team->assigned_task_slot];

        evidence_out[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 2u] =
            (task != NULL) ? (float)task->priority : 0.0f;
        evidence_out[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 3u] =
            (task != NULL) ? (float)task->victim_count : 0.0f;
    }

    evidence_out[FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS + 4u] =
        (float)environment_cell_travel_cost(team->loc_x, team->loc_y);
}

static RecoverySignal_t evaluate_fault_predictor(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog)
{
    MissionStage_t stage = recovery_stage_for_team(team, twin);
    float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES];
    FaultPredictorScores_t scores;
    uint8_t best_class = 0u;
    float confidence;
    uint8_t sustained;
    uint8_t corroborated;

    (void)twin;

    recovery_manager_build_predictor_evidence(team->team_id, evidence);

    scores = fault_predictor_run(evidence);

    for(uint8_t fault_class = 1u; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
    {
        if(scores.score[fault_class] > scores.score[best_class])
            best_class = fault_class;
    }

    confidence = scores.score[best_class];

    /* Updated unconditionally, ahead of both early returns below, so a
       low-confidence tick still resets the streak even though this
       function returns CONTINUE_MONITORING for it - the streak must
       reflect every tick's confidence, not just the ticks that reach the
       corroboration check further down. */
    sustained = predictor_persistence_streak_update(team->team_id, confidence)
                    >= FAULT_PERSISTENCE_TRIP_TICKS;

    /* This early return is why WATCHDOG_ANOMALY_THRESHOLD (core/watchdog.c)
     * is currently risk-free to keep tuned aggressively (0.10, lowered
     * from 0.35 this pass - 43.3% fault-active detection / 9.5% no-fault
     * false-positive rate at that value, combined fault profile, 5
     * seeds x 3000 ticks): fault_predictor_run() is still the untrained
     * placeholder (FAULT_MODEL_LOADED undefined, core/fault_predictor.c
     * returns all-zero scores), so confidence is always <= 0 here and
     * this function always returns CONTINUE_MONITORING before ever
     * reaching the corroborated = watchdog->anomaly_detected check
     * below - a 9.5% false-positive rate on that flag has zero live
     * effect on any dispatched RecoveryAction_t today, only on logging
     * (core/simulation.c's EVENT_WATCHDOG_ANOMALY_CHANGED) and the
     * dashboard's anomaly counter (laptop/tools/app.py). Once a real
     * trained-weights header lands and FAULT_MODEL_LOADED is defined,
     * this early return stops firing unconditionally and
     * WATCHDOG_ANOMALY_THRESHOLD's false-positive rate starts actually
     * gating escalation past LOCAL_RECOVERY (below) - re-evaluate the
     * threshold's FPR/TPR tradeoff at that point, don't assume today's
     * "good enough" verdict still holds.
     *
     * The 43.3%/9.5% figures above were measured while the watchdog's
     * communication channel and the degraded flag were still reading
     * the injector's fault_state (since removed), so they are not
     * trustworthy either - re-measure before relying on them. */
    if(confidence <= 0.0f)
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_LOCAL_RECOVERY))
        return recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            RECOVERY_REASON_NONE,
            confidence);

    corroborated = watchdog->anomaly_detected || sustained;

    if(corroborated && confidence >= RECOVERY_MANAGER_PREDICTOR_ESCALATE_CONFIDENCE)
    {
        if(team->degraded &&
           (team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE))
        {
            return recovery_make_signal(
                recovery_downgrade_to_achievable_action(RECOVERY_ACTION_ABORT_MISSION, stage, confidence),
                RECOVERY_REASON_PREDICTED_FAULT,
                confidence);
        }

        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_RETURN_TO_SAFE_ZONE, stage, confidence),
            RECOVERY_REASON_PREDICTED_FAULT,
            confidence);
    }

    if(corroborated && confidence >= RECOVERY_MANAGER_PREDICTOR_REALLOCATE_CONFIDENCE)
    {
        return recovery_make_signal(
            recovery_downgrade_to_achievable_action(RECOVERY_ACTION_REQUEST_TASK_REALLOCATION, stage, confidence),
            RECOVERY_REASON_PREDICTED_FAULT,
            confidence);
    }

    /* Uncorroborated, or corroborated but below the reallocate
       threshold: AI-alone territory per the tiered-trust rule - cap at
       LOCAL_RECOVERY. */
    return recovery_make_signal(RECOVERY_ACTION_LOCAL_RECOVERY,
        RECOVERY_REASON_PREDICTED_FAULT,
        confidence);
}

uint32_t g_diag_discarded_signal_count = 0;

static RecoverySignal_t recovery_select_primary_signal(
    const RecoverySignal_t *signals,
    size_t signal_count,
    MissionStage_t stage)
{
    RecoverySignal_t best = recovery_make_signal(
        RECOVERY_ACTION_CONTINUE_MONITORING,
        RECOVERY_REASON_NONE,
        0.0f);

    for(size_t i = 0; i < signal_count; i++)
    {
        const RecoverySignal_t *signal = &signals[i];
        float action_floor = recovery_stage_action_floor(stage, signal->action);

        if(signal->confidence < action_floor)
        {
            g_diag_discarded_signal_count++;
            continue;
        }

        if(signal->action > best.action ||
           (signal->action == best.action && signal->confidence > best.confidence))
        {
            best = *signal;
        }
    }

    return best;
}

static RecoverySignal_t recovery_apply_hysteresis(
    RecoveryManagerTeamState_t *state,
    const RecoverySignal_t *candidate,
    MissionStage_t stage)
{
    RecoverySignal_t active = recovery_make_signal(
        state->status.recommendation,
        state->status.reason,
        state->status.confidence);

    if(candidate->action == active.action)
    {
        state->pending_action = candidate->action;
        state->pending_reason = candidate->reason;
        state->pending_streak = 0u;
        state->cooldown_ticks = recovery_action_cooldown(stage, active.action);
        return *candidate;
    }

    if(candidate->action > active.action)
    {
        if(state->pending_action != candidate->action)
        {
            state->pending_action = candidate->action;
            state->pending_reason = candidate->reason;
            state->pending_streak = 1u;
        }
        else if(state->pending_streak < UINT8_MAX)
        {
            state->pending_streak++;
        }

        if(state->pending_streak >= recovery_action_persistence(stage, candidate->action))
        {
            RecoveryAction_t next_action = (RecoveryAction_t)(active.action + 1u);
            RecoverySignal_t promoted = recovery_make_signal(
                next_action,
                candidate->reason,
                candidate->confidence);

            state->status.recommendation = promoted.action;
            state->status.reason = promoted.reason;
            state->status.confidence = promoted.confidence;
            state->status.escalation_level = promoted.escalation_level;
            state->pending_streak = 0u;
            state->cooldown_ticks = recovery_action_cooldown(stage, promoted.action);
            return promoted;
        }

        return active;
    }

    if(state->cooldown_ticks > 0u)
    {
        state->cooldown_ticks--;
        return active;
    }

    if(state->pending_action != candidate->action)
    {
        state->pending_action = candidate->action;
        state->pending_reason = candidate->reason;
        state->pending_streak = 1u;
    }
    else if(state->pending_streak < UINT8_MAX)
    {
        state->pending_streak++;
    }

    if(state->pending_streak >= RECOVERY_MANAGER_DEESCALATION_TICKS &&
       active.action > RECOVERY_ACTION_CONTINUE_MONITORING)
    {
        RecoveryAction_t next_action = (RecoveryAction_t)(active.action - 1u);
        RecoverySignal_t demoted = recovery_make_signal(
            next_action,
            candidate->reason,
            candidate->confidence);

        state->status.recommendation = demoted.action;
        state->status.reason = demoted.reason;
        state->status.confidence = demoted.confidence;
        state->status.escalation_level = demoted.escalation_level;
        state->pending_streak = 0u;
        state->cooldown_ticks = recovery_action_cooldown(stage, demoted.action);
        return demoted;
    }

    return active;
}

static RecoverySignal_t recovery_build_status(
    RecoveryManagerTeamState_t *state,
    const RecoverySignal_t *signals,
    size_t signal_count,
    MissionStage_t stage)
{
    RecoverySignal_t selected = recovery_select_primary_signal(signals, signal_count, stage);
    RecoverySignal_t active = recovery_apply_hysteresis(state, &selected, stage);
    float active_support = recovery_aggregate_signal_confidence(
        signals,
        signal_count,
        active.action);
    float selected_support = recovery_aggregate_signal_confidence(
        signals,
        signal_count,
        selected.action);

    if(active.action == selected.action)
    {
        active.reason = selected.reason;
        active.confidence = recovery_clamp01(
            selected.confidence * RECOVERY_MANAGER_ACTION_CONFIDENCE_PRIMARY_WEIGHT +
            selected_support * RECOVERY_MANAGER_ACTION_CONFIDENCE_SUPPORT_WEIGHT);
        active.escalation_level = selected.escalation_level;
    }
    else
    {
        active.reason = recovery_reason_for_action(signals, signal_count, active.action);
        active.confidence = recovery_clamp01(
            active_support * RECOVERY_MANAGER_ACTION_CONFIDENCE_PRIMARY_WEIGHT +
            selected_support * RECOVERY_MANAGER_ACTION_CONFIDENCE_SUPPORT_WEIGHT);
        active.escalation_level = (uint8_t)active.action;
    }

    state->status.recommendation = active.action;
    state->status.reason = active.reason;
    state->status.confidence = active.confidence;
    state->status.escalation_level = active.escalation_level;

    if(state->status.confidence < RECOVERY_MANAGER_RECOMMENDATION_MIN_CONFIDENCE &&
       state->status.recommendation == RECOVERY_ACTION_CONTINUE_MONITORING)
    {
        state->status.reason = RECOVERY_REASON_NONE;
    }

    return state->status.recommendation == RECOVERY_ACTION_CONTINUE_MONITORING
        ? recovery_make_signal(RECOVERY_ACTION_CONTINUE_MONITORING,
            state->status.reason,
            state->status.confidence)
        : active;
}

static RecoveryStatus_t recovery_status_from_state(
    const RecoveryManagerTeamState_t *state)
{
    return state->status;
}

void recovery_manager_init(void)
{
    recovery_metrics_reset();
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        g_recovery_state[team_id].status.recommendation = RECOVERY_ACTION_CONTINUE_MONITORING;
        g_recovery_state[team_id].status.reason = RECOVERY_REASON_NONE;
        g_recovery_state[team_id].status.confidence = 0.0f;
        g_recovery_state[team_id].status.escalation_level = RECOVERY_ACTION_CONTINUE_MONITORING;
        g_recovery_state[team_id].pending_action = RECOVERY_ACTION_CONTINUE_MONITORING;
        g_recovery_state[team_id].pending_reason = RECOVERY_REASON_NONE;
        g_recovery_state[team_id].pending_streak = 0u;
        g_recovery_state[team_id].predictor_persistence_streak = 0u;
        g_recovery_state[team_id].cooldown_ticks = 0u;
        g_recovery_state[team_id].last_assigned_task_pool = g_teams[team_id].assigned_task_pool;
        g_recovery_state[team_id].last_assigned_task_slot = g_teams[team_id].assigned_task_slot;
        g_recovery_state[team_id].last_status = g_teams[team_id].status;
        g_recovery_dispatch_state[team_id] = (RecoveryDispatchState_t){0};
    }
    g_realloc_done_tick = UINT32_MAX;
}

void recovery_manager_step(void)
{
    static const RecoveryEvaluatorFn evaluators[RECOVERY_MANAGER_EVALUATOR_COUNT] =
    {
        evaluate_progress,
        evaluate_stamina,
        evaluate_mobility,
        evaluate_communication,
        evaluate_equipment,
        evaluate_fault_predictor
    };

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        const TeamState_t *team = &g_teams[team_id];
        const MissionExecutionTwin_t *twin = &g_mission_execution_twin[team_id];
        WatchdogStatus_t watchdog = watchdog_get_status(team_id);
        RecoverySignal_t signals[RECOVERY_MANAGER_EVALUATOR_COUNT];
        MissionStage_t stage;
        RecoveryManagerTeamState_t *state = &g_recovery_state[team_id];

        /* A team's assignment can legitimately change out from under a
         * still-active recovery recommendation - most notably,
         * team_can_accept_incident()'s "steal a resting team" path
         * (core/allocator.c) pulling a RESTING team straight into a
         * brand-new CRITICAL incident via assign_incident(), which runs
         * in allocate_all_incidents() at the very start of sim_tick(),
         * before this function or recovery_dispatch() run for the same
         * tick (core/sim_runner.c). The escalation/hysteresis state
         * below (pending_streak, cooldown_ticks, status.escalation_
         * level) is intentionally sticky across ticks so a genuinely
         * ongoing problem doesn't get dispatched, de-dispatched, and
         * re-dispatched on every small fluctuation - see
         * recovery_apply_hysteresis(). But that stickiness assumes the
         * team's situation is continuous; it has no way to know the
         * team it was tracking got reassigned entirely. Without this
         * reset, a RETURN_TO_SAFE_ZONE (or LOCAL_RECOVERY) recommendation
         * built up while the team was RESTING/idle survives the steal,
         * gets recomputed as still active this same tick, and
         * recovery_dispatch()'s only guard against acting on a RESTING
         * team (`status != STATUS_RESTING`) does not catch this case -
         * status is now EN_ROUTE with a legitimate fresh assignment, so
         * the guard passes and abandon_task_for_recovery() cancels the
         * brand-new incident and sends the team straight back to the
         * safe zone it just left, which (since it's already standing
         * there) re-enters RESTING the same tick, gets stolen again the
         * moment allocate_all_incidents() next runs, and repeats - the
         * RESTING<->EN_ROUTE bounce. Detecting the assignment change
         * here and starting this team's recommendation fresh at
         * CONTINUE_MONITORING gives the new mission an unbiased
         * evaluation instead of inheriting a verdict about a mission it
         * was never actually running. */
        /* Second, related case: update_team()'s STATUS_RESTING handler
         * (core/team.c) can promote a team straight to STATUS_AVAILABLE
         * the instant its stamina/equipment clear team_set_available()'s
         * bar - in the same tick, before this function or
         * recovery_dispatch() run. assigned_task_pool/slot don't change
         * for this transition (both stay TASK_POOL_NONE/NO_ASSIGNMENT
         * across RESTING and idle AVAILABLE), so the pool/slot check
         * above can't see it - but it's the exact same stale-escalation
         * problem: a RETURN_TO_SAFE_ZONE (or LOCAL_RECOVERY) recommendation
         * built up while this team was RESTING has no bearing on a team
         * that has just finished resting and is now genuinely eligible
         * again. Left unhandled, recovery_dispatch()'s RETURN_TO_SAFE_ZONE
         * case (guarded only by `status != STATUS_RESTING`, which an
         * AVAILABLE team fails) fires immediately, abandons the team right
         * back toward the safe zone it's already standing in, which
         * re-enters RESTING the same tick, gets promoted back to AVAILABLE
         * the moment it's eligible again, and repeats every couple of
         * ticks - observed directly via this exact seed/team. */
        if(team->assigned_task_pool != state->last_assigned_task_pool ||
           team->assigned_task_slot != state->last_assigned_task_slot ||
           (state->last_status == STATUS_RESTING && team->status != STATUS_RESTING))
        {
            state->status.recommendation = RECOVERY_ACTION_CONTINUE_MONITORING;
            state->status.reason = RECOVERY_REASON_NONE;
            state->status.confidence = 0.0f;
            state->status.escalation_level = RECOVERY_ACTION_CONTINUE_MONITORING;
            state->pending_action = RECOVERY_ACTION_CONTINUE_MONITORING;
            state->pending_reason = RECOVERY_REASON_NONE;
            state->pending_streak = 0u;
            state->cooldown_ticks = 0u;
        }
        state->last_assigned_task_pool = team->assigned_task_pool;
        state->last_assigned_task_slot = team->assigned_task_slot;
        state->last_status = team->status;

        stage = recovery_stage_for_team(team, twin);

        for(size_t i = 0; i < RECOVERY_MANAGER_EVALUATOR_COUNT; i++)
            signals[i] = evaluators[i](team, twin, &watchdog);

        (void)recovery_build_status(&g_recovery_state[team_id], signals,
            RECOVERY_MANAGER_EVALUATOR_COUNT, stage);
        recovery_metrics_note_recommendation(
            team_id,
            g_recovery_state[team_id].status.recommendation);
        recovery_metrics_update_resolution(
            team_id,
            g_recovery_state[team_id].status.recommendation);
    }

    g_recovery_metrics.evaluation_ticks++;
}

void recovery_dispatch(uint8_t team_id, RecoveryAction_t action)
{
    TeamState_t *team;
    RecoveryStatus_t status;
    RecoveryDispatchState_t *dispatch;
    uint64_t signature;

    if(team_id >= NUM_TEAMS)
        return;

    team = &g_teams[team_id];
    status = recovery_manager_get_status(team_id);
    dispatch = &g_recovery_dispatch_state[team_id];
    signature = recovery_team_signature(team) ^
        ((uint64_t)status.escalation_level << 56);

    if(dispatch->last_action == action &&
       dispatch->last_escalation_level == status.escalation_level &&
       dispatch->last_signature == signature &&
       dispatch->last_tick == g_current_tick)
    {
        return;
    }

    dispatch->last_action = action;
    dispatch->last_escalation_level = status.escalation_level;
    dispatch->last_signature = signature;
    dispatch->last_tick = g_current_tick;

    switch(action)
    {
        case RECOVERY_ACTION_CONTINUE_MONITORING:
            break;

        case RECOVERY_ACTION_LOCAL_RECOVERY:
            /* LOCAL_RECOVERY is the mildest recovery action (lowest
             * confidence floor of any action), so it should never be more
             * disruptive than the higher-severity actions above it in the
             * ladder. It previously cleared any active assignment and
             * forced RESTING unconditionally - which meant it could (and
             * did) interrupt a team mid-travel to a real incident before it
             * ever arrived, well before the more careful
             * RETURN_TO_SAFE_ZONE (which preserves mission state via
             * abandon_task_for_recovery) even had a chance to fire. Only
             * apply it when the team has no active mission to interrupt;
             * a team actively EN_ROUTE/ON_SCENE keeps working, and if the
             * underlying issue is serious enough, the recovery manager will
             * naturally escalate to RETURN_TO_SAFE_ZONE instead.
             *
             * Second guard: only act on STATUS_AVAILABLE specifically, not
             * STATUS_RESTING too. Forcing an already-RESTING team back to
             * RESTING is a pure no-op on its own - but combined with
             * update_team()'s RESTING->AVAILABLE promotion (team.c,
             * fires the instant stamina/equipment clear their thresholds),
             * a still-standing LOCAL_RECOVERY recommendation that hasn't
             * caught up to the team's now-healthy state yet would slam it
             * straight back to RESTING the same tick it got promoted,
             * every tick, forever - the recovery-manager equivalent of the
             * RETURN_TO_SAFE_ZONE oscillation above. A RESTING team is
             * already doing exactly what LOCAL_RECOVERY wants; there's
             * nothing left to apply.
             *
             * Third fix: setting team->status = STATUS_RESTING directly
             * (the old code just cleared the assignment in place) has
             * zero location check - it marks the team "resting" wherever
             * it happens to be
             * standing, which is how both Search teams ended up idling in
             * the middle of the road and Support 4 ended up "resting"
             * outside both safe zones. RESTING is supposed to mean "at a
             * safe zone"; use the exact same safe-zone-return machinery
             * RETURN_TO_SAFE_ZONE relies on (abandon_task_for_recovery ->
             * begin_return_to_safe_zone) so LOCAL_RECOVERY actually
             * sends the team home (EN_ROUTE) instead of freezing it in
             * place. abandon_task_for_recovery already no-ops safely when
             * there's nothing to abandon (the common AVAILABLE case here)
             * and falls back to team_set_available() if no safe zone slot
             * is free, so this is a strict improvement with no new
             * failure mode. */
            if(team->status == STATUS_AVAILABLE)
            {
                abandon_task_for_recovery(team);
            }
            break;

        case RECOVERY_ACTION_RETURN_TO_SAFE_ZONE:
            /* Same guard LOCAL_RECOVERY uses above, for the same reason:
             * without it, a team that is already RESTING (i.e. already
             * arrived and recovering, possibly at the exact safe zone
             * this would send it to) gets pulled off RESTING and handed
             * a brand-new return-to-safe-zone task every tick the
             * recommendation stays active - abandon_task_for_recovery
             * unconditionally sets status back to EN_ROUTE, and if the
             * team is already standing on the destination coordinates,
             * the arrival check in move_team() fires the same tick,
             * putting it straight back to RESTING - which triggers this
             * same dispatch again next tick. A RESTING team has nothing
             * to abandon; let it keep recovering. */
            if(team->status != STATUS_RESTING)
                abandon_task_for_recovery(team);
            break;

        case RECOVERY_ACTION_REQUEST_TASK_REALLOCATION:
            if(g_realloc_done_tick != g_current_tick)
            {
                allocate_all_incidents();
                g_realloc_done_tick = g_current_tick;
            }
            break;

        case RECOVERY_ACTION_ABORT_MISSION:
            /* ABORT_MISSION only makes sense for a team that actually has a
             * mission in progress (EN_ROUTE / ON_SCENE). A RESTING or already
             * AVAILABLE team has nothing to abort; forcing it to AVAILABLE in
             * that case just cuts recovery short (e.g. a severe communication
             * fault landing on a resting team used to yank it out of rest
             * instantly, regardless of stamina/equipment recovery progress). */
            if(team->status == STATUS_EN_ROUTE || team->status == STATUS_ON_SCENE)
            {
                abandon_task_for_recovery(team);
            }
            break;
    }

    recovery_metrics_note_dispatch(team_id, action);
}

RecoveryAction_t recovery_manager_get_recommendation(uint8_t team_id)
{
    if(team_id >= NUM_TEAMS)
        return RECOVERY_ACTION_CONTINUE_MONITORING;

    return g_recovery_state[team_id].status.recommendation;
}

RecoveryStatus_t recovery_manager_get_status(uint8_t team_id)
{
    if(team_id >= NUM_TEAMS)
    {
        RecoveryStatus_t status;

        status.recommendation = RECOVERY_ACTION_CONTINUE_MONITORING;
        status.reason = RECOVERY_REASON_NONE;
        status.confidence = 0.0f;
        status.escalation_level = RECOVERY_ACTION_CONTINUE_MONITORING;
        return status;
    }

    return recovery_status_from_state(&g_recovery_state[team_id]);
}

RecoveryMetrics_t recovery_manager_get_metrics(void)
{
    return g_recovery_metrics;
}
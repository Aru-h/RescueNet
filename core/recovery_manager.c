#include "recovery_manager.h"

#include <limits.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

#include "allocator.h"
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
    uint8_t allocated_this_tick;
} RecoveryDispatchState_t;

typedef RecoverySignal_t (*RecoveryEvaluatorFn)(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const WatchdogStatus_t *watchdog);

static RecoveryManagerTeamState_t g_recovery_state[NUM_TEAMS];
static RecoveryDispatchState_t g_recovery_dispatch_state[NUM_TEAMS];
static RecoveryMetricsTeamState_t g_recovery_metrics_state[NUM_TEAMS];
static RecoveryMetrics_t g_recovery_metrics;
static uint32_t g_last_dispatch_tick = UINT32_MAX;

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
    signature ^= (uint64_t)team->fault_state.active_mask << 52;

    return signature;
}

static void recovery_clear_assignment_only(TeamState_t *team)
{
    IncidentState_t *task = NULL;
    TaskPool_t pool = (TaskPool_t)team->assigned_task_pool;

    if(pool == TASK_POOL_INCIDENT &&
       team->assigned_task_slot < NUM_INCIDENTS)
    {
        task = &g_incidents[team->assigned_task_slot];
    }
    else if(pool == TASK_POOL_SYSTEM &&
            team->assigned_task_slot < NUM_SYSTEM_TASKS)
    {
        task = &g_system_tasks[team->assigned_task_slot];
    }

    if(task != NULL && task->slot_occupied)
    {
        if(task->assigned_team == team->team_id)
            task->assigned_team = NO_ASSIGNMENT;
        if(task->support_team == team->team_id)
            task->support_team = NO_ASSIGNMENT;

        if(task->status == INC_ASSIGNED)
            task->status = INC_OPEN;
    }

    team->assigned_task_pool = TASK_POOL_NONE;
    team->assigned_task_slot = NO_ASSIGNMENT;
    team->incident_queue_size = 0;
    team->incident_queue_index = 0;

    for(uint8_t q = 0; q < CBBA_MAX_BUNDLE_SIZE; q++)
    {
        team->incident_queue_slot[q] = NO_ASSIGNMENT;
        team->incident_queue_id[q] = 0;
    }
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

        if(signal->confidence >= best_confidence)
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

            if(signal->confidence >= best_confidence)
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

    (void)watchdog;

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_LOCAL_RECOVERY))
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
    else if(team->degraded || team->stamina <= RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD)
    {
        /* No active mission to abort - route to rest instead, same as the
         * TRAVEL/WORK branch below does for a less severe deviation. */
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

    if(team->fault_state.active[FAULT_CLASS_MOBILITY])
    {
        float fault_severity = (float)team->fault_state.severity[FAULT_CLASS_MOBILITY] /
            RECOVERY_MANAGER_MOBILITY_SEVERITY_SCALE;

        if(fault_severity > severity)
            severity = fault_severity;
    }

    confidence = recovery_clamp01(severity * recovery_stage_multiplier_mobility(stage));

    if(confidence < recovery_stage_action_floor(stage, RECOVERY_ACTION_RETURN_TO_SAFE_ZONE))
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
        g_recovery_state[team_id].cooldown_ticks = 0u;
        g_recovery_dispatch_state[team_id] = (RecoveryDispatchState_t){0};
    }
    g_last_dispatch_tick = UINT32_MAX;
}

void recovery_manager_step(void)
{
    static const RecoveryEvaluatorFn evaluators[RECOVERY_MANAGER_EVALUATOR_COUNT] =
    {
        evaluate_progress,
        evaluate_stamina,
        evaluate_mobility,
        evaluate_communication,
        evaluate_equipment
    };

    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        const TeamState_t *team = &g_teams[team_id];
        const MissionExecutionTwin_t *twin = &g_mission_execution_twin[team_id];
        WatchdogStatus_t watchdog = watchdog_get_status(team_id);
        RecoverySignal_t signals[RECOVERY_MANAGER_EVALUATOR_COUNT];
        MissionStage_t stage = recovery_stage_for_team(team, twin);

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

    if(g_last_dispatch_tick != g_current_tick)
    {
        for(uint8_t i = 0; i < NUM_TEAMS; i++)
            g_recovery_dispatch_state[i].allocated_this_tick = 0;
        g_last_dispatch_tick = g_current_tick;
    }

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
             * nothing left to apply. */
            if(team->status == STATUS_AVAILABLE)
            {
                recovery_clear_assignment_only(team);
                team->status = STATUS_RESTING;
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
            if(!dispatch->allocated_this_tick)
            {
                allocate_all_incidents();
                dispatch->allocated_this_tick = 1u;
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

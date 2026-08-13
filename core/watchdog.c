#include "watchdog.h"

#include <math.h>
#include <stdint.h>

#include "mission_state.h"

#define WATCHDOG_PROGRESS_ERROR_FULL_SCALE 0.35f
#define WATCHDOG_STAMINA_ERROR_FULL_SCALE 18.0f
#define WATCHDOG_CONFIDENCE_RISE 0.35f
#define WATCHDOG_CONFIDENCE_DECAY 0.80f
#define WATCHDOG_PROGRESS_WEIGHT 0.2222f
#define WATCHDOG_STAMINA_WEIGHT 0.1667f
#define WATCHDOG_COMMUNICATION_WEIGHT 0.2037f
#define WATCHDOG_OPERATING_STATE_WEIGHT 0.1296f
#define WATCHDOG_PATH_WEIGHT 0.1481f
#define WATCHDOG_ASSIGNMENT_WEIGHT 0.0556f
#define WATCHDOG_TIMEOUT_WEIGHT 0.0741f
#define WATCHDOG_ANOMALY_THRESHOLD 0.35f
#define WATCHDOG_INVESTIGATE_THRESHOLD 0.18f
#define WATCHDOG_STAGNATION_STREAK_MAX 5u
#define WATCHDOG_TIMEOUT_STREAK_MAX 6u

typedef enum
{
    WATCHDOG_EVIDENCE_SLOT_PROGRESS = 0,
    WATCHDOG_EVIDENCE_SLOT_STAMINA,
    WATCHDOG_EVIDENCE_SLOT_COMMUNICATION,
    WATCHDOG_EVIDENCE_SLOT_OPERATING_STATE,
    WATCHDOG_EVIDENCE_SLOT_PATH,
    WATCHDOG_EVIDENCE_SLOT_ASSIGNMENT,
    WATCHDOG_EVIDENCE_SLOT_TIMEOUT,

    NUM_WATCHDOG_EVIDENCE_SLOTS
} WatchdogEvidenceSlot_t;

typedef struct
{
    WatchdogStatus_t status;
    float accumulated_evidence[NUM_WATCHDOG_EVIDENCE_SLOTS];
    uint16_t last_remaining_cost;
    uint32_t last_observed_task_id;
    uint8_t last_assigned_task_pool;
    uint8_t last_assigned_task_slot;
    uint8_t last_status;
    uint8_t has_last_sample;
    uint8_t path_stagnation_streak;
    uint8_t timeout_streak;
} WatchdogTeamState_t;

static WatchdogTeamState_t g_watchdog_state[NUM_TEAMS];

static float clamp01(float value)
{
    if(value < 0.0f)
        return 0.0f;

    if(value > 1.0f)
        return 1.0f;

    return value;
}

static float normalized_error(float magnitude, float full_scale)
{
    if(full_scale <= 0.0f)
        return 0.0f;

    return clamp01(magnitude / full_scale);
}

static float accumulate_evidence(float previous, float instantaneous)
{
    instantaneous = clamp01(instantaneous);

    if(instantaneous > previous)
        return previous + WATCHDOG_CONFIDENCE_RISE * (instantaneous - previous);

    return previous * WATCHDOG_CONFIDENCE_DECAY;
}

static float progress_evidence(
    const MissionExecutionTwin_t *twin)
{
    return normalized_error(fabsf(twin->progress_error), WATCHDOG_PROGRESS_ERROR_FULL_SCALE);
}

static float stamina_evidence(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin)
{
    float evidence = normalized_error(fabsf(twin->stamina_error), WATCHDOG_STAMINA_ERROR_FULL_SCALE);

    if(team->stamina <= LIGHT_MIN_STAMINA)
        evidence = clamp01(evidence + 0.20f);

    return evidence;
}

static float communication_evidence(
    const MissionExecutionTwin_t *twin)
{
    return clamp01((float)twin->communication_health / 3.0f);
}

static float operating_state_evidence(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin)
{
    return team->status == twin->expected_operating_state ? 0.0f : 1.0f;
}

static float path_evidence(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    WatchdogTeamState_t *watchdog)
{
    if(twin->mission_stage != MISSION_STAGE_TRAVEL ||
       twin->assigned_task_pool == TASK_POOL_NONE ||
       twin->assigned_task_slot == NO_ASSIGNMENT)
    {
        watchdog->path_stagnation_streak = 0;
        return 0.0f;
    }

    if(!watchdog->has_last_sample)
        return 0.0f;

    if(twin->remaining_path_cost < watchdog->last_remaining_cost)
    {
        watchdog->path_stagnation_streak = 0;
        return 0.0f;
    }

    if(twin->progress_error <= 0.0f &&
       !team->fault_state.active[FAULT_CLASS_MOBILITY])
    {
        watchdog->path_stagnation_streak = 0;
        return 0.0f;
    }

    if(team->fault_state.active[FAULT_CLASS_MOBILITY])
    {
        if(watchdog->path_stagnation_streak + 2u < WATCHDOG_STAGNATION_STREAK_MAX)
            watchdog->path_stagnation_streak = (uint8_t)(watchdog->path_stagnation_streak + 2u);
        else
            watchdog->path_stagnation_streak = WATCHDOG_STAGNATION_STREAK_MAX;
    }
    else if(watchdog->path_stagnation_streak < WATCHDOG_STAGNATION_STREAK_MAX)
    {
        watchdog->path_stagnation_streak++;
    }

    {
        float evidence = clamp01(
        (float)watchdog->path_stagnation_streak /
        (float)WATCHDOG_STAGNATION_STREAK_MAX);

        if(team->fault_state.active[FAULT_CLASS_MOBILITY])
            evidence = clamp01(0.35f + 0.65f * evidence);

        return evidence;
    }
}

static float assignment_evidence(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    WatchdogTeamState_t *watchdog)
{
    if(!watchdog->has_last_sample)
        return 0.0f;

    if(watchdog->last_assigned_task_pool == TASK_POOL_NONE ||
       watchdog->last_assigned_task_slot == NO_ASSIGNMENT)
    {
        return 0.0f;
    }

    if(team->assigned_task_pool == watchdog->last_assigned_task_pool &&
       team->assigned_task_slot == watchdog->last_assigned_task_slot)
    {
        return 0.0f;
    }

    if(twin->actual_progress < 0.90f)
        return 1.0f;

    return 0.25f;
}

static float timeout_evidence(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    WatchdogTeamState_t *watchdog)
{
    float evidence = 0.0f;

    if(twin->assigned_task_pool == TASK_POOL_NONE ||
       twin->assigned_task_slot == NO_ASSIGNMENT)
    {
        watchdog->timeout_streak = 0;
        return 0.0f;
    }

    if(twin->expected_operating_state == STATUS_ON_SCENE &&
       team->status != STATUS_ON_SCENE &&
       g_current_tick > twin->expected_arrival_tick)
    {
        if(watchdog->timeout_streak < WATCHDOG_TIMEOUT_STREAK_MAX)
            watchdog->timeout_streak++;
    }
    else if(twin->expected_progress >= 0.999f &&
            twin->actual_progress + 0.05f < twin->expected_progress)
    {
        if(watchdog->timeout_streak < WATCHDOG_TIMEOUT_STREAK_MAX)
            watchdog->timeout_streak++;
    }
    else
    {
        watchdog->timeout_streak = 0;
    }

    evidence = clamp01(
        (float)watchdog->timeout_streak /
        (float)WATCHDOG_TIMEOUT_STREAK_MAX);

    return evidence;
}

static uint32_t evidence_flags_from_values(const float *values)
{
    uint32_t flags = 0;

    if(values[WATCHDOG_EVIDENCE_SLOT_PROGRESS] >= 0.20f)
        flags |= WATCHDOG_EVIDENCE_PROGRESS;
    if(values[WATCHDOG_EVIDENCE_SLOT_STAMINA] >= 0.20f)
        flags |= WATCHDOG_EVIDENCE_STAMINA;
    if(values[WATCHDOG_EVIDENCE_SLOT_COMMUNICATION] >= 0.20f)
        flags |= WATCHDOG_EVIDENCE_COMMUNICATION;
    if(values[WATCHDOG_EVIDENCE_SLOT_OPERATING_STATE] >= 0.20f)
        flags |= WATCHDOG_EVIDENCE_OPERATING_STATE;
    if(values[WATCHDOG_EVIDENCE_SLOT_PATH] >= 0.20f)
        flags |= WATCHDOG_EVIDENCE_PATH;
    if(values[WATCHDOG_EVIDENCE_SLOT_ASSIGNMENT] >= 0.20f)
        flags |= WATCHDOG_EVIDENCE_ASSIGNMENT;
    if(values[WATCHDOG_EVIDENCE_SLOT_TIMEOUT] >= 0.20f)
        flags |= WATCHDOG_EVIDENCE_TIMEOUT;

    return flags;
}

static float weighted_confidence(const float *values)
{
    return clamp01(
        values[WATCHDOG_EVIDENCE_SLOT_PROGRESS] * WATCHDOG_PROGRESS_WEIGHT +
        values[WATCHDOG_EVIDENCE_SLOT_STAMINA] * WATCHDOG_STAMINA_WEIGHT +
        values[WATCHDOG_EVIDENCE_SLOT_COMMUNICATION] * WATCHDOG_COMMUNICATION_WEIGHT +
        values[WATCHDOG_EVIDENCE_SLOT_OPERATING_STATE] * WATCHDOG_OPERATING_STATE_WEIGHT +
        values[WATCHDOG_EVIDENCE_SLOT_PATH] * WATCHDOG_PATH_WEIGHT +
        values[WATCHDOG_EVIDENCE_SLOT_ASSIGNMENT] * WATCHDOG_ASSIGNMENT_WEIGHT +
        values[WATCHDOG_EVIDENCE_SLOT_TIMEOUT] * WATCHDOG_TIMEOUT_WEIGHT);
}

static WatchdogFailureMode_t classify_failure_mode(
    const TeamState_t *team,
    const MissionExecutionTwin_t *twin,
    const float *values)
{
    float best = 0.0f;
    WatchdogFailureMode_t mode = WATCHDOG_FAILURE_NONE;

    if(values[WATCHDOG_EVIDENCE_SLOT_COMMUNICATION] > best)
    {
        best = values[WATCHDOG_EVIDENCE_SLOT_COMMUNICATION];
        mode = WATCHDOG_FAILURE_COMMUNICATION_FAILURE;
    }

    if((values[WATCHDOG_EVIDENCE_SLOT_PATH] +
        values[WATCHDOG_EVIDENCE_SLOT_PROGRESS] * 0.5f) > best)
    {
        best = values[WATCHDOG_EVIDENCE_SLOT_PATH] +
            values[WATCHDOG_EVIDENCE_SLOT_PROGRESS] * 0.5f;
        mode = team->fault_state.active[FAULT_CLASS_MOBILITY]
            ? WATCHDOG_FAILURE_MOBILITY_DEGRADATION
            : WATCHDOG_FAILURE_TASK_EXECUTION_DELAY;
    }

    if((values[WATCHDOG_EVIDENCE_SLOT_STAMINA] +
        values[WATCHDOG_EVIDENCE_SLOT_TIMEOUT] * 0.25f) > best)
    {
        best = values[WATCHDOG_EVIDENCE_SLOT_STAMINA] +
            values[WATCHDOG_EVIDENCE_SLOT_TIMEOUT] * 0.25f;
        mode = WATCHDOG_FAILURE_FATIGUE_EXHAUSTION;
    }

    if(team->equipment_level <= 20 &&
       values[WATCHDOG_EVIDENCE_SLOT_PROGRESS] >= 0.20f &&
       values[WATCHDOG_EVIDENCE_SLOT_STAMINA] < 0.40f)
    {
        best = 0.90f;
        mode = WATCHDOG_FAILURE_EQUIPMENT_DEGRADATION;
    }

    if((values[WATCHDOG_EVIDENCE_SLOT_TIMEOUT] +
        values[WATCHDOG_EVIDENCE_SLOT_PROGRESS] * 0.5f +
        values[WATCHDOG_EVIDENCE_SLOT_ASSIGNMENT] * 0.25f) > best)
    {
        best = values[WATCHDOG_EVIDENCE_SLOT_TIMEOUT] +
            values[WATCHDOG_EVIDENCE_SLOT_PROGRESS] * 0.5f +
            values[WATCHDOG_EVIDENCE_SLOT_ASSIGNMENT] * 0.25f;
        mode = WATCHDOG_FAILURE_TASK_EXECUTION_DELAY;
    }

    if(best >= 0.20f)
        return mode;

    if(twin->assigned_task_pool != TASK_POOL_NONE &&
       twin->assigned_task_slot != NO_ASSIGNMENT)
    {
        return WATCHDOG_FAILURE_UNKNOWN;
    }

    return WATCHDOG_FAILURE_NONE;
}

static uint8_t recommend_action(
    const WatchdogStatus_t *status)
{
    if(status->anomaly_confidence < WATCHDOG_INVESTIGATE_THRESHOLD)
        return 2u;

    if(status->anomaly_confidence < WATCHDOG_ANOMALY_THRESHOLD)
        return 1u;

    switch(status->dominant_failure_mode)
    {
        case WATCHDOG_FAILURE_COMMUNICATION_FAILURE:
            return 3u;

        case WATCHDOG_FAILURE_FATIGUE_EXHAUSTION:
        case WATCHDOG_FAILURE_EQUIPMENT_DEGRADATION:
            return 4u;

        case WATCHDOG_FAILURE_MOBILITY_DEGRADATION:
        case WATCHDOG_FAILURE_TASK_EXECUTION_DELAY:
            return 5u;

        case WATCHDOG_FAILURE_UNKNOWN:
            return 6u;

        case WATCHDOG_FAILURE_NONE:
        default:
            return 1u;
    }
}

void watchdog_init(void)
{
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        WatchdogTeamState_t *watchdog = &g_watchdog_state[team_id];

        *watchdog = (WatchdogTeamState_t){0};
        watchdog->last_remaining_cost = UINT16_MAX;
        watchdog->last_assigned_task_pool = TASK_POOL_NONE;
        watchdog->last_assigned_task_slot = NO_ASSIGNMENT;
        watchdog->status.recommended_action = 0u;
        watchdog->status.dominant_failure_mode = WATCHDOG_FAILURE_NONE;
    }
}

void watchdog_step(void)
{
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        const TeamState_t *team = &g_teams[team_id];
        const MissionExecutionTwin_t *twin = &g_mission_execution_twin[team_id];
        WatchdogTeamState_t *watchdog = &g_watchdog_state[team_id];
        float instantaneous[NUM_WATCHDOG_EVIDENCE_SLOTS] = {0};

        instantaneous[WATCHDOG_EVIDENCE_SLOT_PROGRESS] =
            progress_evidence(twin);
        instantaneous[WATCHDOG_EVIDENCE_SLOT_STAMINA] =
            stamina_evidence(team, twin);
        instantaneous[WATCHDOG_EVIDENCE_SLOT_COMMUNICATION] =
            communication_evidence(twin);
        instantaneous[WATCHDOG_EVIDENCE_SLOT_OPERATING_STATE] =
            operating_state_evidence(team, twin);
        instantaneous[WATCHDOG_EVIDENCE_SLOT_PATH] =
            path_evidence(team, twin, watchdog);
        instantaneous[WATCHDOG_EVIDENCE_SLOT_ASSIGNMENT] =
            assignment_evidence(team, twin, watchdog);
        instantaneous[WATCHDOG_EVIDENCE_SLOT_TIMEOUT] =
            timeout_evidence(team, twin, watchdog);

        for(uint8_t i = 0; i < NUM_WATCHDOG_EVIDENCE_SLOTS; i++)
        {
            watchdog->accumulated_evidence[i] = accumulate_evidence(
                watchdog->accumulated_evidence[i],
                instantaneous[i]);
        }

        watchdog->status.evidence_flags =
            evidence_flags_from_values(watchdog->accumulated_evidence);
        watchdog->status.anomaly_confidence =
            weighted_confidence(watchdog->accumulated_evidence);
        watchdog->status.anomaly_detected =
            watchdog->status.anomaly_confidence >= WATCHDOG_ANOMALY_THRESHOLD;
        watchdog->status.dominant_failure_mode =
            classify_failure_mode(team, twin, watchdog->accumulated_evidence);
        watchdog->status.recommended_action =
            recommend_action(&watchdog->status);

        watchdog->last_remaining_cost = twin->remaining_path_cost;
        watchdog->last_observed_task_id = twin->observed_task_id;
        watchdog->last_assigned_task_pool = (uint8_t)twin->assigned_task_pool;
        watchdog->last_assigned_task_slot = twin->assigned_task_slot;
        watchdog->last_status = team->status;
        watchdog->has_last_sample = 1;

        /* Suspected-but-not-yet-determined: watchdog confidence has crossed
         * the investigate threshold. Doesn't matter which evidence channel
         * or how many faults are actually behind it - any elevated
         * suspicion is enough to flag the team as degraded. */
        if(watchdog->status.anomaly_confidence >= WATCHDOG_INVESTIGATE_THRESHOLD)
        {
            g_teams[team_id].degraded = 1;
        }
        else
        {
            /* Confidence has decayed back to normal. Only actually clear
             * the flag if there's no confirmed-but-unrepaired fault still
             * hanging over this team - a real fault keeps the team flagged
             * for its entire lifecycle (through the two-stage repair
             * confirmation delay in fault_injector.c), regardless of
             * whether the watchdog's own noisy evidence has settled down
             * in the meantime. Pure watchdog noise (no fault ever actually
             * confirmed) clears on its own once confidence decays. */
            uint8_t any_fault_active = 0;
            for(uint8_t c = 0; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
            {
                if(g_teams[team_id].fault_state.active[c])
                {
                    any_fault_active = 1;
                    break;
                }
            }

            if(!any_fault_active)
                g_teams[team_id].degraded = 0;
        }
    }
}

WatchdogStatus_t watchdog_get_status(uint8_t team_id)
{
    if(team_id >= NUM_TEAMS)
        return (WatchdogStatus_t){0};

    return g_watchdog_state[team_id].status;
}

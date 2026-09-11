#ifndef RECOVERY_MANAGER_H
#define RECOVERY_MANAGER_H

#include <stdint.h>

typedef enum
{
    RECOVERY_ACTION_CONTINUE_MONITORING = 0,
    RECOVERY_ACTION_LOCAL_RECOVERY,
    RECOVERY_ACTION_RETURN_TO_SAFE_ZONE,
    RECOVERY_ACTION_REQUEST_TASK_REALLOCATION,
    RECOVERY_ACTION_ABORT_MISSION
} RecoveryAction_t;

typedef enum
{
    RECOVERY_REASON_NONE = 0,
    RECOVERY_REASON_PROGRESS_DEVIATION,
    RECOVERY_REASON_LOW_STAMINA,
    RECOVERY_REASON_MOBILITY_FAILURE,
    RECOVERY_REASON_COMMUNICATION_FAILURE,
    RECOVERY_REASON_EQUIPMENT_FAULT,
    RECOVERY_REASON_MISSION_DELAY,
    RECOVERY_REASON_TEAM_DEGRADED,
    RECOVERY_REASON_CONSENSUS_FAULT,
    RECOVERY_REASON_PREDICTED_FAULT
} RecoveryReason_t;

typedef struct
{
    RecoveryAction_t recommendation;
    RecoveryReason_t reason;
    float confidence;
    uint8_t escalation_level;
} RecoveryStatus_t;

enum
{
    RECOVERY_ACTION_COUNT = 5u
};

typedef struct
{
    uint32_t dispatched[RECOVERY_ACTION_COUNT];
    uint32_t resolved[RECOVERY_ACTION_COUNT];
    uint32_t resolution_ticks_sum[RECOVERY_ACTION_COUNT];
    uint32_t timed_out[RECOVERY_ACTION_COUNT];
    uint32_t recommendation_reversals;
    uint32_t evaluation_ticks;
} RecoveryMetrics_t;

void recovery_manager_init(void);
void recovery_manager_step(void);
void recovery_dispatch(uint8_t team_id, RecoveryAction_t action);
RecoveryAction_t recovery_manager_get_recommendation(uint8_t team_id);
RecoveryStatus_t recovery_manager_get_status(uint8_t team_id);
RecoveryMetrics_t recovery_manager_get_metrics(void);

#endif

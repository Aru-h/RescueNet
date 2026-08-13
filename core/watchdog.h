#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

typedef enum
{
    WATCHDOG_FAILURE_NONE = 0,
    WATCHDOG_FAILURE_MOBILITY_DEGRADATION,
    WATCHDOG_FAILURE_FATIGUE_EXHAUSTION,
    WATCHDOG_FAILURE_COMMUNICATION_FAILURE,
    WATCHDOG_FAILURE_EQUIPMENT_DEGRADATION,
    WATCHDOG_FAILURE_TASK_EXECUTION_DELAY,
    WATCHDOG_FAILURE_UNKNOWN
} WatchdogFailureMode_t;

enum
{
    WATCHDOG_EVIDENCE_PROGRESS = 1u << 0,
    WATCHDOG_EVIDENCE_STAMINA = 1u << 1,
    WATCHDOG_EVIDENCE_COMMUNICATION = 1u << 2,
    WATCHDOG_EVIDENCE_OPERATING_STATE = 1u << 3,
    WATCHDOG_EVIDENCE_PATH = 1u << 4,
    WATCHDOG_EVIDENCE_ASSIGNMENT = 1u << 5,
    WATCHDOG_EVIDENCE_TIMEOUT = 1u << 6
};

typedef struct
{
    uint8_t anomaly_detected;
    float anomaly_confidence;
    WatchdogFailureMode_t dominant_failure_mode;
    uint32_t evidence_flags;
    uint8_t recommended_action;
} WatchdogStatus_t;

void watchdog_init(void);
void watchdog_step(void);
WatchdogStatus_t watchdog_get_status(uint8_t team_id);

#endif

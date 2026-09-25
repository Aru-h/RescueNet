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
    uint8_t anomaly_detected;
    float anomaly_confidence;
    WatchdogFailureMode_t dominant_failure_mode;
    uint32_t evidence_flags;
    uint8_t recommended_action;
} WatchdogStatus_t;

void watchdog_init(void);
void watchdog_step(void);
WatchdogStatus_t watchdog_get_status(uint8_t team_id);

/* Board's own view of team_id's raw evidence vector + confirmed-mobility
   flag - neither is in WatchdogStatus_t above (that's the classifier's
   summarized output, not its inputs). Needed by core/fault_comms.c
   (FAULT_BROADCAST payload assembly) and core/fault_predictor.c (model
   input), both outside this file. 7 is NUM_WATCHDOG_EVIDENCE_SLOTS
   (watchdog.c, private) mirrored here by value, same as core/
   fault_buffer.h's FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS does - not the
   same constant, just the same number by coincidence of what the
   watchdog happens to track. out_evidence must have room for 7 floats;
   either output pointer may be NULL if the caller doesn't need it. An
   out-of-range team_id zeros both outputs rather than leaving them
   unset. */
void watchdog_get_evidence_vector(uint8_t team_id,
    float out_evidence[NUM_WATCHDOG_EVIDENCE_SLOTS],
    uint8_t *out_confirmed_mobility_impairment);

/* Fraction of team_id's recent EN_ROUTE ticks (current travel leg, up
   to the last 4) on which it failed to move a cell - see
   update_movement_stalls() in watchdog.c. 0.0 until at least 2 such
   ticks have been observed, and whenever the team isn't travelling. A
   healthy team never stalls; a severity-3 mobility fault stalls about
   every other tick (0.5). */
float watchdog_get_mobility_stall_fraction(uint8_t team_id);

#endif

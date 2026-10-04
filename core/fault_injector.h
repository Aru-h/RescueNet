#ifndef FAULT_INJECTOR_H
#define FAULT_INJECTOR_H

#include <stdint.h>

#include "fault_config.h"

void fault_injector_init(uint32_t seed);
void fault_injector_step(void);
void fault_injector_set_profile(FaultProfile_t profile);
FaultProfile_t fault_injector_get_profile(void);

/* Causal-profile bookkeeping (all zero under every other profile). Counts
 * are process-lifetime, like the g_diag_* counters; exposure_ticks counts
 * team-ticks with the class active. */
typedef struct
{
    uint32_t onsets[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t cleared_by_cause[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t cleared_by_backstop[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t exposure_ticks[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t active_ticks_total[NUM_OPERATIONAL_FAULT_CLASSES]; /* sum of episode lengths */
} CausalFaultStats_t;

const CausalFaultStats_t *fault_injector_causal_stats(void);

#endif
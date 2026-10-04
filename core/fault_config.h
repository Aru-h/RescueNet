#ifndef FAULT_CONFIG_H
#define FAULT_CONFIG_H

#include <stdint.h>

typedef enum
{
    FAULT_CLASS_MOBILITY = 0,
    FAULT_CLASS_COMMUNICATION,
    FAULT_CLASS_RESOURCE,
    FAULT_CLASS_ENVIRONMENT,
    FAULT_CLASS_MISSION,

    NUM_OPERATIONAL_FAULT_CLASSES
} OperationalFaultClass_t;

typedef enum
{
    FAULT_PROFILE_NONE = 0,
    FAULT_PROFILE_MOBILITY,
    FAULT_PROFILE_COMMUNICATION,
    FAULT_PROFILE_RESOURCE,
    FAULT_PROFILE_COMBINED,
    FAULT_PROFILE_RANDOM,
    /* Appended last so every existing profile keeps its numeric value (the
     * profile feeds snapshot.c's composite run_id). Faults here are cleared
     * by their cause being removed and drawn from a (seed, tick, team,
     * class) hash - see core/fault_injector.c. */
    FAULT_PROFILE_CAUSAL
} FaultProfile_t;

typedef struct
{
    uint8_t enabled;
    FaultProfile_t profile;
    uint8_t class_probability[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t duration_min_ticks[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t duration_max_ticks[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t effect_strength[NUM_OPERATIONAL_FAULT_CLASSES];
} FaultConfig_t;

/* Cause thresholds for FAULT_PROFILE_CAUSAL (read by fault_injector.c).
 * Every value is "assumed" - see docs/CAUSAL_FAULT_MODEL.md. */
#define CAUSAL_MOBILITY_REPAIR_REST_TICKS  4u  /* ticks RESTING at a safe zone = field repair done */
#define CAUSAL_COMM_REPOSITION_CELLS       3u  /* Manhattan cells moved from the onset cell */
#define CAUSAL_RESOURCE_RESUPPLY_LEVEL     90u /* equipment level that counts as resupplied (the fault's own drain keeps it just under 100) */

extern FaultConfig_t g_fault_config;

void fault_config_reset(void);
void fault_config_apply_profile(FaultProfile_t profile);
FaultProfile_t fault_profile_from_string(const char *name);
/* Strict variant: returns 1 and sets *out for a known name, 0 (leaving *out
 * untouched) for anything else. fault_profile_from_string() above keeps its
 * historical "unknown -> none" behaviour for existing callers. */
int fault_profile_parse(const char *name, FaultProfile_t *out);
int fault_profile_is_causal(void);
const char *fault_profile_to_string(FaultProfile_t profile);

#endif
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
    FAULT_PROFILE_RANDOM
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

extern FaultConfig_t g_fault_config;

void fault_config_reset(void);
void fault_config_apply_profile(FaultProfile_t profile);
FaultProfile_t fault_profile_from_string(const char *name);
const char *fault_profile_to_string(FaultProfile_t profile);

#endif
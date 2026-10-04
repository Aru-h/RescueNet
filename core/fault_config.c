#include "fault_config.h"

#include <string.h>

FaultConfig_t g_fault_config;

static void set_duration_range(
    OperationalFaultClass_t fault_class,
    uint8_t min_ticks,
    uint8_t max_ticks)
{
    g_fault_config.duration_min_ticks[fault_class] = min_ticks;
    g_fault_config.duration_max_ticks[fault_class] = max_ticks;
}

void fault_config_reset(void)
{
    memset(&g_fault_config, 0, sizeof(g_fault_config));
    g_fault_config.profile = FAULT_PROFILE_NONE;
}

void fault_config_apply_profile(FaultProfile_t profile)
{
    fault_config_reset();
    g_fault_config.enabled = (profile != FAULT_PROFILE_NONE);
    g_fault_config.profile = profile;

    switch(profile)
    {
        case FAULT_PROFILE_MOBILITY:
            g_fault_config.class_probability[FAULT_CLASS_MOBILITY] = 14;
            g_fault_config.effect_strength[FAULT_CLASS_MOBILITY] = 3;
            set_duration_range(FAULT_CLASS_MOBILITY, 8, 18);
            break;

        case FAULT_PROFILE_COMMUNICATION:
            g_fault_config.class_probability[FAULT_CLASS_COMMUNICATION] = 14;
            g_fault_config.effect_strength[FAULT_CLASS_COMMUNICATION] = 3;
            set_duration_range(FAULT_CLASS_COMMUNICATION, 6, 14);
            break;

        case FAULT_PROFILE_RESOURCE:
            g_fault_config.class_probability[FAULT_CLASS_RESOURCE] = 14;
            g_fault_config.effect_strength[FAULT_CLASS_RESOURCE] = 3;
            set_duration_range(FAULT_CLASS_RESOURCE, 6, 12);
            break;

        case FAULT_PROFILE_COMBINED:
            g_fault_config.class_probability[FAULT_CLASS_MOBILITY] = 8;
            g_fault_config.class_probability[FAULT_CLASS_COMMUNICATION] = 8;
            g_fault_config.class_probability[FAULT_CLASS_RESOURCE] = 8;
            g_fault_config.class_probability[FAULT_CLASS_ENVIRONMENT] = 7;
            g_fault_config.class_probability[FAULT_CLASS_MISSION] = 7;
            g_fault_config.effect_strength[FAULT_CLASS_MOBILITY] = 3;
            g_fault_config.effect_strength[FAULT_CLASS_COMMUNICATION] = 2;
            g_fault_config.effect_strength[FAULT_CLASS_RESOURCE] = 2;
            g_fault_config.effect_strength[FAULT_CLASS_ENVIRONMENT] = 3;
            g_fault_config.effect_strength[FAULT_CLASS_MISSION] = 2;
            set_duration_range(FAULT_CLASS_MOBILITY, 6, 14);
            set_duration_range(FAULT_CLASS_COMMUNICATION, 6, 14);
            set_duration_range(FAULT_CLASS_RESOURCE, 6, 14);
            set_duration_range(FAULT_CLASS_ENVIRONMENT, 5, 12);
            set_duration_range(FAULT_CLASS_MISSION, 5, 12);
            break;

        case FAULT_PROFILE_RANDOM:
            g_fault_config.class_probability[FAULT_CLASS_MOBILITY] = 10;
            g_fault_config.class_probability[FAULT_CLASS_COMMUNICATION] = 10;
            g_fault_config.class_probability[FAULT_CLASS_RESOURCE] = 10;
            g_fault_config.class_probability[FAULT_CLASS_ENVIRONMENT] = 10;
            g_fault_config.class_probability[FAULT_CLASS_MISSION] = 10;
            g_fault_config.effect_strength[FAULT_CLASS_MOBILITY] = 3;
            g_fault_config.effect_strength[FAULT_CLASS_COMMUNICATION] = 3;
            g_fault_config.effect_strength[FAULT_CLASS_RESOURCE] = 3;
            g_fault_config.effect_strength[FAULT_CLASS_ENVIRONMENT] = 4;
            g_fault_config.effect_strength[FAULT_CLASS_MISSION] = 2;
            set_duration_range(FAULT_CLASS_MOBILITY, 5, 16);
            set_duration_range(FAULT_CLASS_COMMUNICATION, 5, 16);
            set_duration_range(FAULT_CLASS_RESOURCE, 5, 16);
            set_duration_range(FAULT_CLASS_ENVIRONMENT, 5, 16);
            set_duration_range(FAULT_CLASS_MISSION, 5, 16);
            break;

        case FAULT_PROFILE_CAUSAL:
            /* Phase 1 placeholder onset rates, in PER-MILLE per team-tick
             * (every other profile uses percent). Combined's percent rates
             * are unusable here: combined survives them only because of the
             * 33-tick confirm delay and 80-tick immunity this profile drops
             * (measured: any-fault exposure 0.88, incidents resolved 6 vs
             * 36). Phase 2 replaces these with the hazard model. Recovery is
             * cause-driven (fault_injector.c); the duration range is only
             * the maximum backstop. */
            g_fault_config.class_probability[FAULT_CLASS_MOBILITY] = 12;
            g_fault_config.class_probability[FAULT_CLASS_COMMUNICATION] = 12;
            g_fault_config.class_probability[FAULT_CLASS_RESOURCE] = 12;
            g_fault_config.class_probability[FAULT_CLASS_ENVIRONMENT] = 10;
            g_fault_config.class_probability[FAULT_CLASS_MISSION] = 3;
            g_fault_config.effect_strength[FAULT_CLASS_MOBILITY] = 3;
            g_fault_config.effect_strength[FAULT_CLASS_COMMUNICATION] = 2;
            g_fault_config.effect_strength[FAULT_CLASS_RESOURCE] = 2;
            g_fault_config.effect_strength[FAULT_CLASS_ENVIRONMENT] = 3;
            g_fault_config.effect_strength[FAULT_CLASS_MISSION] = 2;
            set_duration_range(FAULT_CLASS_MOBILITY, 6, 14);
            set_duration_range(FAULT_CLASS_COMMUNICATION, 6, 14);
            set_duration_range(FAULT_CLASS_RESOURCE, 6, 14);
            set_duration_range(FAULT_CLASS_ENVIRONMENT, 5, 12);
            set_duration_range(FAULT_CLASS_MISSION, 5, 12);
            break;

        case FAULT_PROFILE_NONE:
        default:
            break;
    }
}

int fault_profile_parse(const char *name, FaultProfile_t *out)
{
    static const struct { const char *name; FaultProfile_t profile; } table[] = {
        { "none", FAULT_PROFILE_NONE },
        { "mobility", FAULT_PROFILE_MOBILITY },
        { "communication", FAULT_PROFILE_COMMUNICATION },
        { "resource", FAULT_PROFILE_RESOURCE },
        { "combined", FAULT_PROFILE_COMBINED },
        { "random", FAULT_PROFILE_RANDOM },
        { "causal", FAULT_PROFILE_CAUSAL },
    };

    if(name == NULL || out == NULL)
        return 0;

    for(size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
        if(strcmp(name, table[i].name) == 0)
        {
            *out = table[i].profile;
            return 1;
        }
    }

    return 0;
}

FaultProfile_t fault_profile_from_string(const char *name)
{
    FaultProfile_t profile = FAULT_PROFILE_NONE;

    (void)fault_profile_parse(name, &profile);
    return profile;
}

int fault_profile_is_causal(void)
{
    return g_fault_config.profile == FAULT_PROFILE_CAUSAL;
}

const char *fault_profile_to_string(FaultProfile_t profile)
{
    switch(profile)
    {
        case FAULT_PROFILE_MOBILITY: return "mobility";
        case FAULT_PROFILE_COMMUNICATION: return "communication";
        case FAULT_PROFILE_RESOURCE: return "resource";
        case FAULT_PROFILE_COMBINED: return "combined";
        case FAULT_PROFILE_RANDOM: return "random";
        case FAULT_PROFILE_CAUSAL: return "causal";
        case FAULT_PROFILE_NONE:
        default:
            return "none";
    }
}
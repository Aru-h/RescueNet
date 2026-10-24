#include "fault_config.h"

#include <string.h>

FaultConfig_t g_fault_config;
float g_fault_stress = 1.0f;

/* Defaults; see docs/CAUSAL_FAULT_MODEL.md for the source of each value. */
CausalFaultParams_t g_causal_params = {
    .tx_power_dbm = 14.0f,
    .body_loss_db = 5.0f,
    .pl0_db = 31.2f,
    .d0_m = 1.0f,
    .path_loss_exponent = 2.35f,
    .shadow_sigma_db = 7.9f,
    .shadow_lattice_cells = 3.0f,
    .building_penalty_db = 20.0f,
    .rubble_penalty_db = 10.0f,
    .rx_sensitivity_dbm = -123.0f,
    .cell_size_m = 100.0f,
    .severity_db_per_level = 6.0f,
    .comm_clear_hysteresis_db = 3.0f,
    .fade_amp_db = 2.5f,
    .fade_window_ticks = 20.0f,

    .wear_per_cell = 0.002f,
    .wear_per_cell_rough = 0.010f,
    .mob_hazard_base = 1.0f / 3000.0f,
    .mob_hazard_per_wear = 0.0008f,
    .mob_hazard_rough = 0.002f,
    .wear_repair_reduction = 0.6f,
    .wear_rest_reduction = 0.3f,
    .wear_symptom_start = 3.0f,
    .wear_symptom_slope = 0.10f,
    .wear_symptom_max = 0.5f,

    .res_hazard_base = 1.0f / 3000.0f,
    .res_hazard_battery = 0.004f,
    .res_hazard_since_rest = 0.0005f,
    .res_since_rest_horizon = 800.0f,
    .res_mobility_cascade = 1.5f,
    .res_drain_stamina_floor = 25.0f,
    .res_drain_equipment_floor = 25.0f,

    .omori_k = 28.0f,
    .omori_c_ticks = 10.0f,
    .omori_p = 1.1f,
    .omori_t_offset_ticks = 200.0f,
    .aftershock_radius_cells = 3,
    .aftershock_route_lookahead = 8,

    .survival_lambda_h = 54.9f,
    .survival_k = 1.47f,
    .escalate_every_h = 12.0f,

    .hazard_cap = 0.25f,
    .max_concurrent_faults = 3,
};

/* Consequential roads. Grounding and "assumed" markers: docs/CONSEQUENTIAL_ROADS.md. */
CausalRoadParams_t g_causal_roads = {
    .enabled = 1,
    .partial_pct = 70,
    .crossing_delay_ticks = 4,
    .persist_min_ticks = 600,
    .persist_max_ticks = 2400,
    .clear_work_partial = 120,
    .clear_work_full = 240,
    .cost_per_tick = 3,   /* tuned on seeds 1001-1020 from {1,2,3,5}: see docs/CONSEQUENTIAL_ROADS.md */
    .route_cost_visible = 1,
};

uint8_t g_causal_fixes = CAUSAL_FIXES_ALL;

/* Step 3.6 persistent faults. Sources and "assumed" markers: docs/PERSISTENT_FAULTS_PROPOSAL.md section 9.1. */
CausalPersistParams_t g_causal_persist_params = {
    .natural_mean_ticks = 1084.0f,
    .field_min_ticks = 34,
    .field_max_ticks = 102,
    .field_success = 2.0f / 3.0f,
    .base_dwell_min_ticks = 17,
    .base_dwell_max_ticks = 51,
    .escalation_mult = 1.5f,
    .maintenance_wear_cut = 0.3f,
    .realloc_exclusion_ticks = 30,
    .forecast_horizon_ticks = 100.0f,
};
uint8_t g_causal_persist = CAUSAL_PERSIST_ON;

void fault_config_set_causal_persist(uint8_t mode)
{
    g_causal_persist = (mode > CAUSAL_PERSIST_ON) ? (uint8_t)CAUSAL_PERSIST_ON : mode;
}

int fault_profile_causal_persist(void)
{
    return g_fault_config.profile == FAULT_PROFILE_CAUSAL && g_causal_persist != CAUSAL_PERSIST_OFF;
}

int fault_profile_causal_field_repair(void)
{
    return g_fault_config.profile == FAULT_PROFILE_CAUSAL && g_causal_persist == CAUSAL_PERSIST_ON;
}

void fault_config_set_causal_fixes(uint8_t mask)
{
    g_causal_fixes = (uint8_t)(mask & CAUSAL_FIXES_VALID);
}

int fault_profile_causal_fix(unsigned fix)
{
    if(g_fault_config.profile != FAULT_PROFILE_CAUSAL || !(g_causal_fixes & fix))
        return 0;
    if(fix == CAUSAL_FIX_LABEL || fix == CAUSAL_FIX_STALL)
        return g_causal_roads.enabled;
    if(fix == CAUSAL_FIX_HOLD_REPAIR)
        return g_causal_persist == CAUSAL_PERSIST_ON;   /* only the mode that has field repair */
    return 1;
}

RecoveryMode_t g_recovery_mode = RECOVERY_MODE_FIXED;

PlannerParams_t g_planner = {
    .layers = PLANNER_LAYERS_ALL,
    .p_source = PLANNER_P_AI,
    .approval = 0,
    .log_enabled = 0,
    .calib_dump = 0,
    .preempt_k = 3,
    .preempt_cooldown_ticks = 30,   /* as RELAY_REPOSITION's cooldown (Step 3) */
    .v_mult = 1.0f,
    .margin_frac = 0.25f,           /* tuned on dev seeds 1001-1020 */
    .n_ref_victims = 5.07f,
    .age_ref_ticks = 266.0f,
    .reassign_delay = 40.0f,
    .ref_travel_ticks = 10.0f,
    .ref_stage_ticks = 30.0f,
    .rate_window_ticks = 100.0f,
    .blocked_detour = 2.0f,
    .cblind = 0.1f,
    .repair_mobility = 1.0f,
    .repair_resource = 1.0f,
    .repair_comm = 0.5f,
    .repair_scale = 1.0f,
    .crossing_scale = 1.0f,
    .p_log_floor = 0.05f,
};

int fault_profile_planner_on(void)
{
    return g_fault_config.profile == FAULT_PROFILE_CAUSAL && g_recovery_mode == RECOVERY_MODE_UTILITY;
}

int fault_profile_planner_layer(unsigned layer)
{
    return fault_profile_planner_on() && (g_planner.layers & layer) != 0u;
}

int fault_profile_causal_roads_on(void)
{
    return g_fault_config.profile == FAULT_PROFILE_CAUSAL && g_causal_roads.enabled;
}

CausalActionParams_t g_causal_actions = {
    .reroute_cost_penalty = 3,
    .env_crossing_delay_ticks = 0,
    .reroute_lookahead = 8,
    .reroute_max_ticks = 40,

    .clear_road_persist_ticks = 5,
    .clear_road_teams_threshold = 2,
    .clear_road_max_tasks = 2,
    .clear_road_work = 60,

    .reinforce_event_window = 60,
    .reinforce_hold_ticks = 150,
    .reinforce_work_boost = 1.25f,
    .reinforce_survival_factor = 0.5f,

    .relay_min_fault_ticks = 3,
    .relay_cooldown_ticks = 30,
    .relay_search_radius = 8,
    .relay_max_ticks = 20,
};
/* Off by default: on the approved Phase 2 model the actions cost incidents
 * (docs/RECOVERY_ACTIONS.md). Enable with --causal-actions=on|<list> until a
 * planner gates them. */
uint8_t g_causal_actions_enabled = 0;
static uint8_t g_causal_env_min = 0, g_causal_env_max = 0;

void fault_config_set_causal_env_duration(uint8_t min_ticks, uint8_t max_ticks)
{
    g_causal_env_min = min_ticks;
    g_causal_env_max = max_ticks;
}

void fault_config_set_causal_env_crossing_delay(uint8_t ticks)
{
    g_causal_actions.env_crossing_delay_ticks = ticks;
}

void fault_config_set_causal_reroute_penalty(uint16_t penalty)
{
    g_causal_actions.reroute_cost_penalty = penalty;
}

void fault_config_set_causal_actions(uint8_t mask)
{
    g_causal_actions_enabled = (uint8_t)(mask & CAUSAL_ACTIONS_ALL);
}

int fault_profile_causal_actions_on(void)
{
    return g_fault_config.profile == FAULT_PROFILE_CAUSAL && g_causal_actions_enabled != 0u;
}

int fault_profile_causal_action_enabled(unsigned action)
{
    return g_fault_config.profile == FAULT_PROFILE_CAUSAL && ((g_causal_actions_enabled >> action) & 1u);
}

void fault_config_set_stress(float stress)
{
    g_fault_stress = stress;
}

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
            /* Onset is not a per-class probability here: mobility/resource
             * come from the hazard model, communication from the link
             * margin, environment from Omori aftershocks and mission from
             * the survival model (fault_injector.c), so class_probability
             * stays 0. The duration ranges are only the maximum backstops
             * (communication has none). */
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
            if(g_causal_env_max != 0)
                set_duration_range(FAULT_CLASS_ENVIRONMENT, g_causal_env_min, g_causal_env_max);
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
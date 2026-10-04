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
 * Every value is documented, with its source or "assumed", in
 * docs/CAUSAL_FAULT_MODEL.md. */
#define CAUSAL_MOBILITY_REPAIR_REST_TICKS  4u  /* ticks RESTING at a safe zone = field repair done */
#define CAUSAL_RESOURCE_RESUPPLY_LEVEL     90u /* equipment level that counts as resupplied (the fault's own drain keeps it just under 100) */

typedef struct
{
    /* --- Communication: deterministic link margin ------------------------
     * margin = tx - body - PL(d) - shadow(cell) - building - rubble - rx_sens
     * PL(d) = pl0 + 10 n log10(d / d0)                                    */
    float tx_power_dbm;            /* LORA_TX_POWER_DBM (core/lora_radio_params.h) */
    float body_loss_db;            /* handheld/body + antenna loss */
    float pl0_db;                  /* free-space path loss at d0 */
    float d0_m;
    float path_loss_exponent;      /* n */
    float shadow_sigma_db;         /* log-normal shadowing sigma, fixed per cell */
    float building_penalty_db;     /* at district_density == 100 */
    float rubble_penalty_db;       /* at infrastructure_damage == 100 */
    float rx_sensitivity_dbm;      /* SF7 / BW125 */
    float cell_size_m;
    float severity_db_per_level;   /* comm fault severity = 1 + deficit / this, capped at 4 */
    float comm_clear_hysteresis_db; /* fault at margin < 0; clears only above +this */
    float fade_amp_db;             /* slow fading: +/- this, per cell and time window */
    float fade_window_ticks;

    /* --- Mobility: hidden cumulative wear -------------------------------- */
    float wear_per_cell;           /* wear added per cell crossed */
    float wear_per_cell_rough;     /* extra wear per cell crossed at roughness 1 */
    float mob_hazard_base;         /* per tick */
    float mob_hazard_per_wear;     /* per tick per wear unit */
    float mob_hazard_rough;        /* per tick while crossing a roughness-1 cell */
    float wear_repair_reduction;   /* fraction of wear removed by a field repair (fault cleared by cause) */
    float wear_rest_reduction;     /* fraction removed by one safe-zone rest of >= repair dwell, no fault active */
    float wear_symptom_start;      /* wear above which movement visibly slows */
    float wear_symptom_slope;      /* skip probability per wear unit above start */
    float wear_symptom_max;

    /* --- Resource -------------------------------------------------------- */
    float res_hazard_base;         /* per tick */
    float res_hazard_battery;      /* per tick at equipment 0, quadratic in depletion */
    float res_hazard_since_rest;   /* per tick once since_rest reaches the horizon */
    float res_since_rest_horizon;  /* ticks */
    float res_mobility_cascade;    /* hazard multiplier while a mobility fault is active */
    float res_drain_stamina_floor; /* resource-fault drain stops below this stamina */
    float res_drain_equipment_floor;

    /* --- Environment: Omori-Utsu aftershocks R(t) = K / (c + t)^p -------- */
    float omori_k;                 /* events per tick at (c + t) == 1 */
    float omori_c_ticks;
    float omori_p;
    float omori_t_offset_ticks;    /* mainshock-to-tick-0 delay */
    uint8_t aftershock_radius_cells;
    uint8_t aftershock_route_lookahead;

    /* --- Mission: entrapment survival (Weibull fit to MacIntyre 2006) ---- */
    float survival_lambda_h;
    float survival_k;
    float escalate_every_h;        /* priority steps one level toward CRITICAL */

    /* --- Bounds ----------------------------------------------------------- */
    float hazard_cap;              /* max per-tick hazard for any class */
    uint8_t max_concurrent_faults; /* per team, stochastic classes */
} CausalFaultParams_t;

extern CausalFaultParams_t g_causal_params;
/* Evaluation stress multiplier (--fault-stress=<x>, default 1 = realistic
 * field rates). Scales mobility/resource hazards and the Omori rate K ONLY.
 * It does not scale the deterministic link-margin model or the mission
 * clock (victim survival/escalation always run in real time).
 * Survives fault_config_apply_profile(), which runs once per seed. */
extern float g_fault_stress;
void fault_config_set_stress(float stress);

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
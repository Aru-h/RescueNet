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
    float shadow_sigma_db;         /* log-normal shadowing sigma */
    float shadow_lattice_cells;    /* spacing of the hashed noise lattice; sets the decorrelation distance */
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

/* Recovery actions that only exist under the causal profile (Step 3):
 * REROUTE, CLEAR_ROAD, REINFORCE_INCIDENT, RELAY_REPOSITION. They are NOT
 * RecoveryAction_t values: that enum is written to the dataset CSV
 * (recovery_recommendation) and compared numerically by tools, so it is
 * left alone. Every effect size is here; docs/RECOVERY_ACTIONS.md documents
 * each one with its source or "assumed". */
typedef struct
{
    /* REROUTE */
    uint16_t reroute_cost_penalty;      /* extra path cost on a faulted/blocked cell, for the rerouting team only; a detour is taken only when it is nearly free */
    uint8_t  env_crossing_delay_ticks;  /* sensitivity override (default 0 = Phase 2 model): ticks a team stalls before stepping onto a faulted cell */
    uint8_t  reroute_lookahead;         /* route steps scanned for faulted cells */
    uint16_t reroute_max_ticks;         /* a reroute never lasts longer than this */
    /* CLEAR_ROAD */
    uint16_t clear_road_persist_ticks;  /* N: a cell faulted this long gets a clearing task */
    uint8_t  clear_road_teams_threshold;/* ... or as soon as this many teams have it on their route */
    uint8_t  clear_road_max_tasks;      /* concurrent clearing tasks */
    uint16_t clear_road_work;           /* work units to clear a cell (a team does ~8-12 per tick) */
    /* REINFORCE_INCIDENT */
    uint16_t reinforce_event_window;    /* ticks after a death/escalation that count as "deteriorating" */
    uint16_t reinforce_hold_ticks;      /* how long a reinforcement request stands */
    float    reinforce_work_boost;      /* work-rate multiplier while a support team is on scene */
    float    reinforce_survival_factor; /* victim death-hazard multiplier while a support team is on scene */
    /* RELAY_REPOSITION */
    uint8_t  relay_min_fault_ticks;     /* only a comms fault this old triggers a reposition (1-2 tick blips are ignored) */
    uint16_t relay_cooldown_ticks;      /* after a reposition ends, no new one for this team for this long */
    uint8_t  relay_search_radius;       /* cells searched for a positive-margin cell */
    uint16_t relay_max_ticks;           /* a reposition never lasts longer than this */
} CausalActionParams_t;

/* Consequential roads (Step 3.1): under the causal profile an environment
 * fault damages a road cell for real. Partial debris makes a team stall
 * before stepping onto the cell; full blockage makes it impassable. See
 * docs/CONSEQUENTIAL_ROADS.md for the grounding ("to verify" / "assumed"
 * marked per value). --causal-roads=off restores the Step 3 behaviour. */
typedef struct
{
    uint8_t  enabled;               /* default on under the causal profile */
    uint8_t  partial_pct;           /* share of damage that is partial debris; the rest blocks the road */
    uint8_t  crossing_delay_ticks;  /* extra ticks to step onto a partially blocked cell */
    uint16_t persist_min_ticks;     /* unattended damage lasts this long at least ... */
    uint16_t persist_max_ticks;     /* ... and at most this long (uniform) */
    uint16_t clear_work_partial;    /* CLEAR_ROAD work units, partial debris */
    uint16_t clear_work_full;       /* CLEAR_ROAD work units, full blockage */
    uint8_t  cost_per_tick;         /* A* cost units that one stall tick is worth */
    uint8_t  route_cost_visible;    /* 1: the stall cost is in the cell's travel_cost (A* and the map frames see it); 0: A* sees only the fault's own +severity */
} CausalRoadParams_t;

extern CausalRoadParams_t g_causal_roads;

/* Step 3.2 plumbing fixes (causal profile only; docs/CAUSAL_PLUMBING_FIXES.md).
 * --causal-fixes=off restores the Step 3.1 behaviour exactly. */
#define CAUSAL_FIX_LABEL        0x01u  /* environment label = "this team's route touches a damaged cell" */
#define CAUSAL_FIX_ENV_NO_RTB   0x02u  /* an environment detection never drives a team-level reaction (RETURN_TO_SAFE_ZONE, ...) */
#define CAUSAL_FIX_TRUE_COST    0x04u  /* the progress tracker's remaining cost ignores the REROUTE overlay */
#define CAUSAL_FIX_STALL        0x08u  /* evaluators are stall-aware: a team held at a partial-debris cell is not evidence of a mobility / progress fault */
#define CAUSAL_FIXES_ALL        0x0Fu
extern uint8_t g_causal_fixes;
void fault_config_set_causal_fixes(uint8_t mask);
/* 1 under the causal profile when this fix is enabled. CAUSAL_FIX_LABEL and CAUSAL_FIX_STALL also need consequential roads
 * (the label is the route-touch signal; the stalls are the roads' debris crossings). */
int fault_profile_causal_fix(unsigned fix);

/* Step 3.5 cost-based recovery planner (docs/RECOVERY_PLANNER_PROPOSAL.md).
 * --recovery=utility, causal profile only; the fixed gates stay the default. */
typedef enum
{
    RECOVERY_MODE_FIXED = 0,
    RECOVERY_MODE_UTILITY
} RecoveryMode_t;

#define PLANNER_LAYER_TEAM     0x01u  /* Layer A: stay / go home per team-fault */
#define PLANNER_LAYER_TASKS    0x02u  /* Layer B: REROUTE / CLEAR_ROAD / REINFORCE / RELAY decided by cost */
#define PLANNER_LAYER_PREEMPT  0x04u  /* pull a busy team onto a system task */
#define PLANNER_LAYERS_ALL     0x07u

#define PLANNER_P_AI        0u  /* AI + watchdog + consensus */
#define PLANNER_P_WATCHDOG  1u  /* watchdog + consensus only (the AI's scores are ignored) */
#define PLANNER_P_ORACLE    2u  /* ground-truth fault flags: laptop evaluation upper bound, never on a board */

typedef struct
{
    uint8_t layers;           /* PLANNER_LAYER_* mask */
    uint8_t p_source;         /* PLANNER_P_* */
    uint8_t approval;         /* 0 deny (default), 1 auto-approve (laptop upper bound) */
    uint8_t log_enabled;      /* laptop decision log (--planner-log) */
    uint8_t calib_dump;       /* laptop calibration histograms (--planner-calib) */
    uint8_t preempt_k;        /* nearest busy teams considered per task */
    uint16_t preempt_cooldown_ticks;
    float v_mult;             /* x the derived exchange rate V (D1 sweep: 0.5, 1, 2) */
    float margin_frac;        /* rho: relative switch margin (tuned on dev seeds) */
    /* Constants measured from the baseline (docs/SPECIALIZATION_LOAD.md) or derived from
     * existing constants; every one is overridable for the +-50% sensitivity runs. */
    float n_ref_victims;      /* victims per victim-bearing incident: 171.9 victims / 33.9 incidents per seed = 5.07 */
    float age_ref_ticks;      /* typical age at first response: 226 spawn-to-discovery + 40 queue wait = 266 */
    float reassign_delay;     /* ticks a released task waits for another team: measured mean wait 40.1-44.1 */
    float ref_travel_ticks;   /* mean route length of a task leg: 10.1 cells (reroute probe) */
    float ref_stage_ticks;    /* mean ticks of one stage: ref_travel + stage work / mean work rate */
    float rate_window_ticks;  /* horizon of the crossing-rate average: one hour of ticks */
    float blocked_detour;     /* extra steps a team pays to go round one fully blocked cell (assumed 2) */
    float cblind;             /* IT per tick a team with a comm fault is invisible to consensus (assumed) */
    float repair_mobility;    /* go-home repair effectiveness: cause-cleared by rest (CAUSAL_MOBILITY_REPAIR_REST_TICKS) */
    float repair_resource;    /* resupply at the safe zone (CAUSAL_RESOURCE_RESUPPLY_LEVEL) */
    float repair_comm;        /* assumed: the safe zone's link margin is better than the field */
    float repair_scale;       /* sensitivity multiplier on the three repair values */
    float crossing_scale;     /* sensitivity multiplier on the crossing delay */
    float p_log_floor;        /* decisions with a fault probability at or above this are logged */
} PlannerParams_t;

extern PlannerParams_t g_planner;
extern RecoveryMode_t g_recovery_mode;
/* 1 under the causal profile with --recovery=utility. */
int fault_profile_planner_on(void);
/* 1 when additionally this layer is enabled. */
int fault_profile_planner_layer(unsigned layer);
/* 1 only under the causal profile with consequential roads enabled. */
int fault_profile_causal_roads_on(void);

extern CausalActionParams_t g_causal_actions;
extern uint8_t g_causal_actions_enabled;     /* bit mask, one bit per CausalAction_t; --causal-actions=on|off|<list>, default off */
void fault_config_set_causal_actions(uint8_t mask);
#define CAUSAL_ACTIONS_ALL 0x0Fu
/* --causal-env-duration=<min>-<max>: sensitivity override of the causal
 * environment-fault backstop (ticks, 1-255). 0/0 = the profile default. */
void fault_config_set_causal_env_duration(uint8_t min_ticks, uint8_t max_ticks);
/* --causal-env-crossing-delay=<ticks> (sensitivity override, causal only). */
void fault_config_set_causal_env_crossing_delay(uint8_t ticks);
/* --causal-reroute-penalty=<cost units> (sensitivity, causal only). */
void fault_config_set_causal_reroute_penalty(uint16_t penalty);
/* 1 only under the causal profile with at least one action enabled. */
int fault_profile_causal_actions_on(void);
/* 1 under the causal profile when this particular action (CausalAction_t) is enabled. */
int fault_profile_causal_action_enabled(unsigned action);

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
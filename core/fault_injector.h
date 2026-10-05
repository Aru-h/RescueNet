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
    uint32_t ticks_by_cause[NUM_OPERATIONAL_FAULT_CLASSES];     /* ... of episodes ended by the cause being removed */
    uint32_t planned_by_cause[NUM_OPERATIONAL_FAULT_CLASSES];   /* ... and what their backstop timers had drawn (recovery_tick - start_tick) */
} CausalFaultStats_t;

const CausalFaultStats_t *fault_injector_causal_stats(void);

/* Why a causal fault started / ended (ground truth, never a model input). */
typedef enum
{
    CAUSAL_ONSET_NONE = 0,
    CAUSAL_ONSET_BASELINE,        /* random baseline hazard */
    CAUSAL_ONSET_WEAR,            /* accumulated wear term dominated */
    CAUSAL_ONSET_ROUGHNESS,       /* roughness of the cell being crossed */
    CAUSAL_ONSET_BATTERY,         /* equipment depletion */
    CAUSAL_ONSET_SINCE_REST,      /* time since last rest */
    CAUSAL_ONSET_LINK_MARGIN,     /* margin < 0 */
    CAUSAL_ONSET_AFTERSHOCK,      /* Omori aftershock on the route */
    CAUSAL_ONSET_DETERIORATION    /* victim loss / escalation on the assigned incident */
} CausalOnsetCause_t;

typedef enum
{
    CAUSAL_CLEAR_NONE = 0,
    CAUSAL_CLEAR_CAUSE_REMOVED,
    CAUSAL_CLEAR_BACKSTOP
} CausalClearCause_t;

/* Per-team ground truth for the tick the injector just stepped. Written to
 * the causal-only sidecar CSV by laptop/snapshot.c, never to the main
 * dataset. */
typedef struct
{
    float hidden_wear;
    float link_margin_db;
    float hazard_mobility;
    float hazard_resource;
    uint32_t ticks_since_rest;
    uint8_t onset_cause[NUM_OPERATIONAL_FAULT_CLASSES];  /* CausalOnsetCause_t, this tick */
    uint8_t clear_cause[NUM_OPERATIONAL_FAULT_CLASSES];  /* CausalClearCause_t, this tick */
    uint8_t aftershock_hit;
    /* Bit 0: the team's route (next 8 steps) crosses a partially blocked cell;
     * bit 1: it passes within one cell of a fully blocked cell. Sidecar only. */
    uint8_t route_touch;
} CausalTeamTruth_t;

const CausalTeamTruth_t *fault_injector_causal_truth(uint8_t team_id);

/* Causal profile only: 1 when accumulated wear makes the team skip this
 * tick's step (a noisy, visible symptom of hidden wear). */
int fault_injector_causal_symptom_skip(uint8_t team_id);

/* Laptop-only inspection of the causal link model (used by main.c's
 * --link-margin-map): margin and the shadowing component alone, in dB, at
 * the current tick. */
float fault_injector_causal_link_margin_at(int x, int y);
float fault_injector_causal_shadow_at(int x, int y);
/* Causal REROUTE/CLEAR_ROAD: ends every environment fault on cell (x, y) and
 * restores the cell. Returns the number of faults ended. Laptop only. */
int fault_injector_clear_cell_faults(int x, int y);

/* Consequential roads (Step 3.1), laptop only. */
typedef struct
{
    uint32_t partial_injected;      /* partial-debris damages created */
    uint32_t blocked_injected;      /* full blockages created */
    uint32_t block_downgraded;      /* blockages refused by the connectivity guard (made partial) */
    uint32_t audit_ticks;           /* ticks the connectivity audit ran */
    uint32_t audit_violations;      /* cells cut off from every safe zone by road blocks: must stay 0 */
    uint32_t max_blocked_cells;     /* most fully blocked cells at once */
    uint32_t label_ticks;           /* team-ticks the environment label (route touches damage) was on */
    uint32_t label_episodes;        /* distinct label-on episodes; mean length = label_ticks / label_episodes */
} CausalRoadStats_t;

const CausalRoadStats_t *fault_injector_causal_road_stats(void);
/* The environment-fault label: under the causal profile with the label fix
 * (and roads on) 1 while the team's route touches a damaged cell; otherwise
 * the team's per-team environment fault flag, unchanged. */
uint8_t fault_injector_causal_env_label(uint8_t team_id);
/* 0 none, 1 partial debris, 2 fully blocked. */
uint8_t fault_injector_causal_road_kind(int x, int y);

/* Causal profile mission model, for result reporting. */
typedef struct
{
    uint32_t victims_lost;
    uint32_t victims_seen;      /* initial victim count of every incident that entered the model */
    uint32_t escalations;
    uint32_t aftershock_events;
    uint32_t aftershock_team_hits;
} CausalWorldStats_t;

const CausalWorldStats_t *fault_injector_causal_world_stats(void);

#endif
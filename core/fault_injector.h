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
    CAUSAL_CLEAR_CAUSE_REMOVED,   /* base repair: the team rested at a safe zone (mobility, resource) or the link recovered */
    CAUSAL_CLEAR_BACKSTOP,        /* the drawn timer ran out; under persistence this is the natural-recovery tail */
    CAUSAL_CLEAR_FIELD_REPAIR     /* a field repair succeeded (Step 3.6) */
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
    /* Step 3.6, sidecar only. */
    uint8_t field_state;          /* 0 none, 1 mobility repair, 2 resource repair, 3 maintenance, held this tick */
    uint8_t escalated[NUM_OPERATIONAL_FAULT_CLASSES]; /* the active fault of this class has escalated */
    float risk_mobility;          /* hazard-oracle P(onset within the forecast horizon), mobility */
    float risk_resource;          /* ... resource */
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

/* ------------------------------------------------------------------
 * Step 3.6 persistent faults (docs/PERSISTENT_FAULTS_PROPOSAL.md). Causal
 * profile only; every function below returns 0 / does nothing otherwise
 * and on a board.
 * ------------------------------------------------------------------ */
typedef struct
{
    uint32_t escalations[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t field_started[3];       /* 0 mobility repair, 1 resource repair, 2 maintenance (PREVENT_FIELD) */
    uint32_t field_ok[3];
    uint32_t field_failed[3];        /* repair attempts that failed (the fault stays) */
    uint32_t field_cancelled[3];     /* the team left or the fault ended first */
    uint32_t field_ticks;            /* team-ticks held for a field repair or maintenance */
    uint32_t ticks_by_field[NUM_OPERATIONAL_FAULT_CLASSES]; /* episode lengths of faults ended by a field repair */
    uint32_t prevent_orders;         /* PREVENT (rest at base) orders given */
    uint32_t prevent_done;           /* ... that completed their rest */
    /* Step 3.7 hold-repair (statistics only). Cancel cause per repair kind: 0 stamina trigger, 1 other-class trigger,
     * 2 planner choice above the floor, 3 task released or resolved (team not sent home), 4 the fault ended by
     * another route, 5 other / unattributed. */
    uint32_t cancel_cause[3][6];
    uint32_t hold_capped[3];         /* rule-based recommendation capped at LOCAL_RECOVERY, by original action: RETURN, REALLOC, ABORT */
    uint32_t hold_lifted[2];         /* ticks the cap was lifted: 0 stamina trigger, 1 other-class trigger */
    uint32_t hold_h2_refused;        /* repair attempts refused (one attempt per fault episode) */
    uint32_t hold_h3_resets;         /* recovery state reset after a successful repair */
} CausalPersistStats_t;

const CausalPersistStats_t *fault_injector_causal_persist_stats(void);

/* Start a field activity on a working team: kind 1 = repair its active mobility fault,
 * 2 = repair its active resource fault, 3 = routine maintenance (wear only). Returns 1 when started.
 * A team is held (no movement, no work) until it completes; leaving the task cancels it. */
int fault_injector_causal_field_start(uint8_t team_id, uint8_t kind);
uint8_t fault_injector_causal_field_state(uint8_t team_id);   /* 0 when not held */
int fault_injector_causal_team_held(uint8_t team_id);          /* 1 while a field activity holds the team this tick */
/* Step 3.7 (hold-repair). The recovery manager reports, each tick a repair runs, what would end it if it ended now
 * (0 nothing, 1 stamina trigger, 2 other-class trigger, 3 planner choice above the floor); used only to attribute a
 * cancellation. hold_count: 0 capped[idx], 1 lifted[idx], 2 H3 reset. just_repaired: 1 on the tick a repair succeeded. */
void fault_injector_causal_hold_note(uint8_t team_id, uint8_t cause);
void fault_injector_causal_hold_count(uint8_t counter, uint8_t idx);
int fault_injector_causal_field_just_repaired(uint8_t team_id);
/* PREVENT: the planner has sent the team home to rest before a fault; it stays RESTING until the repair dwell
 * has removed wear (the existing 4-tick routine maintenance). */
void fault_injector_causal_prevent_order(uint8_t team_id);
int fault_injector_causal_rest_hold(uint8_t team_id);
/* Hazard-oracle forecast: P(onset within the horizon) for mobility [0] and resource [1], from the hidden state.
 * after: 0 as is, 1 after a rest at a safe zone, 2 after in-field maintenance. Laptop only. */
int fault_injector_causal_road_lane(int x, int y);   /* Step 5 RR3 */
int fault_injector_causal_onset_within(uint8_t team_id, uint32_t ticks);   /* Step 5 RR1 */
uint32_t fault_injector_causal_field_remaining(uint8_t team_id);   /* Step 5 RR2a */
void fault_injector_causal_road_base(int x, int y, uint8_t *cost, uint8_t *navigable);   /* Step 5 probe */
float fault_injector_causal_future_victim_loss(uint8_t slot, uint32_t window);   /* Step 5 probe */
void fault_injector_causal_forecast(uint8_t team_id, uint8_t after, float out[2]);

#endif

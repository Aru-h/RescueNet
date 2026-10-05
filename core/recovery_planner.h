#ifndef RECOVERY_PLANNER_H
#define RECOVERY_PLANNER_H

#include <stdint.h>

#include "mission_state.h"
#include "recovery_manager.h"

/*
 * Cost-based recovery planner (Step 3.5, --recovery=utility, causal profile
 * only). Design, cost derivations and decisions: docs/RECOVERY_PLANNER_PROPOSAL.md.
 *
 * Currency: incident-ticks (IT), one tick of delay on one open incident,
 * extended with victims through the survival model:
 *     u_i = 1 + V * n_i * h(age_i)
 * Layer A (planner_team_decide): per team, STAY or GO HOME, by
 *     E(a) = sum_f P_f * C_fault(a, f) + C_action(a)
 * Layer B (planner_*_wanted): REROUTE / CLEAR_ROAD / REINFORCE / RELAY started
 * when benefit > cost * (1 + margin). Preemption (planner_preempt_step): a busy
 * team is pulled onto a system task when benefit > switch cost + margin.
 *
 * Fixed-size static state only: no heap, no recursion, bounded loops. Layer A
 * and the stats compile for the board; Layer B and preemption need the world
 * (ENVIRONMENT_HAS_WORLD_STORAGE) and are stubs elsewhere.
 */

/* What the planner is told about one team this tick. */
typedef struct
{
    float ai_score[3];     /* raw AI scores: mobility, communication, resource (0 with no model) */
    float wd_conf;         /* watchdog anomaly confidence */
    uint8_t wd_class;      /* class the watchdog's dominant failure mode points at (0..2), 255 none */
    uint8_t wd_flag;       /* watchdog anomaly flag */
    float cons_y[2];       /* consensus severity: mobility, communication */
    uint8_t cons_elevated; /* peer consensus at its ELEVATED tier, any class */
    uint8_t streak_ok;     /* the AI's 8-tick persistence streak tripped */
    uint8_t rule_floor;    /* RecoveryAction_t the rule-based evaluators alone recommend (their own hysteresis applied) */
    uint8_t rule_reason;   /* RecoveryReason_t of that recommendation */
} PlannerEvidence_t;

/* One decision-log record (laptop-only sink). */
typedef struct
{
    uint32_t tick;
    uint8_t team;
    uint8_t layer;         /* 0 = A (team), 1 = B (task/map action), 2 = preemption */
    uint8_t chosen;        /* Layer A: RecoveryAction_t; Layer B: CausalAction_t */
    uint8_t alt;           /* top alternative (same encoding), 255 none */
    float p[3];            /* fused fault probability per class */
    float p_ai[3];         /* calibrated AI part */
    float cost_fault;      /* P * C_fault of the chosen action (Layer A) or benefit (Layer B) */
    float cost_action;     /* C_action of the chosen action (Layer A) or cost (Layer B) */
    float cost_alt;        /* total expected cost of the alternative */
    float margin;
    uint8_t flags;         /* PLANNER_FLAG_* */
    int16_t target_x, target_y;
    int16_t aux;           /* preempted task slot, or -1 */
    float aux_cost;        /* switch cost of a preemption */
} PlannerDecision_t;

#define PLANNER_FLAG_FORCED_FLOOR   0x01u  /* the rule-based floor forced the action */
#define PLANNER_FLAG_FORCED_WD      0x02u  /* watchdog flag was set */
#define PLANNER_FLAG_FORCED_CONS    0x04u  /* consensus ELEVATED was set */
#define PLANNER_FLAG_AI_CAP         0x08u  /* AI-only evidence capped the choice */
#define PLANNER_FLAG_APPROVAL_REQ   0x10u  /* an approval was requested */
#define PLANNER_FLAG_APPROVAL_OK    0x20u  /* ... and granted */

typedef struct
{
    uint32_t decisions;
    uint32_t go_home_planner;       /* go-home chosen by cost */
    uint32_t go_home_floor;         /* go-home forced by the rule-based floor */
    uint32_t stay_with_p;           /* a fault probability >= p_log_floor and the planner stayed */
    uint32_t ai_cap_blocks;
    uint32_t approvals_requested, approvals_granted, approvals_denied;
    uint32_t abort_requests;        /* ABORT is dominated by RETURN; counted if it was ever the cheapest */
    uint32_t wanted[4];             /* Layer B: starts allowed, indexed by CausalAction_t */
    uint32_t refused[4];            /* Layer B: candidates refused on cost */
    uint32_t preempt_considered, preempt_done[4], preempt_blocked_cost, preempt_blocked_approval, preempt_blocked_finish;
    uint32_t preempt_net_sum_x100;  /* sum of (benefit - switch cost) of executed preemptions, x100 */
    uint32_t preempt_succeeded[4];  /* preempted job finished (indexed by CausalAction_t) */
    uint32_t preempt_failed[4];     /* preempted team left the job before it finished */
    uint32_t go_home_events;        /* recommendation changes into RETURN_TO_SAFE_ZONE / ABORT_MISSION for a working team (any mode) */
    uint32_t go_home_near_stall;    /* ... within 12 ticks after the team was held at a debris cell */
    uint32_t action_changes;        /* recommendation changes per team (chattering) */
    uint32_t reversals10;           /* a change that undoes the previous change within 10 ticks */
} PlannerStats_t;

typedef struct
{
    uint32_t n[3][3][32];           /* [source 0 AI, 1 WD, 2 CONS][class][raw-score bin] */
    uint32_t pos[3][3][32];
    uint32_t cal_n[3][3][32];       /* by calibrated bin */
    uint32_t cal_pos[3][3][32];
    float cal_sum[3][3][32];
    uint32_t fused_n[3][32];        /* fused P, by bin */
    uint32_t fused_pos[3][32];
    float fused_sum[3][32];
} PlannerCalib_t;

void planner_reset(void);

/* Layer A. Returns 1 and fills *out when the planner owns this team's status this tick. */
int planner_team_decide(const TeamState_t *team, const PlannerEvidence_t *ev, RecoveryStatus_t *out);

/* Chattering bookkeeping; harmless in every mode. */
void planner_note_recommendation(uint8_t team_id, uint8_t action);

/* Layer B. Each returns 1 when the action should start now. */
int planner_reroute_wanted(const TeamState_t *team);
int planner_clear_road_wanted(int x, int y, uint8_t kind, float touch_rate, uint32_t faulted_age_ticks);
int planner_reinforce_wanted(uint8_t incident_slot);
int planner_relay_wanted(const TeamState_t *team);

/* Preemption of busy teams onto open system tasks (called once per tick before allocation). */
void planner_preempt_step(void);

const PlannerStats_t *planner_stats(void);
const PlannerCalib_t *planner_calib(void);
const float *planner_last_p(uint8_t team_id);   /* fused P of the last Layer A step */

/* Decision log sink (laptop). */
void planner_set_log_sink(void (*sink)(const PlannerDecision_t *));

#endif

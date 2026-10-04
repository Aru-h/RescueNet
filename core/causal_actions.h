#ifndef CAUSAL_ACTIONS_H
#define CAUSAL_ACTIONS_H

#include <stdint.h>

#include "fault_config.h"
#include "mission_state.h"

/*
 * Recovery actions that exist only under --fault-profile=causal (Step 3).
 * They are deliberately NOT RecoveryAction_t values: that enum is written to
 * the dataset CSV (recovery_recommendation) and compared numerically by
 * tools, so it stays exactly as it was. See docs/RECOVERY_ACTIONS.md.
 *
 * Everything here is a no-op under every other profile, and on a board
 * (the bodies need the laptop-only world grid).
 */
typedef enum
{
    CAUSAL_ACTION_REROUTE = 0,
    CAUSAL_ACTION_CLEAR_ROAD,
    CAUSAL_ACTION_REINFORCE_INCIDENT,
    CAUSAL_ACTION_RELAY_REPOSITION,

    NUM_CAUSAL_ACTIONS
} CausalAction_t;

typedef struct
{
    uint32_t dispatched[NUM_CAUSAL_ACTIONS];     /* started */
    uint32_t resolved[NUM_CAUSAL_ACTIONS];       /* ended because the problem was removed */
    uint32_t abandoned[NUM_CAUSAL_ACTIONS];      /* ended by timeout/cancellation */
    uint32_t active_ticks[NUM_CAUSAL_ACTIONS];   /* summed over teams/tasks */
    uint32_t from_detector[NUM_CAUSAL_ACTIONS];  /* requested by the AI/oracle class flag, not by a rule */
} CausalActionStats_t;

void causal_actions_reset(void);
/* Once per tick, after the fault injector and before allocation. */
void causal_actions_step(void);

/* The AI/oracle flagged an environment or mission fault on this team
 * (core/recovery_manager.c). Only called when the actions are on. */
void causal_actions_detector_flag(uint8_t team_id, OperationalFaultClass_t fault_class);

/* move_team() hooks. */
int causal_actions_move_override(TeamState_t *team);   /* 1 = the team's step was handled here */
void causal_actions_path_begin(const TeamState_t *team);
void causal_actions_path_end(void);

/* Sensitivity override (--causal-env-crossing-delay): 1 = this step onto a
 * faulted cell must wait. Independent of which actions are enabled. */
int causal_actions_crossing_stall(const TeamState_t *team, int next_x, int next_y);
uint32_t causal_actions_stall_ticks(void);

/* Incident hooks. */
int causal_actions_reinforce_wanted(uint8_t incident_slot);
uint8_t causal_actions_boost_work(const IncidentState_t *incident, uint8_t work_rate);
float causal_actions_survival_factor(uint8_t incident_slot);
void causal_actions_road_cleared(int16_t x, int16_t y);

/* Bit i set = action i currently involves this team. */
uint8_t causal_actions_team_mask(uint8_t team_id);

const CausalActionStats_t *causal_actions_stats(void);

#endif

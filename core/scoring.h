#ifndef SCORING_H
#define SCORING_H

#include "mission_state.h"

typedef struct
{
	float capability;
	float travel;
	float priority;
	float stamina;
	float equipment;
} ScoringWeights_t;

void scoring_set_weights(ScoringWeights_t weights);
ScoringWeights_t scoring_get_weights(void);
void scoring_reset_weights(void);

float calculate_score(const TeamState_t *team, const IncidentState_t *incident);
float distance_score(const TeamState_t *team, const IncidentState_t *incident);
float capability_score(const TeamState_t *team, const IncidentState_t *incident);
float calculate_route_utility(const TeamState_t *team,const TaskRef_t *path,uint8_t path_size);
float calculate_marginal_bundle_bid(uint8_t team_id,const CBBAAgentState_t *agent,const IncidentState_t *incident,uint8_t *best_insert_position);
float priority_score( const IncidentState_t *incident);
float stamina_score(const TeamState_t *team);
int team_has_minimum_equipment(const TeamState_t *team,const IncidentState_t *incident);
int validate_equipment_requirement_matrix(void);
int candidate_claim_is_better(uint8_t candidate_team,float candidate_bid,uint8_t current_team,float current_bid,const IncidentState_t *incident);
#endif

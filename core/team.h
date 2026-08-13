#ifndef TEAM_H
#define TEAM_H

#include "mission_state.h"

void generate_teams(uint32_t seed);
void update_team(TeamState_t *team);
void update_all_teams(void);
void move_team(TeamState_t *team);
void move_all_teams(void);


#endif
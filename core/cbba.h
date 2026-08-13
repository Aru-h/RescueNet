#ifndef CBBA_H
#define CBBA_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "mission_state.h"
#include "allocator.h"
#include "scoring.h"
#include "team.h" 

void cbba_build_bundle(uint8_t team_id);
void cbba_run(void);
void cbba_improve_routes(void);
void cbba_sync_mission_execution_twin(void);
uint32_t cbba_get_two_opt_improvements(void);
uint32_t cbba_get_relocate_improvements(void);
void cbba_reset_improvement_counters(void);

#endif
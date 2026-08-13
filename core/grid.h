#ifndef GRID_H
#define GRID_H

#include "mission_state.h"

void initialize_grid(void);
void apply_incident_grid_degradation(const IncidentState_t *incident);

#endif

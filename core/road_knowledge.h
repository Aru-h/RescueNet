#ifndef ROAD_KNOWLEDGE_H
#define ROAD_KNOWLEDGE_H

#include <stdint.h>

#include "mission_state.h"

/*
 * Step 5 road-information probes R1-R3 (docs/HEADROOM_MAP_PREREG.md). Opt-in (--probe-roadknow), causal profile
 * only. By default (mode 0) the pathfinder sees the true map, exactly as before: every function here returns at
 * once and the simulation is unchanged.
 *
 * With mode 1 or 2 the pathfinder's planning view of the causal road damage (only those cells) is a fleet-wide
 * belief: a damaged cell is planned at its undamaged cost until it has been observed (a team within `radius` cells of
 * it, or stopped at it) and the report has reached the fleet (`latency` ticks, lost with probability `loss`, always
 * lost from a team with an active communication fault). One shared picture, not a per-team map (the pathfinder has
 * no team argument). Movement and the world stay true: a team whose planned step enters a truly blocked cell stops.
 */
typedef struct
{
    uint8_t mode;       /* 0 omniscient (default), 1 observation only (R1), 2 observation plus a rate prior (R2) */
    uint8_t radius;     /* Chebyshev observation radius in cells (R1: 1; strict R1s: 0) */
    uint8_t latency;    /* ticks from observation to the fleet picture */
    uint8_t instant;    /* R3: zero latency, no loss, communication-faulted teams report too */
    float loss;         /* report loss probability */
} RoadKnowledgeCfg_t;

typedef struct
{
    uint32_t damage_episodes;       /* cells that became damaged */
    uint32_t known_episodes;        /* ... of which the fleet picture came to show the damage while it lasted */
    uint32_t known_within_2;        /* ... within 2 ticks of onset */
    uint64_t known_delay_sum;       /* ticks from onset to fleet-known, over known_episodes */
    uint32_t known_delay_max;
    uint32_t reports_made, reports_lost;
    uint32_t unknown_partial_enter; /* a team stepped onto partial debris the fleet did not know about */
    uint32_t blocked_stops;         /* team-ticks a team was stopped at a truly blocked cell it had planned through */
    uint32_t stale_cell_ticks;      /* cell-ticks the picture showed damage on an already clear cell */
    uint32_t prior_add_max;         /* R2: largest per-cell cost the prior added */
} RoadKnowledgeStats_t;

extern RoadKnowledgeCfg_t g_road_know;

void road_knowledge_reset(void);
void road_knowledge_step(void);                                   /* once per tick, after the fault injector */
int road_knowledge_planning_view(int x, int y, int *navigable, int *cost);   /* 1 = override given */
int road_knowledge_step_blocked(const TeamState_t *team, int next_x, int next_y);   /* 1 = stop the team */
int road_knowledge_believed_damaged(int x, int y);
const RoadKnowledgeStats_t *road_knowledge_stats(void);

#endif

#include "mission_state.h"

TeamState_t g_teams[NUM_TEAMS];
IncidentState_t g_incidents[NUM_INCIDENTS];
IncidentState_t g_system_tasks[NUM_SYSTEM_TASKS];
SafeZone_t g_safezones[MAX_SAFE_ZONES];
uint8_t g_num_safe_zones = 0;
GridCell_t g_grid[GRID_WIDTH][GRID_HEIGHT];
EvacuationGateway_t g_evacuation_gateways[NUM_EVACUATION_GATEWAYS];
uint8_t g_safezone_evacuation_active[MAX_SAFE_ZONES];
uint32_t g_current_tick = 0;
/* Tick number of the step currently being processed. Equals g_current_tick
 * once simulation_step() has incremented it, but is ALSO correct during the
 * pre-increment fault_injector_step() / allocate_all_incidents() phase of
 * sim_tick() (where g_current_tick still holds the previous step's value).
 * Event logging uses this so a snapshot row's cycle always matches the step
 * the event belongs to. Maintained by sim_tick() and simulation_step(). */
uint32_t g_sim_step_tick = 0;
uint32_t g_world_seed = 1;
CBBAAgentState_t g_cbba_agent_state[NUM_TEAMS];
MissionExecutionTwin_t g_mission_execution_twin[NUM_TEAMS];

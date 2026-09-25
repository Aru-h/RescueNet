#include "sim_runner.h"

#include "scoring.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "simulation.h"
#include "allocator.h"
#include "snapshot.h"
#include "mission_state.h"

int sim_runner_init(void)
{
    if(!validate_equipment_requirement_matrix())
        return 1;

    scoring_reset_weights();
    fault_config_apply_profile(FAULT_PROFILE_NONE);
    g_simulation_trace_enabled = 0;
    g_sim_role = SIM_ROLE_STANDALONE;

    return 0;
}

/* World-authority variant — see sim_runner.h's doc comment on this
 * function before editing. Callers: laptop/main.c,
 * laptop/tools/override_selftest.c. */
void sim_start_seed(uint32_t seed, FaultProfile_t fault_profile)
{
    fault_config_apply_profile(fault_profile);
    fault_injector_set_profile(fault_profile);

    generate_world(seed);
    snapshot_begin_run(seed);
}

/* Board variant — deliberately does NOT call generate_world(). See
 * sim_runner.h's doc comment on this function (read it before
 * editing this one, or sim_start_seed() above) for why the split
 * exists and the maintenance warning about keeping the two in sync
 * on everything except the generate_world() call. Only caller:
 * board/STM32N6570-DK/main_board.c. */
void sim_start_seed_board(uint32_t seed, FaultProfile_t fault_profile)
{
    fault_config_apply_profile(fault_profile);
    fault_injector_set_profile(fault_profile);

    /* NO generate_world(seed) HERE — this is the entire point of
     * this function existing separately from sim_start_seed(). The
     * board is not a world authority; it waits for
     * MissionLinkRoster_t/MissionLinkIncident_t/MissionLinkFault_t
     * over the mission link instead (core/mission_link.h). If you
     * are adding a call to generate_world() (or anything that
     * transitively calls it, e.g. via a helper) on this path, stop
     * — that almost certainly belongs in sim_start_seed() for a
     * genuine world-authority caller, not here. */

    /* Same rule for the tick itself: a board executes its teams but
     * runs no world events - see SimRole_t (simulation.h). */
    g_sim_role = SIM_ROLE_BOARD;

    snapshot_begin_run(seed);
}

void sim_tick(void)
{
    /* fault_injector_step() and allocate_all_incidents() run before
     * simulation_step() increments g_current_tick, so events they log
     * belong to the step about to be processed - g_current_tick + 1.
     * simulation_step() re-syncs this to g_current_tick after its ++. */
    g_sim_step_tick = g_current_tick + 1u;
    if(g_sim_role != SIM_ROLE_BOARD)
        fault_injector_step();  /* faults reach a board as MissionLinkFault_t */
    allocate_all_incidents();
    simulation_step();
}

void sim_end_seed(void)
{
    snapshot_end_run();
}

void sim_runner_shutdown(void)
{
    snapshot_close_dataset();
}

#include "sim_runner.h"

#include "scoring.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "simulation.h"
#include "allocator.h"
#include "snapshot.h"

int sim_runner_init(void)
{
    if(!validate_equipment_requirement_matrix())
        return 1;

    scoring_reset_weights();
    fault_config_apply_profile(FAULT_PROFILE_NONE);
    g_simulation_trace_enabled = 0;

    return 0;
}

void sim_start_seed(uint32_t seed, FaultProfile_t fault_profile)
{
    fault_config_apply_profile(fault_profile);
    fault_injector_set_profile(fault_profile);

    generate_world(seed);
    snapshot_begin_run(seed);
}

void sim_tick(void)
{
    fault_injector_step();
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

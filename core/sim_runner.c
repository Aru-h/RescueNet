#include "headroom_probes.h"
#include "road_knowledge.h"
#include "sim_runner.h"

#include "scoring.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "causal_actions.h"
#include "simulation.h"
#include "allocator.h"
#include "snapshot.h"
#include "mission_state.h"
#include "watchdog.h"
#include "mission_execution_twin.h"
#include "cbba.h"
#include "recovery_manager.h"
#include "fault_comms.h"
#include "search_zones.h"
#include "consensus.h"
#include "fault_predictor_features.h"

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
 * on everything except world generation. Only callers:
 * board/STM32N6570-DK/main_board.c and board/Raspberry_Pi_Pico/
 * main_board.c. */
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

    /* The per-run module resets generate_world() does on the laptop,
     * minus everything that builds or wipes world state. Without them
     * a board ran on zero-initialised statics that are NOT the
     * intended defaults (watchdog last_remaining_cost 0 instead of
     * UINT16_MAX and last_assigned_task_slot 0 instead of
     * NO_ASSIGNMENT, fault_buffer's per-board last-heard tick 0
     * instead of FAULT_BUFFER_BOARD_NEVER_HEARD, ...).
     *
     * By the time this runs the board has already been handed its
     * incidents, roster, route map, sites and clock over the mission
     * link (main_board.c waits for roster/map/sites before the seed
     * loop, and applies incidents during that wait), so every call
     * here was checked to touch only its own module's state:
     *   watchdog_init()                  g_watchdog_state[] only
     *   mission_execution_twin_reset()   g_mission_execution_twin[] and
     *                                    its comm-gap window; READS
     *                                    g_teams[] loc/stamina/status
     *                                    (which is why it must run
     *                                    after the roster arrives)
     *   cbba_reset_improvement_counters() two diagnostic counters
     *   recovery_manager_init()          recovery state/metrics; READS
     *                                    g_teams[] assignment/status
     *   fault_comms_init()               fault_buffer histories and
     *                                    last-heard ticks; fault traffic
     *                                    that arrived during the boot
     *                                    wait was already discarded
     *                                    (board_boot_wait_discard_
     *                                    stale_frames()), and
     *                                    comms_transport_init() has run
     *   search_zones_init()              sweep bookkeeping, which is
     *                                    laptop-side only on a board
     *   fault_predictor_features_init()  g_fault_predictor_team_state[]/
     *                                    g_fault_predictor_incident_
     *                                    baseline[] only - own module,
     *                                    reads nothing at init time
     * Deliberately NOT called: initialize_incidents(), generate_teams(),
     * initialize_grid(), environment/safe-zone/gateway generation,
     * random_seed_all() and fault_injector_init() (a board injects no
     * faults), and anything else that clears g_incidents[], g_teams[]
     * roster/location fields, the map or the sites.
     *
     * Same order as generate_world(). g_current_tick is NOT reset
     * here (it follows the laptop's clock), which is why main_board.c
     * requires BOARD_SEED_COUNT == 1. */
    search_zones_init();
    watchdog_init();
    mission_execution_twin_reset();
    cbba_reset_improvement_counters();
    recovery_manager_init();
    fault_comms_init();
#if VIRTUAL_BUS_ENABLED
    consensus_init(seed);
#endif
    fault_predictor_features_init();

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
    probe_step();           /* Step 5 probes; no-op unless a --probe-* flag was given */
    if(g_sim_role != SIM_ROLE_BOARD)
        road_knowledge_step();  /* Step 5 R1-R3; no-op unless --probe-roadknow */
    if(g_sim_role != SIM_ROLE_BOARD)
        causal_actions_step();  /* causal profile only; no-op otherwise */
    allocate_all_incidents();
    if(g_sim_role != SIM_ROLE_BOARD)
        probe_preposition();   /* Step 5 M2-M4; no-op unless --probe-foresight */
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

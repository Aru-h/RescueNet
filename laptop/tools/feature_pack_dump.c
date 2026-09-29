/*
 * Feature-pack parity dump (docs/ml/FEATURE_CONTRACT_DESIGN.md section 5 test:
 * "C vs Python feature parity: <=1e-5 scaled"): runs a real seeded,
 * random-fault-profile mission through the SAME simulation + snapshot.c
 * pipeline `tron` itself uses - so tron_snapshot_dataset.csv it writes is
 * the real training CSV, not a special-purpose one - and, for every team
 * on every tick, ALSO dumps core/feature_pack.c's pre-dispatch cached
 * vector (feature_pack_get_cached_vector() - the same one core/
 * simulation.c fills every tick before recovery_dispatch() runs and
 * evaluate_fault_predictor() consumes, NOT a fresh feature_pack_build_
 * vector() call after the tick has fully finished, which would read
 * post-dispatch state instead - see core/feature_pack.h's own doc) to a
 * side CSV keyed by (run_id, team_id, cycle).
 *
 * laptop/tools/feature_pack_parity_test.py compares this dump against
 * data_prep.py's own reconstruction of the same rows from tron_snapshot_
 * dataset.csv (--dump-raw-features, scaled with the same committed
 * feature_spec.py constants). A disagreement means core/feature_pack.c
 * and data_prep.py's feature construction have drifted from each other -
 * exactly the risk feature_spec.json's single-source-of-truth generation
 * exists to catch, checked end to end here rather than just by
 * inspection.
 *
 * Runs in the normal ENVIRONMENT_HAS_WORLD_STORAGE=1 (laptop/world-
 * authority) build, deliberately not the board build: feature_pack_
 * build_vector() is written to give identical answers in either mode
 * from identical observable state (that is its whole point - core/
 * feature_pack.h's own doc), and calling it in-process, same tick, same
 * g_teams[]/g_search_zones[]/g_environment[] state snapshot.c just
 * recorded from, removes any wire-transfer question from this test
 * entirely - that is what laptop/tools/zone_parity_test.c and zone_
 * refresh_test.c already cover separately.
 *
 * usage: feature_pack_dump <num_seeds> <ticks_per_seed> <first_seed> <features_csv_path>
 * (writes tron_snapshot_dataset.csv in the current directory, exactly
 * like `./tron`, plus features_csv_path)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mission_state.h"
#include "feature_pack.h"
#include "feature_spec.h"
#include "sim_runner.h"
#include "fault_config.h"
#include "snapshot.h"
#include "simulation.h"

int main(int argc, char **argv)
{
    uint32_t num_seeds, ticks_per_seed, first_seed;
    FILE *ff;
    uint32_t s, t, i;
    uint8_t team_id;

    if(argc != 5)
    {
        fprintf(stderr, "usage: %s <num_seeds> <ticks_per_seed> <first_seed> <features_csv_path>\n",
                argv[0]);
        return 1;
    }

    g_simulation_trace_enabled = 0u; /* keep this tool's output free of debug_trace() noise */
    num_seeds = (uint32_t)strtoul(argv[1], NULL, 10);
    ticks_per_seed = (uint32_t)strtoul(argv[2], NULL, 10);
    first_seed = (uint32_t)strtoul(argv[3], NULL, 10);

    ff = fopen(argv[4], "w");
    if(ff == NULL)
    {
        fprintf(stderr, "feature_pack_dump: cannot open %s\n", argv[4]);
        return 1;
    }

    fprintf(ff, "run_id,team_id,cycle");
    for(i = 0u; i < FEATURE_SPEC_NUM_INPUTS; i++)
        fprintf(ff, ",f%u", i);
    fprintf(ff, "\n");

    if(sim_runner_init() != 0)
    {
        fprintf(stderr, "feature_pack_dump: sim_runner_init failed\n");
        return 1;
    }

    for(s = 0u; s < num_seeds; s++)
    {
        uint32_t seed = first_seed + s;
        uint32_t run_id;

        sim_start_seed(seed, FAULT_PROFILE_RANDOM);
        run_id = snapshot_current_run_id();

        for(t = 1u; t <= ticks_per_seed; t++)
        {
            sim_tick();

            for(team_id = 0u; team_id < NUM_TEAMS; team_id++)
            {
                float feat[FEATURE_SPEC_NUM_INPUTS];

                /* The cache core/simulation.c filled this tick, before
                   recovery_dispatch() ran - the exact same pre-dispatch
                   snapshot evaluate_fault_predictor() used this tick, not
                   a fresh post-dispatch build (see feature_pack.h's own
                   doc on why those can differ). */
                feature_pack_get_cached_vector(team_id, feat);

                fprintf(ff, "%u,%u,%u", (unsigned)run_id, (unsigned)team_id, (unsigned)t);
                for(i = 0u; i < FEATURE_SPEC_NUM_INPUTS; i++)
                    fprintf(ff, ",%.9g", (double)feat[i]);
                fprintf(ff, "\n");
            }
        }

        sim_end_seed();
    }

    sim_runner_shutdown();
    fclose(ff);

    fprintf(stderr, "feature_pack_dump: wrote %s (%u seed(s) x %u ticks x %u teams)\n",
            argv[4], num_seeds, ticks_per_seed, NUM_TEAMS);
    return 0;
}

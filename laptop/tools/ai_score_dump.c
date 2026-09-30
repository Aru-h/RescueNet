/*
 * AI score dump: runs a real seeded, random-fault-profile mission (same
 * simulation + snapshot.c pipeline as `tron`, so tron_snapshot_dataset.csv
 * is written as usual) and, for every team on every tick, also writes the
 * fault predictor's 5 per-class scores (from the same pre-dispatch cached
 * feature vector evaluate_fault_predictor() used that tick) to a side CSV
 * keyed by (run_id, team_id, cycle). Build with -DFAULT_MODEL_LOADED=1 and
 * the laptop consensus flags to log the scores of the closed-loop AI build
 * (./tron_ai); laptop/tools/ai_closed_loop_analysis.py joins it with the
 * snapshot CSV. Diagnostic tool, not part of any check.
 *
 * usage: ai_score_dump <num_seeds> <ticks_per_seed> <first_seed> <scores_csv_path>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "mission_state.h"
#include "feature_pack.h"
#include "feature_spec.h"
#include "fault_predictor.h"
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
        fprintf(stderr, "ai_score_dump: cannot open %s\n", argv[4]);
        return 1;
    }

    fprintf(ff, "run_id,team_id,cycle,s_mobility,s_communication,s_resource,s_environment,s_mission\n");

    if(sim_runner_init() != 0)
    {
        fprintf(stderr, "ai_score_dump: sim_runner_init failed\n");
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

                FaultPredictorScores_t sc = fault_predictor_run(feat);

                fprintf(ff, "%u,%u,%u", (unsigned)run_id, (unsigned)team_id, (unsigned)t);
                for(i = 0u; i < NUM_OPERATIONAL_FAULT_CLASSES; i++)
                    fprintf(ff, ",%.6g", (double)sc.score[i]);
                fprintf(ff, "\n");
            }
        }

        sim_end_seed();
    }

    sim_runner_shutdown();
    fclose(ff);

    fprintf(stderr, "ai_score_dump: wrote %s (%u seed(s) x %u ticks x %u teams)\n",
            argv[4], num_seeds, ticks_per_seed, NUM_TEAMS);
    return 0;
}

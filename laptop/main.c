#define _POSIX_C_SOURCE 200809L /* for usleep - desktop test tooling only */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>    /* nanosleep - desktop test tooling only, live-mode pacing */

#include "allocator.h"
#include "environment.h"
#include "search_zones.h"
#include "simulation.h"
#include "scoring.h"
#include "debug.h"
#include "snapshot.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "sim_runner.h"

static int is_fault_profile_flag(const char *arg)
{
    return arg != NULL &&
           (strcmp(arg, "--fault-profile") == 0 ||
            strncmp(arg, "--fault-profile=", 16) == 0);
}

static FaultProfile_t parse_fault_profile_value(const char *arg)
{
    if(arg == NULL)
        return FAULT_PROFILE_NONE;

    if(strncmp(arg, "--fault-profile=", 16) == 0)
        return fault_profile_from_string(arg + 16);

    return FAULT_PROFILE_NONE;
}

static int is_number_arg(const char *arg)
{
    return arg != NULL && arg[0] >= '0' && arg[0] <= '9';
}

static int is_tick_delay_flag(const char *arg)
{
    return arg != NULL && strncmp(arg, "--tick-delay-ms=", 16) == 0;
}

int main(int argc, char **argv)
{
    if(sim_runner_init() != 0)
    {
        fprintf(stderr, "equipment requirement matrix validation failed\n");
        return 1;
    }

    FaultProfile_t fault_profile = FAULT_PROFILE_NONE;
    int live_mode = 0;
    int diag_mode = 0;  /* off by default - see --diag below */
    uint32_t tick_delay_ms = 0;
    uint32_t positional_args[5] = {0};
    uint32_t positional_count = 0;

    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "--live") == 0)
        {
            live_mode = 1;
            continue;
        }

        /* Prints the [DIAG SUMMARY]/[DIAG WORK]/[DIAG THROUGHPUT]/
         * [DIAG FOG]/[DIAG SWEEP] end-of-run instrumentation. The
         * underlying g_diag_* counters always get tracked (cheap,
         * always-on bookkeeping) - this flag only controls whether the
         * summary gets printed, so a normal run stays quiet and you
         * opt in with --diag when you actually want to look at it. */
        if(strcmp(argv[i], "--diag") == 0)
        {
            diag_mode = 1;
            continue;
        }

        /* Enables the per-tick [DIAG] status/RESTED trace in
         * simulation.c (g_simulation_trace_enabled) - off by default so
         * a bare `./tron` run doesn't scroll a status line every time
         * any team changes state. app.py passes this when it launches
         * the binary, since the dashboard's Recovery/Rest tab parses
         * this trace live. */
        if(strcmp(argv[i], "--trace") == 0)
        {
            g_simulation_trace_enabled = 1;
            continue;
        }

        if(is_tick_delay_flag(argv[i]))
        {
            tick_delay_ms = (uint32_t)strtoul(argv[i] + 16, NULL, 10);
            continue;
        }

        if(strcmp(argv[i], "--fault-profile") == 0 && i + 1 < argc)
        {
            fault_profile = fault_profile_from_string(argv[++i]);
            continue;
        }

        if(is_fault_profile_flag(argv[i]))
        {
            fault_profile = parse_fault_profile_value(argv[i]);
            continue;
        }

        if(!is_number_arg(argv[i]))
            continue;

        if(positional_count < 5)
            positional_args[positional_count++] = (uint32_t)strtoul(argv[i], NULL, 10);
    }

    fault_config_apply_profile(fault_profile);
    fault_injector_set_profile(fault_profile);

    uint32_t first_seed = positional_count >= 1 ? positional_args[0] : 1;
    uint32_t seed_count = positional_count >= 2 ? positional_args[1] : 1;
    uint32_t ticks_per_seed = positional_count >= 3 ? positional_args[2] : 10000;

    for(uint32_t seed = first_seed;
        seed < first_seed + seed_count;
        ++seed)
    {
        sim_start_seed(seed, fault_profile);

        if(live_mode)
        {
            /* Static layout - buildings, roads, districts don't move once
             * generated, so this is sent once per run, not every tick, to
             * keep LIVEFRAME payloads small. LIVEFRAME itself carries no
             * building/damage data at all (only teams + incidents) - so
             * building damage is normally a safe one-time snapshot, EXCEPT
             * for aftershocks (environment.c: apply_aftershocks()), which can
             * destroy an already-damaged building mid-run. That mutation was
             * previously never re-sent to the dashboard after this initial
             * emit, so a collapsed building silently never updated on any
             * client. See the aftershock_collapse_count check below, which
             * re-emits this same static-env payload only on the ticks where
             * that actually happens. */
            debug_emit_static_environment();
        }

        for(uint32_t tick = 0; tick < ticks_per_seed; ++tick)
        {
            uint32_t collapse_count_before = g_environment_summary.aftershock_collapse_count;
            sim_tick();

            if(live_mode)
            {
                debug_emit_live_frame(tick);
                if(g_environment_summary.aftershock_collapse_count != collapse_count_before)
                {
                    /* An aftershock destroyed at least one building this tick -
                     * re-send the (small) full building list so clients pick up
                     * the new damage_state/hazard_flags instead of going stale. */
                    debug_emit_static_environment();
                }
                if(tick_delay_ms > 0)
                {
                    struct timespec delay;
                    delay.tv_sec = tick_delay_ms / 1000u;
                    delay.tv_nsec = (long)(tick_delay_ms % 1000u) * 1000000L;
                    nanosleep(&delay, NULL);
                }
            }
        }

        sim_end_seed();
    }

    sim_runner_shutdown();

    extern uint32_t g_diag_safezone_fallback_count;
    extern uint32_t g_diag_discarded_signal_count;
    if(diag_mode)
    {
        extern uint32_t g_diag_incidents_resolved[NUM_TEAMS];
        extern uint32_t g_diag_stage_completions[NUM_TEAMS];
        extern uint32_t g_diag_work_progress_calls;
        extern uint32_t g_diag_in_progress_entries;
        extern uint32_t g_diag_ticks_resting[NUM_TEAMS];
        extern uint32_t g_diag_ticks_working[NUM_TEAMS];
        extern uint32_t g_diag_ticks_available[NUM_TEAMS];
        fprintf(stderr, "[DIAG SUMMARY] safezone_fallback_count=%u discarded_signal_count=%u\n",
            g_diag_safezone_fallback_count, g_diag_discarded_signal_count);
        fprintf(stderr, "[DIAG WORK] in_progress_entries=%u work_progress_calls=%u\n",
            g_diag_in_progress_entries, g_diag_work_progress_calls);
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
            fprintf(stderr, "[DIAG THROUGHPUT] team=%u incidents_resolved=%u stage_completions=%u resting=%u working=%u available=%u\n",
                t, g_diag_incidents_resolved[t], g_diag_stage_completions[t], g_diag_ticks_resting[t], g_diag_ticks_working[t], g_diag_ticks_available[t]);

        extern uint32_t g_diag_discovered_by_type[NUM_INCIDENT_TYPES];
        extern uint32_t g_diag_resolved_by_type[NUM_INCIDENT_TYPES];
        extern uint32_t g_diag_spawned_by_type[NUM_INCIDENT_TYPES];
        for(uint8_t ty = 0; ty < NUM_INCIDENT_TYPES; ty++)
            fprintf(stderr, "[DIAG FOG] type=%u spawned=%u discovered=%u resolved=%u\n",
                ty, g_diag_spawned_by_type[ty], g_diag_discovered_by_type[ty], g_diag_resolved_by_type[ty]);

        extern uint32_t g_diag_sweep_genuine_complete[NUM_SEARCH_ZONES];
        extern uint32_t g_diag_sweep_stalled[NUM_SEARCH_ZONES];
        extern uint32_t g_diag_sweep_stall_coverage_sum[NUM_SEARCH_ZONES];
        extern uint32_t g_diag_sweep_tasks_created[NUM_SEARCH_ZONES];
        for(uint8_t z = 0; z < NUM_SEARCH_ZONES; z++)
        {
            uint32_t stalled = g_diag_sweep_stalled[z];
            uint32_t avg_cov = stalled ? (g_diag_sweep_stall_coverage_sum[z] / stalled) : 0;
            fprintf(stderr, "[DIAG SWEEP] district=%u tasks_created=%u genuine_complete=%u stalled=%u avg_coverage_at_stall=%u%%\n",
                z, g_diag_sweep_tasks_created[z], g_diag_sweep_genuine_complete[z], stalled, avg_cov);
        }
        extern uint32_t g_diag_sweep_abandoned_for_recovery;
        extern uint32_t g_diag_sweep_reclaimed_for_safezone;
        extern uint32_t g_diag_sweep_duplicate_while_in_progress;
        fprintf(stderr, "[DIAG SWEEP CHURN] abandoned_for_recovery=%u reclaimed_for_safezone=%u duplicate_while_in_progress=%u\n",
            g_diag_sweep_abandoned_for_recovery, g_diag_sweep_reclaimed_for_safezone, g_diag_sweep_duplicate_while_in_progress);
        extern uint32_t g_diag_sweep_retargets;
        fprintf(stderr, "[DIAG SWEEP PATROL] retargets=%u\n", g_diag_sweep_retargets);
    }

    print_world_state();

    return 0;
}

#define _POSIX_C_SOURCE 200809L /* for usleep - desktop test tooling only */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>    /* nanosleep - desktop test tooling only, live-mode pacing */
#include <sys/select.h> /* mission-feed mode: board reports on stdin between ticks */
#include <unistd.h>

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
#include "mission_feed.h"
#include "mission_mirror.h"
#include "mission_state.h" /* TICK_TIME_SEC */
#include "consensus.h"
#include "fault_predictor.h" /* FAULT_MODEL_LOADED - --model-info */

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000L);
}

/* Board mission (--mission-feed): until deadline_ms, applies each board
 * report the bridge writes to stdin (laptop/mission_mirror.h). Returns
 * once the deadline passes - immediately after one non-blocking drain
 * when the tick delay is 0. A closed stdin (no bridge attached) just
 * sleeps out the rest of the tick. */
static int g_mission_stdin_open = 1;
static char g_mission_line[256];
static size_t g_mission_line_len;

static void mission_wait_for_reports(uint64_t deadline_ms)
{
    for(;;)
    {
        uint64_t now = monotonic_ms();
        uint64_t left = (deadline_ms > now) ? (deadline_ms - now) : 0u;

        if(!g_mission_stdin_open)
        {
            if(left > 0u)
            {
                struct timespec delay;
                delay.tv_sec = (time_t)(left / 1000u);
                delay.tv_nsec = (long)(left % 1000u) * 1000000L;
                nanosleep(&delay, NULL);
            }
            return;
        }

        fd_set readable;
        struct timeval timeout;
        FD_ZERO(&readable);
        FD_SET(STDIN_FILENO, &readable);
        timeout.tv_sec = (time_t)(left / 1000u);
        timeout.tv_usec = (suseconds_t)((left % 1000u) * 1000u);

        int ready = select(STDIN_FILENO + 1, &readable, NULL, NULL, &timeout);
        if(ready <= 0)
            return;  /* deadline reached (or interrupted) */

        char chunk[256];
        ssize_t n = read(STDIN_FILENO, chunk, sizeof(chunk));
        if(n <= 0)
        {
            g_mission_stdin_open = 0;
            continue;
        }

        for(ssize_t i = 0; i < n; i++)
        {
            if(chunk[i] == '\n')
            {
                g_mission_line[g_mission_line_len] = '\0';
                if(mission_mirror_ingest_line(g_mission_line))
                    mission_feed_tasks(g_current_tick);
                g_mission_line_len = 0;
            }
            else if(g_mission_line_len + 1u < sizeof(g_mission_line))
            {
                g_mission_line[g_mission_line_len++] = chunk[i];
            }
            else
            {
                g_mission_line_len = 0;  /* over-long line: not a report, drop it */
            }
        }
    }
}

static int is_fault_profile_flag(const char *arg)
{
    return arg != NULL &&
           (strcmp(arg, "--fault-profile") == 0 ||
            strncmp(arg, "--fault-profile=", 16) == 0);
}

/* An unknown name used to silently become "none", so a typo ran a
 * fault-free simulation that looked like a valid result. */
static FaultProfile_t require_fault_profile(const char *name)
{
    FaultProfile_t profile = FAULT_PROFILE_NONE;

    if(name == NULL || !fault_profile_parse(name, &profile))
    {
        fprintf(stderr,
            "unknown --fault-profile '%s' (valid: none, mobility, communication, "
            "resource, combined, random, causal)\n",
            name != NULL ? name : "");
        exit(2);
    }

    return profile;
}

static FaultProfile_t parse_fault_profile_value(const char *arg)
{
    if(arg != NULL && strncmp(arg, "--fault-profile=", 16) == 0)
        return require_fault_profile(arg + 16);

    /* Bare "--fault-profile" with nothing after it. */
    return require_fault_profile(NULL);
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
    /* `tron --model-info`: report whether the AI fault predictor is compiled
     * in and exit. Read by laptop/tools/app.py so a recording's header says
     * whether it was an AI run. Runs no simulation and writes no files. */
    if(argc == 2 && strcmp(argv[1], "--model-info") == 0)
    {
        printf("model_loaded=%d\n", (int)FAULT_MODEL_LOADED);
        return 0;
    }

    if(sim_runner_init() != 0)
    {
        fprintf(stderr, "equipment requirement matrix validation failed\n");
        return 1;
    }

    /* Priority 1 verification tooling - handled as a dedicated pre-pass
     * so flag order doesn't matter (e.g. `--sweep-priority1 1 200
     * 20000 --fault-profile=combined` and `--fault-profile=combined
     * --sweep-priority1 1 200 20000` both work), and so it can exit
     * before the normal run loop below ever starts. Not part of the
     * move_team() fix itself - see debug_run_priority1_sweep()'s doc
     * comment for what it checks. */
    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "--repro-priority1") == 0)
        {
            return debug_run_priority1_repro();
        }

        if(strcmp(argv[i], "--sweep-priority1") == 0)
        {
            uint32_t first_seed = (uint32_t)((i + 1 < argc && is_number_arg(argv[i + 1])) ? strtoul(argv[i + 1], NULL, 10) : 1);
            uint32_t seed_count = (uint32_t)((i + 2 < argc && is_number_arg(argv[i + 2])) ? strtoul(argv[i + 2], NULL, 10) : 50);
            uint32_t ticks_per_seed = (uint32_t)((i + 3 < argc && is_number_arg(argv[i + 3])) ? strtoul(argv[i + 3], NULL, 10) : 10000);
            FaultProfile_t sweep_profile = FAULT_PROFILE_COMBINED;

            for(int j = 1; j < argc; j++)
            {
                if(is_fault_profile_flag(argv[j]))
                {
                    sweep_profile = strcmp(argv[j], "--fault-profile") == 0 && j + 1 < argc
                                         ? require_fault_profile(argv[j + 1])
                                         : parse_fault_profile_value(argv[j]);
                }
            }

            return debug_run_priority1_sweep(first_seed, seed_count, ticks_per_seed, sweep_profile);
        }
    }

    FaultProfile_t fault_profile = FAULT_PROFILE_NONE;
    float fault_stress = 1.0f;
    int fault_stress_given = 0;
    int live_mode = 0;
    int mission_feed_mode = 0; /* --mission-feed, see laptop/mission_feed.h */
    int diag_mode = 0;  /* off by default - see --diag below */
    uint32_t tick_delay_ms = 0;
    /* --fast-forward=T (with --live): silently run ticks 0..T-1, then
     * emit frame T and carry on normally - the dashboard's "take over
     * from replay" rebuilds state by deterministic re-simulation. */
    uint32_t fast_forward_ticks = 0;
    const char *override_script_path = NULL;
    uint32_t positional_args[5] = {0};
    uint32_t positional_count = 0;

    for(int i = 1; i < argc; i++)
    {
        if(strcmp(argv[i], "--live") == 0)
        {
            live_mode = 1;
            continue;
        }

        /* Board mission: world events only, teams mirrored from board
         * reports on stdin (laptop/mission_mirror.h), and the frames the
         * boards need written as MLFRAME lines on stdout
         * (laptop/mission_feed.h). Paced by --tick-delay-ms like --live. */
        if(strcmp(argv[i], "--mission-feed") == 0)
        {
            mission_feed_mode = 1;
            continue;
        }

        /* One tick per TICK_TIME_SEC (36 s) of wall-clock time - the
         * timing a real deployment runs at. Shorthand for
         * --tick-delay-ms=36000. */
        if(strcmp(argv[i], "--realtime") == 0)
        {
            tick_delay_ms = (uint32_t)TICK_TIME_SEC * 1000u;
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

        if(strncmp(argv[i], "--fast-forward=", 15) == 0)
        {
            fast_forward_ticks = (uint32_t)strtoul(argv[i] + 15, NULL, 10);
            continue;
        }

        /* Applied overrides of the recording being resumed (see
         * debug_live_override_load_script()); only used with
         * --fast-forward. */
        if(strncmp(argv[i], "--override-script=", 18) == 0)
        {
            override_script_path = argv[i] + 18;
            continue;
        }

        if(is_tick_delay_flag(argv[i]))
        {
            tick_delay_ms = (uint32_t)strtoul(argv[i] + 16, NULL, 10);
            continue;
        }

        /* Per-link loss probability on the virtual peer bus
         * (docs/architecture/CONSENSUS_DESIGN.md 3.5, decision D4: default 0.05). Ignored in a
         * build without the virtual bus. */
        if(strncmp(argv[i], "--vbus-loss=", 12) == 0)
        {
#if VIRTUAL_BUS_ENABLED
            consensus_set_link_loss((float)atof(argv[i] + 12));
#endif
            continue;
        }

        /* Writes tron_peer_obs.csv, one row per (tick, observer, target):
         * the training set for a future peer-judge model (design 5.2). */
        if(strcmp(argv[i], "--peer-log") == 0)
        {
            snapshot_peer_log_enable(1);
            continue;
        }

        if(strcmp(argv[i], "--fault-profile") == 0 && i + 1 < argc)
        {
            fault_profile = require_fault_profile(argv[++i]);
            continue;
        }

        /* Causal-profile evaluation stress multiplier (1 = realistic field
         * rates). Rejected for every other profile rather than ignored. */
        if(strncmp(argv[i], "--fault-stress=", 15) == 0 ||
           (strcmp(argv[i], "--fault-stress") == 0 && i + 1 < argc))
        {
            const char *text = argv[i][14] == '=' ? argv[i] + 15 : argv[++i];
            char *end = NULL;
            double value = strtod(text, &end);

            if(end == text || *end != '\0' || !(value > 0.0) || value > 1000.0)
            {
                fprintf(stderr, "invalid --fault-stress '%s' (need 0 < x <= 1000)\n", text);
                exit(2);
            }
            fault_stress = (float)value;
            fault_stress_given = 1;
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

    if(fault_stress_given && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--fault-stress only applies to --fault-profile=causal\n");
        return 2;
    }
    fault_config_set_stress(fault_stress);
    fault_config_apply_profile(fault_profile);
    fault_injector_set_profile(fault_profile);
    if(fault_profile == FAULT_PROFILE_CAUSAL)
        fprintf(stderr, "[FAULT] profile=causal fault_stress=%g\n", (double)fault_stress);

    if(fast_forward_ticks > 0 && (!live_mode || mission_feed_mode))
    {
        fprintf(stderr, "--fast-forward requires --live (and not --mission-feed)\n");
        return 2;
    }

    uint32_t first_seed = positional_count >= 1 ? positional_args[0] : 1;
    uint32_t seed_count = positional_count >= 2 ? positional_args[1] : 1;
    uint32_t ticks_per_seed = positional_count >= 3 ? positional_args[2] : 10000;

    /* Counter loop, not `seed < first_seed + seed_count`: that sum can
     * overflow uint32_t for large inputs and run zero (or wrong) seeds. */
    for(uint32_t seed_index = 0; seed_index < seed_count; ++seed_index)
    {
        uint32_t seed = first_seed + seed_index;

        sim_start_seed(seed, fault_profile);

        if(mission_feed_mode)
        {
            g_sim_role = SIM_ROLE_WORLD_MIRROR;
            mission_mirror_begin();
            mission_feed_begin();
        }

        if(fast_forward_ticks > 0 && fast_forward_ticks >= ticks_per_seed)
        {
            fprintf(stderr, "--fast-forward=%u is not before the last tick (%u ticks)\n",
                    fast_forward_ticks, ticks_per_seed);
            return 2;
        }

        if(live_mode && fast_forward_ticks == 0)
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
            /* Part 3: per-cell terrain (travel cost/navigable/flood/
             * road/district), same one-shot-then-only-on-change idea as
             * the STATICENV line above - see debug_emit_static_map_
             * if_changed()'s own comment in laptop/debug.c. */
            debug_emit_static_map_if_changed();

            /* Part 4: sim-mode manual override, --live only - never
             * runs under --mission-feed, which owns stdin for its own
             * board-report reader (mission_wait_for_reports() above);
             * the two are mutually exclusive by this same guard
             * everywhere else in this loop (see the nanosleep gate
             * below). See core/debug.h's doc for the line protocol. */
            if(!mission_feed_mode)
                debug_live_override_init();
        }
        else if(live_mode)
        {
            /* Fast-forward: nothing is emitted until frame T; the static
             * environment/terrain are then sent once, as of tick T. */
            debug_live_override_init();
            if(override_script_path && debug_live_override_load_script(override_script_path) != 0)
            {
                fprintf(stderr, "cannot load --override-script=%s\n", override_script_path);
                return 2;
            }
        }

        uint64_t next_tick_deadline_ms = monotonic_ms() + tick_delay_ms;

        for(uint32_t tick = 0; tick < ticks_per_seed; ++tick)
        {
            uint32_t collapse_count_before = g_environment_summary.aftershock_collapse_count;

            if(mission_feed_mode)
            {
                /* The previous tick's boards report in while the laptop
                 * waits out this tick's wall-clock period. Deadline-based
                 * rather than a fixed sleep so time spent simulating
                 * doesn't stretch the tick. */
                mission_wait_for_reports(next_tick_deadline_ms);
                next_tick_deadline_ms += tick_delay_ms;
            }

            sim_tick();

            if(tick < fast_forward_ticks)
            {
                debug_live_override_replay_script(tick);
                if(tick % 100u == 99u)
                {
                    printf("FFPROGRESS {\"tick\":%u,\"target\":%u}\n", tick + 1u, fast_forward_ticks);
                    fflush(stdout);
                }
                continue;
            }
            if(tick == fast_forward_ticks && fast_forward_ticks > 0)
            {
                debug_emit_static_environment();
                debug_emit_static_map_if_changed();
            }

            if(mission_feed_mode)
            {
                mission_feed_tick(tick + 1u);
            }

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
                /* Cheap every-tick check (1296-cell comparison); only
                 * actually re-prints the STATICMAP line on the rare tick
                 * where a cell's terrain genuinely changed. */
                debug_emit_static_map_if_changed();
            }

            if(live_mode && !mission_feed_mode)
            {
                /* One non-blocking stdin poll for a manual override -
                 * never blocks this loop (see debug_live_override_
                 * poll()'s own comment), applied here so it's visible
                 * to the very next sim_tick()'s cbba_build_bundle()
                 * pass rather than waiting a full extra tick. */
                debug_live_override_poll(tick);
            }

            if(live_mode && !mission_feed_mode && tick_delay_ms > 0)
            {
                struct timespec delay;
                delay.tv_sec = tick_delay_ms / 1000u;
                delay.tv_nsec = (long)(tick_delay_ms % 1000u) * 1000000L;
                nanosleep(&delay, NULL);
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

        if(g_fault_config.profile == FAULT_PROFILE_CAUSAL)
        {
            const CausalWorldStats_t *ws = fault_injector_causal_world_stats();
            fprintf(stderr, "[DIAG CAUSAL] fault_stress=%g victims_seen=%u victims_lost=%u escalations=%u aftershock_events=%u aftershock_team_hits=%u\n",
                (double)g_fault_stress, ws->victims_seen, ws->victims_lost, ws->escalations,
                ws->aftershock_events, ws->aftershock_team_hits);
        }
        if(g_fault_config.profile == FAULT_PROFILE_CAUSAL)
        {
            const CausalFaultStats_t *cs = fault_injector_causal_stats();
            for(uint8_t c = 0; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
                fprintf(stderr, "[DIAG FAULT] class=%u onsets=%u cleared_by_cause=%u cleared_by_backstop=%u exposure_ticks=%u episode_ticks=%u\n",
                    c, cs->onsets[c], cs->cleared_by_cause[c], cs->cleared_by_backstop[c],
                    cs->exposure_ticks[c], cs->active_ticks_total[c]);
        }

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

    extern unsigned long g_probe_no_path_hits;
    fprintf(stderr, "[PROBE] task_branch_no_path_hits=%lu\n", g_probe_no_path_hits);

    return 0;
}
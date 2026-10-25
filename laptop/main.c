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
#include "causal_actions.h"
#include "sim_runner.h"
#include "mission_feed.h"
#include "mission_mirror.h"
#include "mission_state.h" /* TICK_TIME_SEC */
#include "consensus.h"
#include "fault_predictor.h" /* FAULT_MODEL_LOADED - --model-info */
#include "recovery_planner.h"
#include "headroom_probes.h"
#include "road_knowledge.h"

/* Planner decision log (--planner-log): one row per non-trivial decision. Laptop only; not part of the
 * dataset or the causal sidecar, and no simulation effect. */
static FILE *g_planner_log;
static uint32_t g_planner_log_seed;

static void planner_log_sink(const PlannerDecision_t *d)
{
    static const char *const layer[3] = { "A", "B", "preempt" };

    if(g_planner_log == NULL)
        return;
    fprintf(g_planner_log, "%u,%u,%s,%u,%u,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%d,%d,%d,%.3f\n",
            g_planner_log_seed, d->tick, layer[d->layer < 3u ? d->layer : 0u], d->team, d->chosen,
            d->alt == 255u ? -1 : (int)d->alt,
            (double)d->p[0], (double)d->p[1], (double)d->p[2],
            (double)d->p_ai[0], (double)d->p_ai[1], (double)d->p_ai[2],
            (double)d->cost_fault, (double)d->cost_action, (double)d->cost_alt, (double)d->margin,
            (unsigned)d->flags, (int)d->target_x, (int)d->target_y, (int)d->aux, (double)d->aux_cost);
}

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
    int link_margin_map = 0;
    int causal_actions_flag = 0;   /* default off: see docs/RECOVERY_ACTIONS.md */
    int causal_actions_given = 0;
    uint8_t causal_env_min = 0, causal_env_max = 0;
    uint8_t causal_env_delay = 0;
    int roads_flag_given = 0;
    int causal_fixes_mask = -1;
    int causal_persist_mode = -1;  /* Step 3.6: -1 = not given (default on under causal) */
    const char *probe_record_path = NULL;   /* Step 5: --probe-record=FILE writes the hazard events this run queued */
    int planner_flag_given = 0;    /* any --planner* flag */   /* -1 = not given (default: all fixes on under causal) */
    int causal_reroute_penalty = -1;
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

        /* Causal-profile recovery actions (reroute, clear road, reinforce,
         * relay). OFF by default under causal (the profile as it was before
         * they existed); on, or a comma list, enables them. */
        if(strncmp(argv[i], "--causal-actions=", 17) == 0)
        {
            /* on | off | comma list of reroute,clear_road,reinforce,relay */
            const char *v = argv[i] + 17;
            static const char *const names[NUM_CAUSAL_ACTIONS] =
                { "reroute", "clear_road", "reinforce", "relay" };
            int mask = 0;

            if(strcmp(v, "on") == 0)
                mask = (int)CAUSAL_ACTIONS_ALL;
            else if(strcmp(v, "off") != 0)
            {
                char buf[96];
                char *tok;

                snprintf(buf, sizeof(buf), "%s", v);
                for(tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ","))
                {
                    int found = 0;

                    for(int a = 0; a < NUM_CAUSAL_ACTIONS; a++)
                    {
                        if(strcmp(tok, names[a]) == 0)
                        {
                            mask |= 1 << a;
                            found = 1;
                        }
                    }
                    if(!found)
                    {
                        fprintf(stderr, "invalid --causal-actions '%s' (on, off, or a list of reroute,clear_road,reinforce,relay)\n", v);
                        exit(2);
                    }
                }
            }
            causal_actions_flag = mask;
            causal_actions_given = 1;
            continue;
        }

        if(strncmp(argv[i], "--causal-env-duration=", 22) == 0)
        {
            unsigned lo = 0, hi = 0;

            if(sscanf(argv[i] + 22, "%u-%u", &lo, &hi) != 2 || lo < 1 || hi < lo || hi > 255)
            {
                fprintf(stderr, "invalid --causal-env-duration '%s' (need <min>-<max>, 1 <= min <= max <= 255)\n", argv[i] + 22);
                exit(2);
            }
            causal_env_min = (uint8_t)lo;
            causal_env_max = (uint8_t)hi;
            continue;
        }

        if(strncmp(argv[i], "--causal-env-crossing-delay=", 28) == 0)
        {
            char *end = NULL;
            long v = strtol(argv[i] + 28, &end, 10);

            if(end == argv[i] + 28 || *end != '\0' || v < 1 || v > 20)
            {
                fprintf(stderr, "invalid --causal-env-crossing-delay '%s' (need 1-20 ticks)\n", argv[i] + 28);
                exit(2);
            }
            causal_env_delay = (uint8_t)v;
            continue;
        }

        if(strncmp(argv[i], "--causal-reroute-penalty=", 25) == 0)
        {
            char *end = NULL;
            long v = strtol(argv[i] + 25, &end, 10);

            if(end == argv[i] + 25 || *end != '\0' || v < 0 || v > 255)
            {
                fprintf(stderr, "invalid --causal-reroute-penalty '%s' (need 0-255)\n", argv[i] + 25);
                exit(2);
            }
            causal_reroute_penalty = (int)v;
            continue;
        }

        /* Step 3.5 recovery planner (causal only): --recovery=fixed|utility and --planner* */
        if(strncmp(argv[i], "--recovery=", 11) == 0)
        {
            const char *v = argv[i] + 11;

            if(strcmp(v, "utility") == 0)
                g_recovery_mode = RECOVERY_MODE_UTILITY;
            else if(strcmp(v, "fixed") == 0)
                g_recovery_mode = RECOVERY_MODE_FIXED;
            else
            {
                fprintf(stderr, "invalid --recovery '%s' (need fixed or utility)\n", v);
                exit(2);
            }
            continue;
        }
        if(strncmp(argv[i], "--probe-", 8) == 0)
        {
            /* Step 5 headroom probes (docs/HEADROOM_MAP_PREREG.md): opt-in, causal profile only. */
            const char *a = argv[i] + 8;
            char *end;
            double x;

            g_probe.any = 1;
            if(strcmp(a, "diag") == 0) g_probe.diag = 1;
            else if(strncmp(a, "oracle-delay=", 13) == 0)
            {
                x = strtod(a + 13, &end);
                if(*end != 0 || !(x >= 0.0 && x < 60.0)) { fprintf(stderr, "invalid --probe-oracle-delay (0-59 ticks)\n"); exit(2); }
                g_probe.oracle_delay = (uint16_t)x;
            }
            else if(strncmp(a, "oracle-fn=", 10) == 0 || strncmp(a, "oracle-fp=", 10) == 0)
            {
                x = strtod(a + 10, &end);
                if(*end != 0 || !(x >= 0.0 && x <= 1.0)) { fprintf(stderr, "invalid --probe-oracle-fn/fp (0-1)\n"); exit(2); }
                if(a[8] == 'n') g_probe.oracle_fn = (float)x; else g_probe.oracle_fp = (float)x;
                g_probe.oracle_noise = 1;
            }
            else if(strncmp(a, "evidence-dropout=", 17) == 0)
            {
                x = strtod(a + 17, &end);
                if(*end != 0 || !(x >= 0.0 && x < 1.0)) { fprintf(stderr, "invalid --probe-evidence-dropout (0-1)\n"); exit(2); }
                g_probe.evidence_dropout = (float)x;
            }
            else if(strncmp(a, "prevent-cost=", 13) == 0)
            {
                x = strtod(a + 13, &end);
                if(*end != 0 || !(x > 0.0 && x <= 10.0)) { fprintf(stderr, "invalid --probe-prevent-cost (>0)\n"); exit(2); }
                g_probe.prevent_cost = (float)x;
            }
            else if(strcmp(a, "hazard-foresight=draw") == 0) g_probe.hazard_draw = 1;
            else if(strcmp(a, "urgency=survival") == 0) g_probe.urgency = 1;
            else if(strcmp(a, "urgency=foresight") == 0) g_probe.urgency = 2;
            else if(strcmp(a, "reveal-all") == 0) g_probe.reveal_all = 1;
            else if(strncmp(a, "offline=", 8) == 0)
            {
                x = strtod(a + 8, &end);
                if(*end != 0 || !(x >= 0.0 && x <= 5.0)) { fprintf(stderr, "invalid --probe-offline (0-5)\n"); exit(2); }
                g_probe.offline_k = (uint8_t)x;
            }
            else if(strncmp(a, "work-mult=", 10) == 0)
            {
                x = strtod(a + 10, &end);
                if(*end != 0 || !(x >= 0.1 && x <= 20.0)) { fprintf(stderr, "invalid --probe-work-mult\n"); exit(2); }
                g_probe.work_mult = (float)x;
            }
            else if(strcmp(a, "work-fatigue=a") == 0) g_probe.work_fatigue_b = 0;
            else if(strcmp(a, "work-fatigue=b") == 0) g_probe.work_fatigue_b = 1;
            else if(strncmp(a, "work-spec=", 10) == 0)
            {
                static const char *names[5] = {"search", "medical", "extraction", "perimeter", "support"};
                char nm[16]; double xv; int found = 0;

                if(sscanf(a + 10, "%15[a-z]:%lf", nm, &xv) != 2 || xv < 0.1 || xv > 20.0) { fprintf(stderr, "invalid --probe-work-spec (name:mult)\n"); exit(2); }
                for(int i2 = 0; i2 < 5; i2++) if(strcmp(nm, names[i2]) == 0) { g_probe.work_spec = (uint8_t)(i2 + 1); found = 1; }
                if(!found) { fprintf(stderr, "invalid --probe-work-spec name\n"); exit(2); }
                g_probe.work_spec_mult = (float)xv;
            }
            else if(strncmp(a, "work-stage=", 11) == 0)
            {
                char nm[8]; double xv;

                if(sscanf(a + 11, "%7[a-z]:%lf", nm, &xv) != 2 || xv < 0.1 || xv > 20.0) { fprintf(stderr, "invalid --probe-work-stage (early|late:mult)\n"); exit(2); }
                if(strcmp(nm, "early") == 0) g_probe.work_stage = 1;
                else if(strcmp(nm, "late") == 0) g_probe.work_stage = 2;
                else { fprintf(stderr, "invalid --probe-work-stage name\n"); exit(2); }
                g_probe.work_stage_mult = (float)xv;
            }
            else if(strcmp(a, "instant-work") == 0) g_probe.instant_work = 1;
            else if(strcmp(a, "instant-travel=task") == 0) g_probe.instant_travel = 1;
            else if(strcmp(a, "instant-travel=all") == 0) g_probe.instant_travel = 2;
            else if(strncmp(a, "speed=", 6) == 0)
            {
                static const char *names[5] = {"search", "medical", "extraction", "perimeter", "support"};
                char buf[96], *tok;

                strncpy(buf, a + 6, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
                for(tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ","))
                {
                    char *colon = strchr(tok, ':'); int found = 0;

                    if(colon == NULL) { fprintf(stderr, "invalid --probe-speed\n"); exit(2); }
                    *colon = 0;
                    x = strtod(colon + 1, &end);
                    if(*end != 0 || !(x > 0.0 && x <= 8.0)) { fprintf(stderr, "invalid --probe-speed value\n"); exit(2); }
                    for(int i2 = 0; i2 < 5; i2++) if(strcmp(tok, names[i2]) == 0) { g_probe.speed[i2] = (float)x; found = 1; }
                    if(!found) { fprintf(stderr, "invalid --probe-speed name\n"); exit(2); }
                }
            }
            else if(strcmp(a, "team-mix=a") == 0) g_probe.team_mix = 1;
            else if(strcmp(a, "team-mix=b") == 0) g_probe.team_mix = 2;
            else if(strcmp(a, "team-mix=c") == 0) g_probe.team_mix = 3;
            else if(strcmp(a, "central-alloc") == 0) g_probe.central_alloc = 1;
            else if(strncmp(a, "foresight=", 10) == 0)
            {
                if(strcmp(a + 10, "fire") == 0) g_probe.foresight = 1;
                else if(strcmp(a + 10, "collapse") == 0) g_probe.foresight = 2;
                else if(strcmp(a + 10, "hazard") == 0) g_probe.foresight = 0xFF;
                else { fprintf(stderr, "invalid --probe-foresight (fire, collapse, hazard)\n"); exit(2); }
            }
            else if(strncmp(a, "lead=", 5) == 0)
            {
                x = strtod(a + 5, &end);
                if(*end != 0 || !(x >= 1.0 && x <= 1000.0)) { fprintf(stderr, "invalid --probe-lead (1-1000 ticks)\n"); exit(2); }
                g_probe.lead = (uint16_t)x;
            }
            else if(strncmp(a, "record=", 7) == 0) probe_record_path = a + 7;
            else if(strncmp(a, "events=", 7) == 0)
            {
                FILE *ef = fopen(a + 7, "r");
                unsigned tk, ty; int ex, ey;

                if(ef == NULL) { fprintf(stderr, "cannot open --probe-events file\n"); exit(2); }
                g_probe_ev_plan_n = 0;
                while(g_probe_ev_plan_n < PROBE_MAX_EVENTS && fscanf(ef, "%u %u %d %d", &tk, &ty, &ex, &ey) == 4)
                {
                    g_probe_ev_plan[g_probe_ev_plan_n].tick = tk;
                    g_probe_ev_plan[g_probe_ev_plan_n].type = (uint8_t)ty;
                    g_probe_ev_plan[g_probe_ev_plan_n].x = (int16_t)ex;
                    g_probe_ev_plan[g_probe_ev_plan_n].y = (int16_t)ey;
                    g_probe_ev_plan_n++;
                }
                fclose(ef);
            }
            else if(strncmp(a, "roadknow=", 9) == 0)
            {
                if(strcmp(a + 9, "omni") == 0) g_road_know.mode = 0;
                else if(strcmp(a + 9, "obs") == 0) g_road_know.mode = 1;
                else if(strcmp(a + 9, "prior") == 0) g_road_know.mode = 2;
                else { fprintf(stderr, "invalid --probe-roadknow (omni, obs, prior)\n"); exit(2); }
            }
            else if(strncmp(a, "road-radius=", 12) == 0)
            {
                x = strtod(a + 12, &end);
                if(*end != 0 || !(x >= 0.0 && x <= 4.0)) { fprintf(stderr, "invalid --probe-road-radius (0-4)\n"); exit(2); }
                g_road_know.radius = (uint8_t)x;
            }
            else if(strncmp(a, "road-latency=", 13) == 0)
            {
                x = strtod(a + 13, &end);
                if(*end != 0 || !(x >= 0.0 && x <= 200.0)) { fprintf(stderr, "invalid --probe-road-latency\n"); exit(2); }
                g_road_know.latency = (uint8_t)x;
            }
            else if(strncmp(a, "road-loss=", 10) == 0)
            {
                x = strtod(a + 10, &end);
                if(*end != 0 || !(x >= 0.0 && x <= 1.0)) { fprintf(stderr, "invalid --probe-road-loss\n"); exit(2); }
                g_road_know.loss = (float)x;
            }
            else if(strcmp(a, "road-share=instant") == 0) g_road_know.instant = 1;
            else if(strcmp(a, "no-comm-faults") == 0) g_probe.no_comm_faults = 1;
            else if(strncmp(a, "rest-mult=", 10) == 0 || strncmp(a, "drain-mult=", 11) == 0)
            {
                int rest = (a[0] == 'r');
                x = strtod(a + (rest ? 10 : 11), &end);
                if(*end != 0 || !(x > 0.0 && x <= 10.0)) { fprintf(stderr, "invalid --probe-rest-mult/drain-mult (>0)\n"); exit(2); }
                if(rest) g_probe.rest_mult = (float)x; else g_probe.drain_mult = (float)x;
            }
            else { fprintf(stderr, "unknown probe flag '%s'\n", argv[i]); exit(2); }
            continue;
        }
        if(strncmp(argv[i], "--planner", 9) == 0)
        {
            const char *a = argv[i];

            planner_flag_given = 1;
            if(strncmp(a, "--planner=", 10) == 0)
            {
                char buf[64];
                char *tok;
                unsigned mask = 0;

                strncpy(buf, a + 10, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
                if(strcmp(buf, "all") == 0)
                    mask = PLANNER_LAYERS_ALL;
                else if(strcmp(buf, "none") == 0)
                    mask = 0;   /* evidence and calibration only (--planner-calib); the fixed gates decide */
                else
                    for(tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ","))
                    {
                        if(strcmp(tok, "team") == 0) mask |= PLANNER_LAYER_TEAM;
                        else if(strcmp(tok, "tasks") == 0) mask |= PLANNER_LAYER_TASKS;
                        else if(strcmp(tok, "preempt") == 0) mask |= PLANNER_LAYER_PREEMPT;
                        else { fprintf(stderr, "invalid --planner '%s' (need all or a list of team,tasks,preempt)\n", a + 10); exit(2); }
                    }
                g_planner.layers = (uint8_t)mask;
            }
            else if(strncmp(a, "--planner-approval=", 19) == 0)
            {
                if(strcmp(a + 19, "deny") == 0) g_planner.approval = 0;
                else if(strcmp(a + 19, "auto") == 0) g_planner.approval = 1;
                else { fprintf(stderr, "invalid --planner-approval '%s' (need deny or auto)\n", a + 19); exit(2); }
            }
            else if(strncmp(a, "--planner-p=", 12) == 0)
            {
                if(strcmp(a + 12, "ai") == 0) g_planner.p_source = PLANNER_P_AI;
                else if(strcmp(a + 12, "watchdog") == 0) g_planner.p_source = PLANNER_P_WATCHDOG;
                else if(strcmp(a + 12, "oracle") == 0) g_planner.p_source = PLANNER_P_ORACLE;
                else if(strcmp(a + 12, "hazard") == 0) g_planner.p_source = PLANNER_P_HAZARD;
                else if(strcmp(a + 12, "both") == 0) g_planner.p_source = PLANNER_P_BOTH;
                else { fprintf(stderr, "invalid --planner-p '%s' (need ai, watchdog, oracle, hazard or both)\n", a + 12); exit(2); }
            }
            else if(strncmp(a, "--planner-prevent=", 18) == 0)
            {
                if(strcmp(a + 18, "none") == 0) g_planner.prevent = 0;
                else if(strcmp(a + 18, "rest") == 0) g_planner.prevent = PLANNER_PREVENT_REST;
                else if(strcmp(a + 18, "field") == 0) g_planner.prevent = PLANNER_PREVENT_FIELD;
                else if(strcmp(a + 18, "both") == 0) g_planner.prevent = PLANNER_PREVENT_REST | PLANNER_PREVENT_FIELD;
                else { fprintf(stderr, "invalid --planner-prevent '%s' (need none, rest, field or both)\n", a + 18); exit(2); }
            }
            else if(strncmp(a, "--planner-v=", 12) == 0)
            {
                char *end;
                double x = strtod(a + 12, &end);
                if(*end != 0 || !(x > 0.0 && x <= 100.0)) { fprintf(stderr, "invalid --planner-v '%s' (need a multiplier > 0)\n", a + 12); exit(2); }
                g_planner.v_mult = (float)x;
            }
            else if(strncmp(a, "--planner-margin=", 17) == 0)
            {
                char *end;
                double x = strtod(a + 17, &end);
                if(*end != 0 || !(x >= 0.0 && x <= 10.0)) { fprintf(stderr, "invalid --planner-margin '%s' (need 0-10)\n", a + 17); exit(2); }
                g_planner.margin_frac = (float)x;
            }
            else if(strncmp(a, "--planner-set=", 14) == 0)
            {
                char buf[160];
                char *tok;

                strncpy(buf, a + 14, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
                for(tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ","))
                {
                    char *eq = strchr(tok, '=');
                    char *end;
                    double x;
                    float *dst = NULL;

                    if(eq == NULL) { fprintf(stderr, "invalid --planner-set item '%s' (need name=value)\n", tok); exit(2); }
                    *eq = 0;
                    x = strtod(eq + 1, &end);
                    if(*end != 0 || !(x >= 0.0)) { fprintf(stderr, "invalid --planner-set value for '%s'\n", tok); exit(2); }
                    if(strcmp(tok, "reassign") == 0) dst = &g_planner.reassign_delay;
                    else if(strcmp(tok, "travel") == 0) dst = &g_planner.ref_travel_ticks;
                    else if(strcmp(tok, "stage") == 0) dst = &g_planner.ref_stage_ticks;
                    else if(strcmp(tok, "window") == 0) dst = &g_planner.rate_window_ticks;
                    else if(strcmp(tok, "detour") == 0) dst = &g_planner.blocked_detour;
                    else if(strcmp(tok, "cblind") == 0) dst = &g_planner.cblind;
                    else if(strcmp(tok, "repair_mob") == 0) dst = &g_planner.repair_mobility;
                    else if(strcmp(tok, "repair_res") == 0) dst = &g_planner.repair_resource;
                    else if(strcmp(tok, "repair_comm") == 0) dst = &g_planner.repair_comm;
                    else if(strcmp(tok, "repair") == 0) dst = &g_planner.repair_scale;
                    else if(strcmp(tok, "crossing") == 0) dst = &g_planner.crossing_scale;
                    else if(strcmp(tok, "nref") == 0) dst = &g_planner.n_ref_victims;
                    else if(strcmp(tok, "ageref") == 0) dst = &g_planner.age_ref_ticks;
                    else { fprintf(stderr, "unknown --planner-set name '%s'\n", tok); exit(2); }
                    *dst = (float)x;
                }
            }
            else if(strcmp(a, "--planner-log") == 0)
                g_planner.log_enabled = 1;
            else if(strcmp(a, "--planner-calib") == 0)
                g_planner.calib_dump = 1;
            else
            {
                fprintf(stderr, "unknown flag %s\n", a);
                exit(2);
            }
            continue;
        }

        /* Step 3.2 plumbing fixes, causal only: on | off | label,rtb,cost */
        if(strncmp(argv[i], "--causal-fixes=", 15) == 0)
        {
            const char *v = argv[i] + 15;
            char buf[64];
            char *tok;
            unsigned mask = 0;

            if(strcmp(v, "on") == 0)
                mask = CAUSAL_FIXES_ALL;
            else if(strcmp(v, "off") != 0)
            {
                strncpy(buf, v, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
                for(tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ","))
                {
                    if(strcmp(tok, "label") == 0)
                        mask |= CAUSAL_FIX_LABEL;
                    else if(strcmp(tok, "rtb") == 0)
                        mask |= CAUSAL_FIX_ENV_NO_RTB;
                    else if(strcmp(tok, "cost") == 0)
                        mask |= CAUSAL_FIX_TRUE_COST;
                    else if(strcmp(tok, "stall") == 0)
                        mask |= CAUSAL_FIX_STALL;
                    else if(strcmp(tok, "semantics") == 0)
                        mask |= CAUSAL_FIX_SEMANTICS;
                    else if(strcmp(tok, "hold-repair") == 0)
                        mask |= CAUSAL_FIX_HOLD_REPAIR;
                    else
                    {
                        fprintf(stderr, "invalid --causal-fixes '%s' (need on, off, or a list of label,rtb,cost,stall,semantics,hold-repair)\n", v);
                        exit(2);
                    }
                }
            }
            causal_fixes_mask = (int)mask;
            continue;
        }

        /* Persistent faults (Step 3.6), causal only: off | timer | on. */
        if(strncmp(argv[i], "--causal-persist=", 17) == 0)
        {
            const char *v = argv[i] + 17;

            if(strcmp(v, "off") == 0) causal_persist_mode = (int)CAUSAL_PERSIST_OFF;
            else if(strcmp(v, "timer") == 0) causal_persist_mode = (int)CAUSAL_PERSIST_TIMER;
            else if(strcmp(v, "on") == 0) causal_persist_mode = (int)CAUSAL_PERSIST_ON;
            else { fprintf(stderr, "invalid --causal-persist '%s' (need off, timer or on)\n", v); exit(2); }
            continue;
        }

        /* Robustness runs (Step 3.6): override persistence parameters, never used to choose a value.
         * --causal-persist-set=natural=<ticks>,field_scale=<x>,success=<p>,esc=<x>,dwell_field=1 */
        if(strncmp(argv[i], "--causal-persist-set=", 21) == 0)
        {
            char buf[160];
            char *tok;

            strncpy(buf, argv[i] + 21, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = 0;
            for(tok = strtok(buf, ","); tok != NULL; tok = strtok(NULL, ","))
            {
                char *eq = strchr(tok, '=');
                float v;

                if(eq == NULL) { fprintf(stderr, "invalid --causal-persist-set token '%s'\n", tok); exit(2); }
                *eq = 0;
                v = (float)atof(eq + 1);
                if(strcmp(tok, "natural") == 0) g_causal_persist_params.natural_mean_ticks = v;
                else if(strcmp(tok, "field_scale") == 0)
                {
                    g_causal_persist_params.field_min_ticks = (uint16_t)(34.0f * v + 0.5f);
                    g_causal_persist_params.field_max_ticks = (uint16_t)(102.0f * v + 0.5f);
                }
                else if(strcmp(tok, "success") == 0) g_causal_persist_params.field_success = v;
                else if(strcmp(tok, "esc") == 0) g_causal_persist_params.escalation_mult = 1.5f * v;
                else if(strcmp(tok, "dwell_field") == 0 && v != 0.0f)
                {
                    g_causal_persist_params.base_dwell_min_ticks = g_causal_persist_params.field_min_ticks;
                    g_causal_persist_params.base_dwell_max_ticks = g_causal_persist_params.field_max_ticks;
                }
                else { fprintf(stderr, "invalid --causal-persist-set key '%s'\n", tok); exit(2); }
            }
            continue;
        }

        /* Consequential roads (Step 3.1), causal only. */
        if(strncmp(argv[i], "--causal-roads=", 15) == 0)
        {
            const char *v = argv[i] + 15;

            if(strcmp(v, "on") == 0)
                g_causal_roads.enabled = 1;
            else if(strcmp(v, "off") == 0)
                g_causal_roads.enabled = 0;
            else
            {
                fprintf(stderr, "invalid --causal-roads '%s' (need on or off)\n", v);
                exit(2);
            }
            roads_flag_given = 1;
            continue;
        }

        if(strncmp(argv[i], "--roads-", 8) == 0)
        {
            const char *a = argv[i] + 8;
            unsigned u1 = 0, u2 = 0;
            int ok = 0;

            if(strncmp(a, "partial-fraction=", 17) == 0 && sscanf(a + 17, "%u", &u1) == 1 && u1 <= 100)
            {
                g_causal_roads.partial_pct = (uint8_t)u1; ok = 1;
            }
            else if(strncmp(a, "delay=", 6) == 0 && sscanf(a + 6, "%u", &u1) == 1 && u1 >= 1 && u1 <= 20)
            {
                g_causal_roads.crossing_delay_ticks = (uint8_t)u1; ok = 1;
            }
            else if(strncmp(a, "persist=", 8) == 0 && sscanf(a + 8, "%u-%u", &u1, &u2) == 2 && u1 >= 1 && u2 >= u1 && u2 <= 65535)
            {
                g_causal_roads.persist_min_ticks = (uint16_t)u1;
                g_causal_roads.persist_max_ticks = (uint16_t)u2; ok = 1;
            }
            else if(strncmp(a, "clear-work=", 11) == 0 && sscanf(a + 11, "%u-%u", &u1, &u2) == 2 && u1 >= 1 && u2 >= 1 && u1 <= 65535 && u2 <= 65535)
            {
                g_causal_roads.clear_work_partial = (uint16_t)u1;
                g_causal_roads.clear_work_full = (uint16_t)u2; ok = 1;
            }
            else if(strncmp(a, "cost-per-tick=", 14) == 0 && sscanf(a + 14, "%u", &u1) == 1 && u1 >= 1 && u1 <= 50)
            {
                g_causal_roads.cost_per_tick = (uint8_t)u1; ok = 1;
            }
            else if(strncmp(a, "astar=", 6) == 0 && (strcmp(a + 6, "true") == 0 || strcmp(a + 6, "blind") == 0))
            {
                g_causal_roads.route_cost_visible = strcmp(a + 6, "true") == 0 ? 1u : 0u; ok = 1;
            }
            if(!ok)
            {
                fprintf(stderr, "invalid %s (see docs/CONSEQUENTIAL_ROADS.md for the --roads-* flags)\n", argv[i]);
                exit(2);
            }
            roads_flag_given = 1;
            continue;
        }

        if(strcmp(argv[i], "--link-margin-map") == 0)
        {
            link_margin_map = 1;
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

    if(causal_actions_given && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--causal-actions only applies to --fault-profile=causal\n");
        return 2;
    }
    if(causal_env_max != 0 && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--causal-env-duration only applies to --fault-profile=causal\n");
        return 2;
    }
    if(causal_env_delay != 0 && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--causal-env-crossing-delay only applies to --fault-profile=causal\n");
        return 2;
    }
    if(causal_fixes_mask >= 0 && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--causal-fixes only applies to --fault-profile=causal\n");
        return 2;
    }
    if(causal_fixes_mask >= 0)
        fault_config_set_causal_fixes((uint8_t)causal_fixes_mask);
    if(causal_persist_mode >= 0 && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--causal-persist only applies to --fault-profile=causal\n");
        return 2;
    }
    if(causal_persist_mode >= 0)
        fault_config_set_causal_persist((uint8_t)causal_persist_mode);
    if(causal_fixes_mask >= 0 && (causal_fixes_mask & (int)CAUSAL_FIX_HOLD_REPAIR) && g_causal_persist != CAUSAL_PERSIST_ON)
    {
        fprintf(stderr, "--causal-fixes=...hold-repair needs --causal-persist=on (only that mode has field repair)\n");
        return 2;
    }
    if(g_probe.any && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--probe-* flags only apply to --fault-profile=causal\n");
        return 2;
    }
    if((g_probe.oracle_delay != 0u || g_probe.oracle_noise) && g_planner.p_source != PLANNER_P_ORACLE)
    {
        fprintf(stderr, "--probe-oracle-delay / -fn / -fp need --planner-p=oracle\n");
        return 2;
    }
    if(g_road_know.mode == 0 && (g_road_know.instant || g_road_know.radius != 1 || g_road_know.latency != 2))
    {
        fprintf(stderr, "--probe-road-* options need --probe-roadknow=obs|prior\n");
        return 2;
    }
    if(g_probe.no_comm_faults)
        consensus_set_link_loss(0.0f);   /* C2: perfect comms also means no vbus loss */
    if(g_planner.prevent != 0 &&
       g_planner.p_source != PLANNER_P_HAZARD && g_planner.p_source != PLANNER_P_BOTH)
    {
        fprintf(stderr, "--planner-prevent needs a forecast source: --planner-p=hazard or both\n");
        return 2;
    }
    if((g_planner.p_source == PLANNER_P_HAZARD || g_planner.p_source == PLANNER_P_BOTH) && fault_profile == FAULT_PROFILE_CAUSAL &&
       g_causal_persist == CAUSAL_PERSIST_OFF)
    {
        fprintf(stderr, "--planner-p=hazard|both needs --causal-persist=timer|on (the forecast is the persistent-fault model's)\n");
        return 2;
    }
    if(roads_flag_given && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--causal-roads / --roads-* only apply to --fault-profile=causal\n");
        return 2;
    }
    if(g_causal_roads.enabled && (causal_env_max != 0 || causal_env_delay != 0))
    {
        fprintf(stderr, "--causal-env-duration / --causal-env-crossing-delay are the Step 3 sensitivity overrides: use --causal-roads=off with them, or --roads-persist / --roads-delay\n");
        return 2;
    }
    fault_config_set_causal_env_duration(causal_env_min, causal_env_max);
    fault_config_set_causal_env_crossing_delay(causal_env_delay);
    if(causal_reroute_penalty >= 0)
    {
        if(fault_profile != FAULT_PROFILE_CAUSAL)
        {
            fprintf(stderr, "--causal-reroute-penalty only applies to --fault-profile=causal\n");
            return 2;
        }
        fault_config_set_causal_reroute_penalty((uint16_t)causal_reroute_penalty);
    }
    if(g_recovery_mode == RECOVERY_MODE_UTILITY && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--recovery=utility only applies to --fault-profile=causal\n");
        return 2;
    }
    if(planner_flag_given && g_recovery_mode != RECOVERY_MODE_UTILITY)
    {
        fprintf(stderr, "--planner* flags need --recovery=utility\n");
        return 2;
    }
    if(g_recovery_mode == RECOVERY_MODE_UTILITY && (g_planner.layers & PLANNER_LAYER_TASKS))
        causal_actions_flag = (int)CAUSAL_ACTIONS_ALL;   /* the planner decides when each action starts; the mechanics are Step 3's */
    fault_config_set_causal_actions((uint8_t)causal_actions_flag);
    if(fault_stress_given && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--fault-stress only applies to --fault-profile=causal\n");
        return 2;
    }
    if(link_margin_map && fault_profile != FAULT_PROFILE_CAUSAL)
    {
        fprintf(stderr, "--link-margin-map only applies to --fault-profile=causal\n");
        return 2;
    }
    fault_config_set_stress(fault_stress);
    fault_config_apply_profile(fault_profile);
    fault_injector_set_profile(fault_profile);
    if(fault_profile == FAULT_PROFILE_CAUSAL)
        fprintf(stderr, "[FAULT] profile=causal fault_stress=%g causal_actions=%s env_backstop=%u-%u env_crossing_delay=%u roads=%s\n", (double)fault_stress,
                causal_actions_flag == (int)CAUSAL_ACTIONS_ALL ? "on" : (causal_actions_flag ? "partial" : "off"), causal_env_max ? causal_env_min : 5u, causal_env_max ? causal_env_max : 12u, (unsigned)causal_env_delay,
                g_causal_roads.enabled ? "on" : "off");
    if(g_recovery_mode == RECOVERY_MODE_UTILITY)
        fprintf(stderr, "[FAULT] recovery=utility layers=%u p=%s approval=%s v_mult=%g margin=%g\n", (unsigned)g_planner.layers,
                g_planner.p_source == PLANNER_P_AI ? "ai" : (g_planner.p_source == PLANNER_P_WATCHDOG ? "watchdog" :
                    (g_planner.p_source == PLANNER_P_ORACLE ? "oracle" : (g_planner.p_source == PLANNER_P_HAZARD ? "hazard" : "both"))),
                g_planner.approval ? "auto" : "deny", (double)g_planner.v_mult, (double)g_planner.margin_frac);
    if(fault_profile == FAULT_PROFILE_CAUSAL)
        fprintf(stderr, "[FAULT] fixes label=%d env_no_rtb=%d true_cost=%d stall=%d semantics=%d\n",
                fault_profile_causal_fix(CAUSAL_FIX_LABEL), fault_profile_causal_fix(CAUSAL_FIX_ENV_NO_RTB), fault_profile_causal_fix(CAUSAL_FIX_TRUE_COST),
                fault_profile_causal_fix(CAUSAL_FIX_STALL), fault_profile_causal_fix(CAUSAL_FIX_SEMANTICS));
    if(fault_profile == FAULT_PROFILE_CAUSAL)
        fprintf(stderr, "[FAULT] persist=%s natural_mean=%g field=%u-%u success=%g dwell=%u-%u escalation=x%g prevent=%u\n",
                g_causal_persist == CAUSAL_PERSIST_ON ? "on" : (g_causal_persist == CAUSAL_PERSIST_TIMER ? "timer" : "off"),
                (double)g_causal_persist_params.natural_mean_ticks, (unsigned)g_causal_persist_params.field_min_ticks,
                (unsigned)g_causal_persist_params.field_max_ticks, (double)g_causal_persist_params.field_success,
                (unsigned)g_causal_persist_params.base_dwell_min_ticks, (unsigned)g_causal_persist_params.base_dwell_max_ticks,
                (double)g_causal_persist_params.escalation_mult, (unsigned)g_planner.prevent);
    if(fault_profile == FAULT_PROFILE_CAUSAL && g_causal_roads.enabled)
        fprintf(stderr, "[FAULT] roads partial_pct=%u delay=%u persist=%u-%u clear_work=%u-%u cost_per_tick=%u astar=%s\n",
                g_causal_roads.partial_pct, g_causal_roads.crossing_delay_ticks,
                g_causal_roads.persist_min_ticks, g_causal_roads.persist_max_ticks,
                g_causal_roads.clear_work_partial, g_causal_roads.clear_work_full,
                g_causal_roads.cost_per_tick, g_causal_roads.route_cost_visible ? "true" : "blind");
    (void)0;

    if(fast_forward_ticks > 0 && (!live_mode || mission_feed_mode))
    {
        fprintf(stderr, "--fast-forward requires --live (and not --mission-feed)\n");
        return 2;
    }

    uint32_t first_seed = positional_count >= 1 ? positional_args[0] : 1;
    uint32_t seed_count = positional_count >= 2 ? positional_args[1] : 1;
    uint32_t ticks_per_seed = positional_count >= 3 ? positional_args[2] : 10000;

    if(g_planner.log_enabled)
    {
        g_planner_log = fopen("tron_planner_log.csv", "w");
        if(g_planner_log != NULL)
        {
            fprintf(g_planner_log, "world_seed,tick,layer,team,chosen,alt,p_mobility,p_communication,p_resource,"
                                   "p_ai_mobility,p_ai_communication,p_ai_resource,cost_fault_or_benefit,cost_action,cost_alt,margin,"
                                   "flags,target_x,target_y,aux,aux_cost\n");
            planner_set_log_sink(planner_log_sink);
        }
    }

    /* Counter loop, not `seed < first_seed + seed_count`: that sum can
     * overflow uint32_t for large inputs and run zero (or wrong) seeds. */
    for(uint32_t seed_index = 0; seed_index < seed_count; ++seed_index)
    {
        uint32_t seed = first_seed + seed_index;

        g_planner_log_seed = seed;
        sim_start_seed(seed, fault_profile);

        if(link_margin_map)
        {
            /* Debug aid: print the causal link-margin and shadowing fields
             * for this seed's world, then stop. */
            for(int pass = 0; pass < 2; pass++)
            {
                printf("# seed=%u %s (dB), rows are y, columns are x\n", seed,
                       pass == 0 ? "link_margin" : "shadowing");
                for(int y = 0; y < GRID_HEIGHT; y++)
                {
                    for(int x = 0; x < GRID_WIDTH; x++)
                        printf("%s%.1f", x ? " " : "",
                               (double)(pass == 0 ? fault_injector_causal_link_margin_at(x, y)
                                                  : fault_injector_causal_shadow_at(x, y)));
                    printf("\n");
                }
            }
            return 0;
        }

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
        if(probe_record_path != NULL)
        {
            FILE *rf = fopen(probe_record_path, "w");

            if(rf != NULL)
            {
                for(uint32_t ei = 0; ei < g_probe_ev_seen_n; ei++)
                    fprintf(rf, "%u %u %d %d\n", g_probe_ev_seen[ei].tick, g_probe_ev_seen[ei].type,
                        g_probe_ev_seen[ei].x, g_probe_ev_seen[ei].y);
                fclose(rf);
            }
        }
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
            static const char *const action_names[NUM_CAUSAL_ACTIONS] =
                { "reroute", "clear_road", "reinforce_incident", "relay_reposition" };
            const CausalActionStats_t *as = causal_actions_stats();

            fprintf(stderr, "[DIAG ACTIONS] enabled_mask=%d crossing_stall_ticks=%u\n", g_causal_actions_enabled, causal_actions_stall_ticks());
            {
                const CausalRoadStats_t *rs = fault_injector_causal_road_stats();
                fprintf(stderr, "[DIAG ROADS] enabled=%d partial=%u blocked=%u block_downgraded=%u max_blocked_cells=%u audit_ticks=%u audit_violations=%u\n",
                    g_causal_roads.enabled, rs->partial_injected, rs->blocked_injected, rs->block_downgraded,
                    rs->max_blocked_cells, rs->audit_ticks, rs->audit_violations);
                fprintf(stderr, "[DIAG ENVLABEL] fix=%d label_ticks=%u label_episodes=%u\n",
                    fault_profile_causal_fix(CAUSAL_FIX_LABEL), rs->label_ticks, rs->label_episodes);
            }
            {
                const PlannerStats_t *ps = planner_stats();

                fprintf(stderr, "[DIAG PLANNER] mode=%s decisions=%u go_home_planner=%u go_home_floor=%u stay_with_p=%u ai_cap_blocks=%u "
                                "approvals_requested=%u approvals_granted=%u approvals_denied=%u go_home_events=%u go_home_near_stall=%u action_changes=%u reversals10=%u\n",
                        g_recovery_mode == RECOVERY_MODE_UTILITY ? "utility" : "fixed", ps->decisions, ps->go_home_planner, ps->go_home_floor,
                        ps->stay_with_p, ps->ai_cap_blocks, ps->approvals_requested, ps->approvals_granted, ps->approvals_denied,
                        ps->go_home_events, ps->go_home_near_stall, ps->action_changes, ps->reversals10);
                fprintf(stderr, "[DIAG PLANNER_P] fr_chosen=%u prevent_considered=%u prevent_rest=%u prevent_field=%u\n",
                        ps->fr_chosen, ps->prevent_considered, ps->prevent_rest, ps->prevent_field);
                fprintf(stderr, "[DIAG PLANNER_B] eval_wanted_reroute=%u eval_wanted_clear_road=%u eval_wanted_reinforce=%u eval_wanted_relay=%u "
                                "preempt_considered=%u preempt_clear_road=%u preempt_reinforce=%u blocked_cost=%u blocked_approval=%u blocked_finish=%u "
                                "preempt_net_sum=%.2f succeeded_clear=%u failed_clear=%u succeeded_reinforce=%u failed_reinforce=%u\n",
                        ps->wanted[0], ps->wanted[1], ps->wanted[2], ps->wanted[3], ps->preempt_considered, ps->preempt_done[CAUSAL_ACTION_CLEAR_ROAD],
                        ps->preempt_done[CAUSAL_ACTION_REINFORCE_INCIDENT], ps->preempt_blocked_cost, ps->preempt_blocked_approval,
                        ps->preempt_blocked_finish, (double)ps->preempt_net_sum_x100 / 100.0,
                        ps->preempt_succeeded[CAUSAL_ACTION_CLEAR_ROAD], ps->preempt_failed[CAUSAL_ACTION_CLEAR_ROAD],
                        ps->preempt_succeeded[CAUSAL_ACTION_REINFORCE_INCIDENT], ps->preempt_failed[CAUSAL_ACTION_REINFORCE_INCIDENT]);
            }
            if(g_planner.calib_dump)
            {
                const PlannerCalib_t *pc = planner_calib();

                for(int s = 0; s < 3; s++)
                    for(int c = 0; c < 3; c++)
                        for(int b = 0; b < 32; b++)
                        {
                            if(pc->n[s][c][b] != 0u)
                                fprintf(stderr, "[PLANNERCAL] kind=raw src=%d class=%d bin=%d n=%u pos=%u\n", s, c, b, pc->n[s][c][b], pc->pos[s][c][b]);
                            if(pc->cal_n[s][c][b] != 0u)
                                fprintf(stderr, "[PLANNERCAL] kind=cal src=%d class=%d bin=%d n=%u pos=%u sum=%.4f\n", s, c, b, pc->cal_n[s][c][b], pc->cal_pos[s][c][b], (double)pc->cal_sum[s][c][b]);
                        }
                for(int c = 0; c < 3; c++)
                    for(int b = 0; b < 32; b++)
                        if(pc->fused_n[c][b] != 0u)
                            fprintf(stderr, "[PLANNERCAL] kind=fused src=3 class=%d bin=%d n=%u pos=%u sum=%.4f\n", c, b, pc->fused_n[c][b], pc->fused_pos[c][b], (double)pc->fused_sum[c][b]);
            }
            for(uint8_t a = 0; a < NUM_CAUSAL_ACTIONS; a++)
                fprintf(stderr, "[DIAG ACTION] name=%s dispatched=%u resolved=%u abandoned=%u active_ticks=%u from_detector=%u\n",
                    action_names[a], as->dispatched[a], as->resolved[a], as->abandoned[a],
                    as->active_ticks[a], as->from_detector[a]);
        }
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
            for(uint8_t c = 0; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
                fprintf(stderr, "[DIAG FAULTEND] class=%u cause_ticks=%u cause_planned_ticks=%u\n",
                    c, cs->ticks_by_cause[c], cs->planned_by_cause[c]);
            if(fault_profile_causal_persist())
            {
                const CausalPersistStats_t *ps = fault_injector_causal_persist_stats();
                extern uint32_t g_diag_exclusions[3];
                for(uint8_t c = 0; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
                    fprintf(stderr, "[DIAG PERSIST] class=%u escalations=%u field_ticks_total=%u\n",
                        c, ps->escalations[c], ps->ticks_by_field[c]);
                for(uint8_t k = 0; k < 3u; k++)
                    fprintf(stderr, "[DIAG FIELD] kind=%u started=%u ok=%u failed=%u cancelled=%u\n",
                        k, ps->field_started[k], ps->field_ok[k], ps->field_failed[k], ps->field_cancelled[k]);
                if(fault_profile_causal_fix(CAUSAL_FIX_HOLD_REPAIR))
                {
                    for(uint8_t k = 0; k < 2u; k++)
                        fprintf(stderr, "[DIAG HOLDCANCEL] kind=%u stamina=%u other_class=%u planner=%u released=%u fault_ended=%u other=%u\n",
                            k, ps->cancel_cause[k][0], ps->cancel_cause[k][1], ps->cancel_cause[k][2],
                            ps->cancel_cause[k][3], ps->cancel_cause[k][4], ps->cancel_cause[k][5]);
                    fprintf(stderr, "[DIAG HOLD] capped_return=%u capped_realloc=%u capped_abort=%u lifted_stamina=%u lifted_other=%u h2_refused=%u h3_resets=%u\n",
                        ps->hold_capped[0], ps->hold_capped[1], ps->hold_capped[2], ps->hold_lifted[0], ps->hold_lifted[1],
                        ps->hold_h2_refused, ps->hold_h3_resets);
                }
                fprintf(stderr, "[DIAG PREVENT] held_ticks=%u prevent_orders=%u prevent_done=%u abort_releases=%u realloc_releases=%u exclusion_refusals=%u\n",
                    ps->field_ticks, ps->prevent_orders, ps->prevent_done, g_diag_exclusions[0], g_diag_exclusions[1], g_diag_exclusions[2]);
            }
        }

        if(g_road_know.mode != 0)
        {
            const RoadKnowledgeStats_t *rk = road_knowledge_stats();
            extern unsigned long g_probe_no_path_hits;
            fprintf(stderr, "[DIAG ROADUSE] no_path_hits=%lu stall_ticks=%u\n", g_probe_no_path_hits, causal_actions_stall_ticks());
            fprintf(stderr, "[DIAG ROADKNOW] mode=%u damage_episodes=%u known_episodes=%u known_within_2=%u known_delay_sum=%llu known_delay_max=%u reports=%u lost=%u unknown_partial_enter=%u blocked_stops=%u stale_cell_ticks=%u prior_add_max=%u\n",
                g_road_know.mode, rk->damage_episodes, rk->known_episodes, rk->known_within_2, (unsigned long long)rk->known_delay_sum,
                rk->known_delay_max, rk->reports_made, rk->reports_lost, rk->unknown_partial_enter, rk->blocked_stops,
                rk->stale_cell_ticks, rk->prior_add_max);
        }
        if(g_probe.diag)
        {
            fprintf(stderr, "[DIAG PRIO] seen=%u,%u,%u,%u lost=%u,%u,%u,%u\n",
                g_probe_stats.seen_by_prio[1], g_probe_stats.seen_by_prio[2], g_probe_stats.seen_by_prio[3], g_probe_stats.seen_by_prio[4],
                g_probe_stats.lost_by_prio[1], g_probe_stats.lost_by_prio[2], g_probe_stats.lost_by_prio[3], g_probe_stats.lost_by_prio[4]);
            for(uint8_t sp = 0; sp < 5u; sp++)
                fprintf(stderr, "[DIAG TB] spec=%u resting=%u to_rest=%u held=%u faulted=%u on_scene=%u to_task=%u available=%u\n", sp,
                    g_probe_stats.tb[sp][0], g_probe_stats.tb[sp][1], g_probe_stats.tb[sp][2], g_probe_stats.tb[sp][3],
                    g_probe_stats.tb[sp][4], g_probe_stats.tb[sp][5], g_probe_stats.tb[sp][6]);
            for(uint32_t ii = 0; ii < g_probe_inc_log_n; ii++)
                fprintf(stderr, "[DIAG INC] type=%u prio=%u reported=%u arrived=%u resolved=%u\n", g_probe_inc_log[ii].type,
                    g_probe_inc_log[ii].prio, g_probe_inc_log[ii].reported, g_probe_inc_log[ii].arrived, g_probe_inc_log[ii].resolved);
        }
        if(g_probe.any)
            fprintf(stderr, "[DIAG PROBE] oracle_misses=%u oracle_false_alarms=%u dropout_team_ticks=%u events_seen=%u events_planned=%u event_mismatch=%u\n",
                g_probe_stats.oracle_misses, g_probe_stats.oracle_false_alarms, g_probe_stats.dropout_team_ticks,
                g_probe_ev_seen_n, g_probe_ev_plan_n, g_probe_ev_mismatch);

        extern uint32_t g_diag_discovered_by_type[NUM_INCIDENT_TYPES];
        extern uint32_t g_diag_resolved_by_type[NUM_INCIDENT_TYPES];
        extern uint32_t g_diag_spawned_by_type[NUM_INCIDENT_TYPES];
        for(uint8_t ty = 0; ty < (g_fault_config.profile == FAULT_PROFILE_CAUSAL ? NUM_INCIDENT_TYPES : INCIDENT_CLEAR_ROAD); ty++)
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
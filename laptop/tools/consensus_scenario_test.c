/* Whole-simulation scenario tests for peer consensus (CONSENSUS_DESIGN.md 7.1
 * to 7.5, 7.11) and the measurement modes behind the 7.2 table and the D1
 * comparison. Build: make check-consensus-scenarios
 *
 *   consensus_scenario_test assert [quick]
 *   consensus_scenario_test table  --model M --loss Q [--seeds A:N] [--ticks T]
 *   consensus_scenario_test compare [--seeds A:N] [--ticks T] [--loss Q]
 *   consensus_scenario_test airtime
 *
 * Ground truth: this harness reads team->fault_state to LABEL and SCORE. It is
 * the only consensus-related code that does. Nothing under core/ consults it
 * except the physics (move_team, update_team, team_effective_work_rate).
 * Built with CONSENSUS_TEST_HOOKS=1; the variant flags (CONSENSUS_FAST_PATH,
 * TEAM_TEST_RANDOM_EMISSION, CONSENSUS_ENFORCE, CONSENSUS_CAP_LOCAL_EVALUATORS)
 * are compile-time and chosen by the Makefile or the sweep driver. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "consensus.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "mission_state.h"
#include "recovery_manager.h"
#include "sim_runner.h"
#include "simulation.h"
#include "team.h"
#include "vbus.h"
#include "watchdog.h"

#if !VIRTUAL_BUS_ENABLED || !CONSENSUS_TEST_HOOKS
#error "consensus_scenario_test needs -DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_TEST_HOOKS=1"
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECKF(c, ...) do { if (!(c)) { printf("FAIL %s:%d: %s: ", __FILE__, __LINE__, #c); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

/* ------------------------------------------------------------------ */
/* World control                                                      */
/* ------------------------------------------------------------------ */

typedef enum { LM_IID, LM_SRC, LM_GE3, LM_GE10, LM_BURST } LossKind_t;

static const char *loss_kind_name(LossKind_t k)
{
    switch(k)
    {
        case LM_IID: return "iid";
        case LM_SRC: return "per-source";
        case LM_GE3: return "GE-burst3";
        case LM_GE10: return "GE-burst10";
        default: return "shared-burst";
    }
}

static LossKind_t loss_kind_from(const char *s)
{
    if(!strcmp(s, "src")) return LM_SRC;
    if(!strcmp(s, "ge3")) return LM_GE3;
    if(!strcmp(s, "ge10")) return LM_GE10;
    if(!strcmp(s, "burst")) return LM_BURST;
    return LM_IID;
}

/* q is the long-run average per-link loss for every model. The bursty models
   lose everything in the bad state and are tuned so the bad-state share is q. */
static void set_bus(LossKind_t kind, float q)
{
    ConsensusBusConfig_t cfg;
    float mean_burst = (kind == LM_GE10) ? 10.0f : 3.0f;
    float p_bg = 1.0f / mean_burst;
    float p_gb = (q >= 1.0f) ? 1.0f : q * p_bg / (1.0f - q);

    memset(&cfg, 0, sizeof(cfg));
    switch(kind)
    {
        case LM_SRC:
            cfg.loss_model = (uint8_t)VBUS_LOSS_PER_SOURCE;
            cfg.link_loss = q;
            break;
        case LM_GE3:
        case LM_GE10:
            cfg.loss_model = (uint8_t)VBUS_LOSS_GILBERT_ELLIOTT;
            cfg.link_loss = 0.0f;
            cfg.p_gb = p_gb;
            cfg.p_bg = p_bg;
            cfg.loss_bad = 1.0f;
            break;
        case LM_BURST:
            cfg.loss_model = (uint8_t)VBUS_LOSS_SHARED_BURST;
            cfg.link_loss = 0.0f;
            cfg.p_gb = p_gb;
            cfg.p_bg = p_bg;
            break;
        case LM_IID:
        default:
            cfg.loss_model = (uint8_t)VBUS_LOSS_IID;
            cfg.link_loss = q;
            break;
    }
    consensus_configure_bus(&cfg);
}

static void world_begin(uint32_t seed, FaultProfile_t profile)
{
    fault_config_apply_profile(profile);
    fault_injector_set_profile(profile);
    generate_world(seed);       /* also consensus_init(seed) */
}

#define NCLS_SCRIPT 2

/* ------------------------------------------------------------------ */
/* Scripted fault injector (harness only)                             */
/* ------------------------------------------------------------------ */

/* The built-in profiles keep a fault active for its duration plus the
 * 33-tick repair confirmation (fault_injector.c), and roll 8 to 14 % per
 * team per tick, so --fault-profile=communication has most teams faulted at
 * once. The design's fault budget is f = 2 faulty peers out of 9, so the
 * scripted injector produces the regime the design assumes: isolated faults
 * of a chosen length, with a cap on how many are active together. It runs
 * with FAULT_PROFILE_NONE (the real injector is off) and writes the same
 * TeamFaultState_t fields the injector does, which only physics reads. */
enum { SCRIPT_COMM = 1, SCRIPT_MOB = 2 };

typedef struct
{
    uint8_t mask, max_conc;
    uint16_t exclude;       /* teams the script never faults */
    uint8_t long_dur;       /* 38..49 ticks: a built-in fault, 5-16 plus the 33-tick repair confirmation */
    uint32_t rng;
    uint32_t end_tick[NUM_TEAMS][NCLS_SCRIPT];
} Script_t;

static Script_t g_script;

static uint32_t script_rand(void)
{
    uint32_t x = g_script.rng;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_script.rng = x;
    return x;
}

static uint8_t g_script_long;
static uint16_t g_script_exclude;     /* set by a test before run_score()/script_begin() */

static void script_begin(uint32_t seed, uint8_t mask, uint8_t max_conc)
{
    memset(&g_script, 0, sizeof(g_script));
    g_script.exclude = g_script_exclude;
    g_script.long_dur = g_script_long;
    g_script.mask = mask;
    g_script.max_conc = max_conc;
    g_script.rng = seed * 2654435761u + 0x1234567u;
    if(g_script.rng == 0u) g_script.rng = 1u;
}

static void script_set_fault(uint8_t t, uint8_t cls, uint8_t sev, uint32_t dur)
{
    TeamFaultState_t *f = &g_teams[t].fault_state;
    uint8_t fc = (cls == 0) ? FAULT_CLASS_MOBILITY : FAULT_CLASS_COMMUNICATION;

    f->active[fc] = 1;
    f->start_tick[fc] = g_current_tick;
    f->duration_ticks[fc] = dur;
    f->recovery_tick[fc] = g_current_tick + dur;
    f->severity[fc] = sev;
    f->target_team[fc] = t;
    f->active_mask |= (1u << fc);
    g_script.end_tick[t][cls] = g_current_tick + dur;
}

static void script_clear_fault(uint8_t t, uint8_t cls)
{
    TeamFaultState_t *f = &g_teams[t].fault_state;
    uint8_t fc = (cls == 0) ? FAULT_CLASS_MOBILITY : FAULT_CLASS_COMMUNICATION;

    f->active[fc] = 0;
    f->start_tick[fc] = 0;
    f->duration_ticks[fc] = 0;
    f->recovery_tick[fc] = 0;
    f->severity[fc] = 0;
    f->active_mask &= ~(1u << fc);
    g_script.end_tick[t][cls] = 0;
}

/* Call before every sim_tick(): clears faults that are over, maybe starts one. */
static void script_step(void)
{
    uint8_t conc[NCLS_SCRIPT] = { 0, 0 };

    if(g_script.mask == 0u)
        return;
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        for(uint8_t c = 0; c < NCLS_SCRIPT; c++)
        {
            if(g_script.end_tick[t][c] != 0u && g_current_tick >= g_script.end_tick[t][c])
                script_clear_fault(t, c);
            if(g_script.end_tick[t][c] != 0u)
                conc[c]++;
        }
    }
    for(uint8_t c = 0; c < NCLS_SCRIPT; c++)
    {
        uint8_t bit = (c == 0) ? SCRIPT_MOB : SCRIPT_COMM;

        if(!(g_script.mask & bit) || conc[c] >= g_script.max_conc)
            continue;
        if((script_rand() % 100u) >= 8u)        /* about one new fault per 12 ticks when room */
            continue;
        {
            uint8_t t = (uint8_t)(script_rand() % NUM_TEAMS);
            uint32_t dur = g_script.long_dur ? 38u + script_rand() % 12u    /* 38..49 ticks */
                                             : 6u + script_rand() % 11u;    /* 6..16 ticks */
            uint8_t sev = (c == 0) ? 3u : (uint8_t)(2u + script_rand() % 2u);   /* comm: 2 or 3 */

            if(g_script.end_tick[t][c] == 0u && !(g_script.exclude & (1u << t)))
                script_set_fault(t, c, sev, dur);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Ground-truth scoring (harness only)                                */
/* ------------------------------------------------------------------ */

#define POST_WINDOW 3u          /* detection counts up to this many ticks after a fault ends */
#define LINGER_TICKS 12u        /* ELEVATED this long after a fault is stale, not false: a
                                   6-tick window, a delivery tick, a 2-tick round and a hold */
#define NCLS CONSENSUS_NUM_VOTED_CLASSES

static const uint8_t k_fault_class[NCLS] = { FAULT_CLASS_MOBILITY, FAULT_CLASS_COMMUNICATION };

typedef struct
{
    uint8_t open;
    uint32_t start, last_active;
    int first_elev;         /* ticks after start, -1 = none */
    uint8_t first_via_fast;
    uint8_t sev;
    uint32_t voters_mask;   /* peers that voted FAULT on this class during the episode */
    uint8_t max_silent_peers;   /* most OTHER teams with an active comm fault at once */
    uint32_t en_route;          /* ticks the target spent EN_ROUTE during the episode */
} Episode_t;

typedef struct
{
    uint32_t dur;
    int first_elev;
    uint8_t via_fast;
    uint8_t sev;
    uint8_t voters;
    uint8_t max_silent_peers;
    uint32_t en_route;
} EpisodeResult_t;

#define MAX_EP 4096
typedef struct
{
    EpisodeResult_t ep[NCLS][MAX_EP];
    uint32_t n_ep[NCLS];
    uint64_t target_rounds;
    uint64_t false_vote[NCLS], false_fast[NCLS];   /* per target-round */
    uint64_t elevated_ticks[NCLS];
    uint64_t fault_verdicts[NCLS];                  /* observer-target-ticks */
    uint64_t plaus_fail, self_deaf_ticks, graded_ticks;
    uint64_t healthy_stall_obs;                     /* observer stall with no mobility fault */
    uint64_t healthy_stall_wd;
    uint64_t mob_fault_votes_in_comm_only;          /* mobility FAULT vote while no mobility fault */
    uint64_t ticks;
    uint64_t conc_hist[NUM_TEAMS + 1];              /* ticks with k comm-faulted teams */
    uint64_t conc_any_hist[NUM_TEAMS + 1];          /* ticks with k teams having any fault active */
    uint64_t conc_mob_hist[NUM_TEAMS + 1];
    uint64_t rounds_b[2];                           /* target-rounds by silent peers: <= F, > F */
    uint64_t false_b[NCLS][2];
} Score_t;

static Episode_t g_epi[NUM_TEAMS][NCLS];

static uint8_t truth_active(uint8_t t, uint8_t c)
{
    return g_teams[t].fault_state.active[k_fault_class[c]];
}

static void score_reset_world(void)
{
    memset(g_epi, 0, sizeof(g_epi));
}

/* Called after every sim_tick(). observe_pairs: also walk the 90 pair views. */
static void score_tick(Score_t *s, uint8_t observe_pairs, uint32_t tick)
{
    uint8_t n_comm_active = 0;

    s->ticks++;
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
        n_comm_active = (uint8_t)(n_comm_active + truth_active(t, CONSENSUS_CLASS_COMMUNICATION));
    s->conc_hist[n_comm_active]++;
    {
        uint8_t n_any = 0, n_mob = 0;

        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            n_any = (uint8_t)(n_any + (g_teams[t].fault_state.active_mask != 0u));
            n_mob = (uint8_t)(n_mob + truth_active(t, CONSENSUS_CLASS_MOBILITY));
        }
        s->conc_any_hist[n_any]++;
        s->conc_mob_hist[n_mob]++;
    }

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        for(uint8_t c = 0; c < NCLS; c++)
        {
            ConsensusResult_t r = consensus_get_result(t, (ConsensusClass_t)c);
            Episode_t *e = &g_epi[t][c];
            uint8_t active = truth_active(t, c);
            uint8_t elevated = (r.tier == CONSENSUS_TIER_ELEVATED);
            uint8_t near_fault = active || (e->open && tick <= e->last_active + POST_WINDOW);
            uint8_t lingering = active || (e->open && tick <= e->last_active + LINGER_TICKS);
            uint8_t silent_peers = (uint8_t)(n_comm_active -
                truth_active(t, CONSENSUS_CLASS_COMMUNICATION));

            if(active)
            {
                if(!e->open)
                {
                    memset(e, 0, sizeof(*e));
                    e->open = 1u;
                    e->start = g_teams[t].fault_state.start_tick[k_fault_class[c]];
                    e->first_elev = -1;
                    e->sev = g_teams[t].fault_state.severity[k_fault_class[c]];
                }
                e->last_active = tick;
            }
            if(e->open && silent_peers > e->max_silent_peers)
                e->max_silent_peers = silent_peers;
            if(e->open && g_teams[t].status == STATUS_EN_ROUTE)
                e->en_route++;
            {
                uint8_t b = (uint8_t)(silent_peers > CONSENSUS_F);

                if((tick & 1u) && c == 0)
                    s->rounds_b[b]++;
                if((tick & 1u) && elevated && !lingering)
                    s->false_b[c][b]++;
            }

            if(elevated)
            {
                s->elevated_ticks[c]++;
                if(e->open && e->first_elev < 0 && near_fault)
                {
                    e->first_elev = (int)tick - (int)e->start;
                    e->first_via_fast = r.fast_path;
                }
            }

            if((tick & 1u) && elevated && !lingering)
            {
                if(r.fast_path)
                    s->false_fast[c]++;
                else
                    s->false_vote[c]++;
            }

            if(e->open && !active && tick > e->last_active + LINGER_TICKS)
            {
                if(s->n_ep[c] < MAX_EP)
                {
                    EpisodeResult_t *o = &s->ep[c][s->n_ep[c]++];

                    o->dur = e->last_active - e->start;      /* physical ticks: the first is start + 1 */
                    o->first_elev = e->first_elev;
                    o->via_fast = e->first_via_fast;
                    o->sev = e->sev;
                    o->max_silent_peers = e->max_silent_peers;
                    o->en_route = e->en_route;
                    o->voters = 0u;
                    for(uint8_t p = 0; p < NUM_TEAMS; p++)
                        if(e->voters_mask & (1u << p)) o->voters++;
                }
                e->open = 0u;
            }
        }
        if(tick & 1u)
            s->target_rounds++;
    }

    if(!observe_pairs)
        return;

    for(uint8_t o = 0; o < NUM_TEAMS; o++)
    {
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            PeerObservation_t p;
            uint8_t mob_active, comm_active;

            if(o == t) continue;
            consensus_get_peer_observation(o, t, &p);
            mob_active = truth_active(t, CONSENSUS_CLASS_MOBILITY);
            comm_active = truth_active(t, CONSENSUS_CLASS_COMMUNICATION);
            if(p.plaus_fail) s->plaus_fail++;
            if(p.observer_self_deaf && t == (uint8_t)((o + 1u) % NUM_TEAMS)) s->self_deaf_ticks++;
            if(p.observer_graded_abstain && t == (uint8_t)((o + 1u) % NUM_TEAMS)) s->graded_ticks++;
            for(uint8_t c = 0; c < NCLS; c++)
            {
                if(p.vote[c].verdict == PEER_VERDICT_FAULT)
                {
                    s->fault_verdicts[c]++;
                    if(g_epi[t][c].open)
                        g_epi[t][c].voters_mask |= (1u << o);
                }
            }
            if(p.vote[CONSENSUS_CLASS_MOBILITY].verdict == PEER_VERDICT_FAULT && !mob_active)
                s->mob_fault_votes_in_comm_only++;
            if(p.mob_stalls > 0u && !mob_active)
            {
                if(s->healthy_stall_obs < 5u)
                    printf("  healthy stall seen by observer %u about team %u at tick %u (leg pool %u slot %u)\n",
                        o, t, (unsigned)tick, p.rep_task_pool, p.rep_task_slot);
                s->healthy_stall_obs++;
            }
            (void)comm_active;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Runs                                                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint32_t first_seed, seed_count, ticks;
    FaultProfile_t profile;
    LossKind_t kind;
    float loss;
    uint8_t observe_pairs;
    uint8_t script_mask;    /* SCRIPT_COMM / SCRIPT_MOB: scripted faults instead of a profile */
    uint8_t script_max_conc;
    uint8_t script_long;
} RunSpec_t;

static void run_score(const RunSpec_t *spec, Score_t *s)
{
    memset(s, 0, sizeof(*s));
    for(uint32_t si = 0; si < spec->seed_count; si++)
    {
        set_bus(spec->kind, spec->loss);
        world_begin(spec->first_seed + si, spec->profile);
        score_reset_world();
        g_script_long = spec->script_long;
        script_begin(spec->first_seed + si, spec->script_mask, spec->script_max_conc);
        for(uint32_t i = 0; i < spec->ticks; i++)
        {
            script_step();
            sim_tick();
            score_tick(s, spec->observe_pairs, g_current_tick);
        }
    }
}

static double pct(uint64_t a, uint64_t b)
{
    return b ? 100.0 * (double)a / (double)b : 0.0;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* Detection over communication episodes >= min_dur ticks. */
static void detection_f(const Score_t *s, uint8_t cls, uint32_t min_dur, uint32_t max_peers,
    uint32_t *n, uint32_t *det, uint32_t *by4, int *lat_med, int *lat_p90, uint32_t *via_fast)
{
    int lat[MAX_EP];
    uint32_t nl = 0u;

    *n = *det = *by4 = *via_fast = 0u;
    for(uint32_t i = 0; i < s->n_ep[cls]; i++)
    {
        const EpisodeResult_t *e = &s->ep[cls][i];

        if(e->dur < min_dur || e->max_silent_peers > max_peers) continue;
        (*n)++;
        if(e->first_elev >= 0)
        {
            (*det)++;
            lat[nl++] = e->first_elev;
            if(e->first_elev <= 4) (*by4)++;
            if(e->via_fast) (*via_fast)++;
        }
    }
    *lat_med = *lat_p90 = -1;
    if(nl)
    {
        qsort(lat, nl, sizeof(int), cmp_int);
        *lat_med = lat[nl / 2];
        *lat_p90 = lat[(nl * 9u) / 10u];
    }
}

static void detection(const Score_t *s, uint8_t cls, uint32_t min_dur, uint32_t *n,
    uint32_t *det, uint32_t *by4, int *lat_med, int *lat_p90, uint32_t *via_fast)
{
    detection_f(s, cls, min_dur, 0xFFu, n, det, by4, lat_med, lat_p90, via_fast);
}

/* Faults seen while at most CONSENSUS_F other teams also had a communication
   fault: inside the fault budget the design assumes. */
static void detection_in_budget(const Score_t *s, uint8_t cls, uint32_t min_dur, uint32_t *n,
    uint32_t *det, uint32_t *by4, int *lat_med, int *lat_p90, uint32_t *via_fast)
{
    detection_f(s, cls, min_dur, CONSENSUS_F, n, det, by4, lat_med, lat_p90, via_fast);
}

/* ------------------------------------------------------------------ */
/* table mode                                                         */
/* ------------------------------------------------------------------ */

static void do_table(LossKind_t kind, float q, uint32_t first_seed, uint32_t seeds, uint32_t ticks)
{
    RunSpec_t spec;
    Score_t *none = calloc(1, sizeof(Score_t));
    Score_t *scr = calloc(1, sizeof(Score_t));
    Score_t *prof = calloc(1, sizeof(Score_t));
    Score_t *lng = calloc(1, sizeof(Score_t));
    uint32_t nl, detl, by4l, viafl;
    int medl, p90l;
    uint32_t n, det, by4, viaf, nb, detb, by4b, viafb, np, detp, by4p, viafp;
    int med, p90, medb, p90b, medp, p90p;
    double mean_conc = 0.0;

    memset(&spec, 0, sizeof(spec));
    spec.first_seed = first_seed;
    spec.seed_count = seeds;
    spec.ticks = ticks;
    spec.kind = kind;
    spec.loss = q;

    spec.profile = FAULT_PROFILE_NONE;
    run_score(&spec, none);

    spec.script_mask = SCRIPT_COMM;         /* isolated 6-16 tick faults, at most 3 at once */
    spec.script_max_conc = 3;
    run_score(&spec, scr);

    spec.script_long = 1;                   /* the same, at the built-in fault length (38-49 ticks) */
    run_score(&spec, lng);
    spec.script_long = 0;

    spec.script_mask = 0;
    spec.profile = FAULT_PROFILE_COMMUNICATION;
    run_score(&spec, prof);

    detection(lng, CONSENSUS_CLASS_COMMUNICATION, 6u, &nl, &detl, &by4l, &medl, &p90l, &viafl);
    detection(scr, CONSENSUS_CLASS_COMMUNICATION, 6u, &n, &det, &by4, &med, &p90, &viaf);
    detection(prof, CONSENSUS_CLASS_COMMUNICATION, 6u, &np, &detp, &by4p, &medp, &p90p, &viafp);
    detection_in_budget(prof, CONSENSUS_CLASS_COMMUNICATION, 6u, &nb, &detb, &by4b, &medb, &p90b, &viafb);
    for(uint32_t k = 0; k <= NUM_TEAMS; k++)
        mean_conc += (double)k * (double)prof->conc_hist[k];
    mean_conc /= (double)prof->ticks;

    /* false ELEVATED per 1000 target-rounds (a round is 2 ticks): profile
       none, both classes; the scripted comm-fault run outside faults; then
       detection over communication faults of >= 6 ticks. SCRIPTED = isolated
       faults (the design's regime). PROFILE = --fault-profile=communication,
       which keeps mean %.1f teams faulted at once, ALL faults and those seen
       with at most CONSENSUS_F other teams silent. */
    printf("ROW model=%s q=%.2f fast=%d emis=%s | none per1000: mob=%.3f comm_vote=%.3f comm_fast=%.3f (rounds=%llu) | "
           "SCRIPTED false comm_vote=%.3f comm_fast=%.3f faults=%u det=%.1f%% lat_med=%d lat_p90=%d by4=%.1f%% fast_first=%.1f%% | "
           "PROFILE(conc %.1f) false comm_vote=%.3f comm_fast=%.3f ALL faults=%u det=%.1f%% lat_med=%d by4=%.1f%% "
           "IN-BUDGET faults=%u det=%.1f%% lat_med=%d by4=%.1f%% | "
           "SCRIPTED-LONG(38-49) false comm_vote=%.3f comm_fast=%.3f faults=%u det=%.1f%% lat_med=%d by4=%.1f%%\n",
        loss_kind_name(kind), q, (int)CONSENSUS_FAST_PATH, TEAM_TEST_RANDOM_EMISSION ? "random" : "periodic",
        1000.0 * (double)none->false_vote[CONSENSUS_CLASS_MOBILITY] / (double)none->target_rounds,
        1000.0 * (double)none->false_vote[CONSENSUS_CLASS_COMMUNICATION] / (double)none->target_rounds,
        1000.0 * (double)none->false_fast[CONSENSUS_CLASS_COMMUNICATION] / (double)none->target_rounds,
        (unsigned long long)none->target_rounds,
        1000.0 * (double)scr->false_vote[CONSENSUS_CLASS_COMMUNICATION] / (double)scr->target_rounds,
        1000.0 * (double)scr->false_fast[CONSENSUS_CLASS_COMMUNICATION] / (double)scr->target_rounds,
        n, pct(det, n), med, p90, pct(by4, n), pct(viaf, det),
        mean_conc,
        1000.0 * (double)prof->false_vote[CONSENSUS_CLASS_COMMUNICATION] / (double)prof->target_rounds,
        1000.0 * (double)prof->false_fast[CONSENSUS_CLASS_COMMUNICATION] / (double)prof->target_rounds,
        np, pct(detp, np), medp, pct(by4p, np), nb, pct(detb, nb), medb, pct(by4b, nb),
        1000.0 * (double)lng->false_vote[CONSENSUS_CLASS_COMMUNICATION] / (double)lng->target_rounds,
        1000.0 * (double)lng->false_fast[CONSENSUS_CLASS_COMMUNICATION] / (double)lng->target_rounds,
        nl, pct(detl, nl), medl, pct(by4l, nl));
    (void)p90l; (void)viafl;
    free(lng);
    (void)p90p; (void)viafp; (void)p90b; (void)viafb;
    free(none);
    free(scr);
    free(prof);
}

static void print_hist(const char *label, const uint64_t *h)
{
    uint64_t total = 0;

    for(uint32_t k = 0; k <= NUM_TEAMS; k++) total += h[k];
    printf("%s", label);
    for(uint32_t k = 0; k <= NUM_TEAMS; k++)
        printf(" %u:%.1f%%", k, pct(h[k], total));
    printf("\n");
}

/* Fault concurrency for one profile (design section 10): how many teams are
   faulted at once, and how consensus fares inside (<= F other teams silent)
   and outside (> F) the fault budget. */
static void do_concurrency(uint32_t first_seed, uint32_t seeds, uint32_t ticks, float q, FaultProfile_t profile)
{
    Score_t *s = calloc(1, sizeof(Score_t));
    RunSpec_t spec;

    memset(&spec, 0, sizeof(spec));
    spec.first_seed = first_seed; spec.seed_count = seeds; spec.ticks = ticks;
    spec.kind = LM_IID; spec.loss = q; spec.profile = profile;
    run_score(&spec, s);

    printf("CONC profile=%s q=%.2f seeds=%u ticks=%u enforce=%d\n", fault_profile_to_string(profile), q, seeds, ticks,
        (int)CONSENSUS_ENFORCE);
    print_hist("CONC teams with any fault active (share of ticks):", s->conc_any_hist);
    print_hist("CONC teams with a communication fault active:    ", s->conc_hist);
    print_hist("CONC teams with a mobility fault active:         ", s->conc_mob_hist);
    {
        uint64_t in_budget = 0, total = 0;

        for(uint32_t k = 0; k <= NUM_TEAMS; k++)
        {
            total += s->conc_hist[k];
            if(k <= CONSENSUS_F + 1u) in_budget += s->conc_hist[k];
        }
        printf("CONC ticks with <= %u communication-faulted teams (target plus <= f peers): %.1f%%\n",
            (unsigned)CONSENSUS_F + 1u, pct(in_budget, total));
    }
    for(uint8_t c = 0; c < NCLS; c++)
    {
        const char *name = c == 0 ? "mobility" : "communication";
        uint32_t n[2] = {0, 0}, det[2] = {0, 0};

        for(uint32_t i = 0; i < s->n_ep[c]; i++)
        {
            const EpisodeResult_t *e = &s->ep[c][i];
            uint8_t b = (uint8_t)(e->max_silent_peers > CONSENSUS_F);

            if(e->dur < 6u) continue;
            if(c == 0 && e->en_route < 4u) continue;    /* nothing observable unless it travels */
            n[b]++;
            if(e->first_elev >= 0) det[b]++;
        }
        printf("CONC %s faults (>=6 ticks%s): silent peers <= %u: %u faults, detected %.1f%% | > %u: %u faults, detected %.1f%%\n",
            name, c == 0 ? ", travelling >= 4 ticks" : "", (unsigned)CONSENSUS_F, n[0], pct(det[0], n[0]),
            (unsigned)CONSENSUS_F, n[1], pct(det[1], n[1]));
        printf("CONC %s false ELEVATED per 1000 target-rounds: silent peers <= %u: %.3f (%llu rounds) | > %u: %.3f (%llu rounds)\n",
            name, (unsigned)CONSENSUS_F, 1000.0 * (double)s->false_b[c][0] / (double)(s->rounds_b[0] ? s->rounds_b[0] : 1),
            (unsigned long long)s->rounds_b[0], (unsigned)CONSENSUS_F,
            1000.0 * (double)s->false_b[c][1] / (double)(s->rounds_b[1] ? s->rounds_b[1] : 1),
            (unsigned long long)s->rounds_b[1]);
    }
    free(s);
}

/* ------------------------------------------------------------------ */
/* Trajectory hash (liar test: recovery columns byte-identical)        */
/* ------------------------------------------------------------------ */

static uint64_t fnv(uint64_t h, uint64_t v)
{
    for(int i = 0; i < 8; i++)
    {
        h ^= (v >> (8 * i)) & 0xFFu;
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t recovery_hash_tick(uint64_t h)
{
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        RecoveryStatus_t r = recovery_manager_get_status(t);

        h = fnv(h, (uint64_t)r.recommendation);
        h = fnv(h, (uint64_t)r.reason);
        h = fnv(h, (uint64_t)(int64_t)(r.confidence * 10000.0f));
        h = fnv(h, (uint64_t)r.escalation_level);
        h = fnv(h, (uint64_t)g_teams[t].status);
        h = fnv(h, (uint64_t)(uint16_t)g_teams[t].loc_x);
        h = fnv(h, (uint64_t)(uint16_t)g_teams[t].loc_y);
    }
    return h;
}

/* ------------------------------------------------------------------ */
/* assert mode                                                        */
/* ------------------------------------------------------------------ */

static uint32_t g_seeds = 3, g_ticks = 1200;

static void test_healthy_and_mirror(void)
{
    Score_t *s = calloc(1, sizeof(Score_t));
    RunSpec_t spec = { 1, g_seeds, g_ticks, FAULT_PROFILE_NONE, LM_IID, 0.0f, 1, 0, 0, 0 };

    /* 7.5: profile none, loss 0: zero FAULT verdicts, zero plausibility
       failures, zero ELEVATED, zero healthy stalls (any observer). */
    run_score(&spec, s);
    CHECKF(s->fault_verdicts[0] == 0 && s->fault_verdicts[1] == 0,
        "FAULT verdicts on a healthy loss-0 run: mob=%llu comm=%llu",
        (unsigned long long)s->fault_verdicts[0], (unsigned long long)s->fault_verdicts[1]);
    CHECKF(s->plaus_fail == 0, "plausibility failures on a healthy run: %llu", (unsigned long long)s->plaus_fail);
    CHECKF(s->elevated_ticks[0] == 0 && s->elevated_ticks[1] == 0, "ELEVATED on a healthy run");
    CHECKF(s->healthy_stall_obs == 0, "healthy stalls seen by peers: %llu", (unsigned long long)s->healthy_stall_obs);
    printf("  healthy loss 0: %llu ticks, %llu ELEVATED, %llu FAULT verdicts, %llu plaus fails, %llu healthy stalls\n",
        (unsigned long long)s->ticks, (unsigned long long)(s->elevated_ticks[0] + s->elevated_ticks[1]),
        (unsigned long long)(s->fault_verdicts[0] + s->fault_verdicts[1]),
        (unsigned long long)s->plaus_fail, (unsigned long long)s->healthy_stall_obs);

    /* Loss 0.05: zero ELEVATED; FAULT verdict count is reported. */
    spec.loss = 0.05f;
    run_score(&spec, s);
    CHECKF(s->elevated_ticks[0] == 0 && s->elevated_ticks[1] == 0,
        "ELEVATED on a healthy run at q=0.05: mob=%llu comm=%llu",
        (unsigned long long)s->elevated_ticks[0], (unsigned long long)s->elevated_ticks[1]);
    CHECKF(s->plaus_fail == 0, "plausibility failures at q=0.05: %llu", (unsigned long long)s->plaus_fail);
    printf("  healthy q=0.05: FAULT verdicts comm=%llu per 1e4 pair-ticks = %.2f (design expects about 0.9), mob=%llu\n",
        (unsigned long long)s->fault_verdicts[1],
        1.0e4 * (double)s->fault_verdicts[1] / (double)(s->ticks * NUM_TEAMS * (NUM_TEAMS - 1u)),
        (unsigned long long)s->fault_verdicts[0]);
    free(s);

    /* Mirror equivalence, profiles none and mobility, loss 0. */
    {
        FaultProfile_t profiles[2] = { FAULT_PROFILE_NONE, FAULT_PROFILE_MOBILITY };
        uint64_t compared = 0, mismatch = 0, nonzero = 0;

        for(int pi = 0; pi < 2; pi++)
        {
            for(uint32_t si = 0; si < g_seeds; si++)
            {
                uint8_t prev_hist[NUM_TEAMS], prev_samp[NUM_TEAMS];

                set_bus(LM_IID, 0.0f);
                world_begin(1u + si, profiles[pi]);
                memset(prev_hist, 0, sizeof(prev_hist));
                memset(prev_samp, 0, sizeof(prev_samp));
                for(uint32_t i = 0; i < g_ticks; i++)
                {
                    sim_tick();
                    if(i >= 3u)
                    {
                        for(uint8_t o = 0; o < NUM_TEAMS; o++)
                            for(uint8_t t = 0; t < NUM_TEAMS; t++)
                            {
                                PeerObservation_t p;

                                if(o == t) continue;
                                consensus_get_peer_observation(o, t, &p);
                                compared++;
                                if(p.mob_history != prev_hist[t] || p.mob_window_n != prev_samp[t])
                                {
                                    if(mismatch < 5u)
                                        printf("  mirror mismatch seed %u tick %u obs %u tgt %u: peer (%u,%u) watchdog (%u,%u)\n",
                                            1u + si, g_current_tick, o, t, p.mob_history, p.mob_window_n,
                                            prev_hist[t], prev_samp[t]);
                                    mismatch++;
                                }
                                if(p.mob_history) nonzero++;
                            }
                    }
                    /* The peers see tick k's state at tick k+1, so compare next
                       tick against the watchdog as it stands now. */
                    for(uint8_t t = 0; t < NUM_TEAMS; t++)
                        watchdog_test_get_stall_state(t, &prev_hist[t], &prev_samp[t]);
                }
            }
        }
        CHECKF(mismatch == 0, "mobility mirror differs from the watchdog in %llu of %llu comparisons",
            (unsigned long long)mismatch, (unsigned long long)compared);
        CHECK(nonzero > 0);     /* the mobility profile must actually exercise stalls */
        printf("  mirror equivalence: %llu comparisons, %llu mismatches, %llu with a non-empty stall window\n",
            (unsigned long long)compared, (unsigned long long)mismatch, (unsigned long long)nonzero);
    }
}

static void test_loss_table_pass(void)
{
    /* 7.2 pass criteria: 0 false ELEVATED for iid q <= 0.05, and >= 90%
       detection at q <= 0.05 with the periodic fault. Detection is asserted
       on the scripted isolated faults (design regime). The built-in
       --fault-profile=communication faults most teams at once, far past the
       f = 2 budget, so it is reported, not asserted. */
    static const float qs[2] = { 0.0f, 0.05f };

    for(int i = 0; i < 2; i++)
    {
        Score_t *none = calloc(1, sizeof(Score_t));
        Score_t *scr = calloc(1, sizeof(Score_t));
        Score_t *prof = calloc(1, sizeof(Score_t));
        RunSpec_t spec;
        uint32_t n, det, by4, viaf, nb, detb, by4b, viafb;
        int med, p90, medb, p90b;

        memset(&spec, 0, sizeof(spec));
        spec.first_seed = 1; spec.seed_count = g_seeds; spec.ticks = g_ticks;
        spec.kind = LM_IID; spec.loss = qs[i];
        run_score(&spec, none);
        spec.script_mask = SCRIPT_COMM;
        spec.script_max_conc = 3;
        run_score(&spec, scr);
        spec.script_mask = 0;
        spec.profile = FAULT_PROFILE_COMMUNICATION;
        run_score(&spec, prof);

        detection(scr, CONSENSUS_CLASS_COMMUNICATION, 6u, &n, &det, &by4, &med, &p90, &viaf);
        detection_in_budget(prof, CONSENSUS_CLASS_COMMUNICATION, 6u, &nb, &detb, &by4b, &medb, &p90b, &viafb);
        CHECKF(none->false_vote[0] + none->false_vote[1] + none->false_fast[0] + none->false_fast[1] == 0,
            "false ELEVATED at iid q=%.2f on a healthy run", qs[i]);
        CHECKF(scr->false_vote[0] + scr->false_vote[1] + scr->false_fast[0] + scr->false_fast[1] == 0,
            "false ELEVATED outside scripted faults at q=%.2f", qs[i]);
#if !TEAM_TEST_RANDOM_EMISSION
        CHECKF(n > 0 && pct(det, n) >= 90.0, "scripted detection %.1f%% (%u/%u) at q=%.2f", pct(det, n), det, n, qs[i]);
#endif
        printf("  iid q=%.2f: false ELEVATED healthy %llu, scripted-outside %llu | scripted faults %u detected %.1f%% "
               "(median lat %d, p90 %d, by tick 4 %.1f%%, fast first %.1f%%) | built-in profile in-budget faults %u detected %.1f%%\n",
            qs[i],
            (unsigned long long)(none->false_vote[0] + none->false_vote[1] + none->false_fast[0] + none->false_fast[1]),
            (unsigned long long)(scr->false_vote[0] + scr->false_vote[1] + scr->false_fast[0] + scr->false_fast[1]),
            n, pct(det, n), med, p90, pct(by4, n), pct(viaf, det), nb, pct(detb, nb));
        free(none);
        free(scr);
        free(prof);
    }
}

/* 7.4: communication faults are voted on by >= 2 peers, and gaps never make a
   mobility stall. */
static void test_comm_fault_voted(void)
{
    Score_t *s = calloc(1, sizeof(Score_t));
    RunSpec_t spec;
    uint32_t n = 0, ok2 = 0;
    uint32_t nd, det, by4, viaf;
    int med, p90;

    memset(&spec, 0, sizeof(spec));
    spec.first_seed = 1; spec.seed_count = g_seeds; spec.ticks = g_ticks;
    spec.kind = LM_IID; spec.loss = 0.05f; spec.observe_pairs = 1;
    spec.script_mask = SCRIPT_COMM;
    spec.script_max_conc = 3;
    run_score(&spec, s);
    for(uint32_t i = 0; i < s->n_ep[CONSENSUS_CLASS_COMMUNICATION]; i++)
    {
        const EpisodeResult_t *e = &s->ep[CONSENSUS_CLASS_COMMUNICATION][i];

        if(e->dur < 6u) continue;
        n++;
        if(e->voters >= 2u) ok2++;
    }
    CHECKF(n > 0 && pct(ok2, n) >= 95.0, "scripted faults voted FAULT by >= 2 peers: %u of %u", ok2, n);
    CHECKF(s->mob_fault_votes_in_comm_only == 0,
        "%llu mobility FAULT votes cast while only communication faults were active (gaps made stalls)",
        (unsigned long long)s->mob_fault_votes_in_comm_only);
    detection(s, CONSENSUS_CLASS_COMMUNICATION, 6u, &nd, &det, &by4, &med, &p90, &viaf);
    CHECKF(pct(det, nd) >= 90.0, "scripted communication faults reaching ELEVATED: %.1f%%", pct(det, nd));
#if CONSENSUS_FAST_PATH && !TEAM_TEST_RANDOM_EMISSION
    CHECKF(pct(by4, nd) >= 95.0, "fast path elevated by fault tick 4 in only %.1f%% of scripted faults (need 95%%)",
        pct(by4, nd));
#endif
    printf("  scripted comm faults >=6 ticks: %u, voted by >=2 peers %.1f%%, ELEVATED %.1f%% (median latency %d, p90 %d), "
           "by tick 4 %.1f%%, fast path first %.1f%%; mobility FAULT votes: %llu\n", n,
        pct(ok2, n), pct(det, nd), med, p90, pct(by4, nd), pct(viaf, det),
        (unsigned long long)s->mob_fault_votes_in_comm_only);
    free(s);

    /* Sparse faults at the built-in length (38-49 ticks: 5-16 plus the 33-tick
       repair confirmation), still at most 3 at once. */
    s = calloc(1, sizeof(Score_t));
    memset(&spec, 0, sizeof(spec));
    spec.first_seed = 1; spec.seed_count = g_seeds; spec.ticks = g_ticks;
    spec.kind = LM_IID; spec.loss = 0.05f;
    spec.script_mask = SCRIPT_COMM; spec.script_max_conc = 3; spec.script_long = 1;
    run_score(&spec, s);
    detection(s, CONSENSUS_CLASS_COMMUNICATION, 6u, &nd, &det, &by4, &med, &p90, &viaf);
    CHECKF(nd > 0 && pct(det, nd) >= 90.0, "long scripted faults reaching ELEVATED: %.1f%% of %u", pct(det, nd), nd);
    CHECKF(s->false_vote[1] + s->false_fast[1] == 0, "false comm ELEVATED outside long scripted faults");
    printf("  scripted 38-49 tick comm faults: %u, ELEVATED %.1f%% (median latency %d), by tick 4 %.1f%%, false ELEVATED %llu\n",
        nd, pct(det, nd), med, pct(by4, nd), (unsigned long long)(s->false_vote[1] + s->false_fast[1]));
    free(s);

    /* The built-in communication profile, for the record. */
    s = calloc(1, sizeof(Score_t));
    memset(&spec, 0, sizeof(spec));
    spec.first_seed = 1; spec.seed_count = g_seeds; spec.ticks = g_ticks;
    spec.kind = LM_IID; spec.loss = 0.05f; spec.profile = FAULT_PROFILE_COMMUNICATION;
    run_score(&spec, s);
    {
        uint64_t total = 0;

        for(uint32_t k = 0; k <= NUM_TEAMS; k++) total += s->conc_hist[k];
        printf("  built-in communication profile, teams faulted at once (share of ticks):");
        for(uint32_t k = 0; k <= NUM_TEAMS; k++)
            if(s->conc_hist[k]) printf(" %u:%.0f%%", k, pct(s->conc_hist[k], total));
        printf("\n");
    }
    free(s);
}

/* 7.3: faulty voters. */
static uint64_t liar_run(uint32_t seed, uint32_t ticks, float q, const uint8_t *liars, uint8_t n_liars,
    const uint8_t *dark, uint8_t n_dark, uint64_t *elevated)
{
    uint64_t h = 1469598103934665603ull;

    set_bus(LM_IID, q);
    world_begin(seed, FAULT_PROFILE_NONE);
    for(uint8_t i = 0; i < n_liars; i++)
        consensus_test_force_verdict(liars[i], CONSENSUS_ANY, CONSENSUS_ANY, PEER_VERDICT_FAULT);
    for(uint8_t i = 0; i < n_dark; i++)
        vbus_set_node_dark(dark[i], 1);
    *elevated = 0;
    for(uint32_t i = 0; i < ticks; i++)
    {
        sim_tick();
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            uint8_t skip = 0;

            for(uint8_t d = 0; d < n_dark; d++)
                if(dark[d] == t) skip = 1;
            if(skip) continue;
            for(uint8_t c = 0; c < NCLS; c++)
                if(consensus_get_result(t, (ConsensusClass_t)c).tier == CONSENSUS_TIER_ELEVATED)
                    (*elevated)++;
        }
        h = recovery_hash_tick(h);
    }
    return h;
}

static void test_liars(void)
{
    uint8_t one[1] = { 4 }, two[2] = { 4, 7 }, dark[3] = { 1, 2, 3 };
    uint64_t elev, clean_hash, liar_hash;

    /* Reference: no liar. */
    clean_hash = liar_run(1, g_ticks, 0.05f, NULL, 0, NULL, 0, &elev);
    CHECKF(elev == 0, "ELEVATED without any liar: %llu", (unsigned long long)elev);

    liar_hash = liar_run(1, g_ticks, 0.05f, one, 1, NULL, 0, &elev);
    CHECKF(elev == 0, "one liar elevated a healthy target %llu times", (unsigned long long)elev);
#if CONSENSUS_ENFORCE
    /* Enforcing: the liar must not change a single recovery decision. */
    CHECKF(liar_hash == clean_hash, "recovery trajectory changed because of one liar");
#endif
    (void)liar_hash;

    liar_run(1, g_ticks, 0.05f, two, 2, NULL, 0, &elev);
    CHECKF(elev == 0, "two liars elevated a healthy target %llu times at q=0.05", (unsigned long long)elev);

    liar_run(1, g_ticks, 0.05f, two, 2, dark, 3, &elev);
    CHECKF(elev == 0, "two liars + three dark honest peers elevated %llu times at q=0.05", (unsigned long long)elev);
    liar_run(1, g_ticks, 0.20f, two, 2, dark, 3, &elev);
    printf("  two liars + 3 dark honest peers at q=0.20: %llu ELEVATED target-ticks (reported; honest loss adds FAULT votes)\n",
        (unsigned long long)elev);

    /* Faulty reporters. Two reporters claiming "missed" about healthy target 5:
       the fast path never fires. */
    set_bus(LM_IID, 0.05f);
    world_begin(1, FAULT_PROFILE_NONE);
    consensus_test_force_heard_bit(1, 5, 0);
    consensus_test_force_heard_bit(2, 5, 0);
    for(uint32_t i = 0; i < g_ticks; i++)
        sim_tick();
    CHECKF(consensus_get_stats().fast_path_fires == 0, "false-claiming reporters fired the fast path %u times",
        (unsigned)consensus_get_stats().fast_path_fires);

#if CONSENSUS_FAST_PATH && !TEAM_TEST_RANDOM_EMISSION
    /* One reporter always claiming "heard": the fast path never fires for
       any target but itself, and the vote path still detects. */
    {
        Score_t *s = calloc(1, sizeof(Score_t));
        uint32_t n = 0, det = 0;

        /* At q = 0 the blocking reporter's row always reaches every fuser, so
           the fast path can never fire (asserted). At q = 0.05 its beacon is
           sometimes lost, the reporter drops out of R_k on those ticks, and
           the fast path can fire: reported. */
        static const float blk_q[2] = { 0.0f, 0.05f };

        g_script_exclude = 1u << 1;         /* the blocking reporter itself is never faulted */

        for(int bi = 0; bi < 2; bi++)
        {
            uint32_t fired = 0;

            n = det = 0;
            set_bus(LM_IID, blk_q[bi]);
            memset(s, 0, sizeof(*s));
            for(uint32_t si = 0; si < g_seeds; si++)
            {
                world_begin(1u + si, FAULT_PROFILE_NONE);
                score_reset_world();
                script_begin(1u + si, SCRIPT_COMM, 3);
                consensus_test_force_heard_bit(1, CONSENSUS_ANY, 1);
                for(uint32_t i = 0; i < g_ticks; i++)
                {
                    script_step();
                    sim_tick();
                    score_tick(s, 0, g_current_tick);
                    for(uint8_t t = 0; t < NUM_TEAMS; t++)
                        if(t != 1u && consensus_get_result(t, CONSENSUS_CLASS_COMMUNICATION).fast_path)
                            fired++;
                }
            }
            for(uint32_t i = 0; i < s->n_ep[CONSENSUS_CLASS_COMMUNICATION]; i++)
            {
                const EpisodeResult_t *e = &s->ep[CONSENSUS_CLASS_COMMUNICATION][i];

                if(e->dur < 6u) continue;
                n++;
                if(e->first_elev >= 0) det++;
            }
            printf("  blocking reporter at q=%.2f: fast path active on %u target-ticks; scripted comm faults reaching ELEVATED %.1f%% of %u\n",
                blk_q[bi], fired, pct(det, n), n);
            if(bi == 0)
            {
                CHECKF(fired == 0, "fast path fired %u target-ticks despite a blocking reporter at q=0", fired);
                CHECKF(n > 0 && pct(det, n) >= 50.0, "vote path detection only %.1f%% with a blocking reporter", pct(det, n));
            }
        }
        free(s);
        g_script_exclude = 0;
    }
#endif
}

/* 7.11: degraded observer. */
static void test_degraded_observer(void)
{
    uint64_t elevated = 0, accuse3 = 0, self_deaf = 0;

    set_bus(LM_IID, 0.05f);
    for(uint32_t si = 0; si < g_seeds; si++)
    {
        world_begin(1u + si, FAULT_PROFILE_NONE);
        vbus_set_node_rx_loss(3, 0.5f);
        for(uint32_t i = 0; i < g_ticks; i++)
        {
            uint8_t accused = 0;

            sim_tick();
            for(uint8_t t = 0; t < NUM_TEAMS; t++)
            {
                PeerObservation_t p;

                if(t == 3) continue;
                consensus_get_peer_observation(3, t, &p);
                if(p.vote[CONSENSUS_CLASS_COMMUNICATION].verdict == PEER_VERDICT_FAULT) accused++;
                if(p.observer_self_deaf) self_deaf++;
                if(consensus_get_result(t, CONSENSUS_CLASS_COMMUNICATION).tier == CONSENSUS_TIER_ELEVATED ||
                   consensus_get_result(t, CONSENSUS_CLASS_MOBILITY).tier == CONSENSUS_TIER_ELEVATED)
                    elevated++;
            }
            if(accused >= 3u) accuse3++;
        }
    }
    printf("  degraded observer: %llu self-deaf observer-target ticks, %llu ticks accusing >= 3\n",
        (unsigned long long)self_deaf, (unsigned long long)accuse3);
    CHECKF(accuse3 == 0, "degraded observer accused >= 3 targets on %llu ticks", (unsigned long long)accuse3);
    CHECKF(elevated == 0, "degraded observer led to %llu ELEVATED target-ticks", (unsigned long long)elevated);
}

/* 7.1: dropout mid-round, in the full simulation. */
static void test_dropout_mid_round(void)
{
    uint32_t checked = 0;

    for(uint32_t seed = 1; seed <= 6 && checked < 3; seed++)
    {
        set_bus(LM_IID, 0.0f);
        world_begin(seed, FAULT_PROFILE_MOBILITY);
        for(uint32_t i = 0; i < 1500 && checked < 3; i++)
        {
            uint8_t target = 0xFF, voter = 0xFF;

            sim_tick();
            if((g_current_tick & 1u) == 0u)
                continue;                   /* round r has just closed: the next tick is its first */
            for(uint8_t t = 0; t < NUM_TEAMS && target == 0xFF; t++)
            {
                ConsensusResult_t r = consensus_get_result(t, CONSENSUS_CLASS_MOBILITY);

                if(r.tier != CONSENSUS_TIER_ELEVATED || r.n_fault < 4u || r.n_live != NUM_TEAMS - 1u)
                    continue;
                for(uint8_t p = 0; p < NUM_TEAMS; p++)
                {
                    PeerObservation_t po;

                    if(p == t) continue;
                    consensus_get_peer_observation(p, t, &po);
                    if(po.vote[CONSENSUS_CLASS_MOBILITY].verdict == PEER_VERDICT_FAULT)
                    {
                        target = t;
                        voter = p;
                        break;
                    }
                }
            }
            if(target == 0xFF)
                continue;

            {
                ConsensusResult_t before = consensus_get_result(target, CONSENSUS_CLASS_MOBILITY);
                ConsensusResult_t in_round, next_round;
                uint8_t f_after, c_after;
                uint8_t want_elevated;

                sim_tick();                             /* first tick of round r: voter has sent its beacon */
                vbus_set_node_dark(voter, 1);
                sim_tick();                             /* last tick of round r: the in-flight beacon arrives */
                in_round = consensus_get_result(target, CONSENSUS_CLASS_MOBILITY);
                CHECKF(in_round.n_live == before.n_live,
                    "round r: voter %u dropped (n_live %u -> %u)", voter, before.n_live, in_round.n_live);
                sim_tick();
                sim_tick();                             /* round r+1 closes without the voter */
                next_round = consensus_get_result(target, CONSENSUS_CLASS_MOBILITY);
                CHECKF(next_round.n_live == in_round.n_live - 1u, "round r+1: n_live %u -> %u",
                    in_round.n_live, next_round.n_live);
                CHECKF(next_round.n_dark == in_round.n_dark + 1u, "round r+1: n_dark %u -> %u",
                    in_round.n_dark, next_round.n_dark);
                /* Dark is never CLEAR. The others' verdicts may move with the
                   simulation, so |C| may only change if some peer's own
                   verdict changed: it must not grow by the voter's absence. */
                CHECKF(next_round.n_clear <= in_round.n_clear + 1u, "round r+1: |C| jumped %u -> %u",
                    in_round.n_clear, next_round.n_clear);
                f_after = next_round.n_fault;
                c_after = next_round.n_clear;
                want_elevated = (uint8_t)(f_after >= CONSENSUS_X_MIN && f_after >= 2u * c_after + 1u &&
                    next_round.n_live >= CONSENSUS_MIN_LIVE_VOTERS);
                CHECKF((next_round.tier == CONSENSUS_TIER_ELEVATED) == want_elevated,
                    "round r+1 tier %d but recomputed |F|=%u |C|=%u says elevated=%u",
                    (int)next_round.tier, f_after, c_after, want_elevated);

                /* Back from dark: no communication FAULT vote in the next 6 ticks. */
                vbus_set_node_dark(voter, 0);
                for(int k = 0; k < 6; k++)
                {
                    sim_tick();
                    for(uint8_t t = 0; t < NUM_TEAMS; t++)
                    {
                        PeerObservation_t po;

                        if(t == voter) continue;
                        consensus_get_peer_observation(voter, t, &po);
                        CHECKF(po.vote[CONSENSUS_CLASS_COMMUNICATION].verdict != PEER_VERDICT_FAULT,
                            "voter %u cast a comm FAULT vote on %u right after returning from dark", voter, t);
                    }
                }
                checked++;
                printf("  dropout: seed %u target %u voter %u: n_live %u -> %u, |F| %u -> %u, tier %d\n",
                    seed, target, voter, in_round.n_live, next_round.n_live, in_round.n_fault, f_after,
                    (int)next_round.tier);
            }
            /* The rest of this seed is no longer a clean run. */
            break;
        }
    }
    CHECKF(checked > 0, "no ELEVATED mobility target with >= 4 FAULT voters found to drop out");
}

static void do_assert(int quick)
{
    if(quick)
    {
        g_seeds = 2;
        g_ticks = 800;
    }
    printf("healthy runs and mobility mirror\n");
    test_healthy_and_mirror();
    printf("loss table pass criteria (iid)\n");
    test_loss_table_pass();
    printf("communication faults voted by >= 2 peers, fast path\n");
    test_comm_fault_voted();
    printf("faulty voters and reporters\n");
    test_liars();
    printf("degraded observer\n");
    test_degraded_observer();
    printf("dropout mid-round\n");
    test_dropout_mid_round();
}

/* ------------------------------------------------------------------ */
/* airtime                                                            */
/* ------------------------------------------------------------------ */

static void do_airtime(void)
{
    VbusStats_t st;
    uint32_t ticks = 1000;
    double per_tick_ms, duty_pct, channel_pct;
    uint32_t wire = CONSENSUS_BEACON_SIZE + LORA_FRAME_HEADER_SIZE + LORA_FRAME_TRAILER_SIZE;

    set_bus(LM_IID, CONSENSUS_DEFAULT_LINK_LOSS);
    world_begin(1, FAULT_PROFILE_COMBINED);
    for(uint32_t i = 0; i < ticks; i++)
        sim_tick();
    st = vbus_get_stats();

    per_tick_ms = (double)st.airtime_ms_total / ticks;
    /* Per node: this run's mean transmit time per tick, at 36 s per tick. */
    duty_pct = 100.0 * (per_tick_ms / NUM_TEAMS) / ((double)TICK_TIME_SEC * 1000.0);
    channel_pct = 100.0 * per_tick_ms / ((double)TICK_TIME_SEC * 1000.0);
    printf("beacon payload %u B, wire %u B, ToA %.3f ms\n", (unsigned)CONSENSUS_BEACON_SIZE, wire,
        lora_time_on_air_ms_x1000(wire) / 1000.0);
    printf("%u ticks (combined profile): sent %u delivered %u dropped %u, airtime %u ms total (%.1f ms/tick)\n",
        ticks, st.sent, st.delivered, st.dropped, st.airtime_ms_total, per_tick_ms);
    printf("mean per-node duty cycle at %u s/tick: %.3f %% (worst case, all 10 beaconing every tick: %.3f %%)\n",
        TICK_TIME_SEC, duty_pct,
        100.0 * (lora_time_on_air_ms_x1000(wire) / 1000.0) / ((double)TICK_TIME_SEC * 1000.0));
    printf("whole-channel occupancy: %.3f %%\n", channel_pct);
    printf("minimum real tick period: %.2f s to keep each node under 1 %% duty cycle, %.2f s for a 10-slot TDMA frame (150 ms slots)\n",
        (lora_time_on_air_ms_x1000(wire) / 1000.0) / 10.0, 10.0 * 0.150);

    CHECK(wire == 80u);
    CHECK(lora_time_on_air_ms_x1000(wire) == 143616u);
    CHECK(duty_pct < 1.0);
    /* airtime_ms_total is the sum of ToA of the frames actually sent. */
    CHECK(st.airtime_ms_total == (uint32_t)(((uint64_t)st.sent * lora_time_on_air_ms_x1000(wire)) / 1000u));
}

/* ------------------------------------------------------------------ */
/* compare mode: D1 cap / enforcement comparison                      */
/* ------------------------------------------------------------------ */

typedef struct
{
    uint32_t episodes, detected_recovery, short_eps, short_missed;
    uint64_t team_ticks, false_escalation_ticks, escalation_ticks, capped;
    uint32_t consensus_detected;
    uint64_t resolved_incidents;
} CompareOut_t;

typedef struct
{
    uint8_t open;
    uint32_t start, last_active;
    uint8_t escalated;
    uint8_t consensus;
} CmpEp_t;

static void do_compare(uint32_t first_seed, uint32_t seeds, uint32_t ticks, float q, FaultProfile_t profile)
{
    CompareOut_t o;

    memset(&o, 0, sizeof(o));
    set_bus(LM_IID, q);
    for(uint32_t si = 0; si < seeds; si++)
    {
        CmpEp_t ep[NUM_TEAMS][NCLS];

        memset(ep, 0, sizeof(ep));
        world_begin(first_seed + si, profile);
        for(uint32_t i = 0; i < ticks; i++)
        {
            sim_tick();
            for(uint8_t t = 0; t < NUM_TEAMS; t++)
            {
                RecoveryStatus_t r = recovery_manager_get_status(t);
                uint8_t esc = (r.recommendation >= RECOVERY_ACTION_RETURN_TO_SAFE_ZONE);
                uint8_t attributable = (r.reason == RECOVERY_REASON_MOBILITY_FAILURE ||
                    r.reason == RECOVERY_REASON_COMMUNICATION_FAILURE ||
                    r.reason == RECOVERY_REASON_TEAM_DEGRADED ||
                    r.reason == RECOVERY_REASON_CONSENSUS_FAULT);
                uint8_t any_near = 0;

                o.team_ticks++;
                if(esc) o.escalation_ticks++;
                o.capped += g_consensus_capped_now[t];

                for(uint8_t c = 0; c < NCLS; c++)
                {
                    CmpEp_t *e = &ep[t][c];
                    uint8_t active = truth_active(t, c);
                    ConsensusResult_t cr = consensus_get_result(t, (ConsensusClass_t)c);

                    if(active)
                    {
                        if(!e->open)
                        {
                            memset(e, 0, sizeof(*e));
                            e->open = 1;
                            e->start = g_teams[t].fault_state.start_tick[k_fault_class[c]];
                        }
                        e->last_active = g_current_tick;
                    }
                    if(e->open)
                    {
                        if(esc && g_current_tick <= e->last_active + POST_WINDOW) e->escalated = 1;
                        if(cr.tier == CONSENSUS_TIER_ELEVATED) e->consensus = 1;
                        any_near = 1;
                    }
                    if(e->open && !active && g_current_tick > e->last_active + POST_WINDOW)
                    {
                        uint32_t dur = e->last_active - e->start + 1u;

                        if(dur >= 6u)
                        {
                            o.episodes++;
                            if(e->escalated) o.detected_recovery++;
                            if(e->consensus) o.consensus_detected++;
                            if(dur <= 10u)
                            {
                                o.short_eps++;
                                if(!e->escalated) o.short_missed++;
                            }
                        }
                        e->open = 0;
                    }
                }
                if(esc && attributable && !any_near)
                    o.false_escalation_ticks++;
            }
        }
        {
            extern uint32_t g_diag_incidents_resolved[NUM_TEAMS];

            for(uint8_t t = 0; t < NUM_TEAMS; t++)
                o.resolved_incidents += g_diag_incidents_resolved[t];
        }
    }
    printf("COMPARE profile=%s enforce=%d cap=%d fast=%d q=%.2f | episodes>=6=%u recovery_escalated=%.1f%% consensus_elevated=%.1f%% "
           "short(6-10)=%u missed_on_short=%u (%.1f%%) | false_escalations=%llu of %llu team-ticks (%.3f per 1000) "
           "escalation_ticks=%llu capped=%llu resolved=%llu\n",
        fault_profile_to_string(profile), (int)CONSENSUS_ENFORCE, (int)CONSENSUS_CAP_LOCAL_EVALUATORS,
        (int)CONSENSUS_FAST_PATH, q, o.episodes, pct(o.detected_recovery, o.episodes),
        pct(o.consensus_detected, o.episodes), o.short_eps, o.short_missed, pct(o.short_missed, o.short_eps),
        (unsigned long long)o.false_escalation_ticks, (unsigned long long)o.team_ticks,
        1000.0 * (double)o.false_escalation_ticks / (double)o.team_ticks,
        (unsigned long long)o.escalation_ticks, (unsigned long long)o.capped,
        (unsigned long long)o.resolved_incidents);
}

/* ------------------------------------------------------------------ */

static void parse_seeds(const char *s, uint32_t *first, uint32_t *n)
{
    const char *colon = strchr(s, ':');

    *first = (uint32_t)strtoul(s, NULL, 10);
    *n = colon ? (uint32_t)strtoul(colon + 1, NULL, 10) : 1u;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "assert";
    LossKind_t kind = LM_IID;
    float q = 0.05f;
    uint32_t first = 1, seeds = 5, ticks = 1500;
    FaultProfile_t profile = FAULT_PROFILE_COMBINED;
    int quick = 0;

    for(int i = 2; i < argc; i++)
    {
        if(!strcmp(argv[i], "--model") && i + 1 < argc) kind = loss_kind_from(argv[++i]);
        else if(!strcmp(argv[i], "--loss") && i + 1 < argc) q = (float)atof(argv[++i]);
        else if(!strcmp(argv[i], "--seeds") && i + 1 < argc) parse_seeds(argv[++i], &first, &seeds);
        else if(!strcmp(argv[i], "--ticks") && i + 1 < argc) ticks = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if(!strcmp(argv[i], "--profile") && i + 1 < argc) profile = fault_profile_from_string(argv[++i]);
        else if(!strcmp(argv[i], "quick")) quick = 1;
    }

    if(sim_runner_init() != 0)
    {
        printf("sim_runner_init failed\n");
        return 2;
    }

    if(!strcmp(mode, "table"))
        do_table(kind, q, first, seeds, ticks);
    else if(!strcmp(mode, "compare"))
        do_compare(first, seeds, ticks, q, profile);
    else if(!strcmp(mode, "concurrency"))
        do_concurrency(first, seeds, ticks, q, profile);
    else if(!strcmp(mode, "airtime"))
        do_airtime();
    else
        do_assert(quick);

    if(g_fail)
    {
        printf("consensus_scenario_test %s: %d FAILED\n", mode, g_fail);
        return 1;
    }
    printf("consensus_scenario_test %s: ok\n", mode);
    return 0;
}

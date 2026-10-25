#ifndef HEADROOM_PROBES_H
#define HEADROOM_PROBES_H

#include <stdint.h>

#include "mission_state.h"

/*
 * Step 5 headroom probes (docs/HEADROOM_MAP_PREREG.md). Opt-in, causal profile only, laptop-driven (the flags are
 * parsed in laptop/main.c). Every field is zero by default, and every hook in the core tests it first, so with no
 * --probe-* flag the simulation is exactly what it was before this file existed. Draws are hashes of (seed, tick,
 * team, salt), never a shared RNG stream. Fixed-size static state only; no heap, no I/O.
 */
typedef struct
{
    uint8_t any;                 /* at least one probe flag was given */
    uint8_t diag;                /* --probe-diag: print [DIAG PROBE] (output only) */
    /* D2 / D3a: the detector oracle (--planner-p=oracle) */
    uint16_t oracle_delay;       /* ticks (0 = live truth) */
    uint8_t oracle_noise;        /* 1: apply oracle_fn / oracle_fp */
    float oracle_fn, oracle_fp;
    /* D3b: baseline evidence dropout (recovery_manager reads only) */
    float evidence_dropout;
    /* S1 */
    float prevent_cost;          /* 0 = off, else multiplier on cost AND real maintenance time */
    uint8_t hazard_draw;         /* 1: the hazard forecast uses the realised draws (level i) */
    /* M1 */
    uint8_t urgency;             /* 0 off, 1 survival (rate), 2 foresight (realised draws) */
    /* M5, C2 */
    uint8_t reveal_all;
    uint8_t no_comm_faults;
    /* K1 */
    float rest_mult;             /* rest duration multiplier (0 = off): recovery rate / x */
    float drain_mult;            /* stamina drain multiplier (0 = off) */
    /* K2: teams taken out of service from tick 0 */
    uint8_t offline_k;
    /* Tier 2: work efficiency, travel, team mix */
    float work_mult;             /* W1: all teams (0 = off) */
    uint8_t work_fatigue_b;      /* W1 variant b: drain per on-scene tick scales with the work multiplier */
    uint8_t work_spec;           /* W2: specialization index + 1 (0 = off) */
    float work_spec_mult;
    uint8_t work_stage;          /* W3: 1 early stage only, 2 late stages only (0 = off) */
    float work_stage_mult;
    uint8_t instant_work;        /* W0 */
    uint8_t instant_travel;      /* V0: 1 to tasks, 2 to tasks and to rest */
    float speed[5];              /* V1: cells per tick by specialization (0 = default 1) */
    uint8_t team_mix;            /* TM1: 1 a, 2 b, 3 c (0 = default i % 5) */
    uint8_t central_alloc;       /* DQ1 */
    /* M2-M4: foresight of exogenous hazard events by record and replay */
    uint8_t foresight;           /* bit 0 fire, bit 1 structural collapse; 0xFF all */
    uint16_t lead;               /* ticks of warning */
} HeadroomProbes_t;

#define PROBE_MAX_EVENTS 1024u
typedef struct
{
    uint32_t tick;
    int16_t x, y;
    uint8_t type;
} ProbeEvent_t;

extern ProbeEvent_t g_probe_ev_seen[PROBE_MAX_EVENTS];   /* events this run queued (for --probe-record) */
extern uint32_t g_probe_ev_seen_n;
extern ProbeEvent_t g_probe_ev_plan[PROBE_MAX_EVENTS];   /* events loaded from a recording (replay) */
extern uint32_t g_probe_ev_plan_n;
extern uint32_t g_probe_ev_mismatch;                     /* replay: events that differed from the recording */

/* DQ1: after cbba_run(), replace the auction's assignment by a central optimal one-to-one assignment. */
void probe_central_assign(void);

/* K2 */
int probe_team_offline(uint8_t team_id);
/* TM1: specialization of team i */
uint8_t probe_team_spec(uint8_t team_index);
/* W1-W3: multiplier on a team's on-scene work this tick (1.0 when no work probe is on). */
float probe_work_mult(const TeamState_t *team, const IncidentState_t *incident);
void probe_stage_note(const IncidentState_t *incident);   /* an incident stage just completed */
/* V1: cells per tick for a specialization (1.0 default); the perimeter overlay marks damaged cells as +255 cost. */
float probe_speed(uint8_t spec);
const uint8_t *probe_perimeter_overlay(void);

/* Called by the environment when it queues a hazard-spawned incident. */
void probe_hazard_note(uint32_t tick, uint8_t type, int16_t x, int16_t y);
/* M2-M4 policy: idle teams of the right specialisation step toward a known upcoming event. */
void probe_preposition(void);

extern HeadroomProbes_t g_probe;

typedef struct
{
    uint32_t oracle_misses, oracle_false_alarms;
    uint32_t dropout_team_ticks;
    uint32_t tb[5][7];   /* TB: team-ticks by specialization x {resting, heading to rest, held in repair, faulted (mobility or resource, not held), on scene, en route to task, available}; the Step 4 order */
    uint32_t seen_by_prio[5], lost_by_prio[5];   /* victims seen / lost by the incident's priority at that time (index = Priority_t) */
} HeadroomProbeStats_t;

/* X2 secondary outcomes: one record per resolved incident (printed by the laptop under --probe-diag). */
#define PROBE_INC_LOG_MAX 2048u
typedef struct
{
    uint32_t id, reported, arrived, resolved;
    uint8_t type, prio;
} ProbeIncRecord_t;
extern ProbeIncRecord_t g_probe_inc_log[PROBE_INC_LOG_MAX];
extern uint32_t g_probe_inc_log_n;
void probe_inc_arrived(const IncidentState_t *inc);
void probe_inc_resolved(const IncidentState_t *inc);

extern HeadroomProbeStats_t g_probe_stats;

uint32_t probe_hash(uint32_t tick, uint32_t a, uint32_t salt);
float probe_unit(uint32_t tick, uint32_t a, uint32_t salt);

void probe_reset(void);
/* Once per tick from sim_tick(), after the fault injector: records the true fault flags for the delayed oracle and
 * refreshes the per-incident urgency cache. */
void probe_step(void);

/* D2 / D3a: what the detector oracle says about class f (0 mobility, 1 communication, 2 resource) for this team. */
float probe_oracle_p(const TeamState_t *team, uint8_t f);
/* D3b: 1 when the baseline's twin/watchdog evidence is lost for this team this tick. */
int probe_evidence_dropped(uint8_t team_id);
/* M1: allocator urgency score in [0, 1] for an incident-pool incident, or a negative value when the probe is off or
 * the task is not an incident-pool task. */
float probe_urgency_score(const IncidentState_t *incident);

#endif

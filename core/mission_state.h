#ifndef MISSION_STATE_H
#define MISSION_STATE_H

#include <stdint.h>

#include "fault_config.h"

typedef enum {
    SPEC_SEARCH       = 0,
    SPEC_MEDICAL      = 1,
    SPEC_EXTRACTION   = 2,
    SPEC_PERIMETER    = 3,
    SPEC_SUPPORT      = 4
} TeamSpecialization_t;

#define NUM_TEAM_SPECIALIZATIONS (SPEC_SUPPORT + 1)

typedef enum {
    STATUS_AVAILABLE = 0,
    STATUS_EN_ROUTE  = 1,
    STATUS_ON_SCENE  = 2,
    STATUS_RESTING   = 3
} TeamStatus_t;

typedef enum {
    PRIORITY_CRITICAL = 1,
    PRIORITY_HIGH     = 2,
    PRIORITY_MEDIUM   = 3,
    PRIORITY_LOW      = 4
} Priority_t;

typedef enum {
    INC_OPEN       = 0,
    INC_ASSIGNED   = 1,
    INC_IN_PROGRESS= 2,
    INC_RESOLVED   = 3
} IncidentStatus_t;

typedef enum {
    EVENT_NONE = 0,
    EVENT_NEW_INCIDENT,
    EVENT_TEAM_ASSIGNED,
    EVENT_TEAM_ON_SCENE,
    EVENT_STAGE_COMPLETE,
    EVENT_TEAM_REST,
    EVENT_TEAM_AVAILABLE,
    EVENT_INCIDENT_RESOLVED,
    EVENT_FAULT_INJECTED,
    EVENT_FAULT_RECOVERED,
    EVENT_WATCHDOG_ANOMALY_CHANGED,
    EVENT_RECOVERY_ACTION_CHANGED
} EventType_t;

typedef enum {
    ARDUOUS,
    MODERATE,
    LIGHT
} Fitness_Tier;


typedef enum {
    INCIDENT_STRUCTURAL_COLLAPSE = 0,
    INCIDENT_CONFINED_SPACE,
    INCIDENT_MASS_CASUALTY,
    INCIDENT_CHEMICAL_EXPOSURE,
    INCIDENT_UNSEARCHED_SECTOR,
    INCIDENT_SURVIVOR_SIGNAL,
    INCIDENT_GAS_LEAK,
    INCIDENT_ACTIVE_FIRE,
    INCIDENT_EQUIPMENT_CACHE,
    INCIDENT_RETURN_TO_SAFE_ZONE,
    INCIDENT_EVACUEE_OFFBOARD,

    NUM_INCIDENT_TYPES
} IncidentType_t;


#define EQ_ROPE         (1 << 0)
#define EQ_SPREADER     (1 << 1)
#define EQ_MEDICAL_KIT  (1 << 2)
#define EQ_K9           (1 << 4)
#define EQ_HAZMAT_SUIT  (1 << 5)
#define EQ_STRUCTURAL   (1 << 6)
#define EQ_GENERATOR    (1 << 7)
#define EQ_MAX_LEVEL 100

#define HAZ_GAS         (1 << 0)
#define HAZ_FLOOD       (1 << 1)
#define HAZ_STRUCTURAL  (1 << 2)
#define HAZ_CBRN        (1 << 3)

#define MAX_STAMINA 100.00f
#define MIN_STAMINA 0.00f
#define ARDUOUS_MIN_STAMINA   20.0f
#define MODERATE_MIN_STAMINA  26.0f
#define LIGHT_MIN_STAMINA     40.0f
#define ARDUOUS_READY_STAMINA   50.0f
#define MODERATE_READY_STAMINA  55.0f
#define LIGHT_READY_STAMINA     65.0f
#define ARDUOUS_RECOVERY_RATE   0.168f
#define MODERATE_RECOVERY_RATE  0.132f
#define LIGHT_RECOVERY_RATE     0.108f
#define STAMINA_TRAVEL_COST     0.110f
#define STAMINA_OPERATION_COST  0.330f

/* Per-tick incident work-rate range. Was 1-5, matched almost exactly to
 * EQUIPMENT_DRAIN_MAX (team.c) at the same 1-5/tick range, which meant an
 * average task (work_remaining=100) and average equipment supply (100)
 * both ran out in ~33 ticks - close enough to a coin flip whether a team
 * finished a task or got yanked home for equipment exhaustion first. That
 * collision was the single largest driver of total task volume: sensitivity
 * sweep (20 seeds x 3000 ticks per candidate) showed RETURN_TO_SAFE_ZONE
 * system tasks were 92% of all tasks ever created at 1-5, dropping to 89%
 * and cutting total task volume 39% (13598->8359) and safe-zone-slot
 * fallback failures 91% (9475->827) at 1-8, while also resolving *more*
 * organic incidents than the 1-5 baseline (167 vs 158) - the only
 * candidate range tested that improved both volume and throughput at
 * once. Equipment-side alternatives (slowing EQUIPMENT_DRAIN_MAX instead)
 * cut volume as much or more but resolved fewer incidents in most runs,
 * likely because busier teams register as less "available" to
 * calculate_generation_probability() and throttle down how much work gets
 * generated in the first place - so raising work rate was chosen over
 * slowing equipment drain. */
#define WORK_RATE_MIN            1
#define WORK_RATE_MAX            8

/* NFPA 1584 (Standard on the Rehabilitation Process for Members During
 * Emergency Operations) draws a hard line between a brief, informal
 * self-rehab after a single work cycle (>=10 min rest/hydration, no formal
 * medical clearance required) and the FULL, formal rehab it mandates once a
 * member reaches a second consecutive work cycle without one (>=20 min rest
 * plus vital-sign/medical evaluation) before a third deployment is allowed.
 * We mirror that two-tier structure here: a team may be pulled back into
 * service from a partially-recovered ("minimal rest") state for up to this
 * many consecutive cycles before it must be taken all the way to full (100)
 * stamina - not just its normal READY_STAMINA threshold - before it is
 * eligible to be pulled early again. */
#define MAX_CONSECUTIVE_PARTIAL_REST_CYCLES 2

// Equipment + fitness travel speed multiplier system.
// Tunable independently of team specialization — driven purely by what's
// carried (equipment_flags bitmask) and physical conditioning (fitness_tier).
#define EQUIP_SPEED_BASE_MULT        0.70f  // unencumbered baseline

#define EQUIP_SPEED_PENALTY_ROPE          0.05f
#define EQUIP_SPEED_PENALTY_SPREADER      0.20f
#define EQUIP_SPEED_PENALTY_MEDICAL_KIT   0.05f
#define EQUIP_SPEED_PENALTY_K9             0.00f
#define EQUIP_SPEED_PENALTY_HAZMAT_SUIT   0.15f
#define EQUIP_SPEED_PENALTY_STRUCTURAL    0.20f
#define EQUIP_SPEED_PENALTY_GENERATOR     0.25f

#define FITNESS_SPEED_MULT_ARDUOUS   0.90f
#define FITNESS_SPEED_MULT_MODERATE  1.00f
#define FITNESS_SPEED_MULT_LIGHT     1.15f

#define WEIGHT_CAPABILITY 0.40f
#define WEIGHT_DISTANCE   0.30f
#define WEIGHT_PRIORITY   0.20f
#define WEIGHT_STAMINA    0.10f

#define NUM_TEAMS     10
#define MIN_NUM_INCIDENTS 4
#define NUM_INCIDENTS 20
#define NUM_SYSTEM_TASKS 20
#define CBBA_MAX_BUNDLE_SIZE 3
#define CBBA_COMMITMENT_LIMIT 3
#define MIN_SAFE_ZONES 2
#define MAX_SAFE_ZONES 4
#define MIN_SAFE_ZONE_CAPACITY 10
#define MAX_SAFE_ZONE_CAPACITY 50

#define GRID_WIDTH       36
#define GRID_HEIGHT      36
#define CELL_SIZE_METERS 10
#define PATH_UNVISITED 0
#define PATH_OPEN      1
#define PATH_CLOSED    2

#define TICK_TIME_SEC    36
#define TICKS_PER_HOUR   (3600 / TICK_TIME_SEC)
#define NO_ASSIGNMENT 0xFF
#define NO_SAFE_ZONE 0xFF

/* --- Fault repair confirmation and post-repair reliability window ---
 * Grounded in real Incident Command System practice rather than picked
 * arbitrarily:
 *   - FEMA's National US&R Response System structures operations into
 *     "operational periods" (12 hours for Type III/IV task forces, 24
 *     hours for Type I/II) - the natural cadence at which a disaster
 *     response operation is planned and re-assessed.
 *   - Within a period, status changes are gated by Personnel
 *     Accountability Report (PAR) check-ins, commonly taken on ~20-minute
 *     benchmarks in fire/EMS incident command.
 * We model a dashboard's "final say" on a field repair as gated by one PAR
 * interval: the underlying fix can be technically complete, but nothing
 * downstream treats the fault as resolved until the next accountability
 * check-in confirms it. After a confirmed repair, the same fault class is
 * treated as reliable (immune to recurrence) for several PAR intervals -
 * consistent with reliability engineering's general MTBF >> MTTR
 * relationship (a genuine fix usually holds well past the repair window
 * itself) - while staying well short of a full operational period so the
 * effect is visible within a normal run instead of disabling a fault class
 * outright. */
#define FAULT_PAR_INTERVAL_TICKS         (TICKS_PER_HOUR / 3)       /* ~20 min */
#define FAULT_REPAIR_CONFIRM_DELAY_TICKS (FAULT_PAR_INTERVAL_TICKS)      /* 1 PAR cycle */
#define FAULT_POST_REPAIR_IMMUNITY_TICKS (FAULT_PAR_INTERVAL_TICKS * 4)  /* 4 PAR cycles */

#define NUM_EVACUATION_GATEWAYS 3
#define MIN_EVACUATION_TEAM_CAPACITY 8
#define MAX_EVACUATION_TEAM_CAPACITY 15
#define NO_EVAC_TARGET 0xFF

typedef enum {
    TASK_POOL_NONE = 0,
    TASK_POOL_INCIDENT,
    TASK_POOL_SYSTEM
} TaskPool_t;

typedef struct
{
    uint32_t active_mask;
    uint8_t active[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t start_tick[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t duration_ticks[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t recovery_tick[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t severity[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t target_team[NUM_OPERATIONAL_FAULT_CLASSES];
    int16_t target_x[NUM_OPERATIONAL_FAULT_CLASSES];
    int16_t target_y[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t original_travel_cost[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t original_road_blocked[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t original_navigable[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t original_priority[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t original_victim_count[NUM_OPERATIONAL_FAULT_CLASSES];
    uint8_t last_fault_class;
    uint32_t last_fault_start_tick;
    uint32_t last_fault_duration_ticks;
    uint32_t last_fault_recovery_tick;
    uint32_t last_fault_mask;
    /* Repair is a two-stage process: recovery_tick[] above is when the
     * underlying technical fix completes, but the fault stays fully active
     * (effects and all) until a separate confirmation gate clears it -
     * modeling final sign-off from an operator/dashboard rather than an
     * instantaneous auto-clear. See FAULT_REPAIR_CONFIRM_DELAY_TICKS. */
    uint8_t pending_repair[NUM_OPERATIONAL_FAULT_CLASSES];
    uint32_t repair_confirm_tick[NUM_OPERATIONAL_FAULT_CLASSES];
    /* Once a fault class is confirmed repaired, that team is immune to a
     * new fault of the same class until this tick - see
     * FAULT_POST_REPAIR_IMMUNITY_TICKS. Deliberately NOT reset by
     * clear_fault_summary(), since it must outlive the fault instance it
     * was set from. */
    uint32_t immune_until_tick[NUM_OPERATIONAL_FAULT_CLASSES];
} TeamFaultState_t;

typedef struct {
    uint8_t  team_id;
    int16_t  loc_x, loc_y;
    float  stamina;              // 0–100
    uint8_t  specialization;       // Specialization_t
    uint8_t  status;               // TeamStatus_t
    uint8_t  assigned_task_pool;     // TaskPool_t
    uint8_t  assigned_task_slot;     // slot within selected pool, or NO_ASSIGNMENT
    uint8_t incident_queue_slot[CBBA_MAX_BUNDLE_SIZE];
    uint32_t incident_queue_id[CBBA_MAX_BUNDLE_SIZE];
    uint8_t incident_queue_size;
    uint8_t incident_queue_index;
    uint8_t  hours_worked;         // integer hours this operational period
    uint16_t ticks_worked_this_period; // sub-hour accumulator; not RESTING each tick it advances
    uint8_t  equipment_flags;      // EQ_* bitmask
    uint32_t last_heartbeat_tick;  // µT-Kernel tk_get_tim() value
    uint8_t  fitness_tier;         // 0=ARDUOUS, 1=MODERATE, 2=LIGHT
    uint8_t  resting_zone_id;      // Safezone_id currently at, 0xFF if not resting
    uint8_t  equipment_level;
    TeamFaultState_t fault_state;
    uint16_t cached_remaining_path_cost;
    uint32_t cached_remaining_path_cost_tick;
    uint8_t  partial_rest_cycles;  // consecutive times pulled to work before
                                    // reaching full (100) stamina recovery;
                                    // see MAX_CONSECUTIVE_PARTIAL_REST_CYCLES
    uint8_t  degraded;             // watchdog suspects this team (anomaly
                                    // confidence in [INVESTIGATE, ANOMALY)
                                    // threshold) but hasn't determined a
                                    // specific fault yet. Orthogonal to
                                    // status - a degraded team keeps working
                                    // normally, it's just watched more
                                    // closely by the recovery evaluators.
} TeamState_t;

typedef struct {
    uint32_t incident_id;          // unique logical incident identity
    uint8_t  slot_occupied;
    int16_t  loc_x, loc_y;
    Priority_t  priority;             // Priority_t
    IncidentType_t incident_type;           // IncidentType_t
    TeamSpecialization_t  req_capability;       // Specialization_t
    uint8_t  status;               // IncidentStatus_t
    uint8_t  victim_count;         // tiebreaker + multi-team trigger
    uint8_t  hazard_flags;         // HAZ_* bitmask
    uint32_t reported_at_tick;     // age-weighted priority scoring
    uint8_t  structure_type;       // 0=NONE, 1=LIGHT, 2=MEDIUM, 3=HEAVY
    uint8_t  assigned_team;        // 0–4, or NO_ASSIGNMENT
    uint8_t  passive_effect;
    uint8_t  passive_timer;
    uint8_t  spawned_by;
    uint8_t  support_team;
    uint16_t work_remaining;
    uint8_t discovered;                          /* Fog-of-war reveal flag */
    uint8_t mission_fault_active_count;          /* Fault injection tracking */
    uint8_t mission_fault_original_priority;     /* Backup for priority override */
    uint8_t mission_fault_original_victim_count; /* Backup for victim count override */
} IncidentState_t;

typedef struct{
    uint8_t safezone_id;
    int16_t loc_x,loc_y;
    uint8_t capacity;
    uint8_t evacuee_count;
} SafeZone_t;
typedef struct {
    uint8_t gateway_id;
    int16_t loc_x;
    int16_t loc_y;
} EvacuationGateway_t;

typedef struct{
    uint8_t evacuation_team_id;
    int16_t loc_x,loc_y;
    uint8_t target_safezone;
    uint8_t gateway_id;
    uint8_t evacuee_capacity;
    uint8_t evacuee_count;
} EvacuationTeam_t;

typedef struct {
    uint8_t safety_level;
} GridCell_t;

typedef struct
{
    TaskPool_t pool;
    uint8_t slot;
} TaskRef_t;

typedef struct
{
    TaskRef_t bundle[CBBA_MAX_BUNDLE_SIZE];
    float bundle_bid[CBBA_MAX_BUNDLE_SIZE];
    uint8_t bundle_size;
    TaskRef_t path[CBBA_MAX_BUNDLE_SIZE];
    uint8_t path_size;
    uint32_t timestamp[NUM_TEAMS];
    uint8_t winner[NUM_INCIDENTS];
    float winning_bid[NUM_INCIDENTS];
    /* Manual dashboard override pin (core/mission_link_override.h) -
     * NOT touched by cbba_init()'s per-tick reset (deliberately: a
     * pin is a standing operator directive that must survive across
     * ticks until the incident it targets is no longer INC_OPEN, not
     * a per-round bidding artifact). See core/cbba.c's
     * cbba_build_bundle() for where these are read: force-injected
     * ahead of normal marginal-bid competition for the owning team,
     * and excluded from every OTHER team's bidding entirely, so a
     * forced assignment can't be silently re-bid-away next round.
     *
     * COORDINATION NOTE: this field pair was added to the END of
     * CBBAAgentState_t specifically (not inserted between existing
     * fields) to minimize collision risk with any concurrent edit to
     * this same struct's neighboring code. If another change also
     * adds a field here around the same time, reconcile field order
     * explicitly rather than letting one silently clobber the other -
     * both this struct's size (see the _Static_assert two lines
     * below the struct) and every fixed-offset assumption elsewhere
     * in this codebase (there should be none - packed offset
     * assumptions on a non-wire, RAM-only struct like this one would
     * themselves be a bug - but re-check before assuming that holds).
     *
     * MERGE NOTE (re-applied after this struct's surrounding tuning
     * work - WORK_RATE_MIN/MAX, NUM_SYSTEM_TASKS=20 - landed from a
     * branch that started before these two fields existed and so
     * dropped them; see the incoming diff that triggered this note):
     * neither of those changes touches this struct's layout (WORK_
     * RATE_MIN/MAX are plain #defines, NUM_SYSTEM_TASKS only sizes an
     * unrelated array elsewhere), so the size re-probe below is
     * expected to land back on the same 216 bytes it did the first
     * time - confirmed, not assumed, by re-running the same host
     * gcc probe after this merge (see that probe's own caveat below
     * about this still not being the real Cortex-M55 target).
     */
    uint8_t override_active;       /* 1 = override_incident_id below is a live pin */
    uint32_t override_incident_id; /* valid only when override_active */
} CBBAAgentState_t;

/* Size re-verified after adding override_active/override_incident_id
 * above, per this project's own rule for any struct-size change (see
 * core/lora_frame.h's CBBAMessage_t note for the precedent this
 * follows): NOT hand-calculated and NOT assumed free. Measured via a
 * host x86-64 gcc probe (`sizeof(CBBAAgentState_t)`): 208 bytes
 * before this change, 216 after - the two new fields cost 8 bytes,
 * not the 5 (1 + 4) a naive "just one small field" assumption would
 * expect, because a uint8_t immediately followed by a uint32_t forces
 * 3 bytes of alignment padding between them on a 4-byte-aligned
 * struct (this struct is NOT __attribute__((packed)) - it is
 * RAM-only, never put on a wire, so natural alignment is correct and
 * intended here, unlike the packed wire-format structs elsewhere in
 * this project).
 *
 * Caveat, stated plainly rather than silently assumed: this specific
 * probe ran on x86-64, not the real Cortex-M55 target - no
 * arm-none-eabi-gcc toolchain was available in the environment this
 * change was made in. uint8_t/uint32_t/float alignment rules are
 * standard EABI behavior expected to match between the two (no
 * 8-byte-aligned type like double/int64_t is present here, which is
 * where x86-64 SysV and ARM EABI padding rules can actually diverge),
 * but per this project's established process this _Static_assert
 * should be treated as "expected to hold on the real target" rather
 * than "confirmed on the real target" until someone with the actual
 * toolchain re-runs this probe on real hardware. */
_Static_assert(sizeof(CBBAAgentState_t) == 216u,
               "CBBAAgentState_t size changed - re-verify padding assumptions "
               "(see the comment immediately above this assert), including on "
               "the real Cortex-M55 target, before trusting this number");

typedef enum
{
    MISSION_STAGE_IDLE = 0,
    MISSION_STAGE_TRAVEL,
    MISSION_STAGE_WORK,
    MISSION_STAGE_RECOVERY
} MissionStage_t;

typedef struct
{
    uint8_t team_id;
    TaskPool_t assigned_task_pool;
    uint8_t assigned_task_slot;
    TaskRef_t planned_path[CBBA_MAX_BUNDLE_SIZE];
    uint8_t planned_path_size;
    uint8_t expected_path_index;
    int16_t destination_x;
    int16_t destination_y;
    uint32_t expected_arrival_tick;
    float expected_progress;
    float actual_progress;
    float progress_error;
    float expected_stamina;
    float actual_stamina;
    float stamina_error;
    uint16_t expected_work_remaining;
    uint16_t remaining_path_cost;
    MissionStage_t mission_stage;
    uint8_t communication_health;
    TeamStatus_t expected_operating_state;
    uint32_t observed_task_id;
    uint8_t observed_req_capability;
    TaskRef_t observed_plan[CBBA_MAX_BUNDLE_SIZE];
    uint8_t observed_plan_size;
    uint16_t baseline_path_cost;
    uint16_t baseline_work_remaining;
    uint16_t expected_remaining_path_cost;
    int16_t expected_loc_x;
    int16_t expected_loc_y;
} MissionExecutionTwin_t;

extern TeamState_t    g_teams[NUM_TEAMS];
extern IncidentState_t g_incidents[NUM_INCIDENTS];
extern IncidentState_t g_system_tasks[NUM_SYSTEM_TASKS];
extern SafeZone_t g_safezones[MAX_SAFE_ZONES];
extern uint8_t g_num_safe_zones;
extern GridCell_t g_grid[GRID_WIDTH][GRID_HEIGHT];
extern EvacuationTeam_t g_evacuation_teams[1];
extern EvacuationGateway_t g_evacuation_gateways[NUM_EVACUATION_GATEWAYS];
extern uint8_t g_safezone_evacuation_active[MAX_SAFE_ZONES];
extern uint32_t g_current_tick;
extern uint32_t g_world_seed;
extern CBBAAgentState_t g_cbba_agent_state[NUM_TEAMS];
extern MissionExecutionTwin_t g_mission_execution_twin[NUM_TEAMS];
#endif
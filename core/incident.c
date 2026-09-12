#include "incident.h"
#include "mission_state.h"
#include "cbba.h"
#include "simulation.h"
#include <stdlib.h>
#include "random.h"
#include "grid.h"
#include <stdbool.h>
#include "path.h"
#include "debug.h"
#include "snapshot.h"
#include "environment.h"
#include "mission_state_utils.h"
#include "search_zones.h"
#include "scoring.h"

uint32_t g_diag_incidents_resolved[NUM_TEAMS] = {0};
uint32_t g_diag_discovered_by_type[NUM_INCIDENT_TYPES] = {0};
uint32_t g_diag_resolved_by_type[NUM_INCIDENT_TYPES] = {0};
uint32_t g_diag_spawned_by_type[NUM_INCIDENT_TYPES] = {0};
uint32_t g_diag_undiscovered_at_end_by_type[NUM_INCIDENT_TYPES] = {0};
uint32_t g_diag_search_team_ticks = 0;
uint32_t g_diag_onscene_reveal_ticks = 0;
uint32_t g_diag_sweep_abandoned_for_recovery = 0;
uint32_t g_diag_sweep_reclaimed_for_safezone = 0;
uint32_t g_diag_sweep_duplicate_while_in_progress = 0;
uint32_t g_diag_sweep_retargets = 0;
/* Cumulative count of passive-coverage cells whose own district differs
 * from the district the sweeping team is standing in - i.e. sweep-radius
 * cells that straddle a district boundary. These are currently all
 * miscredited to the team's district (the M37 bug); this measures how
 * pervasive that misattribution is. See search_sweep_step() for why the
 * fix is shelved. */
uint32_t g_diag_sweep_cross_district_cells = 0;
/* Cumulative count of incidents that hit INCIDENT_UNREACHABLE_GIVEUP_
 * THRESHOLD consecutive no-path abandons and got quarantined out of the
 * candidate pools. Process-cumulative; the P1 sweep captures before/after
 * deltas the same way it does for the carve-repair counter. */
unsigned long g_diag_incidents_quarantined = 0;
/* Every stage a team hands off, including intermediate multi-stage legs
   (e.g. PERIMETER->MEDICAL on a STRUCTURAL_COLLAPSE). Distinct from
   g_diag_incidents_resolved, which only counts the team that completed
   the FINAL stage. Use this one for "who actually did work" views. */
uint32_t g_diag_stage_completions[NUM_TEAMS] = {0};
uint32_t g_diag_work_progress_calls = 0;
uint32_t g_diag_in_progress_entries = 0;
uint32_t g_diag_ticks_resting[NUM_TEAMS] = {0};
uint32_t g_diag_ticks_working[NUM_TEAMS] = {0};
uint32_t g_diag_ticks_available[NUM_TEAMS] = {0};
static IncidentState_t *find_free_system_task_slot(void);
static IncidentState_t *find_reclaimable_system_task_slot(void);



static int team_needs_safe_zone(const TeamState_t *team);
static int is_valid_team_id(uint8_t team_id);
static void clear_task_slot(TaskPool_t pool, IncidentState_t *task);
static void release_team(uint8_t team_id);
static void release_main_task_team(TaskPool_t pool, IncidentState_t *task);
static void release_all_task_teams(TaskPool_t pool, IncidentState_t *task);
static void complete_task(TaskPool_t pool, IncidentState_t *task);
static void clear_stale_equipment_cache_requests(void);
void reopen_future_queued_incidents(void);
uint8_t find_nearest_safe_zone(const TeamState_t *team);
static IncidentState_t *find_free_incident_slot(void);
static void apply_incident_archetype(IncidentState_t *incident, IncidentType_t type);

static uint8_t find_best_gateway(const SafeZone_t *safezone);

typedef enum
{
    GEN_LOAD_IDLE = 0,
    GEN_LOAD_LOW,
    GEN_LOAD_MODERATE,
    GEN_LOAD_HIGH,
    GEN_LOAD_CRITICAL
} GenerationLoad_t;

typedef struct
{
    uint8_t available_teams;
    uint8_t en_route_teams;
    uint8_t on_scene_teams;
    uint8_t resting_teams;
    uint8_t available_by_spec[NUM_TEAMS];
    uint8_t active_by_spec[NUM_TEAMS];
    uint8_t waiting_by_spec[NUM_TEAMS];
    uint8_t open_incidents;
    uint8_t assigned_incidents;
    uint8_t in_progress_incidents;
    uint8_t active_system_tasks;
    uint16_t degraded_cells;
    uint8_t hidden_incidents;   /* open, but !discovered - undiscovered backlog */
} GenerationContext_t;

typedef struct
{
    IncidentType_t type;
    uint8_t base_weight;
    uint8_t demand[NUM_TEAMS];
} IncidentGenerationProfile_t;

static void build_generation_context(GenerationContext_t *ctx);
static GenerationLoad_t calculate_generation_load(const GenerationContext_t *ctx);
static uint32_t calculate_generation_probability(const GenerationContext_t *ctx, GenerationLoad_t load);
static IncidentType_t select_weighted_incident_type(const GenerationContext_t *ctx);
static void schedule_next_generation_evaluation(GenerationLoad_t load);
static int generate_incident(IncidentType_t type);
static int generate_incident_at(IncidentType_t type, int16_t loc_x, int16_t loc_y);



static uint32_t g_incident_counter = 0;

static uint32_t g_next_incident_evaluation_tick = 0;

static const IncidentGenerationProfile_t g_generation_profiles[] =
{
    {
        INCIDENT_STRUCTURAL_COLLAPSE,
        20,
        {
            [SPEC_PERIMETER]  = 100,
            [SPEC_MEDICAL]    = 60,
            [SPEC_EXTRACTION] = 100,
            [SPEC_SUPPORT]    = 40
        }
    },
    {
        INCIDENT_CONFINED_SPACE,
        20,
        {
            [SPEC_PERIMETER]  = 100,
            [SPEC_MEDICAL]    = 40,
            [SPEC_EXTRACTION] = 100,
            [SPEC_SUPPORT]    = 20
        }
    },
    {
        INCIDENT_MASS_CASUALTY,
        20,
        {
            [SPEC_MEDICAL]    = 100,
            [SPEC_EXTRACTION] = 100,
            [SPEC_SUPPORT]    = 50
        }
    },
    {
        INCIDENT_CHEMICAL_EXPOSURE,
        20,
        {
            [SPEC_PERIMETER]  = 100,
            [SPEC_MEDICAL]    = 40,
            [SPEC_EXTRACTION] = 60,
            [SPEC_SUPPORT]    = 40
        }
    },
    {
        INCIDENT_UNSEARCHED_SECTOR,
        20,
        {
            [SPEC_SEARCH]     = 100,
            [SPEC_MEDICAL]    = 70,
            [SPEC_EXTRACTION] = 100,
            [SPEC_SUPPORT]    = 20
        }
    },
    {
        INCIDENT_SURVIVOR_SIGNAL,
        20,
        {
            [SPEC_SEARCH]     = 100,
            [SPEC_EXTRACTION] = 100,
            [SPEC_SUPPORT]    = 20
        }
    },
    {
        INCIDENT_GAS_LEAK,
        20,
        {
            [SPEC_PERIMETER]  = 100,
            [SPEC_MEDICAL]    = 30,
            [SPEC_EXTRACTION] = 50,
            [SPEC_SUPPORT]    = 30
        }
    },
    {
        INCIDENT_ACTIVE_FIRE,
        20,
        {
            [SPEC_PERIMETER]  = 100,
            [SPEC_MEDICAL]    = 60,
            [SPEC_EXTRACTION] = 80,
            [SPEC_SUPPORT]    = 50
        }
    }
};

#define GENERATION_PROFILE_COUNT \
    (sizeof(g_generation_profiles) / sizeof(g_generation_profiles[0]))

static uint32_t g_last_generation_weights[GENERATION_PROFILE_COUNT];
static uint32_t g_last_generation_total_weight = 0;
static uint32_t g_last_generation_type_roll = 0;

uint32_t allocate_incident_id(void)
{
    return g_incident_counter++;
}

const IncidentArchetype_t g_incident_archetypes[NUM_INCIDENT_TYPES] =
{
        [INCIDENT_STRUCTURAL_COLLAPSE] =
    {
        .incident_type = INCIDENT_STRUCTURAL_COLLAPSE,
        .priority = PRIORITY_CRITICAL,
        .required_specialization = SPEC_PERIMETER,
        .hazard_flags = HAZ_STRUCTURAL,
        .passive_effect = PASSIVE_NONE,
    },
        [INCIDENT_SURVIVOR_SIGNAL]=
    {
        .incident_type = INCIDENT_SURVIVOR_SIGNAL,
        .priority = PRIORITY_CRITICAL,
        .required_specialization = SPEC_SEARCH,
        .hazard_flags = HAZ_STRUCTURAL,
        .passive_effect = PASSIVE_NONE,
    },
        [INCIDENT_CONFINED_SPACE]=
    {
        .incident_type = INCIDENT_CONFINED_SPACE,
        .priority = PRIORITY_HIGH,
        .required_specialization = SPEC_PERIMETER,
        .hazard_flags = HAZ_STRUCTURAL,
        .passive_effect = PASSIVE_NONE,
    },
        [INCIDENT_MASS_CASUALTY]=
        {
            .incident_type=INCIDENT_MASS_CASUALTY,
            .priority= PRIORITY_CRITICAL,
            .required_specialization =SPEC_MEDICAL,
            .hazard_flags = 0,
            .passive_effect = PASSIVE_NONE,
        },
        [INCIDENT_CHEMICAL_EXPOSURE]=
        {
            .incident_type=INCIDENT_CHEMICAL_EXPOSURE,
            .priority= PRIORITY_HIGH,
            .required_specialization =SPEC_PERIMETER,
            .hazard_flags = HAZ_CBRN,
            .passive_effect = PASSIVE_GRID_DEGRADE,
        },
        [INCIDENT_UNSEARCHED_SECTOR]=
        {
            .incident_type=INCIDENT_UNSEARCHED_SECTOR,
            .priority= PRIORITY_MEDIUM,
            .required_specialization =SPEC_SEARCH,
            .hazard_flags = HAZ_STRUCTURAL,
            .passive_effect = PASSIVE_NONE,
        },
        [INCIDENT_GAS_LEAK]=
        {
            .incident_type=INCIDENT_GAS_LEAK,
            .priority= PRIORITY_HIGH,
            .required_specialization =SPEC_PERIMETER,
            .hazard_flags = HAZ_GAS,
            .passive_effect = PASSIVE_GRID_DEGRADE,
        },
        [INCIDENT_ACTIVE_FIRE]=
        {
            .incident_type=INCIDENT_ACTIVE_FIRE,
            .priority= PRIORITY_CRITICAL,
            .required_specialization =SPEC_PERIMETER,
            .hazard_flags = HAZ_STRUCTURAL,
            .passive_effect = PASSIVE_GRID_DEGRADE,
        },
        [INCIDENT_EVACUEE_OFFBOARD] =
        {
            .incident_type = INCIDENT_EVACUEE_OFFBOARD,
            .priority = PRIORITY_HIGH,
            .required_specialization = SPEC_EXTRACTION,
            .hazard_flags = 0,
            .passive_effect = PASSIVE_NONE,
        },
        [INCIDENT_EQUIPMENT_CACHE] =
        {
            .incident_type=INCIDENT_EQUIPMENT_CACHE,
            .priority=PRIORITY_CRITICAL,
            .required_specialization=SPEC_SUPPORT,
            .hazard_flags=0,
            .passive_effect=PASSIVE_NONE,
        },
        [INCIDENT_RETURN_TO_SAFE_ZONE] =
        {
            .incident_type = INCIDENT_RETURN_TO_SAFE_ZONE,
            .priority=PRIORITY_LOW,
            .passive_effect=PASSIVE_NONE,
            .hazard_flags=0,
        }

};

static void apply_incident_archetype(IncidentState_t *incident, IncidentType_t type)
{
    const IncidentArchetype_t *archetype;

    if((uint32_t)type >= NUM_INCIDENT_TYPES)
        return;

    archetype = &g_incident_archetypes[type];

    incident->incident_type = type;
    incident->priority = archetype->priority;
    incident->req_capability = archetype->required_specialization;
    incident->hazard_flags = archetype->hazard_flags;
    incident->passive_effect = archetype->passive_effect;
}

/* Generates OPEN INCIDENT_UNSEARCHED_SECTOR SYSTEM tasks (TASK_POOL_SYSTEM,
 * not the incident pool) for the stalest districts that don't already
 * have one in flight - then leaves them at status=INC_OPEN for the
 * EXISTING competitive system-task loop in allocate_all_incidents()
 * (the same one EQUIPMENT_CACHE and every other real system task goes
 * through: scans open tasks, calls select_best_team(), single winner
 * gets assigned) to pick up on its own. This is NOT a direct-assign
 * bypass like begin_return_to_safe_zone() - sweeping is routine
 * operational work, not an edge-case interrupt, so it competes for a
 * team like anything else rather than being handed one. req_capability
 * comes from the INCIDENT_UNSEARCHED_SECTOR archetype (SPEC_SEARCH) via
 * apply_incident_archetype() - a non-Search team is still technically
 * eligible to win one if no Search team is competing, same as every
 * other capability-scored task in this codebase; nothing hard-blocks
 * it, scoring just favors the right specialization.
 *
 * One open task per stale district per call, budgeted by how many
 * Search-spec teams aren't already actively sweeping something -
 * generating more open tasks than there are teams to win them would
 * just sit as dead inventory. Location is the nearest uncovered
 * navigable cell in the target district, not a fixed point, so the
 * "arrival" special-case in move_team() can keep retargeting it as
 * coverage expands once a team wins it. */
void search_generate_sweep_tasks(uint32_t current_tick)
{
    uint16_t districts_with_task_in_flight = 0;
    uint8_t search_teams_available_for_new_task = 0;

    /* A district counts as "in flight" for OPEN, ASSIGNED, *and*
     * IN_PROGRESS sweep tasks. Originally this only checked OPEN/
     * ASSIGNED, which left a gap the instant a team actually arrived
     * on-scene and move_team() flipped the task to IN_PROGRESS: this
     * function would then see "nothing in flight for this district"
     * and hand a second Search team a brand-new task for the exact
     * same district while the first one was still actively sweeping
     * it - confirmed happening (g_diag_sweep_duplicate_while_in_progress)
     * before this fix. */
    for(uint16_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        IncidentState_t *task = &g_system_tasks[i];
        if(task->slot_occupied && task->incident_type == INCIDENT_UNSEARCHED_SECTOR &&
           (task->status == INC_OPEN || task->status == INC_ASSIGNED ||
            task->status == INC_IN_PROGRESS))
        {
            uint8_t district_id = zone_id_for_location(task->loc_x, task->loc_y);
            if(district_id < NUM_SEARCH_ZONES)
                districts_with_task_in_flight |= (uint16_t)(1u << district_id);
        }
    }

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        TeamState_t *team = &g_teams[t];
        if(team->specialization != SPEC_SEARCH)
            continue;

        uint8_t already_sweeping =
            (team->assigned_task_pool == TASK_POOL_SYSTEM &&
             team->assigned_task_slot < NUM_SYSTEM_TASKS &&
             g_system_tasks[team->assigned_task_slot].incident_type == INCIDENT_UNSEARCHED_SECTOR);

        if(!already_sweeping)
            search_teams_available_for_new_task++;
    }

    for(uint8_t n = 0; n < search_teams_available_for_new_task; n++)
    {
        uint8_t district_id = search_zone_pick_next_target(current_tick, districts_with_task_in_flight);
        if(district_id == NO_ASSIGNMENT)
            break;  /* every district already has a task in flight */

        /* Skip a district still in its post-reclaim cooldown - see
         * ZONE_SWEEP_RECLAIM_COOLDOWN_TICKS. Excluding it and trying
         * the next-stalest candidate (rather than just breaking) keeps
         * other districts' sweeps flowing normally while this one
         * waits out the cooldown. */
        if(current_tick - g_search_zones[district_id].reclaimed_at_tick < ZONE_SWEEP_RECLAIM_COOLDOWN_TICKS)
        {
            districts_with_task_in_flight |= (uint16_t)(1u << district_id);
            continue;
        }

        /* Must happen BEFORE the frontier pick, not after - the frontier
         * pick needs a correct cycle_started_tick to know what's already
         * covered this cycle. Both cycle_started_tick and
         * g_cell_last_covered_tick[][] default to 0 at world-init, so if
         * this were set only after a successful pick, a fresh district
         * could never produce a first success at all: every cell would
         * compare as "covered" against a stale cycle_started_tick of 0. */
        if(g_search_zones[district_id].covered_cells == 0)
            g_search_zones[district_id].cycle_started_tick = current_tick;

        int16_t target_x, target_y;
        int16_t district_center_x = 0, district_center_y = 0;
        environment_get_district_center(district_id, &district_center_x, &district_center_y);
        if(!search_zone_pick_frontier_cell(district_id, district_center_x, district_center_y, &target_x, &target_y))
        {
            /* nothing left uncovered in this district right now - exclude
             * it and try the next-stalest candidate instead */
            districts_with_task_in_flight |= (uint16_t)(1u << district_id);
            continue;
        }

        IncidentState_t *task = find_free_system_task_slot();
        if(task == NULL)
            break;  /* out of system task slots entirely */

        task->incident_id = allocate_incident_id();
        apply_incident_archetype(task, INCIDENT_UNSEARCHED_SECTOR);

        task->status = INC_OPEN;
        task->assigned_team = NO_ASSIGNMENT;
        task->support_team = NO_ASSIGNMENT;
        task->loc_x = target_x;
        task->loc_y = target_y;
        task->victim_count = 0;
        task->reported_at_tick = current_tick;
        task->structure_type = 0;
        task->passive_timer = 0;
        task->spawned_by = NO_ASSIGNMENT;
        task->work_remaining = 0;  /* not worked in the usual sense - see move_team()'s arrival special-case */

        districts_with_task_in_flight |= (uint16_t)(1u << district_id);
        extern uint32_t g_diag_sweep_tasks_created[NUM_SEARCH_ZONES];
        g_diag_sweep_tasks_created[district_id]++;
    }
}

/* How far a team's sweep contribution reaches while actively holding an
 * UNSEARCHED_SECTOR task, scaled by the same CAPABILITY_MATRIX every
 * other task in this codebase already uses (scoring.c) - not a
 * separate ad-hoc number. Search scores 1.00 against its own required
 * capability, so this reduces to exactly SEARCH_SWEEP_RADIUS for
 * Search, matching its existing passive radius. A non-Search team that
 * wins the task via calculate_score() gets a real, proportionally
 * smaller radius instead of contributing nothing - consistent with how
 * a capability mismatch works everywhere else: reduced effectiveness,
 * not zero effectiveness. */
static uint8_t sweep_radius_for_team(const TeamState_t *team)
{
    IncidentState_t probe = {0};
    probe.req_capability = SPEC_SEARCH;

    float capability = capability_score(team, &probe);
    int32_t scaled = (int32_t)(SEARCH_SWEEP_RADIUS * capability + 0.5f);
    if(scaled < 1)
        scaled = 1;
    if(scaled > SEARCH_SWEEP_RADIUS)
        scaled = SEARCH_SWEEP_RADIUS;
    return (uint8_t)scaled;
}

/* Runs once per tick. Two responsibilities:
 *
 * 1. Coverage marking + cycle-completion for whichever teams are
 *    currently actively sweeping (derived directly from team
 *    assignment state, not a separate claim flag - a team's own
 *    assigned_task_pool/slot is already the ground truth for what
 *    it's doing) - increments active_sweep_ticks (only while actively
 *    sweeping, so interruptions don't count against the stall cap),
 *    and releases the team back to AVAILABLE on genuine completion
 *    (the system task never goes through the normal
 *    arrive->ON_SCENE->work flow - see move_team()'s special-case).
 *
 * 2. Reveal: any hidden incident within a qualifying team's radius gets
 *    discovered=1. Search's radius is continuous (any status, any task -
 *    a standing passive sense, not toggled per-task). Other teams only
 *    reveal while ON_SCENE with a smaller radius - see search_zones.h
 *    for the actual radius constants. */
void search_sweep_step(uint32_t current_tick)
{
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        TeamState_t *team = &g_teams[t];

        if(team->assigned_task_pool != TASK_POOL_SYSTEM ||
           team->assigned_task_slot >= NUM_SYSTEM_TASKS)
            continue;

        IncidentState_t *task = &g_system_tasks[team->assigned_task_slot];
        if(task->incident_type != INCIDENT_UNSEARCHED_SECTOR)
            continue;

        uint8_t district_id = zone_id_for_location(task->loc_x, task->loc_y);

        /* Only count ticks toward the stall cap while the team is
         * actually ON_SCENE covering ground - the header comment above
         * says this counter should only grow "while actively sweeping,
         * so interruptions don't count against the stall cap", but this
         * previously incremented unconditionally, including every tick
         * spent just traveling to the frontier cell. For a distant
         * target that ate a large chunk of the 150-tick budget before
         * the team ever started covering anything, making the stall
         * timeout trigger far earlier than intended. */
        if(team->status == STATUS_ON_SCENE)
            g_search_zones[district_id].active_sweep_ticks++;

        if(search_zone_check_cycle_completion(district_id, current_tick))
        {
            clear_task_slot(TASK_POOL_SYSTEM, task);
            team->assigned_task_pool = TASK_POOL_NONE;
            team->assigned_task_slot = NO_ASSIGNMENT;
            team->status = STATUS_AVAILABLE;
        }
        else if(team->status == STATUS_ON_SCENE)
        {
            /* Patrol: the district isn't done and this team is sitting
             * still. The task comment upstream claims move_team()'s
             * arrival special-case "keeps retargeting it as coverage
             * expands" - that retargeting never actually existed
             * anywhere in the codebase, so a team parked at its first
             * frontier cell covered exactly one static radius circle
             * for its entire attempt and then either genuinely finished
             * (only possible for a district small enough to fit in one
             * circle) or sat there wasting ticks until the stall cap
             * released it. This is the actual retarget: once the
             * current cell stops yielding new ground (checked via the
             * same frontier search used to pick the very first target),
             * walk the team to the next-nearest uncovered cell in the
             * district instead of leaving it planted. Re-uses
             * move_team()'s existing EN_ROUTE pathing - no new movement
             * code needed, just point the task at a new destination and
             * let the normal travel flow take it there. */
            int16_t next_x, next_y;
            if(search_zone_pick_frontier_cell(district_id, task->loc_x, task->loc_y,
                                               &next_x, &next_y) &&
               (next_x != task->loc_x || next_y != task->loc_y))
            {
                /* Credit the current cell as covered before moving the task —
                   overwriting loc_x/loc_y without this loses the coverage mark
                   for the cell the team just finished standing on */
                search_zone_mark_cell_covered(district_id,
                                              task->loc_x, task->loc_y,
                                              current_tick);

                task->loc_x = next_x;
                task->loc_y = next_y;
                team->status = STATUS_EN_ROUTE;
                extern uint32_t g_diag_sweep_retargets;
                g_diag_sweep_retargets++;
            }
        }
    }

    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        TeamState_t *team = &g_teams[t];
        uint8_t on_active_sweep_task =
            (team->assigned_task_pool == TASK_POOL_SYSTEM &&
             team->assigned_task_slot < NUM_SYSTEM_TASKS &&
             g_system_tasks[team->assigned_task_slot].incident_type == INCIDENT_UNSEARCHED_SECTOR);

        /* Search's passive continuous radius already covers the case
         * where Search itself is on a sweep task (capability_score is
         * 1.00 against its own required capability, so
         * sweep_radius_for_team() would just return SEARCH_SWEEP_RADIUS
         * anyway) - this branch is what makes a non-Search team's
         * ACTUAL sweep-task work contribute real coverage, scaled by
         * the same capability matrix that won it the task in the first
         * place, instead of the zero-contribution gap this used to be. */
        if(team->specialization != SPEC_SEARCH && !on_active_sweep_task)
            continue;

        uint8_t radius = (team->specialization == SPEC_SEARCH)
            ? SEARCH_SWEEP_RADIUS : sweep_radius_for_team(team);

        uint8_t team_district = zone_id_for_location(team->loc_x, team->loc_y);

        for(int16_t dx = -(int16_t)radius; dx <= (int16_t)radius; dx++)
        {
            for(int16_t dy = -(int16_t)radius; dy <= (int16_t)radius; dy++)
            {
                if(dx * dx + dy * dy > (int32_t)radius * radius)
                    continue;

                int16_t cx = (int16_t)(team->loc_x + dx);
                int16_t cy = (int16_t)(team->loc_y + dy);

                /* M37: this sweep cell may belong to a different district
                 * than the team's own - crediting coverage to team_district
                 * here (rather than the cell's own district) misattributes
                 * boundary-straddling coverage. The straight fix (credit
                 * zone_id_for_location(cx,cy) instead) was tried and shelved
                 * for now: it's correct but shifts district coverage %s,
                 * which feed sweep-cycle completion and thus incident
                 * dispatch timing - moving the P1 quarantine count 44->47
                 * even as no-path hits improved 204->196. Needs the same
                 * coverage-accounting design decision as the deferred triad.
                 * The counter below measures how often the misattribution
                 * would apply. */
                if(zone_id_for_location(cx, cy) != team_district)
                    g_diag_sweep_cross_district_cells++;

                search_zone_mark_cell_covered(team_district, cx, cy, current_tick);
            }
        }
    }

    for(uint16_t i = 0; i < NUM_INCIDENTS; i++)
    {
        IncidentState_t *incident = &g_incidents[i];
        if(!incident->slot_occupied || incident->discovered)
            continue;

        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            TeamState_t *team = &g_teams[t];
            int32_t radius;

            uint8_t on_active_sweep_task =
                (team->assigned_task_pool == TASK_POOL_SYSTEM &&
                 team->assigned_task_slot < NUM_SYSTEM_TASKS &&
                 g_system_tasks[team->assigned_task_slot].incident_type == INCIDENT_UNSEARCHED_SECTOR);

            if(team->specialization == SPEC_SEARCH)
                radius = SEARCH_SWEEP_RADIUS;
            else if(on_active_sweep_task)
                radius = sweep_radius_for_team(team);  /* actively sweeping: scaled radius, not gated on ON_SCENE - this task never reaches ON_SCENE by design */
            else if(team->status == STATUS_ON_SCENE)
                radius = OTHER_TEAM_REVEAL_RADIUS;
            else
                continue;

            int32_t dx = incident->loc_x - team->loc_x;
            int32_t dy = incident->loc_y - team->loc_y;
            if(dx * dx + dy * dy <= radius * radius)
            {
                incident->discovered = 1;
                extern uint32_t g_diag_discovered_by_type[NUM_INCIDENT_TYPES];
                g_diag_discovered_by_type[incident->incident_type]++;
                break;
            }
        }
    }
}

int begin_return_to_safe_zone(TeamState_t *team) //incident
{
    /* Team-intrinsic now - no system task slot backs this, so there is
     * nothing to dedup against a pool scan. A caller that invokes this
     * while the team is already heading_to_rest would just redundantly
     * recompute the same destination and re-set the same fields (still
     * safe), but every current call site (see incident.h) either checks
     * heading_to_rest itself first (abandon_task_for_recovery()) or can
     * only run for a team that provably isn't already heading to rest
     * (release_team() clears a just-completed task; team.c's
     * STATUS_AVAILABLE case can't fire while status is EN_ROUTE) - see
     * this project's own review notes on that call-site audit. */
    uint8_t safezone = find_nearest_safe_zone(team);
    if(safezone == NO_SAFE_ZONE)
    {
        return NO_ASSIGNMENT;
    }

    team->assigned_task_pool = TASK_POOL_NONE;
    team->assigned_task_slot = NO_ASSIGNMENT;
    team->resting_zone_id = safezone;
    team->heading_to_rest = 1;
    team->status = STATUS_EN_ROUTE;

    return safezone;
}


int create_equipment_cache_incident(TeamState_t *team) //incident
{
    IncidentState_t *task = find_free_system_task_slot();
    if(task == NULL)
        task = find_reclaimable_system_task_slot();
    if(task == NULL)
        return NO_ASSIGNMENT;

    task->incident_id = allocate_incident_id();

    apply_incident_archetype(task, INCIDENT_EQUIPMENT_CACHE);

    task->status = INC_OPEN;
    task->assigned_team = NO_ASSIGNMENT;
    task->support_team = NO_ASSIGNMENT;

    task->loc_x = team->loc_x;
    task->loc_y = team->loc_y;

    task->victim_count = 0;
    task->reported_at_tick = g_current_tick;
    task->structure_type = 0;
    task->passive_timer = 0;
    task->spawned_by = team->team_id;
    task->work_remaining = 0;

    return task->incident_id;
}

int incident_degradation_radius(IncidentType_t type)
{
    switch(type)
    {
        case(INCIDENT_STRUCTURAL_COLLAPSE):return 2;
        case(INCIDENT_MASS_CASUALTY): return 1;
        case(INCIDENT_CHEMICAL_EXPOSURE): return 3;
        case(INCIDENT_GAS_LEAK):return 3;
        case(INCIDENT_ACTIVE_FIRE):return 2;
        default: return 0;
    }
}

int incident_base_degradation(IncidentType_t type)
{
    switch(type)
    {
        case(INCIDENT_STRUCTURAL_COLLAPSE):return 35;
        case(INCIDENT_MASS_CASUALTY): return 15;
        case(INCIDENT_CHEMICAL_EXPOSURE): return 55;
        case(INCIDENT_GAS_LEAK):return 45;
        case(INCIDENT_ACTIVE_FIRE):return 65;
        default: return 0;
    }
}

static int is_valid_team_id(uint8_t team_id)
{
    return team_id < NUM_TEAMS;
}

static void clear_task_slot(TaskPool_t pool, IncidentState_t *task)
{
    if(task == NULL)
        return;

    if(pool != TASK_POOL_INCIDENT && pool != TASK_POOL_SYSTEM)
        return;

    task->loc_x = 0;
    task->loc_y = 0;
    task->priority = PRIORITY_LOW;
    task->incident_type = INCIDENT_UNSEARCHED_SECTOR;
    task->req_capability = SPEC_SEARCH;
    task->status = INC_RESOLVED;
    task->victim_count = 0;
    task->hazard_flags = 0;
    task->reported_at_tick = 0;
    task->structure_type = 0;
    task->assigned_team = NO_ASSIGNMENT;
    task->passive_effect = PASSIVE_NONE;
    task->passive_timer = 0;
    task->spawned_by = NO_ASSIGNMENT;
    task->support_team = NO_ASSIGNMENT;
    task->work_remaining = 0;
    task->mission_fault_active_count = 0;
    task->mission_fault_original_priority = 0;
    task->mission_fault_original_victim_count = 0;
    task->unreachable_streak = 0;
    task->discovered = 1;  /* default until fog-of-war reveal logic lands */
    task->evac_safezone_id = 0;
    task->evac_gateway_id = NO_EVAC_TARGET;
    task->evac_capacity = 0;
    task->slot_occupied = 0;
}

void initialize_incidents(void)
{
    g_incident_counter = 0;
    g_next_incident_evaluation_tick = 0;

    g_last_generation_total_weight = 0;
    g_last_generation_type_roll = 0;

    for(uint32_t i = 0; i < GENERATION_PROFILE_COUNT; i++)
        g_last_generation_weights[i] = 0;

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        clear_task_slot(TASK_POOL_INCIDENT, &g_incidents[i]);
    }

    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        clear_task_slot(TASK_POOL_SYSTEM, &g_system_tasks[i]);
    }
}

void complete_return_to_rest(TeamState_t *team)
{
    if(team == NULL)
        return;

    team->status = STATUS_RESTING;
    team->heading_to_rest = 0;
}

/*
 * Deterministic, fault-adjusted work rate for a single team - see this
 * function's declaration in incident.h for why it's shared (not
 * duplicated) between update_incident_work() below and core/
 * mission_link.c's mission_link_build_status().
 *
 * Base rate scales linearly with equipment_level across the same
 * [WORK_RATE_MIN, WORK_RATE_MAX] band the STREAM_WORK_RATE RNG roll
 * draws from, so a board-hosted team's reported figures stay
 * comparable at a glance to a remote (RNG-rolled) team's. Faults then
 * degrade that base rate, same spirit as team.c's change_stamina(
 * -severity[FAULT_CLASS_RESOURCE]*0.12f) drain: FAULT_CLASS_RESOURCE
 * (equipment/supply shortfall) and FAULT_CLASS_MOBILITY (impaired
 * physical operation, not just travel - move_team() already applies
 * FAULT_CLASS_MOBILITY to travel; this is its on-scene-output
 * counterpart) each shave one unit off the rate per point of active
 * severity. Floored at WORK_RATE_MIN rather than allowed to reach 0:
 * this models a *degraded* team, not a stalled one - a team reporting
 * truly zero progress would be visually indistinguishable from an idle
 * team (assigned_incident_id == MISSION_LINK_NO_INCIDENT) on the
 * dashboard, which would misrepresent an actively-struggling team as
 * not working at all. A team that can no longer make even minimal
 * progress is expected to get pulled off via abandon_task_for_recovery()
 * (team.c) rather than sit here reporting 0.
 *
 * Independent of status/assignment - this is queried for every team's
 * MissionLinkStatus_t report (mission_link_build_status()), including
 * idle teams, since the field documents "currently-effective rate",
 * not "rate currently being applied to something".
 */
uint8_t team_base_work_rate(uint8_t equipment_level)
{
    uint32_t base_range = (uint32_t)(WORK_RATE_MAX - WORK_RATE_MIN);
    uint32_t scaled = ((uint32_t)equipment_level * base_range) / (uint32_t)EQ_MAX_LEVEL;

    return (uint8_t)(WORK_RATE_MIN + scaled);
}

uint8_t team_effective_work_rate(const TeamState_t *team)
{
    uint8_t rate;

    if(team == NULL)
        return WORK_RATE_MIN;

    rate = team_base_work_rate(team->equipment_level);

    if(team->fault_state.active[FAULT_CLASS_RESOURCE])
    {
        uint8_t severity = team->fault_state.severity[FAULT_CLASS_RESOURCE];
        rate = (rate > severity) ? (uint8_t)(rate - severity) : (uint8_t)WORK_RATE_MIN;
    }

    if(team->fault_state.active[FAULT_CLASS_MOBILITY])
    {
        uint8_t severity = team->fault_state.severity[FAULT_CLASS_MOBILITY];
        rate = (rate > severity) ? (uint8_t)(rate - severity) : (uint8_t)WORK_RATE_MIN;
    }

    if(rate < WORK_RATE_MIN) rate = WORK_RATE_MIN;
    if(rate > WORK_RATE_MAX) rate = WORK_RATE_MAX;

    return rate;
}

/* core/cbba.c's cbba_team_is_local() - public, but deliberately not
 * declared in cbba.h (see that function's own comment for why:
 * "public, but not declared in cbba.h" is an established convention in
 * that file for anything a standalone selftest needs to reach without
 * standing up a full mission). Forward-declared here at the one call
 * site that needs it, matching how this project already forward-
 * declares this kind of cross-file-but-undeclared function elsewhere
 * (e.g. board/STM32N6570-DK/main_board.c's board_snapshot_ring_drain()). */
extern bool cbba_team_is_local(uint8_t team_id);

/*
 * team_id's own work-rate contribution for this tick: the deterministic
 * board-side formula for a locally-hosted team, the original
 * STREAM_WORK_RATE RNG roll for everyone else (a team no board here
 * hosts has no local execution state to compute a real rate from - see
 * mission_link.h's MissionLinkFault_t doc on why a board's g_teams[]
 * entry for a remote team isn't kept live). This is the single
 * decision point both the primary and support-team contributions below
 * route through, so the two can never pick different rules for the
 * same team.
 */
static uint8_t incident_work_rate_for_team(uint8_t team_id)
{
    if(cbba_team_is_local(team_id))
        return team_effective_work_rate(&g_teams[team_id]);

    return random_range_stream(STREAM_WORK_RATE, WORK_RATE_MIN, WORK_RATE_MAX);
}

static void update_incident_work(IncidentState_t *incident) //incident
{
    if(incident->status!=INC_IN_PROGRESS) return;
    if(incident->incident_type == INCIDENT_UNSEARCHED_SECTOR) return;
    if(!is_valid_team_id(incident->assigned_team)) return;
    if(g_teams[incident->assigned_team].status!=STATUS_ON_SCENE) return;
    if(g_teams[incident->assigned_team].equipment_level == 0) return;

    extern uint32_t g_diag_work_progress_calls;
    g_diag_work_progress_calls++;

    uint8_t work_rate = incident_work_rate_for_team(incident->assigned_team);

    if(is_valid_team_id(incident->support_team) && g_teams[incident->support_team].status==STATUS_ON_SCENE)
        work_rate += incident_work_rate_for_team(incident->support_team);

    if(incident->work_remaining<=work_rate)
    {
        incident->work_remaining=0;
        complete_incident_stage(incident);
        return;
    }
    incident->work_remaining-=work_rate;
}

void update_all_incident_work(void) //incident
{
    clear_stale_equipment_cache_requests();

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        if(!g_incidents[i].slot_occupied)
            continue;

        update_incident_work(&g_incidents[i]);
    }

    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        if(!g_system_tasks[i].slot_occupied)
            continue;

        update_incident_work(&g_system_tasks[i]);
    }
}

void update_all_passive_effects(void)
{
    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        IncidentState_t *incident = &g_incidents[i];

        if(!incident->slot_occupied ||
           incident->status == INC_RESOLVED ||
           incident->passive_effect == PASSIVE_NONE)
        {
            continue;
        }

        switch(incident->passive_effect)
        {
            case PASSIVE_GRID_DEGRADE:
                apply_incident_grid_degradation(incident);
                break;

            case PASSIVE_COMMS_BOOST:
                break;

            default:
                break;
        }

        if(incident->passive_timer > 0)
        {
            incident->passive_timer--;
            if(incident->passive_timer == 0)
                incident->passive_effect = PASSIVE_NONE;
        }
    }
}

static bool dispatch_next_queued_incident(TeamState_t *team)
{
    if(team == NULL)
        return false;

    while(team->incident_queue_index + 1 < team->incident_queue_size)
    {
        team->incident_queue_index++;
        uint8_t q = team->incident_queue_index;
        uint8_t incident_slot = team->incident_queue_slot[q];
        uint32_t incident_id = team->incident_queue_id[q];
        if(incident_slot >= NUM_INCIDENTS)
            continue;
        IncidentState_t *incident = &g_incidents[incident_slot];
        if(!incident->slot_occupied)
            continue;
        if(incident->incident_id != incident_id)
            continue;
        if(incident->status != INC_ASSIGNED)
            continue;
        if(incident->assigned_team != team->team_id)
            continue;
        team->assigned_task_pool = TASK_POOL_INCIDENT;
        team->assigned_task_slot = incident_slot;
        team->status = STATUS_EN_ROUTE;
        return true;
    }
    team->incident_queue_size = 0;
    team->incident_queue_index = 0;

    for(uint8_t q = 0; q < CBBA_MAX_BUNDLE_SIZE; q++)
    {
        team->incident_queue_slot[q] = NO_ASSIGNMENT;
        team->incident_queue_id[q] = 0;
    }

    return false;
}

static void release_team(uint8_t team_id)
{
    if(!is_valid_team_id(team_id))
        return;

    TeamState_t *team = &g_teams[team_id];

    team->assigned_task_pool = TASK_POOL_NONE;
    team->assigned_task_slot = NO_ASSIGNMENT;
    if(dispatch_next_queued_incident(team))
        return;

    if(team_needs_safe_zone(team))
    {
        if(begin_return_to_safe_zone(team) == NO_ASSIGNMENT)
            team_set_available(team);
    }
    else
    {
        team_set_available(team);
    }
}

static void release_main_task_team(TaskPool_t pool, IncidentState_t *task)
{
    if(task == NULL ||
       (pool != TASK_POOL_INCIDENT && pool != TASK_POOL_SYSTEM))
    {
        return;
    }

    uint8_t team_id = task->assigned_team;

    if(pool == TASK_POOL_INCIDENT && team_id < NUM_TEAMS)
    {
        extern uint32_t g_diag_stage_completions[NUM_TEAMS];
        g_diag_stage_completions[team_id]++;
    }

    task->assigned_team = NO_ASSIGNMENT;
    release_team(team_id);
}

static void release_all_task_teams(TaskPool_t pool, IncidentState_t *task)
{
    if(task == NULL ||
       (pool != TASK_POOL_INCIDENT && pool != TASK_POOL_SYSTEM))
    {
        return;
    }

    uint8_t team_id = task->assigned_team;
    uint8_t support_id = task->support_team;

    task->assigned_team = NO_ASSIGNMENT;
    task->support_team = NO_ASSIGNMENT;

    release_team(team_id);

    if(support_id != team_id)
        release_team(support_id);
}

static void complete_task(TaskPool_t pool, IncidentState_t *task)
{
    if(task == NULL ||
       (pool != TASK_POOL_INCIDENT && pool != TASK_POOL_SYSTEM))
    {
        return;
    }

    task->status = INC_RESOLVED;

    if(pool == TASK_POOL_INCIDENT && task->assigned_team < NUM_TEAMS)
    {
        extern uint32_t g_diag_incidents_resolved[NUM_TEAMS];
        g_diag_incidents_resolved[task->assigned_team]++;
    }
    if(pool == TASK_POOL_INCIDENT)
    {
        extern uint32_t g_diag_resolved_by_type[NUM_INCIDENT_TYPES];
        g_diag_resolved_by_type[task->incident_type]++;
    }

    /* Log every resolved task, including system tasks. Uses task's own
     * state directly rather than snapshot_record()'s live team lookup -
     * see snapshot_record_resolved() for why that distinction matters
     * here specifically. */
    snapshot_record_resolved(g_current_tick, pool, task);

    if(pool == TASK_POOL_SYSTEM)
    {
        uint8_t team_id = task->assigned_team;
        uint8_t support_id = task->support_team;

        task->assigned_team = NO_ASSIGNMENT;
        task->support_team = NO_ASSIGNMENT;

        clear_task_slot(TASK_POOL_SYSTEM, task);

        release_team(team_id);
        if(support_id != team_id)
            release_team(support_id);

        return;
    }

    release_all_task_teams(TASK_POOL_INCIDENT, task);
    clear_task_slot(TASK_POOL_INCIDENT, task);
}

void complete_incident_stage(IncidentState_t *incident) //incident
{
    switch(incident->req_capability)
    {
        case(SPEC_PERIMETER):
            if(incident->incident_type==INCIDENT_STRUCTURAL_COLLAPSE)
            {
                if(random_range_stream(STREAM_STAGE_BRANCH,1,100)<=60)
                {
                    incident->req_capability=SPEC_MEDICAL;

                }
                else
                {
                    incident->req_capability=SPEC_EXTRACTION;
                }
                snapshot_record(g_current_tick, EVENT_STAGE_COMPLETE, incident->assigned_team);
                release_main_task_team(TASK_POOL_INCIDENT, incident);
                incident->status=INC_OPEN;
                incident->work_remaining = 100;
            }
            else if(incident->incident_type==INCIDENT_CONFINED_SPACE)
            {
                if(random_range_stream(STREAM_STAGE_BRANCH,1,100)<=40)
                {
                    incident->req_capability=SPEC_MEDICAL;

                }
                else
                {
                    incident->req_capability=SPEC_EXTRACTION;
                }
                snapshot_record(g_current_tick, EVENT_STAGE_COMPLETE, incident->assigned_team);
                release_main_task_team(TASK_POOL_INCIDENT, incident);
                incident->status=INC_OPEN;
                incident->work_remaining = 100;
            }
            else if(incident->incident_type == INCIDENT_CHEMICAL_EXPOSURE)
            {
                incident->req_capability = SPEC_MEDICAL;
                snapshot_record(g_current_tick, EVENT_STAGE_COMPLETE, incident->assigned_team);
                release_main_task_team(TASK_POOL_INCIDENT, incident);
                incident->status = INC_OPEN;
                incident->work_remaining = 100;
            }
            else if(incident->incident_type == INCIDENT_GAS_LEAK)
            {
                complete_task(TASK_POOL_INCIDENT, incident);
            }
            else if(incident->incident_type == INCIDENT_ACTIVE_FIRE)
            {
                incident->req_capability = SPEC_EXTRACTION;
                snapshot_record(g_current_tick, EVENT_STAGE_COMPLETE, incident->assigned_team);
                release_main_task_team(TASK_POOL_INCIDENT, incident);
                incident->status = INC_OPEN;
                incident->work_remaining = 100;
            }
            break;
        case(SPEC_MEDICAL):
            if(incident->incident_type == INCIDENT_MASS_CASUALTY ||
               incident->incident_type == INCIDENT_CHEMICAL_EXPOSURE)
            {
                complete_task(TASK_POOL_INCIDENT, incident);
            }
            else
            {
                incident->req_capability = SPEC_EXTRACTION;
                snapshot_record(g_current_tick, EVENT_STAGE_COMPLETE, incident->assigned_team);
                release_main_task_team(TASK_POOL_INCIDENT, incident);
                incident->status = INC_OPEN;
                incident->work_remaining = 100;
            }
            break;
        case(SPEC_EXTRACTION):
            if(incident->incident_type == INCIDENT_EVACUEE_OFFBOARD)
            {
                SafeZone_t *safezone = &g_safezones[incident->evac_safezone_id];

                if(incident->victim_count == 0)
                {
                    if(safezone->evacuee_count == 0)
                    {
                        uint8_t safezone_id = incident->evac_safezone_id;
                        complete_task(TASK_POOL_SYSTEM, incident);
                        g_safezone_evacuation_active[safezone_id] = 0;
                        return;
                    }

                    incident->evac_gateway_id = find_best_gateway(safezone);

                    if(incident->evac_gateway_id == NO_EVAC_TARGET)
                    {
                        uint8_t safezone_id = incident->evac_safezone_id;
                        complete_task(TASK_POOL_SYSTEM, incident);
                        g_safezone_evacuation_active[safezone_id] = 0;
                        return;
                    }

                    uint8_t load_count = safezone->evacuee_count;

                    if(load_count > incident->evac_capacity)
                        load_count = incident->evac_capacity;

                    incident->victim_count = load_count;
                    safezone->evacuee_count -= load_count;

                    incident->loc_x =
                        g_evacuation_gateways[incident->evac_gateway_id].loc_x;

                    incident->loc_y =
                        g_evacuation_gateways[incident->evac_gateway_id].loc_y;

                    incident->status = INC_ASSIGNED;

                    g_teams[incident->assigned_team].status =
                        STATUS_EN_ROUTE;

                    incident->work_remaining = 100;

                    return;
                }

                incident->victim_count = 0;

                {
                    uint8_t safezone_id = incident->evac_safezone_id;
                    complete_task(TASK_POOL_SYSTEM, incident);
                    g_safezone_evacuation_active[safezone_id] = 0;
                }

                return;
            }

            /* General rescue completion (not EVACUEE_OFFBOARD, which is
             * the separate zone->gateway outflow above): the incident's
             * victim_count has been rescued but never had anywhere to
             * go - deliver them to the nearest safe zone, clamped to its
             * remaining capacity so this can't push evacuee_count past
             * what the zone can actually hold. This is the missing
             * inflow half of the evacuee_count pipeline; the outflow
             * half (safe zone -> gateway) already existed above. */
            {
                uint8_t safezone_id =
                    find_nearest_safe_zone(&g_teams[incident->assigned_team]);

                if(safezone_id != NO_SAFE_ZONE)
                {
                    SafeZone_t *safezone = &g_safezones[safezone_id];
                    uint8_t room =
                        (safezone->capacity > safezone->evacuee_count)
                            ? (uint8_t)(safezone->capacity - safezone->evacuee_count)
                            : 0u;
                    uint8_t delivered =
                        (incident->victim_count < room) ? incident->victim_count : room;

                    safezone->evacuee_count =
                        (uint8_t)(safezone->evacuee_count + delivered);
                }
            }

            complete_task(TASK_POOL_INCIDENT, incident);
            break;

        case(SPEC_SEARCH):
            if(incident->incident_type==INCIDENT_UNSEARCHED_SECTOR)
            {
                if(random_range_stream(STREAM_STAGE_BRANCH,1,100)<=70)
                {
                    incident->req_capability=SPEC_MEDICAL;
                }
                else
                {
                    incident->req_capability=SPEC_EXTRACTION;

                }
                snapshot_record(g_current_tick, EVENT_STAGE_COMPLETE, incident->assigned_team);
                release_main_task_team(TASK_POOL_INCIDENT, incident);
                incident->status=INC_OPEN;
                incident->work_remaining = 100;
            }
            else if(incident->incident_type==INCIDENT_SURVIVOR_SIGNAL)
            {
                incident->req_capability=SPEC_EXTRACTION;
                snapshot_record(g_current_tick, EVENT_STAGE_COMPLETE, incident->assigned_team);
                release_main_task_team(TASK_POOL_INCIDENT, incident);
                incident->status=INC_OPEN;
                incident->work_remaining = 100;
            }
            break;

        case(SPEC_SUPPORT):
            if(incident->incident_type == INCIDENT_EQUIPMENT_CACHE && incident->spawned_by != NO_ASSIGNMENT &&incident->spawned_by < NUM_TEAMS)
            {
                g_teams[incident->spawned_by].equipment_level = EQ_MAX_LEVEL;
                complete_task(TASK_POOL_SYSTEM, incident);
            }
            break;

        default:
            break;
    }

    /* Keep the execution twin consistent with any assignment or status
       changes made inside the stage transitions above */
    cbba_sync_mission_execution_twin();
}
    

int incident_needs_support(const IncidentState_t *incident) //incident
{
    switch(incident->incident_type)
    {
        case(INCIDENT_STRUCTURAL_COLLAPSE):
        case(INCIDENT_MASS_CASUALTY): return 1;
        default: return 0;
    }
}

static float get_min_stamina(const TeamState_t *team) //incident

{
    switch(team->fitness_tier)
    {
        case(ARDUOUS): return ARDUOUS_MIN_STAMINA;
        case(MODERATE): return MODERATE_MIN_STAMINA;
        case(LIGHT): return LIGHT_MIN_STAMINA;
        default: return LIGHT_MIN_STAMINA;
    }
}

static int team_needs_safe_zone(const TeamState_t *team) //incident
{
    if(team->stamina<=get_min_stamina(team))
    {
        return 1;
    }
    else if(team->equipment_level<=TEAM_NEEDS_SAFE_ZONE_EQUIPMENT_THRESHOLD)
    {
        return 1;
    }
    else return 0;
}

uint8_t find_nearest_safe_zone(const TeamState_t *team) //incident
{
    uint8_t zone=NO_SAFE_ZONE;
    uint16_t min=UINT16_MAX;
    for(int i=0;i<g_num_safe_zones;i++)
    {
        uint16_t distance=find_path_cost(team->loc_x,team->loc_y,g_safezones[i].loc_x,g_safezones[i].loc_y);
        if(min>distance){ min=distance; zone=i;}
    }
    return zone;
}


/* Deterministic last resort for initialize_evacuation_gateways()'s random-
 * retry loop below: scans the whole perimeter in a fixed order for the
 * first cell not already claimed by an earlier gateway (index < i) or a
 * safe zone, and is therefore guaranteed to succeed as long as any free
 * perimeter cell exists at all - with NUM_EVACUATION_GATEWAYS (3) and
 * MAX_SAFE_ZONES (4) claiming at most 7 cells out of ~2*(GRID_WIDTH+
 * GRID_HEIGHT)-4 (~140) perimeter cells, that's always true in practice.
 * Water is deliberately NOT excluded here (unlike the random draw above) -
 * this only runs after the random draw has already failed
 * EVACUATION_GATEWAY_MAX_RANDOM_ATTEMPTS times, so a water-heavy world is
 * exactly the case where insisting on dry land could exhaust the scan too;
 * any dry, unclaimed cell is still preferred first (the loop checks all
 * dry cells before considering water ones), same failure-mode-avoidance
 * spirit as environment.c's stage_generate_infrastructure() no longer
 * requiring has_road() on its own gateway candidates. */
static uint8_t find_free_perimeter_cell(int gateway_index, int16_t *out_x, int16_t *out_y)
{
    for(int pass = 0; pass < 2; pass++)
    {
        uint8_t require_dry = (pass == 0);

        for(int x = 0; x < GRID_WIDTH; x++)
        {
            for(int y = 0; y < GRID_HEIGHT; y++)
            {
                if(x != 0 && y != 0 && x != GRID_WIDTH - 1 && y != GRID_HEIGHT - 1)
                    continue;

                if(require_dry && !environment_cell_is_navigable(x, y))
                    continue;

                uint8_t claimed = 0;

                for(int j = 0; j < gateway_index; j++)
                {
                    if(g_evacuation_gateways[j].loc_x == x && g_evacuation_gateways[j].loc_y == y)
                    {
                        claimed = 1;
                        break;
                    }
                }

                if(!claimed)
                {
                    for(int j = 0; j < g_num_safe_zones; j++)
                    {
                        if(g_safezones[j].loc_x == x && g_safezones[j].loc_y == y)
                        {
                            claimed = 1;
                            break;
                        }
                    }
                }

                if(!claimed)
                {
                    *out_x = (int16_t)x;
                    *out_y = (int16_t)y;
                    return 1;
                }
            }
        }
    }

    return 0;
}

#define EVACUATION_GATEWAY_MAX_RANDOM_ATTEMPTS 100

unsigned long g_diag_evacuation_gateway_fallback_scan_count = 0;

void initialize_evacuation_gateways(uint32_t seed) //incident
{
    random_seed_stream(STREAM_EVACUATION_GATEWAYS, derive_seed(seed, STREAM_EVACUATION_GATEWAYS));

    for(int i = 0; i < NUM_EVACUATION_GATEWAYS; i++)
    {
        int valid_position = 0;
        int deterministic_site_rejected = 0;
        int random_attempts = 0;

        while(!valid_position)
        {
            /* Bug fix: this retry loop used to have no iteration cap - a
             * pathological run of colliding random draws (vanishingly
             * unlikely given how few cells are ever claimed relative to
             * the perimeter's size, but not impossible) would spin
             * forever with no external watchdog to catch it on this
             * single-task board model (see AUDIT.md item #21). After a
             * bounded number of random attempts, fall back to a
             * deterministic scan that is guaranteed to terminate. */
            if(random_attempts >= EVACUATION_GATEWAY_MAX_RANDOM_ATTEMPTS)
            {
                if(find_free_perimeter_cell(i, &g_evacuation_gateways[i].loc_x,
                                             &g_evacuation_gateways[i].loc_y))
                {
                    g_diag_evacuation_gateway_fallback_scan_count++;
                    break;
                }

                /* Should be unreachable (see find_free_perimeter_cell()'s
                   own doc), but if every perimeter cell is somehow
                   claimed, stop retrying rather than loop forever - accept
                   whatever position was last drawn and let the duplicate-
                   detection above generate_safe_zones() already accepts
                   the same tradeoff for the analogous MAX_ATTEMPTS
                   fallback there. */
                break;
            }

            int16_t gateway_x = 0;
            int16_t gateway_y = 0;

            if(!deterministic_site_rejected &&
               environment_find_gateway_site((uint8_t)i, &gateway_x, &gateway_y))
            {
                g_evacuation_gateways[i].loc_x = gateway_x;
                g_evacuation_gateways[i].loc_y = gateway_y;
            }
            else
            {
                random_attempts++;
                int edge = random_range_stream(STREAM_EVACUATION_GATEWAYS,0, 3);

                switch(edge)
                {
                    case 0:
                        g_evacuation_gateways[i].loc_x = random_range_stream(STREAM_EVACUATION_GATEWAYS,0, GRID_WIDTH - 1);
                        g_evacuation_gateways[i].loc_y = 0;
                        break;

                    case 1:
                        g_evacuation_gateways[i].loc_x = random_range_stream(STREAM_EVACUATION_GATEWAYS,0, GRID_WIDTH - 1);
                        g_evacuation_gateways[i].loc_y = GRID_HEIGHT - 1;
                        break;

                    case 2:
                        g_evacuation_gateways[i].loc_x = 0;
                        g_evacuation_gateways[i].loc_y = random_range_stream(STREAM_EVACUATION_GATEWAYS,0, GRID_HEIGHT - 1);
                        break;

                    case 3:
                        g_evacuation_gateways[i].loc_x = GRID_WIDTH - 1;
                        g_evacuation_gateways[i].loc_y = random_range_stream(STREAM_EVACUATION_GATEWAYS,0, GRID_HEIGHT - 1);
                        break;
                }
            }

            valid_position = 1;

            for(int j = 0; j < i; j++)
            {
                if(g_evacuation_gateways[i].loc_x == g_evacuation_gateways[j].loc_x && g_evacuation_gateways[i].loc_y == g_evacuation_gateways[j].loc_y)
                {
                    valid_position = 0;
                    break;
                }
            }

            for(int j = 0; j < g_num_safe_zones; j++)
            {
                if(g_evacuation_gateways[i].loc_x == g_safezones[j].loc_x && g_evacuation_gateways[i].loc_y == g_safezones[j].loc_y)
                {
                    valid_position = 0;
                    break;
                }
            }

            if(!valid_position)
                deterministic_site_rejected = 1;
        }

        g_evacuation_gateways[i].gateway_id = i;
        environment_register_gateway((uint8_t)i,
                                     g_evacuation_gateways[i].loc_x,
                                     g_evacuation_gateways[i].loc_y);
    }
}

int create_evacuee_offboard_incident(uint8_t safezone_id)
{
    if(safezone_id >= g_num_safe_zones)
        return NO_ASSIGNMENT;

    SafeZone_t *safezone = &g_safezones[safezone_id];

    if(safezone->evacuee_count == 0)
        return NO_ASSIGNMENT;

    /* g_safezone_evacuation_active[] already existed (zeroed at world
     * reset) but was never actually set or read anywhere - this was the
     * one missing wire. O(1) per-zone dedup instead of the old O(20)
     * scan over every system-task slot, and correctly scoped per safe
     * zone rather than the old global "only one EVACUEE_OFFBOARD task
     * anywhere" rule the singleton forced. */
    if(g_safezone_evacuation_active[safezone_id])
        return NO_ASSIGNMENT;

    /* Deliberately TASK_POOL_SYSTEM, not the incident pool - this must
     * still be able to run even when the real-incident pool (fires,
     * mass casualty, etc.) is completely full, same reasoning as
     * UNSEARCHED_SECTOR and EQUIPMENT_CACHE. */
    IncidentState_t *task = find_free_system_task_slot();

    if(task == NULL)
        return NO_ASSIGNMENT;

    task->incident_id = allocate_incident_id();

    apply_incident_archetype(task, INCIDENT_EVACUEE_OFFBOARD);

    task->status = INC_OPEN;
    task->assigned_team = NO_ASSIGNMENT;
    task->support_team = NO_ASSIGNMENT;

    task->loc_x = safezone->loc_x;
    task->loc_y = safezone->loc_y;

    task->victim_count = 0;   /* live evacuee headcount currently being carried */
    task->reported_at_tick = g_current_tick;
    task->structure_type = 0;
    task->passive_timer = 0;
    task->spawned_by = NO_ASSIGNMENT;
    task->work_remaining = 100;

    task->evac_safezone_id = safezone_id;
    task->evac_gateway_id = NO_EVAC_TARGET;
    task->evac_capacity = (uint8_t)random_range_stream(
        STREAM_EVACUATION_TEAMS, MIN_EVACUATION_TEAM_CAPACITY, MAX_EVACUATION_TEAM_CAPACITY);

    g_safezone_evacuation_active[safezone_id] = 1;

    return task->incident_id;
}

/* generate_evacuee_offboard_incidents() below is EVACUEE_OFFBOARD's
 * missing trigger - the piece that was never wired up (this function
 * used to have zero callers anywhere). Tops up every safe zone with a
 * waiting backlog and no run already in flight, same tick-cadence shape
 * as search_generate_sweep_tasks(). */
void generate_evacuee_offboard_incidents(void)
{
    for(uint8_t z = 0; z < g_num_safe_zones; z++)
    {
        create_evacuee_offboard_incident(z);
    }
}

void initialize_evacuation_teams(void)
{
    /* The old singleton EvacuationTeam_t is gone (its fields now live
     * directly on each EVACUEE_OFFBOARD task - see IncidentState_t) but
     * this still needs to run once per world/seed to seed the RNG
     * stream create_evacuee_offboard_incident() draws evac_capacity
     * from, same as every other STREAM_* seeded at world init. */
    random_seed_stream(STREAM_EVACUATION_TEAMS, derive_seed(g_world_seed, STREAM_EVACUATION_TEAMS));
}

static uint8_t find_best_gateway(const SafeZone_t *safezone) //incident
{
    uint16_t lowest_cost=UINT16_MAX;
    uint8_t best_gateway=NO_EVAC_TARGET;
    for(int i=0;i<NUM_EVACUATION_GATEWAYS;i++)
    {
        int path_cost=find_next_path_step(safezone->loc_x,safezone->loc_y,g_evacuation_gateways[i].loc_x,g_evacuation_gateways[i].loc_y,1,NULL,NULL);
        if(path_cost>=0 && path_cost<lowest_cost)
        {
            lowest_cost=path_cost;
            best_gateway=i;
        }
    }
    return best_gateway;
}


static IncidentState_t *find_free_incident_slot(void)
{
    for(int i = 0; i < NUM_INCIDENTS; i++)
    {
        if(!g_incidents[i].slot_occupied)
        {
            g_incidents[i].slot_occupied = 1;
            return &g_incidents[i];
        }
    }
    return NULL;
}

static IncidentState_t *find_free_system_task_slot(void)
{
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        if(!g_system_tasks[i].slot_occupied)
        {
            g_system_tasks[i].slot_occupied = 1;
            return &g_system_tasks[i];
        }
    }

    return NULL;
}

/* Used only when the system task pool (NUM_SYSTEM_TASKS, shared by
 * UNSEARCHED_SECTOR sweeps, EQUIPMENT_CACHE, and RETURN_TO_SAFE_ZONE)
 * is completely full and a team urgently needs to be routed to a safe
 * zone to recover. Without this, a full pool of low-priority sweep
 * tasks permanently starves RETURN_TO_SAFE_ZONE task creation, and a
 * team's equipment/stamina never recovers. A discretionary area sweep
 * must never outrank a team's own recovery, so this reclaims the
 * first still-OPEN (not yet claimed) UNSEARCHED_SECTOR slot - it
 * simply regenerates next tick if still needed. */
static IncidentState_t *find_reclaimable_system_task_slot(void)
{
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        IncidentState_t *task = &g_system_tasks[i];

        if(task->slot_occupied &&
           task->incident_type == INCIDENT_UNSEARCHED_SECTOR &&
           task->status == INC_OPEN)
        {
            extern uint32_t g_diag_sweep_reclaimed_for_safezone;
            g_diag_sweep_reclaimed_for_safezone++;

            /* Start this district's reclaim cooldown before clearing
             * the task - clear_task_slot() doesn't touch loc_x/loc_y,
             * but read it here for clarity/safety rather than relying
             * on that. Without this cooldown, search_generate_sweep_
             * tasks() just re-nominates the same still-stalest
             * district on its very next call and gets evicted again,
             * every tick, for as long as the safe-zone pressure lasts -
             * see ZONE_SWEEP_RECLAIM_COOLDOWN_TICKS. */
            uint8_t district_id = zone_id_for_location(task->loc_x, task->loc_y);
            if(district_id < NUM_SEARCH_ZONES)
                g_search_zones[district_id].reclaimed_at_tick = g_current_tick;

            clear_task_slot(TASK_POOL_SYSTEM, task);
            task->slot_occupied = 1;
            return task;
        }
    }

    return NULL;
}
// removed: select_generated_incident_type

static void populate_generated_incident_model(IncidentState_t *incident)
{
    apply_incident_archetype(incident, incident->incident_type);
    incident->passive_timer = incident->passive_effect == PASSIVE_NONE ? 0 : random_range_stream(STREAM_INCIDENTS, 10, 30);

    switch(incident->incident_type)
    {
        case INCIDENT_STRUCTURAL_COLLAPSE:
            incident->victim_count = random_range_stream(STREAM_INCIDENTS, 1, 8);
            incident->structure_type = random_range_stream(STREAM_INCIDENTS, 2, 3);
            break;

        case INCIDENT_CONFINED_SPACE:
            incident->victim_count = random_range_stream(STREAM_INCIDENTS, 1, 3);
            incident->structure_type = random_range_stream(STREAM_INCIDENTS, 1, 3);
            break;

        case INCIDENT_MASS_CASUALTY:
            incident->victim_count = random_range_stream(STREAM_INCIDENTS, 5, 20);
            incident->structure_type = random_range_stream(STREAM_INCIDENTS, 0, 2);
            break;

        case INCIDENT_CHEMICAL_EXPOSURE:
            incident->victim_count = random_range_stream(STREAM_INCIDENTS, 1, 6);
            incident->hazard_flags |= HAZ_CBRN;
            incident->structure_type = random_range_stream(STREAM_INCIDENTS, 0, 2);
            break;

        case INCIDENT_GAS_LEAK:
            incident->victim_count = random_range_stream(STREAM_INCIDENTS, 0, 4);
            incident->hazard_flags |= HAZ_GAS;
            incident->structure_type = random_range_stream(STREAM_INCIDENTS, 1, 3);
            break;

        case INCIDENT_ACTIVE_FIRE:
            incident->victim_count = random_range_stream(STREAM_INCIDENTS, 1, 10);
            incident->structure_type = random_range_stream(STREAM_INCIDENTS, 1, 3);
            break;

        default:
            incident->victim_count = random_range_stream(STREAM_INCIDENTS, 0, 5);
            incident->structure_type = random_range_stream(STREAM_INCIDENTS, 0, 2);
            break;
    }
}

// removed: pressure_should_generate_incident


static void build_generation_context(GenerationContext_t *ctx)
{
    if(ctx == NULL)
        return;

    *ctx = (GenerationContext_t){0};

    for(uint8_t i = 0; i < NUM_TEAMS; i++)
    {
        TeamState_t *team = &g_teams[i];

        switch(team->status)
        {
            case STATUS_AVAILABLE:
                ctx->available_teams++;
                if(team->specialization < NUM_TEAM_SPECIALIZATIONS)
                    ctx->available_by_spec[team->specialization]++;
                break;

            case STATUS_EN_ROUTE:
                ctx->en_route_teams++;
                if(team->specialization < NUM_TEAM_SPECIALIZATIONS)
                    ctx->active_by_spec[team->specialization]++;
                break;

            case STATUS_ON_SCENE:
                ctx->on_scene_teams++;
                if(team->specialization < NUM_TEAM_SPECIALIZATIONS)
                    ctx->active_by_spec[team->specialization]++;
                break;

            case STATUS_RESTING:
                ctx->resting_teams++;
                break;

            default:
                break;
        }
    }

    for(uint8_t i = 0; i < NUM_INCIDENTS; i++)
    {
        IncidentState_t *incident = &g_incidents[i];

        if(!incident->slot_occupied || incident->status == INC_RESOLVED)
            continue;

        if(incident->status == INC_OPEN)
        {
            ctx->open_incidents++;
            if(incident->req_capability < NUM_TEAMS)
                ctx->waiting_by_spec[incident->req_capability]++;
            if(!incident->discovered)
                ctx->hidden_incidents++;
        }
        else if(incident->status == INC_ASSIGNED)
        {
            ctx->assigned_incidents++;
        }
        else if(incident->status == INC_IN_PROGRESS)
        {
            ctx->in_progress_incidents++;
        }
    }

    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        if(g_system_tasks[i].slot_occupied &&
           g_system_tasks[i].status != INC_RESOLVED)
        {
            ctx->active_system_tasks++;
        }
    }

    for(int x = 0; x < GRID_WIDTH; x++)
    {
        for(int y = 0; y < GRID_HEIGHT; y++)
        {
            if(g_grid[x][y].safety_level < 70)
                ctx->degraded_cells++;
        }
    }
}

static GenerationLoad_t calculate_generation_load(const GenerationContext_t *ctx)
{
    uint32_t unresolved = ctx->open_incidents +
                          ctx->assigned_incidents +
                          ctx->in_progress_incidents;
    uint32_t load_score = 0;

    load_score += unresolved * 6;
    load_score += ctx->open_incidents * 4;
    load_score += ctx->en_route_teams * 4;
    load_score += ctx->on_scene_teams * 7;
    load_score += ctx->resting_teams * 5;
    load_score += ctx->active_system_tasks * 3;

    for(uint8_t spec = 0; spec < NUM_TEAMS; spec++)
    {
        if(ctx->waiting_by_spec[spec] > 0 &&
           ctx->available_by_spec[spec] == 0)
        {
            load_score += ctx->waiting_by_spec[spec] * 5;
        }
    }

    if(load_score <= 20)
        return GEN_LOAD_IDLE;
    if(load_score <= 40)
        return GEN_LOAD_LOW;
    if(load_score <= 60)
        return GEN_LOAD_MODERATE;
    if(load_score <= 80)
        return GEN_LOAD_HIGH;

    return GEN_LOAD_CRITICAL;
}

static uint32_t calculate_generation_probability(const GenerationContext_t *ctx, GenerationLoad_t load)
{
    uint32_t unresolved = ctx->open_incidents +
                          ctx->assigned_incidents +
                          ctx->in_progress_incidents;
    int32_t probability = 2;

    probability += (int32_t)(ctx->degraded_cells / 5);
    probability += (int32_t)(ctx->available_teams * 8);

    if(unresolved < MIN_NUM_INCIDENTS)
        probability += 5;

    switch(load)
    {
        case GEN_LOAD_IDLE:     probability += 15; break;
        case GEN_LOAD_LOW:      probability += 5;  break;
        case GEN_LOAD_MODERATE: probability -= 5;  break;
        case GEN_LOAD_HIGH:     probability -= 20; break;
        case GEN_LOAD_CRITICAL: probability -= 35; break;
        default: break;
    }

    probability -= (int32_t)(ctx->open_incidents * 4);
    probability -= (int32_t)(ctx->active_system_tasks * 2);

    /* Undiscovered-backlog penalty: the more of what's currently open is
     * still hidden (nobody's found it yet), the less sense it makes to
     * keep spawning more on top. A simple subtraction here would get
     * swallowed by the fixed [5,8] clamp below and never show a
     * graduated effect, so this moves the FLOOR itself down instead -
     * from 5 at zero hidden backlog, toward 1 as the open set
     * approaches 100% hidden. The ceiling of 8 is untouched; a fully
     * discovered, well-staffed world can still hit the normal ceiling. */
    int32_t floor = 5;
    if(ctx->open_incidents > 0)
    {
        int32_t hidden_permille = (int32_t)((ctx->hidden_incidents * 1000u) / ctx->open_incidents);
        floor -= (hidden_permille * 4) / 1000;  /* scales 5 -> 1 as hidden_fraction: 0 -> 1.0 */
        if(floor < 1)
            floor = 1;
    }

    if(probability < floor)
        probability = floor;
    if(probability > 8)
        probability = 8;

    return (uint32_t)probability;
}

static IncidentType_t select_weighted_incident_type(const GenerationContext_t *ctx)
{
    uint32_t weights[GENERATION_PROFILE_COUNT];
    uint32_t total_weight = 0;

    for(uint32_t i = 0; i < GENERATION_PROFILE_COUNT; i++)
    {
        int32_t weight = g_generation_profiles[i].base_weight;

        for(uint8_t spec = 0; spec < NUM_TEAMS; spec++)
        {
            uint8_t demand = g_generation_profiles[i].demand[spec];

            if(demand == 0)
                continue;

            if(ctx->waiting_by_spec[spec] > 0 &&
               ctx->available_by_spec[spec] == 0)
            {
                weight -= (int32_t)(demand / 20);
                weight -= (int32_t)(ctx->waiting_by_spec[spec] * 2);
            }
            else if(ctx->available_by_spec[spec] > 0)
            {
                weight += (int32_t)(demand / 25);
            }
        }

        if(weight < 1)
            weight = 1;

        weights[i] = (uint32_t)weight;
        g_last_generation_weights[i] = weights[i];
        total_weight += weights[i];
    }

    g_last_generation_total_weight = total_weight;
    g_last_generation_type_roll =
        random_range_stream(STREAM_INCIDENTS, 1, total_weight);

    uint32_t cumulative = 0;

    for(uint32_t i = 0; i < GENERATION_PROFILE_COUNT; i++)
    {
        cumulative += weights[i];

        if(g_last_generation_type_roll <= cumulative)
            return g_generation_profiles[i].type;
    }

    return g_generation_profiles[GENERATION_PROFILE_COUNT - 1].type;
}

static void schedule_next_generation_evaluation(GenerationLoad_t load)
{
    uint32_t min_interval;
    uint32_t max_interval;

    switch(load)
    {
        case GEN_LOAD_IDLE:
            min_interval = 3;
            max_interval = 5;
            break;

        case GEN_LOAD_LOW:
            min_interval = 4;
            max_interval = 7;
            break;

        case GEN_LOAD_MODERATE:
            min_interval = 6;
            max_interval = 10;
            break;

        case GEN_LOAD_HIGH:
            min_interval = 9;
            max_interval = 15;
            break;

        case GEN_LOAD_CRITICAL:
        default:
            min_interval = 12;
            max_interval = 20;
            break;
    }

    g_next_incident_evaluation_tick =
        g_current_tick + random_range_stream(STREAM_INCIDENT_SCHEDULE,min_interval,max_interval);
}

static int generate_incident_at(IncidentType_t type, int16_t loc_x, int16_t loc_y)
{
    IncidentState_t *incident = find_free_incident_slot();

    if(incident == NULL)
        return 0;

    incident->incident_id = allocate_incident_id();
    incident->incident_type = type;
    incident->status = INC_OPEN;
    incident->assigned_team = NO_ASSIGNMENT;
    incident->support_team = NO_ASSIGNMENT;
    incident->loc_x = loc_x;
    incident->loc_y = loc_y;
    incident->reported_at_tick = g_current_tick;
    incident->spawned_by = NO_ASSIGNMENT;
    incident->work_remaining = 100;
    incident->discovered = 0;  /* fog-of-war: hidden until Search/other teams' radius finds it */
    populate_generated_incident_model(incident);
    apply_incident_grid_degradation(incident);
    extern uint32_t g_diag_spawned_by_type[NUM_INCIDENT_TYPES];
    g_diag_spawned_by_type[type]++;
    return 1;
}

static int generate_incident(IncidentType_t type)
{
    int16_t loc_x;
    int16_t loc_y;

    if(!environment_find_incident_site(type, &loc_x, &loc_y))
        return 0;

    return generate_incident_at(type, loc_x, loc_y);
}

/* Maps district trust directly onto the 4 existing priority levels
 * instead of a tick-modulo decrement clock. Idempotent by design - it
 * recomputes the "correct" priority for the incident's district's
 * current trust every time it's called, rather than stepping a
 * counter, so there's no interval/cooldown concept needed at all.
 *
 * NOT CURRENTLY CALLED from the live tick loop. last_swept_tick only
 * ever gets reset by sweep-cycle completion, which isn't built yet -
 * every district currently starts at last_swept_tick=0 and nothing
 * ever refreshes it, so trust would decay to 0% for every district
 * within ZONE_TRUST_DECAY_CAP_TICKS of simulation start and stay
 * there permanently. Combined with `discovered` currently defaulting
 * true for every incident (its own reveal-writer isn't built yet
 * either), calling this now would push every open incident in the
 * game to permanent CRITICAL priority after ~3 hours simulated time -
 * an active regression, not a harmless no-op. Wire this in only once
 * sweep-cycle completion is writing real last_swept_tick values. */
void escalate_incidents_by_zone_trust(uint32_t current_tick)
{
    for(uint16_t i = 0; i < NUM_INCIDENTS; i++)
    {
        IncidentState_t *incident = &g_incidents[i];

        if(!incident->slot_occupied || incident->status == INC_RESOLVED)
            continue;
        if(!incident->discovered)
            continue;

        uint8_t zone_id = zone_id_for_location(incident->loc_x, incident->loc_y);
        uint8_t trust = search_zone_trust(zone_id, current_tick);

        Priority_t target_priority;
        if(trust > 75)
            target_priority = PRIORITY_LOW;
        else if(trust > 50)
            target_priority = PRIORITY_MEDIUM;
        else if(trust > 25)
            target_priority = PRIORITY_HIGH;
        else
            target_priority = PRIORITY_CRITICAL;

        incident->priority = target_priority;
    }
}

/*
 * Anti-starvation for OPEN (unassigned) incidents. Deliberately
 * separate from priority_score()'s own per-bid scoring (core/
 * scoring.c): that function only ever moves an incident's score
 * within its OWN tier's band, by design (see that function's own
 * comment for why - letting a lower tier's score climb into a higher
 * tier's territory is exactly the bug this whole mechanism exists to
 * fix). A tier-respecting score alone means a badly-matched CRITICAL
 * incident CAN sit open indefinitely if no currently-idle team is a
 * good enough capability/distance fit to ever outbid whatever
 * same-or-higher-tier work that team already has - band separation
 * fixes the "tier order gets inverted" problem but reintroduces a
 * "starvation" problem for whichever tier is currently least
 * competitive for the available teams, so it needs its own, explicit
 * fix rather than trying to make priority_score() serve both jobs.
 *
 * This function is the fix: once an incident has sat OPEN for longer
 * than INCIDENT_STARVATION_ESCALATION_TICKS - deliberately well past
 * priority_score()'s own in-tier saturation window (~2000 ticks, see
 * logarithmic_waiting_time_bonus()'s normalized_age_limit) so this
 * only fires for incidents that band separation alone has genuinely
 * failed to get picked up, not ones just aging normally within their
 * tier - its priority is promoted a full tier, same "decrement the
 * Priority_t enum value" pattern core/fault_injector.c's
 * inject_mission_fault() already uses for a different trigger. Unlike
 * escalate_incidents_by_zone_trust() above, this can only ever
 * escalate (never silently reassign to a LOWER priority based on an
 * unrelated signal), only touches incidents nobody has picked up yet
 * (INC_OPEN - once a team is working it, it's not neglected, whatever
 * tier it's at), and its trigger (this incident's own open-ticks) has
 * no dependency on the currently-broken search-zone-trust decay
 * machinery that keeps the zone-trust version disabled.
 *
 * reported_at_tick is reset on escalation, not left alone: priority_
 * score()'s in-band aging bonus should restart for the NEW tier
 * (a freshly-escalated incident hasn't been sitting at ITS new tier
 * for the same duration it sat at the old one), and resetting it also
 * naturally prevents this same incident from re-triggering escalation
 * again next tick. reported_at_tick's own doc comment (mission_state.h)
 * confirms this field has no other consumer to disturb - laptop-side
 * "age-weighted priority scoring" only, never sent to a board (see
 * core/mission_link.h's own note on why it's excluded from the wire).
 */
#define INCIDENT_STARVATION_ESCALATION_TICKS 3000u

void escalate_starved_incidents(uint32_t current_tick)
{
    for(uint16_t i = 0; i < NUM_INCIDENTS; i++)
    {
        IncidentState_t *incident = &g_incidents[i];

        if(!incident->slot_occupied || incident->status != INC_OPEN)
            continue;

        if(incident->priority <= PRIORITY_CRITICAL)
            continue; /* already at the top - nothing to escalate to */

        uint32_t age = (current_tick >= incident->reported_at_tick)
                        ? (current_tick - incident->reported_at_tick)
                        : 0u;

        if(age >= INCIDENT_STARVATION_ESCALATION_TICKS)
        {
            incident->priority = (Priority_t)(incident->priority - 1);
            incident->reported_at_tick = current_tick;
        }
    }
}

void generate_incidents_if_needed(void)
{
    IncidentType_t hazard_type;
    int16_t hazard_x;
    int16_t hazard_y;

    while(environment_pop_pending_hazard_event(&hazard_type, &hazard_x, &hazard_y))
        generate_incident_at(hazard_type, hazard_x, hazard_y);

    if(g_current_tick < g_next_incident_evaluation_tick)
        return;

    GenerationContext_t ctx;
    build_generation_context(&ctx);

    GenerationLoad_t load = calculate_generation_load(&ctx);
    uint32_t probability = calculate_generation_probability(&ctx, load);
    uint32_t spawn_roll =
        random_range_stream(STREAM_INCIDENT_SCHEDULE, 1, 100);
    uint8_t spawned = 0;

    if(spawn_roll <= probability)
    {
        IncidentType_t type = select_weighted_incident_type(&ctx);
        spawned = generate_incident(type) ? 1 : 0;

        debug_print_generator_selection(g_last_generation_weights,GENERATION_PROFILE_COUNT,g_last_generation_total_weight,g_last_generation_type_roll,(uint8_t)type);
    }

    schedule_next_generation_evaluation(load);

    debug_print_generator_evaluation(g_current_tick,
                                     (uint8_t)load,
                                     probability,
                                     spawn_roll,
spawned,ctx.available_teams,ctx.en_route_teams,ctx.on_scene_teams,ctx.resting_teams,
                                     ctx.open_incidents,ctx.assigned_incidents,ctx.in_progress_incidents,ctx.active_system_tasks,ctx.degraded_cells,g_next_incident_evaluation_tick);
}

static void clear_stale_equipment_cache_requests(void)
{
    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
    {
        IncidentState_t *task = &g_system_tasks[i];

        if(!task->slot_occupied ||
           task->status == INC_RESOLVED ||
           task->incident_type != INCIDENT_EQUIPMENT_CACHE ||
           !is_valid_team_id(task->spawned_by))
        {
            continue;
        }

        TeamState_t *team = &g_teams[task->spawned_by];
        int returning_to_safe_zone = team->heading_to_rest;

        if(team->equipment_level == EQ_MAX_LEVEL ||
           team->status == STATUS_RESTING ||
           returning_to_safe_zone)
        {
            complete_task(TASK_POOL_SYSTEM, task);
        }
    }
}

void reopen_future_queued_incidents(void)
{
    for(uint8_t team_id = 0; team_id < NUM_TEAMS; team_id++)
    {
        TeamState_t *team = &g_teams[team_id];

        if(team->incident_queue_size == 0)
            continue;

        for(uint8_t q = team->incident_queue_index + 1;
            q < team->incident_queue_size;
            q++)
        {
            uint8_t slot = team->incident_queue_slot[q];
            uint32_t incident_id = team->incident_queue_id[q];

            if(slot >= NUM_INCIDENTS)
                continue;

            IncidentState_t *incident = &g_incidents[slot];

            if(!incident->slot_occupied)
                continue;

            if(incident->incident_id != incident_id)
                continue;

            if(incident->status != INC_ASSIGNED)
                continue;

            if(incident->assigned_team != team_id)
                continue;

            incident->status = INC_OPEN;
            incident->assigned_team = NO_ASSIGNMENT;
        }

        uint8_t current_index = team->incident_queue_index;
        uint8_t current_slot = team->incident_queue_slot[current_index];
        uint32_t current_id = team->incident_queue_id[current_index];

        team->incident_queue_size = 0;
        team->incident_queue_index = 0;

        for(uint8_t q = 0; q < CBBA_MAX_BUNDLE_SIZE; q++)
        {
            team->incident_queue_slot[q] = NO_ASSIGNMENT;
            team->incident_queue_id[q] = 0;
        }

        if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
           current_slot < NUM_INCIDENTS)
        {
            IncidentState_t *current = &g_incidents[current_slot];

            if(current->slot_occupied &&
               current->incident_id == current_id &&
               current->assigned_team == team_id &&
               (current->status == INC_ASSIGNED ||
                current->status == INC_IN_PROGRESS))
            {
                team->incident_queue_slot[0] = current_slot;
                team->incident_queue_id[0] = current_id;
                team->incident_queue_size = 1;
            }
        }
    }
}

static void release_queued_incidents(TeamState_t *team)
{
    for(uint8_t q = team->incident_queue_index + 1;
        q < team->incident_queue_size;
        q++)
    {
        uint8_t slot = team->incident_queue_slot[q];
        uint32_t incident_id = team->incident_queue_id[q];

        if(slot >= NUM_INCIDENTS)
            continue;

        IncidentState_t *incident = &g_incidents[slot];

        if(!incident->slot_occupied)
            continue;

        if(incident->incident_id != incident_id)
            continue;

        if(incident->status != INC_ASSIGNED)
            continue;

        if(incident->assigned_team != team->team_id)
            continue;

        incident->status = INC_OPEN;
        incident->assigned_team = NO_ASSIGNMENT;
    }

    team->incident_queue_size = 0;
    team->incident_queue_index = 0;

    for(uint8_t q = 0; q < CBBA_MAX_BUNDLE_SIZE; q++)
    {
        team->incident_queue_slot[q] = NO_ASSIGNMENT;
        team->incident_queue_id[q] = 0;
    }
}

uint32_t g_diag_safezone_fallback_count = 0;

void abandon_task_for_recovery(TeamState_t *team)
{
    if(team == NULL)
        return;

    /* Root cause of the abort/reassign churn originally traced here: a
     * team already heading to rest has nothing to abandon - previously
     * this had to be inferred from a task slot (RETURN_TO_SAFE_ZONE no
     * longer occupies one - see begin_return_to_safe_zone()), now it's
     * a direct flag check. Without this, every call site below would
     * unconditionally re-derive a (likely identical) destination every
     * tick this function runs, and - worse, before this state became
     * team-intrinsic - would destroy the in-flight trip's own bookkeeping
     * before recomputing it, so the team never got a contiguous window
     * to actually travel and rest. The narrower slot-reclaim/allocation
     * race this comment used to document (RETURN_TO_SAFE_ZONE's system-
     * task slot getting raced against find_reclaimable_system_task_slot()
     * and the allocate_all_incidents() assignment loop) no longer applies
     * either, since there is no slot left to race over. */
    if(team->heading_to_rest)
    {
        return;
    }

    IncidentState_t *task = NULL;
    TaskPool_t pool = (TaskPool_t)team->assigned_task_pool;

    if(pool == TASK_POOL_INCIDENT &&
       team->assigned_task_slot < NUM_INCIDENTS)
    {
        task = &g_incidents[team->assigned_task_slot];
    }
    else if(pool == TASK_POOL_SYSTEM &&
            team->assigned_task_slot < NUM_SYSTEM_TASKS)
    {
        task = &g_system_tasks[team->assigned_task_slot];
    }

    if(task == NULL || !task->slot_occupied)
    {
        team->assigned_task_pool = TASK_POOL_NONE;
        team->assigned_task_slot = NO_ASSIGNMENT;
        if(begin_return_to_safe_zone(team) == NO_ASSIGNMENT)
        {
            g_diag_safezone_fallback_count++;
            team_set_available(team);
        }
        return;
    }

    if(task->assigned_team == team->team_id)
    {
        task->assigned_team = NO_ASSIGNMENT;

        if(task->unreachable_streak < INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD)
        {
            task->status = INC_OPEN;
        }
        else
        {
            /* Quarantined: leave status alone - stays out of the INC_OPEN
             * candidate pools (allocator.c / cbba.c) without touching
             * INC_RESOLVED (which scoring/metrics treat as a genuine
             * success). Count it once, on the crossing tick. */
            if(task->unreachable_streak == INCIDENT_UNREACHABLE_GIVEUP_THRESHOLD)
                g_diag_incidents_quarantined++;
        }

        if(task->incident_type == INCIDENT_UNSEARCHED_SECTOR)
        {
            extern uint32_t g_diag_sweep_abandoned_for_recovery;
            g_diag_sweep_abandoned_for_recovery++;
        }
    }
    else if(task->support_team == team->team_id)
    {
        task->support_team = NO_ASSIGNMENT;
    }

    team->assigned_task_pool = TASK_POOL_NONE;
    team->assigned_task_slot = NO_ASSIGNMENT;
    release_queued_incidents(team);
    if(begin_return_to_safe_zone(team) == NO_ASSIGNMENT)
    {
        g_diag_safezone_fallback_count++;
        team_set_available(team);
    }
}

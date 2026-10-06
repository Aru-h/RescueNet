#include "team.h"
#include "mission_state.h"
#include "random.h"
#include "incident.h"
#include "snapshot.h"
#include "mission_state_utils.h"
#include "allocator.h"
#include "recovery_manager.h"
#include "recovery_manager_config.h"
#include "fault_injector.h"
#include <stddef.h>
#include <string.h>

#define EQ_RESERVE_LEVEL 15

#if TEAM_TEST_RANDOM_EMISSION
/* Private stream (xorshift32), never core/random.c, so the variant cannot
 * shift the world. Seeded per run by consensus_init(). */
static uint32_t g_random_emission_state = 2463534242u;

void team_test_seed_random_emission(uint32_t seed)
{
    g_random_emission_state = seed ? seed : 2463534242u;
}

static uint8_t random_emission_draw(uint8_t severity)
{
    uint32_t x = g_random_emission_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_random_emission_state = x;
    return (x % ((uint32_t)severity + 1u)) == 0u;
}
#endif

void generate_teams(uint32_t seed) //team
{
    random_seed_stream(STREAM_TEAMS, derive_seed(seed, STREAM_TEAMS));

    for(int i = 0; i < NUM_TEAMS; i++)
    {
        memset(&g_teams[i], 0, sizeof(g_teams[i]));
        g_teams[i].specialization = i % 5;
        uint8_t zone = random_range_stream(STREAM_TEAMS, 0, g_num_safe_zones - 1);
        g_teams[i].resting_zone_id = zone;
        g_teams[i].loc_x = g_safezones[zone].loc_x;
        g_teams[i].loc_y = g_safezones[zone].loc_y;
        g_teams[i].team_id = i;
        g_teams[i].status = STATUS_AVAILABLE;
        g_teams[i].assigned_task_pool = TASK_POOL_NONE;
        g_teams[i].assigned_task_slot = NO_ASSIGNMENT;
        g_teams[i].incident_queue_size = 0;
        g_teams[i].incident_queue_index = 0;

        for(uint8_t q = 0; q < CBBA_MAX_BUNDLE_SIZE; q++)
        {
            g_teams[i].incident_queue_slot[q] = NO_ASSIGNMENT;
            g_teams[i].incident_queue_id[q] = 0;
        }

        g_teams[i].stamina = 100;
        g_teams[i].hours_worked = 0;
        g_teams[i].ticks_worked_this_period = 0;

        switch(g_teams[i].specialization)
        {
            case SPEC_SEARCH:
                g_teams[i].equipment_flags = EQ_ROPE | EQ_K9;
                break;

            case SPEC_MEDICAL:
                g_teams[i].equipment_flags = EQ_MEDICAL_KIT;
                break;

            case SPEC_EXTRACTION:
                g_teams[i].equipment_flags = EQ_ROPE | EQ_SPREADER | EQ_STRUCTURAL;
                break;

            case SPEC_PERIMETER:
                g_teams[i].equipment_flags = EQ_HAZMAT_SUIT | EQ_STRUCTURAL | EQ_ROPE;
                break;

            case SPEC_SUPPORT:
                /*
                 * EQ_STRUCTURAL added on top of SUPPORT's original
                 * loadout (EQ_GENERATOR | EQ_MEDICAL_KIT | EQ_ROPE) -
                 * a deliberate fix, not the original design.
                 *
                 * Root cause this addresses: STRUCTURAL_COLLAPSE and
                 * ACTIVE_FIRE, tagged SPEC_PERIMETER, both hard-require
                 * EQ_STRUCTURAL as their equipment_requirements()
                 * minimum (core/scoring.c) - team_has_minimum_equipment()
                 * refuses ANY team lacking it outright, before
                 * capability_score() or any other scoring factor is
                 * even consulted. Only EXTRACTION and PERIMETER
                 * carried EQ_STRUCTURAL before this change, so those
                 * two incident types could only ever be serviced by 4
                 * of the 10 teams - and PERIMETER itself is the most
                 * oversubscribed specialization in the roster (5 of 11
                 * incident archetypes require it, vs 1-2 for every
                 * other specialization - see the archetype table's own
                 * required_specialization column), so its own 2 teams
                 * are already the bottleneck, not a source of spare
                 * capacity to lean on further.
                 *
                 * SUPPORT was chosen as the specialization to extend,
                 * not SEARCH or MEDICAL: capability_score() (core/
                 * scoring.c's CAPABILITY_MATRIX) already rates SUPPORT
                 * as the best non-PERIMETER match for PERIMETER work
                 * (0.70) - better than SEARCH's 0.60, and well ahead of
                 * EXTRACTION's own 0.45 or MEDICAL's 0.30 - so it was
                 * already the natural substitute by score; it just
                 * couldn't act on it because of this hard gear gate.
                 * Adding the flag lets that existing capability
                 * ranking actually take effect instead of being
                 * silently vetoed before scoring ever runs.
                 */
                g_teams[i].equipment_flags = EQ_GENERATOR | EQ_MEDICAL_KIT | EQ_ROPE | EQ_STRUCTURAL;
                break;

            default:
                g_teams[i].equipment_flags = 0;
                break;
        }

        g_teams[i].last_heartbeat_tick = 0;
        g_teams[i].equipment_level = EQ_MAX_LEVEL;

        switch(g_teams[i].specialization)
        {
            case SPEC_PERIMETER:
            case SPEC_SEARCH:
                g_teams[i].fitness_tier = MODERATE;
                break;

            case SPEC_SUPPORT:
            case SPEC_MEDICAL:
                g_teams[i].fitness_tier = LIGHT;
                break;

            case SPEC_EXTRACTION:
                g_teams[i].fitness_tier = ARDUOUS;
                break;

            default:
                /* Matches fitness_speed_multiplier()'s (core/mission_
                   state_utils.c) own default-to-MODERATE convention for
                   an unhandled specialization, rather than leaving
                   fitness_tier at whatever memset left it. */
                g_teams[i].fitness_tier = MODERATE;
                break;
        }
    }
}

static void change_stamina(TeamState_t *team, float delta)
{
    float new_stamina = team->stamina + delta;

    if(new_stamina > MAX_STAMINA)
        new_stamina = MAX_STAMINA;

    if(new_stamina < MIN_STAMINA)
        new_stamina = MIN_STAMINA;

    team->stamina = new_stamina;
}

void update_team(TeamState_t *team) //team
{
    uint8_t replenish_rate;
    switch (team->status){
        case(STATUS_AVAILABLE):
        {
            /* An AVAILABLE team must be immediately eligible for new
             * work (team_can_accept_incident()'s AVAILABLE branch,
             * core/allocator.c: stamina >= get_soft_stamina_floor(),
             * equipment_level > RECOVERY_MANAGER_EQUIPMENT_CRITICAL_
             * THRESHOLD) or it is permanently stuck - this case is a
             * no-op, so only STATUS_RESTING actually recovers stamina/
             * equipment. An ineligible-but-AVAILABLE team can never be
             * picked up again.
             *
             * Root cause (confirmed empirically this session, traced
             * via a status-transition log on a stalled SEARCH team):
             * multiple call sites across the codebase set a team
             * directly to STATUS_AVAILABLE with no stamina/equipment
             * check whenever its current task disappears out from
             * under it. team_set_available() itself (core/
             * mission_state_utils.c) is unconditional by design, and
             * at least two OTHER sites bypass even that and assign
             * team->status = STATUS_AVAILABLE directly: core/
             * incident.c's search_sweep_step() (a SEARCH team's sweep
             * cycle completing) and core/simulation.c's move_team()
             * (task vanished mid-travel). Any one of them can land a
             * team on AVAILABLE below the eligibility floor - traced
             * directly: a SEARCH team hit this via search_sweep_step(),
             * stamina 24.50 against a 26.0 floor, and simply never
             * changed status again for the rest of a 2000-tick run.
             *
             * Rather than chase down and fix every current and future
             * call site individually, close the gap at the one place
             * that can catch all of them: if an AVAILABLE team doesn't
             * actually meet the eligibility bar, route it to recovery
             * here instead of leaving it idle. */
            if(team->stamina >= get_soft_stamina_floor(team) &&
               team->equipment_level > RECOVERY_MANAGER_EQUIPMENT_CRITICAL_THRESHOLD)
            {
                break;
            }

            if(begin_return_to_safe_zone(team) == NO_ASSIGNMENT)
            {
                /* True last resort: couldn't route this team anywhere
                 * (no reachable safe zone). STATUS_RESTING
                 * recovers stamina and equipment unconditionally
                 * regardless of location in this simulator's model (see
                 * the STATUS_RESTING case below - no position check),
                 * so forcing it directly here guarantees the team
                 * eventually recovers instead of freezing forever. */
                extern uint32_t g_diag_safezone_fallback_count;
                g_diag_safezone_fallback_count++;
                team->status = STATUS_RESTING;
            }
            break;
        }
        case(STATUS_EN_ROUTE):
            if(team->stamina <= MIN_STAMINA)
            {
                /* Same failure mode as the ON_SCENE equipment-exhaustion
                 * check below: a team that bottoms out mid-travel has to
                 * be pulled off and routed to actually recover, not left
                 * to keep traveling indefinitely at the stamina floor
                 * with no path back to RESTING. abandon_task_for_recovery
                 * sends it toward a real safe zone (see incident.c),
                 * consistent with every other abandonment path. */
                snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                abandon_task_for_recovery(team);
                break;
            }
            change_stamina(team,-team_travel_stamina_cost(team));
            break;
        case(STATUS_ON_SCENE):
        {
            IncidentState_t *task = NULL ;

            if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
               team->assigned_task_slot < NUM_INCIDENTS)
            {
                task = &g_incidents[team->assigned_task_slot];
            }
            else if(team->assigned_task_pool == TASK_POOL_SYSTEM &&
                    team->assigned_task_slot < NUM_SYSTEM_TASKS)
            {
                task = &g_system_tasks[team->assigned_task_slot];
            }

            if(task == NULL || !task->slot_occupied)
            {
                /* The task this team was working disappeared out from
                 * under it (slot freed/reclaimed elsewhere while still
                 * ON_SCENE) - route it through abandon_task_for_recovery()
                 * exactly like every other "lost the task mid-work" case
                 * in this function (equipment exhaustion, SUPPORT low-
                 * equipment abort) rather than jumping straight to
                 * team_set_available().
                 *
                 * Root cause this fixes (confirmed empirically - see
                 * core/allocator.c's team_can_accept_incident(), AVAILABLE
                 * branch): team_set_available() sets STATUS_AVAILABLE
                 * unconditionally, with no stamina/equipment check. If
                 * this team's stamina had already drained below its
                 * fitness tier's soft floor (get_soft_stamina_floor())
                 * by the time its task vanished, it landed in AVAILABLE
                 * simultaneously ineligible for any new work (the
                 * AVAILABLE branch of team_can_accept_incident() requires
                 * stamina >= that same floor) - and STATUS_AVAILABLE's
                 * own case above does nothing, only STATUS_RESTING
                 * recovers stamina. That's a permanent deadlock: too
                 * weak to be assigned, never resting to recover. Observed
                 * directly via traced status transitions - a team would
                 * go AVAILABLE once at low stamina and simply never
                 * change status again for the rest of the run.
                 *
                 * abandon_task_for_recovery() instead tries to route the
                 * team to a real safe zone (RESTING, which DOES recover
                 * stamina) via begin_return_to_safe_zone(), and
                 * only falls back to team_set_available() if that
                 * genuinely can't be created (tracked separately via
                 * g_diag_safezone_fallback_count) - matching how every
                 * other task-loss path in this same function already
                 * behaves. */
                team->assigned_task_pool = TASK_POOL_NONE;
                team->assigned_task_slot = NO_ASSIGNMENT;
                abandon_task_for_recovery(team);
                break;
            }

            if(task->status == INC_IN_PROGRESS)
            {
                if(team->equipment_level == 0)
                {
                    snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                    abandon_task_for_recovery(team);
                    break;
                }

                if(team->specialization == SPEC_SUPPORT &&
                   task->incident_type != INCIDENT_EQUIPMENT_CACHE &&
                   team->equipment_level <= EQ_RESERVE_LEVEL)
                {
                    snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                    abandon_task_for_recovery(team);
                    break;
                }

                change_stamina(team,-STAMINA_OPERATION_COST);

                if(!(team->specialization == SPEC_SUPPORT &&
                     task->incident_type == INCIDENT_EQUIPMENT_CACHE))
                {
                    uint8_t equipment_cost = random_range_stream(STREAM_EQUIPMENT,1,5);

                    if(team->equipment_level <= equipment_cost)
                    {
                        team->equipment_level = 0;
                    }
                    else
                    {
                        team->equipment_level -= equipment_cost;
                    }
                }

                if(team->equipment_level == 0)
                {
                    snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                    abandon_task_for_recovery(team);
                    break;
                }

                if(team->specialization == SPEC_SUPPORT &&
                   task->incident_type != INCIDENT_EQUIPMENT_CACHE &&
                   team->equipment_level <= EQ_RESERVE_LEVEL)
                {
                    snapshot_record(g_current_tick, EVENT_TEAM_REST, team->team_id);
                    abandon_task_for_recovery(team);
                    break;
                }

                if(team->equipment_level <= 20 &&
                   task->incident_type != INCIDENT_EQUIPMENT_CACHE)
                {
                    int cache_already_requested = 0;

                    for(uint8_t i = 0; i < NUM_SYSTEM_TASKS; i++)
                    {
                        if(g_system_tasks[i].slot_occupied &&
                           g_system_tasks[i].status != INC_RESOLVED &&
                           g_system_tasks[i].incident_type == INCIDENT_EQUIPMENT_CACHE &&
                           g_system_tasks[i].spawned_by == team->team_id)
                        {
                            cache_already_requested = 1;
                            break;
                        }
                    }

                    if(!cache_already_requested)
                    {
                        create_equipment_cache_incident(team);
                    }
                }
            }
            break;
        }
        case(STATUS_RESTING):
            change_stamina(team,team_recovery_rate(team));
            replenish_rate = random_range_stream(STREAM_EQUIPMENT,5,10);
            if(team->equipment_level+replenish_rate<=EQ_MAX_LEVEL)
            team->equipment_level+=replenish_rate;
            else team->equipment_level=EQ_MAX_LEVEL;
            {
                /* A team that has used up its budget of consecutive
                 * minimal-rest deployments (NFPA 1584: full, formal rehab
                 * mandatory once a second consecutive work cycle would
                 * otherwise occur without one) must be taken all the way to
                 * full stamina before it's eligible again, not just its
                 * normal READY_STAMINA threshold. */
                float required_stamina = get_ready_stamina(team);
                if(team->partial_rest_cycles >= MAX_CONSECUTIVE_PARTIAL_REST_CYCLES)
                    required_stamina = MAX_STAMINA;

                if(team->stamina >= required_stamina && team->equipment_level == EQ_MAX_LEVEL)
                {
                    /* Bug fix: stamina/equipment are the only two things
                     * this promotion has ever checked, but they're not
                     * the only reasons a team could need to stay put -
                     * an active FAULT_CLASS_MOBILITY, communication, or
                     * resource fault is independent of both and doesn't
                     * clear on its own schedule. Without this check,
                     * promoting straight to AVAILABLE the instant stamina/
                     * equipment allow it, regardless of an unresolved
                     * fault, hands the team straight back to
                     * recovery_manager_step() (core/recovery_manager.c),
                     * which - correctly, given the fault is still active -
                     * recommends RETURN_TO_SAFE_ZONE again. recovery_
                     * dispatch()'s guard against acting on a RESTING team
                     * doesn't apply once status is AVAILABLE, so
                     * abandon_task_for_recovery() sends the team right
                     * back out, it arrives almost immediately, re-enters
                     * RESTING, and this same promotion fires again next
                     * tick the thresholds are met - a RESTING<->AVAILABLE
                     * <->EN_ROUTE bounce for as long as the fault lasts.
                     * Confirmed empirically under `--fault-profile
                     * combined`: a team sitting at ~100% stamina/equipment
                     * cycled every 2 ticks, never actually working,
                     * because its mobility fault kept it CONTINUE_
                     * MONITORING/RETURN_TO_SAFE_ZONE while team.c kept
                     * re-promoting it regardless.
                     *
                     * recovery_manager_get_recommendation() reflects LAST
                     * tick's evaluation (recovery_manager_step() runs
                     * later in this same tick, after update_all_teams())
                     * - a one-tick-stale read, not this tick's fresh one.
                     * That's an acceptable approximation here: fault
                     * durations run many ticks (core/fault_injector.c),
                     * so "was recommending RETURN_TO_SAFE_ZONE as of last
                     * tick" is a reliable proxy for "still is," and the
                     * cost of being wrong is only a one-tick delay in an
                     * otherwise-legitimate promotion, not a correctness
                     * issue.
                     *
                     * Also checks LOCAL_RECOVERY, not just RETURN_TO_
                     * SAFE_ZONE: confirmed empirically that the plain
                     * RETURN_TO_SAFE_ZONE check alone had zero effect on
                     * the target bounce. The reason is recovery_manager's
                     * own downgrade logic - while a team is still RESTING
                     * (MISSION_STAGE_RECOVERY), an evaluator that would
                     * otherwise recommend RETURN_TO_SAFE_ZONE gets
                     * downgraded to the milder LOCAL_RECOVERY instead,
                     * since "return to the safe zone" is meaningless for
                     * a team already there. So the recommendation this
                     * check reads back for a RESTING team is essentially
                     * never literally RETURN_TO_SAFE_ZONE - checking only
                     * for it structurally could never catch the case it
                     * was meant to catch. recovery_dispatch()'s own
                     * RECOVERY_ACTION_LOCAL_RECOVERY case (core/
                     * recovery_manager.c) confirms LOCAL_RECOVERY has the
                     * identical effect on a STATUS_AVAILABLE team as
                     * RETURN_TO_SAFE_ZONE does - both call
                     * abandon_task_for_recovery() - so both need to be
                     * treated the same way here. */
                    if(recovery_manager_get_recommendation(team->team_id) == RECOVERY_ACTION_RETURN_TO_SAFE_ZONE ||
                       recovery_manager_get_recommendation(team->team_id) == RECOVERY_ACTION_LOCAL_RECOVERY)
                        break;

                    /* Causal PREVENT (Step 3.6): a team sent home to rest before a fault stays until the
                     * routine-maintenance dwell has run (0 for every other profile and every other team). */
                    if(fault_injector_causal_rest_hold(team->team_id))
                        break;

                    if(team->stamina >= MAX_STAMINA)
                        team->partial_rest_cycles = 0;

                    team_set_available(team);
                }
            }
            break;
    }
    if(team->status != STATUS_RESTING)
    {
        team->ticks_worked_this_period++;
        if(team->ticks_worked_this_period >= TICKS_PER_HOUR)
        {
            team->ticks_worked_this_period -= TICKS_PER_HOUR;
            if(team->hours_worked < 255)
                team->hours_worked++;
        }
    }

    if(team->fault_state.active[FAULT_CLASS_RESOURCE])
    {
        /* A resting team isn't expending physical effort, so a resource fault
         * shouldn't keep draining stamina while it's trying to recover - that
         * drain (up to 0.36/tick at severity 3) can exceed the recovery rate
         * itself (0.108-0.168/tick), permanently trapping a team near 0
         * stamina. Equipment can still degrade from the fault either way. */
        if(team->status != STATUS_RESTING &&
           (!fault_profile_is_causal() || team->stamina > g_causal_params.res_drain_stamina_floor))
        {
            float extra_stamina_drain = (float)team->fault_state.severity[FAULT_CLASS_RESOURCE] * 0.12f;
            change_stamina(team, -extra_stamina_drain);
        }

        /* Causal profile: the fault's own drain stops at a floor, so a
         * resource fault cannot push a team further into the depletion
         * that raises the next resource hazard (fault -> drain -> fault). */
        if(fault_profile_is_causal() &&
           team->equipment_level <= g_causal_params.res_drain_equipment_floor)
        {
            /* floor reached: no further equipment drain */
        }
        else if(team->equipment_level > team->fault_state.severity[FAULT_CLASS_RESOURCE])
            team->equipment_level -= team->fault_state.severity[FAULT_CLASS_RESOURCE];
        else
            team->equipment_level = 0;
    }

#if TEAM_TEST_RANDOM_EMISSION
    if(!team->fault_state.active[FAULT_CLASS_COMMUNICATION] ||
       random_emission_draw(team->fault_state.severity[FAULT_CLASS_COMMUNICATION]))
#else
    if(!team->fault_state.active[FAULT_CLASS_COMMUNICATION] ||
       ((g_current_tick - team->fault_state.start_tick[FAULT_CLASS_COMMUNICATION]) %
        (uint32_t)(team->fault_state.severity[FAULT_CLASS_COMMUNICATION] + 1u) == 0))
#endif
    {
        team->last_heartbeat_tick = g_current_tick;
    }
}

uint8_t team_radio_emitted(const TeamState_t *team) //team
{
    return team->last_heartbeat_tick == g_current_tick;
}

void update_all_teams(void) //team
{
    extern uint32_t g_diag_ticks_resting[NUM_TEAMS];
    extern uint32_t g_diag_ticks_working[NUM_TEAMS];
    extern uint32_t g_diag_ticks_available[NUM_TEAMS];
    for(uint8_t i=0;i<NUM_TEAMS;i++)
    {
        update_team(&g_teams[i]);
        if(g_teams[i].status == STATUS_RESTING)
            g_diag_ticks_resting[i]++;
        else if(g_teams[i].status == STATUS_EN_ROUTE || g_teams[i].status == STATUS_ON_SCENE)
            g_diag_ticks_working[i]++;
        else if(g_teams[i].status == STATUS_AVAILABLE)
            g_diag_ticks_available[i]++;
    }
}

/* Declared non-static in team.h - see that header's own comment for
 * why: core/allocator.c shares this exact threshold now, rather than
 * keeping its own, misaligned copy. */
float get_ready_stamina(const TeamState_t *team) //team
{
    switch(team->fitness_tier)
    {
        case(ARDUOUS): return ARDUOUS_READY_STAMINA;
        case(MODERATE):return MODERATE_READY_STAMINA;
        case(LIGHT):return LIGHT_READY_STAMINA ;
        default: return LIGHT_READY_STAMINA ;
    }
}
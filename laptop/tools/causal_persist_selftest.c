/* Step 3.6 selftest: causal action semantics (ABORT_MISSION and REQUEST_TASK_REALLOCATION), the field-repair
 * hold, and the hazard-oracle forecast. Whole-simulation, causal profile. Build: make check-causal-persistence.
 *
 * Scenario: run a causal world until a team is working an incident-pool task, then drive recovery_dispatch()
 * directly with each action on a different working team and check the documented effect. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "incident.h"
#include "mission_state.h"
#include "recovery_manager.h"
#include "sim_runner.h"
#include "simulation.h"
#include "team.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int working_team(uint16_t skip_mask)
{
    for(uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        const TeamState_t *tm = &g_teams[t];

        if((skip_mask & (1u << t)) != 0u)
            continue;
        if((tm->status == STATUS_ON_SCENE || tm->status == STATUS_EN_ROUTE) && !tm->heading_to_rest &&
           tm->assigned_task_pool == TASK_POOL_INCIDENT && tm->assigned_task_slot < NUM_INCIDENTS &&
           g_incidents[tm->assigned_task_slot].assigned_team == t)
            return (int)t;
    }
    return -1;
}

static void run_until_working(int n_needed, int *teams)
{
    uint16_t mask = 0;   /* 16 bits: NUM_TEAMS is 10, an 8-bit mask would drop teams 8 and 9 */
    int found = 0;

    for(int guard = 0; guard < 3000 && found < n_needed; guard++)
    {
        sim_tick();
        found = 0;
        mask = 0;
        for(int k = 0; k < n_needed; k++)
        {
            teams[k] = working_team(mask);
            if(teams[k] < 0)
                break;
            mask |= (uint16_t)(1u << teams[k]);
            found++;
        }
    }
    CHECK(found == n_needed);
}

static void scenario(uint8_t fixes, int expect_distinct)
{
    int teams[3] = {-1, -1, -1};
    uint32_t id_abort, id_realloc;
    uint8_t slot_abort, slot_realloc;
    TeamState_t *ta, *tr, *tt;

    fault_config_set_stress(2.0f);
    fault_config_set_causal_fixes(fixes);
    sim_start_seed(1003, FAULT_PROFILE_CAUSAL);
    run_until_working(3, teams);
    if(teams[0] < 0 || teams[1] < 0 || teams[2] < 0)
        return;

    ta = &g_teams[teams[0]];
    tr = &g_teams[teams[1]];
    tt = &g_teams[teams[2]];
    slot_abort = ta->assigned_task_slot;
    slot_realloc = tr->assigned_task_slot;
    id_abort = g_incidents[slot_abort].incident_id;
    id_realloc = g_incidents[slot_realloc].incident_id;

    recovery_dispatch((uint8_t)teams[0], RECOVERY_ACTION_ABORT_MISSION);
    recovery_dispatch((uint8_t)teams[1], RECOVERY_ACTION_REQUEST_TASK_REALLOCATION);
    recovery_dispatch((uint8_t)teams[2], RECOVERY_ACTION_RETURN_TO_SAFE_ZONE);

    /* RETURN: always releases the task and heads home. */
    CHECK(tt->heading_to_rest == 1);
    CHECK(tt->assigned_task_pool == TASK_POOL_NONE);

    if(expect_distinct)
    {
        /* ABORT: released, stays put, AVAILABLE, excluded until the stage completes. */
        /* (the allocation pass a later REALLOC runs may already have given the team other work: that is fine,
         * what matters is that it is not on the incident it gave up and was not sent home) */
        CHECK(ta->heading_to_rest == 0);
        CHECK(ta->status != STATUS_RESTING);
        CHECK(!(ta->assigned_task_pool == TASK_POOL_INCIDENT && ta->assigned_task_slot < NUM_INCIDENTS &&
                g_incidents[ta->assigned_task_slot].incident_id == id_abort));
        CHECK(g_incidents[slot_abort].assigned_team != (uint8_t)teams[0]);
        CHECK(g_incidents[slot_abort].incident_id == id_abort);
        CHECK(!team_can_accept_incident(ta, &g_incidents[slot_abort]));
        /* REALLOC: released with the 30-tick exclusion; the team stays where it is. */
        CHECK(tr->heading_to_rest == 0);
        CHECK(tr->status != STATUS_RESTING);
        CHECK(!(tr->assigned_task_pool == TASK_POOL_INCIDENT && tr->assigned_task_slot < NUM_INCIDENTS &&
                g_incidents[tr->assigned_task_slot].incident_id == id_realloc));
        CHECK(g_incidents[slot_realloc].incident_id == id_realloc);
        CHECK(!team_can_accept_incident(tr, &g_incidents[slot_realloc]));
        CHECK(g_diag_exclusions[0] >= 1u && g_diag_exclusions[1] >= 1u);
        for(int i = 0; i < 10; i++)
            sim_tick();
        /* Ten ticks later the ABORT exclusion still holds unless the stage completed (id changes or status moves on). */
        if(g_incidents[slot_abort].slot_occupied && g_incidents[slot_abort].incident_id == id_abort &&
           g_incidents[slot_abort].status != INC_RESOLVED)
            CHECK(g_incidents[slot_abort].assigned_team != (uint8_t)teams[0]);
        if(g_incidents[slot_realloc].slot_occupied && g_incidents[slot_realloc].incident_id == id_realloc)
            CHECK(g_incidents[slot_realloc].assigned_team != (uint8_t)teams[1]);
    }
    else
    {
        /* Semantics off: ABORT is RETURN, REALLOC keeps the task (Step 3.5 behaviour). */
        CHECK(ta->heading_to_rest == 1);
        CHECK(tr->assigned_task_pool == TASK_POOL_INCIDENT);
        CHECK(g_diag_exclusions[0] == 0u && g_diag_exclusions[1] == 0u);
    }
    sim_end_seed();
}

static void forecast_and_hold(void)
{
    int found = 0;

    fault_config_set_stress(7.0f);
    fault_config_set_causal_fixes(CAUSAL_FIXES_ALL);
    sim_start_seed(1004, FAULT_PROFILE_CAUSAL);
    for(int tick = 0; tick < 2500 && !found; tick++)
    {
        sim_tick();
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            float now[2], rest[2], fld[2];

            fault_injector_causal_forecast(t, 0u, now);
            fault_injector_causal_forecast(t, 1u, rest);
            fault_injector_causal_forecast(t, 2u, fld);
            CHECK(now[0] >= 0.0f && now[0] <= 1.0f && now[1] >= 0.0f && now[1] <= 1.0f);
            CHECK(rest[0] <= now[0] + 1e-6f);      /* a rest never raises the risk */
            CHECK(fld[0] <= now[0] + 1e-6f);
            CHECK(fld[1] == now[1]);               /* in-field maintenance acts on wear only */
        }
        for(uint8_t t = 0; t < NUM_TEAMS && !found; t++)
        {
            TeamState_t *tm = &g_teams[t];

            if((tm->status == STATUS_ON_SCENE || tm->status == STATUS_EN_ROUTE) && !tm->heading_to_rest &&
               tm->fault_state.active[FAULT_CLASS_MOBILITY] && fault_injector_causal_field_start(t, 1u))
            {
                int16_t x = tm->loc_x, y = tm->loc_y;
                uint16_t work = 0;

                if(tm->assigned_task_pool == TASK_POOL_INCIDENT && tm->assigned_task_slot < NUM_INCIDENTS)
                    work = g_incidents[tm->assigned_task_slot].work_remaining;
                CHECK(fault_injector_causal_team_held(t));
                sim_tick();
                /* While the repair holds the team it neither moves nor works (unless something sent it home). */
                if(fault_injector_causal_team_held(t))
                {
                    CHECK(tm->loc_x == x && tm->loc_y == y);
                    if(tm->assigned_task_pool == TASK_POOL_INCIDENT && tm->assigned_task_slot < NUM_INCIDENTS)
                        CHECK(g_incidents[tm->assigned_task_slot].work_remaining >= work);
                }
                found = 1;
            }
        }
    }
    CHECK(found);
    sim_end_seed();
}

int main(void)
{
    if(sim_runner_init() != 0)
    {
        printf("sim_runner_init failed\n");
        return 1;
    }
    scenario(CAUSAL_FIXES_ALL, 1);
    scenario((uint8_t)(CAUSAL_FIXES_ALL & ~CAUSAL_FIX_SEMANTICS), 0);
    forecast_and_hold();
    if(g_fail)
    {
        printf("causal_persist_selftest: %d check(s) failed\n", g_fail);
        return 1;
    }
    printf("causal_persist_selftest: ABORT distinct from RETURN, REALLOC releases the task, exclusions hold, field repair holds the team, forecast in range: all checks passed\n");
    return 0;
}

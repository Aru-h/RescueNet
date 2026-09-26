/*
 * laptop/mission_mirror.c - the laptop's picture of a board mission built
 * from board status reports (no hardware, no subprocess).
 *
 * Covers:
 *   1. A report copies status, position, stamina and equipment into the
 *      team, and puts the team on the named incident (ASSIGNED while en
 *      route, IN_PROGRESS on scene, with the board's work figure).
 *   2. An off-grid position is ignored rather than written into g_teams.
 *   3. A team moving off an incident whose extrapolated work reached zero
 *      completes the stage here (complete_incident_stage()): a multi-stage
 *      incident re-opens for its next specialization, a final stage frees
 *      the slot.
 *   4. A team moving off an incident with work left re-opens it.
 *   5. A support team's rate counts toward finishing, and a support team
 *      leaving only clears support_team.
 *   6. A sweep (system task) is never "finished" by work - leaving it
 *      re-opens it - and a report naming it assigns the team.
 *   7. MLSTATUS line parsing: valid lines apply, anything else is
 *      rejected without touching state.
 *   8. An evacuee run (system task): finishing the safe-zone leg loads
 *      evacuees here and re-opens the run toward a gateway for any
 *      extraction team; finishing the gateway leg resolves it.
 *
 * Build/run: make check-mission-mirror-selftest
 */

#include <stdio.h>
#include <string.h>

#include "mission_state.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "simulation.h"
#include "mission_mirror.h"

static int g_failures;

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++; \
        } \
    } while (0)

static MissionLinkStatus_t report(uint8_t team, uint8_t status, uint32_t incident_id,
                                  uint16_t work, uint8_t rate, uint32_t tick,
                                  int16_t x, int16_t y)
{
    MissionLinkStatus_t s;

    memset(&s, 0, sizeof(s));
    s.team_id = team;
    s.status = status;
    s.equipment_level = 90u;
    s.assigned_incident_id = incident_id;
    s.work_remaining_at_report = work;
    s.work_rate = rate;
    s.stamina_at_report = 64u;
    s.report_tick = tick;
    s.loc_x = x;
    s.loc_y = y;
    return s;
}

static void clear_tasks(void)
{
    uint32_t i;

    for (i = 0u; i < NUM_INCIDENTS; i++)
    {
        g_incidents[i].slot_occupied = 0u;
    }
    for (i = 0u; i < NUM_SYSTEM_TASKS; i++)
    {
        g_system_tasks[i].slot_occupied = 0u;
    }
}

static IncidentState_t *make_incident(uint8_t slot, uint32_t id, IncidentType_t type,
                                      TeamSpecialization_t req, uint16_t work)
{
    IncidentState_t *inc = &g_incidents[slot];

    memset(inc, 0, sizeof(*inc));
    inc->slot_occupied = 1u;
    inc->incident_id = id;
    inc->incident_type = type;
    inc->req_capability = req;
    inc->status = INC_OPEN;
    inc->work_remaining = work;
    inc->assigned_team = NO_ASSIGNMENT;
    inc->support_team = NO_ASSIGNMENT;
    inc->discovered = 1u;
    inc->loc_x = 10;
    inc->loc_y = 10;
    return inc;
}

static void fresh_mission(void)
{
    generate_world(7u);
    g_sim_role = SIM_ROLE_WORLD_MIRROR;
    clear_tasks();
    mission_mirror_begin();
}

static void test_report_copies_team_and_assignment(void)
{
    MissionLinkStatus_t s;
    IncidentState_t *inc;

    fresh_mission();
    inc = make_incident(3u, 500u, INCIDENT_MASS_CASUALTY, SPEC_MEDICAL, 100u);

    s = report(2u, STATUS_EN_ROUTE, 500u, 100u, 5u, 10u, 12, 13);
    mission_mirror_apply_status(&s);
    CHECK(g_teams[2].status == STATUS_EN_ROUTE);
    CHECK(g_teams[2].loc_x == 12 && g_teams[2].loc_y == 13);
    CHECK(g_teams[2].stamina == 64.0f);
    CHECK(g_teams[2].equipment_level == 90u);
    CHECK(g_teams[2].assigned_task_pool == TASK_POOL_INCIDENT && g_teams[2].assigned_task_slot == 3u);
    CHECK(inc->assigned_team == 2u && inc->status == INC_ASSIGNED);

    s = report(2u, STATUS_ON_SCENE, 500u, 80u, 5u, 14u, 10, 10);
    mission_mirror_apply_status(&s);
    CHECK(inc->status == INC_IN_PROGRESS);
    CHECK(inc->work_remaining == 80u);

    /* 2. off-grid position ignored, everything else still applied */
    s = report(2u, STATUS_ON_SCENE, 500u, 75u, 5u, 15u, (int16_t)(GRID_WIDTH + 3), 4);
    mission_mirror_apply_status(&s);
    CHECK(g_teams[2].loc_x == 10 && g_teams[2].loc_y == 10);
    CHECK(inc->work_remaining == 75u);
    CHECK(mission_mirror_reports_applied() == 3u);
}

static void test_finished_stage_completes_here(void)
{
    MissionLinkStatus_t s;
    IncidentState_t *inc;

    /* Mass casualty on its medical stage is a final stage: it resolves. */
    fresh_mission();
    inc = make_incident(1u, 600u, INCIDENT_MASS_CASUALTY, SPEC_MEDICAL, 100u);
    s = report(4u, STATUS_ON_SCENE, 600u, 8u, 5u, 20u, 10, 10);
    mission_mirror_apply_status(&s);
    /* Two ticks later the team is free: 8 - 5*2 <= 0, the work was done. */
    s = report(4u, STATUS_AVAILABLE, MISSION_LINK_NO_INCIDENT, 0u, 5u, 22u, 10, 10);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_stages_completed() == 1u);
    CHECK(!inc->slot_occupied);
    CHECK(g_teams[4].status == STATUS_AVAILABLE);
    CHECK(g_teams[4].assigned_task_pool == TASK_POOL_NONE);

    /* Active fire on its perimeter stage branches to extraction: same slot,
       new stage, open for bidding with fresh work. */
    fresh_mission();
    inc = make_incident(2u, 601u, INCIDENT_ACTIVE_FIRE, SPEC_PERIMETER, 100u);
    s = report(1u, STATUS_ON_SCENE, 601u, 3u, 4u, 30u, 10, 10);
    mission_mirror_apply_status(&s);
    s = report(1u, STATUS_AVAILABLE, MISSION_LINK_NO_INCIDENT, 0u, 4u, 31u, 10, 10);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_stages_completed() == 1u);
    CHECK(inc->slot_occupied && inc->incident_id == 601u);
    CHECK(inc->req_capability == SPEC_EXTRACTION);
    CHECK(inc->status == INC_OPEN && inc->assigned_team == NO_ASSIGNMENT);
    CHECK(inc->work_remaining == 100u);
}

static void test_left_early_reopens(void)
{
    MissionLinkStatus_t s;
    IncidentState_t *inc;

    fresh_mission();
    inc = make_incident(0u, 700u, INCIDENT_MASS_CASUALTY, SPEC_MEDICAL, 100u);
    s = report(3u, STATUS_ON_SCENE, 700u, 60u, 5u, 40u, 10, 10);
    mission_mirror_apply_status(&s);
    /* Pulled away one tick later with 55 work left (recovery). */
    s = report(3u, STATUS_EN_ROUTE, MISSION_LINK_NO_INCIDENT, 0u, 5u, 41u, 11, 10);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_stages_completed() == 0u);
    CHECK(mission_mirror_incidents_reopened() == 1u);
    CHECK(inc->slot_occupied && inc->status == INC_OPEN);
    CHECK(inc->assigned_team == NO_ASSIGNMENT);
    CHECK(inc->work_remaining == 60u); /* the board's last figure stands */
}

static void test_support_team(void)
{
    MissionLinkStatus_t s;
    IncidentState_t *inc;

    fresh_mission();
    inc = make_incident(5u, 800u, INCIDENT_MASS_CASUALTY, SPEC_MEDICAL, 100u);
    s = report(1u, STATUS_ON_SCENE, 800u, 12u, 4u, 50u, 10, 10);
    mission_mirror_apply_status(&s);
    s = report(6u, STATUS_ON_SCENE, 800u, 12u, 4u, 50u, 10, 10);
    mission_mirror_apply_status(&s);
    CHECK(inc->assigned_team == 1u && inc->support_team == 6u);

    /* Support leaves: only support_team clears. */
    s = report(6u, STATUS_AVAILABLE, MISSION_LINK_NO_INCIDENT, 0u, 4u, 50u, 10, 10);
    mission_mirror_apply_status(&s);
    CHECK(inc->support_team == NO_ASSIGNMENT && inc->assigned_team == 1u);
    CHECK(inc->status == INC_IN_PROGRESS);

    /* With support back on, 12 work at 4+4 per tick is gone in 2 ticks -
       the primary alone (4/tick) would not have finished it. */
    s = report(6u, STATUS_ON_SCENE, 800u, 12u, 4u, 50u, 10, 10);
    mission_mirror_apply_status(&s);
    s = report(1u, STATUS_AVAILABLE, MISSION_LINK_NO_INCIDENT, 0u, 4u, 52u, 10, 10);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_stages_completed() == 1u);
    CHECK(!inc->slot_occupied);
}

static void test_sweep_system_task(void)
{
    MissionLinkStatus_t s;
    IncidentState_t *sweep = &g_system_tasks[4];

    fresh_mission();
    memset(sweep, 0, sizeof(*sweep));
    sweep->slot_occupied = 1u;
    sweep->incident_id = 900u;
    sweep->incident_type = INCIDENT_UNSEARCHED_SECTOR;
    sweep->req_capability = SPEC_SEARCH;
    sweep->status = INC_OPEN;
    sweep->assigned_team = NO_ASSIGNMENT;
    sweep->support_team = NO_ASSIGNMENT;
    sweep->loc_x = 20;
    sweep->loc_y = 21;

    s = report(0u, STATUS_ON_SCENE, 900u, 0u, 5u, 60u, 20, 21);
    mission_mirror_apply_status(&s);
    CHECK(g_teams[0].assigned_task_pool == TASK_POOL_SYSTEM && g_teams[0].assigned_task_slot == 4u);
    CHECK(sweep->assigned_team == 0u && sweep->status == INC_IN_PROGRESS);

    /* Leaving a sweep never counts as finishing it. */
    s = report(0u, STATUS_EN_ROUTE, MISSION_LINK_NO_INCIDENT, 0u, 5u, 90u, 19, 21);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_stages_completed() == 0u);
    CHECK(sweep->slot_occupied && sweep->status == INC_OPEN);
    CHECK(sweep->assigned_team == NO_ASSIGNMENT);
}

static void test_line_parsing(void)
{
    MissionLinkStatus_t s = report(8u, STATUS_RESTING, MISSION_LINK_NO_INCIDENT, 0u, 0u, 70u, 30, 31);
    uint8_t wire[MISSION_LINK_STATUS_WIRE_SIZE];
    char line[128];
    uint32_t i;
    int n;

    fresh_mission();
    (void)mission_link_pack_status(&s, wire);
    n = snprintf(line, sizeof(line), "MLSTATUS ");
    for (i = 0u; i < MISSION_LINK_STATUS_WIRE_SIZE; i++)
    {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x", wire[i]);
    }

    CHECK(mission_mirror_ingest_line(line) == 1);
    CHECK(g_teams[8].status == STATUS_RESTING && g_teams[8].loc_x == 30 && g_teams[8].loc_y == 31);

    CHECK(mission_mirror_ingest_line("LIVEFRAME {}") == 0);
    CHECK(mission_mirror_lines_rejected() == 0u); /* not ours: not a reject */

    line[n - 1] = (line[n - 1] == '0') ? '1' : '0'; /* break the checksum */
    CHECK(mission_mirror_ingest_line(line) == 0);
    CHECK(mission_mirror_ingest_line("MLSTATUS f99f") == 0);
    CHECK(mission_mirror_ingest_line("MLSTATUS zz") == 0);
    CHECK(mission_mirror_lines_rejected() == 3u);
    CHECK(mission_mirror_reports_applied() == 1u);
}

static void test_evacuee_run_legs(void)
{
    MissionLinkStatus_t s;
    IncidentState_t *run = &g_system_tasks[6];
    SafeZone_t *zone;
    int16_t zone_x;
    int16_t zone_y;

    fresh_mission();
    CHECK(g_num_safe_zones > 0u);
    zone = &g_safezones[0];
    zone->evacuee_count = 10u;
    zone_x = zone->loc_x;
    zone_y = zone->loc_y;

    memset(run, 0, sizeof(*run));
    run->slot_occupied = 1u;
    run->incident_id = 1000u;
    run->incident_type = INCIDENT_EVACUEE_OFFBOARD;
    run->req_capability = SPEC_EXTRACTION;
    run->status = INC_OPEN;
    run->assigned_team = NO_ASSIGNMENT;
    run->support_team = NO_ASSIGNMENT;
    run->loc_x = zone_x;
    run->loc_y = zone_y;
    run->work_remaining = 100u;
    run->evac_safezone_id = 0u;
    run->evac_gateway_id = NO_EVAC_TARGET;
    run->evac_capacity = 6u;

    /* Leg 1: team 3 loads at the safe zone. */
    s = report(3u, STATUS_ON_SCENE, 1000u, 4u, 5u, 100u, zone_x, zone_y);
    mission_mirror_apply_status(&s);
    CHECK(run->assigned_team == 3u && run->status == INC_IN_PROGRESS && run->work_remaining == 4u);
    s = report(3u, STATUS_AVAILABLE, MISSION_LINK_NO_INCIDENT, 0u, 5u, 101u, zone_x, zone_y);
    mission_mirror_apply_status(&s);

    CHECK(mission_mirror_stages_completed() == 1u);
    CHECK(run->slot_occupied);
    CHECK(run->victim_count == 6u && zone->evacuee_count == 4u);
    CHECK(run->evac_gateway_id < NUM_EVACUATION_GATEWAYS);
    if (run->evac_gateway_id < NUM_EVACUATION_GATEWAYS)
    {
        CHECK(run->loc_x == g_evacuation_gateways[run->evac_gateway_id].loc_x &&
              run->loc_y == g_evacuation_gateways[run->evac_gateway_id].loc_y);
    }
    CHECK(run->status == INC_OPEN && run->assigned_team == NO_ASSIGNMENT);
    CHECK(run->work_remaining == 100u);

    /* Leg 2: another extraction team unloads at the gateway. */
    s = report(8u, STATUS_ON_SCENE, 1000u, 3u, 5u, 140u, run->loc_x, run->loc_y);
    mission_mirror_apply_status(&s);
    CHECK(run->assigned_team == 8u);
    s = report(8u, STATUS_AVAILABLE, MISSION_LINK_NO_INCIDENT, 0u, 5u, 141u, run->loc_x, run->loc_y);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_stages_completed() == 2u);
    CHECK(!run->slot_occupied);
}

/* Reports the mirror must refuse or sanitize (review L18): a status
   outside TeamStatus_t, an equipment_level above EQ_MAX_LEVEL, and a
   report older than the one already applied for that team. */
static void test_bad_and_out_of_order_reports(void)
{
    MissionLinkStatus_t s;
    IncidentState_t *inc;

    fresh_mission();
    inc = make_incident(2u, 800u, INCIDENT_MASS_CASUALTY, SPEC_MEDICAL, 100u);

    /* First report for a team at tick 0 - prev->report_tick is only the
       zeroed default before any report, so this must still apply. */
    s = report(5u, STATUS_EN_ROUTE, 800u, 100u, 5u, 0u, 3, 4);
    mission_mirror_apply_status(&s);
    CHECK(g_teams[5].loc_x == 3 && g_teams[5].loc_y == 4);
    CHECK(inc->assigned_team == 5u);
    CHECK(mission_mirror_reports_applied() == 1u && mission_mirror_reports_ignored() == 0u);

    /* Status not in TeamStatus_t: ignored outright. */
    s = report(5u, (uint8_t)(STATUS_RESTING + 1), 800u, 90u, 5u, 1u, 6, 6);
    mission_mirror_apply_status(&s);
    CHECK(g_teams[5].status == STATUS_EN_ROUTE);
    CHECK(g_teams[5].loc_x == 3 && g_teams[5].loc_y == 4);
    CHECK(mission_mirror_reports_ignored() == 1u && mission_mirror_reports_applied() == 1u);

    /* equipment_level above EQ_MAX_LEVEL: clamped, report still applied. */
    s = report(5u, STATUS_ON_SCENE, 800u, 80u, 5u, 20u, 10, 10);
    s.equipment_level = 250u;
    mission_mirror_apply_status(&s);
    CHECK(g_teams[5].equipment_level == EQ_MAX_LEVEL);
    CHECK(inc->status == INC_IN_PROGRESS && inc->work_remaining == 80u);
    CHECK(mission_mirror_reports_applied() == 2u);

    /* Out of order: a tick-18 report arriving after tick 20 would move
       the team off the incident and re-open it. Must be ignored. */
    s = report(5u, STATUS_RESTING, MISSION_LINK_NO_INCIDENT, 0u, 5u, 18u, 1, 1);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_reports_ignored() == 2u && mission_mirror_reports_applied() == 2u);
    CHECK(mission_mirror_incidents_reopened() == 0u);
    CHECK(g_teams[5].status == STATUS_ON_SCENE && g_teams[5].loc_x == 10);
    CHECK(inc->assigned_team == 5u && inc->status == INC_IN_PROGRESS);

    /* Same tick as the last applied report: a real second change within
       one tick, applied. */
    s = report(5u, STATUS_ON_SCENE, 800u, 78u, 5u, 20u, 10, 10);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_reports_applied() == 3u && inc->work_remaining == 78u);

    /* Another team's clock is independent: team 6's first report at an
       earlier tick than team 5's latest still applies. */
    s = report(6u, STATUS_AVAILABLE, MISSION_LINK_NO_INCIDENT, 0u, 5u, 5u, 7, 7);
    mission_mirror_apply_status(&s);
    CHECK(g_teams[6].loc_x == 7 && mission_mirror_reports_applied() == 4u);

    /* A new mission forgets the per-team ordering state. */
    mission_mirror_begin();
    CHECK(mission_mirror_reports_ignored() == 0u);
    s = report(5u, STATUS_AVAILABLE, MISSION_LINK_NO_INCIDENT, 0u, 5u, 1u, 2, 2);
    mission_mirror_apply_status(&s);
    CHECK(mission_mirror_reports_applied() == 1u && g_teams[5].loc_x == 2);
}

int main(void)
{
    test_report_copies_team_and_assignment();
    test_finished_stage_completes_here();
    test_left_early_reopens();
    test_support_team();
    test_sweep_system_task();
    test_line_parsing();
    test_evacuee_run_legs();
    test_bad_and_out_of_order_reports();

    if (g_failures == 0)
    {
        printf("mission_mirror_selftest: all checks passed\n");
        return 0;
    }
    printf("mission_mirror_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

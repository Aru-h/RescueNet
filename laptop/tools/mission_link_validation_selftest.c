/*
 * mission_link_validation_selftest.c - board-side incident table limits
 * and wire-field validation for core/mission_link.c.
 *
 * Covers two review findings:
 *
 *   H3  A board's NUM_INCIDENTS incident slots filled up with incidents
 *       it could not use, and mission_link_apply_incident() silently
 *       dropped every new one once full. It now evicts the least urgent
 *       incident nobody is working when a strictly more urgent one
 *       arrives, drops (and counts) otherwise, and picks the same victim
 *       whatever the board's slot layout, so both boards stay consistent.
 *
 *   H4  Incident/roster/fault frames arrive over USART1 with an 8-bit
 *       additive checksum (about 1 corrupted frame in 256 passes), and
 *       their fields are later used as array indices and grid
 *       coordinates. Out-of-range values are rejected and counted.
 *
 * Built by `make check-mission-link-validation-selftest`, normally and
 * under -fsanitize=address,undefined, so an out-of-bounds access on any
 * of these malformed frames fails the target even where a CHECK() alone
 * wouldn't notice.
 *
 * Links all of core/ plus laptop/debug.c and laptop/snapshot.c, the same
 * set laptop/tools/two_board_cbba_harness.c uses - mission_link.c reaches
 * incident.c (incident_close_for_authority()) and through it most of the
 * simulator.
 */

#include <stdio.h>
#include <string.h>

#include "mission_state.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "incident.h"

extern uint8_t g_simulation_trace_enabled;

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

static void reset_world(void)
{
    initialize_incidents();

    for (uint8_t t = 0; t < NUM_TEAMS; t++)
    {
        memset(&g_teams[t], 0, sizeof(g_teams[t]));
        g_teams[t].team_id = t;
        g_teams[t].status = STATUS_AVAILABLE;
        g_teams[t].assigned_task_pool = TASK_POOL_NONE;
        g_teams[t].assigned_task_slot = NO_ASSIGNMENT;
    }
}

static MissionLinkIncident_t make_incident(uint32_t id, uint8_t priority)
{
    MissionLinkIncident_t msg;

    memset(&msg, 0, sizeof(msg));
    msg.incident_id = id;
    msg.loc_x = (int16_t)(id % GRID_WIDTH);
    msg.loc_y = (int16_t)((id / GRID_WIDTH) % GRID_HEIGHT);
    msg.work_remaining = 50u;
    msg.priority = priority;
    msg.incident_type = (uint8_t)INCIDENT_SURVIVOR_SIGNAL;
    msg.req_capability = (uint8_t)SPEC_SEARCH;
    msg.status = (uint8_t)INC_OPEN;
    msg.victim_count = 1u;
    msg.discovered = 1u;
    return msg;
}

static const IncidentState_t *find_incident(uint32_t id)
{
    for (int i = 0; i < NUM_INCIDENTS; i++)
    {
        if (g_incidents[i].slot_occupied && g_incidents[i].incident_id == id)
            return &g_incidents[i];
    }
    return NULL;
}

static int occupied_count(void)
{
    int n = 0;

    for (int i = 0; i < NUM_INCIDENTS; i++)
        n += g_incidents[i].slot_occupied ? 1 : 0;
    return n;
}

/* Fills the table with ids 100..119 in the given order, priorities
   cycling HIGH, MEDIUM, LOW, LOW (so several LOW ties exist). */
static void fill_table(int reversed)
{
    for (int k = 0; k < NUM_INCIDENTS; k++)
    {
        int i = reversed ? (NUM_INCIDENTS - 1 - k) : k;
        static const uint8_t prios[4] = {2u, 3u, 4u, 4u};

        MissionLinkIncident_t msg = make_incident(100u + (uint32_t)i, prios[i % 4]);

        mission_link_apply_incident(&msg);
    }
}

/* H3: full table, strictly more urgent new incident evicts the least
   urgent unworked one; the choice is by incident_id, so two different
   slot layouts (arrival orders) evict the same incident. */
static void test_full_table_eviction(void)
{
    uint32_t drops_before;
    uint32_t evicted_forward = 0u, evicted_reversed = 0u;

    for (int layout = 0; layout < 2; layout++)
    {
        MissionLinkIncident_t critical = make_incident(500u, (uint8_t)PRIORITY_CRITICAL);

        reset_world();
        fill_table(layout);
        CHECK(occupied_count() == NUM_INCIDENTS);

        drops_before = mission_link_incident_drop_count();
        mission_link_apply_incident(&critical);
        CHECK(find_incident(500u) != NULL);
        CHECK(occupied_count() == NUM_INCIDENTS);
        CHECK(mission_link_incident_drop_count() == drops_before + 1u);

        for (uint32_t id = 100u; id < 100u + NUM_INCIDENTS; id++)
        {
            if (find_incident(id) == NULL)
            {
                if (layout == 0)
                    evicted_forward = id;
                else
                    evicted_reversed = id;
            }
        }
    }

    /* LOW (4) is the least urgent tier; ids 102,103,106,107,...,118,119
       are LOW, and the highest id among them goes. */
    CHECK(evicted_forward == 119u);
    CHECK(evicted_reversed == evicted_forward);
}

/* H3: nothing evictable, or the newcomer isn't strictly more urgent ->
   dropped and counted, table untouched. */
static void test_full_table_drops(void)
{
    uint32_t drops_before;
    MissionLinkIncident_t low = make_incident(600u, (uint8_t)PRIORITY_LOW);
    MissionLinkIncident_t critical = make_incident(601u, (uint8_t)PRIORITY_CRITICAL);
    MissionLinkIncident_t hidden = make_incident(602u, (uint8_t)PRIORITY_CRITICAL);

    reset_world();
    fill_table(0);

    /* Equal urgency to the least urgent entry: dropped, no churn. */
    drops_before = mission_link_incident_drop_count();
    mission_link_apply_incident(&low);
    CHECK(find_incident(600u) == NULL);
    CHECK(find_incident(119u) != NULL);
    CHECK(mission_link_incident_drop_count() == drops_before + 1u);

    /* An undiscovered incident never evicts anything. */
    hidden.discovered = 0u;
    mission_link_apply_incident(&hidden);
    CHECK(find_incident(602u) == NULL);
    CHECK(mission_link_incident_drop_count() == drops_before + 2u);

    /* Every entry is being worked (not OPEN, or a team is on it): even a
       CRITICAL newcomer is dropped rather than displacing live work. */
    for (int i = 0; i < NUM_INCIDENTS; i += 2)
    {
        g_incidents[i].status = INC_ASSIGNED;
        g_incidents[i].assigned_team = (uint8_t)((i / 2) % NUM_TEAMS);
    }
    /* The 10 odd slots stay OPEN/unassigned in the table itself, but a
       team on this board is already heading for each (one team per
       slot). */
    for (int i = 1; i < NUM_INCIDENTS; i += 2)
    {
        g_teams[i / 2].assigned_task_pool = TASK_POOL_INCIDENT;
        g_teams[i / 2].assigned_task_slot = (uint8_t)i;
    }
    mission_link_apply_incident(&critical);
    CHECK(find_incident(601u) == NULL);
    CHECK(mission_link_incident_drop_count() == drops_before + 3u);
    CHECK(occupied_count() == NUM_INCIDENTS);
}

/* H3: a RESOLVED frame for an unknown incident never takes a slot and is
   not a drop. */
static void test_resolved_never_takes_slot(void)
{
    uint32_t drops_before;
    MissionLinkIncident_t done = make_incident(700u, (uint8_t)PRIORITY_CRITICAL);

    reset_world();
    fill_table(0);
    drops_before = mission_link_incident_drop_count();
    done.status = (uint8_t)INC_RESOLVED;
    mission_link_apply_incident(&done);
    CHECK(find_incident(700u) == NULL);
    CHECK(occupied_count() == NUM_INCIDENTS);
    CHECK(mission_link_incident_drop_count() == drops_before);
}

/* H4: every out-of-range incident field rejects the whole frame, counts
   it, and leaves the table untouched - nothing is created, and an
   existing incident is not overwritten with the bad values. */
static void test_rejects_malformed_incidents(void)
{
    MissionLinkIncident_t good = make_incident(800u, (uint8_t)PRIORITY_HIGH);
    MissionLinkIncident_t bad[10];
    const IncidentState_t *held;
    uint32_t rejects_before;
    int n = 0;

    reset_world();
    mission_link_apply_incident(&good);
    held = find_incident(800u);
    CHECK(held != NULL);

    bad[n] = good; bad[n].loc_x = -1; n++;
    bad[n] = good; bad[n].loc_x = GRID_WIDTH; n++;
    bad[n] = good; bad[n].loc_y = -300; n++;
    bad[n] = good; bad[n].loc_y = GRID_HEIGHT; n++;
    bad[n] = good; bad[n].incident_type = (uint8_t)NUM_INCIDENT_TYPES; n++;
    bad[n] = good; bad[n].req_capability = (uint8_t)NUM_TEAM_SPECIALIZATIONS; n++;
    bad[n] = good; bad[n].priority = 0u; n++;
    bad[n] = good; bad[n].priority = 5u; n++;
    bad[n] = good; bad[n].status = (uint8_t)(INC_RESOLVED + 1); n++;
    bad[n] = good; bad[n].incident_type = 0xFFu; bad[n].req_capability = 0xFFu; n++;

    rejects_before = mission_link_incident_reject_count();
    for (int i = 0; i < n; i++)
    {
        MissionLinkIncident_t fresh = bad[i];

        /* as an update to the held incident */
        mission_link_apply_incident(&bad[i]);
        /* and as a brand-new incident */
        fresh.incident_id = 900u + (uint32_t)i;
        mission_link_apply_incident(&fresh);
        CHECK(find_incident(fresh.incident_id) == NULL);
    }
    CHECK(mission_link_incident_reject_count() == rejects_before + 2u * (uint32_t)n);

    held = find_incident(800u);
    CHECK(held != NULL);
    if (held != NULL)
    {
        CHECK(held->loc_x == good.loc_x && held->loc_y == good.loc_y);
        CHECK((uint8_t)held->priority == good.priority);
        CHECK((uint8_t)held->incident_type == good.incident_type);
        CHECK((uint8_t)held->req_capability == good.req_capability);
        CHECK(held->status == INC_OPEN);
    }
    CHECK(occupied_count() == 1);

    /* Boundary values are still accepted. */
    good.incident_id = 801u;
    good.loc_x = GRID_WIDTH - 1;
    good.loc_y = GRID_HEIGHT - 1;
    good.priority = (uint8_t)PRIORITY_LOW;
    good.incident_type = (uint8_t)(NUM_INCIDENT_TYPES - 1);
    good.req_capability = (uint8_t)(NUM_TEAM_SPECIALIZATIONS - 1);
    mission_link_apply_incident(&good);
    CHECK(find_incident(801u) != NULL);
    CHECK(mission_link_incident_reject_count() == rejects_before + 2u * (uint32_t)n);
}

/* H4: roster specialization/fitness_tier out of range reject the whole
   roster (counted), and the team keeps its previous identity. */
static void test_rejects_malformed_roster(void)
{
    MissionLinkRoster_t roster;
    uint32_t rejects_before;

    reset_world();
    memset(&roster, 0, sizeof(roster));
    roster.team_id = 3u;
    roster.specialization = (uint8_t)SPEC_MEDICAL;
    roster.fitness_tier = (uint8_t)MODERATE;
    roster.loc_x = 5;
    roster.loc_y = 6;
    roster.stamina = 90u;
    roster.equipment_level = 80u;
    mission_link_apply_roster(&roster);
    CHECK(g_teams[3].specialization == SPEC_MEDICAL);

    rejects_before = mission_link_roster_reject_count();

    roster.specialization = (uint8_t)NUM_TEAM_SPECIALIZATIONS;
    roster.loc_x = 9;
    mission_link_apply_roster(&roster);
    roster.specialization = (uint8_t)SPEC_SEARCH;
    roster.fitness_tier = (uint8_t)(LIGHT + 1);
    mission_link_apply_roster(&roster);
    roster.fitness_tier = 0xFFu;
    mission_link_apply_roster(&roster);

    CHECK(mission_link_roster_reject_count() == rejects_before + 3u);
    CHECK(g_teams[3].specialization == SPEC_MEDICAL);
    CHECK(g_teams[3].fitness_tier == (uint8_t)MODERATE);
    CHECK(g_teams[3].loc_x == 5);

    roster.fitness_tier = (uint8_t)LIGHT;
    roster.specialization = (uint8_t)SPEC_SUPPORT;
    mission_link_apply_roster(&roster);
    CHECK(mission_link_roster_reject_count() == rejects_before + 3u);
    CHECK(g_teams[3].specialization == SPEC_SUPPORT);
}

/* H4: fault active flags are normalized to 0/1 and severity is clamped;
   an out-of-range team_id is ignored. */
static void test_fault_normalized(void)
{
    MissionLinkFault_t fault;

    reset_world();
    memset(&fault, 0, sizeof(fault));
    fault.team_id = 4u;
    fault.active[FAULT_CLASS_MOBILITY] = 0x80u;
    fault.severity[FAULT_CLASS_MOBILITY] = 0xFFu;
    fault.active[FAULT_CLASS_RESOURCE] = 1u;
    fault.severity[FAULT_CLASS_RESOURCE] = 2u;
    mission_link_apply_fault(&fault);

    CHECK(g_teams[4].fault_state.active[FAULT_CLASS_MOBILITY] == 1u);
    CHECK(g_teams[4].fault_state.severity[FAULT_CLASS_MOBILITY] == MISSION_LINK_FAULT_SEVERITY_MAX);
    CHECK(g_teams[4].fault_state.active[FAULT_CLASS_RESOURCE] == 1u);
    CHECK(g_teams[4].fault_state.severity[FAULT_CLASS_RESOURCE] == 2u);
    CHECK(g_teams[4].fault_state.active[FAULT_CLASS_COMMUNICATION] == 0u);

    fault.team_id = NUM_TEAMS;
    mission_link_apply_fault(&fault); /* must not touch g_teams[NUM_TEAMS] (ASan) */
}

int main(void)
{
    g_simulation_trace_enabled = 0;

    test_full_table_eviction();
    test_full_table_drops();
    test_resolved_never_takes_slot();
    test_rejects_malformed_incidents();
    test_rejects_malformed_roster();
    test_fault_normalized();

    if (g_failures == 0)
    {
        printf("mission_link_validation_selftest: all checks passed\n");
        return 0;
    }

    printf("mission_link_validation_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

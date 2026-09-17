#include <stdio.h>
#include <string.h>

#include "mission_feed.h"
#include "mission_state.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "environment.h"

/* Largest mission-link wire frame emitted here (map row, 40 bytes). */
#define MISSION_FEED_MAX_WIRE 48u

/* Last incident payload emitted per slot, so a frame goes out only when
   something the board sees actually changed. */
typedef struct
{
    uint8_t valid;
    MissionLinkIncident_t msg;
} MissionFeedIncidentCache_t;

static MissionFeedIncidentCache_t g_sent_incident[NUM_INCIDENTS];
/* Search sweeps and evacuee runs, which live in the system task pool -
   see mission_feed_sends_system_task(). */
static MissionFeedIncidentCache_t g_sent_sweep[NUM_SYSTEM_TASKS];
static MissionLinkFault_t g_sent_fault[NUM_TEAMS];
/* Map bytes last sent per cell ([x][y]), so only changed rows go out. */
static uint8_t g_sent_map[GRID_WIDTH][GRID_HEIGHT];

static void mission_feed_emit(uint32_t tick, const uint8_t *wire, uint32_t len)
{
    uint32_t i;

    printf("MLFRAME %u ", (unsigned)tick);
    for (i = 0u; i < len; i++)
    {
        printf("%02x", (unsigned)wire[i]);
    }
    printf("\n");
}

static uint8_t mission_feed_quantize_stamina(float stamina)
{
    if (!(stamina > 0.0f))
    {
        return 0u;
    }
    if (stamina >= 100.0f)
    {
        return 100u;
    }
    return (uint8_t)(stamina + 0.5f);
}

static void mission_feed_build_incident(const IncidentState_t *inc, MissionLinkIncident_t *out)
{
    memset(out, 0, sizeof(*out));
    out->incident_id = inc->incident_id;
    out->loc_x = inc->loc_x;
    out->loc_y = inc->loc_y;
    out->work_remaining = inc->work_remaining;
    out->priority = (uint8_t)inc->priority;
    out->incident_type = (uint8_t)inc->incident_type;
    out->req_capability = (uint8_t)inc->req_capability;
    out->status = inc->status;
    out->victim_count = inc->victim_count;
    out->discovered = inc->discovered;
}

/* Builds row y from the laptop's environment. Returns 1 if it differs from
   what was last sent (and records it as sent). */
static int mission_feed_build_map_row(uint8_t y, MissionLinkMapRow_t *row)
{
    int changed = 0;
    uint8_t x;

    row->row = y;
    for (x = 0u; x < GRID_WIDTH; x++)
    {
        uint8_t cell = mission_link_map_encode_cell(environment_cell_is_navigable(x, y),
                                                    environment_cell_travel_cost(x, y));
        row->cells[x] = cell;
        if (g_sent_map[x][y] != cell)
        {
            g_sent_map[x][y] = cell;
            changed = 1;
        }
    }
    return changed;
}

static void mission_feed_emit_sites(void)
{
    uint8_t wire[MISSION_FEED_MAX_WIRE];
    MissionLinkSites_t sites;
    uint8_t i;

    memset(&sites, 0, sizeof(sites));
    sites.num_safe_zones = g_num_safe_zones;
    for (i = 0u; i < g_num_safe_zones && i < MAX_SAFE_ZONES; i++)
    {
        sites.safe_zones[i].loc_x = g_safezones[i].loc_x;
        sites.safe_zones[i].loc_y = g_safezones[i].loc_y;
        sites.safe_zones[i].capacity = g_safezones[i].capacity;
    }
    for (i = 0u; i < NUM_EVACUATION_GATEWAYS; i++)
    {
        sites.gateway_x[i] = g_evacuation_gateways[i].loc_x;
        sites.gateway_y[i] = g_evacuation_gateways[i].loc_y;
    }
    mission_feed_emit(0u, wire, mission_link_pack_sites(&sites, wire));
}

static void mission_feed_emit_clock(uint32_t tick)
{
    uint8_t wire[MISSION_FEED_MAX_WIRE];
    MissionLinkClock_t clock;

    clock.tick = tick;
    mission_feed_emit(tick, wire, mission_link_pack_clock(&clock, wire));
}

void mission_feed_begin(void)
{
    uint8_t wire[MISSION_FEED_MAX_WIRE];
    uint32_t t;

    memset(g_sent_incident, 0, sizeof(g_sent_incident));
    memset(g_sent_sweep, 0, sizeof(g_sent_sweep));

    for (t = 0u; t < NUM_TEAMS; t++)
    {
        const TeamState_t *team = &g_teams[t];
        MissionLinkRoster_t roster;

        memset(&roster, 0, sizeof(roster));
        roster.team_id = (uint8_t)t;
        roster.specialization = team->specialization;
        roster.equipment_flags = team->equipment_flags;
        roster.fitness_tier = team->fitness_tier;
        roster.loc_x = team->loc_x;
        roster.loc_y = team->loc_y;
        roster.stamina = mission_feed_quantize_stamina(team->stamina);
        roster.equipment_level = team->equipment_level;
        mission_feed_emit(0u, wire, mission_link_pack_roster(&roster, wire));

        /* Every board starts with no active faults, so the first frame
           per team goes out only once something is actually active. */
        memset(&g_sent_fault[t], 0, sizeof(g_sent_fault[t]));
        g_sent_fault[t].team_id = (uint8_t)t;
    }

    mission_feed_emit_sites();

    {
        MissionLinkMapRow_t row;
        uint8_t y;

        for (y = 0u; y < GRID_HEIGHT; y++)
        {
            (void)mission_feed_build_map_row(y, &row);
            mission_feed_emit(0u, wire, mission_link_pack_map_row(&row, wire));
        }
    }

    mission_feed_emit_clock(0u);
    fflush(stdout);
}

/* Emits one task slot's frame if anything a board acts on changed.
   Returns 1 if a frame went out. */
static int mission_feed_task_slot(uint32_t tick, const IncidentState_t *inc,
                                  MissionFeedIncidentCache_t *sent)
{
    uint8_t wire[MISSION_FEED_MAX_WIRE];
    MissionLinkIncident_t msg;
    int emitted = 0;

    /* The incident this slot last reported is gone (slot freed or
       reused): make sure the board sees it closed, even if the
       simulator cleared the slot in the same tick it resolved it. */
    if (sent->valid &&
        (!inc->slot_occupied || inc->incident_id != sent->msg.incident_id))
    {
        if (sent->msg.status != (uint8_t)INC_RESOLVED)
        {
            sent->msg.status = (uint8_t)INC_RESOLVED;
            mission_feed_emit(tick, wire, mission_link_pack_incident(&sent->msg, wire));
            emitted = 1;
        }
        sent->valid = 0u;
    }

    if (!inc->slot_occupied)
    {
        return emitted;
    }

    mission_feed_build_incident(inc, &msg);
    if (sent->valid)
    {
        /* work_remaining alone changing is not news for a board: on
           a board mission it IS the boards' own progress, copied in
           from their reports (laptop/mission_mirror.h), and a board
           keeps its live figure over the laptop's anyway
           (mission_link_apply_incident()). A new stage still goes
           out - it changes req_capability. */
        MissionLinkIncident_t cmp = msg;
        cmp.work_remaining = sent->msg.work_remaining;
        if (memcmp(&sent->msg, &cmp, sizeof(cmp)) == 0)
        {
            return emitted;
        }
    }

    sent->msg = msg;
    sent->valid = 1u;
    mission_feed_emit(tick, wire, mission_link_pack_incident(&msg, wire));
    emitted = 1;

    return emitted;
}

/* System tasks the boards work: search sweeps and evacuee runs. The rest
   (equipment caches) a board creates for its own teams. */
static int mission_feed_sends_system_task(const IncidentState_t *task)
{
    return task->incident_type == INCIDENT_UNSEARCHED_SECTOR ||
           task->incident_type == INCIDENT_EVACUEE_OFFBOARD;
}

void mission_feed_tasks(uint32_t tick)
{
    uint32_t i;

    for (i = 0u; i < NUM_INCIDENTS; i++)
    {
        (void)mission_feed_task_slot(tick, &g_incidents[i], &g_sent_incident[i]);
    }

    for (i = 0u; i < NUM_SYSTEM_TASKS; i++)
    {
        const IncidentState_t *task = &g_system_tasks[i];

        /* A slot now holding a task the boards don't get counts as freed,
           so whatever used it last is still closed on the boards. */
        if (task->slot_occupied && !mission_feed_sends_system_task(task))
        {
            static const IncidentState_t freed_slot;
            (void)mission_feed_task_slot(tick, &freed_slot, &g_sent_sweep[i]);
            continue;
        }
        (void)mission_feed_task_slot(tick, task, &g_sent_sweep[i]);
    }

    fflush(stdout);
}

void mission_feed_tick(uint32_t tick)
{
    uint8_t wire[MISSION_FEED_MAX_WIRE];
    uint32_t i;
    int emitted = 0;

    mission_feed_tasks(tick);

    for (i = 0u; i < NUM_TEAMS; i++)
    {
        const TeamFaultState_t *fs = &g_teams[i].fault_state;
        MissionLinkFault_t msg;
        uint32_t c;

        memset(&msg, 0, sizeof(msg));
        msg.team_id = (uint8_t)i;
        for (c = 0u; c < NUM_OPERATIONAL_FAULT_CLASSES; c++)
        {
            msg.active[c] = fs->active[c];
            msg.severity[c] = fs->severity[c];
        }

        if (memcmp(&msg, &g_sent_fault[i], sizeof(msg)) == 0)
        {
            continue;
        }

        g_sent_fault[i] = msg;
        mission_feed_emit(tick, wire, mission_link_pack_fault(&msg, wire));
        emitted = 1;
    }

    /* Rows whose cells changed this tick: hazard spread, aftershocks,
       road damage, environment faults. */
    {
        MissionLinkMapRow_t row;
        uint8_t y;

        for (y = 0u; y < GRID_HEIGHT; y++)
        {
            if (mission_feed_build_map_row(y, &row))
            {
                mission_feed_emit(tick, wire, mission_link_pack_map_row(&row, wire));
            }
        }
    }

    /* Last, so a board has this tick's incidents, faults and map before it
       is allowed to run the tick. */
    mission_feed_emit_clock(tick);
    (void)emitted;
    fflush(stdout);
}

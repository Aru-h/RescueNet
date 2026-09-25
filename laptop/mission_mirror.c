#include <stdio.h>
#include <string.h>

#include "mission_mirror.h"
#include "mission_state.h"
#include "mission_link.h"
#include "mission_link_codec.h"
#include "incident.h"

/* laptop/snapshot.c - the dataset rows read board-reported work and
   stamina through this anchor; not declared in a header (same
   convention that file already uses for its own cross-file calls). */
extern void laptop_mission_link_status_received(const MissionLinkStatus_t *status);

/* Last report per team, as far as finishing an incident needs it. */
typedef struct
{
    uint8_t  known;
    uint32_t incident_id;
    uint16_t work_remaining;
    uint8_t  work_rate;
    uint32_t report_tick;
} MissionMirrorReport_t;

static MissionMirrorReport_t g_last_report[NUM_TEAMS];
static uint32_t g_reports_applied;
static uint32_t g_lines_rejected;
static uint32_t g_stages_completed;
static uint32_t g_incidents_reopened;
static uint32_t g_reports_ignored;

void mission_mirror_begin(void)
{
    memset(g_last_report, 0, sizeof(g_last_report));
    g_reports_applied = 0u;
    g_reports_ignored = 0u;
    g_lines_rejected = 0u;
    g_stages_completed = 0u;
    g_incidents_reopened = 0u;
}

static IncidentState_t *mission_mirror_find_task(uint32_t incident_id, TaskPool_t *pool_out, uint8_t *slot_out)
{
    uint8_t i;

    for (i = 0u; i < NUM_INCIDENTS; i++)
    {
        if (g_incidents[i].slot_occupied && g_incidents[i].incident_id == incident_id)
        {
            *pool_out = TASK_POOL_INCIDENT;
            *slot_out = i;
            return &g_incidents[i];
        }
    }

    for (i = 0u; i < NUM_SYSTEM_TASKS; i++)
    {
        if (g_system_tasks[i].slot_occupied && g_system_tasks[i].incident_id == incident_id)
        {
            *pool_out = TASK_POOL_SYSTEM;
            *slot_out = i;
            return &g_system_tasks[i];
        }
    }

    return NULL;
}

/* Work left on incident_id at at_tick, from the last report of every
   team that was on it (primary and support both add their rate). */
static uint16_t mission_mirror_work_left(uint32_t incident_id, uint32_t at_tick)
{
    uint32_t anchor_work = 0u;
    uint32_t anchor_tick = 0u;
    uint32_t done = 0u;
    uint8_t found = 0u;
    uint8_t t;

    for (t = 0u; t < NUM_TEAMS; t++)
    {
        const MissionMirrorReport_t *r = &g_last_report[t];

        if (!r->known || r->incident_id != incident_id)
        {
            continue;
        }

        /* The freshest report carries the best work figure. */
        if (!found || r->report_tick > anchor_tick)
        {
            anchor_work = r->work_remaining;
            anchor_tick = r->report_tick;
            found = 1u;
        }
    }

    if (!found)
    {
        return 0u;
    }

    for (t = 0u; t < NUM_TEAMS; t++)
    {
        const MissionMirrorReport_t *r = &g_last_report[t];

        if (r->known && r->incident_id == incident_id && at_tick > anchor_tick)
        {
            done += (uint32_t)r->work_rate * (at_tick - anchor_tick);
        }
    }

    return (done >= anchor_work) ? 0u : (uint16_t)(anchor_work - done);
}

/* team_id's report no longer names prev->incident_id. */
static void mission_mirror_team_left_task(uint8_t team_id, const MissionMirrorReport_t *prev,
                                          uint32_t now_tick)
{
    TaskPool_t pool;
    uint8_t slot;
    IncidentState_t *task = mission_mirror_find_task(prev->incident_id, &pool, &slot);

    if (task == NULL)
    {
        return; /* already resolved or cleared here */
    }

    if (task->support_team == team_id)
    {
        task->support_team = NO_ASSIGNMENT;
        return;
    }

    if (task->assigned_team != team_id)
    {
        return; /* another team has since taken it */
    }

    if ((pool == TASK_POOL_INCIDENT || task->incident_type == INCIDENT_EVACUEE_OFFBOARD) &&
        task->incident_type != INCIDENT_UNSEARCHED_SECTOR &&
        mission_mirror_work_left(prev->incident_id, now_tick) == 0u)
    {
        /* The stage is done. complete_incident_stage() releases the
           team through g_teams[team_id]'s assignment, so point it here
           first; the team fields are overwritten from this same report
           right after. */
        g_teams[team_id].assigned_task_pool = pool;
        g_teams[team_id].assigned_task_slot = slot;
        task->work_remaining = 0u;
        task->status = INC_IN_PROGRESS;
        complete_incident_stage(task);
        g_stages_completed++;

        /* An evacuee run that just loaded continues to its gateway. The
           fast simulation keeps the same team on it; here the gateway leg
           is open to any extraction team, because the board that ran the
           first leg has already let its team go (complete_incident_
           stage()'s SIM_ROLE_BOARD case). The loaded team is usually the
           nearest bidder anyway. */
        if (task->slot_occupied && task->incident_type == INCIDENT_EVACUEE_OFFBOARD)
        {
            task->status = INC_OPEN;
            task->assigned_team = NO_ASSIGNMENT;
        }
        return;
    }

    /* Left before the work was done - or a sweep, which only the
       laptop's coverage check ever finishes. */
    task->assigned_team = NO_ASSIGNMENT;
    if (task->status == INC_ASSIGNED || task->status == INC_IN_PROGRESS)
    {
        task->status = INC_OPEN;
        g_incidents_reopened++;
    }
}

void mission_mirror_apply_status(const MissionLinkStatus_t *status)
{
    TeamState_t *team;
    MissionMirrorReport_t *prev;

    if (status == NULL || status->team_id >= NUM_TEAMS)
    {
        return;
    }

    if (status->status > STATUS_RESTING)
    {
        /* Not a TeamStatus_t value. The frame's checksum passed, so this
           is a board/laptop schema mismatch rather than line noise - but
           either way there is no sane team state to mirror from it. */
        g_reports_ignored++;
        return;
    }

    team = &g_teams[status->team_id];
    prev = &g_last_report[status->team_id];

    if (prev->known && status->report_tick < prev->report_tick)
    {
        /* Out-of-order delivery: older than the report already applied
           for this team (a relayed frame overtaken by a newer one, or a
           retransmit). Applying it would roll the team back and could
           run mission_mirror_team_left_task() for a move the newer
           report already superseded. Same-tick reports are applied - a
           team can legitimately change twice within one tick. Only
           compared once a report is known: before that, prev->
           report_tick is just the zeroed default. */
        g_reports_ignored++;
        return;
    }

    if (prev->known && prev->incident_id != MISSION_LINK_NO_INCIDENT &&
        prev->incident_id != status->assigned_incident_id)
    {
        mission_mirror_team_left_task(status->team_id, prev, status->report_tick);
    }

    team->status = status->status;
    team->equipment_level = (status->equipment_level > EQ_MAX_LEVEL)
        ? (uint8_t)EQ_MAX_LEVEL : status->equipment_level;
    team->stamina = (float)status->stamina_at_report;
    if (status->loc_x >= 0 && status->loc_x < GRID_WIDTH &&
        status->loc_y >= 0 && status->loc_y < GRID_HEIGHT)
    {
        team->loc_x = status->loc_x;
        team->loc_y = status->loc_y;
    }
    team->assigned_task_pool = TASK_POOL_NONE;
    team->assigned_task_slot = NO_ASSIGNMENT;

    if (status->assigned_incident_id != MISSION_LINK_NO_INCIDENT)
    {
        TaskPool_t pool;
        uint8_t slot;
        IncidentState_t *task = mission_mirror_find_task(status->assigned_incident_id, &pool, &slot);

        if (task != NULL && task->status != INC_RESOLVED)
        {
            team->assigned_task_pool = pool;
            team->assigned_task_slot = slot;

            if (task->assigned_team < NUM_TEAMS && task->assigned_team != status->team_id &&
                g_teams[task->assigned_team].assigned_task_pool == pool &&
                g_teams[task->assigned_team].assigned_task_slot == slot)
            {
                /* Another team is already on it as the primary. */
                task->support_team = status->team_id;
            }
            else
            {
                task->assigned_team = status->team_id;
                task->status = (status->status == STATUS_ON_SCENE) ? INC_IN_PROGRESS : INC_ASSIGNED;
                if (task->incident_type != INCIDENT_UNSEARCHED_SECTOR)
                {
                    task->work_remaining = status->work_remaining_at_report;
                }
            }
        }
    }

    prev->known = 1u;
    prev->incident_id = status->assigned_incident_id;
    prev->work_remaining = status->work_remaining_at_report;
    prev->work_rate = status->work_rate;
    prev->report_tick = status->report_tick;

    laptop_mission_link_status_received(status);
    g_reports_applied++;
}

static int mission_mirror_hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int mission_mirror_ingest_line(const char *line)
{
    static const char prefix[] = "MLSTATUS ";
    uint8_t wire[MISSION_LINK_STATUS_WIRE_SIZE];
    MissionLinkStatus_t status;
    const char *hex;
    uint32_t i;

    if (line == NULL || strncmp(line, prefix, sizeof(prefix) - 1u) != 0)
    {
        return 0;
    }

    hex = line + sizeof(prefix) - 1u;
    for (i = 0u; i < MISSION_LINK_STATUS_WIRE_SIZE; i++)
    {
        int hi = mission_mirror_hex_nibble(hex[2u * i]);
        int lo = (hi < 0) ? -1 : mission_mirror_hex_nibble(hex[2u * i + 1u]);

        if (hi < 0 || lo < 0)
        {
            g_lines_rejected++;
            return 0;
        }
        wire[i] = (uint8_t)((hi << 4) | lo);
    }

    {
        char end = hex[2u * MISSION_LINK_STATUS_WIRE_SIZE];
        if (end != '\0' && end != '\n' && end != '\r')
        {
            g_lines_rejected++;
            return 0;
        }
    }

    if (mission_link_parse_status(wire, MISSION_LINK_STATUS_WIRE_SIZE, &status) != 1)
    {
        g_lines_rejected++;
        return 0;
    }

    mission_mirror_apply_status(&status);
    return 1;
}

uint32_t mission_mirror_reports_applied(void) { return g_reports_applied; }
uint32_t mission_mirror_lines_rejected(void) { return g_lines_rejected; }
uint32_t mission_mirror_stages_completed(void) { return g_stages_completed; }
uint32_t mission_mirror_incidents_reopened(void) { return g_incidents_reopened; }
uint32_t mission_mirror_reports_ignored(void) { return g_reports_ignored; }

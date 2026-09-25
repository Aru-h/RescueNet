#include "snapshot.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "team.h"
#include "incident.h"
#include "mission_state.h"
#include "recovery_manager.h"
#include "watchdog.h"
#include "search_zones.h"
#include "mission_link.h"
#include "environment.h" /* environment_cell_travel_cost() - env_raw_travel_cost column below */

/*
 * core/cbba.c's cbba_team_is_local() - public, but deliberately not
 * declared in cbba.h (see that function's own comment for the
 * established "public, but not declared in cbba.h" convention).
 * Forward-declared here at its one call site in this file, same
 * pattern core/incident.c already follows for the same function.
 */
extern bool cbba_team_is_local(uint8_t team_id);

/* ---------------------------------------------------------------
 * Board-report anchor + laptop-side extrapolation
 * ---------------------------------------------------------------
 * Per-team last-received MissionLinkStatus_t anchor (schema v2 - see
 * mission_link.h) plus the extrapolation formula that turns it into a
 * smoothly-advancing display value between reports, instead of a
 * message every tick. See mission_link.h's MissionLinkStatus_t doc for
 * the full design; this is the laptop-side half of it.
 *
 * INGESTION STATUS: laptop_mission_link_status_received() below is
 * the reset point every incoming MissionLinkStatus_t frame must go
 * through (last-write-wins by arrival, same pattern CBBA already uses
 * for timestamp[] conflict resolution - see mission_link.h). Nothing
 * in this C build calls it yet: live MissionLinkStatus_t ingestion
 * from a real board is a USB/serial concern that today lives in
 * laptop/tools/app.py + laptop/tools/telemetry_bridge.py (Python), outside this
 * prompt's file ownership, and that bridge doesn't parse mission-link
 * frames yet either (only TelemetryFrame_t and MissionLinkOverride_t -
 * see app.py's own wire-format section). Until one of those wires a
 * real frame in, g_mission_link_anchor[] simply stays all-unknown and
 * mission_link_extrapolated_stamina() below transparently falls back
 * to the team's own live TeamState_t.stamina - so a standalone `tron`
 * desktop run (which never receives real board reports at all) is
 * completely unaffected by any of this.
 */
typedef struct
{
    uint8_t  known;                     /* 0 until the first report arrives */
    uint32_t assigned_incident_id;
    uint16_t work_remaining_at_report;
    uint8_t  work_rate;
    uint8_t  stamina_at_report;
    uint32_t report_tick;
} MissionLinkAnchor_t;

static MissionLinkAnchor_t g_mission_link_anchor[NUM_TEAMS];

/* Reset point for every incoming MissionLinkStatus_t frame - see the
   INGESTION STATUS note above for who's expected to call this once a
   real receiver exists. */
void laptop_mission_link_status_received(const MissionLinkStatus_t *status)
{
    MissionLinkAnchor_t *anchor;

    if (status == NULL || status->team_id >= NUM_TEAMS)
        return;

    anchor = &g_mission_link_anchor[status->team_id];

    anchor->known = 1u;
    anchor->assigned_incident_id = status->assigned_incident_id;
    anchor->work_remaining_at_report = status->work_remaining_at_report;
    anchor->work_rate = status->work_rate;
    anchor->stamina_at_report = status->stamina_at_report;
    anchor->report_tick = status->report_tick;
}

/*
 * Displayed work-remaining for team_id's currently-anchored incident at
 * current_tick: work_remaining_at_report - work_rate * (current_tick -
 * report_tick), clamped at 0 - see mission_link.h's MissionLinkStatus_t
 * doc. Returns 0 (not the team's live g_incidents[] value) if there is
 * no live anchor yet or the team is idle (MISSION_LINK_NO_INCIDENT) -
 * callers that need a live-sim fallback for a team with no anchor
 * should check mission_link_anchor_is_live() first, same as
 * mission_link_extrapolated_stamina() does below.
 */
uint16_t mission_link_extrapolated_work_remaining(uint8_t team_id, uint32_t current_tick)
{
    const MissionLinkAnchor_t *anchor;
    uint32_t elapsed_ticks;
    uint32_t decayed;

    if (team_id >= NUM_TEAMS)
        return 0u;

    anchor = &g_mission_link_anchor[team_id];

    if (!anchor->known || anchor->assigned_incident_id == MISSION_LINK_NO_INCIDENT)
        return 0u;

    /* current_tick is expected to be >= report_tick (extrapolating
       forward from an anchor already received) - an out-of-order or
       stale current_tick just clamps to the anchor's own value rather
       than underflowing. */
    elapsed_ticks = (current_tick > anchor->report_tick) ?
        (current_tick - anchor->report_tick) : 0u;

    decayed = (uint32_t)anchor->work_rate * elapsed_ticks;

    if (decayed >= (uint32_t)anchor->work_remaining_at_report)
        return 0u;

    return (uint16_t)((uint32_t)anchor->work_remaining_at_report - decayed);
}

/* Same extrapolation, stamina equivalent - clamped to [0, 100] since
   stamina's wire quantization (mission_link.h) is 0-100, unlike
   work_remaining which has no fixed upper bound. */
static uint8_t mission_link_extrapolated_stamina(uint8_t team_id, uint32_t current_tick)
{
    const MissionLinkAnchor_t *anchor;
    uint32_t elapsed_ticks;
    uint32_t decayed;
    uint32_t result;

    anchor = &g_mission_link_anchor[team_id];

    elapsed_ticks = (current_tick > anchor->report_tick) ?
        (current_tick - anchor->report_tick) : 0u;

    /* work_rate is a *work* rate, not a stamina-drain rate - there is
       no separate reported stamina-decay-per-tick figure on the wire.
       Absent one, holding stamina_at_report flat between reports
       (rather than inventing a second implied rate) is the safer
       extrapolation: it can't drift stamina toward 0 or 100 based on a
       number that was never actually about stamina. The next report
       (change-triggered or heartbeat) still re-anchors it to the
       board's real, ground-truth value on its own cadence. */
    (void)elapsed_ticks;
    (void)decayed;
    result = anchor->stamina_at_report;

    if (result > 100u)
        result = 100u;

    return (uint8_t)result;
}

/* True once at least one MissionLinkStatus_t has been received for
   team_id - callers use this to decide between the board's reported
   (extrapolated) figures and this build's own live TeamState_t/
   g_incidents[] simulation. See write_snapshot_row() below for the
   one caller in this file today. */
static int mission_link_anchor_is_live(uint8_t team_id)
{
    if (team_id >= NUM_TEAMS)
        return 0;

    return g_mission_link_anchor[team_id].known &&
           cbba_team_is_local(team_id);
}

static FILE *g_snapshot_file = NULL;
static uint32_t g_dataset_seed = 0;
static uint32_t g_run_id = 0;
static uint32_t g_process_epoch = 0;
static uint8_t g_process_epoch_set = 0;

void snapshot_begin_run(uint32_t seed)
{
    g_dataset_seed = seed;

    if(!g_process_epoch_set)
    {
        g_process_epoch = (uint32_t)time(NULL);
        g_process_epoch_set = 1;
    }

    g_run_id = g_process_epoch + seed;

    if(g_snapshot_file != NULL)
        return;

    g_snapshot_file = fopen("tron_snapshot_dataset.csv", "w");
    if(g_snapshot_file == NULL)
        return;

    fprintf(g_snapshot_file,
        "run_id,world_seed,cycle,team_id,loc_x,loc_y,stamina,specialization,status,"
        "assigned_incident,incident_priority,incident_req_capability,"
        "incident_loc_x,incident_loc_y,victim_count,hazard_flags,"
        "hours_worked,fitness_tier,equipment_flags,event_type,cbba_selected_team,"
        "equipment_level,support_team,fault_active_mask,"
        "twin_expected_progress,twin_actual_progress,twin_progress_error,"
        "twin_expected_stamina,twin_actual_stamina,twin_stamina_error,"
        "twin_remaining_path_cost,twin_expected_work_remaining,twin_mission_stage,"
        "twin_communication_health,twin_expected_loc_x,twin_expected_loc_y,"
        "watchdog_anomaly_detected,watchdog_anomaly_confidence,"
        "watchdog_failure_mode,watchdog_evidence_flags,watchdog_recommended_action,"
        "recovery_recommendation,recovery_reason,recovery_confidence,recovery_escalation_level,"
        "twin_expected_arrival_tick,twin_expected_operating_state,"
        "twin_assigned_task_pool,twin_assigned_task_slot,"
        "incident_discovered,team_zone_id,team_zone_trust,"
        "fault_active_mobility,fault_active_communication,fault_active_resource,"
        "fault_active_environment,fault_active_mission,"
        "fault_severity_mobility,fault_severity_communication,fault_severity_resource,"
        "fault_severity_environment,fault_severity_mission,"
        "fault_ticks_remaining_mobility,fault_ticks_remaining_communication,"
        "fault_ticks_remaining_resource,fault_ticks_remaining_environment,"
        "fault_ticks_remaining_mission,"
        "env_raw_travel_cost,"
        "team_task_pool,team_task_slot,heading_to_rest,resting_zone_id\n");
}

/*
 * Shared row writer. Incident-context fields (incident_id, priority,
 * capability, location, victims, hazards, selected_team, support_team,
 * discovered) are passed in explicitly by the caller rather than
 * derived here from live team state - see snapshot_record() and
 * snapshot_record_resolved() below for why that split exists.
 */
static void write_snapshot_row(uint32_t cycle, EventType_t event_type,
    uint8_t team_id, int incident_id, int priority, int capability,
    int ix, int iy, int victims, unsigned hazards, int selected_team,
    int support_team, int discovered)
{
    TeamState_t *team;
    MissionExecutionTwin_t *twin;
    WatchdogStatus_t watchdog;
    RecoveryStatus_t recovery;
    uint8_t zone_id;
    uint8_t zone_trust;
    uint8_t env_raw_travel_cost;
    float display_stamina;

    if(g_snapshot_file == NULL || team_id >= NUM_TEAMS)
        return;

    team = &g_teams[team_id];
    twin = &g_mission_execution_twin[team_id];
    watchdog = watchdog_get_status(team_id);
    recovery = recovery_manager_get_status(team_id);

    /* Board-hosted team with a live anchor: display what the board
       actually reported (extrapolated forward from its last report),
       not this process's own live TeamState_t.stamina - see this
       file's mission-link anchor section above for why the two must
       not race. Every other team (no board hosts it, or no report has
       arrived yet - e.g. every team in a standalone `tron` desktop
       run) falls straight through to the live simulated value, same
       as before this prompt. */
    display_stamina = mission_link_anchor_is_live(team_id) ?
        (float)mission_link_extrapolated_stamina(team_id, cycle) :
        team->stamina;

    /* Zone context is meaningful for every team regardless of what it's
     * assigned to (or not) - unlike incident_discovered above, which
     * only means anything when the team actually has an incident-pool
     * assignment. */
    zone_id = zone_id_for_location(team->loc_x, team->loc_y);
    zone_trust = search_zone_trust(zone_id, cycle);

    /* fault_predictor_architecture_v4.md Section 8 item 4: a raw, non-
     * twin-relative reading of the team's own live grid cell's travel
     * cost - environment_cell_travel_cost() reads the SAME shared
     * g_environment[][] grid inject_environment_fault() (core/
     * fault_injector.c) writes its cost bump into, at the team's
     * actual current position, not twin_remaining_path_cost's twin-
     * relative comparison (which the same doc section identifies as
     * the reason ENVIRONMENT-class faults are structurally invisible
     * to every other logged signal - the twin's own expected-cost
     * recomputation cancels the bump before it ever shows up as
     * error). This is a plain derived-for-logging value, computed
     * here the same way zone_id/zone_trust just above it are -
     * nothing in TeamState_t/MissionExecutionTwin_t changes size or
     * layout. */
    env_raw_travel_cost = environment_cell_travel_cost(team->loc_x, team->loc_y);

    fprintf(g_snapshot_file,
        "%u,%u,%u,%u,%d,%d,%.2f,%u,%u,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%d,%u,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f,%u,%u,%u,%u,%d,%d,%u,%.4f,%u,%u,%u,%u,%u,%.4f,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        g_run_id,
        g_dataset_seed,
        cycle,
        team->team_id,
        team->loc_x,
        team->loc_y,
        display_stamina,
        team->specialization,
        team->status,
        incident_id,
        priority,
        capability,
        ix,
        iy,
        victims,
        hazards,
        team->hours_worked,
        team->fitness_tier,
        team->equipment_flags,
        event_type,
        selected_team,
        team->equipment_level,
        support_team,
        team->fault_state.active_mask,
        twin->expected_progress,
        twin->actual_progress,
        twin->progress_error,
        twin->expected_stamina,
        twin->actual_stamina,
        twin->stamina_error,
        twin->remaining_path_cost,
        twin->expected_work_remaining,
        twin->mission_stage,
        twin->communication_health,
        twin->expected_loc_x,
        twin->expected_loc_y,
        watchdog.anomaly_detected,
        watchdog.anomaly_confidence,
        watchdog.dominant_failure_mode,
        watchdog.evidence_flags,
        watchdog.recommended_action,
        recovery.recommendation,
        recovery.reason,
        recovery.confidence,
        recovery.escalation_level,
        twin->expected_arrival_tick,
        twin->expected_operating_state,
        twin->assigned_task_pool,
        twin->assigned_task_slot,
        discovered,
        zone_id,
        zone_trust,
        team->fault_state.active[FAULT_CLASS_MOBILITY],
        team->fault_state.active[FAULT_CLASS_COMMUNICATION],
        team->fault_state.active[FAULT_CLASS_RESOURCE],
        team->fault_state.active[FAULT_CLASS_ENVIRONMENT],
        team->fault_state.active[FAULT_CLASS_MISSION],
        team->fault_state.severity[FAULT_CLASS_MOBILITY],
        team->fault_state.severity[FAULT_CLASS_COMMUNICATION],
        team->fault_state.severity[FAULT_CLASS_RESOURCE],
        team->fault_state.severity[FAULT_CLASS_ENVIRONMENT],
        team->fault_state.severity[FAULT_CLASS_MISSION],
        /* recovery_tick > cycle guard: active[] stays 1 through the
         * whole repair-confirmation window in fault_injector.c (past
         * recovery_tick, before repair_confirm_tick - see that file's
         * own comment on the two-stage repair process), so cycle can
         * be >= recovery_tick while still active. Both are uint32_t;
         * subtracting unguarded would underflow to ~4 billion instead
         * of reading 0 for the whole confirmation window. */
        (team->fault_state.active[FAULT_CLASS_MOBILITY] &&
         team->fault_state.recovery_tick[FAULT_CLASS_MOBILITY] > cycle
            ? team->fault_state.recovery_tick[FAULT_CLASS_MOBILITY] - cycle : 0u),
        (team->fault_state.active[FAULT_CLASS_COMMUNICATION] &&
         team->fault_state.recovery_tick[FAULT_CLASS_COMMUNICATION] > cycle
            ? team->fault_state.recovery_tick[FAULT_CLASS_COMMUNICATION] - cycle : 0u),
        (team->fault_state.active[FAULT_CLASS_RESOURCE] &&
         team->fault_state.recovery_tick[FAULT_CLASS_RESOURCE] > cycle
            ? team->fault_state.recovery_tick[FAULT_CLASS_RESOURCE] - cycle : 0u),
        (team->fault_state.active[FAULT_CLASS_ENVIRONMENT] &&
         team->fault_state.recovery_tick[FAULT_CLASS_ENVIRONMENT] > cycle
            ? team->fault_state.recovery_tick[FAULT_CLASS_ENVIRONMENT] - cycle : 0u),
        (team->fault_state.active[FAULT_CLASS_MISSION] &&
         team->fault_state.recovery_tick[FAULT_CLASS_MISSION] > cycle
            ? team->fault_state.recovery_tick[FAULT_CLASS_MISSION] - cycle : 0u),
        env_raw_travel_cost,
        /* The team's own tasking, as watchdog.c's update_movement_stalls()
         * sees it (its leg-change reset) - evidence_channels.py mirrors
         * that from these. Observable tasking, not fault_state. */
        (unsigned)team->assigned_task_pool,
        (unsigned)team->assigned_task_slot,
        (unsigned)team->heading_to_rest,
        (unsigned)team->resting_zone_id);
}

void snapshot_record(uint32_t cycle, EventType_t event_type, uint8_t team_id)
{
    TeamState_t *team;
    int incident_id = -1;
    int priority = -1;
    int capability = -1;
    int ix = -1;
    int iy = -1;
    int victims = -1;
    unsigned hazards = 0;
    int selected_team = NO_ASSIGNMENT;
    int support_team = -1;
    int discovered = -1;

    if(team_id >= NUM_TEAMS)
        return;

    team = &g_teams[team_id];

    /*
     * Derives incident context from the team's *current live* state.
     * Correct for every call site except one: complete_task()'s
     * EVENT_INCIDENT_RESOLVED log. By the time that fires, the
     * completing team can already have been dispatched to a new task
     * earlier in the same tick (allocate_all_incidents() runs before
     * simulation_step()'s completion detection) - so a live read at
     * that specific point captures the team's *next* assignment, not
     * the one that just finished. See snapshot_record_resolved() for
     * that case; every other event type (assignment, stage-complete,
     * rest, fault, etc.) logs at a point where the team's live state
     * genuinely does reflect the event being logged, so this is fine
     * for those.
     */
    if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
       team->assigned_task_slot != NO_ASSIGNMENT)
    {
        IncidentState_t *inc = &g_incidents[team->assigned_task_slot];
        incident_id = inc->incident_id;
        priority = inc->priority;
        capability = inc->req_capability;
        ix = inc->loc_x;
        iy = inc->loc_y;
        victims = inc->victim_count;
        hazards = inc->hazard_flags;
        selected_team = inc->assigned_team;
        support_team = (inc->support_team == NO_ASSIGNMENT) ? -1 : (int)inc->support_team;
        discovered = inc->discovered;
    }

    write_snapshot_row(cycle, event_type, team_id, incident_id, priority,
        capability, ix, iy, victims, hazards, selected_team, support_team,
        discovered);
}

/*
 * Same row format as snapshot_record(), but for logging a task's own
 * resolution (complete_task() in incident.c): incident context comes
 * directly from the task that just resolved, not from a live lookup
 * on the team. task->assigned_team is still the team_id the row gets
 * logged under - the fix is only in *where the incident-detail fields
 * come from*, not which team the row belongs to.
 */
void snapshot_record_resolved(uint32_t cycle, TaskPool_t pool, const IncidentState_t *task)
{
    int incident_id = -1;
    int priority = -1;
    int capability = -1;
    int ix = -1;
    int iy = -1;
    int victims = -1;
    unsigned hazards = 0;
    int selected_team = NO_ASSIGNMENT;
    int support_team = -1;
    int discovered = -1;

    if(task == NULL || task->assigned_team >= NUM_TEAMS)
        return;

    if(pool == TASK_POOL_INCIDENT)
    {
        incident_id = task->incident_id;
        priority = task->priority;
        capability = task->req_capability;
        ix = task->loc_x;
        iy = task->loc_y;
        victims = task->victim_count;
        hazards = task->hazard_flags;
        selected_team = task->assigned_team;
        support_team = (task->support_team == NO_ASSIGNMENT) ? -1 : (int)task->support_team;
        discovered = task->discovered;
    }

    write_snapshot_row(cycle, EVENT_INCIDENT_RESOLVED, task->assigned_team,
        incident_id, priority, capability, ix, iy, victims, hazards,
        selected_team, support_team, discovered);
}

void snapshot_end_run(void)
{
}

void snapshot_close_dataset(void)
{
    if(g_snapshot_file != NULL)
    {
        fclose(g_snapshot_file);
        g_snapshot_file = NULL;
    }
}
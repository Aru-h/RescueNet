#include "snapshot.h"

/*
 * Every float column in this file is written with %.9g (round-trips a
 * 32-bit float exactly), not a fixed decimal count - a feature-pack
 * C-vs-Python parity test (laptop/tools/feature_pack_parity_test.py)
 * found that the previous %.2f/%.3f/%.4f/%.6f choices introduced real,
 * if small, rounding noise once data_prep.py read the text back and
 * scaled it, on stamina, the twin_* progress/stamina fields and
 * evidence_timeout specifically. This changes every already-collected
 * dataset's exact byte values (though not what they mean) - any dataset
 * generated before this change needs regenerating to compare cleanly
 * against one generated after it.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "team.h"
#include "incident.h"
#include "mission_state.h"
#include "recovery_manager.h"
#include "watchdog.h"
#include "search_zones.h"
#include "mission_link.h"
#include "environment.h" /* environment_cell_travel_cost() - env_raw_travel_cost column below */
#include "consensus.h"
#include "fault_config.h"
#include "fault_injector.h"
#include "causal_actions.h" /* g_fault_config.profile - composite run_id, below */

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
static FILE *g_peer_file = NULL;
static uint8_t g_peer_log_enabled = 0;
static uint32_t g_dataset_seed = 0;
static uint32_t g_run_id = 0;

/* Causal-profile ground truth, one row per (world_seed, cycle, team_id).
 * Separate from tron_snapshot_dataset.csv on purpose: the main dataset's
 * schema (and everything in ml/ that reads it) is untouched, and these
 * columns can never be picked up as features. See
 * docs/CAUSAL_FAULT_MODEL.md for the column definitions. */
static FILE *g_causal_truth_file = NULL;


/*
 * Composite run_id (feature-contract Decision 5): world_seed, fault
 * profile, CONSENSUS_ENFORCE and the vbus link-loss setting, hashed into
 * a single stable uint32_t - identical in tron_snapshot_dataset.csv and
 * tron_peer_obs.csv (both read g_run_id, set once per run below), so
 * data_prep can join the two on it. Deliberately NOT derived from
 * process start time (the old g_process_epoch + seed scheme this
 * replaces): two dataset-generation processes started in the same
 * wall-clock second produced colliding run_ids for overlapping seed
 * ranges - see docs/research/feature_contract_research.md Q7. This
 * formula is a pure function of run configuration, so the same
 * (world_seed, profile, enforce, link_loss) always yields the same
 * run_id, run after run, process after process.
 */
static uint32_t compute_composite_run_id(uint32_t world_seed)
{
    char buf[64];
    uint32_t enforce;
    uint32_t link_loss_ppm;
    uint32_t hash;
    size_t i;

#if VIRTUAL_BUS_ENABLED
    {
        ConsensusRunInfo_t info = consensus_get_run_info();
        enforce = (uint32_t)info.enforce;
        link_loss_ppm = (uint32_t)info.vbus_link_loss_ppm;
    }
#else
    enforce = 0u;
    link_loss_ppm = 0u;
#endif

    snprintf(buf, sizeof(buf), "%u:%u:%u:%u",
        world_seed, (unsigned)g_fault_config.profile, enforce, link_loss_ppm);

    /* Causal runs at different stress multipliers are different
     * experiments and must not share a run_id. Other profiles keep the
     * exact string above. */
    if(g_fault_config.profile == FAULT_PROFILE_CAUSAL)
    {
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, ":%u", (unsigned)(g_fault_stress * 1000.0f + 0.5f));
    }

    /* FNV-1a, 32-bit. */
    hash = 2166136261u;
    for(i = 0; buf[i] != '\0'; i++)
    {
        hash ^= (uint8_t)buf[i];
        hash *= 16777619u;
    }
    return hash;
}

/* Exposes the composite run_id computed for the current run (decision
   5) - laptop/tools/feature_pack_dump.c needs this to tag its own
   per-tick feature dump with the same run_id the training CSV rows for
   this run carry, so a later comparison can match rows by (run_id,
   team_id, cycle) without recomputing the hash a second time. */
uint32_t snapshot_current_run_id(void)
{
    return g_run_id;
}

void snapshot_begin_run(uint32_t seed)
{
    g_dataset_seed = seed;
    g_run_id = compute_composite_run_id(seed);

#if VIRTUAL_BUS_ENABLED
    if(g_peer_log_enabled && g_peer_file == NULL)
    {
        g_peer_file = fopen("tron_peer_obs.csv", "w");
        if(g_peer_file != NULL)
            fprintf(g_peer_file,
                "run_id,cycle,observer_id,target_id,heard_this_tick,rx_age_ticks,"
                "rx_on_time_6,comm_miss_6,mobility_stall_count_6,target_loc_x,target_loc_y,"
                "plausibility_fail,heard_count_window,"
                "world_seed,rx_on_time,rx_late,rx_stale,observer_self_deaf,comm_window_n,"
                "rep_status,rep_task_pool,rep_task_slot,rep_heading_to_rest,rep_resting_zone_id,"
                "leg_changed,mob_sampled,mob_stalled,mob_window_n,"
                "verdict_mobility,lo_mobility,hi_mobility,"
                "verdict_communication,lo_communication,hi_communication,"
                "observer_graded_abstain,reported_heard_target,rep_alpha,rep_beta,"
                "gt_link_dropped,gt_loss_model\n");
    }
#endif

    if(g_fault_config.profile == FAULT_PROFILE_CAUSAL && g_causal_truth_file == NULL)
    {
        g_causal_truth_file = fopen("tron_causal_truth.csv", "w");
        if(g_causal_truth_file != NULL)
            fprintf(g_causal_truth_file,
                "run_id,world_seed,cycle,team_id,fault_stress,hidden_wear,link_margin_db,"
                "hazard_mobility,hazard_resource,ticks_since_rest,aftershock_hit,"
                "onset_cause_mobility,onset_cause_communication,onset_cause_resource,"
                "onset_cause_environment,onset_cause_mission,"
                "clear_cause_mobility,clear_cause_communication,clear_cause_resource,"
                "clear_cause_environment,clear_cause_mission,action_mask,route_touch,env_hold%s\n",
                fault_profile_causal_persist()
                    ? ",field_state,escalated_mobility,escalated_resource,risk_mobility_h,risk_resource_h" : "");
    }

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
        "team_task_pool,team_task_slot,heading_to_rest,resting_zone_id,"
        "incident_floor,incident_building_id,"
        "consensus_enforce,vbus_link_loss_ppm,consensus_round,"
        "consensus_tier_mobility,consensus_tier_communication,"
        "consensus_x_mobility,consensus_x_communication,"
        "consensus_y_mobility,consensus_y_communication,"
        "peer_fault_votes_mobility,peer_fault_votes_communication,"
        "peer_clear_votes_mobility,peer_clear_votes_communication,"
        "peer_abstain_votes_mobility,peer_abstain_votes_communication,"
        "peer_live_voters,peer_dark_voters,"
        "consensus_fast_path_communication,peer_reporters,peer_heard_target,"
        "beacon_emitted,consensus_capped,"
        "evidence_progress,evidence_stamina,evidence_communication,"
        "evidence_operating_state,evidence_path,evidence_assignment,"
        "evidence_timeout,confirmed_mobility_impairment,"
        "eval_progress_reason,eval_progress_confidence,"
        "eval_stamina_reason,eval_stamina_confidence,"
        "eval_mobility_reason,eval_mobility_confidence,"
        "eval_communication_reason,eval_communication_confidence,"
        "eval_equipment_reason,eval_equipment_confidence,"
        "eval_fault_predictor_reason,eval_fault_predictor_confidence,"
        "eval_peer_consensus_reason,eval_peer_consensus_confidence\n");
}

/*
 * Appended consensus columns (docs/architecture/CONSENSUS_DESIGN.md 5.1). The header is the
 * same in every build: a build without the virtual bus writes -1 for the
 * tiers and 0 elsewhere, so data_prep needs no per-build branches. Describes
 * the team as a TARGET, from the latest published result computed at that
 * team's own node. Terminates the row.
 */
static void write_consensus_columns(uint8_t team_id)
{
#if VIRTUAL_BUS_ENABLED
    ConsensusResult_t mob = consensus_get_result(team_id, CONSENSUS_CLASS_MOBILITY);
    ConsensusResult_t comm = consensus_get_result(team_id, CONSENSUS_CLASS_COMMUNICATION);
    ConsensusRunInfo_t info = consensus_get_run_info();

    fprintf(g_snapshot_file,
        ",%u,%u,%u,%d,%d,%u,%u,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
        info.enforce, info.vbus_link_loss_ppm, (unsigned)mob.round,
        (int)mob.tier, (int)comm.tier,
        mob.x, comm.x,
        mob.y, comm.y,
        mob.n_fault, comm.n_fault,
        mob.n_clear, comm.n_clear,
        mob.n_abstain, comm.n_abstain,
        mob.n_live, mob.n_dark,
        comm.fast_path, comm.n_reporters, comm.n_heard_target,
        (unsigned)team_radio_emitted(&g_teams[team_id]),
        (unsigned)g_consensus_capped_now[team_id]);
#else
    (void)team_id;
    fprintf(g_snapshot_file,
        ",0,0,0,%d,%d,0,0,0.000,0.000,0,0,0,0,0,0,0,0,0,0,0,0,0",
        CONSENSUS_TIER_NOT_BUILT, CONSENSUS_TIER_NOT_BUILT);
#endif
}

/*
 * C watchdog evidence columns (feature-contract Decision 2): the 7
 * accumulators watchdog_get_evidence_vector() already computes for the
 * recovery path, plus confirmed_mobility_impairment, emitted directly
 * from the same watchdog state the board itself sees - not recomputed
 * from the CSV in Python (evidence_channels.py's reconstruction has
 * ~99.98% agreement with this, per docs/research/feature_contract_
 * research.md Q2; emitting the real values removes that residual and
 * the row-gap forward-fill it otherwise needs). Order matches
 * WatchdogEvidenceSlot_t exactly (progress, stamina, communication,
 * operating_state, path, assignment, timeout), which is also
 * evidence_channels.py's WEIGHTS dict order. Terminates the row.
 */
static void write_evidence_columns(uint8_t team_id)
{
    float evidence[NUM_WATCHDOG_EVIDENCE_SLOTS];
    uint8_t confirmed_mobility_impairment = 0u;

    watchdog_get_evidence_vector(team_id, evidence, &confirmed_mobility_impairment);

    fprintf(g_snapshot_file, ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u",
        evidence[WATCHDOG_EVIDENCE_SLOT_PROGRESS],
        evidence[WATCHDOG_EVIDENCE_SLOT_STAMINA],
        evidence[WATCHDOG_EVIDENCE_SLOT_COMMUNICATION],
        evidence[WATCHDOG_EVIDENCE_SLOT_OPERATING_STATE],
        evidence[WATCHDOG_EVIDENCE_SLOT_PATH],
        evidence[WATCHDOG_EVIDENCE_SLOT_ASSIGNMENT],
        evidence[WATCHDOG_EVIDENCE_SLOT_TIMEOUT],
        (unsigned)confirmed_mobility_impairment);
}

/*
 * Per-evaluator verdict columns (evaluation harness support - docs/
 * evaluation/README.md, recovery_manager.h's recovery_manager_get_
 * evaluator_verdict() doc). Pure logging: reads recovery_manager_step()'s
 * already-computed per-evaluator signals for this tick; writing this row
 * has no effect on recovery_manager_step()'s or recovery_dispatch()'s own
 * decisions. Same header in every build (RECOVERY_MANAGER_EVAL_SLOT_COUNT
 * is fixed regardless of CONSENSUS_ENFORCE) - a build without consensus
 * reads back {reason=0, confidence=-1} for eval_peer_consensus_*, the
 * same "not built in this binary" convention consensus_tier_* already
 * uses. Terminates the row.
 */
static void write_evaluator_verdict_columns(uint8_t team_id)
{
    uint8_t slot;

    for(slot = 0; slot < RECOVERY_MANAGER_EVAL_SLOT_COUNT; slot++)
    {
        RecoveryEvaluatorVerdict_t verdict = recovery_manager_get_evaluator_verdict(team_id, slot);

        fprintf(g_snapshot_file, ",%u,%.9g", (unsigned)verdict.reason, verdict.confidence);
    }

    fprintf(g_snapshot_file, "\n");
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
    int support_team, int discovered, int floor, int building_id)
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

    /* Causal profile, label fix: the environment class is labelled by "this
     * team's route touches a damaged cell", not by the team holding a fault
     * for the fault's lifetime (docs/CAUSAL_PLUMBING_FIXES.md). Every other
     * profile reads the fault state unchanged. */
    uint8_t env_label_active = team->fault_state.active[FAULT_CLASS_ENVIRONMENT];
    uint8_t env_label_severity = team->fault_state.severity[FAULT_CLASS_ENVIRONMENT];
    uint32_t env_label_remaining =
        (team->fault_state.active[FAULT_CLASS_ENVIRONMENT] &&
         team->fault_state.recovery_tick[FAULT_CLASS_ENVIRONMENT] > cycle
            ? team->fault_state.recovery_tick[FAULT_CLASS_ENVIRONMENT] - cycle : 0u);
    uint32_t env_label_mask = team->fault_state.active_mask;

    if(fault_profile_causal_fix(CAUSAL_FIX_LABEL))
    {
        env_label_active = fault_injector_causal_env_label(team_id);
        if(!env_label_active)
        {
            env_label_severity = 0u;
            env_label_remaining = 0u;
        }
        env_label_mask &= ~(1u << FAULT_CLASS_ENVIRONMENT);
        if(env_label_active)
            env_label_mask |= (1u << FAULT_CLASS_ENVIRONMENT);
    }

    fprintf(g_snapshot_file,
        "%u,%u,%u,%u,%d,%d,%.9g,%u,%u,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u,%u,%u,%d,%d,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%d,%d,%u,%.9g,%u,%u,%u,%u,%u,%.9g,%u,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d",
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
        env_label_mask,
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
        env_label_active,
        team->fault_state.active[FAULT_CLASS_MISSION],
        team->fault_state.severity[FAULT_CLASS_MOBILITY],
        team->fault_state.severity[FAULT_CLASS_COMMUNICATION],
        team->fault_state.severity[FAULT_CLASS_RESOURCE],
        env_label_severity,
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
        env_label_remaining,
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
        (unsigned)team->resting_zone_id,
        floor,
        building_id);

    write_consensus_columns(team_id);
    write_evidence_columns(team_id);
    write_evaluator_verdict_columns(team_id);
}

void snapshot_record(uint32_t cycle, EventType_t event_type, uint8_t team_id)
{
    if(event_type == EVENT_TICK && g_causal_truth_file != NULL && team_id < NUM_TEAMS)
    {
        const CausalTeamTruth_t *t = fault_injector_causal_truth(team_id);
        fprintf(g_causal_truth_file,
            "%u,%u,%u,%u,%g,%.4f,%.3f,%.6f,%.6f,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
            g_run_id, g_dataset_seed, cycle, team_id, (double)g_fault_stress,
            (double)t->hidden_wear, (double)t->link_margin_db,
            (double)t->hazard_mobility, (double)t->hazard_resource,
            t->ticks_since_rest, t->aftershock_hit,
            t->onset_cause[0], t->onset_cause[1], t->onset_cause[2], t->onset_cause[3], t->onset_cause[4],
            t->clear_cause[0], t->clear_cause[1], t->clear_cause[2], t->clear_cause[3], t->clear_cause[4],
            causal_actions_team_mask(team_id), t->route_touch,
            g_teams[team_id].fault_state.active[FAULT_CLASS_ENVIRONMENT]);   /* env_hold: the pre-Step-3.2 flag, read at row time like the dataset's */
        if(fault_profile_causal_persist())   /* Step 3.6 columns, only with persistence (off = the Step 3.5 sidecar exactly) */
            fprintf(g_causal_truth_file, ",%u,%u,%u,%.5f,%.5f", t->field_state,
                    t->escalated[FAULT_CLASS_MOBILITY], t->escalated[FAULT_CLASS_RESOURCE],
                    (double)t->risk_mobility, (double)t->risk_resource);
        fputc('\n', g_causal_truth_file);
    }

    TeamState_t *team;
    int incident_id = -1;
    /* Feature-contract Decision 4: numeric incident fields read 0, not
     * -1, when no incident is assigned - so the model's numerical
     * feature and its "no task" state are on the same 0-anchored scale
     * on both the CSV and the board's own feature vector (core/
     * fault_predictor.c). incident_req_capability is NOT one of these -
     * it keeps -1 as its own one-hot "none" category (below), which
     * can never collide with a real capability value of 0. */
    int priority = 0;
    int capability = -1;
    int ix = -1;
    int iy = -1;
    int victims = 0;
    unsigned hazards = 0;
    int selected_team = NO_ASSIGNMENT;
    int support_team = -1;
    int discovered = -1;
    int floor = -1;
    int building_id = -1;

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
        floor = inc->floor;
        building_id = inc->building_id == INCIDENT_NO_BUILDING ? -1 : (int)inc->building_id;
    }

    write_snapshot_row(cycle, event_type, team_id, incident_id, priority,
        capability, ix, iy, victims, hazards, selected_team, support_team,
        discovered, floor, building_id);
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
    /* See snapshot_record()'s identical comment on Decision 4. */
    int priority = 0;
    int capability = -1;
    int ix = -1;
    int iy = -1;
    int victims = 0;
    unsigned hazards = 0;
    int selected_team = NO_ASSIGNMENT;
    int support_team = -1;
    int discovered = -1;
    int floor = -1;
    int building_id = -1;

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
        floor = task->floor;
        building_id = task->building_id == INCIDENT_NO_BUILDING ? -1 : (int)task->building_id;
    }

    write_snapshot_row(cycle, EVENT_INCIDENT_RESOLVED, task->assigned_team,
        incident_id, priority, capability, ix, iy, victims, hazards,
        selected_team, support_team, discovered, floor, building_id);
}

void snapshot_peer_log_enable(uint8_t enable)
{
    g_peer_log_enabled = enable;
}

/*
 * Training set for a future peer-judge model (docs/architecture/CONSENSUS_DESIGN.md 5.2): one
 * row per (tick, observer, target), observer != target. Labels come from
 * joining the main CSV on (run_id, cycle, target_id). Columns prefixed gt_
 * are label-only, like fault_*; data_prep must never feed them to a model.
 * Every other column is something the observer actually had.
 */
void snapshot_record_peer_obs(uint32_t cycle)
{
#if VIRTUAL_BUS_ENABLED
    if(g_peer_file == NULL)
        return;

    for(uint8_t o = 0; o < NUM_TEAMS; o++)
    {
        for(uint8_t t = 0; t < NUM_TEAMS; t++)
        {
            PeerObservation_t p;

            if(o == t)
                continue;
            consensus_get_peer_observation(o, t, &p);
            fprintf(g_peer_file,
                "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,"
                "%u,%.9g,%.9g,%u,%.9g,%.9g,%u,%u,%u,%u,%u,%u\n",
                g_run_id, cycle, o, t,
                (unsigned)(p.rx_on_time || p.rx_late), p.rx_age_ticks,
                (unsigned)(p.comm_window_n - p.comm_misses), p.comm_misses, p.mob_stalls,
                p.rep_loc_x, p.rep_loc_y, p.plaus_fail, p.observer_heard_count,
                g_dataset_seed, p.rx_on_time, p.rx_late, p.rx_stale,
                p.observer_self_deaf, p.comm_window_n,
                p.rep_status, p.rep_task_pool, p.rep_task_slot,
                p.rep_heading_to_rest, p.rep_resting_zone_id,
                p.leg_changed, p.mob_sampled, p.mob_stalled, p.mob_window_n,
                p.vote[CONSENSUS_CLASS_MOBILITY].verdict,
                p.lo[CONSENSUS_CLASS_MOBILITY], p.hi[CONSENSUS_CLASS_MOBILITY],
                p.vote[CONSENSUS_CLASS_COMMUNICATION].verdict,
                p.lo[CONSENSUS_CLASS_COMMUNICATION], p.hi[CONSENSUS_CLASS_COMMUNICATION],
                p.observer_graded_abstain, p.reported_heard_target,
                p.rep_alpha, p.rep_beta, p.gt_link_dropped, p.gt_loss_model);
        }
    }
#else
    (void)cycle;
#endif
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
    if(g_peer_file != NULL)
    {
        fclose(g_peer_file);
        g_peer_file = NULL;
    }
    if(g_causal_truth_file != NULL)
    {
        fclose(g_causal_truth_file);
        g_causal_truth_file = NULL;
    }
}
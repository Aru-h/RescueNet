#include "feature_pack.h"

#include <string.h>

#include "mission_state.h"
#include "watchdog.h"
#include "search_zones.h"
#include "environment.h"
#include "fault_predictor_features.h"

/*
 * "No incident" convention: TASK_POOL_INCIDENT only, matching laptop/
 * snapshot.c's snapshot_record() (which is what data_prep.py's incident_
 * priority/victim_count/incident_req_capability/hazard flags/has_support
 * columns are trained against) and core/fault_predictor_features.c's own
 * identical check. A TASK_POOL_SYSTEM assignment (search sweep, evacuee
 * run, equipment cache) counts as "no incident" here, deliberately - NOT
 * the broader "either pool" convention core/recovery_manager.c's
 * recovery_manager_build_predictor_evidence() uses for the older 12-
 * feature vector (core/fault_buffer.h) - the two evidence shapes are
 * unrelated contracts trained/used differently; matching the wrong one
 * would be a real train/serve skew, not a stylistic choice.
 */
static const IncidentState_t *assigned_incident_or_null(const TeamState_t *team)
{
    if(team->assigned_task_pool == TASK_POOL_INCIDENT &&
       team->assigned_task_slot != NO_ASSIGNMENT &&
       team->assigned_task_slot < NUM_INCIDENTS)
    {
        return &g_incidents[team->assigned_task_slot];
    }
    return NULL;
}

static void set_one_hot(float *raw, int base, int count, int min_value, int value)
{
    int offset = value - min_value;

    if(offset >= 0 && offset < count)
        raw[base + offset] = 1.0f;
}

void feature_pack_build_vector(uint8_t team_id, float out_scaled[FEATURE_SPEC_NUM_INPUTS])
{
    float raw[FEATURE_SPEC_NUM_INPUTS];
    const TeamState_t *team;
    const MissionExecutionTwin_t *twin;
    const IncidentState_t *task;
    float evidence[NUM_WATCHDOG_EVIDENCE_SLOTS];
    uint8_t confirmed_mobility_impairment = 0u;
    uint8_t zone_id;
    uint8_t zone_trust;
    uint8_t env_travel_cost;
    uint8_t hazard_flags_val;
    int req_capability_val;
    int has_support;
    int i;

    memset(raw, 0, sizeof(raw));

    if(team_id >= NUM_TEAMS)
    {
        memset(out_scaled, 0, FEATURE_SPEC_NUM_INPUTS * sizeof(float));
        return;
    }

    team = &g_teams[team_id];
    twin = &g_mission_execution_twin[team_id];
    task = assigned_incident_or_null(team);

    watchdog_get_evidence_vector(team_id, evidence, &confirmed_mobility_impairment);

    zone_id = zone_id_for_location(team->loc_x, team->loc_y);
    zone_trust = search_zone_trust(zone_id, g_current_tick);
    env_travel_cost = environment_cell_travel_cost(team->loc_x, team->loc_y);

    hazard_flags_val = (task != NULL) ? task->hazard_flags : 0u;
    req_capability_val = (task != NULL) ? (int)task->req_capability : -1;
    has_support = (task != NULL && task->support_team != NO_ASSIGNMENT) ? 1 : 0;

    /* -- numeric block, indices [0, FEATURE_SPEC_NUMERIC_COUNT) ------ */
    raw[FEATURE_SPEC_IDX_INCIDENT_PRIORITY] = (task != NULL) ? (float)task->priority : 0.0f;
    raw[FEATURE_SPEC_IDX_VICTIM_COUNT] = (task != NULL) ? (float)task->victim_count : 0.0f;
    raw[FEATURE_SPEC_IDX_HOURS_WORKED] = (float)team->hours_worked;
    raw[FEATURE_SPEC_IDX_STAMINA] = team->stamina;
    raw[FEATURE_SPEC_IDX_TWIN_EXPECTED_PROGRESS] = twin->expected_progress;
    raw[FEATURE_SPEC_IDX_TWIN_ACTUAL_PROGRESS] = twin->actual_progress;
    raw[FEATURE_SPEC_IDX_TWIN_PROGRESS_ERROR] = twin->progress_error;
    raw[FEATURE_SPEC_IDX_TWIN_EXPECTED_STAMINA] = twin->expected_stamina;
    raw[FEATURE_SPEC_IDX_TWIN_ACTUAL_STAMINA] = twin->actual_stamina;
    raw[FEATURE_SPEC_IDX_TWIN_STAMINA_ERROR] = twin->stamina_error;
    raw[FEATURE_SPEC_IDX_TWIN_REMAINING_PATH_COST] =
        (twin->remaining_path_cost == 65535u)
            ? FEATURE_SPEC_PATH_COST_SENTINEL_REPLACEMENT
            : (float)twin->remaining_path_cost;
    raw[FEATURE_SPEC_IDX_TWIN_EXPECTED_WORK_REMAINING] = (float)twin->expected_work_remaining;
    raw[FEATURE_SPEC_IDX_TWIN_MISSION_STAGE] = (float)twin->mission_stage;
    raw[FEATURE_SPEC_IDX_TWIN_COMMUNICATION_HEALTH] = (float)twin->communication_health;
    raw[FEATURE_SPEC_IDX_EQUIPMENT_LEVEL] = (float)team->equipment_level;
    raw[FEATURE_SPEC_IDX_TEAM_ZONE_TRUST] = (float)zone_trust;
    raw[FEATURE_SPEC_IDX_ENV_RAW_TRAVEL_COST] = (float)env_travel_cost;
    raw[FEATURE_SPEC_IDX_STALL_FRAC_LAST_10] = fault_predictor_stall_frac(team_id, 10u);
    raw[FEATURE_SPEC_IDX_STALL_FRAC_LAST_5] = fault_predictor_stall_frac(team_id, 5u);
    raw[FEATURE_SPEC_IDX_DELTA_PRIORITY] = fault_predictor_delta_priority(team_id);
    raw[FEATURE_SPEC_IDX_DELTA_VICTIM_COUNT] = fault_predictor_delta_victim_count(team_id);

    /* -- fixed one-hot blocks ----------------------------------------- */
    set_one_hot(raw, FEATURE_SPEC_SPECIALIZATION_BASE, FEATURE_SPEC_SPECIALIZATION_COUNT,
                FEATURE_SPEC_SPECIALIZATION_MIN, (int)team->specialization);
    set_one_hot(raw, FEATURE_SPEC_TWIN_EXPECTED_OPERATING_STATE_BASE,
                FEATURE_SPEC_TWIN_EXPECTED_OPERATING_STATE_COUNT,
                FEATURE_SPEC_TWIN_EXPECTED_OPERATING_STATE_MIN, (int)twin->expected_operating_state);
    set_one_hot(raw, FEATURE_SPEC_STATUS_BASE, FEATURE_SPEC_STATUS_COUNT,
                FEATURE_SPEC_STATUS_MIN, (int)team->status);
    set_one_hot(raw, FEATURE_SPEC_FITNESS_TIER_BASE, FEATURE_SPEC_FITNESS_TIER_COUNT,
                FEATURE_SPEC_FITNESS_TIER_MIN, (int)team->fitness_tier);
    set_one_hot(raw, FEATURE_SPEC_INCIDENT_REQ_CAPABILITY_BASE,
                FEATURE_SPEC_INCIDENT_REQ_CAPABILITY_COUNT,
                FEATURE_SPEC_INCIDENT_REQ_CAPABILITY_MIN, req_capability_val);
    set_one_hot(raw, FEATURE_SPEC_TEAM_ZONE_ID_BASE, FEATURE_SPEC_TEAM_ZONE_ID_COUNT,
                FEATURE_SPEC_TEAM_ZONE_ID_MIN, (int)zone_id);

    raw[FEATURE_SPEC_IDX_HAS_SUPPORT] = (float)has_support;

    /* -- equipment/hazard bitmasks - bit order matches data_prep.py's
     * equipment_names/hazard_names lists exactly (confirmed against
     * core/mission_state.h's EQ_* and HAZ_* defines; bit 3 has no EQ_BOAT
     * constant defined at all today, so equipment_boat is always 0 -
     * that's the real, current behaviour this mirrors, not a bug to fix
     * here). ---------------------------------------------------------- */
    raw[FEATURE_SPEC_IDX_EQUIPMENT_ROPE] = (team->equipment_flags & (1u << 0)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_EQUIPMENT_SPREADER] = (team->equipment_flags & (1u << 1)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_EQUIPMENT_MEDICAL_KIT] = (team->equipment_flags & (1u << 2)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_EQUIPMENT_BOAT] = (team->equipment_flags & (1u << 3)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_EQUIPMENT_K9] = (team->equipment_flags & (1u << 4)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_EQUIPMENT_HAZMAT_SUIT] = (team->equipment_flags & (1u << 5)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_EQUIPMENT_STRUCTURAL] = (team->equipment_flags & (1u << 6)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_EQUIPMENT_GENERATOR] = (team->equipment_flags & (1u << 7)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_HAZARD_GAS] = (hazard_flags_val & (1u << 0)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_HAZARD_FLOOD] = (hazard_flags_val & (1u << 1)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_HAZARD_STRUCTURAL] = (hazard_flags_val & (1u << 2)) ? 1.0f : 0.0f;
    raw[FEATURE_SPEC_IDX_HAZARD_CBRN] = (hazard_flags_val & (1u << 3)) ? 1.0f : 0.0f;

    /* -- evidence: the 7 watchdog accumulators, in WatchdogEvidenceSlot_t
     * order, which is also feature_spec.h's order for this block (both
     * ultimately follow evidence_channels.py's WEIGHTS dict order). --- */
    for(i = 0; i < (int)NUM_WATCHDOG_EVIDENCE_SLOTS; i++)
        raw[FEATURE_SPEC_IDX_EVIDENCE_PROGRESS + i] = evidence[i];
    raw[FEATURE_SPEC_IDX_CONFIRMED_MOBILITY_IMPAIRMENT] = (float)confirmed_mobility_impairment;

    raw[FEATURE_SPEC_IDX_PATH_IS_UNREACHABLE] =
        (twin->remaining_path_cost == 65535u) ? 1.0f : 0.0f;

    /* -- scale exactly as data_prep.py's SafeStandardScaler does, over
     * every column (numeric, one-hot, bitmask and evidence alike). ---- */
    for(i = 0; i < FEATURE_SPEC_NUM_INPUTS; i++)
        out_scaled[i] = (raw[i] - FEATURE_SPEC_SCALER_MEAN[i]) / FEATURE_SPEC_SCALER_STD[i];
}

/* Pre-dispatch snapshot cache - see feature_pack.h's own doc on why this
   exists and exactly where it is filled/read. Zero-initialized, so a
   read before the first feature_pack_cache_tick() call for a team is an
   all-zero vector, not garbage. */
static float g_feature_vector_cache[NUM_TEAMS][FEATURE_SPEC_NUM_INPUTS];

void feature_pack_cache_tick(uint8_t team_id)
{
    if(team_id >= NUM_TEAMS)
        return;

    feature_pack_build_vector(team_id, g_feature_vector_cache[team_id]);
}

void feature_pack_get_cached_vector(uint8_t team_id, float out_scaled[FEATURE_SPEC_NUM_INPUTS])
{
    if(team_id >= NUM_TEAMS)
    {
        memset(out_scaled, 0, FEATURE_SPEC_NUM_INPUTS * sizeof(float));
        return;
    }

    memcpy(out_scaled, g_feature_vector_cache[team_id], FEATURE_SPEC_NUM_INPUTS * sizeof(float));
}

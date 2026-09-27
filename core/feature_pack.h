#ifndef FEATURE_PACK_H
#define FEATURE_PACK_H

#include <stdint.h>
#include "feature_spec.h"

/*
 * Fills out_scaled[FEATURE_SPEC_NUM_INPUTS] with team_id's current-tick
 * input vector, in feature_spec.h order, scaled exactly as data_prep.py
 * scales training data: (raw[i] - FEATURE_SPEC_SCALER_MEAN[i]) /
 * FEATURE_SPEC_SCALER_STD[i], for every i - one-hot/bitmask/evidence
 * columns included, since data_prep.py's SafeStandardScaler fits over the
 * full 74-column matrix, not just the numeric columns.
 *
 * Built entirely from observables available on both the laptop and a
 * board: team state (core/mission_state.h's TeamState_t), the mission
 * execution twin (MissionExecutionTwin_t), the watchdog's evidence vector
 * (watchdog_get_evidence_vector()), the team's assigned incident (g_
 * incidents[], TASK_POOL_INCIDENT only - see this file's own comment on
 * why TASK_POOL_SYSTEM tasks are "no incident" here, matching laptop/
 * snapshot.c's snapshot_record() exactly), fault_predictor_features.c's
 * stall/delta history features, and the mission-link zone/map data
 * (core/search_zones.c's zone_id_for_location()/search_zone_trust(),
 * core/environment.c's environment_cell_travel_cost()).
 *
 * NEVER reads team->fault_state - the model must not be able to see
 * ground truth. This is the same constraint core/fault_predictor_
 * features.h's own two functions are built under; nothing in this file
 * touches TeamFaultState_t at all, by construction, not by an added
 * check.
 *
 * Inference gate: this function does not itself decide whether it's safe
 * to call - core/fault_predictor.c's evaluate_fault_predictor() is where
 * that gate lives (see its own doc). Before the route map/district map
 * have arrived (mission_link_map_rows_received()/mission_link_district_
 * rows_received() < their respective row counts), env_raw_travel_cost
 * reads 1 (environment_cell_travel_cost()'s no-map default) and
 * team_zone_id reads district 0 (zone_id_for_location()'s no-data
 * default) for every team - silently wrong values, not missing ones -
 * so a caller that cares about that distinction should check those two
 * counts itself before trusting this function's output for those two
 * features specifically.
 */
void feature_pack_build_vector(uint8_t team_id, float out_scaled[FEATURE_SPEC_NUM_INPUTS]);

#endif /* FEATURE_PACK_H */

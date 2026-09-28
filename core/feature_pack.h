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

/*
 * Builds team_id's current-tick vector (feature_pack_build_vector(),
 * above) and stores it in a per-team cache, overwriting whatever this
 * tick's earlier call cached.
 *
 * Call site: core/simulation.c's simulation_step(), in the per-team loop
 * right before recovery_manager_step() is called - the same team/
 * incident/twin state laptop/snapshot.c's own per-tick row will read a
 * little later in that same tick's Pass 1 (nothing mutates that state in
 * between: update_all_incident_work()/update_all_passive_effects() and
 * this tick's allocate_all_incidents() have already run by this point,
 * and recovery_dispatch() - Pass 2, the thing that actually changes an
 * assignment/status in response to a recommendation - has not). Placed
 * before recovery_manager_step() specifically, not literally inside
 * Pass 1's own loop, because evaluate_fault_predictor() (called from
 * inside recovery_manager_step()) is this cache's first reader every
 * tick - filling it any later would make evaluate_fault_predictor() read
 * last tick's cached vector instead of this tick's.
 *
 * Why cache at all, instead of evaluate_fault_predictor() just calling
 * feature_pack_build_vector() itself (as it did before this function
 * existed): a team's assignment/status can still change later the same
 * tick, in recovery_dispatch() (Pass 2) - task abandonment, a status
 * change to RESTING, a fresh allocate_all_incidents() call for
 * reallocation. Those are the recovery system's OWN decisions about a
 * suspected fault, not the team's independent situation; letting the
 * fault predictor's input reflect them would be a back-door hint about
 * the very thing it's trying to predict, and would make it less
 * independent of the other evaluators feeding the same recommendation.
 * Caching the pre-dispatch vector keeps the model judging the team's own
 * situation, matching what the training CSV's row for this tick already
 * captures (both are Pass-1-timed, by construction, not by coincidence).
 */
void feature_pack_cache_tick(uint8_t team_id);

/*
 * Copies team_id's cached vector (feature_pack_cache_tick(), above) into
 * out_scaled. Consumers: core/recovery_manager.c's evaluate_fault_
 * predictor() (the real, on-board consumer) and laptop/tools/feature_
 * pack_dump.c (the parity-test consumer - reads the exact same pre-
 * dispatch snapshot evaluate_fault_predictor() would have used that
 * tick, rather than calling feature_pack_build_vector() itself after a
 * tick has fully finished, which would read post-dispatch state instead).
 * Reads all zero if feature_pack_cache_tick() was never called for
 * team_id (e.g. before the first tick).
 */
void feature_pack_get_cached_vector(uint8_t team_id, float out_scaled[FEATURE_SPEC_NUM_INPUTS]);

#endif /* FEATURE_PACK_H */

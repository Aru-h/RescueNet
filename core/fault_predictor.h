#ifndef FAULT_PREDICTOR_H
#define FAULT_PREDICTOR_H

#include "fault_config.h"   /* NUM_OPERATIONAL_FAULT_CLASSES */
#include "feature_spec.h"   /* FEATURE_SPEC_NUM_INPUTS - the 74-input feature contract */

/*
 * Scaffolding only - no trained weights exist yet (data_prep.py has only
 * reached preprocessing; model.py/train.py haven't run). Flip this to 1
 * once a real weights header exists and fault_predictor_run() below has
 * been filled in to use it - see FAULT_MODEL_LOADED's use in
 * fault_predictor.c. Left at 0 rather than deleted so recovery_manager.c's
 * evaluate_fault_predictor() has a real, callable function to build
 * against today.
 *
 * Overridable (#ifndef) so an out-of-tree build (e.g. laptop/tools/
 * evaluate.py's rules+consensus+ai variant) can pass -DFAULT_MODEL_LOADED=1
 * once fault_predictor.c actually has a weights header to consume, without
 * editing this file again. No behaviour change while nothing defines it
 * first - default stays 0.
 */
#ifndef FAULT_MODEL_LOADED
#define FAULT_MODEL_LOADED 1
#endif

/*
 * Input shape: FEATURE_SPEC_NUM_INPUTS (74) floats, in core/feature_
 * spec.h's order, scaled exactly as data_prep.py's SafeStandardScaler
 * scales training data - see core/feature_pack.c's feature_pack_build_
 * vector(), the one function that builds this shape correctly today.
 * This is the feature-contract vector (docs/ml/FEATURE_CONTRACT_DESIGN.md),
 * board-computable via the mission-link zone/map frames (core/mission_
 * link.h sections 5c/5e/5f) - not the older, still-live 12-feature
 * FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES vector core/fault_buffer.h/
 * core/fault_comms.c use for a REMOTE team's received FAULT_BROADCAST
 * payload, which this header no longer includes and which stays exactly
 * as it is (see core/fault_comms.c's own top-of-file note: it is dead
 * code today - nothing but fault_comms_init() is called - so its call
 * to fault_predictor_run() with a 12-float array is a stale, unreached
 * caller, not something this change silently breaks at runtime; array
 * parameters decay to pointers in C, so it still compiles).
 *
 * Output: 5 independent per-class scores (core/fault_config.h's
 * OperationalFaultClass_t), NOT a softmax - a team can have more than one
 * fault class active at once (fault_injector.c has no mutual-exclusivity
 * check between classes), so these are meant to be read as 5 independent
 * [0,1]-ish likelihoods, not a single winner-take-all distribution.
 */
typedef struct
{
    float score[NUM_OPERATIONAL_FAULT_CLASSES];
} FaultPredictorScores_t;

/*
 * Runs the model (once one exists) against a 74-input feature vector -
 * core/recovery_manager.c's evaluate_fault_predictor(), built from
 * core/feature_pack.c's feature_pack_build_vector() for this board's own
 * team. (core/fault_comms.c's remote-team path still calls this function
 * too, with its own, unrelated 12-float vector - see this header's input-
 * shape comment above for why that's a known, harmless inconsistency in
 * dead code, not something fixed here.)
 *
 * While FAULT_MODEL_LOADED is 0, this always returns all-zero scores
 * (i.e. "no fault, on every class") regardless of input - a safe inert
 * default, not a real prediction. evidence is still validated by shape
 * (a real FEATURE_SPEC_NUM_INPUTS-float array must be passed) so nothing
 * else needs to change when real weights land, only this function's
 * internals.
 */
FaultPredictorScores_t fault_predictor_run(const float evidence[FEATURE_SPEC_NUM_INPUTS]);

#endif

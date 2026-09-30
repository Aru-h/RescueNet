#ifndef FAULT_PREDICTOR_H
#define FAULT_PREDICTOR_H

#include "fault_config.h"   /* NUM_OPERATIONAL_FAULT_CLASSES */
#include "feature_spec.h"   /* FEATURE_SPEC_NUM_INPUTS - the 75-input feature contract */

/*
 * FAULT_MODEL_LOADED: 1 = fault_predictor_run() evaluates the trained
 * float32 MLP in fault_predictor_weights.h (75 -> 128 -> 64 -> 5, sigmoid
 * outputs); 0 = it returns all-zero scores and never touches the weights.
 *
 * The AI is opt-in and OFF by default: the default build, the dataset
 * generator and every board/Pico/harness target compile with 0, so the
 * simulation and the CSV are exactly what they are without a model.
 * Enable only with -DFAULT_MODEL_LOADED=1 - `make tron-ai` (./tron_ai),
 * the "+ai" row of `make evaluate`, check-model-parity, and the CubeIDE
 * DevRam configuration.
 */
#ifndef FAULT_MODEL_LOADED
#define FAULT_MODEL_LOADED 0
#endif

/*
 * Input shape: FEATURE_SPEC_NUM_INPUTS (75) floats, in core/feature_
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
 * Per-class decision thresholds, in OperationalFaultClass_t order
 * (mobility, communication, resource, environment, mission). A class is
 * "flagged" when its score is >= its threshold; FAULT_PREDICTOR_CLASS_OFF
 * (> 1.0, a sigmoid never reaches it) disables a class.
 *
 * mobility/communication/resource: validation best-F1 points
 * (docs/ml/FAULT_PREDICTOR_THRESHOLDS.md). environment and mission are OFF:
 * closed-loop testing showed they cause false recovery actions that pull
 * healthy teams off their tasks (incidents resolved fell 37 -> 23-33 even
 * at threshold 0.95), and neither has a recovery evaluator to act on.
 * Re-run laptop/tools/ai_threshold_sweep.py after any retrain.
 * Overridable for sweeps.
 */
#define FAULT_PREDICTOR_CLASS_OFF 1.01f
#ifndef FAULT_PREDICTOR_CLASS_THRESHOLDS
#define FAULT_PREDICTOR_CLASS_THRESHOLDS \
    { 0.573f, 0.965f, 0.705f, FAULT_PREDICTOR_CLASS_OFF, FAULT_PREDICTOR_CLASS_OFF }
#endif

/*
 * Runs the trained model against a 75-input feature vector -
 * core/recovery_manager.c's evaluate_fault_predictor(), built from
 * core/feature_pack.c's feature_pack_build_vector() for this board's own
 * team. (core/fault_comms.c's remote-team path still calls this function
 * too, with its own, unrelated 12-float vector - see this header's input-
 * shape comment above for why that's a known, harmless inconsistency in
 * dead code, not something fixed here.)
 *
 * While FAULT_MODEL_LOADED is 0 (the default), this always returns
 * all-zero scores (i.e. "no fault, on every class") regardless of input,
 * and does not read the weights.
 */
FaultPredictorScores_t fault_predictor_run(const float evidence[FEATURE_SPEC_NUM_INPUTS]);

#endif

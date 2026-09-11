#ifndef FAULT_PREDICTOR_H
#define FAULT_PREDICTOR_H

#include "fault_buffer.h"   /* FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES */
#include "fault_config.h"   /* NUM_OPERATIONAL_FAULT_CLASSES */

/*
 * Scaffolding only - no trained weights exist yet (data_prep.py has only
 * reached preprocessing; model.py/train.py haven't run). Flip this to 1
 * once a real weights header exists and fault_predictor_run() below has
 * been filled in to use it - see FAULT_MODEL_LOADED's use in
 * fault_predictor.c. Left at 0 rather than deleted so recovery_manager.c's
 * evaluate_fault_predictor() has a real, callable function to build
 * against today.
 */
#define FAULT_MODEL_LOADED 0

/*
 * Input shape: this is FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES (9 floats -
 * the 7 accumulated_evidence[] channels + equipment_level/100.0f +
 * confirmed_mobility_impairment), NOT the bigger "raw telemetry +
 * accumulated_evidence[7]" feature set the v4 architecture doc's Section
 * 2/3 describes for the on-device per-team evaluator. That bigger set is
 * explicitly not finalized yet (data_prep.py's own comment: still raw-
 * telemetry-only, the evidence-channel columns "currently being debugged
 * elsewhere and isn't ready to add correctly yet"). This module is scoped
 * to the one feature shape that's actually locked in code today -
 * core/fault_buffer.h's 9-feature vector - which has two things going
 * for it: it's real and stable right now, and it's the exact shape
 * core/fault_comms.c (Prompt 1) already needs to hand a received
 * FAULT_BROADCAST to for vote-casting. When the bigger feature set lands,
 * this signature will need to change - flagged here so that's not a
 * surprise, not treated as done.
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
 * Runs the model (once one exists) against a 9-feature evidence vector -
 * either this board's own team (core/recovery_manager.c's
 * evaluate_fault_predictor(), built from watchdog_get_evidence_vector()
 * + team->equipment_level/100.0f + confirmed_mobility_impairment) or a
 * remote team's received FAULT_BROADCAST payload (core/fault_comms.c).
 * Same function, same input shape, either way.
 *
 * While FAULT_MODEL_LOADED is 0, this always returns all-zero scores
 * (i.e. "no fault, on every class") regardless of input - a safe inert
 * default, not a real prediction. evidence is still validated by shape
 * (a real 9-float array must be passed) so nothing else needs to change
 * when real weights land, only this function's internals.
 */
FaultPredictorScores_t fault_predictor_run(const float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES]);

#endif

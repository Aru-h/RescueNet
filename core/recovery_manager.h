#ifndef RECOVERY_MANAGER_H
#define RECOVERY_MANAGER_H

#include <stdint.h>

typedef enum
{
    RECOVERY_ACTION_CONTINUE_MONITORING = 0,
    RECOVERY_ACTION_LOCAL_RECOVERY,
    RECOVERY_ACTION_RETURN_TO_SAFE_ZONE,
    RECOVERY_ACTION_REQUEST_TASK_REALLOCATION,
    RECOVERY_ACTION_ABORT_MISSION
} RecoveryAction_t;

typedef enum
{
    RECOVERY_REASON_NONE = 0,
    RECOVERY_REASON_PROGRESS_DEVIATION,
    RECOVERY_REASON_LOW_STAMINA,
    RECOVERY_REASON_MOBILITY_FAILURE,
    RECOVERY_REASON_COMMUNICATION_FAILURE,
    RECOVERY_REASON_EQUIPMENT_FAULT,
    RECOVERY_REASON_MISSION_DELAY,
    RECOVERY_REASON_TEAM_DEGRADED,
    RECOVERY_REASON_CONSENSUS_FAULT,
    RECOVERY_REASON_PREDICTED_FAULT
} RecoveryReason_t;

typedef struct
{
    RecoveryAction_t recommendation;
    RecoveryReason_t reason;
    float confidence;
    uint8_t escalation_level;
} RecoveryStatus_t;

enum
{
    RECOVERY_ACTION_COUNT = 5u
};

typedef struct
{
    uint32_t dispatched[RECOVERY_ACTION_COUNT];
    uint32_t resolved[RECOVERY_ACTION_COUNT];
    uint32_t resolution_ticks_sum[RECOVERY_ACTION_COUNT];
    uint32_t timed_out[RECOVERY_ACTION_COUNT];
    uint32_t recommendation_reversals;
    uint32_t evaluation_ticks;
} RecoveryMetrics_t;

/*
 * Pure hard-consecutive-tick persistence-streak step for evaluate_fault_
 * predictor()'s "sustained AI persistence" corroboration path (core/
 * recovery_manager_config.h's FAULT_PERSISTENCE_FLOOR/_TRIP_TICKS - see
 * that header for the mechanism choice, why a cumulative up/down counter
 * was tried and rejected, and what this constant is calibrated against).
 *
 * Exposed here (not static in recovery_manager.c) purely so it can be
 * unit-tested directly with synthetic confidence sequences (laptop/tools/
 * recovery_manager_persistence_selftest.c) without needing a trained
 * fault_predictor model or the rest of this module's per-team state -
 * the real call site (recovery_manager.c's predictor_persistence_streak_
 * update(), private to that file) is the only production caller.
 *
 * streak: the caller's current per-team streak value (0 on a cold start).
 * confidence: this tick's AI confidence (fault_predictor_run()'s
 * scores.score[best_class] - NOT watchdog->anomaly_confidence).
 * Returns the updated streak: incremented (saturating at UINT8_MAX) if
 * confidence >= FAULT_PERSISTENCE_FLOOR, reset to 0 otherwise - a single
 * below-floor tick fully resets progress, deliberately (see this
 * function's config header doc for why the alternative - a cumulative
 * up/down counter that forgives a bad tick - was measured and rejected).
 */
uint8_t recovery_manager_persistence_streak_step(uint8_t streak, float confidence);

/*
 * EXPERIMENTAL, NOT the production mechanism - see this function's own
 * doc in recovery_manager.c for the full measured verdict (laptop/
 * tools/persistence_bounded_forgiveness_ablation.py). Tolerates exactly
 * one isolated below-floor tick without resetting, hard-resets on two
 * consecutive misses. Closes the specific dip-every-Nth-tick exploit it
 * was aimed at, but is NOT a net improvement over recovery_manager_
 * persistence_streak_step() on real simulator data (worse false-accept
 * than the false-reject it buys) - kept here as tested, documented, and
 * deliberately not adopted, not silently discarded.
 *
 * *was_last_miss is extra caller-owned state this variant needs beyond
 * the plain streak (true if the immediately preceding tick was itself
 * a miss) - persist it across calls the same way streak itself is.
 */
uint8_t recovery_manager_persistence_streak_step_bounded_forgiveness(
    uint8_t streak, float confidence, uint8_t *was_last_miss);

/*
 * Builds the FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES-wide evidence vector
 * evaluate_fault_predictor() feeds to fault_predictor_run() for team_id:
 * watchdog_get_evidence_vector()'s 7 accumulated_evidence[] channels,
 * team->equipment_level/100.0f, confirmed_mobility_impairment, then
 * incident_priority/victim_count looked up from whichever task pool
 * team_id is currently assigned to (0.0f for both when unassigned), then
 * env_raw_travel_cost (environment_cell_travel_cost() at the team's own
 * cell).
 *
 * Exposed here (not static/inlined in evaluate_fault_predictor(), same
 * rationale as recovery_manager_persistence_streak_step() above) purely
 * so a test can call it directly and compare its output byte-for-byte
 * against what core/fault_comms.c's fault_comms_broadcast_local_teams()
 * independently assembles for the wire (audit #163: the two call sites
 * must produce identical vectors for the same team/tick, but are kept as
 * separate copies rather than sharing code - see fault_comms.c's own
 * comment on that call site). evaluate_fault_predictor() itself is the
 * only production caller.
 */
void recovery_manager_build_predictor_evidence(
    uint8_t team_id, float evidence_out[/* FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES */]);

void recovery_manager_init(void);
void recovery_manager_step(void);
void recovery_dispatch(uint8_t team_id, RecoveryAction_t action);
RecoveryAction_t recovery_manager_get_recommendation(uint8_t team_id);
RecoveryStatus_t recovery_manager_get_status(uint8_t team_id);
RecoveryMetrics_t recovery_manager_get_metrics(void);

/*
 * Per-evaluator verdicts (evaluation harness support - docs/evaluation/
 * README.md). recovery_manager_step()'s fused RecoveryStatus_t collapses
 * up to 7 evaluators (recovery_select_primary_signal() picks one) into a
 * single (reason, confidence) per team per tick - which loses information
 * a fault-detection evaluation needs: with concurrent faults, one
 * evaluator's signal can occupy the fused slot while another evaluator
 * also fired this same tick (e.g. a peer-consensus CONSENSUS_FAULT for
 * mobility winning the fused slot while evaluate_communication() also
 * flagged a real communication fault the same tick - the fused reason
 * alone would make that look like a missed communication detection).
 * These getters expose each evaluator's own, unfused (reason, confidence)
 * for the tick just computed by recovery_manager_step(), so a caller can
 * ask "did evaluator X flag anything" independently of which one won.
 *
 * Pure logging: reading these has no effect on recovery_manager_step()'s
 * or recovery_dispatch()'s own decisions - it does not change simulator
 * behaviour, only what can be observed about it.
 *
 * Slot indices are fixed (RECOVERY_MANAGER_EVAL_SLOT_COUNT total)
 * regardless of CONSENSUS_ENFORCE, so the CSV schema laptop/snapshot.c
 * writes is identical across builds: RECOVERY_MANAGER_EVAL_PEER_CONSENSUS
 * reads back {RECOVERY_REASON_NONE, -1.0f} (confidence -1, matching
 * CONSENSUS_TIER_NOT_BUILT's existing "not built in this binary"
 * convention - never a real evaluator's own output) when CONSENSUS_ENFORCE
 * is 0, since that evaluator does not exist in that build.
 */
enum
{
    RECOVERY_MANAGER_EVAL_PROGRESS = 0,
    RECOVERY_MANAGER_EVAL_STAMINA,
    RECOVERY_MANAGER_EVAL_MOBILITY,
    RECOVERY_MANAGER_EVAL_COMMUNICATION,
    RECOVERY_MANAGER_EVAL_EQUIPMENT,
    RECOVERY_MANAGER_EVAL_FAULT_PREDICTOR,
    RECOVERY_MANAGER_EVAL_PEER_CONSENSUS,
    RECOVERY_MANAGER_EVAL_SLOT_COUNT
};

typedef struct
{
    RecoveryReason_t reason;
    float confidence;
} RecoveryEvaluatorVerdict_t;

RecoveryEvaluatorVerdict_t recovery_manager_get_evaluator_verdict(
    uint8_t team_id, uint8_t evaluator_slot);

#endif

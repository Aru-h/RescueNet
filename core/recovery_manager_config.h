#ifndef RECOVERY_MANAGER_CONFIG_H
#define RECOVERY_MANAGER_CONFIG_H

#include "mission_state.h"

#include "consensus_config.h"

/* Six local evaluators, plus evaluate_peer_consensus() when
 * CONSENSUS_ENFORCE (core/consensus_config.h). */
#if CONSENSUS_ENFORCE
#define RECOVERY_MANAGER_EVALUATOR_COUNT 7u
#else
#define RECOVERY_MANAGER_EVALUATOR_COUNT 6u
#endif

#define RECOVERY_MANAGER_PROGRESS_ERROR_SCALE 0.35f
#define RECOVERY_MANAGER_STAMINA_ERROR_SCALE 18.0f
#define RECOVERY_MANAGER_MOBILITY_ERROR_SCALE 1.00f
#define RECOVERY_MANAGER_COMMUNICATION_ERROR_SCALE 3.00f
#define RECOVERY_MANAGER_EQUIPMENT_ERROR_SCALE 20.0f

#define RECOVERY_MANAGER_STAGE_TRAVEL_PROGRESS_MULT 0.80f
#define RECOVERY_MANAGER_STAGE_WORK_PROGRESS_MULT 0.70f
#define RECOVERY_MANAGER_STAGE_RECOVERY_PROGRESS_MULT 1.20f
#define RECOVERY_MANAGER_STAGE_IDLE_PROGRESS_MULT 1.30f

#define RECOVERY_MANAGER_STAGE_TRAVEL_STAMINA_MULT 0.90f
#define RECOVERY_MANAGER_STAGE_WORK_STAMINA_MULT 1.00f
#define RECOVERY_MANAGER_STAGE_RECOVERY_STAMINA_MULT 0.75f
#define RECOVERY_MANAGER_STAGE_IDLE_STAMINA_MULT 1.10f

#define RECOVERY_MANAGER_STAGE_TRAVEL_MOBILITY_MULT 0.85f
#define RECOVERY_MANAGER_STAGE_WORK_MOBILITY_MULT 1.00f
#define RECOVERY_MANAGER_STAGE_RECOVERY_MOBILITY_MULT 1.10f
#define RECOVERY_MANAGER_STAGE_IDLE_MOBILITY_MULT 1.15f

#define RECOVERY_MANAGER_STAGE_TRAVEL_COMMUNICATION_MULT 0.85f
#define RECOVERY_MANAGER_STAGE_WORK_COMMUNICATION_MULT 0.95f
#define RECOVERY_MANAGER_STAGE_RECOVERY_COMMUNICATION_MULT 1.05f
#define RECOVERY_MANAGER_STAGE_IDLE_COMMUNICATION_MULT 1.10f

#define RECOVERY_MANAGER_STAGE_TRAVEL_EQUIPMENT_MULT 0.95f
#define RECOVERY_MANAGER_STAGE_WORK_EQUIPMENT_MULT 0.85f
#define RECOVERY_MANAGER_STAGE_RECOVERY_EQUIPMENT_MULT 1.05f
#define RECOVERY_MANAGER_STAGE_IDLE_EQUIPMENT_MULT 1.10f

#define RECOVERY_MANAGER_STAGE_BASE_PROGRESS_FLOOR 0.18f
#define RECOVERY_MANAGER_STAGE_BASE_STAMINA_FLOOR 0.18f
#define RECOVERY_MANAGER_STAGE_BASE_MOBILITY_FLOOR 0.20f
#define RECOVERY_MANAGER_STAGE_BASE_COMMUNICATION_FLOOR 0.18f
#define RECOVERY_MANAGER_STAGE_BASE_EQUIPMENT_FLOOR 0.16f

#define RECOVERY_MANAGER_ACTION_FLOOR_LOCAL 0.24f
#define RECOVERY_MANAGER_ACTION_FLOOR_RETURN 0.36f
#define RECOVERY_MANAGER_ACTION_FLOOR_REALLOC 0.48f
#define RECOVERY_MANAGER_ACTION_FLOOR_ABORT 0.72f

#define RECOVERY_MANAGER_ACTION_CONFIDENCE_PRIMARY_WEIGHT 0.70f
#define RECOVERY_MANAGER_ACTION_CONFIDENCE_SUPPORT_WEIGHT 0.30f
#define RECOVERY_MANAGER_ACTION_CONFIDENCE_DECAY 0.65f

#define RECOVERY_MANAGER_STAGE_TRAVEL_LOCAL_PERSISTENCE 1u
#define RECOVERY_MANAGER_STAGE_WORK_LOCAL_PERSISTENCE 1u
#define RECOVERY_MANAGER_STAGE_RECOVERY_LOCAL_PERSISTENCE 1u
#define RECOVERY_MANAGER_STAGE_IDLE_LOCAL_PERSISTENCE 1u

#define RECOVERY_MANAGER_STAGE_TRAVEL_RETURN_PERSISTENCE 2u
#define RECOVERY_MANAGER_STAGE_WORK_RETURN_PERSISTENCE 2u
#define RECOVERY_MANAGER_STAGE_RECOVERY_RETURN_PERSISTENCE 1u
#define RECOVERY_MANAGER_STAGE_IDLE_RETURN_PERSISTENCE 1u

#define RECOVERY_MANAGER_STAGE_TRAVEL_REALLOC_PERSISTENCE 3u
#define RECOVERY_MANAGER_STAGE_WORK_REALLOC_PERSISTENCE 2u
#define RECOVERY_MANAGER_STAGE_RECOVERY_REALLOC_PERSISTENCE 2u
#define RECOVERY_MANAGER_STAGE_IDLE_REALLOC_PERSISTENCE 2u

#define RECOVERY_MANAGER_STAGE_TRAVEL_ABORT_PERSISTENCE 4u
#define RECOVERY_MANAGER_STAGE_WORK_ABORT_PERSISTENCE 4u
#define RECOVERY_MANAGER_STAGE_RECOVERY_ABORT_PERSISTENCE 3u
#define RECOVERY_MANAGER_STAGE_IDLE_ABORT_PERSISTENCE 3u

#define RECOVERY_MANAGER_STAGE_TRAVEL_LOCAL_COOLDOWN 0u
#define RECOVERY_MANAGER_STAGE_WORK_LOCAL_COOLDOWN 0u
#define RECOVERY_MANAGER_STAGE_RECOVERY_LOCAL_COOLDOWN 0u
#define RECOVERY_MANAGER_STAGE_IDLE_LOCAL_COOLDOWN 0u

#define RECOVERY_MANAGER_STAGE_TRAVEL_RETURN_COOLDOWN 1u
#define RECOVERY_MANAGER_STAGE_WORK_RETURN_COOLDOWN 1u
#define RECOVERY_MANAGER_STAGE_RECOVERY_RETURN_COOLDOWN 0u
#define RECOVERY_MANAGER_STAGE_IDLE_RETURN_COOLDOWN 0u

#define RECOVERY_MANAGER_STAGE_TRAVEL_REALLOC_COOLDOWN 2u
#define RECOVERY_MANAGER_STAGE_WORK_REALLOC_COOLDOWN 2u
#define RECOVERY_MANAGER_STAGE_RECOVERY_REALLOC_COOLDOWN 1u
#define RECOVERY_MANAGER_STAGE_IDLE_REALLOC_COOLDOWN 1u

#define RECOVERY_MANAGER_STAGE_TRAVEL_ABORT_COOLDOWN 3u
#define RECOVERY_MANAGER_STAGE_WORK_ABORT_COOLDOWN 3u
#define RECOVERY_MANAGER_STAGE_RECOVERY_ABORT_COOLDOWN 2u
#define RECOVERY_MANAGER_STAGE_IDLE_ABORT_COOLDOWN 2u

#define RECOVERY_MANAGER_STAGE_TRAVEL_RETURN_FLOOR_MULT 0.90f
#define RECOVERY_MANAGER_STAGE_TRAVEL_REALLOC_FLOOR_MULT 0.95f
#define RECOVERY_MANAGER_STAGE_WORK_REALLOC_FLOOR_MULT 0.90f
#define RECOVERY_MANAGER_STAGE_WORK_ABORT_FLOOR_MULT 0.95f
#define RECOVERY_MANAGER_STAGE_RECOVERY_LOCAL_FLOOR_MULT 0.90f
#define RECOVERY_MANAGER_STAGE_IDLE_FLOOR_MULT 1.00f

#define RECOVERY_MANAGER_RECOMMENDATION_MIN_CONFIDENCE 0.18f
#define RECOVERY_MANAGER_STAMINA_LOW_THRESHOLD 20.0f
#define RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD 10.0f
#define RECOVERY_MANAGER_STAMINA_LOW_BONUS 0.20f
#define RECOVERY_MANAGER_STAMINA_CRITICAL_BONUS 0.20f
#define RECOVERY_MANAGER_EQUIPMENT_LOW_THRESHOLD 20u
#define RECOVERY_MANAGER_EQUIPMENT_CRITICAL_THRESHOLD 10u
#define RECOVERY_MANAGER_MOBILITY_SEVERITY_SCALE 3.0f
#define RECOVERY_MANAGER_MOBILITY_STALL_FULL_SCALE 0.5f
#define RECOVERY_MANAGER_COMMUNICATION_MINOR_THRESHOLD 1u
#define RECOVERY_MANAGER_COMMUNICATION_MAJOR_THRESHOLD 2u
#define RECOVERY_MANAGER_COMMUNICATION_SEVERE_THRESHOLD 3u

/* ~5 min real time (was a bare "2" = 72s, unrealistically fast for
 * confirming sustained recovery improvement before de-escalating). */
#define RECOVERY_MANAGER_DEESCALATION_TICKS (TICKS_PER_HOUR / 12)
#define RECOVERY_MANAGER_MAX_ESCALATION_LEVEL 4u
#define RECOVERY_EVALUATION_WINDOW 20u

/* Tiered-trust rule for evaluate_fault_predictor() (v4 doc Section 3):
 * CONTINUE_MONITORING/LOCAL_RECOVERY are fine on the AI's word alone;
 * anything at or above REQUEST_TASK_REALLOCATION needs watchdog
 * corroboration (watchdog->anomaly_detected). The doc's Section 8 item
 * 2 explicitly lists these exact numbers as still open - "principle
 * locked, numbers not set" - so treat these two as placeholders to be
 * replaced once real evaluation data exists, not as tuned values.
 */
#define RECOVERY_MANAGER_PREDICTOR_REALLOCATE_CONFIDENCE 0.50f
#define RECOVERY_MANAGER_PREDICTOR_ESCALATE_CONFIDENCE 0.80f

/*
 * "OR sustained AI persistence" (v4 doc Section 3's stated alternative to
 * watchdog corroboration - previously NOT implemented, see this file's
 * evaluate_fault_predictor() comment history). This is a hard-consecutive-
 * tick streak counter on the AI's own confidence (fault_predictor_run()'s
 * scores.score[best_class], NOT watchdog->anomaly_confidence - "sustained
 * AI persistence" means the complex controller's own signal, distinct from
 * the safety controller's), reset to 0 on any tick confidence drops below
 * FAULT_PERSISTENCE_FLOOR.
 *
 * A cumulative up/down counter (decay a small amount on a bad tick instead
 * of hard-resetting to 0, matching evidence_channels.py's own RISE/DECAY
 * asymmetry) was designed and measured as an alternative and REJECTED: on
 * 60 seeds x 10 teams x 400 ticks (--fault-profile combined) with the same
 * false-reject/false-accept overlap methodology, no DOWN_STEP choice
 * (tested 1.0/0.5/0.3/0.15 x UP_STEP) improved separation over the hard-
 * streak version - at matched false-reject rates the hard streak's
 * false-accept was consistently as good or better (e.g. ~34.5%/28.1% FR/FA
 * for hard-streak at H=6 vs. ~42.1%/22.1% for the best up/down variant at
 * its nearest matched point, interpolating the hard-streak curve to the
 * same FR gives ~17.3% FA - still better). Reason: the up/down counter's
 * forgiveness applies just as generously to bursty, intermittent NOISE
 * (common under FAULT_PROFILE_COMBINED's concurrent-fault-class background
 * churn) as it does to a real fault's brief dips, canceling the intended
 * benefit on this dataset. Hard-consecutive-streak is therefore the
 * mechanism actually implemented - simpler, and not worse, not "the
 * fallback because the better idea didn't work out."
 *
 * FAULT_PERSISTENCE_FLOOR = 0.20, NOT WATCHDOG_ANOMALY_THRESHOLD (0.10,
 * core/watchdog.c) and NOT evidence_channels.py's ANOMALY_THRESHOLD
 * (also 0.10, matching WATCHDOG_ANOMALY_THRESHOLD - re-confirmed by
 * re-running validate_against_watchdog() against a freshly generated
 * dataset: 99.97% overall anomaly_detected agreement, vs. 88.13% if
 * ANOMALY_THRESHOLD were reverted to 0.35, the stale value an earlier
 * draft of this comment described; that draft is what's now stale, not
 * the constant - evidence_channels.py itself already matches). 0.20
 * instead reuses core/watchdog.c's real, current per-channel
 * evidence_flags_from_values() threshold, applied here to the combined
 * confidence score - a deliberate choice,
 * not a copy of either watchdog threshold, and deliberately NOT
 * FAULT_EVIDENCE_ONSET_LAG_TICKS=20 either (that constant answers a
 * different question - ticks to first detection, not sustained-ticks
 * required to trust the detection - and must not be reused here).
 *
 * FAULT_PERSISTENCE_TRIP_TICKS = 8 is calibrated specifically to the hard-
 * consecutive-streak mechanism at that exact floor: 48.5% false-reject
 * (real sustained episodes not yet trusted at 8 ticks) / 8.2% false-accept
 * (noise episodes wrongly trusted) in the same measurement. Not a strong
 * classifier - both rates are real and non-zero - which is why this stays
 * gating only CONTINUE_MONITORING/LOCAL_RECOVERY (Section 3's existing AI-
 * alone tier), not extended to justify AI-alone trust any higher.
 */
#define FAULT_PERSISTENCE_FLOOR 0.20f
#define FAULT_PERSISTENCE_TRIP_TICKS 8u

#endif

#!/usr/bin/env python3
"""
laptop/tools/test_eval_metrics.py

Unit tests for eval_metrics.py against hand-built ground-truth/detection
sequences with known answers (latency, misses, false alarms, wrong-class
detections, action-taken tracking, the consensus/local race). No simulator
invocation - see `make check-evaluate-determinism` for the end-to-end
determinism check that does run ./tron.

Run directly: python3 laptop/tools/test_eval_metrics.py
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from eval_metrics import (
    ACTION_CONTINUE_MONITORING,
    ACTION_RETURN_TO_SAFE_ZONE,
    EVAL_COMMUNICATION,
    EVAL_EQUIPMENT,
    EVAL_MOBILITY,
    EVAL_PEER_CONSENSUS,
    EVAL_STAMINA,
    FAULT_CLASSES,
    REASON_COMMUNICATION_FAILURE,
    REASON_CONSENSUS_FAULT,
    REASON_EQUIPMENT_FAULT,
    REASON_LOW_STAMINA,
    REASON_MOBILITY_FAILURE,
    REASON_NONE,
    TickRow,
    compute_run_metrics,
    detected_classes_any,
    detected_classes_system,
    parse_mission_outcome,
)

ALL_EVAL_SLOTS = (EVAL_MOBILITY, EVAL_COMMUNICATION, EVAL_STAMINA, EVAL_EQUIPMENT, EVAL_PEER_CONSENSUS)


def make_row(
    cycle,
    team_id,
    active_classes=(),
    system_detected=(),
    eval_reasons=None,
    tier_mobility=0,
    tier_communication=0,
    recommendation=ACTION_CONTINUE_MONITORING,
):
    fault_active = {cls: (1 if cls in active_classes else 0) for cls in FAULT_CLASSES}
    reasons = {slot: REASON_NONE for slot in ALL_EVAL_SLOTS}
    if eval_reasons:
        reasons.update(eval_reasons)
    return TickRow(
        cycle=cycle,
        team_id=team_id,
        fault_active=fault_active,
        detected_system=frozenset(system_detected),
        detected_any=detected_classes_any(reasons, tier_mobility, tier_communication),
        eval_reasons=reasons,
        tier_mobility=tier_mobility,
        tier_communication=tier_communication,
        recommendation=recommendation,
    )


class TestDetectedClassesSystem(unittest.TestCase):
    def test_mobility_failure_reason(self):
        self.assertEqual(detected_classes_system(REASON_MOBILITY_FAILURE, 0, 0), frozenset({"mobility"}))

    def test_communication_failure_reason(self):
        self.assertEqual(detected_classes_system(REASON_COMMUNICATION_FAILURE, 0, 0), frozenset({"communication"}))

    def test_resource_proxies(self):
        self.assertEqual(detected_classes_system(REASON_EQUIPMENT_FAULT, 0, 0), frozenset({"resource"}))
        self.assertEqual(detected_classes_system(REASON_LOW_STAMINA, 0, 0), frozenset({"resource"}))

    def test_consensus_fault_disambiguated_by_tier(self):
        self.assertEqual(detected_classes_system(REASON_CONSENSUS_FAULT, 2, 0), frozenset({"mobility"}))
        self.assertEqual(detected_classes_system(REASON_CONSENSUS_FAULT, 0, 2), frozenset({"communication"}))
        self.assertEqual(detected_classes_system(REASON_CONSENSUS_FAULT, 2, 2), frozenset({"mobility", "communication"}))
        self.assertEqual(detected_classes_system(REASON_CONSENSUS_FAULT, 1, 1), frozenset())  # LOCAL tier, not elevated

    def test_unmapped_reasons_are_empty(self):
        for reason in (0, 1, 6, 7, 9):  # NONE, PROGRESS_DEVIATION, MISSION_DELAY, TEAM_DEGRADED, PREDICTED_FAULT
            self.assertEqual(detected_classes_system(reason, 2, 2), frozenset())

    def test_environment_and_mission_never_attributed(self):
        for reason in range(10):
            for tm in (0, 1, 2):
                for tc in (0, 1, 2):
                    classes = detected_classes_system(reason, tm, tc)
                    self.assertNotIn("environment", classes)
                    self.assertNotIn("mission", classes)


class TestDetectedClassesAny(unittest.TestCase):
    def test_local_mobility_evaluator_alone(self):
        reasons = {EVAL_MOBILITY: REASON_MOBILITY_FAILURE}
        self.assertEqual(detected_classes_any(reasons, 0, 0), frozenset({"mobility"}))

    def test_local_communication_evaluator_alone(self):
        reasons = {EVAL_COMMUNICATION: REASON_COMMUNICATION_FAILURE}
        self.assertEqual(detected_classes_any(reasons, 0, 0), frozenset({"communication"}))

    def test_resource_from_either_stamina_or_equipment(self):
        self.assertEqual(detected_classes_any({EVAL_STAMINA: REASON_LOW_STAMINA}, 0, 0), frozenset({"resource"}))
        self.assertEqual(detected_classes_any({EVAL_EQUIPMENT: REASON_EQUIPMENT_FAULT}, 0, 0), frozenset({"resource"}))

    def test_consensus_and_local_both_flag_independently(self):
        # Peer consensus elevates mobility while the local communication
        # evaluator independently flags communication the same tick - both
        # must show up, unlike the fused single-reason "system" view.
        reasons = {EVAL_PEER_CONSENSUS: REASON_CONSENSUS_FAULT, EVAL_COMMUNICATION: REASON_COMMUNICATION_FAILURE}
        self.assertEqual(detected_classes_any(reasons, 2, 0), frozenset({"mobility", "communication"}))

    def test_consensus_both_tiers_elevated_credits_both(self):
        reasons = {EVAL_PEER_CONSENSUS: REASON_CONSENSUS_FAULT}
        self.assertEqual(detected_classes_any(reasons, 2, 2), frozenset({"mobility", "communication"}))

    def test_peer_consensus_not_built_sentinel_is_inert(self):
        # confidence=-1 sentinel doesn't affect this (reason-only check);
        # reason stays NONE when consensus isn't built, so nothing fires.
        reasons = {EVAL_PEER_CONSENSUS: REASON_NONE}
        self.assertEqual(detected_classes_any(reasons, 0, 0), frozenset())


class TestFusedVsAnyDivergence(unittest.TestCase):
    def test_concurrent_fault_hidden_by_fused_slot_is_caught_by_any(self):
        # Mobility and communication both genuinely active/flagged the same
        # tick, but the fused recovery_reason can only report ONE winner
        # (consensus's mobility claim, say) - "system" must miss the
        # communication episode; "any" must not.
        rows = []
        for cycle in range(10):
            active = ("mobility", "communication") if 2 <= cycle <= 6 else ()
            row = make_row(
                cycle,
                0,
                active_classes=active,
                system_detected=("mobility",) if 2 <= cycle <= 6 else (),
                eval_reasons={
                    EVAL_PEER_CONSENSUS: REASON_CONSENSUS_FAULT if 2 <= cycle <= 6 else REASON_NONE,
                    EVAL_COMMUNICATION: REASON_COMMUNICATION_FAILURE if 2 <= cycle <= 6 else REASON_NONE,
                },
                tier_mobility=2 if 2 <= cycle <= 6 else 0,
            )
            rows.append(row)

        results, _ = compute_run_metrics(rows)
        self.assertEqual(results[("communication", "system")].episodes_detected, 0)
        self.assertEqual(results[("communication", "any")].episodes_detected, 1)
        self.assertEqual(results[("mobility", "system")].episodes_detected, 1)
        self.assertEqual(results[("mobility", "any")].episodes_detected, 1)


class TestEpisodeMetrics(unittest.TestCase):
    def _mobility_metrics(self, rows, mode="system"):
        results, _ = compute_run_metrics(rows)
        return results[("mobility", mode)]

    def test_single_episode_detected_within_latency(self):
        rows = []
        for cycle in range(0, 30):
            active = ("mobility",) if 10 <= cycle <= 19 else ()
            detected = ("mobility",) if cycle == 13 else ()
            rows.append(make_row(cycle, 0, active, system_detected=detected))

        m = self._mobility_metrics(rows)
        self.assertEqual(m.episodes_total, 1)
        self.assertEqual(m.episodes_detected, 1)
        self.assertEqual(m.latencies, [3])
        self.assertEqual(m.detected_within[5], 1)
        self.assertEqual(m.false_alarm_ticks, 0)
        self.assertEqual(m.wrong_class_ticks, 0)

    def test_missed_episode_not_detected(self):
        rows = []
        for cycle in range(0, 20):
            active = ("mobility",) if 5 <= cycle <= 9 else ()
            rows.append(make_row(cycle, 0, active))

        m = self._mobility_metrics(rows)
        self.assertEqual(m.episodes_total, 1)
        self.assertEqual(m.episodes_detected, 0)
        self.assertIsNone(m.latency_median())
        self.assertEqual(m.detected_at_all_rate(), 0.0)
        self.assertIsNone(m.action_taken_rate())

    def test_detection_after_episode_ends_does_not_count(self):
        rows = []
        for cycle in range(0, 20):
            active = ("mobility",) if 5 <= cycle <= 9 else ()
            detected = ("mobility",) if cycle == 12 else ()
            rows.append(make_row(cycle, 0, active, system_detected=detected))

        m = self._mobility_metrics(rows)
        self.assertEqual(m.episodes_detected, 0)
        self.assertEqual(m.false_alarm_ticks, 1)
        self.assertEqual(m.false_alarm_durations, [1])

    def test_false_alarm_run_length(self):
        rows = []
        for cycle in range(0, 10):
            detected = ("mobility",) if 3 <= cycle <= 5 else ()
            rows.append(make_row(cycle, 0, (), system_detected=detected))

        m = self._mobility_metrics(rows)
        self.assertEqual(m.episodes_total, 0)
        self.assertEqual(m.false_alarm_ticks, 3)
        self.assertEqual(m.false_alarm_durations, [3])
        self.assertAlmostEqual(m.false_alarms_per_1000_team_ticks(), 300.0)

    def test_wrong_class_detection(self):
        rows = []
        for cycle in range(0, 10):
            active = ("communication",) if 2 <= cycle <= 6 else ()
            detected = ("mobility",) if 2 <= cycle <= 6 else ()
            rows.append(make_row(cycle, 0, active, system_detected=detected))

        results, _ = compute_run_metrics(rows)
        self.assertEqual(results[("communication", "system")].wrong_class_ticks, 5)
        self.assertEqual(results[("communication", "system")].episodes_detected, 0)
        self.assertEqual(results[("mobility", "system")].false_alarm_ticks, 5)

    def test_concurrency_split(self):
        rows = []
        for team_id in range(3):
            for cycle in range(0, 10):
                active = cycle < 5
                rows.append(make_row(cycle, team_id, ("mobility",) if active else ()))
        for cycle in range(0, 30):
            active = 20 <= cycle < 25
            rows.append(make_row(cycle, 5, ("mobility",) if active else ()))

        results, _ = compute_run_metrics(rows)
        self.assertEqual(results[("mobility>2", "system")].episodes_total, 3)
        self.assertEqual(results[("mobility<=2", "system")].episodes_total, 1)

    def test_action_taken_after_detection(self):
        rows = []
        for cycle in range(0, 20):
            active = ("mobility",) if 5 <= cycle <= 14 else ()
            detected = ("mobility",) if 7 <= cycle <= 14 else ()
            # Action dispatched a few ticks after first detection.
            recommendation = ACTION_RETURN_TO_SAFE_ZONE if cycle == 10 else ACTION_CONTINUE_MONITORING
            rows.append(make_row(cycle, 0, active, system_detected=detected, recommendation=recommendation))

        m = self._mobility_metrics(rows)
        self.assertEqual(m.episodes_detected, 1)
        self.assertEqual(m.episodes_action_taken, 1)
        self.assertEqual(m.action_taken_rate(), 1.0)

    def test_detected_but_no_action_taken(self):
        rows = []
        for cycle in range(0, 10):
            active = ("mobility",) if 2 <= cycle <= 6 else ()
            detected = ("mobility",) if 3 <= cycle <= 6 else ()
            rows.append(make_row(cycle, 0, active, system_detected=detected, recommendation=ACTION_CONTINUE_MONITORING))

        m = self._mobility_metrics(rows)
        self.assertEqual(m.episodes_detected, 1)
        self.assertEqual(m.episodes_action_taken, 0)
        self.assertEqual(m.action_taken_rate(), 0.0)

    def test_detection_survives_post_action_quiet_period(self):
        # Detected at tick 3; action dispatched at tick 4 sends the team to
        # rest, after which several evaluators deliberately go quiet
        # (recovery_manager.c's STATUS_RESTING/heading_to_rest guards) even
        # though fault_active stays 1 through the repair-confirmation
        # window. This must still count as ONE detected episode, not a
        # detection followed by new "misses".
        rows = []
        for cycle in range(0, 20):
            active = ("mobility",) if 2 <= cycle <= 15 else ()
            if cycle == 3:
                detected = ("mobility",)
            else:
                detected = ()  # quiet before and after the single flagged tick
            recommendation = ACTION_RETURN_TO_SAFE_ZONE if cycle >= 4 else ACTION_CONTINUE_MONITORING
            rows.append(make_row(cycle, 0, active, system_detected=detected, recommendation=recommendation))

        m = self._mobility_metrics(rows)
        self.assertEqual(m.episodes_total, 1)
        self.assertEqual(m.episodes_detected, 1)
        self.assertEqual(m.latencies, [1])
        self.assertEqual(m.episodes_action_taken, 1)


class TestRaceMetrics(unittest.TestCase):
    def test_consensus_flags_before_local_evaluator(self):
        rows = []
        for cycle in range(0, 20):
            active = ("mobility",) if 5 <= cycle <= 14 else ()
            consensus_flags = 5 <= cycle <= 14 and cycle >= 6   # consensus at tick 6
            local_flags = 5 <= cycle <= 14 and cycle >= 9       # local mobility evaluator at tick 9
            reasons = {
                EVAL_PEER_CONSENSUS: REASON_CONSENSUS_FAULT if consensus_flags else REASON_NONE,
                EVAL_MOBILITY: REASON_MOBILITY_FAILURE if local_flags else REASON_NONE,
            }
            rows.append(make_row(cycle, 0, active, eval_reasons=reasons, tier_mobility=2 if consensus_flags else 0))

        _, race = compute_run_metrics(rows)
        m = race["mobility"]
        self.assertEqual(m.episodes_total, 1)
        self.assertEqual(m.both_flagged, 1)
        self.assertEqual(m.consensus_first, 1)
        self.assertEqual(m.lead_ticks, [3])  # local at 9, consensus at 6 -> beat by 3 ticks
        self.assertEqual(m.consensus_first_rate(), 1.0)

    def test_local_flags_before_consensus(self):
        rows = []
        for cycle in range(0, 20):
            active = ("communication",) if 5 <= cycle <= 14 else ()
            consensus_flags = 5 <= cycle <= 14 and cycle >= 10
            local_flags = 5 <= cycle <= 14 and cycle >= 6
            reasons = {
                EVAL_PEER_CONSENSUS: REASON_CONSENSUS_FAULT if consensus_flags else REASON_NONE,
                EVAL_COMMUNICATION: REASON_COMMUNICATION_FAILURE if local_flags else REASON_NONE,
            }
            rows.append(make_row(cycle, 0, active, eval_reasons=reasons, tier_communication=2 if consensus_flags else 0))

        _, race = compute_run_metrics(rows)
        m = race["communication"]
        self.assertEqual(m.both_flagged, 1)
        self.assertEqual(m.local_first, 1)
        self.assertEqual(m.consensus_first, 0)

    def test_consensus_only_catch(self):
        rows = []
        for cycle in range(0, 10):
            active = ("mobility",) if 2 <= cycle <= 6 else ()
            reasons = {EVAL_PEER_CONSENSUS: REASON_CONSENSUS_FAULT if 3 <= cycle <= 6 else REASON_NONE}
            rows.append(make_row(cycle, 0, active, eval_reasons=reasons, tier_mobility=2 if 3 <= cycle <= 6 else 0))

        _, race = compute_run_metrics(rows)
        m = race["mobility"]
        self.assertEqual(m.consensus_only, 1)
        self.assertEqual(m.both_flagged, 0)
        self.assertIsNone(m.consensus_first_rate())

    def test_neither_flags(self):
        rows = []
        for cycle in range(0, 10):
            active = ("mobility",) if 2 <= cycle <= 6 else ()
            rows.append(make_row(cycle, 0, active))

        _, race = compute_run_metrics(rows)
        self.assertEqual(race["mobility"].neither, 1)


class TestMissionOutcome(unittest.TestCase):
    def test_sums_across_teams(self):
        diag_text = (
            "[DIAG THROUGHPUT] team=0 incidents_resolved=3 stage_completions=5 resting=1 working=2 available=3\n"
            "[DIAG THROUGHPUT] team=1 incidents_resolved=2 stage_completions=1 resting=0 working=1 available=4\n"
            "some unrelated line\n"
        )
        outcome = parse_mission_outcome(diag_text)
        self.assertEqual(outcome["incidents_resolved"], 5)
        self.assertEqual(outcome["stage_completions"], 6)

    def test_empty_text(self):
        outcome = parse_mission_outcome("")
        self.assertEqual(outcome["incidents_resolved"], 0)
        self.assertEqual(outcome["stage_completions"], 0)


if __name__ == "__main__":
    unittest.main()

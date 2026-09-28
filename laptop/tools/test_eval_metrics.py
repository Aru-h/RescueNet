#!/usr/bin/env python3
"""
laptop/tools/test_eval_metrics.py

Unit tests for eval_metrics.py against hand-built ground-truth/detection
sequences with known answers (latency, misses, false alarms, wrong-class
detections). No simulator invocation - see `make check-evaluate-determinism`
for the end-to-end determinism check that does run ./tron.

Run directly: python3 laptop/tools/test_eval_metrics.py
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from eval_metrics import (
    FAULT_CLASSES,
    TickRow,
    compute_run_metrics,
    detected_classes,
    parse_mission_outcome,
)


def make_row(cycle, team_id, active_classes=(), detected=()):
    fault_active = {cls: (1 if cls in active_classes else 0) for cls in FAULT_CLASSES}
    return TickRow(cycle=cycle, team_id=team_id, fault_active=fault_active, detected=frozenset(detected))


class TestDetectedClasses(unittest.TestCase):
    def test_mobility_failure_reason(self):
        self.assertEqual(detected_classes(3, 0, 0), frozenset({"mobility"}))

    def test_communication_failure_reason(self):
        self.assertEqual(detected_classes(4, 0, 0), frozenset({"communication"}))

    def test_resource_proxies(self):
        self.assertEqual(detected_classes(5, 0, 0), frozenset({"resource"}))
        self.assertEqual(detected_classes(2, 0, 0), frozenset({"resource"}))

    def test_mission_delay(self):
        self.assertEqual(detected_classes(6, 0, 0), frozenset({"mission"}))

    def test_consensus_fault_disambiguated_by_tier(self):
        self.assertEqual(detected_classes(8, 2, 0), frozenset({"mobility"}))
        self.assertEqual(detected_classes(8, 0, 2), frozenset({"communication"}))
        self.assertEqual(detected_classes(8, 2, 2), frozenset({"mobility", "communication"}))
        self.assertEqual(detected_classes(8, 1, 1), frozenset())  # LOCAL tier, not elevated

    def test_unmapped_reasons_are_empty(self):
        for reason in (0, 1, 7, 9):
            self.assertEqual(detected_classes(reason, 2, 2), frozenset())

    def test_environment_never_attributed(self):
        # No reason code maps to "environment" - by design (see module
        # docstring). Confirm no combination of inputs produces it.
        for reason in range(10):
            for tm in (0, 1, 2):
                for tc in (0, 1, 2):
                    self.assertNotIn("environment", detected_classes(reason, tm, tc))


class TestEpisodeMetrics(unittest.TestCase):
    def test_single_episode_detected_within_latency(self):
        # Team 0: mobility fault active ticks 10..19 (10 ticks), detected
        # at tick 13 -> latency 3.
        rows = []
        for cycle in range(0, 30):
            active = ("mobility",) if 10 <= cycle <= 19 else ()
            detected = ("mobility",) if cycle == 13 else ()
            rows.append(make_row(cycle, 0, active, detected))

        results = compute_run_metrics(rows)
        m = results["mobility"]
        self.assertEqual(m.episodes_total, 1)
        self.assertEqual(m.episodes_detected, 1)
        self.assertEqual(m.latencies, [3])
        self.assertEqual(m.detected_within[5], 1)
        self.assertEqual(m.detected_within[10], 1)
        self.assertEqual(m.detected_within[20], 1)
        self.assertEqual(m.false_alarm_ticks, 0)
        self.assertEqual(m.wrong_class_ticks, 0)

    def test_missed_episode_not_detected(self):
        rows = []
        for cycle in range(0, 20):
            active = ("mobility",) if 5 <= cycle <= 9 else ()
            rows.append(make_row(cycle, 0, active, ()))  # never detected

        m = compute_run_metrics(rows)["mobility"]
        self.assertEqual(m.episodes_total, 1)
        self.assertEqual(m.episodes_detected, 0)
        self.assertEqual(m.latencies, [])
        self.assertIsNone(m.latency_median())
        self.assertEqual(m.detected_at_all_rate(), 0.0)

    def test_detection_after_episode_ends_does_not_count(self):
        rows = []
        for cycle in range(0, 20):
            active = ("mobility",) if 5 <= cycle <= 9 else ()
            detected = ("mobility",) if cycle == 12 else ()  # after episode end (9)
            rows.append(make_row(cycle, 0, active, detected))

        m = compute_run_metrics(rows)["mobility"]
        self.assertEqual(m.episodes_total, 1)
        self.assertEqual(m.episodes_detected, 0)
        # The tick-12 detection while mobility is inactive still counts as
        # a false alarm.
        self.assertEqual(m.false_alarm_ticks, 1)
        self.assertEqual(m.false_alarm_durations, [1])

    def test_false_alarm_run_length(self):
        rows = []
        for cycle in range(0, 10):
            detected = ("mobility",) if 3 <= cycle <= 5 else ()  # 3-tick false alarm
            rows.append(make_row(cycle, 0, (), detected))

        m = compute_run_metrics(rows)["mobility"]
        self.assertEqual(m.episodes_total, 0)
        self.assertEqual(m.false_alarm_ticks, 3)
        self.assertEqual(m.false_alarm_durations, [3])
        self.assertEqual(m.false_alarm_mean_duration(), 3.0)
        self.assertAlmostEqual(m.false_alarms_per_1000_team_ticks(), 300.0)

    def test_wrong_class_detection(self):
        # Communication fault is active, but the system's fused decision
        # names mobility instead -> wrong-class detection for
        # communication, AND a false alarm for mobility (mobility isn't
        # active at all).
        rows = []
        for cycle in range(0, 10):
            active = ("communication",) if 2 <= cycle <= 6 else ()
            detected = ("mobility",) if 2 <= cycle <= 6 else ()
            rows.append(make_row(cycle, 0, active, detected))

        results = compute_run_metrics(rows)
        self.assertEqual(results["communication"].wrong_class_ticks, 5)
        self.assertEqual(results["communication"].episodes_detected, 0)
        self.assertEqual(results["mobility"].false_alarm_ticks, 5)

    def test_multiple_episodes_multiple_teams(self):
        rows = []
        # Team 0: two mobility episodes, both detected quickly.
        for cycle in range(0, 40):
            active = cycle in range(2, 6) or cycle in range(20, 25)
            detected = cycle in (3, 21)
            rows.append(make_row(cycle, 0, ("mobility",) if active else (), ("mobility",) if detected else ()))
        # Team 1: one mobility episode, never detected.
        for cycle in range(0, 40):
            active = cycle in range(10, 15)
            rows.append(make_row(cycle, 1, ("mobility",) if active else (), ()))

        m = compute_run_metrics(rows)["mobility"]
        self.assertEqual(m.episodes_total, 3)
        self.assertEqual(m.episodes_detected, 2)
        self.assertEqual(sorted(m.latencies), [1, 1])
        self.assertAlmostEqual(m.detected_at_all_rate(), 2 / 3)

    def test_concurrency_split(self):
        # 3 teams all faulted on mobility starting tick 0 -> a ">2" episode
        # for each of them. 1 team faulted alone starting tick 20 -> a
        # "<=2" episode.
        rows = []
        for team_id in range(3):
            for cycle in range(0, 10):
                active = cycle < 5
                rows.append(make_row(cycle, team_id, ("mobility",) if active else (), ()))
        for cycle in range(0, 30):
            active = 20 <= cycle < 25
            rows.append(make_row(cycle, 5, ("mobility",) if active else (), ()))

        results = compute_run_metrics(rows)
        self.assertEqual(results["mobility>2"].episodes_total, 3)
        self.assertEqual(results["mobility<=2"].episodes_total, 1)


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

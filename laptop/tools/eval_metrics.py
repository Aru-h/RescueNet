#!/usr/bin/env python3
"""
laptop/tools/eval_metrics.py

Pure, dependency-free (stdlib only) functions that turn a tron_snapshot_
dataset.csv (as produced by `./tron <seed> <count> <ticks>
--fault-profile=<p>`) plus the run's --diag stderr text into the fault-
detection and mission-outcome metrics laptop/tools/evaluate.py reports.

Kept separate from evaluate.py (which owns building binaries, running them,
and orchestration) so this module can be unit-tested on hand-built rows
without ever invoking the simulator - see test_eval_metrics.py.

== What counts as "a detection" and why ==

The CSV never states "the system decided team X has fault class Y"
directly: core/recovery_manager.c fuses up to 7 evaluators into ONE
RecoveryStatus_t{recommendation, reason, confidence} per team per tick
(recovery_select_primary_signal() picks the single highest-severity
signal, ties broken by confidence - see recovery_manager.c). We reconstruct
a "detected fault class" set per team per tick from `recovery_reason`
(core/recovery_manager.h's RecoveryReason_t) plus, when the reason is
CONSENSUS_FAULT, the two `consensus_tier_mobility`/`consensus_tier_
communication` columns (core/consensus.h's ConsensusTier_t) to say which
of the two classes consensus actually elevated:

  reason value  name                      -> detected class(es)
  ------------  ------------------------  --------------------------------
  3             MOBILITY_FAILURE          -> {mobility}
  4             COMMUNICATION_FAILURE     -> {communication}
  5             EQUIPMENT_FAULT           -> {resource}   (proxy, see below)
  2             LOW_STAMINA               -> {resource}   (proxy, see below)
  6             MISSION_DELAY             -> {mission}
  8             CONSENSUS_FAULT           -> {mobility} if consensus_tier_
                                             mobility==ELEVATED(2), plus/or
                                             {communication} if consensus_
                                             tier_communication==ELEVATED(2)
  0,1,7,9       NONE/PROGRESS_DEVIATION/
                TEAM_DEGRADED/PREDICTED_FAULT -> {} (no class attribution)

Rationale, and the honest limitations this implies:

- mobility/communication have a clean 1:1 RecoveryReason_t each, AND are
  the only two classes core/consensus.c ever votes on
  (CONSENSUS_NUM_VOTED_CLASSES == 2, core/consensus.h) - so these two
  classes are the only ones where a peer-consensus contribution can be
  measured at all. This asymmetry is structural, not a measurement gap.
- resource is approximated by folding EQUIPMENT_FAULT and LOW_STAMINA
  together. Neither reason is class-exclusive in the code (a recovery_
  manager.c comment at the CONSENSUS_FAULT/PREDICTED_FAULT evaluators
  explicitly says "RESOURCE and ENVIRONMENT don't have a clean existing
  RecoveryReason_t counterpart"), so this is a best-effort proxy, not a
  guaranteed-correct label. Treat resource-class numbers as directional.
- environment has NO RecoveryReason_t at all today - no evaluator in
  recovery_manager.c ever names it as the reason it acted. We do NOT
  invent a proxy for it (unlike resource) because every remaining reason
  code (PROGRESS_DEVIATION, TEAM_DEGRADED) is generic enough to fire for
  almost any underlying cause, and attributing them to "environment"
  specifically would be fabricating a signal the system does not have.
  Environment therefore always reports 0% detected in this tool's output
  - that is a true finding about the current recovery_manager.c, not a
  bug in this tool (see docs/evaluation/README.md).
- CONSENSUS_FAULT (reason 8) is reconstructed from the tier columns rather
  than assumed "mobility" or "communication": both classes can be
  simultaneously elevated, in which case the tick counts as a detection
  for both.
- PREDICTED_FAULT (reason 9) is left unmapped: while FAULT_MODEL_LOADED
  is 0 (see core/fault_predictor.h), fault_predictor_run() always returns
  all-zero scores, so this reason cannot fire in the rules/rules+consensus
  variants this tool ships today; once the AI variant is real, add its
  per-class mapping here (see docs/evaluation/README.md).

Ground truth is always the CSV's own fault_active_mobility/communication/
resource/environment/mission columns - never something this module infers.
"""

from __future__ import annotations

import csv
import statistics
from dataclasses import dataclass, field
from typing import Dict, FrozenSet, Iterable, List, Optional, Sequence

FAULT_CLASSES: List[str] = [
    "mobility",
    "communication",
    "resource",
    "environment",
    "mission",
]

# EventType_t's EVENT_TICK (core/mission_state.h) - the unconditional,
# once-per-team-per-tick row logged in simulation_step()'s Pass 1, before
# any recovery_dispatch() runs. Every other EventType_t value is an extra
# row for the same (team, cycle) tuple layered on top of this one (e.g. a
# FAULT_INJECTED or STAGE_COMPLETE row can share a cycle with its team's
# EVENT_TICK row) - filtering to EVENT_TICK gives exactly one row per
# (team_id, cycle), which is what a per-tick ground-truth/detection time
# series needs.
EVENT_TICK = 12

# core/recovery_manager.h RecoveryReason_t values.
REASON_NONE = 0
REASON_PROGRESS_DEVIATION = 1
REASON_LOW_STAMINA = 2
REASON_MOBILITY_FAILURE = 3
REASON_COMMUNICATION_FAILURE = 4
REASON_EQUIPMENT_FAULT = 5
REASON_MISSION_DELAY = 6
REASON_TEAM_DEGRADED = 7
REASON_CONSENSUS_FAULT = 8
REASON_PREDICTED_FAULT = 9

# core/consensus.h ConsensusTier_t.
CONSENSUS_TIER_ELEVATED = 2

DETECTION_WINDOWS = (5, 10, 20)


def detected_classes(reason: int, tier_mobility: int, tier_communication: int) -> FrozenSet[str]:
    """The set of fault classes the system's fused recovery decision names
    for this team-tick. See module docstring for the mapping and why it is
    what it is."""
    if reason == REASON_MOBILITY_FAILURE:
        return frozenset({"mobility"})
    if reason == REASON_COMMUNICATION_FAILURE:
        return frozenset({"communication"})
    if reason in (REASON_EQUIPMENT_FAULT, REASON_LOW_STAMINA):
        return frozenset({"resource"})
    if reason == REASON_MISSION_DELAY:
        return frozenset({"mission"})
    if reason == REASON_CONSENSUS_FAULT:
        classes = set()
        if tier_mobility == CONSENSUS_TIER_ELEVATED:
            classes.add("mobility")
        if tier_communication == CONSENSUS_TIER_ELEVATED:
            classes.add("communication")
        return frozenset(classes)
    return frozenset()


@dataclass
class TickRow:
    cycle: int
    team_id: int
    fault_active: Dict[str, int]
    detected: FrozenSet[str]


def parse_tick_rows(csv_path: str) -> List[TickRow]:
    """Read a tron_snapshot_dataset.csv and return one TickRow per
    (team_id, cycle), filtered to EVENT_TICK rows only."""
    rows: List[TickRow] = []
    with open(csv_path, newline="") as handle:
        reader = csv.DictReader(handle)
        for raw in reader:
            if int(raw["event_type"]) != EVENT_TICK:
                continue
            fault_active = {cls: int(raw[f"fault_active_{cls}"]) for cls in FAULT_CLASSES}
            classes = detected_classes(
                int(raw["recovery_reason"]),
                int(raw["consensus_tier_mobility"]),
                int(raw["consensus_tier_communication"]),
            )
            rows.append(
                TickRow(
                    cycle=int(raw["cycle"]),
                    team_id=int(raw["team_id"]),
                    fault_active=fault_active,
                    detected=classes,
                )
            )
    return rows


def _episodes(active: Sequence[int], cycles: Sequence[int]) -> List[tuple]:
    """Contiguous (start_cycle, end_cycle) spans where active[i] == 1,
    given cycles is sorted ascending and (mostly) contiguous."""
    spans = []
    start = None
    prev_cycle = None
    for value, cycle in zip(active, cycles):
        if value:
            if start is None:
                start = cycle
        else:
            if start is not None:
                spans.append((start, prev_cycle))
                start = None
        prev_cycle = cycle
    if start is not None:
        spans.append((start, prev_cycle))
    return spans


@dataclass
class ClassMetrics:
    episodes_total: int = 0
    episodes_detected: int = 0
    detected_within: Dict[int, int] = field(default_factory=lambda: {w: 0 for w in DETECTION_WINDOWS})
    latencies: List[int] = field(default_factory=list)
    false_alarm_ticks: int = 0
    false_alarm_durations: List[int] = field(default_factory=list)
    wrong_class_ticks: int = 0
    team_ticks: int = 0

    def detected_at_all_rate(self) -> Optional[float]:
        if self.episodes_total == 0:
            return None
        return self.episodes_detected / self.episodes_total

    def detected_within_rate(self, window: int) -> Optional[float]:
        if self.episodes_total == 0:
            return None
        return self.detected_within[window] / self.episodes_total

    def latency_median(self) -> Optional[float]:
        if not self.latencies:
            return None
        return statistics.median(self.latencies)

    def latency_p90(self) -> Optional[float]:
        if not self.latencies:
            return None
        ordered = sorted(self.latencies)
        idx = min(len(ordered) - 1, int(round(0.9 * (len(ordered) - 1))))
        return float(ordered[idx])

    def false_alarms_per_1000_team_ticks(self) -> Optional[float]:
        if self.team_ticks == 0:
            return None
        return 1000.0 * self.false_alarm_ticks / self.team_ticks

    def false_alarm_mean_duration(self) -> Optional[float]:
        if not self.false_alarm_durations:
            return None
        return statistics.mean(self.false_alarm_durations)

    def wrong_class_per_1000_team_ticks(self) -> Optional[float]:
        if self.team_ticks == 0:
            return None
        return 1000.0 * self.wrong_class_ticks / self.team_ticks


def _compute_class_metrics(
    team_series: Dict[int, List[TickRow]], fault_class: str, episode_filter=None
) -> ClassMetrics:
    metrics = ClassMetrics()
    for team_id, rows in team_series.items():
        rows = sorted(rows, key=lambda r: r.cycle)
        cycles = [r.cycle for r in rows]
        active = [r.fault_active[fault_class] for r in rows]
        detected_here = [1 if fault_class in r.detected else 0 for r in rows]
        by_cycle = {r.cycle: r for r in rows}

        metrics.team_ticks += len(rows)

        # False alarms: detected this class while it is NOT active.
        in_alarm = False
        alarm_len = 0
        for r in rows:
            is_false_alarm = (fault_class in r.detected) and (r.fault_active[fault_class] == 0)
            if is_false_alarm:
                metrics.false_alarm_ticks += 1
                alarm_len += 1
                in_alarm = True
            else:
                if in_alarm:
                    metrics.false_alarm_durations.append(alarm_len)
                in_alarm = False
                alarm_len = 0
        if in_alarm:
            metrics.false_alarm_durations.append(alarm_len)

        # Wrong-class detections: some class active, but the detected
        # class(es) are entirely disjoint from every currently-active class.
        for r in rows:
            active_classes = {c for c, v in r.fault_active.items() if v}
            if fault_class not in active_classes:
                continue
            if not r.detected:
                continue
            if fault_class not in r.detected:
                metrics.wrong_class_ticks += 1

        episodes = _episodes(active, cycles)
        for start, end in episodes:
            if episode_filter is not None and not episode_filter(team_id, start, end):
                continue
            metrics.episodes_total += 1
            first_hit = None
            for cycle in range(start, end + 1):
                row = by_cycle.get(cycle)
                if row is not None and fault_class in row.detected:
                    first_hit = cycle
                    break
            if first_hit is not None:
                metrics.episodes_detected += 1
                latency = first_hit - start
                metrics.latencies.append(latency)
                for window in DETECTION_WINDOWS:
                    if latency <= window:
                        metrics.detected_within[window] += 1
    return metrics


def compute_run_metrics(rows: List[TickRow]) -> Dict[str, ClassMetrics]:
    """Given every TickRow from one seed's run (all teams, all ticks),
    return per-fault-class metrics, plus mobility/communication split by
    how many teams had that class active at the episode's start tick
    ("<=2" vs ">2" concurrently-faulted teams)."""
    team_series: Dict[int, List[TickRow]] = {}
    for row in rows:
        team_series.setdefault(row.team_id, []).append(row)

    # concurrency[class][cycle] = number of teams with that class active.
    concurrency: Dict[str, Dict[int, int]] = {cls: {} for cls in ("mobility", "communication")}
    for row in rows:
        for cls in ("mobility", "communication"):
            if row.fault_active[cls]:
                concurrency[cls][row.cycle] = concurrency[cls].get(row.cycle, 0) + 1

    results: Dict[str, ClassMetrics] = {}
    for cls in FAULT_CLASSES:
        results[cls] = _compute_class_metrics(team_series, cls)

    for cls in ("mobility", "communication"):
        def le2(team_id, start, end, cls=cls):
            return concurrency[cls].get(start, 0) <= 2

        def gt2(team_id, start, end, cls=cls):
            return concurrency[cls].get(start, 0) > 2

        results[f"{cls}<=2"] = _compute_class_metrics(team_series, cls, episode_filter=le2)
        results[f"{cls}>2"] = _compute_class_metrics(team_series, cls, episode_filter=gt2)

    return results


DIAG_THROUGHPUT_RE = __import__("re").compile(
    r"\[DIAG THROUGHPUT\] team=(\d+) incidents_resolved=(\d+) stage_completions=(\d+)"
)


def parse_mission_outcome(diag_text: str) -> Dict[str, int]:
    """Sum [DIAG THROUGHPUT] incidents_resolved/stage_completions across
    every team from a run's --diag stderr text (laptop/main.c)."""
    incidents_resolved = 0
    stage_completions = 0
    for match in DIAG_THROUGHPUT_RE.finditer(diag_text):
        incidents_resolved += int(match.group(2))
        stage_completions += int(match.group(3))
    return {"incidents_resolved": incidents_resolved, "stage_completions": stage_completions}

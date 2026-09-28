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

== Two notions of "detection", and why both are reported ==

core/recovery_manager.c's recovery_manager_step() fuses up to 7 evaluators
(progress, stamina, mobility, communication, equipment, the fault
predictor, and, under CONSENSUS_ENFORCE, peer consensus) into ONE
RecoveryStatus_t{recommendation, reason, confidence} per team per tick
(recovery_select_primary_signal() picks the single highest-severity
signal, ties broken by confidence). That fused `recovery_reason` alone is
not enough to answer "did the system's *communication* evaluator ever see
this fault": with concurrent faults, one evaluator's signal can occupy the
fused slot while another evaluator also fired the very same tick (e.g. a
peer-consensus CONSENSUS_FAULT for mobility winning the fused slot while
evaluate_communication() also flagged a real, independent communication
fault that same tick - reading only the fused reason would wrongly score
that as a missed communication detection).

So this module reports detection two ways, per fault class:

1. **"system decision"** - what recovery_dispatch() actually acts on: the
   fused `recovery_reason` (plus, when it is CONSENSUS_FAULT, the
   `consensus_tier_mobility`/`consensus_tier_communication` columns to say
   which of the two classes consensus itself elevated). This is "did the
   system's one, final decision this tick name this class."
2. **"any evaluator flagged"** - core/recovery_manager.h's
   `recovery_manager_get_evaluator_verdict()` per-evaluator columns
   (`eval_<name>_reason`/`eval_<name>_confidence`, laptop/snapshot.c),
   added specifically so this ambiguity is resolvable: each of the 7
   evaluators' own, unfused verdict for the tick, independent of which one
   won the fused slot. "Any evaluator flagged class X" is true whenever
   *some* evaluator relevant to X said something other than
   CONTINUE_MONITORING/NONE, whether or not it won the tick's fused
   decision.

mobility/communication have a clean 1:1 evaluator each
(evaluate_mobility()/evaluate_communication()), are the only two classes
core/consensus.c ever votes on (CONSENSUS_NUM_VOTED_CLASSES == 2), and are
the only two the peer-consensus evaluator (evaluate_peer_consensus()) can
ever contribute to - so consensus's measurable effect is structurally
limited to these two classes.

resource is approximated by folding evaluate_stamina()'s LOW_STAMINA and
evaluate_equipment()'s EQUIPMENT_FAULT verdicts together - core/
recovery_manager.c documents that neither reason is class-exclusive, so
treat resource-class numbers as directional, not exact.

environment and mission have NO evaluator at all: no function in
recovery_manager.c ever names either as the reason it acted (mission's
only nominal RecoveryReason_t, MISSION_DELAY, is defined in recovery_
manager.h's enum but never actually returned anywhere in recovery_
manager.c - dead code, not a live detector). Both classes therefore always
report 0% detected under either notion above. That is a genuine finding
about recovery_manager.c as it stands, not a limitation of this tool -
closing it means adding a real evaluator, which changes simulator
behaviour and is out of this tool's scope.

evaluate_progress()'s PROGRESS_DEVIATION and the mobility/equipment
evaluators' shared TEAM_DEGRADED reason are both generic enough (fire for
almost any underlying cause) that neither is attributed to a specific
fault class beyond what's already handled: TEAM_DEGRADED only ever comes
from evaluate_mobility() or evaluate_equipment() themselves, so under
"any evaluator flagged", checking those two evaluators' own reason column
for "not NONE" already captures it without needing to name TEAM_DEGRADED
specially. PROGRESS_DEVIATION (evaluate_progress()) is not attributed to
any class - it's the one evaluator with no class-specific counterpart at
all.

evaluate_fault_predictor()'s PREDICTED_FAULT is left unmapped for the same
reason as before: while FAULT_MODEL_LOADED is 0 (core/fault_predictor.h),
fault_predictor_run() always returns all-zero scores, so this reason
cannot fire in the rules/rules+consensus variants this tool ships today.

Ground truth is always the CSV's own fault_active_mobility/communication/
resource/environment/mission columns - never something this module infers.

== Episodes are scored by their FIRST detection, not tick-by-tick ==

A fault episode (a contiguous fault_active_<class>==1 span for one team) is
"detected" if it was ever flagged during that span - scored once, at the
first flagged tick. Ticks after that first hit are never re-scored as new
misses, including ticks where the team has already been dispatched an
action (RETURN_TO_SAFE_ZONE/REQUEST_TASK_REALLOCATION/ABORT_MISSION) that
sends it to rest - several evaluators (see evaluate_mobility()/evaluate_
stamina()/evaluate_communication()/evaluate_equipment() in recovery_
manager.c) deliberately go quiet (CONTINUE_MONITORING/NONE) once a team is
RESTING or heading_to_rest specifically to avoid re-dispatching it, and
that quiet period must not undo an already-scored detection.
`test_eval_metrics.py`'s test_detection_survives_post_action_quiet_period
exercises exactly this. Separately, `action_taken_rate` reports how often
a detected episode actually led to a dispatched action (fused
`recovery_recommendation` reaching RETURN_TO_SAFE_ZONE or higher at or
after the first detection) - "detected" and "acted on" are different
things and both are reported.
"""

from __future__ import annotations

import csv
import statistics
from dataclasses import dataclass, field
from typing import Dict, FrozenSet, List, Optional, Sequence

FAULT_CLASSES: List[str] = [
    "mobility",
    "communication",
    "resource",
    "environment",
    "mission",
]

# EventType_t's EVENT_TICK (core/mission_state.h) - the unconditional,
# once-per-team-per-tick row logged in simulation_step()'s Pass 1, before
# any recovery_dispatch() runs. Every other EventType_t value shares a
# (team_id, cycle) with its team's EVENT_TICK row - filtering to EVENT_TICK
# gives exactly one row per (team_id, cycle).
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

# core/recovery_manager.h RecoveryAction_t values.
ACTION_CONTINUE_MONITORING = 0
ACTION_LOCAL_RECOVERY = 1
ACTION_RETURN_TO_SAFE_ZONE = 2
ACTION_REQUEST_TASK_REALLOCATION = 3
ACTION_ABORT_MISSION = 4

# core/consensus.h ConsensusTier_t.
CONSENSUS_TIER_ELEVATED = 2

# core/recovery_manager.h's RECOVERY_MANAGER_EVAL_* slot indices - must
# match laptop/snapshot.c's eval_<name>_reason/eval_<name>_confidence
# column order exactly.
EVAL_PROGRESS = 0
EVAL_STAMINA = 1
EVAL_MOBILITY = 2
EVAL_COMMUNICATION = 3
EVAL_EQUIPMENT = 4
EVAL_FAULT_PREDICTOR = 5
EVAL_PEER_CONSENSUS = 6

DETECTION_WINDOWS = (5, 10, 20)


def detected_classes_system(reason: int, tier_mobility: int, tier_communication: int) -> FrozenSet[str]:
    """"System decision" detection: the fused recovery_reason (plus tier
    columns to disambiguate CONSENSUS_FAULT). See module docstring."""
    if reason == REASON_MOBILITY_FAILURE:
        return frozenset({"mobility"})
    if reason == REASON_COMMUNICATION_FAILURE:
        return frozenset({"communication"})
    if reason in (REASON_EQUIPMENT_FAULT, REASON_LOW_STAMINA):
        return frozenset({"resource"})
    if reason == REASON_CONSENSUS_FAULT:
        classes = set()
        if tier_mobility == CONSENSUS_TIER_ELEVATED:
            classes.add("mobility")
        if tier_communication == CONSENSUS_TIER_ELEVATED:
            classes.add("communication")
        return frozenset(classes)
    return frozenset()


def detected_classes_any(eval_reasons: Dict[int, int], tier_mobility: int, tier_communication: int) -> FrozenSet[str]:
    """"Any evaluator flagged" detection: each evaluator's own, unfused
    verdict (core/recovery_manager.h's recovery_manager_get_evaluator_
    verdict(), laptop/snapshot.c's eval_<name>_reason columns). See module
    docstring for why this differs from detected_classes_system()."""
    classes = set()
    if eval_reasons.get(EVAL_MOBILITY, REASON_NONE) != REASON_NONE:
        classes.add("mobility")
    if eval_reasons.get(EVAL_COMMUNICATION, REASON_NONE) != REASON_NONE:
        classes.add("communication")
    if eval_reasons.get(EVAL_STAMINA, REASON_NONE) != REASON_NONE or eval_reasons.get(EVAL_EQUIPMENT, REASON_NONE) != REASON_NONE:
        classes.add("resource")
    if eval_reasons.get(EVAL_PEER_CONSENSUS, REASON_NONE) == REASON_CONSENSUS_FAULT:
        if tier_mobility == CONSENSUS_TIER_ELEVATED:
            classes.add("mobility")
        if tier_communication == CONSENSUS_TIER_ELEVATED:
            classes.add("communication")
    return frozenset(classes)


@dataclass
class TickRow:
    cycle: int
    team_id: int
    fault_active: Dict[str, int]
    detected_system: FrozenSet[str]
    detected_any: FrozenSet[str]
    eval_reasons: Dict[int, int]
    tier_mobility: int
    tier_communication: int
    recommendation: int


EVAL_COLUMNS = {
    EVAL_PROGRESS: "eval_progress_reason",
    EVAL_STAMINA: "eval_stamina_reason",
    EVAL_MOBILITY: "eval_mobility_reason",
    EVAL_COMMUNICATION: "eval_communication_reason",
    EVAL_EQUIPMENT: "eval_equipment_reason",
    EVAL_FAULT_PREDICTOR: "eval_fault_predictor_reason",
    EVAL_PEER_CONSENSUS: "eval_peer_consensus_reason",
}


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
            tier_mobility = int(raw["consensus_tier_mobility"])
            tier_communication = int(raw["consensus_tier_communication"])
            reason = int(raw["recovery_reason"])
            eval_reasons = {slot: int(raw[col]) for slot, col in EVAL_COLUMNS.items()}
            rows.append(
                TickRow(
                    cycle=int(raw["cycle"]),
                    team_id=int(raw["team_id"]),
                    fault_active=fault_active,
                    detected_system=detected_classes_system(reason, tier_mobility, tier_communication),
                    detected_any=detected_classes_any(eval_reasons, tier_mobility, tier_communication),
                    eval_reasons=eval_reasons,
                    tier_mobility=tier_mobility,
                    tier_communication=tier_communication,
                    recommendation=int(raw["recovery_recommendation"]),
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
    episodes_action_taken: int = 0
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

    def action_taken_rate(self) -> Optional[float]:
        """Of the DETECTED episodes, how many led to an actual dispatched
        action (fused recovery_recommendation reaching RETURN_TO_SAFE_ZONE
        or higher) at or after the first detection."""
        if self.episodes_detected == 0:
            return None
        return self.episodes_action_taken / self.episodes_detected

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
    team_series: Dict[int, List[TickRow]],
    fault_class: str,
    detected_of,
    episode_filter=None,
) -> ClassMetrics:
    """detected_of(row) -> frozenset of classes that row's chosen detection
    notion (system or any-evaluator) flags."""
    metrics = ClassMetrics()
    for team_id, rows in team_series.items():
        rows = sorted(rows, key=lambda r: r.cycle)
        cycles = [r.cycle for r in rows]
        active = [r.fault_active[fault_class] for r in rows]
        by_cycle = {r.cycle: r for r in rows}

        metrics.team_ticks += len(rows)

        # False alarms: detected this class while it is NOT active.
        in_alarm = False
        alarm_len = 0
        for r in rows:
            is_false_alarm = (fault_class in detected_of(r)) and (r.fault_active[fault_class] == 0)
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
            detected = detected_of(r)
            if not detected:
                continue
            if fault_class not in detected:
                metrics.wrong_class_ticks += 1

        episodes = _episodes(active, cycles)
        for start, end in episodes:
            if episode_filter is not None and not episode_filter(team_id, start, end):
                continue
            metrics.episodes_total += 1
            first_hit = None
            for cycle in range(start, end + 1):
                row = by_cycle.get(cycle)
                if row is not None and fault_class in detected_of(row):
                    first_hit = cycle
                    break
            if first_hit is not None:
                metrics.episodes_detected += 1
                latency = first_hit - start
                metrics.latencies.append(latency)
                for window in DETECTION_WINDOWS:
                    if latency <= window:
                        metrics.detected_within[window] += 1

                for cycle in range(first_hit, end + 1):
                    row = by_cycle.get(cycle)
                    if row is not None and row.recommendation >= ACTION_RETURN_TO_SAFE_ZONE:
                        metrics.episodes_action_taken += 1
                        break
    return metrics


@dataclass
class RaceMetrics:
    """Per-class "who flagged the episode first" comparison between the
    peer-consensus evaluator and the matching local evaluator (mobility or
    communication) - only meaningful for the two classes consensus can
    ever vote on. Only counts episodes where the LOCAL evaluator's own
    verdict is being compared, not the fused decision, so this is a direct
    read of evaluate_peer_consensus() vs. evaluate_mobility()/evaluate_
    communication() on the exact same ground-truth episodes."""

    episodes_total: int = 0
    both_flagged: int = 0
    consensus_first: int = 0
    local_first: int = 0
    tie: int = 0
    consensus_only: int = 0
    local_only: int = 0
    neither: int = 0
    lead_ticks: List[int] = field(default_factory=list)  # local_first - consensus_first, when consensus_first

    def consensus_first_rate(self) -> Optional[float]:
        if self.both_flagged == 0:
            return None
        return self.consensus_first / self.both_flagged

    def mean_lead_ticks(self) -> Optional[float]:
        if not self.lead_ticks:
            return None
        return statistics.mean(self.lead_ticks)

    def median_lead_ticks(self) -> Optional[float]:
        if not self.lead_ticks:
            return None
        return statistics.median(self.lead_ticks)


def _compute_race_metrics(team_series: Dict[int, List[TickRow]], cls: str) -> RaceMetrics:
    local_slot = EVAL_MOBILITY if cls == "mobility" else EVAL_COMMUNICATION
    metrics = RaceMetrics()
    for team_id, rows in team_series.items():
        rows = sorted(rows, key=lambda r: r.cycle)
        cycles = [r.cycle for r in rows]
        active = [r.fault_active[cls] for r in rows]
        by_cycle = {r.cycle: r for r in rows}
        episodes = _episodes(active, cycles)
        for start, end in episodes:
            metrics.episodes_total += 1
            consensus_first_tick = None
            local_first_tick = None
            for cycle in range(start, end + 1):
                row = by_cycle.get(cycle)
                if row is None:
                    continue
                if consensus_first_tick is None:
                    tier = row.tier_mobility if cls == "mobility" else row.tier_communication
                    if row.eval_reasons.get(EVAL_PEER_CONSENSUS, REASON_NONE) == REASON_CONSENSUS_FAULT and tier == CONSENSUS_TIER_ELEVATED:
                        consensus_first_tick = cycle
                if local_first_tick is None:
                    if row.eval_reasons.get(local_slot, REASON_NONE) != REASON_NONE:
                        local_first_tick = cycle
                if consensus_first_tick is not None and local_first_tick is not None:
                    break

            if consensus_first_tick is not None and local_first_tick is not None:
                metrics.both_flagged += 1
                if consensus_first_tick < local_first_tick:
                    metrics.consensus_first += 1
                    metrics.lead_ticks.append(local_first_tick - consensus_first_tick)
                elif local_first_tick < consensus_first_tick:
                    metrics.local_first += 1
                else:
                    metrics.tie += 1
            elif consensus_first_tick is not None:
                metrics.consensus_only += 1
            elif local_first_tick is not None:
                metrics.local_only += 1
            else:
                metrics.neither += 1
    return metrics


def compute_run_metrics(rows: List[TickRow]):
    """Given every TickRow from one seed's run (all teams, all ticks),
    return (class_metrics, race_metrics):

    - class_metrics: dict of {(fault_class_key, detection_mode) ->
      ClassMetrics}, detection_mode in ("system", "any"). fault_class_key
      is one of FAULT_CLASSES, plus "mobility<=2"/"mobility>2"/
      "communication<=2"/"communication>2" splitting each class's episodes
      by how many teams had that class active at the episode's start tick.
    - race_metrics: dict of {"mobility"/"communication" -> RaceMetrics}.
    """
    team_series: Dict[int, List[TickRow]] = {}
    for row in rows:
        team_series.setdefault(row.team_id, []).append(row)

    # concurrency[class][cycle] = number of teams with that class active.
    concurrency: Dict[str, Dict[int, int]] = {cls: {} for cls in ("mobility", "communication")}
    for row in rows:
        for cls in ("mobility", "communication"):
            if row.fault_active[cls]:
                concurrency[cls][row.cycle] = concurrency[cls].get(row.cycle, 0) + 1

    detected_getters = {"system": lambda r: r.detected_system, "any": lambda r: r.detected_any}

    results: Dict[tuple, ClassMetrics] = {}
    for cls in FAULT_CLASSES:
        for mode, getter in detected_getters.items():
            results[(cls, mode)] = _compute_class_metrics(team_series, cls, getter)

    for cls in ("mobility", "communication"):
        for mode, getter in detected_getters.items():
            def le2(team_id, start, end, cls=cls):
                return concurrency[cls].get(start, 0) <= 2

            def gt2(team_id, start, end, cls=cls):
                return concurrency[cls].get(start, 0) > 2

            results[(f"{cls}<=2", mode)] = _compute_class_metrics(team_series, cls, getter, episode_filter=le2)
            results[(f"{cls}>2", mode)] = _compute_class_metrics(team_series, cls, getter, episode_filter=gt2)

    race_metrics = {cls: _compute_race_metrics(team_series, cls) for cls in ("mobility", "communication")}

    return results, race_metrics


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

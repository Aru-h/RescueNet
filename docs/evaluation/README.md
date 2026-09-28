# Consensus/AI evaluation harness

`make evaluate` (`laptop/tools/evaluate.py`) answers one question with real
simulator data: does peer consensus (and, later, the AI fault predictor)
actually improve fault detection and mission outcome over a rules-only
baseline? It builds each variant of the laptop `tron` binary, runs the same
seeds against every variant, computes per-fault-class detection/false-alarm/
mission-outcome metrics, and writes a paired per-seed comparison with
bootstrap confidence intervals. Results land in `build/evaluate/` (raw
per-seed CSVs, `summary_metrics.csv`, `paired_diff.csv`,
`per_seed_metrics.json`, `report.md`) and are copied to
`docs/evaluation/RESULTS.md`.

Does not change simulator behaviour: it only builds `tron` with different
pre-existing compile-time flags (`CONSENSUS_ENFORCE`, `FAULT_MODEL_LOADED`)
and reads the CSV/`--diag` output `tron` already produces, plus one small
*pure-logging* addition (`core/recovery_manager.h`'s
`recovery_manager_get_evaluator_verdict()` and the `eval_<name>_reason`/
`eval_<name>_confidence` CSV columns it feeds, laptop/snapshot.c) that
exposes each of the 7 recovery evaluators' own verdict per team per tick -
see "Two notions of detection" below for why that was necessary. Reading
these getters has no effect on `recovery_manager_step()`'s or
`recovery_dispatch()`'s own decisions.

## Running it

```
make evaluate                       # seeds 1001-1020, 3000 ticks, rules vs rules+consensus, fault-profile=random
make evaluate EVAL_PROFILES=random,combined
make evaluate EVAL_SEEDS_COUNT=5 EVAL_TICKS=500   # a quick look
```

Or call the script directly for more control (`--help` for everything):

```
python3 laptop/tools/evaluate.py --variants rules,rules+consensus \
    --seeds-start 1001 --seeds-count 20 --ticks 3000 --profiles random \
    --out-dir build/evaluate
```

Seeds 1-300 are the training range used elsewhere in this repo
(data_prep.py/train.py) and are refused outright unless you pass
`--force-training-seeds` for a deliberate methodology experiment - never for
the real report. The shipped exam/benchmark range is 1001-1020.

## Two notions of "detection", and why both are reported

See `laptop/tools/eval_metrics.py`'s module docstring for the full mapping
and rationale; summary:

`core/recovery_manager.c` fuses up to 7 evaluators (progress, stamina,
mobility, communication, equipment, the fault predictor, and, under
`CONSENSUS_ENFORCE`, peer consensus) into ONE
`RecoveryStatus_t{recommendation, reason, confidence}` per team per tick
(`recovery_select_primary_signal()` picks one winner). That fused
`recovery_reason` alone cannot answer "did the communication evaluator see
this fault" when faults overlap: with two classes active on the same team
the same tick, one evaluator's signal can occupy the fused slot while
another evaluator also genuinely fired - reading only the fused reason
would wrongly score the second one as a miss. (An earlier version of this
tool made exactly that mistake, and separately guessed a now-corrected
mechanism - `CONSENSUS_CAP_LOCAL_EVALUATORS` - for an observed regression;
that flag defaults to 0 and is not compiled in by the shipped `rules+
consensus` build, so it cannot explain anything measured here. Consensus
only ever *adds* a signal via `evaluate_peer_consensus()`; it never
suppresses a local evaluator's own verdict in the current build.)

So every fault class is reported under **two** detection notions:

1. **"system decision"** - the fused `recovery_reason` (what
   `recovery_dispatch()` actually acts on that tick), plus, when it is
   `CONSENSUS_FAULT`, `consensus_tier_mobility`/`consensus_tier_
   communication` to say which class consensus itself elevated.
2. **"any evaluator flagged"** - `core/recovery_manager.h`'s
   `recovery_manager_get_evaluator_verdict()` per-evaluator columns
   (`eval_<name>_reason`, one per evaluator, added specifically to resolve
   this ambiguity): true whenever *some* evaluator relevant to a class said
   anything other than CONTINUE_MONITORING/NONE, whether or not it won the
   fused decision. When consensus has both tiers ELEVATED the same tick,
   both mobility and communication are credited.

Known, load-bearing limitations of this mapping (not bugs in the tool -
properties of what `recovery_manager.c` actually logs today):

- **mobility** and **communication** have a clean 1:1 evaluator each
  (`evaluate_mobility()`/`evaluate_communication()`), and are the *only*
  two classes `core/consensus.c` ever votes on
  (`CONSENSUS_NUM_VOTED_CLASSES == 2`). Consensus's measurable contribution
  is therefore structurally limited to these two classes - it can never be
  credited with catching a resource, environment, or mission fault.
- **resource** is approximated by folding `evaluate_stamina()`'s
  `LOW_STAMINA` and `evaluate_equipment()`'s `EQUIPMENT_FAULT` together.
  `recovery_manager.c` itself documents that neither reason is
  class-exclusive; treat resource-class numbers as directional, not exact.
- **environment and mission have no evaluator at all** - no function in
  `recovery_manager.c` ever names either as the reason it acted. Mission's
  only nominal `RecoveryReason_t`, `MISSION_DELAY`, is defined in the enum
  but never actually returned anywhere in `recovery_manager.c` - it is dead
  code, not a live detector (an earlier version of this tool wrongly
  treated it as one; it never fires under either variant). We do not
  invent a proxy for either class (every remaining generic reason,
  `PROGRESS_DEVIATION`, is too broad to attribute specifically). Both will
  always show 0% detected in this tool's output. That is a genuine finding
  about `recovery_manager.c` as it stands, not a limitation of the
  evaluation tool - closing it means adding a real evaluator, which changes
  simulator behaviour and is out of this tool's scope.
- Ground truth is always the CSV's own `fault_active_<class>` columns,
  never something this tool infers.

Per-tick rows are filtered to `event_type == EVENT_TICK` (`core/mission_
state.h`'s `EventType_t`) - the unconditional, once-per-team-per-tick
snapshot row logged before any recovery dispatch runs that tick. Every
other event type shares a `(team_id, cycle)` with its team's `EVENT_TICK`
row and would double-count ticks if included.

## Metrics computed (per variant, per fault class, per detection mode)

- **fault episodes**: contiguous spans of `fault_active_<class> == 1` per
  team, and the % detected at all / within 5 / 10 / 20 ticks of the
  episode's start. Episodes are scored **once, by their first flagged
  tick** - a tick after that first hit is never re-scored as a new miss,
  including the quiet ticks several evaluators deliberately produce once a
  team is RESTING/heading_to_rest after being dispatched an action (see
  `evaluate_mobility()`/`evaluate_stamina()`/`evaluate_communication()`/
  `evaluate_equipment()`'s own comments on this in `recovery_manager.c`).
  `test_eval_metrics.py`'s `test_detection_survives_post_action_quiet_
  period` exercises this directly. A flag after the episode has already
  ended does not count as detecting that episode - it is still tracked as
  a false alarm, see below.
- **detection latency**: ticks from episode start to first flagged tick,
  median and p90 (over detected episodes only).
- **action taken**: of detected episodes, the fraction where the fused
  `recovery_recommendation` actually reached `RETURN_TO_SAFE_ZONE` or
  higher at or after the first detection - "detected" (some evaluator said
  something) and "acted on" (the team was actually dispatched somewhere)
  are different things, both reported.
- **false alarms**: ticks where the class is detected but not active,
  per 1,000 team-ticks, plus the mean length of contiguous false-alarm
  runs.
- **wrong-class detections**: ticks where the class is active but the
  detected class(es) are entirely disjoint from it (some other class was
  blamed instead), per 1,000 team-ticks.
- **mobility/communication concurrency split**: each episode is bucketed
  by how many teams had that class active at the episode's *start* tick
  (`<=2` vs `>2` concurrently-faulted teams), and every metric above is
  recomputed per bucket.
- **consensus vs. local evaluator race** (mobility/communication only):
  per episode, compares `evaluate_peer_consensus()`'s own first-flagged
  tick against the matching local evaluator's own first-flagged tick -
  `consensus_first_rate` (of episodes where both eventually flagged),
  `mean`/`median_lead_ticks` (how many ticks earlier consensus was, in
  those cases), and `consensus_only_rate`/`local_only_rate` (episodes only
  one of the two ever flagged at all).
- **mission outcome**: `incidents_resolved` and `stage_completions`, summed
  across all teams from the run's `--diag` stderr output
  (`[DIAG THROUGHPUT] team=... incidents_resolved=... stage_completions=...`,
  `laptop/main.c`). Per-victim rescue counts are not available from
  `--diag` without touching `laptop/debug.c`'s `print_world_state()`, which
  is out of scope for this tool (owned by the dashboard agent) - if that
  signal is needed later, log it from `core/incident.c`'s resolution path
  instead, the same way `g_diag_incidents_resolved` already is.

## Paired comparison

Both variants run the *same* seeds, so for every metric above we report,
per seed, `variant_value - baseline_value`, then the mean of those
per-seed differences and a 95% bootstrap CI (10,000 resamples, seeded
deterministically from a stable hash of `(profile, variant, fault_class,
metric)` - not from wall-clock time or Python's randomized string hashing,
so results are reproducible across re-runs and independent of
`--workers`). A difference is reported "real" when the CI excludes 0,
"noise" otherwise.

## Variants

Defined in `evaluate.py`'s `VARIANT_DEFINES` - add a new entry (and, if it
needs more than a `-D` flag list, extend `build_variant()`) to evaluate
another one:

| variant | build flags |
| --- | --- |
| `rules` | `-DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=0` (shadow mode: consensus computed but never acted on - same radio/vbus behaviour as `rules+consensus`, only consensus's *effect* differs) |
| `rules+consensus` | `-DVIRTUAL_BUS_ENABLED=1 -DCONSENSUS_ENFORCE=1` (shipped default) |
| `rules+consensus+ai` | see below |

### Adding the AI variant once a trained model exists

`core/fault_predictor.h`'s `FAULT_MODEL_LOADED` is now `#ifndef`-guarded
(previously an unconditional `#define ... 0`) so it can be overridden from
the command line without editing the header again. `core/fault_predictor.c`
itself still has an *empty* `#if FAULT_MODEL_LOADED` branch today - no
inference code exists yet, so simply flipping the flag would silently
produce the same all-zero scores as the baseline. `evaluate.py` therefore
refuses to build `rules+consensus+ai` at all until you pass
`--model-header <path>`, with a clear error rather than a silent zero-score
run.

Once a real model-to-C path lands (a separate task - `train.py` generating
a flat-weights C header), wiring this variant up the rest of the way is:

1. Have `train.py` emit a header `fault_predictor.c` can consume (weight
   arrays laid out however the model needs them).
2. Fill in `core/fault_predictor.c`'s `#if FAULT_MODEL_LOADED` branch to
   actually run inference using those weights.
3. Run `make evaluate EVAL_VARIANTS=rules,rules+consensus,rules+consensus+ai`
   with `laptop/tools/evaluate.py --model-header <path-to-generated-header>`
   (add an `EVAL_MODEL_HEADER` Makefile variable plumbed to `--model-header`
   if you want it available via `make evaluate` directly). `evaluate.py`
   already builds this variant with `-DVIRTUAL_BUS_ENABLED=1
   -DCONSENSUS_ENFORCE=1 -DFAULT_MODEL_LOADED=1 -include <header>` - no
   further changes to `evaluate.py` should be needed unless the header's
   symbol names need a different include mechanism.
4. Extend `eval_metrics.py`'s `detected_classes_system()`/`detected_
   classes_any()` to map `RECOVERY_REASON_PREDICTED_FAULT` (and the
   `eval_fault_predictor_reason` column, already logged) to the AI's
   per-class output (today deliberately left unmapped, since that reason
   can never fire while `FAULT_MODEL_LOADED` is 0) - likely by adding a
   per-class predictor score to the CSV rather than only the single
   collapsed reason, since `recovery_manager.c`'s comment on `evaluate_
   fault_predictor()` notes it already discards `fault_predictor_run()`'s
   5 independent per-class scores down to one `(confidence, reason)` pair.

## Reproducibility / determinism

`make check-evaluate-determinism` runs a tiny (2 seeds x 300 ticks)
evaluation once with `--workers 1` and once with `--workers 4` and asserts
`summary_metrics.csv`, `paired_diff.csv` and `report.md` are byte-identical.
This holds because: the simulator is a pure function of
`(seed, ticks, fault-profile, build flags)`; every aggregation step in
`evaluate.py` iterates variants/profiles/seeds/classes/metrics in a fixed,
sorted order rather than worker-completion order; and the bootstrap CIs use
a stable SHA-256-derived seed, not wall-clock time.

`make check-eval-metrics-unittest` runs `laptop/tools/test_eval_metrics.py`,
which checks the metric math itself (episode detection, latency, false
alarms, wrong-class detections, the concurrency split, mission-outcome
parsing) against hand-built sequences with known answers - no simulator
invocation.

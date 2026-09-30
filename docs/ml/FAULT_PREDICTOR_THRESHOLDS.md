# Fault predictor: final decision thresholds

## In use (closed-loop tuned)

`{ mobility 0.573, communication 0.965, resource 0.705, environment OFF, mission OFF }` (`core/fault_predictor.h`). Picked with `laptop/tools/ai_threshold_sweep.py` on dev seeds 2001-2008 and verified on held-out seeds 1001-1020; results in `AI_EVALUATION_BRIEF.md`. Environment and mission are disabled because, in the simulator, acting on them lowered incidents resolved even at threshold 0.95. The table below is the offline validation-set analysis (recall-leaning) they started from.

## Offline validation analysis

Model: 5 independent sigmoid outputs (mobility, communication, resource, environment, mission). One threshold per class, selected on the VALIDATION set. The model was also evaluated on a separate held-out test set that was not used for threshold selection. Precision/recall below are validation figures. The values live in `core/fault_predictor.h` (`FAULT_PREDICTOR_CLASS_THRESHOLDS`).

| Class | Threshold | Recall | Precision | Class base rate | Basis |
|---|---|---|---|---|---|
| mobility | 0.532 | 0.80 | 0.493 | 0.237 | recall-leaning |
| communication | 0.965 | 1.00 | 0.966 | - | best F1 |
| resource | 0.705 | 0.90 | 0.839 | - | best F1 |
| environment | 0.706 | 0.60 | 0.299 | 0.070 | recall-leaning |
| mission | 0.731 | 0.75 | 0.600 | 0.081 | recall-leaning |

## Principle

Recall is favored because of what happens after a detection, not because recall is a better metric. A false positive on a cheap action is absorbed by hysteresis, and one that persists only escalates past local recovery if the watchdog or peer consensus corroborates it. A missed real fault has no equivalent safety net. Floor: precision at the chosen point must stay meaningfully above the class base rate, otherwise the model adds no real information there. Precision was also checked for a gradual fall-off versus an elbow, stopping where the exchange rate of recall for precision got bad.

## Why each threshold

- mobility (0.532): precision declines smoothly from recall 0.60 to 0.90, with no elbow. Recall 0.80 gives precision 0.493, about 2.1x the 0.237 base rate.
- mission (0.731): the decline steepens gradually. Recall 0.75 gives precision 0.600, about 7.4x the 0.081 base rate.
- environment (0.706): precision falls slowly up to recall 0.45, then the drop roughly doubles between 0.45 and 0.55 and levels off near 0.60. Recall 0.60 costs only about 0.045 more precision from 0.45 and still gives precision 0.299, about 4.3x the 0.070 base rate. Environment faults are hard to see in team telemetry, so recall is worth more than usual.
- communication (0.965): best-F1 point.
- resource (0.705): best-F1 point.

## Limitations

- Environment is the weakest class. Its detectability relies largely on `env_raw_travel_cost` and zone features (from the mission-link map on the board). Precision is about 0.30, so most environment alerts are false. Tolerable only because the AI alone is capped at local recovery and escalation needs corroboration.
- Mission recall does not exceed the watchdog's best achievable recall; the model's advantage there is precision.
- Mobility mean lead time vs the watchdog is negative (median slightly positive).
- All data is synthetic simulator output.

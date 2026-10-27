# Final evaluation: results

> **Superseded in part by docs/HEADROOM_MAP.md (Step 5):** the stress-7 detector headroom (+1.75) and the shipped-AI planner gain (+1.23) did not replicate on 100 seeds (Step 5: +0.01 [-0.84, +0.85] and +0.14 [-0.47, +0.73]); the +1.68 (fixed gates + shipped AI) was not re-measured in Step 5 and is UNCONFIRMED. Figures and conclusions built on them are marked ‡ below; the original numbers are left unchanged.

AI rows: **shipped model (trained on the old fault model - context only)**.

```
date: 2026-10-24T16:43:32Z
commit: f63de4a6b4d56f72ebf323958bdc1876155a1234 clean
model_header: core/fault_predictor_weights.h (shipped)
thresholds: shipped
calib_header: core/planner_calibration.h (shipped)
extra_args: none (v1 world)
```

## Incidents resolved and victims lost per seed

| Stress | System | Incidents dev | held-out | pooled | Victims lost dev | held-out | pooled | Teams in service (of 10) |
|---|---|---|---|---|---|---|---|---|
| 1 | F fixed gates | 44.25 | 44.35 | 44.30 | 11.00 | 12.55 | 11.78 | 4.19 |
| 1 | P planner (no AI) | 44.25 | 44.65 | 44.45 | 11.90 | 12.25 | 12.07 | 4.16 |
| 1 | PD planner + detector oracle (ceiling) | 45.85 | 45.35 | 45.60 | 11.65 | 12.45 | 12.05 | 4.25 |
| 1 | FA fixed gates + AI * | 44.70 | 44.25 | 44.48 | 11.15 | 13.15 | 12.15 | 4.21 |
| 1 | PA planner + AI * | 44.45 | 43.70 | 44.08 | 11.75 | 11.30 | 11.53 | 4.16 |
| 2 | F fixed gates | 42.10 | 42.95 | 42.52 | 10.75 | 13.40 | 12.07 | 3.76 |
| 2 | P planner (no AI) | 42.40 | 42.25 | 42.33 | 12.00 | 12.60 | 12.30 | 3.77 |
| 2 | PD planner + detector oracle (ceiling) | 43.25 | 42.10 | 42.67 | 11.90 | 12.85 | 12.38 | 3.86 |
| 2 | FA fixed gates + AI * | 42.55 | 41.60 | 42.08 | 12.25 | 13.45 | 12.85 | 3.92 |
| 2 | PA planner + AI * | 41.70 | 43.25 | 42.48 | 11.40 | 13.50 | 12.45 | 3.81 |
| 3.5 | F fixed gates | 40.55 | 39.10 | 39.83 | 12.75 | 14.10 | 13.43 | 3.28 |
| 3.5 | P planner (no AI) | 39.95 | 40.50 | 40.23 | 12.00 | 14.65 | 13.32 | 3.32 |
| 3.5 | PD planner + detector oracle (ceiling) | 40.45 | 39.80 | 40.12 | 12.65 | 13.40 | 13.03 | 3.44 |
| 3.5 | FA fixed gates + AI * | 40.25 | 40.30 | 40.27 | 13.60 | 14.10 | 13.85 | 3.52 |
| 3.5 | PA planner + AI * | 40.80 | 39.75 | 40.27 | 12.95 | 15.55 | 14.25 | 3.34 |
| 7 | F fixed gates | 34.00 | 34.45 | 34.23 | 15.15 | 14.75 | 14.95 | 2.54 |
| 7 | P planner (no AI) | 33.30 | 33.20 | 33.25 | 15.60 | 15.20 | 15.40 | 2.58 |
| 7 | PD planner + detector oracle (ceiling) | 34.85 | 35.15 | 35.00 | 16.10 | 16.85 | 16.48 | 2.63 |
| 7 | FA fixed gates + AI * | 35.80 | 36.00 | 35.90 | 16.25 | 16.00 | 16.12 | 2.83 |
| 7 | PA planner + AI * | 35.05 | 33.90 | 34.48 | 14.65 | 15.45 | 15.05 | 2.63 |

\* shipped model (trained on the old fault model - context only).

Reference, no faults at all (fixed gates, `--fault-profile=none`): 47.40 incidents resolved per seed, victims lost not reported for this profile, teams in service 4.81 of 10. Added after the pre-registration as a reference row only.

## Teams in service (share of team-ticks)

Resting, returning to rest, faulted (mobility or resource fault active) and in service (working or available).

| Stress | System | Working | Available | Faulted | Returning | Resting | In service | Teams in service |
|---|---|---|---|---|---|---|---|---|
| 1 | F | 38.4% | 3.5% | 2.6% | 7.6% | 47.9% | 41.9% | 4.19 |
| 1 | P | 38.3% | 3.4% | 2.2% | 7.6% | 48.5% | 41.6% | 4.16 |
| 1 | PD | 38.6% | 3.8% | 2.0% | 7.4% | 48.1% | 42.5% | 4.25 |
| 1 | FA | 38.3% | 3.8% | 1.2% | 8.7% | 48.0% | 42.1% | 4.21 |
| 1 | PA | 38.2% | 3.4% | 2.2% | 7.6% | 48.6% | 41.6% | 4.16 |
| 2 | F | 34.3% | 3.3% | 4.1% | 9.2% | 49.1% | 37.6% | 3.76 |
| 2 | P | 34.7% | 3.0% | 3.6% | 9.0% | 49.7% | 37.7% | 3.77 |
| 2 | PD | 35.1% | 3.5% | 3.5% | 8.8% | 49.0% | 38.6% | 3.86 |
| 2 | FA | 35.7% | 3.6% | 2.2% | 10.0% | 48.6% | 39.2% | 3.92 |
| 2 | PA | 35.1% | 3.0% | 3.5% | 8.8% | 49.6% | 38.1% | 3.81 |
| 3.5 | F | 30.0% | 2.9% | 5.7% | 11.1% | 50.4% | 32.8% | 3.28 |
| 3.5 | P | 30.5% | 2.7% | 5.0% | 10.8% | 51.0% | 33.2% | 3.32 |
| 3.5 | PD | 31.1% | 3.3% | 4.6% | 10.6% | 50.5% | 34.4% | 3.44 |
| 3.5 | FA | 31.9% | 3.3% | 2.9% | 11.8% | 50.0% | 35.2% | 3.52 |
| 3.5 | PA | 30.6% | 2.8% | 5.0% | 10.6% | 50.9% | 33.4% | 3.34 |
| 7 | F | 23.0% | 2.4% | 7.4% | 14.2% | 53.0% | 25.4% | 2.54 |
| 7 | P | 23.5% | 2.3% | 7.1% | 14.0% | 53.2% | 25.8% | 2.58 |
| 7 | PD | 23.7% | 2.6% | 6.7% | 14.1% | 52.9% | 26.3% | 2.63 |
| 7 | FA | 25.2% | 3.1% | 4.3% | 14.6% | 52.8% | 28.3% | 2.83 |
| 7 | PA | 23.9% | 2.4% | 7.1% | 13.7% | 52.9% | 26.3% | 2.63 |
| 0 | F | 43.5% | 4.6% | 0.0% | 5.3% | 46.6% | 48.1% | 4.81 |

## Paired differences, incidents resolved per seed (95% bootstrap CI)

| Stress | Comparison | Dev | Held-out | Pooled |
|---|---|---|---|---|
| 1 | Detector headroom: PD minus P | +1.60 [+0.10, +3.15] | +0.70 [-0.65, +2.00] | **+1.15 [+0.15, +2.17]** |
| 1 | Planner minus fixed gates: P minus F | +0.00 [-1.90, +1.85] | +0.30 [-0.95, +1.50] | **+0.15 [-0.97, +1.25]** |
| 1 | Shipped-AI gain on fixed gates: FA minus F * | +0.45 [-1.25, +2.15] | -0.10 [-1.40, +1.05] | **+0.17 [-0.90, +1.25]** |
| 1 | Shipped-AI gain on planner: PA minus P * | +0.20 [-0.90, +1.30] | -0.95 [-1.90, -0.10] | **-0.38 [-1.10, +0.38]** |
| 2 | Detector headroom: PD minus P | +0.85 [-0.25, +1.90] | -0.15 [-1.50, +1.30] | **+0.35 [-0.53, +1.25]** |
| 2 | Planner minus fixed gates: P minus F | +0.30 [-1.10, +1.60] | -0.70 [-2.15, +0.75] | **-0.20 [-1.23, +0.80]** |
| 2 | Shipped-AI gain on fixed gates: FA minus F * | +0.45 [-1.35, +2.00] | -1.35 [-2.90, +0.15] | **-0.45 [-1.65, +0.70]** |
| 2 | Shipped-AI gain on planner: PA minus P * | -0.70 [-1.95, +0.55] | +1.00 [+0.00, +2.05] | **+0.15 [-0.72, +0.97]** |
| 3.5 | Detector headroom: PD minus P | +0.50 [-1.20, +2.20] | -0.70 [-2.15, +0.65] | **-0.10 [-1.23, +1.02]** |
| 3.5 | Planner minus fixed gates: P minus F | -0.60 [-2.00, +0.75] | +1.40 [+0.40, +2.40] | **+0.40 [-0.53, +1.30]** |
| 3.5 | Shipped-AI gain on fixed gates: FA minus F * | -0.30 [-1.90, +1.35] | +1.20 [-0.15, +2.70] | **+0.45 [-0.62, +1.57]** |
| 3.5 | Shipped-AI gain on planner: PA minus P * | +0.85 [-0.55, +2.30] | -0.75 [-2.05, +0.50] | **+0.05 [-0.93, +1.05]** |
| 7 | Detector headroom: PD minus P | +1.55 [-0.70, +3.60] | +1.95 [+0.60, +3.30] | **+1.75 [+0.47, +2.98]** ‡ |
| 7 | Planner minus fixed gates: P minus F | -0.70 [-2.50, +1.30] | -1.25 [-3.15, +0.40] | **-0.97 [-2.30, +0.38]** |
| 7 | Shipped-AI gain on fixed gates: FA minus F * | +1.80 [-0.35, +3.85] | +1.55 [-0.95, +3.50] | **+1.68 [+0.05, +3.12]** ‡ |
| 7 | Shipped-AI gain on planner: PA minus P * | +1.75 [+0.30, +3.10] | +0.70 [-0.65, +2.10] | **+1.23 [+0.20, +2.25]** ‡ |

*‡ Superseded in part by `docs/HEADROOM_MAP.md` (Step 5): the stress-7 detector headroom (+1.75) and the shipped-AI planner gain (+1.23) did not replicate on 100 seeds; the +1.68 was not re-measured and is UNCONFIRMED. Original numbers unchanged.*

## Paired differences, victims lost per seed (95% CI; no conclusion is drawn from one comparison alone)

| Stress | Comparison | Dev | Held-out | Pooled |
|---|---|---|---|---|
| 1 | Detector headroom: PD minus P | -0.25 [-1.70, +1.30] | +0.20 [-2.05, +2.50] | -0.03 [-1.38, +1.35] |
| 1 | Planner minus fixed gates: P minus F | +0.90 [-0.85, +2.70] | -0.30 [-2.05, +1.35] | +0.30 [-0.97, +1.52] |
| 1 | Shipped-AI gain on fixed gates: FA minus F * | +0.15 [-1.55, +2.05] | +0.60 [-1.60, +2.90] | +0.38 [-1.05, +1.88] |
| 1 | Shipped-AI gain on planner: PA minus P * | -0.15 [-1.40, +1.20] | -0.95 [-3.10, +1.10] | -0.55 [-1.80, +0.70] |
| 2 | Detector headroom: PD minus P | -0.10 [-2.05, +1.85] | +0.25 [-1.75, +2.20] | +0.07 [-1.35, +1.48] |
| 2 | Planner minus fixed gates: P minus F | +1.25 [-0.40, +2.90] | -0.80 [-3.60, +1.80] | +0.23 [-1.45, +1.75] |
| 2 | Shipped-AI gain on fixed gates: FA minus F * | +1.50 [-0.25, +3.35] | +0.05 [-1.85, +1.75] | +0.78 [-0.53, +2.08] |
| 2 | Shipped-AI gain on planner: PA minus P * | -0.60 [-2.15, +1.10] | +0.90 [-0.25, +2.10] | +0.15 [-0.85, +1.20] |
| 3.5 | Detector headroom: PD minus P | +0.65 [-1.30, +2.55] | -1.25 [-3.15, +0.65] | -0.30 [-1.68, +1.07] |
| 3.5 | Planner minus fixed gates: P minus F | -0.75 [-2.90, +1.45] | +0.55 [-1.60, +2.75] | -0.10 [-1.65, +1.52] |
| 3.5 | Shipped-AI gain on fixed gates: FA minus F * | +0.85 [-1.40, +3.10] | +0.00 [-2.15, +2.15] | +0.42 [-1.18, +2.00] |
| 3.5 | Shipped-AI gain on planner: PA minus P * | +0.95 [-0.75, +2.70] | +0.90 [-0.70, +2.70] | +0.93 [-0.25, +2.15] |
| 7 | Detector headroom: PD minus P | +0.50 [-2.20, +3.25] | +1.65 [-0.30, +3.80] | +1.07 [-0.65, +2.80] |
| 7 | Planner minus fixed gates: P minus F | +0.45 [-2.30, +3.20] | +0.45 [-1.95, +2.60] | +0.45 [-1.35, +2.20] |
| 7 | Shipped-AI gain on fixed gates: FA minus F * | +1.10 [-1.45, +3.85] | +1.25 [-1.45, +3.95] | +1.18 [-0.70, +3.12] |
| 7 | Shipped-AI gain on planner: PA minus P * | -0.95 [-2.90, +1.30] | +0.25 [-1.65, +2.10] | -0.35 [-1.80, +1.15] |

## Headroom against stress and against teams in service

| Stress | Teams in service (P) | Detector headroom (PD minus P) |
|---|---|---|
| 1 | 4.16 | +1.15 [+0.15, +2.17] |
| 2 | 3.77 | +0.35 [-0.53, +1.25] |
| 3.5 | 3.32 | -0.10 [-1.23, +1.02] |
| 7 | 2.58 | +1.75 [+0.47, +2.98] ‡ |

*‡ Superseded in part by `docs/HEADROOM_MAP.md` (Step 5): the stress-7 detector headroom (+1.75) and the shipped-AI planner gain (+1.23) did not replicate on 100 seeds; the +1.68 was not re-measured and is UNCONFIRMED. Original numbers unchanged.*

Spearman rank correlation of headroom with stress: +0.20; with teams in service: -0.20 (4 points; a descriptive summary, not a test). ‡ *(Superseded in part by `docs/HEADROOM_MAP.md`, Step 5: not replicated on 100 seeds; the +1.68 is unconfirmed. See the banner.)*

## How much of the loss to faults a perfect detector recovers (descriptive, not pre-registered)

Incidents resolved per seed: no-fault reference (fixed gates) minus the planner, and the detector headroom as a share of that gap. Caveat: the reference is `--fault-profile=none`, the original world without the causal profile's roads, aftershocks and route damage, so part of this gap is not caused by operational faults.

| Stress | No faults | Planner P | Loss to faults | Detector headroom PD minus P | Share of the loss recovered |
|---|---|---|---|---|---|
| 1 | 47.40 | 44.45 | 2.95 | +1.15 | 39% |
| 2 | 47.40 | 42.33 | 5.07 | +0.35 | 7% |
| 3.5 | 47.40 | 40.23 | 7.17 | -0.10 | -1% |
| 7 | 47.40 | 33.25 | 14.15 | +1.75 | 12% ‡ |

*‡ Superseded in part by `docs/HEADROOM_MAP.md` (Step 5): the stress-7 detector headroom (+1.75) and the shipped-AI planner gain (+1.23) did not replicate on 100 seeds; the +1.68 was not re-measured and is UNCONFIRMED. Original numbers unchanged.*

## Fault exposure by class (fixed gates F; share of team-ticks with the fault active, development and held-out averaged)

| Stress | mobility | communication | resource | environment (route-touch label) | mission |
|---|---|---|---|---|---|
| 1 | 6.9% | 1.3% | 3.5% | 1.9% | 0.0% |
| 2 | 11.8% | 1.2% | 6.0% | 3.6% | 0.1% |
| 3.5 | 17.3% | 1.2% | 9.4% | 5.6% | 0.0% |
| 7 | 26.2% | 1.0% | 14.8% | 8.7% | 0.0% |

## What ends a fault (fixed gates F, mean per seed over all seeds)

| Stress | Class | Onsets | Base repair (go-home) | Field repair | Natural recovery | Still active |
|---|---|---|---|---|---|---|
| 1 | mobility | 30.7 | 28.1 | 0.0 | 1.9 | 0.7 |
| 1 | resource | 22.2 | 21.1 | 0.0 | 0.7 | 0.4 |
| 2 | mobility | 52.2 | 47.3 | 0.0 | 3.4 | 1.4 |
| 2 | resource | 41.8 | 39.4 | 0.1 | 1.7 | 0.6 |
| 3.5 | mobility | 74.8 | 68.1 | 0.1 | 4.5 | 2.1 |
| 3.5 | resource | 65.1 | 61.3 | 0.1 | 2.7 | 1.0 |
| 7 | mobility | 110.5 | 100.5 | 0.2 | 7.1 | 2.7 |
| 7 | resource | 108.3 | 102.2 | 0.2 | 4.2 | 1.6 |


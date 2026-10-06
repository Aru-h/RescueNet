# Persistent faults: results (Step 3.6)

Pre-registration (parameters, comparisons, predictions): `docs/PERSISTENT_FAULTS_PROPOSAL.md` section 9,
committed as `e0dd4a9` before any planner or AI comparison was run. No parameter changed afterwards. The
frozen binaries are the implementation commit `ac63040` (the later commit `1a47a1e` only adds a check, a
report tool and a no-op `(void)` cleanup; I checked the rebuilt binary reproduces the frozen one on two runs).

Setup: causal profile, roads on, all causal fixes on, 3000 ticks, 20 seeds per set. **Development seeds
1001-1020, held-out seeds 3001-3020, pooled 40.** Differences are paired by seed, 95% bootstrap intervals.
**Stress 2 is the realistic headline** (its MTBF matches the 6-20 h field-robot literature); **stress 7 is the
harsh-conditions stress test**; both are co-primary and always shown together. Victims lost: no conclusion is
drawn from one set alone.

## Headline

1. **A perfect detector is worth something at stress 7, and about nothing at stress 2.** Planner with the
   oracle fault probability minus planner without it: stress 2 **+0.35 [-0.53, +1.25]** incidents per seed
   pooled (about 0); stress 7 **+1.75 [+0.47, +2.98]** (interval excludes 0; dev +1.55, held-out +1.95). So the
   "perfect detector adds about 0" finding of Step 3.5 does not carry over to a world where faults persist,
   but only the harsh stress shows it.
2. **A perfect forecaster is worth about nothing**, with either preventive option or both. Hazard oracle minus
   planner: PREVENT (rest at base) -0.10 [-0.50, +0.35] at stress 2 and +0.40 [-0.42, +1.23] at stress 7;
   PREVENT_FIELD exactly 0.00 at both (it was never chosen); both options together equal PREVENT alone.
3. **The planner does not beat the fixed gates.** Planner (no AI) minus fixed gates: stress 2 -0.20
   [-1.23, +0.80], stress 7 -0.97 [-2.30, +0.38].
4. **Making faults persist costs a lot at stress 7 and a little at stress 2.** Fixed gates, Step 3.5 world to v1
   world: stress 7 **-9.10 [-10.28, -7.95]** incidents per seed (43.33 to 34.23), stress 2 **-2.90 [-3.80, -2.00]**
   (45.42 to 42.52).
5. **Field repair almost never completes, so it changes nothing** (section "Field repair"). The fixed gates send a
   faulted team home within a few ticks, which cancels the repair.
6. **The shipped AI, evaluated as context, now helps at stress 7**: fixed gates + AI minus fixed gates +1.68
   [+0.05, +3.12]; planner + AI minus planner +1.23 [+0.20, +2.25]. At stress 2 it is about 0 (-0.45, +0.15).
   It was trained on the old timer model and not retrained, so this is a distribution-shift context number.

## Main table (incidents resolved per seed; victims lost in the last three columns)

| Stress | Condition | Dev | Held-out | Pooled | Dev lost | Held lost | Pooled lost |
|---|---|---|---|---|---|---|---|
| 2 | F fixed gates, Step 3.5 world (`persist` off) | 46.10 | 44.75 | 45.42 | 10.55 | 13.25 | 11.90 |
| 2 | F + `persist` timer | 42.25 | 42.35 | 42.30 | 12.65 | 12.50 | 12.57 |
| 2 | F + `persist` + field repair | 42.10 | 42.95 | 42.52 | 10.75 | 13.40 | 12.07 |
| 2 | **F v1 world** (= + semantics) | 42.10 | 42.95 | 42.52 | 10.75 | 13.40 | 12.07 |
| 2 | **P** planner, no AI | 42.40 | 42.25 | 42.33 | 12.00 | 12.60 | 12.30 |
| 2 | P-det (detector oracle) | 43.25 | 42.10 | 42.67 | 11.90 | 12.85 | 12.38 |
| 2 | P-haz[rest] | 42.25 | 42.20 | 42.23 | 11.85 | 12.55 | 12.20 |
| 2 | P-haz[field] | 42.40 | 42.25 | 42.33 | 12.00 | 12.60 | 12.30 |
| 2 | P-haz[both] | 42.25 | 42.20 | 42.23 | 11.85 | 12.55 | 12.20 |
| 2 | P-both | 42.20 | 41.75 | 41.98 | 12.70 | 13.20 | 12.95 |
| 2 | F + AI (context) | 42.55 | 41.60 | 42.08 | 12.25 | 13.45 | 12.85 |
| 2 | P + AI (context) | 41.70 | 43.25 | 42.48 | 11.40 | 13.50 | 12.45 |
| 7 | F, Step 3.5 world | 43.60 | 43.05 | 43.33 | 13.80 | 13.35 | 13.57 |
| 7 | F + `persist` timer | 33.55 | 35.30 | 34.42 | 15.40 | 17.15 | 16.27 |
| 7 | F + `persist` + field repair | 34.00 | 34.45 | 34.23 | 15.15 | 14.75 | 14.95 |
| 7 | **F v1 world** | 34.00 | 34.45 | 34.23 | 15.15 | 14.75 | 14.95 |
| 7 | **P** planner, no AI | 33.30 | 33.20 | 33.25 | 15.60 | 15.20 | 15.40 |
| 7 | P-det | 34.85 | 35.15 | 35.00 | 16.10 | 16.85 | 16.48 |
| 7 | P-haz[rest] | 33.15 | 34.15 | 33.65 | 15.75 | 13.35 | 14.55 |
| 7 | P-haz[field] | 33.30 | 33.20 | 33.25 | 15.60 | 15.20 | 15.40 |
| 7 | P-haz[both] | 33.15 | 34.15 | 33.65 | 15.75 | 13.35 | 14.55 |
| 7 | P-both | 34.65 | 35.35 | 35.00 | 15.45 | 16.35 | 15.90 |
| 7 | F + AI (context) | 35.80 | 36.00 | 35.90 | 16.25 | 16.00 | 16.12 |
| 7 | P + AI (context) | 35.05 | 33.90 | 34.48 | 14.65 | 15.45 | 15.05 |

Paired differences, pooled (95% CI):

| Comparison | Stress 2 incidents | Stress 7 incidents | Stress 2 victims lost | Stress 7 victims lost |
|---|---|---|---|---|
| F v1 minus F Step 3.5 world | -2.90 [-3.80, -2.00] | -9.10 [-10.28, -7.95] | +0.17 [-1.05, +1.40] | +1.38 [+0.03, +2.77] |
| `persist` timer minus Step 3.5 world | -3.12 [-4.12, -2.17] | -8.90 [-10.00, -7.80] | +0.68 [-0.68, +2.05] | +2.70 [+1.25, +4.25] |
| field repair on (timer to on) | +0.23 [-0.68, +1.18] | -0.20 [-1.00, +0.60] | -0.50 [-1.85, +0.85] | -1.32 [-2.70, +0.10] |
| semantics (on to v1) | 0.00 | 0.00 | 0.00 | 0.00 |
| P minus F | -0.20 [-1.23, +0.80] | -0.97 [-2.30, +0.38] | +0.23 [-1.45, +1.75] | +0.45 [-1.35, +2.20] |
| **P-det minus P** (detector headroom) | **+0.35 [-0.53, +1.25]** | **+1.75 [+0.47, +2.98]** | +0.07 [-1.35, +1.48] | +1.07 [-0.65, +2.80] |
| **P-haz[rest] minus P** | -0.10 [-0.50, +0.35] | +0.40 [-0.42, +1.23] | -0.10 [-0.60, +0.38] | -0.85 [-1.73, +0.00] |
| **P-haz[field] minus P** | 0.00 | 0.00 | 0.00 | 0.00 |
| **P-haz[both] minus P** | -0.10 [-0.50, +0.35] | +0.40 [-0.42, +1.23] | -0.10 [-0.60, +0.38] | -0.85 [-1.73, +0.00] |
| P-both minus P | -0.35 [-1.43, +0.72] | +1.75 [+0.47, +3.05] | +0.65 [-1.00, +2.35] | +0.50 [-1.05, +2.10] |
| F + AI minus F | -0.45 [-1.65, +0.70] | +1.68 [+0.05, +3.12] | +0.78 [-0.53, +2.08] | +1.18 [-0.70, +3.12] |
| P + AI minus P | +0.15 [-0.72, +0.97] | +1.23 [+0.20, +2.25] | +0.15 [-0.85, +1.20] | -0.35 [-1.80, +1.15] |
| Planner, Step 3.5 world minus F Step 3.5 world | +0.30 [-0.65, +1.25] | -0.42 [-1.18, +0.33] | +0.05 [-1.10, +1.20] | +0.03 [-1.10, +1.15] |

Notes on the table:
* "Semantics" is exactly 0 everywhere because the fixed gates (AI off) never issue ABORT_MISSION or REQUEST_TASK_REALLOCATION
  in this world (0 releases per seed), so the two action fixes were not exercised. With the shipped AI on, REALLOC releases
  occur (0.5 per seed at stress 2, 3.0 at stress 7).
* Dev and held-out agree in sign for the detector headroom at stress 7 (+1.55, +1.95) and for the persistence cost.
* The hazard-oracle victims-lost reduction at stress 7 held-out (-1.85 [-3.10, -0.75]) is one set; pooled it is -0.85
  [-1.73, 0.00]. I draw no conclusion (as pre-registered).

## What ends a fault (mean per seed, 40 seeds pooled)

Base repair = the team rested at a safe zone; field repair = a completed field repair; natural = the 10.8 h tail ran
out. "Still active" = faults not ended by the end of the run. Active share = share of team-ticks with the fault active.

| Stress | Config | Class | Onsets | Base repair | Field repair | Natural | Still active | Escalations | Active share | Mean length, base repair (ticks) |
|---|---|---|---|---|---|---|---|---|---|---|
| 2 | F, Step 3.5 world | mobility | 65.0 | 26.6 | 0 | 38.2 (timer) | 0.3 | 0 | 1.6% | 4.3 |
| 2 | F, Step 3.5 world | resource | 46.5 | 10.0 | 0 | 36.4 (timer) | 0.1 | 0 | 1.3% | 1.9 |
| 2 | **F v1** | mobility | 52.2 | 47.3 | 0.0 | 3.4 | 1.4 | 9.6 | **11.8%** | 69.1 |
| 2 | **F v1** | resource | 41.8 | 39.4 | 0.1 | 1.7 | 0.6 | 8.6 | **6.0%** | 39.8 |
| 7 | F, Step 3.5 world | mobility | 169.5 | 70.3 | 0 | 98.8 (timer) | 0.4 | 0 | 4.3% | 4.3 |
| 7 | F, Step 3.5 world | resource | 152.2 | 36.7 | 0 | 115.3 (timer) | 0.1 | 0 | 4.1% | 2.1 |
| 7 | **F v1** | mobility | 110.5 | 100.5 | 0.2 | 7.1 | 2.7 | 39.8 | **26.2%** | 72.6 |
| 7 | **F v1** | resource | 108.3 | 102.2 | 0.2 | 4.2 | 1.6 | 43.0 | **14.8%** | 38.7 |

Planner, detector oracle and the hazard-oracle rows are within a few tenths of F v1 in every column (section tables in
`laptop/tools/persist_report.py` output): faults end by base repair in about 90% of cases at both stress points, by natural
recovery in 3-7%, by field repair in under 0.3%.

**What this means.** A team-level response changes a fault's outcome now: with persistence an unremedied fault would last
10.8 h on average, and instead 90% are ended by a go-home that takes about 70 ticks (mobility) or 39 ticks (resource). But
it is still the same response regardless of which detector asks for it: the rule gates already confirm and act on a
persistent fault within a few ticks, so a perfect detector adds only what the gates miss or delay (+1.75 at stress 7; I did not measure the gates'
miss rate or detection delay separately, so I cannot say which), and a perfect forecaster adds nothing, which I read as rest
costing more than the faults it prevents (it is the expected-cost comparison the planner makes; I did not measure the
counterfactual).

## Field repair

At stress 7, per seed under the fixed gates: 134.7 mobility field repairs started, **0.2 completed, 0.0 failed, 134.3
cancelled**; resource 25.7 started, 0.2 completed. At stress 2: 71.3 started, 0 completed. The cancellation is the rule
gates sending the team home (`heading_to_rest` is set a few ticks after LOCAL_RECOVERY, verified by logging the first
cancellations): the recovery ladder escalates from LOCAL_RECOVERY to RETURN_TO_SAFE_ZONE faster than the 34-102 tick
repair can finish. The planner with the detector oracle chooses a field repair 7.7 times per seed at stress 7 (2.6 at
stress 2) and the rule floor cancels those too (1.7 of 1.8 cancelled). So **the field-repair mechanism is implemented and
unit-tested but is not exercised in the measured system**; nothing here says anything about whether field repair would
help if the gates held a repair in progress. That would be a change to the fixed gates (hold LOCAL_RECOVERY while a repair
runs), not made here.

## Preventive options

PREVENT (rest at base) was chosen 13.8 times per seed at stress 2 and 8.8 at stress 7 (pre-registered: under 2), with no
measurable effect (-0.10 and +0.40). PREVENT_FIELD was chosen 0 times: its benefit never exceeded 1.25 times its cost
(about 68 team-ticks of task delay). The planner took more rest trips than I predicted, but they bought nothing.

## Predictions: did they hold?

| # | Prediction (pre-registered) | Result | Held? |
|---|---|---|---|
| 1 | v1 minus Step 3.5 world, fixed gates: -6 to -9 at stress 7, -2 to -4 at stress 2 | -9.10 and -2.90 | **yes** (stress 7 at the edge) |
| 2 | Fault share of team-ticks 25-40% at stress 7, 10-20% at stress 2 | mobility 26.2% / 11.8% (in range); resource 14.8% / 6.0% (below) | **partly** (mobility yes, resource lower) |
| 3 | `repair` alone on the fixed gates: 0 to +2 at stress 7, about 0 at stress 2 | -0.20 [-1.00, +0.60] and +0.23 [-0.68, +1.18] | **no at stress 7** (about 0, wrong sign), yes at stress 2; explained by repairs not completing |
| 4 | P minus F: +1 to +3 at stress 7, 0 to +1 at stress 2 | -0.97 [-2.30, +0.38] and -0.20 [-1.23, +0.80] | **no** |
| 5 | Detector headroom: +0.5 to +2 at stress 7, about 0 at stress 2 | +1.75 [+0.47, +2.98] and +0.35 [-0.53, +1.25] | **yes** |
| 6a | P-haz[rest] about 0 (at most +1) | +0.40 [-0.42, +1.23] and -0.10 [-0.50, +0.35] | **yes** |
| 6b | P-haz[field] about 0 | exactly 0 (never chosen) | **yes** |
| 6c | P-haz[both] about 0, not above the larger of 6a and 6b by more than noise | +0.40 and -0.10 (equal to 6a) | **yes** |
| 7 | P-both about the sum of 5 and 6, dominated by 5 | +1.75 [+0.47, +3.05] at stress 7 (= detector headroom); -0.35 at stress 2 (predicted about +0.35) | **yes at stress 7**; at stress 2 consistent with about 0 |
| 8 | Hazard headroom <= detector headroom | 0.40 <= 1.75 and -0.10 <= 0.35 | **yes** |
| 8b | Under 2 PREVENT / PREVENT_FIELD per seed | PREVENT 13.8 and 8.8; PREVENT_FIELD 0 | **no** for PREVENT, yes for PREVENT_FIELD |
| 9 | Planner + AI minus P about 0 or negative | +1.23 [+0.20, +2.25] at stress 7; +0.15 at stress 2 | **no at stress 7** (the AI helps), yes at stress 2 |
| 10 | Victims lost: no conclusion | all pooled intervals include 0 except F v1 vs Step 3.5 world at stress 7 (+1.38 [+0.03, +2.77]) and the timer-only step (+2.70 [+1.25, +4.25]) | yes (the exceptions are the persistence cost itself) |
| 11 | Most faults end by base repair; natural under 10% | base repair 90-95%, natural 3-7% | **yes** |

Not predicted: the shipped AI helping at stress 7; the planner taking 9-14 rest trips per seed for no gain.

## Reading this plainly

* If a perfect detector adds anything it is at the harsh stress, and it is +1.75 incidents per seed out of ~35 (5%),
  with an interval that begins at +0.47. At the realistic stress it is +0.35 [-0.53, +1.25].
* A perfect forecaster, with a rest option or an in-field maintenance option, adds nothing measurable at either stress.
* The planner is not better than the fixed gates in any version of the world tested.
* Per the pre-registration, the planner kept its Step 3.5 V and margin and its Step 3.5 calibration tables; I did not retune it.
  The planner's cost model was extended only as the pre-registration allowed (persistent-fault costs, field repair, PREVENT).
  A planner that was designed and tuned for this world might do differently; this result is about the Step 3.5 planner.

## Robustness (reported, never used to choose a value)

Each persistence parameter was changed one at a time (natural-recovery mean 542 / 2168 ticks instead of 1084;
field-repair time x0.5 / x2; field-repair success 0.5 / 0.8 instead of 2/3; escalation multiplier 0.5 / 2 instead of
1.5; a one-tick field dwell), with fixed gates (`rf_*`) and the planner without the AI (`rp_*`) at stress 2 and
stress 7, on development seeds 1001-1020 and held-out seeds 3001-3020 (72 runs). These runs are reported only. No value
in the main configuration was chosen or changed because of them.

Planner minus fixed gates, incidents resolved per seed, pooled over 40 seeds (paired bootstrap 95% CI):

| Variant | Stress 2 | Stress 7 |
|---|---|---|
| natural 542 | -0.33 [-1.52, +0.88] | +0.42 [-0.75, +1.65] |
| natural 2168 | +0.15 [-0.75, +1.05] | -0.45 [-1.90, +1.02] |
| field time x0.5 | -0.42 [-1.40, +0.53] | +0.12 [-1.32, +1.60] |
| field time x2 | -0.28 [-1.27, +0.72] | -0.82 [-2.17, +0.55] |
| success 0.5 | -0.20 [-1.23, +0.80] | -0.68 [-2.05, +0.70] |
| success 0.8 | -0.25 [-1.23, +0.72] | -0.20 [-1.55, +1.20] |
| escalation x0.5 | +0.68 [-0.15, +1.50] | +0.25 [-0.88, +1.38] |
| escalation x2 | -0.80 [-1.90, +0.33] | +0.28 [-1.02, +1.55] |
| field dwell 1 tick | -0.30 [-1.48, +0.88] | -0.25 [-1.40, +0.93] |

Victims lost per seed, pooled, planner minus fixed gates: every one of the 18 intervals includes zero (point estimates
between -1.07 and +0.55 victims per seed).

Reading: in all 18 variant/stress cells the pooled interval for planner against fixed gates includes zero, for
incidents resolved and for victims lost. The sign of the point estimate changes between variants, so there is no
parameter direction in which the planner reliably helps or hurts. A few single-set intervals exclude zero (held-out
only: stress 2 field time x0.5, -1.30 [-2.45, -0.20], and escalation x2, -1.70 [-3.30, -0.10]); they do not appear in
the pooled set or on the development seeds, and with 36 dev/held-out cells per outcome a couple of such cases are
expected by chance, so I do not read them as an effect. The main-table conclusion (the planner is not distinguishable
from fixed gates under this fault model) is unchanged by every one-at-a-time perturbation tested. This does not show
the two are equal, only that 40 seeds cannot separate them. Hazard-oracle headroom was not rerun per variant.

## Checks

`--causal-persist=off` reproduces the Step 3.2 and 3.5 builds byte for byte (dataset and sidecar, with and without the
planner and actions); C1 md5 `b248bea72ce2ceb81389762dccc0a749`, all six other profiles' datasets and `--diag` stderr unchanged.
New: `check-causal-persistence` (flags gated, runs reproducible, sidecar columns only with persistence, ABORT distinct from
RETURN, REALLOC releases the task and keeps progress, exclusions hold, a field repair holds the team, forecast in range and a
rest never raises it). Existing targets re-run: check-evaluate-determinism, check-live-override-noop-regression,
check-consensus-shadow-regression, check-causal-profile, -fixes, -env-no-rtb, -roads-connectivity,
check-causal-fault-exposure (now persistence-aware), check-recovery-planner, check-selftests. ARM `check-board*`
cannot run here (no ARM toolchain); every changed file compiles clean with the host compiler and
`-DENVIRONMENT_HAS_WORLD_STORAGE=0 -Wall -Wextra -Wpedantic`.

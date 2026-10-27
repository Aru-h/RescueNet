# Step 3.7: letting the rule gates hold a field repair (causal profile only)

Status: **specification and pre-registration, written before any code for this step and before any comparison run.**
Results are appended to this file in a later commit. Everything here is under `--fault-profile=causal` with
`--causal-persist=on`; every other profile and the persistence modes `off` / `timer` are untouched.

## 1. The problem (from the Step 3.6 report)

Under the fixed gates at stress 7, 134.7 mobility field repairs were started per seed and 0.2 completed, because the
recovery ladder escalates from LOCAL_RECOVERY to RETURN_TO_SAFE_ZONE within a few ticks and the go-home cancels the
repair (34 to 102 ticks, mean 68). The field-repair mechanism therefore exists but is not exercised in the measured
system. This step lets the gates leave a repair alone unless something is genuinely worse, and measures the effect.

## 2. The change: token `hold-repair`

New `--causal-fixes` token `hold-repair` (`CAUSAL_FIX_HOLD_REPAIR = 0x20u`).

* It is **not** part of `CAUSAL_FIXES_ALL` and not part of `--causal-fixes=on` / the default. The v1 world as frozen in
  Step 3.6 therefore reproduces byte for byte unless the token is named explicitly. (If the adoption rule in section 5
  adopts it, scripts pass it explicitly; the defaults still do not change.)
* It has an effect only with `--causal-persist=on` (the only mode that has field repair). Given with another persistence
  mode, `--causal-fixes` rejects it.
* No new numeric parameter is introduced. The only quantities involved are the Step 3.6 field-repair length (34 to 102
  ticks, which bounds any hold), the existing evaluator thresholds, and the existing stamina and equipment rules.

### Rules

**H1. Cap the rule-based recommendation while a repair runs.** While a mobility or resource field repair (kinds 1 and 2;
not the 68-tick maintenance of PREVENT_FIELD) is in progress on a team, the rule-based recommendation is capped at
LOCAL_RECOVERY unless a worse condition (H1a) holds this tick.

* "Rule-based recommendation" = `g_recovery_state[team]` in fixed-gate mode, and `g_rule_state[team]` (the Simplex
  floor) in planner mode. The cap is applied to the recommendation state, not only to `recovery_dispatch()`, so the
  action logged in the dataset and snapshots is the action that is actually taken. Without this the dataset would say
  RETURN while the team is held, and a RETURN latched in the state would fire the tick the repair succeeds.
* The cap is applied after the existing hysteresis; the hysteresis code itself is unchanged.
* **The planner's own choice may still end a repair.** The planner owns the verdict above the floor; if it chooses GO HOME
  on cost while a repair is running, that decision stands (changing it would change the planner design, which is out of
  bounds). Those cancellations are counted separately (section 4).
* The AI recommendation, when an AI is present, is one input to the same recommendation state and is capped like the
  rest. This is the gates' policy, not the AI's.

**H1a. Worse conditions (observable signals only, no injector ground truth).** The cap is lifted for this tick if any of:

1. *Stamina*: the rule-based stamina evaluator's signal for this team is RETURN_TO_SAFE_ZONE or stronger (the existing
   logic: `degraded`, stamina at or below `RECOVERY_MANAGER_STAMINA_CRITICAL_THRESHOLD`, or stamina evaluator confidence at
   its RETURN floor).
2. *Other repairable class*: for a mobility repair, the equipment evaluator's signal is RETURN or stronger; for a resource
   repair, the mobility evaluator's signal is RETURN or stronger. These are the two classes a go-home actually remedies, so
   a second one appearing is the case where going home is better than staying.

Deliberately not triggers: the repaired class's own evaluator (it is the fault being repaired), the progress evaluator
(a held team makes no progress by construction), the communication evaluator (a go-home does not remedy it), peer
consensus, and the AI. A severity step-up of the repaired class is **not** used: it can only be read from the injector's
ground truth, and a team standing still produces no observable stall difference. The bounded repair length limits the
exposure instead: a held team is committed for at most 102 ticks.

**H2. One attempt per fault episode.** With the token, `fault_injector_causal_field_start()` refuses a second attempt on
the same fault episode (the flag is cleared when the fault is cleared by any cause). Without this, a failed repair
(probability 1/3) leaves a capped LOCAL recommendation that restarts a repair the next tick, which makes eventual success
almost certain and removes the go-home. A failed attempt is read as "this fault is not field-repairable"; after it the
cap lifts for that team and the ladder climbs to RETURN normally.

**H3. Reset after a successful repair.** On the tick a repair succeeds, that team's recovery state (`g_recovery_state`
and `g_rule_state`) is reset to CONTINUE_MONITORING, the same reset an assignment change gets, so a stale escalation
cannot send a repaired team home. After a failed attempt nothing is reset.

**What is not changed:** hysteresis, evaluators, floors, persistence counts, cooldowns, the planner (its V, margin,
calibration tables and cost model), `RecoveryAction_t`, the dataset schema and wire formats. The recorded action column
in the dataset changes only for held ticks (it shows the capped action).

### Counters (all statistics-only, `--diag` and the sidecar are the only places they appear)

* Hold: team-ticks held under the token; recommendations capped, by original action (RETURN, REALLOC, ABORT); ticks the
  cap was lifted by each trigger (stamina, other repairable class); attempts refused by H2; resets by H3.
* Field repairs by outcome (existing): started, completed, failed, cancelled; **cancelled by cause**: (1) stamina trigger,
  (2) other-class trigger, (3) planner choice above the floor, (4) task released or resolved while the team was not sent
  home, (5) the fault ended by another route while the repair ran, (6) other / unattributed.

## 3. Mechanism check (before any comparison; not a comparison)

The long-hold path has never run: in Step 3.6 holds lasted a few ticks before cancellation. After implementing and
before running anything in section 4 I run **one seed outside every evaluation and training range (seed 9001, then 9002
if needed), stress 7, fixed gates**, and read only:

1. no hold-caused trigger: that a worse-condition trigger or a cancellation is not produced by the hold itself (the
   watchdog 4-tick stall window, the 16-slot hold ring, the progress twin's accumulated deficit);
2. nothing sends a team home on the tick after a successful repair because of a deficit built during the hold;
3. how often stamina or equipment drain during a long hold trips the stamina trigger (the likely reason repairs would still
   not complete; predicted in section 6).

I do not read incidents resolved or victims lost on these seeds. **Deviation rule:** a fix that makes the code match this
specification is allowed and is logged with its commit; a change motivated by an outcome (incidents, victims, completion
rate) is not allowed. If the mechanism check shows the specification itself cannot work as written, I stop and report that
instead of redesigning.

## 4. Comparisons (all AI off)

`hold-repair` off vs on, for **fixed gates** and for the **planner** (`--recovery=utility --planner-p=watchdog`), at
**stress 2 and stress 7**; development seeds 1001-1020, held-out 3001-3020, pooled 40; paired by seed; paired bootstrap
95% intervals (10000 resamples, seed 0, `laptop/tools/planner_report.py`). 16 runs (2 systems x 2 settings x 2 stress x 2
seed sets), all with the same new binary. Reported for each:

* incidents resolved and victims lost per seed (victims lost: no conclusion from one set alone);
* field repairs started / completed / failed / cancelled, and cancelled by cause (section 2);
* base-repair counts (faults ended by a go-home), natural recoveries, faults still active at the end;
* fault active share of team-ticks and mean fault length, per class (`laptop/tools/persist_report.py`);
* team-ticks held, recommendations capped by action, trigger lifts, H2 refusals, H3 resets.

Also checked: with the token off the new binary reproduces the frozen `tron_v1` outputs byte for byte (several seeds and
both stress points), so the "off" arm equals the Step 3.6 world.

## 5. Adoption rule for the world used in Part 2

Decided now, before any run: **if, for the fixed gates, the pooled 95% interval of (on minus off) in incidents resolved
lies entirely below zero at stress 2 or at stress 7, the token is not adopted and the Step 4 evaluation uses the frozen v1
world (token off). Otherwise (neutral, or better) it is adopted and the Step 4 evaluation passes
`--causal-fixes=label,rtb,cost,stall,semantics,hold-repair` explicitly.** Victims lost and the planner arm do not enter
the decision. The reason for adopting in the neutral case is fidelity to the fault model (a field-repairable fault whose
repair is always cancelled is not what the model describes), not a performance claim. Either way the result is reported
plainly and no parameter is tuned.

## 6. Predictions (direction and confidence, fixed before any run)

Reference values are the Step 3.6 fixed-gate numbers at stress 7 / stress 2 (mobility unless stated): 134.7 / 71.3 repairs
started, 0.2 / 0 completed, 100.5 / 47.3 faults ended by a go-home, active share 26.2% / 11.8%, mean base-repair length
72.6 ticks, incidents resolved 34.23 / 42.52 per seed.

| # | Prediction | Confidence |
|---|---|---|
| 1 | Under fixed gates at least 65% of started repairs now reach their end (completed + failed) at both stress points | 65% |
| 2 | Among repairs that reach their end, the completed share is 2/3 within +/-0.06 (it is a draw) | 85% |
| 3 | Completed mobility repairs per seed rise from about 0 to at least 40 at stress 7 and at least 20 at stress 2 | 70% |
| 4 | Faults ended by a go-home fall: mobility at stress 7 from 100.5 to 60 or fewer, at stress 2 from 47.3 to 30 or fewer | 65% |
| 5 | Incidents resolved, fixed gates, on minus off, stress 2: between -1.5 and +1.0 with an interval that includes 0 | 55% |
| 6 | Incidents resolved, fixed gates, on minus off, stress 7: between -3 and +1; sign more likely negative than positive (60 to 40) | 55% |
| 7 | Mean mobility fault length and fault active share go **up**, not down: a third of repairs fail after a full hold and then still need the go-home (about 2/3 x 68 + 1/3 x (68 + 70) = 91 against 72.6 ticks); active share +5% to +25% relative | 60% |
| 8 | Victims lost: no conclusion (as in Step 3.6); I expect all pooled intervals to include 0 | 70% |
| 9 | Planner minus fixed gates, both with the token on, pooled interval includes 0 at both stress points | 65% |
| 10 | The stamina trigger lifts the hold in fewer than 25% of started repairs, so it is not the main reason repairs fail to complete (equipment running out during a long hold or the planner's own go-home are the other candidates) | 50% |
| 11 | A structural cost: a held team keeps its task blocked for the whole hold, where a go-home releases it to another team. This is why I expect the incident effect to be flat or negative rather than positive | 60% |
| 12 | The token is **adopted** under the rule in section 5 (for the fixed gates the incident interval does not lie wholly below zero at either stress point) | 60% |

Not predicted: the effect on the planner arm's detector-oracle headroom, the effect on the shipped AI, anything about
victims lost beyond row 8. These are measured in Step 4 and reported.

## 7. Provenance

No parameter is chosen from this step's results. Dev seeds 1001-1020 and held-out seeds 3001-3020 as in Steps 3.5 and 3.6;
the mechanism check uses seed 9001 (and 9002), which belongs to no evaluation or training range. The pre-registration
commit precedes the implementation commit and every comparison run.

---

# Results (Step 3.7, Part 1)

Run after the pre-registration commit `128236b` and the implementation commit `b1da57a`. Same binary for all 16 runs; the
"off" arm reproduces the Step 3.6 numbers exactly (for example stress 2 development, fixed gates: 42.10 incidents and 10.75
victims lost, the same as the Step 3.6 F v1 row). Seeds 1001-1020 (dev), 3001-3020 (held-out), 40 pooled; paired 95%
bootstrap intervals; AI off. Stress 2 is the realistic headline, stress 7 the harsh stress test.

## Plain summary

* **Holding the repair makes the fixed gates worse, not better.** At stress 7 the fixed gates resolve **3.12 fewer
  incidents per seed [-4.33, -1.95]** and lose **5.05 more victims per seed [+3.48, +6.60]** with the token on. At stress 2 the
  incident change is -0.78 [-1.65, +0.07] and victims lost +1.00 [-0.38, +2.27]: the same direction, smaller, and the
  intervals include 0.
* **The mechanism now works for mobility faults.** At stress 7, 25.6 mobility repairs complete per seed (against 0.2) and
  12.3 more run their full length and fail (the 1/3 failure draw); at stress 2, 13.1 complete. About 72% of started mobility
  repairs reach their end. The completed share of finished repairs is 0.675 and 0.668, as the 2/3 success draw says.
* **Resource repairs still almost never complete** (0.4 per seed at stress 7, 0.1 at stress 2). In the mechanism check
  (seed 9001) nearly every unattributed resource cancellation had the team's equipment at 0: the resource fault keeps
  draining equipment, and the hold does not stop the working costs the Step 3.6 hold inherited, so the team runs out of
  equipment before a 34-102 tick repair can finish and the task is abandoned by the existing exhaustion rule.
* **Under the pre-registered adoption rule the token is NOT adopted.** The fixed-gate incident interval lies wholly below
  zero at stress 7 (-3.12 [-4.33, -1.95]). The Step 4 evaluation therefore uses the frozen v1 world (token off). `hold-repair`
  stays an opt-in token.
* **The planner arm is unaffected, because it hardly ever starts a field repair** with the watchdog as its detector (0.0
  repairs per seed at stress 2, 0.2 at stress 7): the planner-minus-planner difference is 0.00 at stress 2 and +0.38
  [-0.12, +1.02] at stress 7. So with the token on, "planner minus fixed gates" turns positive at stress 7 (+2.52 [+1.20,
  +3.95]) only because the fixed gates got worse; it is not a planner improvement (with the token off it is -0.97 [-2.30,
  +0.38], as in Step 3.6).

## Incidents resolved and victims lost (per seed)

| Stress | System | Token | Dev | Held-out | Pooled | Dev lost | Held lost | Pooled lost |
|---|---|---|---|---|---|---|---|---|
| 2 | Fixed gates | off | 42.10 | 42.95 | 42.52 | 10.75 | 13.40 | 12.07 |
| 2 | Fixed gates | on | 41.50 | 42.00 | 41.75 | 12.70 | 13.45 | 13.07 |
| 2 | Planner | off | 42.40 | 42.25 | 42.33 | 12.00 | 12.60 | 12.30 |
| 2 | Planner | on | 42.40 | 42.25 | 42.33 | 12.00 | 12.60 | 12.30 |
| 7 | Fixed gates | off | 34.00 | 34.45 | 34.23 | 15.15 | 14.75 | 14.95 |
| 7 | Fixed gates | on | 30.25 | 31.95 | 31.10 | 20.65 | 19.35 | 20.00 |
| 7 | Planner | off | 33.30 | 33.20 | 33.25 | 15.60 | 15.20 | 15.40 |
| 7 | Planner | on | 34.15 | 33.10 | 33.62 | 15.70 | 15.00 | 15.35 |

Paired differences, on minus off (95% CI):

| Stress | System | Incidents: dev | held-out | pooled | Victims lost: pooled |
|---|---|---|---|---|---|
| 2 | Fixed gates | -0.60 [-2.10, +0.80] | -0.95 [-1.90, +0.00] | **-0.78 [-1.65, +0.07]** | +1.00 [-0.38, +2.27] |
| 2 | Planner | 0.00 | 0.00 | 0.00 | 0.00 |
| 7 | Fixed gates | -3.75 [-5.35, -2.25] | -2.50 [-4.25, -0.70] | **-3.12 [-4.33, -1.95]** | **+5.05 [+3.48, +6.60]** |
| 7 | Planner | +0.85 [0.00, +2.05] | -0.10 [-0.50, +0.25] | +0.38 [-0.12, +1.02] | -0.05 [-0.38, +0.20] |

Development and held-out agree in sign for the fixed-gate harm at both stress points; at stress 7 each set's own interval
excludes 0.

## Field repairs, base repairs, fault length (mean per seed over 40 seeds)

| Stress | Token | Class | Onsets | Repairs started | Completed | Failed | Cancelled | Ended by go-home | Natural | Active share | Mean length of ended faults (ticks) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 2 | off | mobility | 52.2 | 71.3 | 0.0 | 0.0 | 71.2 | 47.3 | 3.4 | 11.8% | 68.9 |
| 2 | on | mobility | 49.0 | 27.4 | 13.1 | 6.5 | 7.2 | 31.4 | 3.3 | 12.0% | 73.8 |
| 2 | off | resource | 41.8 | 13.7 | 0.1 | 0.0 | 13.6 | 39.4 | 1.7 | 6.0% | 41.3 |
| 2 | on | resource | 42.1 | 14.5 | 0.1 | 0.1 | 14.4 | 38.9 | 2.4 | 7.0% | 48.3 |
| 7 | off | mobility | 110.5 | 134.7 | 0.2 | 0.0 | 134.3 | 100.5 | 7.1 | 26.2% | 71.6 |
| 7 | on | mobility | 102.7 | 52.6 | 25.6 | 12.3 | 13.3 | 66.7 | 7.7 | 24.8% | 72.9 |
| 7 | off | resource | 108.3 | 25.7 | 0.2 | 0.1 | 25.3 | 102.2 | 4.2 | 14.8% | 40.0 |
| 7 | on | resource | 106.9 | 31.6 | 0.4 | 0.1 | 31.1 | 99.6 | 5.2 | 18.3% | 50.2 |

(The planner rows start 0.0 to 0.2 repairs per seed and are omitted; they equal the off rows to within a few tenths.) Team-ticks
held for a field activity per seed (fixed gates): 267 to 1599 at stress 2, 531 to 3119 at stress 7 (token off to on).

Cancellations by cause, fixed gates with the token on (per seed; stamina trigger / other-class trigger / planner above
floor / task released or resolved / fault ended another way / other or unattributed):

| Stress | Repair | Stamina | Other class | Planner | Released | Fault ended | Other |
|---|---|---|---|---|---|---|---|
| 2 | mobility | 0.62 | 2.55 | 0.00 | 0.95 | 1.62 | 1.48 |
| 2 | resource | 0.50 | 5.30 | 0.00 | 0.03 | 0.12 | 8.43 |
| 7 | mobility | 1.05 | 5.97 | 0.00 | 0.65 | 3.02 | 2.62 |
| 7 | resource | 0.42 | 10.72 | 0.00 | 0.00 | 0.17 | 19.73 |

Recommendations capped at LOCAL_RECOVERY per seed: RETURN 392 (stress 2) and 759 (stress 7); REALLOC and ABORT 0 (the
fixed gates issue neither in this world). The cap was lifted by the stamina trigger 1.1 and 1.5 times per seed and by the
other-class trigger 7.8 and 16.7 times. Second attempts refused (H2): 38 and 136 per seed. Recovery state reset after a
success (H3): 13.1 and 26.0, equal to the completed repairs.

## Why it is worse: what I measured and what I did not

Measured: more team-time is spent held (about 10% of team-ticks at stress 7 against 1.8%); a third of mobility repairs run a
full hold and fail, after which the team still has to go home; resource repairs hold the team until its equipment runs out
and then lose the task anyway; resource faults stay active longer (active share 14.8% to 18.3% at stress 7). Not measured:
how much of the incident loss comes from each of these, and the structural cost I predicted (a held team keeps its incident
blocked where a go-home releases it to another team). I did not run an ablation to separate them, because a variant built
to make the token win would be tuning.

## Scorecard (12 pre-registered predictions)

| # | Prediction | Result | Held? |
|---|---|---|---|
| 1 | At least 65% of started mobility repairs reach their end | 71.5% (stress 2), 72.0% (stress 7); resource repairs: about 1% | **yes for mobility** (the table said "mobility unless stated"); no for resource |
| 2 | Completed share of finished repairs 2/3 +/- 0.06 | 0.668 and 0.675 | **yes** |
| 3 | Completed mobility repairs at least 40 (stress 7) and 20 (stress 2) | 25.6 and 13.1 | **no** (repairs started fell to 52.6 and 27.4 because of the one-attempt rule and fewer working-team LOCAL dispatches) |
| 4 | Mobility faults ended by a go-home: 60 or fewer (stress 7), 30 or fewer (stress 2) | 66.7 and 31.4 | **no** (close) |
| 5 | Stress 2 fixed-gate incidents, on minus off, in [-1.5, +1.0] with a CI that includes 0 | -0.78 [-1.65, +0.07] | **yes** |
| 6 | Stress 7 fixed-gate incidents in [-3, +1], sign more likely negative | -3.12 [-4.33, -1.95] | sign yes, range **narrowly no** (and the interval excludes 0) |
| 7 | Mean mobility fault length and active share go up (+5% to +25% share) | length +7% (stress 2) and +2% (stress 7); mobility active share +0.2 and -1.4 points | **no** for the magnitude and for the share; direction of length yes |
| 8 | Victims lost: all pooled intervals include 0 | stress 7 fixed gates +5.05 [+3.48, +6.60] | **no** (victims lost clearly worse at stress 7) |
| 9 | Planner minus fixed gates with the token on includes 0 at both stress points | stress 2 +0.57 [-0.45, +1.60]; stress 7 +2.52 [+1.20, +3.95] | stress 2 yes, **stress 7 no** (the fixed gates got worse; the planner arm barely uses repairs) |
| 10 | Stamina trigger lifts the hold in under 25% of started repairs | 1.1 and 1.5 lifts per seed against 41.9 and 84.2 repairs started (about 3% and 2%) | **yes** |
| 11 | Structural cost of a blocked task | consistent with the incident loss; not isolated | **untested** |
| 12 | The token is adopted under the rule | the rule says not adopted | **no** |

Four predictions held outright (2, 5, 10, and 1 for mobility), two partly (6, 9), five did not (3, 4, 7, 8, 12), one was
not tested (11).

## What this does and does not show

It shows that, in this world and with these gates, letting the rule gates hold a field repair makes the fixed-gate system
worse at the harsh stress and not measurably different at the realistic stress. It does not show that field repair is a bad
idea: the repair length (34-102 ticks), the 2/3 success probability, and the inherited working costs during a hold are
assumptions of the world, and the policy tested is one specific hold rule. It says nothing about the planner choosing repairs
well (the planner arm hardly chooses them with the watchdog as its detector), and it says nothing about the shipped AI.

Decision for the rest of Step 3.7: the token stays opt-in and is not used in Step 4.

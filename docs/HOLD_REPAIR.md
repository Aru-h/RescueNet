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

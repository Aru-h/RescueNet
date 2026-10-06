# Step 3.6 proposal: faults that need fixing (causal profile only)

Status: **approved with amendments (section 12); pre-registered in section 9, committed before
any planner or AI comparison is run.** The planner, the AI and every file in `core/` were
untouched while this was written. Implementation and the evaluation follow the pre-registration
commit.

## 0. Why

Step 3.5 (`docs/RECOVERY_PLANNER_RESULTS.md`, `docs/RECOVERY_ACTION_SEMANTICS.md`) found that in
the current model a team-level response can barely change a fault's outcome:

* mobility and resource faults end on a drawn timer of 6-14 ticks (3.6-8.4 min); 42-43% of
  mobility and 24-26% of resource faults end earlier because the team happened to rest, and that
  share is identical for the fixed gates, the planner and a perfect detector;
* a go-home costs a round trip of ~40+ ticks for a fault the timer ends in ~10;
* so a perfect detector adds about 0 (+0.05 [-0.93, +0.95] incidents per seed).

That is a property of the model, not evidence about detectors. Real faults persist until
something is done about them. This step changes the model so that a response matters, and
reports whether a better detector or forecaster is worth anything once it does.

Everything below applies only under `--fault-profile=causal`. Other profiles stay byte-identical
(C1 md5 `b248bea72ce2ceb81389762dccc0a749`, all-profile dataset diffs, `--diag` stderr).

## 1. The fault model

**Flags (proposed).** `--causal-persist=on|off` (the model below; `off` reproduces Step 3.5
byte for byte; default **`on`** under the causal profile in v1) and a new `--causal-fixes` token `semantics` (the
two action bugs, section 4). Each part is separately switchable so the effect of each can be
measured: `persist`, `repair` (field repair) and `semantics`.

### Mobility and resource faults (class 0 and 2)

A fault no longer ends on its 6-14 tick timer. It ends by one of four things, recorded per fault
as its **end cause**:

| End cause | How | Time | Success |
|---|---|---|---|
| **Base repair** (RETURN_TO_SAFE_ZONE) | The team goes home and rests at a safe zone. Mobility: a repair dwell there. Resource: rest until equipment >= 90 and stamina is above the soft floor (as in Step 3). | Trip + dwell (mobility dwell 34 ticks, see A4; resource: the existing rest length) | 100% (A5) |
| **Field repair** (LOCAL_RECOVERY on a working team) | The team stops where it is for the repair time; it keeps its task but makes no progress meanwhile. Mobility: repair. Resource: swap/redistribute a spare kit, which restores equipment to 90 but not stamina (A6). | uniform 34-102 ticks (mean 68 = 0.68 h) | 2/3 per attempt; a failed attempt leaves the fault and the team can try again or go home |
| **Natural recovery** | Nobody does anything. A long tail: exponential with mean 1084 ticks (10.8 h). | 10.8 h mean | n/a |
| **Escalation (not an end)** | While a fault is unremedied it can step up one severity level (mobility 3 to 4 "disabling"; resource 2 to 3) | hazard = 1.5 x the class's current onset hazard per tick | n/a |

Wear removal keeps the existing values: a successful field repair or base repair removes 60% of
hidden wear, a rest without a fault 30% (`wear_repair_reduction`, `wear_rest_reduction`).

**Communication:** unchanged (link-margin recovery, no timer). **Environment:** unchanged
(Step 3.1/3.2: 600-2400 tick persistence, CLEAR_ROAD ends it). **Mission:** unchanged.

### The cost of carrying on while faulted

The simulator already degrades a faulted team, and persistence is what makes that expensive:

* mobility severity 3: moves only every second tick (`move_team`, `core/simulation.c`), work rate
  minus 3 (`team_effective_work_rate`, `core/incident.c`);
* mobility severity 4 (after escalation): moves every third tick, work rate minus 4;
* resource severity 2: work rate minus 2, extra stamina drain 0.24 per tick and equipment minus 2
  per tick, floored at 25 (`core/team.c`); severity 3 after escalation.

Those severities are existing parameters; I do not change them. Previously they applied for
3.6-8.4 minutes; now they apply for hours unless remedied. The new, additional cost is the
**escalation** above: a faulted team is 1.5 times as likely to get worse as it was to get the
fault in the first place (reusing the existing mobility-to-resource cascade factor 1.5).

## 2. How big a change is it? (measured with the Step 3.5 build, no code change)

Fault onset rates at the current model, 5 seeds (1001-1005), 3000 ticks = 30 h, 10 teams:

| Stress | Mobility onsets / team-hour | MTBF | Active share | Resource onsets / team-hour | MTBF | Active share |
|---|---|---|---|---|---|---|
| 1 | 0.13 | 7.8 h | 1.0% | 0.07 | 14.4 h | 0.6% |
| 2 | 0.23 | 4.4 h | 1.8% | 0.15 | 6.6 h | 1.2% |
| 3.5 | 0.35 | 2.8 h | 2.7% | 0.23 | 4.3 h | 1.9% |
| 7 | 0.60 | 1.7 h | 4.5% | 0.47 | 2.1 h | 3.9% |

The field-robot literature gives MTBF 6-20 h (section 3). That is **stress 1-2**, not stress 7.
Stress 7 is the evaluation point used since Step 3; **it was never derived from data** (I found no
recorded derivation; a note to that effect is added to the earlier docs). With
persistent faults the active share rises from ~4% to an expected ~30% at stress 7 (section 8),
which is a heavily loaded fleet. **Framing (decided by the user, section 12):** **stress 2 is the realistic headline** (its MTBF
matches the literature); **stress 7 is reported as a harsh-conditions stress test** (and keeps
continuity with Step 3.5). Both are co-primary in the analysis, are always reported side by side,
and I will not select between them afterwards.

## 3. Grounding, with what is verified and what is not

I could reach search summaries but not the papers themselves (several hosts are blocked from
this environment), so every number below taken from a paper is **to verify against the paper**.

| # | Source | What I use | Status |
|---|---|---|---|
| S1 | Carlson & Murphy, "How UGVs physically fail in the field", IEEE Trans. Robotics 21(3), 2005 | Field robots: MTBF 6-20 h; about **two thirds of failures repairable in the field** ("under favorable environmental conditions with equipment that normally accompanies the robot") | Citation confirmed; figures from search summaries; **to verify** |
| S2 | Carlson & Murphy, "Reliability analysis of mobile robots", ICRA 2003, pp. 274-281 | MTBF background | Citation confirmed; content not read; **to verify** |
| S3 | Availability figures for disaster robots, quoted in a secondary document (hal-lirmm.ccsd.cnrs.fr/lirmm-00804370) | Inuktun: MTBF 6.1 h, availability 90% ("minor, easy to repair"); iRobot: MTBF ~6 h, availability 36% ("serious, hard to repair"). With A = MTBF/(MTBF + MTTR), MTTR = MTBF(1-A)/A gives **0.68 h** (minor) and **10.8 h** (serious). | Numbers from a search summary; the MTTR values are **my derivation, to verify**; the definition of "availability" in the source is not checked |
| S4 | Williamson & Feyer, Occup. Environ. Med. 57:649-655, 2000 | 17-19 h awake gives performance equal to or worse than 0.05% BAC; response speed up to 50% slower | Confirmed from the abstract summary |
| S5 | FEMA US&R task forces and INSARAG classification | 12-hour alternating shifts for US&R work; the INSARAG team classification gives continuous-operation durations | FEMA 12 h shifts confirmed; the INSARAG per-class durations in the summary look wrong for Heavy teams, so **I do not use them (to verify)** |
| S6 | Nowlan & Heap, *Reliability-Centered Maintenance*, 1978 | Only ~11% of components benefit from an age limit; ~89% fail at random or show no age relationship; condition-based maintenance beats age-based | Figures from a search summary; **to verify** |

How each is used: S1 gives the field-repair success rate (2/3) and the literature MTBF that
motivates stress 2; S3 gives the field-repair time (0.68 h) and the natural-recovery tail
(10.8 h, the depot-repair MTTR of an unattended serious fault); S4 and S5 motivate keeping the
existing rest cycle and the hazard that grows with time since rest (`res_hazard_since_rest`,
horizon 8 h) and the 100-tick forecast horizon (one hour, section 6); S6 is the reason I expect
preventive rest to be mostly not worthwhile (section 6).

I could not find a source for: repair time of a *base* repair, escalation rates, or how often an
unattended fault clears by itself. Those are assumptions (below).

## 4. The two action bugs, under the causal profile only

New `--causal-fixes` token `semantics` (bit `CAUSAL_FIX_SEMANTICS = 0x10`; `CAUSAL_FIXES_ALL`
becomes `0x1F`). With it off, behaviour is exactly Step 3.5. Both bugs are described, with
file and line, in `docs/RECOVERY_ACTION_SEMANTICS.md`.

| Action | Current (all profiles) | With `semantics` (causal only) |
|---|---|---|
| `RETURN_TO_SAFE_ZONE` | release the task, go home | unchanged |
| `ABORT_MISSION` | same as RETURN | **release the task and exclude this team from that incident until its current stage completes; the team stays where it is, AVAILABLE, and is re-allocated elsewhere.** Meant for "I cannot do this task" (for example a disabling fault). |
| `REQUEST_TASK_REALLOCATION` | re-runs the allocator; the team keeps the task | **release the task (progress kept, as `planner_release_team_for_preempt()` does) and exclude this team from it for 30 ticks (the planner's existing cooldown), so another team gets it.** The team stays where it is, AVAILABLE. |
| `LOCAL_RECOVERY` | AVAILABLE team: go home | AVAILABLE team: unchanged. **Working team with an active mobility or resource fault: start a field repair** (token `repair`, section 1). |

Exclusion state is one (incident slot, until) pair per team: about 60 bytes, hashed, laptop
world only. The allocator skips an excluded team for that incident. No enum value, dataset
column or wire field changes.

**KNOWN_LIMITATIONS.** I will add two entries to section C of `KNOWN_LIMITATIONS.md` on this
branch: (a) ABORT_MISSION and RETURN_TO_SAFE_ZONE act identically on a working team, (b)
REQUEST_TASK_REALLOCATION does not release the team's task, with file and line, stating that both
exist in every profile and in the submitted build. **Open question for you:** the submitted build
is `origin/submission/single-board-dk`, and you restricted pushes to `feature/causal-faults`. I
will not touch that branch unless you say so; I can give you the exact text to apply there.

## 5. What this does to the baseline (predictions are in section 9)

* Fixed gates and planner both lose time: faults last hours unless remedied, so the share of
  team-ticks with an active mobility or resource fault rises from about 4% to an expected
  25-40% at stress 7 and 10-20% at stress 2 (section 8 gives the arithmetic).
* The fixed gates change behaviour for two reasons that cannot be switched off independently of
  the model: the escalation ladder's LOCAL_RECOVERY now starts a field repair on a working team,
  and ABORT and REALLOC now release the task. I will report the baseline as a ladder to
  separate them: Step 3.5 world, then `persist`, then `persist+repair`, then
  `persist+repair+semantics` (the v1 world).
* Every Step 3.5 conclusion that depends on the fault model (notably "a perfect detector adds
  about 0") is conditional on timer faults and has to be re-measured here; I will not carry
  those numbers over.

## 6. Forecasting headroom: a hazard oracle and a preventive action

Step 3.5's oracle was a perfect **detector** (it knows whether a fault is active now). A forecaster
is a different thing: it says how likely a fault is in the near future. Both are laptop-only
oracles (stubs on the board).

**Hazard oracle (`--planner-p=hazard`).** For each team and class c in {mobility, resource} it
returns the true probability of an onset within a horizon H,
`P_c = 1 - exp(-H * h_c)`, where `h_c` is the hidden per-tick hazard the injector is using right
now (it depends on hidden wear, ticks since rest, battery depletion, roughness, stress:
`truth->hazard_mobility`, `truth->hazard_resource`). **H = 100 ticks (one hour, A9).** It knows
the hazard, not the random draws. (An even stronger "dice oracle" that also knows the draws is
possible; I propose not to build it unless the hazard oracle shows headroom.) It is combined with
the detector oracle in the combination `--planner-p=both`.

**Preventive actions.** Two new Layer A options, both available only when a forecast source is
present (so the no-AI planner and the current AI are unaffected). Each is taken only when its
expected benefit beats its cost by the planner's existing margin (0.25):

`sum_c [P_c(stay) - P_c(after action)] * C_fault_c  >  cost(action) * (1 + margin)`

`P_c(after action)` is the oracle's own hazard recomputed for the post-action state;
`C_fault_c` is the new world's expected cost of a fault of class c (time to remedy x degraded
rate plus escalation, derived from section 1, not tuned).

| Option | What it does | Time cost | What it changes | Constants used |
|---|---|---|---|---|
| **PREVENT** (rest at base) | go home, rest, come back, with the existing RETURN and rest mechanics | trip + rest + return (about 100 team-ticks; resource rests are longer) | wear x (1 - 0.30), time-since-rest reset to 0, equipment resupplied | `wear_rest_reduction` = 30% (existing) |
| **PREVENT_FIELD** (in-field maintenance, no trip) | the team stops where it is for the maintenance time, keeps its task, makes no progress meanwhile | **the field-repair time: uniform 34-102 ticks, mean 68 (0.68 h, source S3)**; there is no figure for routine maintenance, so I use the repair figure (A10) | **wear only**: wear x (1 - 0.30). It does not reset time-since-rest and does not resupply equipment, so it acts on the mobility hazard only. | wear reduction = **`wear_rest_reduction` = 30%**, the existing constant for routine maintenance (the code comment in `fault_injector.c` calls a rest without a fault "routine maintenance"); **not** the 60% `wear_repair_reduction`, which is for repairing an actual fault. Always completes (A11). |

Both reuse the field-repair stationary-hold mechanism (section 1), so evaluators excuse the hold
exactly as for a field repair. An option is selectable with `--planner-prevent=none|rest|field|both`
(default `none`; the pre-registered runs say which is used). The report gives the hazard-oracle
headroom with **each option available separately and with both**. A real model could supply the
forecast later (the retrained AI, section 7).

**Predicted result.** The preventable part of the hazard is only the wear/time-since-rest part
(the Step 3 model was set so that wear is the attributed cause of 30-60% of mobility onsets,
`docs/CAUSAL_FAULT_MODEL.md`, a tuned value; S6 suggests most real faults are random). At stress 7, P(fault in the next hour) is about 45% for mobility already, and
a rest lowers the hazard by perhaps a third, so the gain is about 0.15 x C_fault per class, while
the trip plus rest plus return costs on the order of 100 team-ticks. For PREVENT_FIELD the gain is smaller still (wear only, mobility only) but so is the cost
(~68 against ~100+ team-ticks). With the numbers above (mobility hazard ~0.006 per tick at stress 7,
wear about half of it, 30% removal, H = 100: P falls by about 0.08; fault cost about 50
team-ticks) the expected benefit is ~4 team-ticks against a cost of ~68, a factor of about 17.
I expect **neither** option to be chosen more than a handful of times per run and the hazard-oracle
headroom to be about 0 with each option and with both. This is a prediction, not a measurement.

## 7. Dataset and labels for the later AI retrain

No change to the dataset schema, `RecoveryAction_t` or the wire. Effects:

* **Class balance:** `fault_active_mobility/resource` go from ~4% positive to an expected 25-40%
  at stress 7 (10-20% at stress 2).
* **Episodes:** hours long, not minutes, so labels are strongly autocorrelated in time; a split
  by seed (as now) is still the right one, a random row split would leak.
* **Severity changes within an episode** (escalation).
* **Existing symptoms stay valid** (stalls, wear symptom), but a field repair holds the team in
  place for ~68 ticks: that must be excused in the stall, progress, consensus and mission-twin
  evaluators exactly as the debris stall fix did (reusing `causal_actions_debris_held_at`'s
  ring), or every repair would be read as a mobility fault.
* **`fault_ticks_remaining_*`:** with no backstop the remaining time is the (hidden) natural
  recovery draw. I will check the feature-spec leak guard (`check-feature-spec-leak-guard`) still
  keeps it out of model inputs; if it is an input today I will report that rather than hide it.
* **Sidecar `tron_causal_truth.csv` only:** append `end_cause_*`, `repair_state`, `escalated_*`,
  `risk_mobility_h`, `risk_resource_h` (the forecast labels, so a forecaster can be trained).
* **The shipped model is not retrained** (`ml/` untouched). It was trained on the timer model, so
  its results in the new world are a distribution-shift context number, not a verdict on AI.

## 8. Expected size of the effect (arithmetic behind the predictions)

* Time to remedy by base repair (RETURN) is about detection delay ~10 + trip ~40 + dwell 34 = ~85
  ticks. Active share ~ 85 / (MTBF + 85). Mobility at stress 7: MTBF 1.7 h = 170 ticks, so ~33%;
  at stress 2: MTBF 4.4 h = 440 ticks, so ~16%. Resource is similar with a longer rest.
* A faulted team works at roughly half speed, so incidents resolved should fall by about half the
  active share: ~15-20% at stress 7 (6-9 of 43 per seed) and ~7-8% at stress 2 (2-4 per seed).
* Escalation before remedy: 1 - exp(-1.5 x h x 85); at stress 7 (h ~ 0.006) about half of
  faults, at stress 2 (h ~ 0.0017) about a fifth.
* The rule-based gates confirm a fault in under ~10 ticks, so what a detector can still add is
  mostly the faults the rules miss; a missed fault now costs hours, not minutes. The size of that
  miss rate in the new world is exactly what the oracle comparison measures.

## 9. Pre-registration

To be committed before any planner or AI comparison is run. Nothing below is tuned: values are
from sections 1-3; assumptions are marked A and are not changed after seeing results.

### 9.1 Parameter values (final)

| Parameter | Value | Basis |
|---|---|---|
| Natural recovery of an unattended mobility/resource fault | exponential, mean 1084 ticks (10.8 h) | S3 derived; **to verify** |
| Field repair duration | uniform 34-102 ticks (mean 68, 0.68 h) | S3 derived; **to verify** |
| Field repair success per attempt | 2/3 | S1; **to verify** |
| Base repair dwell (mobility, at a safe zone) | uniform 17-51 ticks (mean 34) | **A4: half the field time (better tools); assumed** |
| Base repair success | 100% | A5, assumed |
| Resource field repair | restores equipment to 90, not stamina | A6, assumed |
| Escalation | one severity step; hazard = 1.5 x the class's current onset hazard | A7: 1.5 is the existing cascade factor; assumed |
| Severity effects while faulted | unchanged (mobility 3/4, resource 2/3) | existing |
| ABORT exclusion | until the incident's current stage completes | A8, assumed |
| REALLOC exclusion | 30 ticks | the planner's existing cooldown |
| Forecast horizon H | 100 ticks (1 h) | A9: one shift-hour; matches the planner's rate window; assumed |
| PREVENT_FIELD time | uniform 34-102 ticks (the field-repair time) | S3 derived; A10 (no routine-maintenance figure exists) |
| PREVENT_FIELD wear reduction | 30% = `wear_rest_reduction` (routine maintenance) | existing constant |
| PREVENT_FIELD success | always completes | A11, assumed |
| `--causal-persist` default | on under the causal profile | user decision |
| Planner (V, margin, all layers) | exactly as Step 3.5 (V x1, margin 0.25); not re-tuned | pre-registered |
| Calibration tables | **not refitted on the new dynamics for the headline planner**; see 9.4 | pre-registered |
| Stress points | 7 and 2, both primary | section 2 |
| Seeds | dev 1001-1020, held-out 3001-3020, pooled 40 | as Step 3.5 |

Assumptions A1-A3 are the existing Step 3 assumptions (wear removals 60%/30%, cascade 1.5, caps).

### 9.2 Runs

World ladder (fixed gates, AI off): Step 3.5 world (`persist` off); `persist`; `persist+repair`;
`persist+repair+semantics` = **v1 world**. In the v1 world, at both stress points:

1. fixed gates, AI off (**F**)
2. planner, no AI (**P**)
3. planner + detector oracle (**P-det**)
4. planner + hazard oracle (**P-haz**), run with each preventive option available: PREVENT only
   (`rest`), PREVENT_FIELD only (`field`), and both (`both`)
5. planner + both oracles (**P-both**), with `--planner-prevent=both`
6. fixed gates + shipped AI, planner + shipped AI (context only)

Paired differences on incidents resolved and victims lost, dev / held-out / pooled, 95% bootstrap
CIs. Plus the fault-ending breakdown per class (base repair, field repair success, field repair
failure, natural recovery, escalations) and the active share of team-ticks.

Stress framing in every table: stress 2 first as the realistic headline, stress 7 as the
harsh-conditions stress test.

Robustness (reported, never used to choose a value): natural-recovery mean x0.5/x2, field time
x0.5/x2, field success 0.5/0.8, escalation x0.5/x2, base dwell = field time, all on the v1 world,
planner no AI vs fixed gates.

### 9.3 Predictions

"Held" means the pooled point estimate has the predicted sign and, where a nonzero effect is
predicted, the 95% CI excludes 0. "About 0" means the CI includes 0 and |point| < 1 incident per
seed.

| # | Comparison | Predicted | Confidence |
|---|---|---|---|
| 1 | v1 world minus Step 3.5 world, fixed gates, incidents | **negative**, about -6 to -9 per seed at stress 7, -2 to -4 at stress 2 | high |
| 2 | v1 fault share of team-ticks (mobility, resource) | 25-40% at stress 7, 10-20% at stress 2 | medium |
| 3 | `repair` token alone on the fixed gates | small positive (0 to +2) at stress 7, about 0 at stress 2 | low |
| 4 | **P minus F** (planner no AI vs fixed gates) | **positive**, +1 to +3 at stress 7; 0 to +1 at stress 2 (CI may include 0) | medium |
| 5 | **P-det minus P** (detector headroom) | **positive but small**: +0.5 to +2 at stress 7, about 0 at stress 2. The rules already confirm faults in ~10 ticks, so only misses remain. | low |
| 6a | **P-haz[rest] minus P** (forecaster headroom, PREVENT available) | **about 0** (at most +1) at both stress points; PREVENT chosen at most a few times per run | medium |
| 6b | **P-haz[field] minus P** (PREVENT_FIELD available) | **about 0** (at most +1) at both stress points; expected benefit is ~1/17 of the cost at stress 7 and lower at stress 2 | medium |
| 6c | **P-haz[both] minus P** (both available) | **about 0**; no larger than the larger of 6a and 6b by more than noise | medium |
| 7 | P-both minus P | about the sum of 5 and 6, so dominated by 5 | low |
| 8 | Hazard headroom (6c) <= detector headroom (5) | yes at both stress points | medium |
| 8b | Number of PREVENT / PREVENT_FIELD actions taken per seed | under 2 each at both stress points | low |
| 9 | Planner + shipped AI minus P | about 0 or negative (the AI is trained on the old model) | medium |
| 10 | Victims lost | no conclusion expected: intervals include 0 | high |
| 11 | End causes | most faults end by base repair at stress 7; natural recovery under 10% | medium |

If a perfect detector (5) is still about 0 in this world, the report will say so plainly, with
the end-cause breakdown as the reason.

### 9.4 Rules for the run

* No parameter in 9.1 changes after the first comparison is run. If a bug is found the run is
  repeated with the same parameters and the bug is reported.
* The planner changes allowed are the ones the new world needs, each derived from 9.1 rather
  than tuned: its fault-cost model prices a persistent fault (time to remedy under stay /
  field repair / return, plus escalation), it gains the field-repair and PREVENT options, and it
  gains the hazard oracle source. V and margin stay at their Step 3.5 values.
* The calibration tables used by the planner stay as fitted in Step 3.5. A refit on the new
  dynamics, on dev seeds only, will be reported as a **separate** row if I run it.
* The shipped AI and `ml/` are untouched.

## 10. Checks (new and existing)

New: `check-causal-persistence` (no fault ends by timer; end causes recorded; field-repair
success rate matches the draws; escalation monotone and capped; two runs identical), 
`check-causal-semantics` (ABORT leaves the team AVAILABLE in place and excluded; REALLOC puts the
task with another team and keeps progress; RETURN unchanged), forecast-oracle check (hazard
matches `truth` and equals 0 for a freshly rested, fully supplied team).

Existing, all re-run: C1 md5, all-profile dataset diffs and `--diag` stderr, `--causal-persist=off`
and `--causal-fixes=label,rtb,cost,stall` equal the Step 3.5 build byte for byte, check-evaluate-
determinism, check-live-override-noop-regression, check-consensus-shadow-regression,
check-causal-profile, -fixes, -env-no-rtb, -roads-connectivity, -fault-exposure,
check-recovery-planner, check-selftests, and the host board-config syntax check (ARM `check-board*`
cannot run here).

## 11. Cost and risks

* Work: injector (persistence, escalation, field repair, counters), allocator exclusion,
  dispatch semantics, stall-style excusal of repair holds in four evaluators, planner cost model
  and options, oracle source, sidecar columns, checks, docs. Roughly the size of Step 3.5's
  simulator work; the evaluation matrix is about 40 sets per stress point, about 2-3 hours each.
* Risk: with persistence at stress 7 the fleet may saturate and every condition collapse to the same
  low number; that is itself a finding and is why stress 2 is co-primary.
* Risk: the S3 derivation (availability to MTTR) is mine and unverified. The robustness runs
  bound its influence but do not remove it.
* Open questions of the proposal were settled in section 12.

## 12. Decisions (user approval)

1. Stress 2 is the realistic headline (matches the 6-20 h MTBF literature); stress 7 is reported
   as a harsh-conditions stress test; both stay co-primary. Earlier docs get a note that stress 7
   was never derived from data.
2. v1 freezes with `--causal-persist` **on** by default under the causal profile.
3. `submission/single-board-dk` is not touched. Both action bugs are added to `KNOWN_LIMITATIONS.md`
   on `feature/causal-faults`, noting they exist in the submitted build and in the default profiles.
4. A second preventive option, PREVENT_FIELD, is added next to PREVENT (section 6), with a
   pre-registered prediction for each, and the hazard-oracle headroom reported with each available.
5. Everything else as proposed. Commit as Harush Temkar, no trailers; push `feature/causal-faults`
   after the pre-registration commit and again with the results.

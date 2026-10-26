# Step 5 pre-registration: headroom map (and the road-information addendum)

Written and committed BEFORE any probe is implemented or run. Exploration only, and the last exploratory step. Nothing
here changes the world, a default, the model, `ml/`, a schema, an enum or the wire format. Every probe is opt-in, under
`--fault-profile=causal` only, laptop-only, with hash-based draws. The addendum (road information, R1-R3) was received
while this document was being written, before any commit; it is part of the same pre-registration and the same Holm
family.

## 1. Question

Step 4 said a perfect fault detector adds at most about 5% incidents. This step asks the broader question: at which link
of the chain (detection, prevention, demand foresight, discovery, coordination, communication, road knowledge, raw
capacity) does ANY extra information or better decision rule have headroom, and how large is it in common units
(incidents, victims, and team-equivalents)?

## 2. The v1 world and the matched baseline

* World: as Step 4. `--fault-profile=causal --causal-persist=on --causal-fixes=label,rtb,cost,stall,semantics`,
  roads on, recovery actions off, hold-repair off, fixed stress. Stress 2 (realistic) and 7 (harsh) in the screen.
* **Matched baseline B = system P**: planner (`--recovery=utility`) with the watchdog detector, no AI, no probe.
  Reason: it is the best non-AI deployable system of Step 4, and the planner is what the detector, prevention and
  foresight probes plug into. Every other probe is an overlay on P. Where a probe changes the baseline's own evidence
  (D3b) the matched baseline is P under the same degradation.
* Headroom = probe minus B, paired by seed. Incidents resolved: positive is better. Victims lost: reported as
  `B - probe`, so positive is better for both metrics.
* Build: scratch binaries under `build/headroom/` from the repository sources at -O2 (checked identical to the -O0
  Step 4 build on a pilot seed: stdout, stderr and both CSVs byte-identical). The shipped AI is used only where a
  probe names it (D3b), labelled "shipped model (trained on the old fault model, context only)".
* Default behaviour: every probe flag defaults to off; with no probe flag the simulator output must be byte-identical
  to `c7408be` (C1 md5 `b248bea72ce2ceb81389762dccc0a749`, all six existing profiles, all existing `make check-*`).

## 3. Information levels

Every INFORMATION oracle is defined at two levels where both exist:
(i) **perfect foresight**: knows the realised outcome; (ii) **rate-aware**: knows the underlying hazard or probability,
not the random draw. (ii) is the realistic ceiling for a learned forecaster. For each probe the table in section 5
states what the corresponding quantity would be on a real system.

Two facts found while scoping that limit which levels exist (verified in the code before writing this):

1. **Hazard-spawned incidents (fire spread, aftershock collapse, gas explosion) are exogenous.** They come from
   deterministic rolls on building state that team behaviour does not touch. Level (i) foresight is therefore
   implemented by **record and replay**: the baseline run writes the event list (tick, type, x, y) with `--probe-record`;
   the probe run reads it. The probe run also compares every event it actually sees with the recorded one and reports
   the mismatch count, which must be 0 (it is the proof of exogeneity).
2. **Random incident arrivals are endogenous.** `calculate_generation_probability` depends on available teams, open
   and hidden incidents, so arrival times depend on what teams do (more available teams means more arrivals). Replay
   would be invalid and cannot be fixed with a hook. So a foresight oracle for random arrivals is **not run**, and M4
   covers the exogenous (hazard) arrivals only. This also means "incidents resolved" is partly demand that responds to
   capacity; I report victims lost as a co-primary metric because of it.
3. A rate-only level (ii) for M2-M4 (an Omori or spread rate with no location) gives a policy nothing to act on, so
   level (ii) is **not defined** for M2-M4 and is reported as such, not run.

## 4. Probes, flags and exact definitions

All flags are `--probe-*`. They are accepted only with `--fault-profile=causal` and rejected otherwise.

### Diagnostics (read-only)

* **D1** detection delay and miss rate of the fixed gates vs ground truth, per class. No flag: computed offline from
  the existing sidecar truth (`tron_causal_truth.csv`) and the dataset's logged recommendation on the B runs, by a new
  script `laptop/tools/detection_delay.py`. An episode is "detected" the first tick the gates' recommendation is at
  least LOCAL_RECOVERY while the fault is active; delay = that tick minus onset; missed = never detected before the
  fault ends. Distribution (median, p10, p90), per class, stress 2 and 7, dev seeds.
* **D2** `--probe-oracle-delay=K`, K in {0, 5, 10, 20, 40}, with `--planner-p=oracle`. The oracle's per-class fault
  indicator is read from K ticks ago (ring buffer of the true fault-active flags). K=0 is the existing PD system and
  is a reproduction check.
* **D3a** `--probe-oracle-fn=0.30 --probe-oracle-fp=0.01` with `--planner-p=oracle`. Per team, class and tick the
  oracle misses a real fault with probability 0.30 and invents one with probability 0.01 (hash draws on
  seed, tick, team, class, kept constant within an 8-tick block so the oracle does not flicker). The rates are
  illustrative, fixed now, and not fitted to the shipped model.
* **D3b** `--probe-evidence-dropout=Q`, Q in {0.25, 0.50, 0.75}. With probability Q per team and tick the baseline's
  rule-based evidence channels (watchdog status and execution-twin signals, as read by the recovery evaluators and the
  planner's watchdog input) read as "no anomaly, no data" for that team-tick (hash draw). The AI's own input vector is
  NOT degraded (it is a separate path), so the question "does the AI matter when the baseline sees less" is posed
  fairly. Each level is run for P, PD (detector oracle) and PA (shipped AI), all against the P of the same Q. Only
  dropout is implemented; additive noise is not.

### Supply side

* **S1** `--planner-p=hazard --planner-prevent=rest` (existing, level ii) and `--probe-hazard-foresight=draw`
  (level i: the forecast uses the realised draws, evaluated on the current hazard for the next forecast-horizon ticks),
  each with `--probe-prevent-cost=M`, M in {1, 0.5, 0.25} multiplying the maintenance (rest at base) cost the planner
  charges for a preventive action. M=1 re-measures Step 3.6 on 100 seeds. The preventive action itself (what it does to
  the team) is unchanged: only the cost the planner compares is scaled, which is what "cheaper prevention" means to
  the decision. (If the planner's cost is not the only place prevention costs time, that is reported as a caveat.)

### Demand side

* **M1** `--probe-urgency=survival` (level ii) and `--probe-urgency=foresight` (level i). Replaces the allocator's
  `priority_score` with an urgency from the victim model: expected victims lost in the next W=240 ticks,
  `u = victim_count * (1 - S(age+W)/S(age))` with the Weibull survival of the simulator's own entrapment model
  (level ii: the rate; level i: the realised draw outcomes for those ticks given the current victim count), mapped as
  `score = min(1, u / 3)`. W and the divisor are fixed now. Note: level (ii) uses only quantities a real allocator
  can observe (age, victim count, a survival curve), so it is a decision rule, not an oracle.
* **M2** `--probe-foresight=fire --probe-lead=50`: record-replay of fire-spread events (ACTIVE_FIRE spawned by
  `propagate_fire`), revealed 50 ticks ahead. **M3** `--probe-foresight=collapse --probe-lead=50`: aftershock-collapse
  events. **M4** `--probe-foresight=hazard --probe-lead=T`, T in {10, 50, 200}: the union of exogenous hazard events.
  Policy shared by M2-M4 ("pre-positioning"): when an event is known to spawn at (x, y) within the lead, the nearest
  idle AVAILABLE team of the matching specialisation (not faulted, above its soft stamina floor) steps toward (x, y)
  one A* step per tick without changing status or paying stamina (a ceiling: free movement). Rules fixed now.
* **M5** `--probe-reveal-all`: every incident is discovered the tick it spawns (removes fog-of-war and the
  undiscovered-backlog effect on generation). Level (ii) is not defined (discovery is search coverage, there is no
  separate rate to know).

### Decision quality

* **C1** central omniscient allocator vs CBBA. **Not run in this pre-registration.** A faithful upper bound replaces
  or re-solves the allocation core (`cbba_run`, the bundle queues, the execution-twin sync), which is more than an
  opt-in hook, so by your rule I stop and ask. The design I would propose if you say yes is in section 8.
* **C2** `--probe-no-comm-faults`: communication-class faults are never injected (perfect comms: no link-margin
  faults), and the vbus link loss is already zero in the laptop run. Reported with the caveat that the in-process CBBA
  auction is already ideal, so this probe measures the value of comms faults on teams, not auction loss.

### Capacity yardstick

* **K1** `--probe-rest-mult=X` (rest duration multiplier: stamina recovery rate divided by X) and
  `--probe-drain-mult=X` (stamina drain multiplier), X in {0.5, 2}, on B. Also measures mean teams in service (the
  Step 4 definition). The yardstick is `s` = incidents per extra mean team in service, estimated as the least-squares
  slope of mean incidents on mean teams in service over B and the four K1 runs (per stress), with a seed-bootstrap CI.
  **Team-equivalents = headroom / s.** Because the arrival process responds to capacity, `s` is a lower-bound-ish local
  slope and I say so wherever it is used. K1 contrasts are reported but are not part of the Holm family (they are the
  yardstick, not probes).

### Road information (addendum)

Today every team knows every damaged road instantly (omniscient A*), which is also why REROUTE was a no-op. These
probes remove that omniscience for the causal road damage only (cells registered by the causal road model); other
terrain hazards remain known.

Hooks (all inactive unless `--probe-roadknow` is given): the cost function in `core/path.c` (`get_movement_cost`)
asks a planning-view function for the cost; the step in `core/causal_actions.c` (`causal_actions_crossing_stall`) is
extended so that a team whose planned next step enters a TRULY blocked cell (or cuts a truly blocked corner) is stopped
and the cell becomes observed; and a per-tick observation step records what teams see. The connectivity guard and
audit stay on the true map.

* `--probe-roadknow=omni` is the default (identical to today).
* **R1** `--probe-roadknow=obs`: a damaged cell is planned at its undamaged base cost until it is known. A cell
  becomes known when a team observes it. A team observes cells within Chebyshev radius **r = 1** (its own cell and the
  8 neighbours), which I justify as: a cell is ~100 m, and a ground crew on a road sees the next block, not further;
  r=1 is the conservative end of that. The strict reading (stall or enter only, r = 0) is run as R1s. The observation
  reports the cell's true state (damaged or clear), so clearing is also learned, and a known-damaged cell that nobody
  re-observes expires after 2400 ticks (the longest a damage lasts), so stale beliefs are possible and measured.
  Sharing: one fleet-wide picture; a report becomes visible after **2 ticks** (72 s, the beacon cadence order of
  magnitude) and is lost with probability 0.05, the same order as the consensus bus loss used in Step 3.5, and is
  always lost when the observing team has an active communication-class fault. Draws are hashes of (seed, tick, team,
  cell). Simplification stated plainly: **one shared picture, not a per-team map**, because the pathfinder API has no
  team argument and adding one is a core change; per-team knowledge differences are therefore not modelled.
* **R2** `--probe-roadknow=prior`: R1 plus a rate-aware prior on unknown cells, planned cost
  `base + pi(t) * 12` where 12 is the partial-debris cost increment and `pi(t)` is the probability that a given cell
  carries damage, from the Omori rate of recent aftershock events and the share of the grid an event can touch. This
  is the honest ceiling of what this simulator's damage process offers a predictor: the causal model places damage at
  hashed epicentres on teams' routes, so it has **no spatial structure from intensity or building density** (those
  drive a different, non-causal hazard layer). The flat prior therefore cannot tell one cell from another; the
  prediction below follows from that. What a real predictor would learn from (building damage grade, shaking intensity,
  debris and landslide susceptibility, time since the last aftershock, reported closures) and what is observable on a
  real system is in section 8.
* **R3** `--probe-roadknow=obs --probe-road-share=instant`: R1 with zero latency, no loss, and communication-faulted
  teams still reporting (broadcast on first observation). Measures the value of the reporting mechanism itself.
* Re-measurement of REROUTE and CLEAR_ROAD: the same systems with `--causal-actions=reroute,clear_road` under
  omniscient, R1 and R2 (contrasts "actions on minus actions off" in each knowledge mode).
* Also reported for R1/R2: stall-hold ticks, no-path abandons, teams stuck (held more than 8 consecutive ticks at one
  cell), stall false alarms (gate recommendations of RETURN or higher within 12 ticks after a stall hold, which Step 3
  counted as `go_home_near_stall`).
* Headroom definitions (paired, per stress): value of road information = omniscient minus R1; value of a damage
  predictor = R2 minus R1; value of better sharing = R3 minus R1.

### Measurement

* **X1** 100 seeds per set (below). **X2** secondary outcomes (descriptive, pre-registered as secondary, not in the
  Holm family): P1 (CRITICAL-priority) incidents resolved and victims lost; response-time percentiles (report to
  assignment to arrival to resolution, p50/p90); per-incident-type outcomes; worst-decile seeds (mean of the 10 worst
  seeds of B and the same seeds under the probe). They need an output-only log line `[DIAG INC]` per resolved or
  expired incident, emitted only with `--probe-inclog` (no change to the simulation).

## 5. Predictions (direction, magnitude, confidence), and what is realisable

Magnitudes are for the pooled paired difference, in incidents per seed, positive = better. Confidence is my
probability that the stated outcome happens (a probe "survives" or "does not survive" as written). These are guesses
before any data. Where Step 4 already measured something, I used it.

| Probe | Prediction | Conf. | Realisable on a real system? |
|---|---|---|---|
| D1 | Median gate detection delay of mobility and resource faults above 10 ticks (6 min); miss rate above 30% for communication faults; resource the slowest | 0.55 | Yes: needs only logged fault ground truth, which a real deployment would have from after-action review, not live |
| D2 | Headroom (PD minus P) falls as K grows; at K=40 it is indistinguishable from 0 at stress 7 (Spearman of K vs headroom at stress 7 at most -0.5) | 0.55 | The delay is a property of any detector; real detectors have delay |
| D3a | An oracle with 30% misses and 1% false alarms keeps at least half of the PD headroom at stress 7 and none material at stress 2 | 0.45 | Yes: those are plausible detector operating points |
| D3b | P's incidents fall with dropout (about -0.5 at Q=0.25, -2 at 0.75, stress 7); oracle headroom grows with Q; the shipped AI's gain grows with Q at stress 7. At stress 2 nothing survives | 0.45 | Evidence loss is realistic (sensor dropout, jamming); an AI on a separate input path is the realisable response |
| S1 level ii | Prevention at M=0.5 or 0.25 gives under +1.0 incident at stress 2; at stress 7 only M=0.25 may reach +1.0 | 0.60 for "no survivor at stress 2" | Level ii is the realistic one: a hazard forecast (wear model) is learnable |
| S1 level i | Larger than level ii by at most 1 incident | 0.40 | Not realisable (draws are unknowable) |
| M1 ii | Victims lost fall by 0.5 to 2 per seed at stress 7, incidents change by under 1 | 0.50 | Fully realisable with no AI (age and victim count are known): a rule, not an oracle |
| M1 i | Victims lost fall by 1 to 4 per seed | 0.40 | Not realisable (realised deaths) |
| M2 / M3 | Each under +0.5 incident (the events are few per seed and few teams are idle near them) | 0.60 | Fire spread is forecastable in practice (wind, fuel); collapse timing is not |
| M4 | Positive and rising with T up to 50, then flat; at most +1.5 incidents at T=50 | 0.40 | Same as M2 and M3 |
| M5 | The largest demand-side effect: +1 to +4 incidents, victims lost down 1 to 4 | 0.55 | A search or reporting system (drones, cell-phone reports) could close part of it; not an AI forecast |
| C2 | Comm-free beats P by 0.5 to 2 incidents at stress 7, about 0 at stress 2 | 0.45 | Better radios or relays; not AI |
| R1 (omniscient minus R1) | Omniscience is worth 0 to 1 incident at stress 2 (CI includes 0) and 0.3 to 1.5 at stress 7; stall-hold ticks at least double; no-path abandons rise | 0.60 for stress 2 null, 0.40 for stress 7 survival | The ceiling for any road-information system, learned or not |
| R2 (R2 minus R1) | A flat prior is a null: CI includes 0 and point under 0.3 at both stresses | 0.85 | Not realisable from this sim's damage process (no spatial structure); a real world has structure this sim lacks |
| R3 (R3 minus R1) | Better sharing is worth at most 0.3 and the CI includes 0 | 0.70 | Yes (broadcast, mesh): cheap |
| REROUTE / CLEAR_ROAD | Under R1 and R2 each moves incidents by under 0.5 (CI includes 0) at stress 2; at stress 7 CLEAR_ROAD may reach +0.5 | 0.55 | Already implemented as actions |
| K1 | Rest x0.5 and drain x0.5 raise mean teams in service by 0.3 to 1.0; incidents per extra team in service `s` between 2 and 8 | 0.45 | Operations, not AI |

Overall prediction (one line): no single probe other than possibly M5, M1 and the road-information probes clears the
materiality threshold at stress 2; at stress 7 two or three do. Confidence 0.5.

## 6. Stages and statistics

* **Screen:** every probe at stress 2 and stress 7 on **development seeds 1001-1100** (100 seeds). Paired seed-level
  differences against the matched baseline of the same stress. Paired bootstrap 95% CI, 20000 resamples, seed 0.
* **Survives the screen** if, for at least one of the two metrics (incidents resolved; victims lost, as B minus probe),
  the CI lower bound is above 0 AND the point estimate is at least the materiality threshold:
  **+1.0 incident per seed** (about 2.5% of ~40) or **1.0 victim per seed** (about 5-10% of the victims lost).
* **Confirmation:** survivors only, on **held-out seeds 3001-3100**: same sign, CI excludes 0, and a bootstrap p-value
  that passes **Holm** correction at alpha 0.05 over the whole screened family. Family size m = number of
  (probe contrast, stress, metric) tests screened (about 33 contrasts x 2 stresses x 2 metrics = 132, the exact number
  is printed in the report). Holm is valid under arbitrary dependence; unadjusted p-values are reported beside it.
* Only confirmed survivors are also run at stress 1 and 3.5 (all four stress levels, 100 seeds on both sets).
* A final pooled estimate (200 seeds) is reported for every probe, as description.
* All probes, including nulls, appear in one table in `docs/HEADROOM_MAP.md`. **No parameter is tuned to any probe**:
  every number above is fixed now.
* Headroom is also given in team-equivalents (section 4, K1).
* Added to the pre-registration at your request (Step 4): **headroom grows as teams in service falls** is tested as a
  prediction across stress levels for every confirmed survivor (rank correlation of headroom with mean teams in
  service); direction predicted: negative.

## 7. Process, rules, stop conditions

* Hooks are opt-in, in `core/`, and compile for the board (stubs where the world is not present). Default output is
  checked byte-identical (C1 md5 and all six profiles) and all existing `make check-*` plus the ARM single-board
  compile are run before the results commit.
* `ml/`, `fault_predictor*`, the planner design, Step 3.5 V / margin / calibration tables are not touched. No
  retraining, no calibration of any model.
* **Stop-and-ask list** (needs more than an opt-in hook): C1 (central allocator), a random-arrival foresight oracle
  (M4 over random arrivals), per-team road knowledge (R1 per team). These are reported as not run, with the reason,
  and the rest of the map proceeds.
* Commits as Harush Temkar `<harush.temkar@gmail.com>`, no AI trailers; push `feature/causal-faults` only, no force,
  no tags; `submission/single-board-dk` untouched.

## 8. Design notes owed (written after the results, only for probes with real headroom)

For any confirmed survivor: what a deployable learned component for that channel would need (inputs, labels, evidence
it could be learned). For C1, if you approve: replace the per-tick CBBA output with an optimal one-to-one
assignment (Hungarian, at most 10 x ~15) of idle teams to open discovered incidents under the system's own
`calculate_score`, plus a pairwise swap pass for en-route teams, behind a flag; its gap to CBBA is the value of
coordination under the system's own utility. For the dashboard (no change made): what `route.js` and the map view
would need to show what each team KNOWS versus the true map is listed in the results document.

## 9. Addendum A (2026-10-25; committed before any probe code or run)

Amendments from a review of this document and from facts checked in the code after the first commit. Nothing has been
implemented or run. These override sections 4-6 where they differ.

1. **S1 redefined (the first version measured the wrong thing).** Scaling only the cost the planner compares would test a
   planner that under-estimates prevention, not cheaper prevention. `--probe-prevent-cost=M` now scales BOTH the planner's
   cost and the real time of the preventive action it prices, by the same M. PREVENT_FIELD (in-field maintenance, no
   trip) is priced as `t_fr * u_x` with `t_fr` the field-repair duration (34-102 ticks); the probe scales that duration
   (the maintenance hold, `s_field_kind` 3) and the planner's `cost_field` together. PREVENT (rest at base) is a trip plus
   rest plus a 4-tick dwell, which has no single "maintenance cost", so it is left unscaled. S1 therefore runs with
   `--planner-prevent=both` (rest and field), M in {1, 0.5, 0.25}, at level ii (hazard) and level i (draw-aware). The
   wear reduction a maintenance gives (0.3) is unchanged.
2. **Demand responds to capacity: guard added.** New arrivals depend on available teams and hidden backlog, so
   "incidents resolved" can rise because more incidents spawned. For every probe I report incidents spawned per seed,
   victims seen per seed and the resolved share (resolved / spawned). **Rule:** any incidents-resolved survivor whose
   gain is matched by a rise in spawned incidents of at least half the gain is flagged "demand-driven" in the table, and
   the victims-lost result is the one I lean on. M5 (which removes the hidden-backlog penalty) and K1 change the arrival
   process directly and are labelled so. **K1 wording corrected:** extra capacity brings extra demand, so the slope `s`
   (incidents per extra team in service) is inflated by demand response, and team-equivalents computed from it are
   understated, not a lower bound.
3. **D1 uses the fixed-gate system.** The logged recommendation on a planner run is the planner's. D1 therefore needs F
   runs (fixed gates, no planner, no AI, v1 world) at stress 2 and 7 on dev seeds; they are added to the run list, with
   the truth sidecar and dataset kept per seed (the harness keeps `tron_causal_truth.csv` and the dataset in a per-seed
   directory instead of a temp directory for these runs only).
4. **Facts checked after the first commit.**
   * Laptop vbus loss is NOT zero: `CONSENSUS_DEFAULT_LINK_LOSS = 0.05` (decision D4). R1's report loss of 0.05 is that
     number, not a new one. C2 therefore also sets the vbus loss to 0 (`--vbus-loss=0`) in addition to suppressing
     communication-class faults; the C2 text "already zero" is withdrawn.
   * The AI's feature vector reads the execution twin (`fault_predictor_features.c`). D3b's dropout is therefore applied
     ONLY at the reads in `recovery_manager.c` and in the planner's watchdog evidence, never inside the watchdog or twin
     getters, so the AI's inputs are not degraded.
   * Exogeneity of hazard events (M2-M4) is a hypothesis, not a verified fact: a resolved incident might clear a hazard
     flag, and events are dropped when the incident pool is full (`g_diag_hazard_event_dropped_pool_full`), which
     depends on behaviour. **Before any M2-M4 policy is written,** I record events under P and under PD on 5 seeds and
     diff them. If they differ, the replay design is dropped and M2-M4 are reported as not run, with the diff.
5. **M1 applies to incident-pool tasks only** (system tasks have no victims; `calculate_score` also scores them).
6. **R1/R2/R3 mechanism diagnostics** (descriptive, so a null is interpretable): share of damaged cells fleet-known at
   onset; ticks from onset to fleet-known (median, p90); number of team-ticks planned through a cell that was damaged
   and unknown; number of teams stopped at a truly blocked cell (a stall caused by missing knowledge); stale-belief
   events (a cell planned as damaged that was already clear). Without these, "R1 about equals omniscient" could not be
   told from a broken hook.
7. **Bootstrap p-values** for Holm use `(count_at_or_beyond_zero + 1) / (B + 1)` (two-sided: twice the smaller tail,
   capped at 1), so no p is exactly 0. B = 20000.
8. **Optimisation level.** The pilot showed -O2 output identical to -O0 on one seed of one configuration. Before the
   screen I repeat the byte comparison on 3 seeds for F, P, PD and the AI binary (PA) and for the first of each new
   probe flag; any difference sends the screen back to -O0.
9. **C1 and per-team road knowledge** are on the stop-and-ask list; the question was sent to you at the start of
   implementation, and the rest of the map proceeds without waiting.

## 10. Addendum B (2026-10-25; combined Step 5 headroom map and capacity budget; committed before any probe result)

A second brief arrived while Addendum A was being implemented. It makes this one combined pass with a **priority
tier order** and results pushed after each tier. It adds probes (scale audit, time budget, teams offline, work-rate
and travel ceilings, team mix, a recovery repertoire) and renames two. Nothing has been run at scale: only single-seed
smoke tests of the hooks (to see that flags parse and the default output is unchanged). Sections 1-9 stay in force
except where this addendum overrides them.

### 10.1 Tier order, naming, push policy

| Tier | Contents | Notes |
|---|---|---|
| 0 | SA scale audit (read-only, own commit), TB time budget, D1 gate delay and miss rate | no code that changes a result |
| 1 | D2, D3a, D3b, S1, M1, M2, M3, M4, M5, DQ1, DQ2, K1, K2 | DQ2 = old C2; DQ1 = old C1 |
| 2 | W1, W2, W3, W0, V0, V1, TM1 | work efficiency and travel |
| 3 | R1, R2, R3 (+ REROUTE/CLEAR_ROAD re-measure) | road information; hooks already built |
| 4 | RR1-RR5, RR0 and the key question | **design finalised by a dated addendum before any Tier 4 run** (allowed: it depends on earlier results) |

After each tier: results table appended to `docs/HEADROOM_MAP.md` (partial), committed and pushed. If time runs out the
completed tiers are usable and the rest are listed as not run. The brief's wall-clock estimate is 4-5 days; the tier
order is the priority.

### 10.2 Facts in the brief to verify (done in SA and TB, reported either way)

Work-rate 1-8 per tick scaled by equipment level and independent of specialization (checked: `team_base_work_rate`,
`team_effective_work_rate`); stage work 100 units; no-fault time budget (resting 44 percent, en route 34, on scene 18,
available 5 on seed 1001); `CELL_SIZE_METERS = 10` unused while the causal model uses `cell_size_m = 100`; gates' delay
and miss rate never measured before D1. I verify each, quote what I find, and correct anything that is wrong. Two
facts found while building: random arrivals respond to capacity (so incidents resolved can rise from more arrivals;
guard in Addendum A item 2), and the default consensus link loss is 0.05, not zero.

### 10.3 Deviation to report (hazard-event replay)

Addendum A item 4 said M2-M4 are dropped if the hazard events under different systems differ. On 5 seeds under P, PD
and F, the event lists were identical for 2 seeds and differed by ONE extra late event in 3 seeds (about 115 events per
seed; so at most 0.9 percent). The cause is that random arrivals choose building sites and mark them, which can stop a
later fire from spreading there. I keep M2-M4 because the replay is 99 percent exact, and the probe run counts every
event it sees that the recording lacks (`event_mismatch`); a run with more than 3 percent mismatch is flagged. This is
a deliberate, disclosed departure from the literal rule, decided on validity (fidelity), not on any result.

### 10.4 New and changed probe definitions (flags `--probe-*`, causal profile only, hooks only)

* **SA** scale audit: no code. Inventory of every quantity that depends on a physical scale, implied values at 10 m,
  100 m and an "abstract, calibrate by speed" reading, map crossing time and typical trip minutes, one recommendation,
  and a list of docs and claims to correct. Own commit.
* **TB** time budget (`--probe-diag` prints it, output only): per stress and specialization, the share of team-ticks
  resting / en route to an incident / en route to rest / on scene / available / held in repair. Mutually exclusive,
  priority order: held in repair, resting, heading to rest, on scene, en route to a task, available.
* **D1** as in section 4, on fixed-gate (F) runs.
* **K1** `--probe-rest-mult=X` and `--probe-drain-mult=X`, X in {0.5, 0.75, 1.5, 2} each (1 is the baseline); the
  fit is the least-squares line of mean incidents on mean teams in service over the baseline and all eight runs, per
  stress, with a seed-bootstrap CI. Team-equivalents = headroom / slope (slope inflated by demand response, so
  team-equivalents are understated; stated wherever used).
* **K2** `--probe-offline=k`, k in {1, 2, 3}: k teams, chosen by a hash of the seed, take no assignments (excluded in
  `team_can_accept_incident`, the support picker and the planner preemption) and receive no fault injections, so they
  are idle, not faulted. They still send beacons, so the consensus vote sees them as healthy (the brief asks for
  exclusion from voting; with no fault and no motion their votes are inert, so I expect no difference, and I assert
  that offline teams finish zero incidents). No change to `NUM_TEAMS` or any layout.
* **DQ1** central omniscient allocator: `--probe-central-alloc`. After `cbba_run`, a central pass re-solves the
  one-to-one assignment of idle accepting teams to open discovered incidents (optimal assignment under the system's own
  `calculate_score` and true path costs), with a pairwise swap pass for en-route teams that have not started work.
  An opt-in branch in `allocate_all_incidents`; if on inspection it cannot be done without touching bundle or queue
  invariants it is reported as not run (the stop-and-ask rule). Reading: gap to CBBA = value of coordination under
  the system's own utility.
* **DQ2** = old C2 (no communication faults, vbus loss 0).
* **W1** `--probe-work-mult=X`, X in {1.25, 1.5, 2, 3, 4}: the work a team does per tick on scene is multiplied by X
  (single hook in `update_incident_work`; support teams too). Fatigue variants: `--probe-work-fatigue=a` (default):
  stamina and equipment drain per tick unchanged; `--probe-work-fatigue=b`: drain per on-scene tick multiplied by X
  (drain tied to work done, so the cost of one unit of work is unchanged).
* **W2** `--probe-work-spec=S:X`, one specialization S in {search, medical, extraction, perimeter, support} at a time,
  X in {2, 3}.
* **W3** `--probe-work-stage=early|late:X`, X in {2, 4}: stage 1 of an incident (debris removal, breaching) vs stage 2
  and later (extrication, medical), by a per-incident stage counter kept by the probe.
* **W0** `--probe-instant-work`: a stage completes on the tick the team is on scene.
* **V0** `--probe-instant-travel=task|all`: with `task`, a team assigned to an incident or system task is at the task
  the same tick; trips to rest keep their normal speed (reported separately). `all` makes trips to rest instant too.
* **V1** `--probe-speed=search:X,extraction:X,perimeter:X`: SEARCH and EXTRACTION teams move X cells per tick in a
  straight line ignoring roads and debris, X in {2, 4}; PERIMETER teams move 0.5 cells per tick and treat partial
  debris as a +255 path cost (soft impassable: truly impassable would need a per-team pathfinder, a core change).
  Re-measure `--causal-actions=reroute,clear_road` against the same V1 baseline.
* **TM1** `--probe-team-mix=a|b|c` replaces `specialization = i % 5` by blocks. Mixes derived by rule from the
  demand shares in `docs/SPECIALIZATION_LOAD.md`, not tuned: **a** = proportional to organic stages done, at least one
  SEARCH, no SUPPORT: SEARCH 1, MEDICAL 2, EXTRACTION 4, PERIMETER 3, SUPPORT 0; **b** = a with one SUPPORT:
  1, 2, 3, 3, 1; **c** = proportional to who actually finishes the stages (including support teams): 1, 1, 3, 3, 2.
  Default is 2, 2, 2, 2, 2.
* **R1-R3** as in section 4 (already built; the screen runs them).
* **RR1-RR5, RR0:** outline only here (finalised before running): RR1 relief/hot swap with reactive, rate-aware and
  perfect-foresight triggers (handover cost = the replacement's travel); RR2 SUPPORT-delivered mobility repair in place
  and equipment delivery (the support team's forgone tasks are counted; the equipment-cache mechanic already exists
  and I report what it does today); RR3 two-stage road clearing; RR4 capability-aware allocation for a faulted team that
  keeps working; RR5 autonomous mode for comm loss (first report what the sim does today). Sources to cite and mark
  "to verify": INSARAG guidelines (team structure), 12-hour shift rotation practice, field-robot repair figures,
  Tohoku "teeth of a comb" lane opening.

### 10.5 Predictions for the new probes (direction, magnitude, confidence, realisable?)

Magnitudes: incidents per seed, pooled, positive is better, versus the matched baseline P. Materiality threshold for
every probe: **+1.0 incident per seed or -1.0 victim lost per seed** (section 6).

| Probe | Prediction | Conf. | Realisable? |
|---|---|---|---|
| TB | Resting 44-55 percent in every world; held in repair under 3 percent at stress 2 and 5-10 percent at stress 7; available under 8 percent | 0.55 | Measurement |
| K1 | Slope between 2 and 8 incidents per extra team in service; rest x0.5 raises teams in service by 0.3-1.0 | 0.45 | Operations |
| K2 | Each team offline costs 2-6 incidents; three offline costs 8 or more | 0.5 | Measurement |
| DQ1 | CBBA is within 1.0 incident of the central allocator at both stresses (the in-process auction is already a converged greedy) | 0.55 | Laptop-only bound |
| W1 | x2 gives +2 to +6 incidents (on-scene is 18 percent of team time, so x2 frees about 9 percent); x4 less than double of x2; variant b below variant a | 0.5 | Needs tools and machinery (a v2 equipment model) |
| W2 | Single specialization x2 gives under +1.5 incidents each; EXTRACTION and PERIMETER the largest (+1 to +3) | 0.45 | As W1 |
| W3 | No stage dominates; late x4 within 1 incident of early x4 | 0.30 | As W1 |
| W0 | +6 to +15 incidents (work is 18 percent of team time but also frees the rest-cycle) | 0.45 | Ceiling |
| V0 | Larger than W0: +8 to +20 (en route is 34 percent) | 0.5 | Ceiling |
| V1 | Search/extraction x2 gives +1 to +3; perimeter x0.5 costs -1 to -4; REROUTE and CLEAR_ROAD move incidents by under 0.5 | 0.45 | Needs aerial or off-road assets |
| TM1 | All three mixes within +-1.5 incidents of the default | 0.5 | Free (configuration) |
| RR key question | With all five responses available, the detector-oracle headroom (PD-P) stays under +1.0 incident at stress 2 | 0.55 | Depends on the response |

Overall: capacity and speed levers (K, W, V) dominate information levers (D, S, M) by a factor of at least 3 in
incidents per team-equivalent. Confidence 0.6. This is the claim the combined map tests.

### 10.6 Statistics, restated

Unchanged: screen at stress 2 and 7 on 1001-1100; survival = paired CI lower bound above 0 and point estimate at least
the materiality threshold on at least one metric; confirm on 3001-3100 with same sign, CI excluding 0 and Holm over
the whole screened family (every contrast x stress x metric screened, size printed in the report; unadjusted
p-values beside it); survivors also at stress 1 and 3.5; every probe in one table; no parameter tuned to a probe.
Headroom is also expressed in team-equivalents using the K1 slope of the same stress. Secondary outcomes (P1
incidents and victims, response-time percentiles, per-type outcomes, worst-decile seeds) are reported, not tested.

## 11. Addendum C (2026-10-25; road-knowledge propagation sensitivity; committed before any run of these configurations)

Request: measure how sensitive the R1 result is to the simplification that road reports reach the whole fleet through a
shared table. Run after Tier 3; it does not delay the other tiers (queued behind them). No new hook is needed: the
sensitivity uses the flags already built (`--probe-road-latency`, `--probe-road-loss`, `--probe-road-radius`).

### 11.1 What the implemented propagation model is (to be documented as such in `docs/HEADROOM_MAP.md`)

One shared table (`core/road_knowledge.c`), laptop-only, not using `comms_transport`, the vbus or any frame. Each tick
every team observes the cells within `radius` (Chebyshev) of itself. If the observed state of a cell differs from the
fleet picture, a report is made. The report is lost if the observing team has an active communication-class fault
(the only link to the comms layer) or with probability `loss` (a hash of seed, tick, team and cell); otherwise it is
delivered at tick + `latency` to all teams at once (due-tick delivery, one pending slot per cell). A believed damage
that nobody re-observes expires after 2400 ticks. Not modelled: per-team maps, hop-by-hop relaying, distance-dependent
delay, scouting, aerial observation, terrain or smoke visibility.

### 11.2 Configurations (one at a time around the R1 default: radius 1, latency 2, loss 0.05)

| Axis | Values | New runs (the default R1 and the strict R1s are in Tier 3) |
|---|---|---|
| latency | 0, 2, 10, 30, 100 ticks | 0, 10, 30, 100 |
| loss | 0.05, 0.2, 0.5 | 0.2, 0.5 |
| radius | 0, 1, 2, 4 cells | 2, 4 (0 is R1s) |

Each setting is also run with `--causal-actions=reroute,clear_road` against the same setting with actions off. Stress 2
and 7, development seeds 1001-1100; survivors confirmed on 3001-3100. Value of road information at a setting =
omniscient (P) minus the setting, as in R1; R3 (instant, no loss, communication-faulted teams also report) is the ceiling
of this sensitivity. Radius is a visibility or scouting proxy, not a propagation parameter, and is reported as such.
Mechanism diagnostics reported for every setting: onset-to-known delay (mean, max), reports made and lost, blocked stops,
unknown partial-debris entries, stale cell-ticks. "Mean report delay" is the onset-to-known delay, which includes the
time until some team first observes the cell.

### 11.3 Predictions (value = omniscient minus setting, incidents per seed; positive = information matters)

| Question | Prediction | Conf. |
|---|---|---|
| Latency 0 to 30 | value is flat: the points differ by under 0.5 incidents and the CI of each includes the others | 0.6 |
| Latency 100 | value is larger than at 0-30 by at least 0.5 at stress 7 | 0.4 |
| Loss 0.05 to 0.5 | flat at stress 2; a rise of 0.3-1.0 at stress 7 | 0.45 |
| Radius 0 to 4 | value falls with radius (more scouting, less loss from not knowing) at stress 7 | 0.5 |
| Actions (REROUTE, CLEAR_ROAD) | no setting moves incidents by more than 0.5 | 0.55 |
| Overall | the shared-picture simplification does not matter for any claim about the sign of the value; the value stays under the +1.0 materiality threshold at stress 2 | 0.55 |

Decision rule stated now: if the value is flat across latency 0-30 and loss 0.05-0.5 (all pairwise differences under
the materiality threshold of 1.0 incident or 1.0 victim, with the CIs overlapping), the report says the simplification
does not matter at this world's damage rate. If it falls or rises by more than the threshold over those ranges, the report
says a real propagation model is needed before any claim about road information is made. Holm: the new contrasts
(8 settings x omniscient-minus-setting, plus 8 actions contrasts, x 2 stresses x 2 metrics = 64 tests) join the family.

### 11.4 Design note owed (no implementation)

Beacon-piggyback route versus dedicated message over `comms_transport`: beacon and frame budgets found in the code,
per-team memory, `path.c` team argument, state hash, mission-link wire format, determinism, board build, dashboard needs,
a recommendation, and the ceiling value that would justify building it. Written after the results, in
`docs/HEADROOM_MAP.md`.

## 12. Addendum D (2026-10-25; Tier 4 recovery repertoire design, and process rules from your answers; committed before any Tier 4 run)

### 12.1 Decisions received and how they are applied

1. **DQ1** stays an opt-in branch, labelled an **upper bound only**, and is compiled out of the board build
   (`core/headroom_probes.c`: the whole central allocator is inside `#if ENVIRONMENT_HAS_WORLD_STORAGE`, with an empty
   stub otherwise). Proven by the ARM single-board compile and the byte-identity checks before Tier 4 and before the
   final report.
2. **Road knowledge as one shared picture** is accepted. `docs/HEADROOM_MAP.md` states that it **understates ignorance**
   (everyone knows everything after `latency` ticks), so the value of road information is a **lower bound**.
3. **RR2a** may preempt a SUPPORT team's own task, priced and counted (below). If it still never fires it is reported as
   a null with the reason (SUPPORT is never idle: capacity). No parameter is tuned to make it fire.
4. **RR3** uses no new per-cell state: "open a lane" = a fully blocked cell becomes partial debris at about 30 percent of
   the full clearing work; the normal CLEAR_ROAD triggers clear the rest later.
5. **Process:** byte-identity vs the frozen build (six profiles, C1 md5, and the planner, recovery-action and hold-repair
   causal variants), the full `make check-*` suite and the ARM single-board compile are re-run before Tier 4 at scale and
   again before the final report. Every result records the binary hash; frozen binaries are used per tier. The integrity
   of the results already written was verified (run counts, duplicates, partial files, mixed binaries; see
   `docs/HEADROOM_MAP.md`, provenance). One compact per-run CSV per tier is committed with the reports. No probes beyond
   the pre-registered list.

### 12.2 Tier 4 definitions (flags `--probe-rr=LIST`; tokens `relief`, `relief-foresight`, `support-repair`, `degraded`,
`autonomy`, `lane`, `all`)

* **RR1 relief / hot swap.** A team on scene with work left, no support, and a replacement that can reach the incident:
  the replacement is sent as support early, and when it is on scene the roles swap if the tired team is within 8 ticks
  of the gates' go-home levels (stamina critical 10, equipment critical 10). Trigger **rate-aware** (`relief`): the
  tired team's time to exhaustion (stamina over the on-scene drain 0.33 per tick; equipment over the mean draw of 3)
  is at most the replacement's travel time plus 1. **Perfect foresight** (`relief-foresight`): the same, or a realised
  mobility or resource fault onset (the hash draws, current hazard) within the replacement's travel time. **Reactive**
  (call a replacement when the gates already say go home) is exactly today's behaviour (the incident reopens and the next
  auction assigns it), so it is reported analytically and not run. Candidates: idle or rested teams that can accept
  the incident; the best by the system's own score. The handover cost is the replacement's travel time, which is real.
* **RR2a support-delivered repair.** A faulted working team (mobility or resource fault, not already held, not heading
  home) is repaired in place by a SUPPORT team: the team is held from the commit for the support team's travel plus the
  repair (the field-repair duration 34-102 ticks), the repair succeeds with probability **0.9** (assumed; a self repair
  is 2/3; to verify), and the support team is committed for the same time and moved to the faulted team. An **idle**
  SUPPORT team is committed for free. If none is idle, the Step 3.5 preemption primitive prices it
  (`pick_preempt_team` with `support_only`): benefit = the faulted team's best response without help (stay, go home or
  self repair, from `persist_costs`) minus holding it for (reference travel + mean repair time) at its task's urgency
  minus the 10 percent that still fails; cost = the support team's switch cost, the margin and the last-specialist
  approval of Step 3.5. The preemption counts (`rr_preempt_considered`, `rr_preempt_done`) and the work left on the
  preempted tasks (`rr_forgone_work`) are reported. **Equipment delivery (RR2b)** already exists in the baseline
  (a SUPPORT team delivers an equipment cache to a depleted team); it is reported as what the sim does today and not
  changed.
* **RR3 lane.** With `--causal-actions=clear_road`: a CLEAR_ROAD task on a fully blocked cell needs 30 percent of the full
  clearing work (72 of 240 units) and, when done, turns the cell into partial debris (passable at the debris cost); the
  partial cell is cleared later by the normal triggers. Counted (`rr_lanes`).
* **RR4 degraded mode.** A working team with a mobility or equipment fault keeps its task (the gates' mobility and
  equipment signals are not acted on while it works and has equipment left; the stamina and progress evaluators still
  act), and bids and scores are multiplied by its capability (effective over nominal work rate, and 1/2 or 1/3 for the
  mobility-fault movement penalty): capability-aware allocation.
* **RR5 autonomy.** A communication-fault verdict does not release the team's task; it keeps working its last
  assignment. What the sim does today: a comm fault makes the gates recommend REQUEST_TASK_REALLOCATION (or ABORT at
  severe) and cuts the team's CBBA broadcast cadence to 1 in (severity + 1) ticks.
* **RR0** = `relief` + `support-repair` + `degraded` + `autonomy` + `lane` (with `--causal-actions=clear_road`), **not
  priced jointly by the planner** (a joint pricing is a planner design change, beyond a hook; stop-and-ask item). Each
  component keeps its own rule above.

### 12.3 Key question and contrasts

Systems: P, P + RR0, PD (detector oracle), PD + RR0, and the hazard oracle with prevention (`S1_ii_1`) with and without
RR0, at stress 2 and 7, dev seeds 1001-1100, screened and confirmed as in section 6; each RR component alone against P.
Key question: **does the detector-oracle headroom (PD minus P) grow when the cheaper responses are available**
(PD+RR0 minus P+RR0 versus PD minus P)? Prediction: no; it stays under +1.0 incident at stress 2 (confidence 0.55);
RR2a is a null because SUPPORT teams are rarely idle (about 2-4 percent of their time), with the preemption path firing
a handful of times per seed at most (0.5); RR4 and RR5 move incidents by under 1.0 (0.5); RR1 under 1.0 (0.6); RR3 under
0.5 (0.6). Holm: the new contrasts (7 RR contrasts + 3 key-question contrasts, x 2 stresses x 2 metrics = 40 tests)
join the family.

## 13. Addendum E (2026-10-26; correction of the "road actions on vs off" contrast; committed before any run of the corrected arm)

**Finding.** The pre-registered way of switching REROUTE and CLEAR_ROAD on and off (`--causal-actions=reroute,clear_road`
against none) is **vacuous under the planner**. With `--recovery=utility` the planner enables REROUTE, CLEAR_ROAD,
REINFORCE and RELAY itself (`[DIAG ACTIONS] enabled_mask=15`) whatever `--causal-actions` says. Verified on a seed at
stress 7: the flag absent, `off`, and listing the two actions give byte-identical results (27 incidents, 15 victims lost,
identical counters), and the two V1 "actions on vs off" contrasts of Tier 2 came out as exactly +0.00 [+0.00, +0.00] on
all 200 seed-pairs for the same reason. The baseline P of every contrast in this map therefore **always has the planner's
task actions on**; my statement in Addendum A that the v1 world has "recovery actions off" was true only of the fixed
gates, not of the planner arm used as baseline. (Step 3.6 and Step 4 numbers are unaffected: they were runs of the same
configurations.)

**Corrected design.** The "actions off" arm is the planner with Layer A only (`--planner=team`), for which
`enabled_mask = 0` and no task action is ever dispatched (verified). The effect of the actions is
**baseline (actions on) minus the Layer-A-only arm**, for each knowledge setting (omniscient, R1, R2, the eight Addendum C
settings, and V1 x2 and x4). Limit stated now: the planner's Layer B switches **all four** actions together, so the
contrast measures REROUTE, CLEAR_ROAD, REINFORCE and RELAY jointly; separating REROUTE and CLEAR_ROAD from the other two
would need a change to the planner's layer switches (a core change beyond a hook; not done). In the code the OFF arms keep
the legacy names ending `_act` (and `ACT`) so queued invocations still resolve; the V1 `_act` results already written
under the vacuous definition were deleted and are re-run. RR3's comparison base is P (CLEAR_ROAD is already on), so
`RR3_base` is a duplicate of P and is not used.

**Prediction for the corrected contrast** (Layer B on minus off, incidents per seed): within +-1.0 at both stresses under
every knowledge setting (confidence 0.5); not below -1.0 (0.6). Holm: the corrected contrasts replace the vacuous ones in
the family (same number of tests).

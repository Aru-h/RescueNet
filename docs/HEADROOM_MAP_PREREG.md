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

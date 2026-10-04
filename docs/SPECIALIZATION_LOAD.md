# Specialization load (measurement only)

Question: is PERIMETER (2 teams, required by 5 of the 11 incident archetypes)
the bottleneck? No allocation or archetype was changed; this is a measurement.

**Setup.** Causal profile, `--fault-stress=7`, roads on, `--causal-actions` at its
default (off), AI predictor off (the `./tron` build), seeds 1001-1020, 3000 ticks.
Counters were added to a scratch copy of the sources
(`laptop/tools/investigations/spec_load.patch`; nothing in `core/` changed). That
instrumented build reproduces the recorded per-seed incidents resolved and victims
lost of the roads-on, actions-off run exactly on all 20 seeds (40.10 and 13.3 per
seed). Report script: `laptop/tools/investigations/spec_load_report.py`.

## A correction to the premise: incidents are multi-stage

The required specialization changes at every stage (`complete_incident_stage()`,
`core/incident.c`): a structural collapse goes PERIMETER, then MEDICAL (60%) or
EXTRACTION, and so on; fire goes PERIMETER then EXTRACTION; confined space ends in
MEDICAL or EXTRACTION. So the archetype table (PERIMETER on 5 of 11 types) is only
the *first* stage. **EXTRACTION is not only evacuee transport:** it is the required
specialization of the last stage of most incidents. All figures below count a
waiting incident under the specialization its *current stage* needs.

Work per seed (mean), 40.1 incidents resolved:

| Specialization (2 teams each) | Organic stages done | System-task stages | Required by |
|---|---|---|---|
| SEARCH | 4.7 | 0.0 (plus about 7 open sweep tasks standing, see below) | unsearched sector, survivor signal |
| MEDICAL | 14.9 | 0.0 | mass casualty, mid-chain stage |
| EXTRACTION | 36.5 | 4.0 (evacuee runs) | final stage of most chains |
| PERIMETER | 34.8 | 0.0 | first stage of collapse, confined space, chemical, gas, fire |
| SUPPORT | 0.0 | 0.1 (equipment cache) | nothing organic |

Per specialist team that is about 17 stages for PERIMETER, 18 for EXTRACTION,
7.5 for MEDICAL and 2.4 for SEARCH.

## Queue, wait and utilisation

Queue = discovered incidents with status OPEN, sampled every tick (60,000
seed-ticks pooled). Wait = from becoming open (discovered, a new stage, or
re-opened after a team was pulled off) to leaving OPEN, in ticks (1 tick = 36 s);
it includes the one-tick allocation cycle. Utilisation = share of the
specialization's team-ticks.

| Specialization | Queue mean | Queue p90 | Ticks with a queue | Wait mean | Wait p90 | Working | Returning to rest | Resting | Available |
|---|---|---|---|---|---|---|---|---|---|
| SEARCH | 0.06 | 0 | 5.1% | 17.1 | 34 | 45.3% | 13.7% | 40.0% | 1.0% |
| MEDICAL | 0.64 | 2 | 35.4% | 44.1 | 104 | 36.7% | 10.4% | 50.2% | 2.7% |
| EXTRACTION | 1.86 | 5 | 60.7% | 40.1 | 99 | 37.9% | 14.3% | 45.6% | 2.3% |
| PERIMETER | 1.89 | 6 | 48.1% | 41.6 | 96 | 35.1% | 11.7% | 51.3% | 1.9% |
| SUPPORT | 0.00 | 0 | 0.0% | n/a | n/a | 31.6% | 9.6% | 56.8% | 1.9% |

Open system tasks (not in the table's queue): SEARCH 7.2 on average (p90 10; the
standing stale-district sweep tasks), EXTRACTION 2.0 (evacuee runs), others about 0.
Wait episodes per seed: PERIMETER 139, EXTRACTION 143, MEDICAL 45, SEARCH 10; with
about 35 stages each that is roughly 4 episodes per stage, consistent with teams
being pulled off and the stage re-opening. I did not trace why.

Fog of war dominates the time to first response: spawn to discovery averages 226
ticks (p90 672) for 42.6 organic incidents per seed, against about 40 ticks of
queue wait per stage.

## Who actually does each stage

Mean stages per seed. Rows are the specialization the stage required, columns the
specialization of the team that finished it.

| Required \ done by | SEARCH | MEDICAL | EXTRACTION | PERIMETER | SUPPORT |
|---|---|---|---|---|---|
| SEARCH | 1.9 | 1.1 | 0.5 | 0.8 | 0.4 |
| MEDICAL | 0.0 | 10.2 | 0.0 | 0.0 | 4.8 |
| EXTRACTION | 9.2 | 0.0 | 23.0 | 2.6 | 1.6 |
| PERIMETER | 0.3 | 0.0 | 6.0 | 17.6 | 10.8 |

The allocator lets any team take a stage (matching specialization only scores
higher). SUPPORT teams, which have no organic work of their own, finish 10.8 of the
34.7 PERIMETER stages (31%), 4.8 of the 15.0 MEDICAL stages and 1.6 EXTRACTION
stages; SEARCH teams finish 9.2 of the 36.4 EXTRACTION stages (25%). The two
PERIMETER teams finish 17.6 of 34.7 PERIMETER stages (51%).

## Outcomes by specialization

Victims lost is the survival-model death count (13.3 per seed). It is attributed
two ways: to the incident's archetype specialization, and to the specialization
its *current stage* required when the victim died.

| Specialization | Stages done | Resolved (by archetype) | Victims seen (archetype) | Lost (archetype) | Lost (stage at death) | Alive, unresolved at end |
|---|---|---|---|---|---|---|
| SEARCH | 4.7 | 4.3 | 13.5 | 0.25 | 0.10 | 2.1 |
| MEDICAL | 14.9 | 1.9 | 25.6 | 1.10 | 2.25 | 5.2 |
| EXTRACTION | 36.5 | 0.0 | 0.0 | 0.00 | 4.00 | 0.0 |
| PERIMETER | 34.8 | 33.9 | 171.9 | 11.95 | 6.95 | 8.1 |
| SUPPORT | 0.0 | 0.0 | 0.0 | 0.00 | 0.00 | 0.0 |

Most victims (172 of 211 seen) are on PERIMETER-archetype incidents, so most
deaths are too (11.95 of 13.3). By the stage a victim was waiting at, 52% died at a
PERIMETER stage, 30% at an EXTRACTION stage, 17% at a MEDICAL stage.

## Finding

**PERIMETER is not the single bottleneck; PERIMETER and EXTRACTION are equally
loaded, and the fleet as a whole is rest-limited.**

* PERIMETER and EXTRACTION stages are 71.3 of 90.9 organic stages per seed (78%),
  with near-identical queue (mean 1.9, p90 5-6), wait (40-42 ticks mean, p90 96-99)
  and utilisation (35% and 38% working). Both have a queue on about half the ticks
  or more.
* Every specialization is resting about half the time (40-57%) and available 1-3%
  of the time. The limit shared by all of them is rest/recovery cycles, not one
  specialization's head-count.
* The 2-team PERIMETER pool understates effective PERIMETER capacity, because
  other specializations (SUPPORT 10.8, EXTRACTION 6.0, SEARCH 0.3) finish 49% of PERIMETER stages.
* The premise "EXTRACTION only does evacuee transport" does not hold: EXTRACTION
  finishes 36.5 organic stages per seed, more than any other specialization.

Caveats: one seed set (20 seeds), AI off, a descriptive measurement with no
intervention, so it shows where load sits, not what adding a team would change.
Mean queue and wait are per-stage; the stage-by-stage chain means an incident's
total time in queue is the sum over its stages.

Step 3.2 re-run with the plumbing fixes on (same seeds, same default configuration): every
figure above is unchanged, because no detector acts in that configuration. With a perfect
environment detector the fixes restore teams' working time from 10.8% to the baseline's
35.4% (`docs/CAUSAL_PLUMBING_FIXES.md`).

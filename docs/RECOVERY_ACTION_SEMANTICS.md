# What ends a fault, and what the recovery actions really do (measurement only)

Nothing in `core/` behaviour was changed for this section. Two read-only additions support it:
per-cause episode counters in `core/fault_injector.[ch]` (statistics only: not hashed, not in
the dataset) and a `[DIAG FAULTEND]` line printed under `--diag` for the causal profile only
(other profiles print nothing new). Scripts: `laptop/tools/fault_end_summary.py`
(sums `[DIAG FAULT]` / `[DIAG FAULTEND]`), `laptop/tools/fault_end_report.py` (dataset-based
cross-check on one seed).

Setup: causal profile, `--fault-stress=7`, roads on, all four causal fixes on, seeds 1001-1020,
3000 ticks (1 tick = 36 s), per-seed means over 20 seeds.

## 1. What ends an active fault, per class

Source: `causal_clear()` and `causal_cause_removed()` in `core/fault_injector.c`, plus
`fault_injector_clear_cell_faults()` for CLEAR_ROAD.

| Class | Ended by the cause being removed | Ended by the injector timer (backstop, drawn at onset) |
|---|---|---|
| Mobility | The team RESTS at a safe zone for `CAUSAL_MOBILITY_REPAIR_REST_TICKS` (4) ticks (field repair; also removes a fraction of the wear). RESTING only ever happens at a safe zone, so this needs a go-home. | Duration drawn at onset (6-14 ticks). Leaves the wear, so the hazard stays where it was. |
| Resource | RESTING with equipment >= 90 and stamina above the soft floor (resupply). Needs a go-home. | Same, 6-14 ticks. |
| Communication | Link margin back above `comm_clear_hysteresis_db`. **No timer at all.** The team has to move to better coverage, or a RELAY has to raise the margin. | none |
| Environment | `CLEAR_ROAD` finishing on the cell (`fault_injector_clear_cell_faults`). | The road-damage persistence (600-2400 ticks drawn per damaged cell). |
| Mission | The incident is resolved, or the team is no longer assigned to it (e.g. it was sent home). | Drawn at onset (~8 ticks). |

Measured (fixed gates = default `--recovery=fixed`, AI off; planner = `--recovery=utility`
no AI; oracle = planner with the oracle fault probability). "Shortened by" is the drawn
backstop length minus the actual length, for episodes that ended by cause. It is **not**
meaningful for communication, which has no timer (the "planned" length is drawn but never used).

| Class | Config | Onsets/seed | Ended by cause | Ended by timer | Mean length (all) | Mean length (cause) | Mean length (timer) | Cause-ended shortened by |
|---|---|---|---|---|---|---|---|---|
| Mobility | fixed | 173.9 | 72.5 (42%) | 101.2 | 7.61 | 4.30 | 9.98 | 5.9 |
| Mobility | planner | 170.8 | 72.8 (43%) | 97.5 | 7.49 | 4.29 | 9.88 | 5.9 |
| Mobility | oracle | 171.3 | 72.7 (43%) | 98.0 | 7.52 | 4.30 | 9.91 | 5.9 |
| Resource | fixed | 148.4 | 35.5 (24%) | 112.7 | 8.06 | 2.15 | 9.93 | 8.1 |
| Resource | planner | 143.8 | 37.1 (26%) | 106.3 | 7.91 | 2.15 | 9.92 | 8.2 |
| Resource | oracle | 146.7 | 38.0 (26%) | 108.5 | 7.95 | 2.21 | 9.95 | 8.2 |
| Communication | fixed | 119.3 | 119.3 (100%) | 0 | 3.18 | 3.18 | n/a | n/a |
| Communication | planner | 111.5 | 111.4 (100%) | 0 | 3.11 | 3.11 | n/a | n/a |
| Environment | fixed (actions off) | 17.9 | 0 | 12.2 | 1390.6 | n/a | 1390.6 | n/a |
| Environment | planner (CLEAR_ROAD on) | 24.4 | 11.1 (55%) | 9.2 | 798.3 | 367.1 | 1316.2 | 1136.8 |
| Environment | oracle | 23.2 | 10.7 (55%) | 8.8 | 803.0 | 351.3 | 1352.2 | 1189.2 |
| Mission | fixed / planner / oracle | 2.0 / 2.3 / 2.2 | 0.6 / 0.6 / 0.7 | 1.4 / 1.7 / 1.6 | 6.9 / 7.1 / 7.7 | 4.1 / 5.1 / 5.4 | 8.1 / 7.8 / 8.7 | 4.8 / 4.3 / 3.8 |

(Fault episodes on open environment faults are cut off at the end of the run, so the
environment "mean length" for episodes that ended is a lower bound on the true duration.)

## 2. Does a recovery action shorten a fault, and does it change the outcome?

* **Mobility and resource faults: yes in duration, but the share does not depend on the
  detector.** A go-home (rest at a safe zone) ends 42-43% of mobility and 24-26% of resource
  faults by cause, about 6 and 8 ticks earlier than the timer would have. That share is the same
  for the fixed gates, the planner and the planner with a perfect fault probability (oracle):
  teams go home on their normal rest cycle in all three, and the extra go-homes a better
  detector requests change almost nothing about how faults end. A faster or better detector
  therefore does not change how long mobility or resource faults last.
* **The cost of that shortening is large compared with the gain.** A fault the timer would
  end in 6-14 ticks is "shortened" by sending the team home, which costs a round trip of
  roughly 40+ ticks plus the rest. The only durable gain is the wear removed by the field repair
  (`wear_repair_reduction`), which lowers future hazard. The planner's cost model reaches the
  same conclusion: it chooses a go-home on cost alone only about once per seed, against about 125
  rule-based go-homes (`docs/RECOVERY_PLANNER_RESULTS.md`).
* **Communication: the team can change the outcome.** There is no timer; the fault ends only when
  the link margin recovers, i.e. when the team moves (or a RELAY raises the margin). Mean length
  is about 3 ticks.
* **Environment: the only class where an action changes the outcome a lot.** Without CLEAR_ROAD a
  damaged road stays damaged for ~1400 ticks on average (the persistence draw). With CLEAR_ROAD
  55% of faults end by an action, on average ~1100 ticks before the timer.
* **Mission: ends when the team leaves or the incident resolves.** A go-home ends it by
  construction (the team no longer works the incident), which is also the cost of the action.

**Plain answer.** In the current model a team-level response can change a fault's outcome only
in two ways: (a) a mobility/resource fault can be ended 6-8 ticks early by going home, which
costs far more working time than it saves and which every detector (including a perfect one)
does at the same rate; (b) an environment fault, which no team-level recovery action touches,
ends early only through the map actions (CLEAR_ROAD, and REROUTE avoids it). For mobility and
resource faults, and for communication faults too, the fault's length is set by the injector
(timer or link geometry) and the response mainly changes what the team loses while it waits.

## 3. Recovery-action semantic bugs found (not fixed in this step)

`recovery_dispatch()` is `core/recovery_manager.c:1921`. Its action cases:

| Case | Lines (current tree) | Behaviour |
|---|---|---|
| `LOCAL_RECOVERY` | 1955-2003 | AVAILABLE team only: `abandon_task_for_recovery(team)` (line 2001). |
| `RETURN_TO_SAFE_ZONE` | 2005-2020 | Not RESTING: `abandon_task_for_recovery(team)` (line 2019). |
| `REQUEST_TASK_REALLOCATION` | 2022-2029 | `allocate_all_incidents()` once per tick (line 2025). Nothing else. |
| `ABORT_MISSION` | 2030-2041 | EN_ROUTE or ON_SCENE: `abandon_task_for_recovery(team)` (line 2039). |

### Bug 1: ABORT_MISSION is indistinguishable from RETURN_TO_SAFE_ZONE
Both call the same `abandon_task_for_recovery()` (`core/incident.c:2545`). That function
reopens the incident (or the support slot), clears the assignment, calls
`release_queued_incidents()` and starts a return to a safe zone. Nothing marks the abort
as different (no quarantine of the task, no longer penalty, no operator hand-off), and
LOCAL_RECOVERY on an AVAILABLE team is the same call again. The only difference between ABORT
and RETURN is which statuses they act on: RETURN also acts on an AVAILABLE team, ABORT does
not, so ABORT is strictly a subset of RETURN. The ladder therefore has two rungs
(`ABORT_MISSION` and `RETURN_TO_SAFE_ZONE`) that act identically on a working team, and the
planner's "ABORT is dominated by RETURN" follows from the dispatcher, not from the cost model.

### Bug 2: REQUEST_TASK_REALLOCATION never releases the task
The case only re-runs `allocate_all_incidents()` (line 2025). That function already runs
every tick (`core/sim_runner.c:140`) and only assigns OPEN work to teams that have nothing to
do. The faulted team keeps its assignment, status and path, so the action is equivalent to
CONTINUE_MONITORING on the host; on a board it adds a redundant CBBA round
(`core/recovery_manager.c:88-97` comment: one reallocation per tick for all teams). The name
promises "give this team's task to someone else", which would need the task released
(`assigned_team` cleared, status back to OPEN) *without* sending the team home.
`planner_release_team_for_preempt()` (`core/incident.c`, added in Step 3.5 for preemption)
already does exactly that release.

### Where these exist
* **Default (non-causal) profiles:** yes. `recovery_dispatch()` does not look at the fault
  profile, and none of the profiles override it. I did not measure the effect there.
* **Submitted build (`origin/submission/single-board-dk`):** yes, identical code, at
  `core/recovery_manager.c` lines 1873 (LOCAL), 1891 (RETURN), 1897 (REALLOC),
  1911 (ABORT). `git diff origin/submission/single-board-dk -- core/recovery_manager.c`
  shows only the Step 3.5 planner hook, nothing in `recovery_dispatch()`.
* **Causal profile:** yes, same code. Step 3.2 added "environment faults never recommend
  RETURN_TO_SAFE_ZONE" in the evaluator, not in the dispatcher.

### Proposed fix (not applied)
1. `REQUEST_TASK_REALLOCATION`: call `planner_release_team_for_preempt()`-style release
   (task reopened, progress kept, team keeps position and status AVAILABLE) before
   `allocate_all_incidents()`, so the next allocation can give the task to someone else. Guard
   with a cooldown so the same task is not bounced every tick.
2. `ABORT_MISSION`: make it differ from RETURN, or remove it. Either (a) quarantine the task
   for N ticks (`unreachable_streak`-style) and do not requeue it for this team, or (b) drop the
   enum value in a later schema bump and map its evaluators to RETURN. Do not change the
   `RecoveryAction_t` enum values in place (dataset schema).
3. Update README/KNOWN_LIMITATIONS to say what each action does today, and add a regression
   check that the dispatch effects of the four actions are pairwise distinct.
These change behaviour in every profile, so each needs its own byte-identity and result
re-baselining, which is why they are not part of this step.

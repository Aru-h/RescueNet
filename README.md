# RESTING<->EN_ROUTE bounce fixes

4 files changed, all in `core/`: `recovery_manager.c`, `mission_execution_twin.c`,
`team.c`, `allocator.c`. Diffs are the `.diff` files; full fixed sources are
the `.c` files (drop them into `core/` and rebuild with `make tron`).

## What each fix addresses

1. **`recovery_manager.c` - stale escalation surviving reassignment.**
   The recovery manager's escalation/hysteresis state is deliberately sticky
   across ticks (anti-flap). But it could survive a team's assignment
   genuinely changing (a "steal a resting team" reassignment, or a natural
   RESTING->AVAILABLE promotion) and get blindly re-applied, cancelling a
   brand-new legitimate assignment. Fix: track each team's last assignment/
   status and reset escalation to CONTINUE_MONITORING when either changes.
   This is the fix for the original repro: `./tron 7 1 5000`, team 8,
   ticks ~126-130.

2. **`mission_execution_twin.c` - one-tick "double advance" on mission reset.**
   When a team's mission twin resets (new assignment), it correctly syncs
   `expected` to the team's real position, then *unconditionally* advances
   the expected position one more step the same tick - permanently putting
   `expected` one tick ahead of `actual` for the rest of the mission. Invisible
   noise on long missions, but a large fraction of total mission length on
   short ones - enough to trip `evaluate_progress()`'s deviation threshold and
   force a false RETURN_TO_SAFE_ZONE. Fix: skip that extra advance on the
   exact tick of reset.

3. **`recovery_manager.c` + `team.c` + `allocator.c` - fault-driven promotion
   race (only shows up under active fault injection, e.g.
   `--fault-profile combined`).**
   - `recovery_manager.c`: `evaluate_mobility`, `evaluate_communication`, and
     `evaluate_equipment` had no exemption for a team already RESTING/heading
     to rest, so they kept re-deriving the same "go to safe zone" signal every
     tick a fault was active, even once redundant.
   - `team.c` / `allocator.c`: the real bug. `team_set_available()`'s RESTING
     -> AVAILABLE promotion (and the separate "steal a resting team for a
     CRITICAL incident" path in `allocator.c`) only ever checked stamina/
     equipment - both blind to an independent, still-active mobility/
     communication/resource fault. The moment either path promoted/stole a
     team, the recovery manager's now-fresh (correct, not stale) recommendation
     sent it right back, faster than the fault could clear. Fixed by gating
     both on `recovery_manager_get_recommendation()`, checking for **both**
     `RETURN_TO_SAFE_ZONE` and `LOCAL_RECOVERY` (the latter gets the identical
     dispatch treatment once a team reaches AVAILABLE - confirmed by reading
     `recovery_dispatch()` itself. This took two iterations: the
     `RETURN_TO_SAFE_ZONE`-only version had **zero measured effect**, because
     `recovery_manager` downgrades that action to `LOCAL_RECOVERY` for a team
     still in the RESTING/RECOVERY stage - so the literal value the gate
     checked for essentially never appears for the population it needed to
     catch).

## How to test

Bounce-detection approach used throughout this session: run with `--trace`,
watch for a team's status going `RESTING(3) -> anything -> RESTING(3)` again
within a handful of ticks. A python sketch (`bounce_check.py` logic, not
included here) parsed:

```
[DIAG] tick=<n> team=<id> status <a>->3
[DIAG] tick=<n> team=<id> status 3-><a>
```

and flagged any team whose RESTING exit and next RESTING re-entry were within
a small tick window (I used 6). Recommend testing:

- **No fault profile** (`./tron <seed> 1 <ticks>`), many seeds: should be
  **zero** bounces after fixes 1+2 alone - this was the original ask and was
  confirmed clean across 15 seeds / 4000 ticks each in this session.
- **`--fault-profile combined`**, many seeds/ticks: won't be fully zero (some
  legitimate fault-driven RESTING<->EN_ROUTE activity is arguably correct
  behavior while a fault is genuinely unresolved), but should be dramatically
  lower than the unfixed baseline. In this session: seed 1, 4000 ticks went
  1340 -> 335 (fixes 1+2) -> 297 (evaluator carve-outs alone, ~no help) -> 115
  (once the team.c/allocator.c gates correctly checked LOCAL_RECOVERY too).
  That last number was **not** verified at scale - this is exactly the piece
  worth testing on more seeds/ticks than I had budget for here.

Things worth specifically checking at scale:
- Does the combined-fault bounce count trend toward zero as tick count grows,
  or plateau at some nonzero rate? A nonzero plateau might be legitimate
  (team stuck oscillating while a single long-duration fault persists) or
  might indicate a fourth variant I didn't find.
- Any *other* evaluator/dispatch-action combination (not mobility/
  communication/equipment/LOCAL_RECOVERY/RETURN_TO_SAFE_ZONE) still producing
  tight bounces - `evaluate_progress` and `evaluate_stamina` weren't touched
  by fix 3 and could in principle have the same class of gap under fault
  conditions, though I didn't find evidence of it in the seeds I checked.

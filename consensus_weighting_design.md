# Two-axis cross-board consensus gate for fault verdicts

Implementation: [core/consensus_weighting.h](core/consensus_weighting.h),
[core/consensus_weighting.c](core/consensus_weighting.c).
Tests: [laptop/tools/consensus_weighting_selftest.c](laptop/tools/consensus_weighting_selftest.c)
(`make check-consensus-weighting`).

## 1. What this replaces, and why

**Flat vote-count cap (e.g. "N-3=6").** With `FAULT_BUFFER_TEAMS_PER_BOARD=5`
teams/board ([core/fault_buffer.h:89](core/fault_buffer.h)), a single board's
maximum possible vote count is 5 — a cap of 6 never binds; it's dead weight,
exactly as `fault_predictor_architecture_v4.md` Section 4's "Concern 1"
already flags. But the deeper problem isn't the specific number: any flat
scalar cap, however tuned, is still just counting *votes*, not counting
*independent failure domains*. It cannot distinguish "6 votes from 2
different boards" from "5 correlated votes from one board plus 1 more" —
exactly the shape a common-mode fault (shared power rail, shared sensor,
shared comms link) on one board would produce.

**Confidence-weighted scalar sum** (e.g. `sum(weight_i)` or
`mean(confidence_i)` across agreeing votes — the shape of
`fault_buffer.c`'s existing `g_vote_weight` accumulator). This conflates two
independent questions into one number: *how many independent domains
corroborate this?* and *how confident is each one?* A plain mean is also
fooled by disagreement hiding behind an equal average: two votes at 0.9 and
0.3 average to 0.6, indistinguishable from two votes both near 0.6 — even
though the latter pair is genuine corroboration and the former is two votes
that don't actually agree on how confident anyone is.

Both problems are solved by splitting fusion into two independent axes and
gating on both, rather than folding them into one number.

## 2. Definitions

**x — distinct boards.** For a given (target team, fault class) verdict this
round, `x` is the number of distinct physical boards with **at least one**
team voting that verdict. A board contributes **at most 1** regardless of how
many of its own teams agree. This is the
[failure-domain-aware distributed quorum](https://en.wikipedia.org/wiki/Quorum_(distributed_computing))
principle — per-domain sub-quorum requirements instead of one global count,
the same idea used by rack/availability-zone-aware quorum systems in
distributed storage, where no number of replicas *within* one rack/AZ counts
as corroboration *across* racks/AZs — applied here with a physical board as
the failure domain. It structurally prevents one board's entire team set
from ever counting as independent corroboration, no matter how many of its
teams agree.

**y — aggregate confidence via Brooks–Iyengar overlap.** Each team's vote is
treated as a confidence **interval** `[lo, hi]`, not a point value. `y` is the
midpoint of the **maximum-overlap region** across the agreeing votes'
intervals — the Brooks–Iyengar algorithm's core idea (Brooks & Iyengar,
*"Robust Distributed Computing and Sensing Algorithm,"* IEEE Computer 29(6),
1996; see also their *Multi-Sensor Fusion*, Prentice Hall, 1998): fuse from
the sub-region covered by the largest number of mutually overlapping
intervals, rather than an unconditioned mean. Two teams at 0.9 and 0.3 have
disjoint intervals (say `[0.85,0.95]` and `[0.25,0.35]`) — no overlap at all,
so they do not reinforce each other's `y` — while two teams both near 0.8
(say `[0.75,0.85]` and `[0.78,0.88]`) overlap tightly at `[0.78,0.85]`, giving
a `y` near 0.815 that a plain average of the same two midpoints (0.8, 0.83)
would arrive at too, but *without* ever checking whether the underlying
evidence actually agreed. The overlap check is what catches the first case
and rejects it as non-corroborating; a plain average can't, because it never
looks past the two numbers to ask whether they describe the same underlying
belief.

Implementation ([core/consensus_weighting.c](core/consensus_weighting.c)):
the maximum-overlap count is always achieved starting at one of the
intervals' own lower endpoints (coverage count is piecewise-constant between
endpoints, so the count *at* a `lo` is never less than the count anywhere
strictly between it and the next endpoint below it) — so it suffices to test
each vote's own `lo` as a candidate point, an O(n²) sweep over at most
`NUM_TEAMS-1` = 9 votes, with no sort required.

## 3. Acceptance-region derivation (N=9, 2 boards)

With `NUM_TEAMS=10` ([core/mission_state.h:168](core/mission_state.h)) and
`FAULT_BUFFER_TEAMS_PER_BOARD=5`, a target team's 9 possible voters split
across exactly 2 physical boards, so `x ∈ {0, 1, 2}`.

```
ACCEPT_ELEVATED  iff  (x >= 2)  OR  (x == 1  AND  y >= tau_single_board)
```

**`tau_single_board = +infinity`.** Single-board corroboration is **never**
sufficient on its own, at any confidence. Justification: a physical board is
one failure domain. Its teams share a power rail, a sensor bus, a comms
radio, and (per `fault_buffer.c`'s own liveness model) a single up/down
status. A common-mode fault on any of those can drive every team on that
board to agree with high confidence simultaneously, without the reported
fault being real, and without it being the fault the *other* board would
independently report. No amount of *intra*-domain agreement is evidence of
anything *outside* that domain — only a second, independent domain reporting
the same thing is. This mirrors a principle the codebase already enforces at
compile time for the old scalar-quorum design:

```c
// core/fault_buffer.h:98
_Static_assert(FAULT_BUFFER_TEAMS_PER_BOARD < FAULT_BUFFER_FULL_STRENGTH_QUORUM,
    "a single board must not be able to reach quorum alone for any class - ...");
```

The two-axis gate makes that same guarantee explicit and structural rather
than an emergent property of a quorum arithmetic: `x==1` cannot reach
`ACCEPT_ELEVATED` *by construction*, independent of what `y` or `NUM_TEAMS`
happen to be.

The `x==1` disjunct is kept **literal** in the implementation
(`consensus_gate_tier()`) rather than simplified to `x>=2` — `y >=
CONSENSUS_TAU_SINGLE_BOARD` is written out even though it's always false for
finite `y`, so the "never" decision stays visible and grep-able in the code,
not only in this document.

A **separate, finite** local-tier gate exists alongside this one:

```
ACCEPT_LOCAL  iff  x == 1  AND  y >= CONSENSUS_TAU_LOCAL   (CONSENSUS_TAU_LOCAL = 0.5)
```

This is not a relaxed version of the elevated question — it answers a
different one ("is this one board's own vote confident enough to justify a
`LOCAL_RECOVERY`-level action at all?"), and no value of `y` here can ever
promote a verdict past `CONSENSUS_TIER_LOCAL`. Only `x>=2` does that.

## 4. Mapping onto the tiered-trust rule (Section 3)

`fault_predictor_architecture_v4.md` Section 3 / `ARCHITECTURE.md`'s
`evaluate_fault_predictor()` (`core/recovery_manager.c:1103-1187`) already
states the rule in principle:

```
CONTINUE_MONITORING / LOCAL_RECOVERY   — AI alone
RETURN_TO_SAFE_ZONE and above          — needs watchdog corroboration
                                          OR sustained AI persistence
```

The two-axis gate gives that rule concrete numbers:

| `(x, y)`                         | `ConsensusTier_t`         | Tier ceiling                                            |
|-----------------------------------|---------------------------|----------------------------------------------------------|
| `x >= 2`                          | `CONSENSUS_TIER_ELEVATED` | Satisfies "watchdog corroboration" → `RETURN_TO_SAFE_ZONE`-and-above eligible |
| `x == 1`, `y >= CONSENSUS_TAU_LOCAL` | `CONSENSUS_TIER_LOCAL`    | Capped at `CONTINUE_MONITORING`/`LOCAL_RECOVERY`, same ceiling as "AI alone" |
| otherwise                          | `CONSENSUS_TIER_NONE`     | No usable corroboration this round                        |

`x==1` never outranks `CONSENSUS_TIER_LOCAL`, matching the requirement that a
single board (however many of its teams agree, however confidently) is
treated exactly like "AI alone" for tiering purposes — because structurally,
it *is* just one entity's opinion, the same as the AI's.

## 5. Worked example

5 teams on board 0 vote FAULT at max confidence (`[0.85, 0.95]` each). 1 team
on board 1 votes FAULT at only moderate confidence (`[0.55, 0.65]`).

**Board 0 alone:**
- `x = 1` (one board, 5 of its own teams agreeing doesn't change that).
- `y` = midpoint of the 5 tightly-overlapping intervals ≈ 0.9 (verified in
  `test_worked_example_correlated_board_does_not_outvote_cross_board`:
  `y = 0.900`, `overlap_count = 5`).
- `consensus_gate_tier(1, 0.9)` → **`CONSENSUS_TIER_LOCAL`** — capped, despite
  near-maximum confidence and 5 agreeing teams.

**Board 0 + board 1 combined:**
- `x = 2` (board 1 now has ≥1 team on this verdict too).
- `consensus_gate_tier(2, y)` → **`CONSENSUS_TIER_ELEVATED`**, unconditionally
  on `y`'s exact value (the elevated branch doesn't gate on `y` at all once
  `x>=2` — see §3's derivation).

Five correlated same-board votes at near-maximum confidence do **not**
out-vote one genuinely cross-board vote at only moderate confidence. This is
the intended outcome: `x` (structural diversity of evidence source) is
weighted above `y` (confidence magnitude) for the elevated tier, precisely
because within one failure domain, confidence itself can be manufactured by
the same common-mode cause that's manufacturing the (possibly false) fault
signal — five teams agreeing with each other proves the fault propagated
across a shared board, not that it's real.

## 6. f < N/3 tolerance in the uncorrelated case

If 4 of board 0's 5 teams vote FAULT and 1 disagrees (votes a different class
or doesn't vote this one at all), board 0's contribution to `x` for FAULT is
**still 1** — `x`'s definition only requires "≥1 team on the board voting
this verdict," so one dissenter cannot erase the other four's agreement.
Verified in
`test_uncorrelated_disagreement_does_not_zero_out_board`: `x` stays 1 with
4/5 agreeing, then reaches 2 once board 1's team also agrees, exactly as it
would with a full 5/5 board — the disagreement changes nothing about board
0's participation in `x`. This is consistent with (and doesn't reopen) the
existing Byzantine `f < N/3` bound `fault_buffer_fuse()` already enforces on
raw vote counts ([core/fault_buffer.c:391-394](core/fault_buffer.c)): that
quorum arithmetic and this board-diversity axis are answering different
questions (how many *votes* clear Byzantine tolerance vs. how many
*independent boards* corroborate), and this gate doesn't relax the former —
it adds the latter as an orthogonal requirement, only for the elevated tier.

## 7. Dark/unreachable boards

This module has **no liveness concept of its own**, deliberately: liveness vs.
disagreement is a hard separation (see `consensus_weighting.h`'s "Liveness vs.
disagreement" section for the full rule). Liveness is a graded (0.0–1.0)
confidence signal from `fault_buffer_board_liveness_confidence()` in
[core/fault_buffer.c](core/fault_buffer.c) — a deadline-based simplification
of the phi-accrual failure-detector pattern (Hayashibara et al., SRDS 2004),
appropriate for this fixed-tick RTOS. `consensus_gate_submit_vote_if_live()`
is the real dark-board mechanism: it checks that confidence against
`CONSENSUS_LIVENESS_CONFIDENCE_THRESHOLD` and only forwards to
`consensus_gate_submit_vote()` if the board clears it; a dark board is
excluded from the round **by never having `consensus_gate_submit_vote()`
called on its behalf**, never by this module counting it as absent or as "no
fault" and subtracting anything.

**FLP caveat, stated explicitly:** perfectly distinguishing "this board
crashed" from "this board is merely slow" is provably impossible in an
asynchronous system (Fischer, Lynch & Paterson, 1985). The liveness ramp is a
bounded-risk approximation, not a certainty — see `fault_buffer.c`'s own doc
on `fault_buffer_board_liveness_confidence()` for the exact risk this accepts.

This is structural, not just a calling convention: `board_mask` inside
`ConsensusGateState_t` is a monotonic bitwise-OR — bits are only ever set (in
`consensus_gate_submit_vote()`), never cleared, except by
`consensus_gate_reset()` at the next round boundary. There is no code path in
`consensus_weighting.c` that can turn a set bit back off mid-round, so a
liveness signal that goes dark *after* a board has already voted cannot
retroactively decrement `x` or perturb `y` for votes already recorded.
Verified in `test_dark_board_mid_vote_does_not_decrement_x`: board 1 votes,
contributes to `x` (2), then genuinely goes dark (real liveness ramp, driven
by advancing tick time past its window with no further proof-of-life heard —
not a stub), and a subsequent recomputation still reports `x=2` and
`CONSENSUS_TIER_ELEVATED` — completely unchanged.

## 8. State-machine shape, not a running sum

`ConsensusGateState_t` is a small, fixed-size, per-(target team, fault class),
per-round struct — a `board_mask` bitmask plus each vote's own `(board_id,
interval)` pair (up to `NUM_TEAMS-1` = 9 entries) — recomputed via
`consensus_gate_compute()` on demand, not an accumulating scalar like
`fault_buffer.c`'s `g_vote_weight[team][class] += weight`. Keeping each vote's
own interval (rather than folding it into a running sum immediately) is what
makes the Brooks–Iyengar overlap computation possible at all: an overlap
region is a property of the *set* of intervals, and cannot be recovered from
any single running scalar once individual votes have been summed away.

**Forward-looking note, not applied today:** a quantization boundary sweep
(2M+ samples, `round(y*255)/255`-style uint8 round-trip) found zero
`CONSENSUS_TIER_LOCAL`/`NONE` flips at the current `CONSENSUS_TAU_LOCAL=0.5f`,
but only because 0.5 happens to land exactly on that quantizer's own
round-half-up tie point — nearby, non-grid-aligned thresholds (e.g. 0.501,
0.503) flipped on ~3% of samples in the same sweep. **No evidence-quantization
path exists in this codebase today** (evidence is still transmitted as full
float — see `core/fault_comms.c`), so there is nothing to patch right now.
If/when evidence quantization to the wire format is added, apply a hysteresis
margin of at least one full quantization step around whichever threshold is
in use at that time (e.g., for an 8-bit/255-level scheme: accept at
`y >= tau + 1/255`, reject below `y < tau - 1/255`, with the narrow dead-band
in between resolved toward whichever side is safer to default to) rather than
relying on the threshold happening to land on a quantization tie point again.

## 9. Files

- [core/consensus_weighting.h](core/consensus_weighting.h) — types
  (`ConsensusInterval_t`, `ConsensusGateState_t`, `ConsensusTier_t`),
  constants (`CONSENSUS_TAU_SINGLE_BOARD`, `CONSENSUS_TAU_LOCAL`), full API
  doc.
- [core/consensus_weighting.c](core/consensus_weighting.c) — `board_mask`
  bitmask tracking, Brooks–Iyengar max-overlap sweep, tier decision.
- [laptop/tools/consensus_weighting_selftest.c](laptop/tools/consensus_weighting_selftest.c)
  — the three required tests (worked example, uncorrelated disagreement,
  dark-board-mid-vote). Run via `make check-consensus-weighting`.

#ifndef CONSENSUS_WEIGHTING_H
#define CONSENSUS_WEIGHTING_H

#include <stdint.h>

#include "fault_buffer.h"   /* FAULT_BUFFER_NUM_BOARDS (board_mask sizing reference) */
#include "mission_state.h"  /* NUM_TEAMS */

/*
 * ---------------------------------------------------------------
 * Two-axis cross-board consensus gate
 * ---------------------------------------------------------------
 * Replaces two things that were considered and rejected for arbitrating
 * a fault verdict, per fault_predictor_architecture_v4.md Section 4:
 *
 *   1. A flat vote-count cap (e.g. "N-3=6"). With FAULT_BUFFER_TEAMS_
 *      PER_BOARD=5 teams/board, a single board's maximum possible vote
 *      count is 5 - a cap of 6 never binds, it's dead weight. Worse,
 *      even a cap that DID bind would still be a single scalar count:
 *      it cannot distinguish "6 votes from 2 independent boards" from
 *      "6 votes that happen to include 5 correlated votes from the
 *      same board plus 1 from another" - exactly the failure mode a
 *      single physical board's common-mode faults could produce.
 *
 *   2. A single confidence-weighted scalar sum (e.g. sum(weight_i) or
 *      mean(confidence_i) across agreeing votes). This conflates two
 *      independent questions - "how many independent failure domains
 *      corroborate this?" and "how confident is each one?" - into one
 *      number, and a plain mean is fooled by two votes with equal mean
 *      but wildly different agreement quality (0.9 and 0.3 average to
 *      0.6, same as two votes both near 0.6 - even though the latter
 *      pair is genuinely corroborating and the former pair is not).
 *
 * This module instead tracks two independent axes per (target team,
 * fault class) per consensus round:
 *
 *   x = number of DISTINCT PHYSICAL BOARDS with >=1 team voting the
 *       same verdict this round. A board contributes at most 1
 *       regardless of how many of its own teams agree - this is the
 *       failure-domain-quorum principle (per-domain sub-quorum
 *       requirements, not one global count, as used in failure-domain-
 *       aware distributed quorum systems, e.g. rack/AZ-aware quorum in
 *       distributed storage) applied to this project's two-board
 *       topology: a board is one failure domain, and no amount of
 *       agreement *within* one domain counts as evidence *across*
 *       domains.
 *
 *   y = aggregate confidence across the agreeing votes, computed as
 *       the midpoint of their Brooks-Iyengar maximum-overlap region
 *       (Brooks & Iyengar, "Robust Distributed Computing and Sensing
 *       Algorithm", IEEE Computer 29(6), 1996) rather than a mean or
 *       min: each vote is treated as a confidence INTERVAL, and votes
 *       only get to reinforce each other's y if their intervals
 *       actually overlap. See consensus_weighting.c's consensus_gate_
 *       compute() for the sweep and the worked example in this
 *       project's design doc for why this catches what a mean can't.
 *
 * Acceptance region (derived for this project's N=9 voters / 2 boards,
 * so x in {0,1,2}):
 *
 *     ACCEPT_ELEVATED iff (x >= 2) OR (x == 1 AND y >= CONSENSUS_TAU_SINGLE_BOARD)
 *
 * with CONSENSUS_TAU_SINGLE_BOARD = +infinity: single-board corroboration
 * is NEVER sufficient on its own, at any confidence, because a single
 * physical board is one failure domain and its teams' votes are not
 * independent evidence of anything outside that domain (correlated/
 * common-mode faults, shared power/comms/sensor rail, etc. can drive
 * every team on one board to agree with high confidence simultaneously
 * without the underlying fault being real, or without it being the
 * fault the OTHER board would independently report). The x==1 branch is
 * therefore kept in the code literally (not simplified away) so this
 * "never" decision stays visible and grep-able, matching this project's
 * existing convention of writing out invariants explicitly rather than
 * relying on them silently (see fault_buffer.h's _Static_assert on the
 * same principle for the old scalar-quorum design).
 *
 * Section 3's tiered-trust rule maps onto this directly:
 *   x >= 2                       -> CONSENSUS_TIER_ELEVATED: satisfies
 *                                    RETURN_TO_SAFE_ZONE-and-above's
 *                                    "watchdog corroboration" requirement.
 *   x == 1, y >= CONSENSUS_TAU_LOCAL -> CONSENSUS_TIER_LOCAL: same
 *                                    ceiling as "AI alone" - capped at
 *                                    CONTINUE_MONITORING/LOCAL_RECOVERY,
 *                                    regardless of how high y climbs.
 *   otherwise                    -> CONSENSUS_TIER_NONE.
 *
 * ---------------------------------------------------------------
 * Liveness vs. disagreement - the exact rule, and who owns what
 * ---------------------------------------------------------------
 * Two questions that must never share or blur state:
 *
 *   LIVENESS ("is this board's link reachable at all, right now?") is a
 *   per-BOARD, per-tick physical-layer fact, wholly independent of what
 *   any vote says. Data source: core/fault_buffer.c's last-heard-tick
 *   tracking, exposed as a GRADED (not boolean) confidence in [0.0,1.0]
 *   via fault_buffer_board_liveness_confidence() - a deadline-based
 *   simplification of the general phi-accrual failure-detector pattern
 *   (Hayashibara et al., SRDS 2004), appropriate here because this is a
 *   fixed-tick RTOS, not a general asynchronous network with a real
 *   inter-arrival distribution to model. Reporting a low-liveness board
 *   as a fault (RECOVERY_REASON_COMMUNICATION_FAILURE) is recovery_
 *   manager.c's evaluate_communication()'s job, not this module's - this
 *   module never explains WHY a board is excluded, it only consumes the
 *   confidence number to decide whether a vote is even eligible.
 *
 *   DISAGREEMENT ("given only the boards trusted enough to count this
 *   round, do their votes actually agree?") is this module's (x,y)
 *   computation, and ONLY this module's. It has no notion of liveness at
 *   all - by the time a vote reaches consensus_gate_submit_vote(), the
 *   liveness question has already been answered by the caller.
 *
 * FLP CAVEAT (state this, don't just imply it): perfectly distinguishing
 * "this board crashed" from "this board is merely slow" is PROVABLY
 * IMPOSSIBLE in an asynchronous system (Fischer, Lynch & Paterson, 1985).
 * fault_buffer_board_liveness_confidence()'s deadline ramp is a
 * BOUNDED-RISK APPROXIMATION, not a certainty - a board silent a little
 * longer than its window is treated as unreachable even though it could
 * still be alive, and one that just barely stayed inside its window is
 * trusted even though it could go dark immediately after. The window and
 * grace-period widths (see fault_buffer.c) are how much of that
 * unavoidable risk this project accepts, not a way to eliminate it.
 *
 * THE EXACT RULE for a (target team, fault class) round, per board B
 * hosting team T:
 *
 *   (a) B's liveness confidence < CONSENSUS_LIVENESS_CONFIDENCE_THRESHOLD
 *       ("B is dark"): every team on B is excluded from this round's x
 *       AND y entirely - not counted as "no fault", not submitted as an
 *       absent/zero vote, simply never reaches consensus_gate_submit_
 *       vote() at all. Attribution of WHY (comms failure) belongs to
 *       evaluate_communication(), never to this module. Use consensus_
 *       gate_submit_vote_if_live() below, which enforces this
 *       mechanically rather than leaving it to caller discipline alone.
 *
 *   (b) B's liveness confidence >= threshold ("B is live") but T's own
 *       vote for this specific (target,class) hasn't arrived yet this
 *       round: this is NOT a liveness failure and must not be treated as
 *       one - B may be live via its OTHER teams' traffic in this same
 *       window even though T specifically hasn't voted yet. This is an
 *       ordinary within-round timing gap: the round-orchestration layer
 *       (whichever caller collects individual team votes as they arrive)
 *       waits up to the round's own deadline for T's vote; if it never
 *       arrives by round close, T's vote is simply absent from this
 *       round's tally - contributing nothing to x or y - exactly as if
 *       T had nothing to report, and it does NOT lower B's liveness
 *       confidence (a silent team is not evidence its board is
 *       unreachable). Case (a) and case (b) must never be conflated:
 *       (a) is a per-board physical fact that blocks ALL of B's teams
 *       and is reported as a communication fault; (b) is a per-team
 *       content fact that affects only T and is reported as nothing at
 *       all (or, if desired, an ordinary "no vote received" - never
 *       "dark board").
 *
 * WORKED FAILURE CASE - whole board dark mid-vote (walked through
 * explicitly, matching laptop/tools/consensus_weighting_selftest.c's
 * test_dark_board_mid_vote_does_not_decrement_x()):
 *
 *   t0: board 0's team votes.                        x=1 (board 0 only)
 *   t1: board 1's team votes while still live.        x=2 (both boards)
 *   t2: board 1 goes dark (no heartbeat/evidence heard for longer than
 *       its liveness window) - fault_buffer_board_liveness_confidence()
 *       for board 1 has now decayed below CONSENSUS_LIVENESS_CONFIDENCE_
 *       THRESHOLD.
 *   t3: another team on board 1 (or the same one, retrying) attempts to
 *       vote again this round. consensus_gate_submit_vote_if_live()
 *       checks board 1's confidence, finds it below threshold, and does
 *       NOT call consensus_gate_submit_vote() - board 1 contributes no
 *       new information.
 *
 *   Result: x is STILL 2 at t3, not silently dropped to 1. This matters
 *   because board_mask is a monotonic bitwise-OR (see consensus_gate_
 *   submit_vote()'s own doc) - there is structurally no operation in
 *   this module that could move x backward once board 1's bit is set,
 *   so a later dark declaration can never retroactively make the round
 *   look like weaker cross-board corroboration than what was genuinely
 *   observed before board 1 went dark. Equally, x does NOT climb further
 *   from board 1 after t2, because the liveness gate blocks board 1's
 *   bit from being (redundantly) set again by any NEW vote attempt -
 *   gating happens at submission time, not at compute time, so a dark
 *   board can neither erase its own earlier legitimate contribution nor
 *   inject fresh, untrustworthy information into the round.
 */

typedef struct
{
    float lo;
    float hi;
} ConsensusInterval_t;

typedef enum
{
    CONSENSUS_TIER_NONE = 0,   /* neither branch below clears - no usable corroboration this round */
    CONSENSUS_TIER_LOCAL,      /* x==1 AND y>=CONSENSUS_TAU_LOCAL - same ceiling as "AI alone" (Section 3) */
    CONSENSUS_TIER_ELEVATED    /* x>=2 - watchdog-corroboration requirement satisfied */
} ConsensusTier_t;

/* Upper bound on votes trackable in one round for one (target team,
   fault class) pair: every other team can vote about a given target
   team at most once per round (mirrors fault_buffer.h's own
   FAULT_BUFFER_FULL_STRENGTH_VOTERS reasoning). */
#define CONSENSUS_WEIGHTING_MAX_VOTES_PER_ROUND (NUM_TEAMS - 1u)

/* board_mask is a plain bitmask, one bit per board_id - 16 bits covers
   any board count this project could plausibly reach, far above
   FAULT_BUFFER_NUM_BOARDS (2 today). board_id values >= 16 are
   rejected defensively by consensus_gate_submit_vote() rather than
   silently corrupting the mask. */
_Static_assert(FAULT_BUFFER_NUM_BOARDS <= 16u,
    "consensus_weighting's board_mask is 16 bits wide - widen it (and this assert) if the board count ever exceeds that");

/*
 * Per (target team, fault class) round state. Deliberately a small
 * fixed-size struct holding each vote's own board id and interval, NOT
 * a running scalar sum/count - see fault_predictor_architecture_v4.md's
 * critique of the old g_vote_weight-style accumulator. board_mask is a
 * monotonic bitwise-OR: bits are only ever set, never cleared, except
 * by consensus_gate_reset() at the next round boundary. That monotonicity
 * is what makes "a dark board must never decrement x" a property of the
 * data structure itself, not just a rule callers have to remember.
 */
typedef struct
{
    uint16_t             board_mask;
    uint8_t              vote_count;
    uint8_t              board_of_vote[CONSENSUS_WEIGHTING_MAX_VOTES_PER_ROUND]; /* diagnostic only */
    ConsensusInterval_t  intervals[CONSENSUS_WEIGHTING_MAX_VOTES_PER_ROUND];
} ConsensusGateState_t;

/* tau_single_board = +infinity - see this header's own top-of-file doc
   for the justification. A named, external constant (not INFINITY
   inlined at each call site) so the "never" decision is one grep away
   wherever it's used. */
extern const float CONSENSUS_TAU_SINGLE_BOARD;

/* UNTUNED PLACEHOLDER - not a derived/validated value. Set to 0.5f as a
   round-number stand-in only; there is no numeric derivation for it
   anywhere in this project (fault_predictor_architecture_v4.md Section 3
   explicitly states the tiered-trust confidence thresholds' "principle
   locked, numbers not set"). Do not read this value as validated just
   because it's committed - treat retuning it as an open TODO, and note
   that if/when it IS retuned, consensus_weighting_design.md's
   quantization hysteresis-margin note applies to whatever new value is
   chosen (see that doc for why 0.5f specifically has been safe so far).
   Finite local-tier confidence floor - same role as recovery_manager.c's
   existing "AI alone" confidence floor (ARCHITECTURE.md Section 3/8:
   "Exact tiered-trust confidence thresholds... numbers not set"; kept
   as a separate, independently-tunable constant here rather than
   reusing recovery_manager.c's, since that one is still an untuned
   placeholder per that same doc). x==1 votes must clear this to reach
   even CONSENSUS_TIER_LOCAL; no value of y can promote an x==1 verdict
   past CONSENSUS_TIER_LOCAL - only x>=2 does that (see consensus_gate_
   tier()). */
#define CONSENSUS_TAU_LOCAL 0.5f

/* UNTUNED PLACEHOLDER - not a derived/validated value, same status as
   CONSENSUS_TAU_LOCAL above, but for a DIFFERENT reason - this one
   is NOT blocked on a trained model's confidence distribution
   (CONSENSUS_TAU_LOCAL's real blocker), and picking a real number
   for it is genuinely pure timing math with zero model dependency.
   It stays untuned because of a real, DIFFERENT prerequisite that
   hasn't happened yet - stated explicitly here so it isn't confused
   for a "just needs a real model" placeholder:

   fault_buffer_board_liveness_confidence()'s whole mechanism is
   TICK-DENOMINATED, end to end - board_mark_heard() stores tick
   values (fed from g_current_tick at every real call site in core/
   fault_comms.c), age is a tick difference, and FAULT_BUFFER_
   LIVENESS_WINDOW_TICKS/GRACE_TICKS (fault_buffer.c) are tick counts.
   Ticks have NO fixed real-time rate on real hardware
   (BOARD_TICK_DELAY_MS=0 - see fault_buffer.c's own doc on this),
   so there is no valid conversion from "real broadcast cadence" into
   "N ticks" to derive this threshold or the window/grace constants
   against - that conversion simply does not exist while the
   mechanism stays tick-denominated. This is the exact same class of
   error fault_predictor_architecture_v4.md's Section 4 already
   flagged and fixed for FAULT_COMMS_BROADCAST_INTERVAL_MS (that fix
   is now applied, see core/fault_comms.c) - deriving a REAL value
   here responsibly requires the same fix applied to THIS mechanism
   first: converting board_mark_heard()/fault_buffer_board_liveness_
   confidence() from tick-units to cbba_now_ms() real milliseconds
   (changing what's stored, every fault_comms.c call site, the window/
   grace constants' units, AND laptop/tools/consensus_weighting_
   selftest.c's existing tick-based test cases, which explicitly pass
   FAULT_BUFFER_LIVENESS_WINDOW_TICKS as a raw tick argument today).
   That is a real, scoped, multi-file follow-up (tick->ms conversion,
   touching existing test coverage) - not a "pick a better number"
   fix, and deliberately not attempted here.
   A board's vote is eligible for this round only while fault_buffer_
   board_liveness_confidence() reports at or above this threshold - see
   this header's "Liveness vs. disagreement" section above for the full
   rule. Below it, the board is "dark": excluded from x/y entirely via
   consensus_gate_submit_vote_if_live(), never counted as an absent or
   "no fault" vote. */
#define CONSENSUS_LIVENESS_CONFIDENCE_THRESHOLD 0.5f

/* Zero the round state. Call at each new consensus round boundary, one
   state instance per (target team, fault class) pair in play. */
void consensus_gate_reset(ConsensusGateState_t *state);

/*
 * Records one team's vote for one (target team, fault class) pair this
 * round, as a confidence INTERVAL (not a point value). Caller must only
 * call this for teams whose board is currently live per Prompt 3's
 * liveness signal (or, pending that, fault_buffer.c's board_is_live()/
 * FaultBufferFusionResult_t.n_live) - this function performs no
 * liveness check of its own and there is no "absent vote" placeholder;
 * a board that never gets this function called on its behalf simply
 * never contributes to board_mask, which is exactly the correct
 * behavior for a dark board (excluded from the vote entirely, not
 * counted as absent or as "no fault" - see this file's top-of-file doc).
 *
 * No-op (silently drops the vote) if state is NULL, the round's vote
 * capacity is already full, or board_id is out of range - see this
 * header's board_mask _Static_assert for the range this covers.
 */
void consensus_gate_submit_vote(
    ConsensusGateState_t *state,
    uint8_t board_id,
    ConsensusInterval_t confidence);

/*
 * Liveness-gated wrapper around consensus_gate_submit_vote() - the real
 * dark-board mechanism (see this header's "Liveness vs. disagreement"
 * section for the full rule this implements, and its "WORKED FAILURE
 * CASE" for the whole-board-goes-dark-mid-vote walkthrough).
 *
 * Looks up board_id's current graded liveness confidence via fault_
 * buffer_board_liveness_confidence(board_id, now) and only forwards to
 * consensus_gate_submit_vote() if that confidence is at or above
 * CONSENSUS_LIVENESS_CONFIDENCE_THRESHOLD; below it, the vote is
 * dropped exactly as if this function had never been called at all - no
 * "no fault"/absent-vote placeholder is ever submitted for a dark
 * board's excluded teams.
 *
 * Returns 1u if the vote was submitted, 0u if it was dropped as
 * liveness-gated (or for any reason consensus_gate_submit_vote() itself
 * would have dropped it - state==NULL, round at capacity, bad board_id).
 * The return value is purely diagnostic; callers should not need to
 * branch on it to behave correctly, since a dropped vote is always the
 * correct behavior for a dark board.
 */
uint8_t consensus_gate_submit_vote_if_live(
    ConsensusGateState_t *state,
    uint8_t board_id,
    ConsensusInterval_t confidence,
    uint32_t now);

/*
 * Computes this round's (x, y) for one (target team, fault class) pair:
 *   out_x             = number of distinct boards that have voted (bits
 *                        set in board_mask) - independent of y, since x
 *                        is defined purely by verdict-label agreement
 *                        (see this file's top-of-file doc), not by
 *                        whether the confidence intervals overlap.
 *   out_y             = midpoint of the Brooks-Iyengar maximum-overlap
 *                        region across all submitted intervals; 0.0f if
 *                        no votes have been submitted.
 *   out_overlap_count = how many of the submitted intervals actually
 *                        participate in that maximum-overlap region
 *                        (diagnostic - not itself gated on, but useful
 *                        for logging/debugging a round where x looks
 *                        healthy but the votes' confidence disagrees
 *                        wildly).
 * Any of the three output pointers may be NULL if that value isn't
 * needed by the caller.
 */
void consensus_gate_compute(
    const ConsensusGateState_t *state,
    uint8_t *out_x,
    float *out_y,
    uint8_t *out_overlap_count);

/*
 * Acceptance-region decision, given this round's (x, y):
 *
 *     ACCEPT_ELEVATED iff (x >= 2) OR (x == 1 AND y >= CONSENSUS_TAU_SINGLE_BOARD)
 *
 * written exactly as derived (the x==1 disjunct is structurally dead
 * since CONSENSUS_TAU_SINGLE_BOARD is +infinity, but is kept literal
 * rather than simplified away - see this header's top-of-file doc).
 * Falls back to the separate, finite CONSENSUS_TIER_LOCAL check (same
 * ceiling as "AI alone") only if the elevated branch does not clear.
 */
ConsensusTier_t consensus_gate_tier(uint8_t x, float y);

#endif

/*
 * Standalone self-test for core/consensus_weighting.c (no hardware
 * required - desktop gcc only, same pattern as every other selftest in
 * this project's laptop/tools/ directory).
 *
 * Covers the three scenarios the two-axis (x,y) consensus gate design
 * doc calls out explicitly:
 *
 *   1. test_worked_example_correlated_board_does_not_outvote_cross_board():
 *      5 teams on one board voting FAULT at max confidence (x=1) vs. 1
 *      team on the other board voting FAULT at moderate confidence
 *      (x=2 once combined) - proves the 5 correlated same-board votes
 *      stay capped at CONSENSUS_TIER_LOCAL while the single genuinely
 *      cross-board vote clears CONSENSUS_TIER_ELEVATED.
 *
 *   2. test_uncorrelated_disagreement_does_not_zero_out_board():
 *      one team on a board disagrees (or is silent) while the rest of
 *      that same board still agrees - proves x's "at least one team on
 *      a board" definition means the board still contributes to x, and
 *      the f<N/3 tolerance isn't violated by treating one dissenter as
 *      erasing the whole board's corroboration.
 *
 *   3. test_dark_board_mid_vote_does_not_decrement_x():
 *      board 1 votes first (contributing to x), then genuinely goes dark
 *      mid-round - via core/fault_buffer.c's REAL liveness signal
 *      (fault_buffer_board_liveness_confidence(), driven by advancing
 *      g_current_tick past board 1's actual liveness window with no
 *      further fault_buffer_record_*() call heard from it - not a
 *      hand-written stub) - proves a later dark declaration never
 *      retroactively decrements x/y for votes already recorded, and
 *      that consensus_gate_submit_vote_if_live()'s liveness gate is what
 *      keeps a NEWLY-dark board from contributing further, without this
 *      module ever needing an "absent" or "no fault" placeholder for it.
 *
 * Build:
 *   gcc -std=gnu11 -Wall -Wextra -Wpedantic -Icore \
 *       -o consensus_weighting_selftest \
 *       laptop/tools/consensus_weighting_selftest.c \
 *       core/consensus_weighting.c core/fault_buffer.c \
 *       core/fault_config.c core/mission_state.c -lm
 * Run:
 *   ./consensus_weighting_selftest
 *
 * (consensus_weighting.c only depends on fault_buffer.h/.c and
 * mission_state.h/.c for shared constants - NUM_TEAMS,
 * FAULT_BUFFER_NUM_BOARDS - and the _Static_assert that checks
 * board_mask's width against it; no wider slice of the simulator is
 * needed to exercise this module in isolation.)
 */

#include "consensus_weighting.h"
#include "fault_buffer.h"
#include "mission_state.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            g_failures++;                                                \
        }                                                                \
    } while (0)

#define BOARD_0 0u
#define BOARD_1 1u

/* ---------------------------------------------------------------
 * 1. Worked example: 5 correlated same-board votes must NOT outweigh
 *    one genuine cross-board vote, even at much lower confidence.
 * ---------------------------------------------------------------
 */
static void test_worked_example_correlated_board_does_not_outvote_cross_board(void)
{
    ConsensusGateState_t state;
    uint8_t x;
    float y;
    uint8_t overlap_count;

    consensus_gate_reset(&state);

    /* 5 teams on board 0, all voting FAULT at max confidence
       ([0.85, 0.95] each) - a single physical board's entire team
       complement agreeing with itself. */
    for (int i = 0; i < 5; i++)
    {
        ConsensusInterval_t max_confidence = {0.85f, 0.95f};
        consensus_gate_submit_vote(&state, BOARD_0, max_confidence);
    }

    consensus_gate_compute(&state, &x, &y, &overlap_count);
    CHECK(x == 1u); /* one board, however many of its own teams agree */
    CHECK(y > 0.8f); /* the 5 identical intervals overlap tightly near 0.9 */
    CHECK(overlap_count == 5u);

    ConsensusTier_t tier_board0_only = consensus_gate_tier(x, y);
    CHECK(tier_board0_only == CONSENSUS_TIER_LOCAL); /* NOT elevated, no matter how high y is */

    /* Now 1 team on board 1 votes FAULT too, at only moderate confidence
       ([0.55, 0.65]) - genuinely from the other physical board. */
    ConsensusInterval_t moderate_confidence = {0.55f, 0.65f};
    consensus_gate_submit_vote(&state, BOARD_1, moderate_confidence);

    consensus_gate_compute(&state, &x, &y, &overlap_count);
    CHECK(x == 2u); /* now both boards have >=1 team agreeing */

    ConsensusTier_t tier_combined = consensus_gate_tier(x, y);
    CHECK(tier_combined == CONSENSUS_TIER_ELEVATED); /* clears, unconditionally on y, once x>=2 */

    printf("  worked example: board0-only x=%u tier=LOCAL (correctly capped); "
           "combined x=%u y=%.3f tier=ELEVATED (cross-board corroboration wins)\n",
           1u, x, y);
}

/* ---------------------------------------------------------------
 * 2. Uncorrelated disagreement within one board: one dissenting team
 *    (or a team that simply doesn't vote this verdict) must not erase
 *    that board's contribution to x, as long as >=1 other team on it
 *    still agrees - the f<N/3 tolerance depends on this.
 * ---------------------------------------------------------------
 */
static void test_uncorrelated_disagreement_does_not_zero_out_board(void)
{
    ConsensusGateState_t state;
    uint8_t x;
    float y;

    consensus_gate_reset(&state);

    /* Board 0 has 5 teams total. Only 4 of them vote FAULT (the 5th
       either votes a different class entirely, or is simply not
       included in this verdict's vote set) - this state only ever
       receives the 4 agreeing votes, exactly as fault_buffer.c's own
       per-class vote accumulation already works (a team's vote for
       class A doesn't touch class B's tally at all). */
    for (int i = 0; i < 4; i++)
    {
        ConsensusInterval_t confidence = {0.7f, 0.8f};
        consensus_gate_submit_vote(&state, BOARD_0, confidence);
    }

    consensus_gate_compute(&state, &x, &y, NULL);
    CHECK(x == 1u); /* board 0 still contributes - 4 of 5 teams is still ">=1" */
    CHECK(y > 0.6f);

    /* Board 1's own single team agrees too - x should now reach 2,
       proving the earlier disagreement on board 0 didn't somehow
       suppress board 0's own contribution nor poison board 1's. */
    ConsensusInterval_t confidence = {0.72f, 0.82f};
    consensus_gate_submit_vote(&state, BOARD_1, confidence);

    consensus_gate_compute(&state, &x, &y, NULL);
    CHECK(x == 2u);
    CHECK(consensus_gate_tier(x, y) == CONSENSUS_TIER_ELEVATED);
}

/* ---------------------------------------------------------------
 * 3. Dark board mid-vote: board 1 goes dark AFTER already contributing
 *    a vote this round. A later liveness signal marking it dark must
 *    never retroactively decrement x/y for the vote it already cast,
 *    and this module itself has no "board went dark, subtract it"
 *    operation at all - the caller (stubbing Prompt 3's liveness
 *    signal here) is what stops FURTHER votes from that board from
 *    ever reaching consensus_gate_submit_vote(), not this module
 *    reacting after the fact.
 * ---------------------------------------------------------------
 */
/* Some team id, distinct from BOARD_0/BOARD_1's own team ids, used purely
   as the "target team" argument to fault_buffer_record_local_vote() calls
   below whose only real purpose is marking a board heard (board-liveness
   proof-of-life) - the vote content itself (class_mask=0, weight=0.0f) is
   irrelevant to this test and deliberately inert. */
#define LIVENESS_PING_TARGET_TEAM 0u

/* Mark board_id heard (proof-of-life) at tick `tick`, via the same public
   ingestion path core/fault_comms.c would use on real hardware - not a
   test-local stub. Advances g_current_tick to `tick` first, since fault_
   buffer_record_local_vote()'s board_mark_heard() call reads the global
   tick, not a parameter. */
static void mark_board_heard_at_tick(uint8_t board_id, uint32_t tick)
{
    g_current_tick = tick;
    fault_buffer_record_local_vote(/*voter_team_id=*/0u, board_id,
        LIVENESS_PING_TARGET_TEAM, /*class_mask=*/0u, /*weight=*/0.0f);
}

static void test_dark_board_mid_vote_does_not_decrement_x(void)
{
    ConsensusGateState_t state;
    uint8_t x;
    float y;
    float mid_ramp_confidence;

    fault_buffer_init(FAULT_BUFFER_NUM_BOARDS, FAULT_BUFFER_TEAMS_PER_BOARD);
    consensus_gate_reset(&state);

    /* t0: both boards heard from (proof-of-life) - both fully live. */
    mark_board_heard_at_tick(BOARD_0, 0u);
    mark_board_heard_at_tick(BOARD_1, 0u);
    CHECK(fault_buffer_board_liveness_confidence(BOARD_0, 0u) == 1.0f);
    CHECK(fault_buffer_board_liveness_confidence(BOARD_1, 0u) == 1.0f);

    /* Board 0's team votes first, via the REAL liveness-gated entry
       point - not the raw consensus_gate_submit_vote(). */
    {
        ConsensusInterval_t confidence = {0.75f, 0.85f};
        CHECK(consensus_gate_submit_vote_if_live(&state, BOARD_0, confidence, 0u) == 1u);
    }

    consensus_gate_compute(&state, &x, &y, NULL);
    CHECK(x == 1u);

    /* Board 1's team votes while still live - contributes normally. */
    {
        ConsensusInterval_t confidence = {0.60f, 0.70f};
        CHECK(consensus_gate_submit_vote_if_live(&state, BOARD_1, confidence, 0u) == 1u);
    }

    consensus_gate_compute(&state, &x, &y, NULL);
    CHECK(x == 2u);
    CHECK(consensus_gate_tier(x, y) == CONSENSUS_TIER_ELEVATED);

    /* Board 1 genuinely goes dark: time advances past its liveness
       window with no further proof-of-life heard from it, while board 0
       keeps getting refreshed (heartbeats/other traffic), so only board
       1 - not both - crosses below CONSENSUS_LIVENESS_CONFIDENCE_
       THRESHOLD. This is the real fault_buffer.c ramp, not a stub:
       confirm the graded (not boolean) shape along the way. */
    mid_ramp_confidence = fault_buffer_board_liveness_confidence(
        BOARD_1, (3u * FAULT_BUFFER_LIVENESS_WINDOW_TICKS) / 4u);
    CHECK(mid_ramp_confidence > 0.0f && mid_ramp_confidence < 1.0f); /* strictly between - graded, not a step */

    mark_board_heard_at_tick(BOARD_0, FAULT_BUFFER_LIVENESS_WINDOW_TICKS + 1u); /* board 0: still live */
    CHECK(fault_buffer_board_liveness_confidence(BOARD_0, FAULT_BUFFER_LIVENESS_WINDOW_TICKS + 1u) == 1.0f);
    CHECK(fault_buffer_board_liveness_confidence(BOARD_1, FAULT_BUFFER_LIVENESS_WINDOW_TICKS + 1u) == 0.0f); /* board 1: dark */

    /* Another team on board 1 attempts to vote this round while it is
       dark. consensus_gate_submit_vote_if_live() must refuse to forward
       this to consensus_gate_submit_vote() at all - no "no fault"/zero
       placeholder, nothing for this module to later subtract. */
    {
        ConsensusInterval_t confidence = {0.90f, 0.99f}; /* even a maximal-confidence vote must not get in */
        CHECK(consensus_gate_submit_vote_if_live(&state, BOARD_1,
            confidence, FAULT_BUFFER_LIVENESS_WINDOW_TICKS + 1u) == 0u);
    }

    /* x/y must be completely unchanged - the dark declaration arrived
       AFTER board 1's first vote was already recorded, and this module's
       board_mask is a monotonic OR (see consensus_weighting.c), so
       there is no operation that could have moved x back to 1. */
    consensus_gate_compute(&state, &x, &y, NULL);
    CHECK(x == 2u); /* still 2 - not decremented back to 1 */
    CHECK(consensus_gate_tier(x, y) == CONSENSUS_TIER_ELEVATED); /* still elevated */
}

int main(void)
{
    test_worked_example_correlated_board_does_not_outvote_cross_board();
    test_uncorrelated_disagreement_does_not_zero_out_board();
    test_dark_board_mid_vote_does_not_decrement_x();

    if (g_failures == 0)
    {
        printf("consensus_weighting_selftest: all checks passed\n");
        return 0;
    }

    printf("consensus_weighting_selftest: %d check(s) FAILED\n", g_failures);
    return 1;
}

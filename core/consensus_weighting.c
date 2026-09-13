#include "consensus_weighting.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

const float CONSENSUS_TAU_SINGLE_BOARD = INFINITY;

void consensus_gate_reset(ConsensusGateState_t *state)
{
    if(state == NULL)
        return;

    memset(state, 0, sizeof(*state));
}

void consensus_gate_submit_vote(
    ConsensusGateState_t *state,
    uint8_t board_id,
    ConsensusInterval_t confidence)
{
    if(state == NULL)
        return;

    if(state->vote_count >= CONSENSUS_WEIGHTING_MAX_VOTES_PER_ROUND)
        return; /* round already at capacity - drop, don't overflow */

    if(board_id >= 16u)
        return; /* out of board_mask's range - see header's _Static_assert */

    state->intervals[state->vote_count] = confidence;
    state->board_of_vote[state->vote_count] = board_id;
    state->vote_count++;

    /* Monotonic bitwise-OR: only ever sets a bit, never clears one
       (the only place board_mask is ever cleared is consensus_gate_
       reset(), at the next round boundary). This is what makes "a dark
       board must never silently decrement x" a structural property of
       this data structure, not a rule that has to be remembered at
       every call site - there is simply no operation in this file that
       can turn a set bit back off within a round. */
    state->board_mask = (uint16_t)(state->board_mask | (uint16_t)(1u << board_id));
}

uint8_t consensus_gate_submit_vote_if_live(
    ConsensusGateState_t *state,
    uint8_t board_id,
    ConsensusInterval_t confidence,
    uint32_t now)
{
    /* fault_buffer_board_liveness_confidence() already returns 0.0f for
       an out-of-range board_id, so this comparison alone is enough to
       reject one - no separate range check needed before the lookup. */
    if(fault_buffer_board_liveness_confidence(board_id, now) < CONSENSUS_LIVENESS_CONFIDENCE_THRESHOLD)
        return 0u; /* dark board - dropped, never submitted, never an "absent vote" placeholder */

    consensus_gate_submit_vote(state, board_id, confidence);

    /* consensus_gate_submit_vote() has its own independent drop reasons
       (state==NULL, round at capacity) that this wrapper doesn't need to
       re-check - vote_count only increments on an actual accepted
       submission, so comparing state's vote_count before/after would be
       the only way to report those precisely, and callers don't need
       that precision (see this function's own header doc: the return
       value is diagnostic only). Report success whenever the liveness
       gate itself didn't block it. */
    return (state != NULL) ? 1u : 0u;
}

static uint8_t popcount16(uint16_t v)
{
    uint8_t count = 0;

    while(v != 0u)
    {
        v = (uint16_t)(v & (uint16_t)(v - 1u)); /* clears the lowest set bit */
        count++;
    }

    return count;
}

void consensus_gate_compute(
    const ConsensusGateState_t *state,
    uint8_t *out_x,
    float *out_y,
    uint8_t *out_overlap_count)
{
    uint8_t best_count = 0u;
    float best_lo = 0.0f;
    float best_hi = 0.0f;

    if(out_x != NULL)
        *out_x = 0u;
    if(out_y != NULL)
        *out_y = 0.0f;
    if(out_overlap_count != NULL)
        *out_overlap_count = 0u;

    if(state == NULL)
        return;

    /* x is purely board-label agreement - independent of whether the
       confidence intervals below actually overlap (see header doc). */
    if(out_x != NULL)
        *out_x = popcount16(state->board_mask);

    if(state->vote_count == 0u)
        return;

    /*
     * Brooks-Iyengar maximum-overlap region (Brooks & Iyengar, "Robust
     * Distributed Computing and Sensing Algorithm", IEEE Computer
     * 29(6), 1996; see also their "Multi-Sensor Fusion", Prentice Hall,
     * 1998): each vote is an interval, not a point, and the fused value
     * comes from the sub-region covered by the largest number of
     * mutually-overlapping intervals - a plain mean or min can't tell
     * two intervals that genuinely agree (both near 0.8) from two that
     * merely average to the same point (0.9 and 0.3), because it never
     * looks at whether the intervals actually intersect.
     *
     * The maximum overlap count is always achieved starting at one of
     * the intervals' own lower endpoints: coverage count is piecewise
     * constant between consecutive endpoints and can only increase at
     * an interval's lo or decrease just past its hi, so the count
     * achieved AT some lo is never less than the count anywhere in the
     * open interval below the next endpoint. It therefore suffices to
     * test each vote's own lo as a candidate point rather than sweep
     * every real-valued point - O(n^2) here (n = vote_count <=
     * CONSENSUS_WEIGHTING_MAX_VOTES_PER_ROUND, 9 for today's NUM_TEAMS
     * = 10), which is fine at this scale and avoids needing a sort.
     */
    for(uint8_t i = 0u; i < state->vote_count; i++)
    {
        float candidate = state->intervals[i].lo;
        uint8_t count = 0u;
        float region_lo = candidate;
        float region_hi = INFINITY;

        for(uint8_t j = 0u; j < state->vote_count; j++)
        {
            if(state->intervals[j].lo <= candidate && candidate <= state->intervals[j].hi)
            {
                count++;
                if(state->intervals[j].lo > region_lo)
                    region_lo = state->intervals[j].lo;
                if(state->intervals[j].hi < region_hi)
                    region_hi = state->intervals[j].hi;
            }
        }

        if(count > best_count)
        {
            best_count = count;
            best_lo = region_lo;
            best_hi = region_hi;
        }
    }

    if(out_y != NULL)
        *out_y = (best_lo + best_hi) * 0.5f;
    if(out_overlap_count != NULL)
        *out_overlap_count = best_count;
}

ConsensusTier_t consensus_gate_tier(uint8_t x, float y)
{
    /* Elevated-tier acceptance region, written exactly as derived:
       ACCEPT_ELEVATED iff (x>=2) OR (x==1 AND y>=CONSENSUS_TAU_SINGLE_BOARD).
       The x==1 disjunct is structurally dead (CONSENSUS_TAU_SINGLE_BOARD
       is +infinity, so `y >= CONSENSUS_TAU_SINGLE_BOARD` is false for
       every finite y) but is kept literal, not simplified to `x>=2`, so
       the "single-board corroboration is never enough, at any
       confidence" decision stays visible in the code itself - see
       consensus_weighting.h's top-of-file doc for the justification. */
    uint8_t accept_elevated = (uint8_t)((x >= 2u) || (x == 1u && y >= CONSENSUS_TAU_SINGLE_BOARD));

    if(accept_elevated)
        return CONSENSUS_TIER_ELEVATED;

    /* Separate, finite-threshold local-tier gate - same ceiling as "AI
       alone" (Section 3). Deliberately not derived from CONSENSUS_TAU_
       SINGLE_BOARD: this is a different question (is this one board's
       vote confident enough to justify a LOCAL_RECOVERY-level action at
       all?), not a relaxed version of the cross-board question above. */
    if(x == 1u && y >= CONSENSUS_TAU_LOCAL)
        return CONSENSUS_TIER_LOCAL;

    return CONSENSUS_TIER_NONE;
}

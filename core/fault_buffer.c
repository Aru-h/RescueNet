#include "fault_buffer.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Sentinel meaning "never heard from this board" - distinct from tick 0,
   which is a legitimate arrival tick. */
#define FAULT_BUFFER_BOARD_NEVER_HEARD 0xFFFFFFFFu

_Static_assert(NUM_OPERATIONAL_FAULT_CLASSES <= 255u, "fault_buffer_fuse sentinel (NUM_OPERATIONAL_FAULT_CLASSES) must fit in OperationalFaultClass_t");

typedef struct
{
    FaultBufferLocalSample_t samples[FAULT_BUFFER_LOCAL_DEPTH_TICKS];
    uint8_t head;    /* next slot to write */
    uint8_t count;   /* valid samples, <= FAULT_BUFFER_LOCAL_DEPTH_TICKS */
} FaultBufferLocalHistory_t;

typedef struct
{
    FaultBufferRemoteEvidenceEntry_t entries[FAULT_BUFFER_REMOTE_DEPTH];
    uint8_t head;
    uint8_t count;
} FaultBufferRemoteEvidenceHistory_t;

typedef struct
{
    FaultBufferRemoteVoteEntry_t entries[FAULT_BUFFER_REMOTE_DEPTH];
    uint8_t head;
    uint8_t count;
} FaultBufferRemoteVoteHistory_t;

static FaultBufferLocalHistory_t          g_local_history[NUM_TEAMS];
static FaultBufferRemoteEvidenceHistory_t g_remote_evidence_history[NUM_TEAMS];
static FaultBufferRemoteVoteHistory_t     g_remote_vote_history[NUM_TEAMS];
static float                              g_vote_weight[NUM_TEAMS][NUM_OPERATIONAL_FAULT_CLASSES];
static uint32_t                           g_board_last_heard_tick[FAULT_BUFFER_NUM_BOARDS];

/*
 * Runtime board layout - separate from the compile-time FAULT_BUFFER_
 * NUM_BOARDS/FAULT_BUFFER_TEAMS_PER_BOARD macros above on purpose.
 * Those macros represent a real-hardware fact (a physical board can
 * never host more than 5 teams) and back the _Static_assert in
 * fault_buffer.h - that assert must keep checking against the real
 * hardware ceiling regardless of any build's runtime configuration, so
 * it stays a compile-time constant, unconditionally.
 *
 * g_num_boards/g_teams_per_board are what fault_buffer_fuse()'s N_live
 * math and teams_on_board() actually use, set once via fault_buffer_
 * init() from whatever comms_transport_board_count() (and the
 * equivalent per-board team count) report at startup - which can
 * legitimately differ from the hardware ceiling, e.g. the laptop
 * build's -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS override
 * collapsing everything to one board. Defaulted to the real-hardware
 * values here (not 0) so a caller that never calls fault_buffer_init()
 * at all (board firmware doesn't call generate_world(), the only
 * current call site - a pre-existing gap, not new) still gets sane,
 * hardware-realistic behavior rather than a division by zero in
 * teams_on_board().
 */
static uint8_t g_num_boards = FAULT_BUFFER_NUM_BOARDS;
static uint8_t g_teams_per_board = FAULT_BUFFER_TEAMS_PER_BOARD;

void fault_buffer_init(uint8_t num_boards, uint8_t teams_per_board)
{
    /* Defensive clamp: g_board_last_heard_tick[] is sized at compile
       time by the hardware-ceiling macro, so a runtime num_boards
       larger than that would let fault_buffer_fuse()'s loop walk past
       the array. Shouldn't happen (runtime board count only ever
       collapses toward 1, it doesn't grow past the hardware ceiling),
       but stay safe rather than trust the caller. */
    g_num_boards = (num_boards > 0u && num_boards <= FAULT_BUFFER_NUM_BOARDS)
        ? num_boards : FAULT_BUFFER_NUM_BOARDS;
    g_teams_per_board = (teams_per_board > 0u) ? teams_per_board : FAULT_BUFFER_TEAMS_PER_BOARD;

    memset(g_local_history, 0, sizeof(g_local_history));
    memset(g_remote_evidence_history, 0, sizeof(g_remote_evidence_history));
    memset(g_remote_vote_history, 0, sizeof(g_remote_vote_history));
    memset(g_vote_weight, 0, sizeof(g_vote_weight));

    for(uint8_t board = 0u; board < FAULT_BUFFER_NUM_BOARDS; board++)
        g_board_last_heard_tick[board] = FAULT_BUFFER_BOARD_NEVER_HEARD;
}

static void board_mark_heard(uint8_t board_id, uint32_t tick)
{
    if(board_id >= FAULT_BUFFER_NUM_BOARDS)
        return;

    /* Guard against an out-of-order/stale frame walking the last-heard
       tick backwards - only ever advance it. */
    if(g_board_last_heard_tick[board_id] == FAULT_BUFFER_BOARD_NEVER_HEARD ||
       tick >= g_board_last_heard_tick[board_id])
        g_board_last_heard_tick[board_id] = tick;
}

static uint8_t board_is_live(uint8_t board_id, uint32_t now)
{
    uint32_t last_heard = g_board_last_heard_tick[board_id];
    uint32_t age;

    if(last_heard == FAULT_BUFFER_BOARD_NEVER_HEARD)
        return 0u;

    /* now should never be behind last_heard, but don't underflow if a
       caller ever passes a stale "now" - treat that as maximally live. */
    age = (now >= last_heard) ? (now - last_heard) : 0u;

    return (age <= FAULT_BUFFER_LIVENESS_WINDOW_TICKS) ? 1u : 0u;
}

/* Teams hosted on board_id - g_teams_per_board for every board except
   possibly the last, which holds whatever remainder is left over if
   NUM_TEAMS isn't an exact multiple. Runtime value (see its own
   declaration above for why this isn't the compile-time macro). */
static uint8_t teams_on_board(uint8_t board_id)
{
    uint32_t first_team = (uint32_t)board_id * g_teams_per_board;

    if(first_team >= NUM_TEAMS)
        return 0u;

    if(NUM_TEAMS - first_team < g_teams_per_board)
        return (uint8_t)(NUM_TEAMS - first_team);

    return g_teams_per_board;
}

void fault_buffer_record_local_evidence(
    uint8_t team_id,
    const float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES],
    uint32_t tick)
{
    FaultBufferLocalHistory_t *history;
    FaultBufferLocalSample_t *slot;

    if(team_id >= NUM_TEAMS || evidence == NULL)
        return;

    history = &g_local_history[team_id];
    slot = &history->samples[history->head];

    memcpy(slot->evidence, evidence, sizeof(slot->evidence));
    slot->tick = tick;
    slot->known = 1u;

    history->head = (uint8_t)((history->head + 1u) % FAULT_BUFFER_LOCAL_DEPTH_TICKS);
    if(history->count < FAULT_BUFFER_LOCAL_DEPTH_TICKS)
        history->count++;
    /* This board computing its own team's evidence is unconditional
       proof-of-life for this board - unlike votes (fault_buffer_
       record_local_vote()), which only fire when the model flags
       something, this runs every tick regardless. Without this, a
       board that never happens to cast a vote (the model is currently
       always-inert, so today that's every board, always) would never
       mark itself live, and fault_buffer_fuse() would treat a fusion
       target on THIS board's own board as if that board were dead -
       wrongly excluding this board's own other teams from N_live even
       when everything is working. team_id/g_teams_per_board mirrors
       comms_transport_board_id_for_team()'s own team_id/COMMS_
       TRANSPORT_TEAMS_PER_BOARD rule without this file depending on
       comms_transport.h - g_teams_per_board is fault_buffer_init()'s
       runtime-configured value, kept in sync with the real one by
       whoever calls that function. */
    board_mark_heard((uint8_t)(team_id / g_teams_per_board), tick);
}

FaultBufferLocalSample_t fault_buffer_get_local_sample(uint8_t team_id)
{
    FaultBufferLocalSample_t empty = {0};
    const FaultBufferLocalHistory_t *history;
    uint8_t last_index;

    if(team_id >= NUM_TEAMS)
        return empty;

    history = &g_local_history[team_id];
    if(history->count == 0u)
        return empty;

    last_index = (uint8_t)((history->head + FAULT_BUFFER_LOCAL_DEPTH_TICKS - 1u) %
        FAULT_BUFFER_LOCAL_DEPTH_TICKS);

    return history->samples[last_index];
}

void fault_buffer_record_remote_evidence(
    uint8_t target_team_id,
    uint8_t source_board_id,
    const float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES],
    uint32_t arrival_tick)
{
    FaultBufferRemoteEvidenceHistory_t *history;
    FaultBufferRemoteEvidenceEntry_t *slot;

    if(target_team_id >= NUM_TEAMS || evidence == NULL)
        return;

    history = &g_remote_evidence_history[target_team_id];
    slot = &history->entries[history->head];

    memcpy(slot->evidence, evidence, sizeof(slot->evidence));
    slot->arrival_tick = arrival_tick;
    slot->known = 1u;

    history->head = (uint8_t)((history->head + 1u) % FAULT_BUFFER_REMOTE_DEPTH);
    if(history->count < FAULT_BUFFER_REMOTE_DEPTH)
        history->count++;

    board_mark_heard(source_board_id, arrival_tick);
}

FaultBufferRemoteEvidenceEntry_t fault_buffer_get_remote_evidence_sample(uint8_t target_team_id)
{
    FaultBufferRemoteEvidenceEntry_t empty = {0};
    const FaultBufferRemoteEvidenceHistory_t *history;
    uint8_t last_index;

    if(target_team_id >= NUM_TEAMS)
        return empty;

    history = &g_remote_evidence_history[target_team_id];
    if(history->count == 0u)
        return empty;

    last_index = (uint8_t)((history->head + FAULT_BUFFER_REMOTE_DEPTH - 1u) %
        FAULT_BUFFER_REMOTE_DEPTH);

    return history->entries[last_index];
}

void fault_buffer_record_remote_vote(
    uint8_t voter_team_id,
    uint8_t voter_board_id,
    uint8_t target_team_id,
    uint32_t class_mask,
    float weight,
    uint32_t arrival_tick)
{
    FaultBufferRemoteVoteHistory_t *history;
    FaultBufferRemoteVoteEntry_t *slot;

    if(target_team_id >= NUM_TEAMS)
        return;

    history = &g_remote_vote_history[target_team_id];
    slot = &history->entries[history->head];

    slot->voter_team_id = voter_team_id;
    slot->voter_board_id = voter_board_id;
    slot->class_mask = class_mask;
    slot->weight = weight;
    slot->arrival_tick = arrival_tick;
    slot->known = 1u;

    history->head = (uint8_t)((history->head + 1u) % FAULT_BUFFER_REMOTE_DEPTH);
    if(history->count < FAULT_BUFFER_REMOTE_DEPTH)
        history->count++;

    board_mark_heard(voter_board_id, arrival_tick);

    for(uint8_t fault_class = 0u; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
    {
        if((class_mask & (1u << fault_class)) != 0u)
        {
            g_vote_weight[target_team_id][fault_class] += weight;
            /* Cap at NUM_TEAMS so a replayed or duplicate frame can't
               inflate weight beyond what the real voter count allows */
            if(g_vote_weight[target_team_id][fault_class] > (float)NUM_TEAMS)
                g_vote_weight[target_team_id][fault_class] = (float)NUM_TEAMS;
        }
    }
}

void fault_buffer_record_local_vote(
    uint8_t voter_team_id,
    uint8_t board_id,
    uint8_t target_team_id,
    uint32_t class_mask,
    float weight)
{
    (void)voter_team_id; /* nothing to log an arrival_tick against on the
        local path (same tick, same process - see this function's own
        header doc), so unlike the remote path there's no event-log
        entry to attribute this to a voter team; only board-liveness +
        the tally itself are recorded. Kept as a parameter anyway for
        signature symmetry with fault_buffer_record_remote_vote() and
        in case a local event log is ever added later. */

    if(target_team_id >= NUM_TEAMS)
        return;

    /* This board hosting a voting team is itself proof this board is
       live - mark it heard the same way the remote path does, using
       the current tick (no arrival_tick parameter here: it can't be
       stale, so there's nothing to pass through). */
    board_mark_heard(board_id, g_current_tick);

    for(uint8_t fault_class = 0u; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
    {
        if((class_mask & (1u << fault_class)) != 0u)
            g_vote_weight[target_team_id][fault_class] += weight;
    }
}

void fault_buffer_round_reset(void)
{
    memset(g_vote_weight, 0, sizeof(g_vote_weight));
}

float fault_buffer_get_vote_weight(uint8_t target_team_id, OperationalFaultClass_t fault_class)
{
    if(target_team_id >= NUM_TEAMS || (uint32_t)fault_class >= NUM_OPERATIONAL_FAULT_CLASSES)
        return 0.0f;

    return g_vote_weight[target_team_id][fault_class];
}

FaultBufferFusionResult_t fault_buffer_fuse(uint8_t target_team_id)
{
    FaultBufferFusionResult_t result = {0};
    uint8_t target_board;
    uint32_t now = g_current_tick;
    uint32_t n_live_teams = 0u;
    uint8_t f_max;
    OperationalFaultClass_t best_class = (OperationalFaultClass_t)0;
    float best_weight;

    if(target_team_id >= NUM_TEAMS)
        return result;

    target_board = (uint8_t)(target_team_id / g_teams_per_board);

    /* N_live: teams belonging to currently-live boards, minus the
       target team itself (it never votes about itself) if its own
       board is among those counted live. Reduces to the fixed
       NUM_TEAMS-1 whenever every board is live - including the
       degenerate g_num_boards==1 case (laptop build's collapse-to-
       one-board override): with one board, board 0 is always either
       live or dead as a whole, so this still just falls out of the
       same loop rather than needing special-casing. */
    for(uint8_t board = 0u; board < g_num_boards; board++)
    {
        if(board_is_live(board, now))
            n_live_teams += teams_on_board(board);
    }

    if(board_is_live(target_board, now) && n_live_teams > 0u)
        n_live_teams -= 1u;

    result.n_live = (uint8_t)n_live_teams;

    for(uint8_t fault_class = 0u; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
        result.class_weight[fault_class] = g_vote_weight[target_team_id][fault_class];

    if(n_live_teams < FAULT_BUFFER_MIN_LIVE_VOTERS)
    {
        result.fusion_skipped = 1u;
        result.quorum = 0u;
        result.best_class = (OperationalFaultClass_t)NUM_OPERATIONAL_FAULT_CLASSES; /* sentinel — not a real class */
        result.best_ratio = 0.0f;
        return result;
    }

    /* f < N/3 strict bound: f_max = floor((N_live-1)/3), quorum = N_live - f_max.
       The -1 matters - floor(N_live/3) under-counts at exact multiples of 3. */
    f_max = (uint8_t)((n_live_teams - 1u) / 3u);
    result.quorum = (uint8_t)(n_live_teams - f_max);
    result.fusion_skipped = 0u;

    /* Max-overlap winner across classes: the discrete/degenerate case of
       Brooks-Iyengar fusion, since votes are single values rather than
       intervals. Quorum is the same divisor for every class this round,
       so ranking by raw weight and ranking by weight/quorum agree - no
       need to divide before comparing. Ties go to the lowest
       OperationalFaultClass_t value, deterministically. */
    best_weight = result.class_weight[0];
    for(uint8_t fault_class = 1u; fault_class < NUM_OPERATIONAL_FAULT_CLASSES; fault_class++)
    {
        if(result.class_weight[fault_class] > best_weight)
        {
            best_weight = result.class_weight[fault_class];
            best_class = (OperationalFaultClass_t)fault_class;
        }
    }

    result.best_class = best_class;
    result.best_ratio = best_weight / (float)result.quorum;

    return result;
}

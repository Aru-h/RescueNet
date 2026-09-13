#ifndef FAULT_BUFFER_H
#define FAULT_BUFFER_H

#include <stdint.h>

#include "fault_config.h"   /* OperationalFaultClass_t, NUM_OPERATIONAL_FAULT_CLASSES */
#include "mission_state.h"  /* NUM_TEAMS */

/*
 * ---------------------------------------------------------------
 * Local-team buffer
 * ---------------------------------------------------------------
 * Tick-indexed ring of this board's own local team's watchdog evidence
 * vector (core/watchdog.c's accumulated_evidence[NUM_WATCHDOG_EVIDENCE_
 * SLOTS] - that enum/count is private to watchdog.c, so the slot count
 * is mirrored here by value only, not by including it). Depth is a
 * placeholder pending real inter-board lag data from Prompt 3's
 * labeling pass - defined as a constant for exactly that reason, do
 * not hardcode 6/7/8 elsewhere.
 */
#define FAULT_BUFFER_LOCAL_EVIDENCE_SLOTS 7u
#define FAULT_BUFFER_LOCAL_DEPTH_TICKS    8u   /* 6-8 range; provisional */

/*
 * ---------------------------------------------------------------
 * Remote-team buffer
 * ---------------------------------------------------------------
 * Event-indexed (not tick-indexed) ring of the last N received
 * evidence-broadcast/vote updates about a given team, one ring per
 * team, each entry timestamped by the tick it arrived on - mirrors the
 * board-report anchor pattern laptop/snapshot.c already uses for
 * stamina display (MissionLinkAnchor_t + laptop_mission_link_status_
 * received()). Evidence broadcasts and votes are tracked in separate
 * rings (FaultBufferRemoteEvidenceEntry_t / FaultBufferRemoteVoteEntry_t)
 * since they carry different payloads, but both are "the remote-team
 * buffer" conceptually and both use this same depth.
 */
#define FAULT_BUFFER_REMOTE_DEPTH 9u

/* Wire feature count for a FAULT_BROADCAST payload (core/fault_comms.h,
   Prompt 1): 7 accumulated_evidence[] floats + equipment_level/100.0f +
   confirmed_mobility_impairment + incident_priority + victim_count.
   Defined here (not in fault_comms.h) because this module needs it to
   size the remote evidence ring, and fault_comms.c already depends on
   this header. */
#define FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES 11u

/*
 * ---------------------------------------------------------------
 * Board layout / dynamic quorum
 * ---------------------------------------------------------------
 * 5 teams per board (comms_transport.h's "this board may host up to 5
 * local teams", cbba.c's `board * 5u` representative-team pattern) -
 * this is a REAL-HARDWARE FACT, and the two constants below exist
 * only to back the _Static_assert further down: a compile-time check
 * that a single physical board can never contribute enough votes to
 * reach quorum alone, regardless of any build's runtime configuration.
 * They are NOT used for the actual runtime N_live/quorum math anymore
 * - that now runs on values set once via fault_buffer_init() (see its
 * own doc), specifically because a build's runtime board count can
 * legitimately differ from this hardware ceiling (the laptop build's
 * -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS override collapses
 * everything to one board) without changing how many teams a real
 * physical board can host. Conflating "how many boards actually exist
 * right now" with "how many a physical board can ever have" was an
 * earlier version of this file's mistake - keeping them as two
 * separate constants (one compile-time and fixed, one runtime and
 * configurable) resolves it rather than picking one meaning for a
 * name that needed two.
 *
 * N_live is measured in team units - the count of teams belonging to
 * currently-live boards, minus the target team itself if its own
 * board is live - so it reduces to the original fixed N=9 (NUM_TEAMS-1)
 * whenever every board is live (including the degenerate case of a
 * single-board runtime configuration where that one board is live -
 * see fault_buffer_fuse()'s own comment).
 *
 * f_max = floor((N_live-1)/3), quorum = N_live - f_max: the -1 matters,
 * floor(N_live/3) under-counts the Byzantine bound at exact multiples
 * of 3 (e.g. gives f_max=3, quorum=6 at N_live=9, when the correct
 * strict bound f<N/3 gives f_max=2, quorum=7).
 *
 * No explicit per-board weight cap is needed: with FAULT_BUFFER_TEAMS_
 * PER_BOARD teams per board and the target team excluded from its own
 * vote, no single board can ever contribute more votes than that, and
 * FAULT_BUFFER_FULL_STRENGTH_QUORUM (quorum at full mesh strength) is
 * always greater - the _Static_assert below documents and enforces
 * that invariant instead of relying on it silently.
 */
#define FAULT_BUFFER_TEAMS_PER_BOARD 5u
#define FAULT_BUFFER_NUM_BOARDS \
    ((NUM_TEAMS + FAULT_BUFFER_TEAMS_PER_BOARD - 1u) / FAULT_BUFFER_TEAMS_PER_BOARD)

#define FAULT_BUFFER_FULL_STRENGTH_VOTERS (NUM_TEAMS - 1u)
#define FAULT_BUFFER_FULL_STRENGTH_F_MAX  ((FAULT_BUFFER_FULL_STRENGTH_VOTERS - 1u) / 3u)
#define FAULT_BUFFER_FULL_STRENGTH_QUORUM \
    (FAULT_BUFFER_FULL_STRENGTH_VOTERS - FAULT_BUFFER_FULL_STRENGTH_F_MAX)

_Static_assert(FAULT_BUFFER_TEAMS_PER_BOARD < FAULT_BUFFER_FULL_STRENGTH_QUORUM,
    "a single board must not be able to reach quorum alone for any class - "
    "see fault_buffer.h's \"Board layout / dynamic quorum\" section");

/* A board with nothing heard from any of its teams in this many ticks
   is not counted toward N_live. Deliberately NOT derived from
   CBBA_ROUND_TIMEOUT_MS (core/cbba.h, 530ms): that constant is
   enforced against cbba_now_ms(), a genuine wall-clock source, precisely
   because - per cbba.h's own comment - sim ticks don't correspond to
   real elapsed time on hardware (BOARD_TICK_DELAY_MS is 0/unpaced by
   default). Converting a wall-clock ms budget into a tick count would
   just trade one wrong unit for another. Instead this is pinned to
   something already in this module's own tick domain: roughly two
   local-evidence-buffer refresh cycles (FAULT_BUFFER_LOCAL_DEPTH_TICKS)
   worth of silence - a provisional heuristic, not a derived value,
   pending real deployment data on how long a board can legitimately go
   quiet. */
#define FAULT_BUFFER_LIVENESS_WINDOW_TICKS (2u * FAULT_BUFFER_LOCAL_DEPTH_TICKS)

/* Below this many live voters, fusion is skipped entirely for the
   round (not enough voters to say anything Byzantine-safe). */
#define FAULT_BUFFER_MIN_LIVE_VOTERS 3u

typedef struct
{
    float    evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES]; /* full 11-feature vector */
    uint32_t tick;
    uint8_t  known;
} FaultBufferLocalSample_t;

typedef struct
{
    float    evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES];
    uint32_t arrival_tick;
    uint8_t  known;
} FaultBufferRemoteEvidenceEntry_t;

typedef struct
{
    uint8_t  voter_team_id;
    uint8_t  voter_board_id;
    uint32_t class_mask;     /* bit i set = voter flagged OperationalFaultClass_t value i;
                                 a single vote can flag more than one class at once,
                                 matching fault_injector_step()'s lack of a mutual-
                                 exclusivity check between FAULT_CLASS_t values. */
    float    weight;
    uint32_t arrival_tick;
    uint8_t  known;
} FaultBufferRemoteVoteEntry_t;

typedef struct
{
    uint8_t n_live;                                        /* live voter count this round */
    uint8_t quorum;                                        /* 0 if fusion_skipped */
    uint8_t fusion_skipped;                                /* 1 if n_live < FAULT_BUFFER_MIN_LIVE_VOTERS */
    float   class_weight[NUM_OPERATIONAL_FAULT_CLASSES];   /* summed vote weight per class, this round */
    OperationalFaultClass_t best_class;                     /* highest weight/quorum ratio; only meaningful if !fusion_skipped */
    float   best_ratio;                                     /* class_weight[best_class] / quorum; 0 if fusion_skipped */
} FaultBufferFusionResult_t;

/*
 * num_boards/teams_per_board configure the RUNTIME board layout used
 * by fault_buffer_fuse()'s N_live math and teams_on_board() (private
 * to fault_buffer.c) - read once here, not baked into every vote/fuse
 * call, and not threaded through the evaluator signature (which stays
 * fixed at 3 args). Pass comms_transport_board_count() and the
 * equivalent per-board team count at startup (see core/fault_comms.c's
 * item 7 call site) - these can legitimately differ from the compile-
 * time FAULT_BUFFER_NUM_BOARDS/FAULT_BUFFER_TEAMS_PER_BOARD macros
 * above (e.g. the laptop build's -DCOMMS_TRANSPORT_TEAMS_PER_BOARD=
 * NUM_TEAMS override collapses everything to one board at runtime).
 * Those macros stay fixed at their real-hardware values regardless -
 * they only back the _Static_assert, a compile-time hardware
 * invariant that must not move with any build's runtime configuration.
 * Never called (board firmware doesn't call generate_world(), the
 * only current call site) still leaves this module usable, defaulting
 * to the real-hardware values rather than failing.
 */
void fault_buffer_init(uint8_t num_boards, uint8_t teams_per_board);

/*
 * Ingestion points - reset points for whoever owns the real data path
 * on each side (board watchdog step for local evidence; the FAULT_
 * BROADCAST/FAULT_VOTE frame receiver, core/fault_comms.c, Prompt 1,
 * for remote evidence/votes). source_board_id/voter_board_id are
 * passed in by the caller rather than derived here - the caller
 * (fault_comms.c) already knows them from comms_transport_board_id_
 * for_team(), and this module has no reason to depend on comms_
 * transport.h just to re-derive them. Both feed board-liveness
 * tracking (see fault_buffer_fuse()'s N_live), not just votes.
 */
void fault_buffer_record_local_evidence(
    uint8_t team_id,
    const float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES],
    uint32_t tick);

void fault_buffer_record_remote_evidence(
    uint8_t target_team_id,
    uint8_t source_board_id,
    const float evidence[FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES],
    uint32_t arrival_tick);

void fault_buffer_record_remote_vote(
    uint8_t voter_team_id,
    uint8_t voter_board_id,
    uint8_t target_team_id,
    uint32_t class_mask,
    float weight,
    uint32_t arrival_tick);

/* Same-board counterpart to fault_buffer_record_remote_vote() - for
   when a locally-hosted team forms an opinion about another team also
   hosted on this same board, which never needs to touch the wire at
   all (same address space, direct function call). No arrival_tick/
   staleness parameter (same tick, same process, can't be stale by
   definition) - but still marks this board as heard, since a locally-
   hosted team voting is itself proof-of-life for this board, same as
   the remote path does for the sender's board.

   board_id is this board's own id, passed in by the caller (core/
   fault_comms.c already has g_comms_transport_this_board_id via
   comms_transport.h) rather than read from that global here - keeps
   this module's existing boundary intact: board_id is always caller-
   supplied, never internally resolved, so this module has no reason to
   depend on comms_transport.h (see the ingestion-points comment above
   fault_buffer_record_remote_evidence() for the same reasoning applied
   there). */
void fault_buffer_record_local_vote(
    uint8_t voter_team_id,
    uint8_t board_id,
    uint8_t target_team_id,
    uint32_t class_mask,
    float weight);

/* Most recent local evidence sample recorded for team_id, if any -
   returns a sample with known=0 if none has been recorded yet. */
FaultBufferLocalSample_t fault_buffer_get_local_sample(uint8_t team_id);

/* Most recent remote evidence-broadcast sample recorded about
   target_team_id, if any - known=0 if none has been recorded yet. */
FaultBufferRemoteEvidenceEntry_t fault_buffer_get_remote_evidence_sample(uint8_t target_team_id);

/* Zero the running per-team, per-class vote weight tally to start a
   new consensus round. Does not affect the local/remote evidence
   rings or board-liveness tracking, which persist across rounds.
   Round-boundary ownership is outside this module - call site to be
   wired in alongside the FAULT_VOTE receiver. */
void fault_buffer_round_reset(void);

/* Current running vote weight for one (target team, class) pair
   within the current round. */
float fault_buffer_get_vote_weight(uint8_t target_team_id, OperationalFaultClass_t fault_class);

/*
 * Brooks-Iyengar fusion, run independently per class, against a
 * dynamic quorum computed from the number of currently-live boards'
 * worth of teams (see "Board layout / dynamic quorum" above). Ties
 * between classes for best_class are broken by lowest
 * OperationalFaultClass_t value, deterministically.
 *
 * Known simplification, not an oversight: if two classes both clear
 * quorum in the same round (plausible given fault_injector.c's
 * confirmed concurrent-fault injection), only the higher-ratio one is
 * reported as best_class/best_ratio. The other isn't lost - its own
 * class_weight[] entry is still in the result, and it'll surface as
 * best_class on a later round if it's genuinely still active -
 * simultaneous-dual-alarm handling just isn't attempted here.
 */
FaultBufferFusionResult_t fault_buffer_fuse(uint8_t target_team_id);

#endif

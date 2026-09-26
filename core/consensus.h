#ifndef CONSENSUS_H
#define CONSENSUS_H

#include <stdint.h>
#include "consensus_config.h"
#include "mission_state.h"

/*
 * Peer consensus on fault state (CONSENSUS_DESIGN.md).
 *
 * Every team is one virtual node on the vbus. Each tick a node that
 * physically transmitted (team_radio_emitted()) broadcasts a beacon of raw
 * state plus its own verdicts about every other node. Each node derives two
 * per-peer verdicts, mobility and communication, from what its own radio
 * received, and node t fuses the votes about t.
 *
 * Hard rule: nothing here reads team->fault_state. The inputs are received
 * beacons, team_radio_emitted() (the physics-owned transmit gate) and the
 * team's own observable state used to build the beacon.
 *
 * Everything below exists only when VIRTUAL_BUS_ENABLED is nonzero. With it
 * off, the small inline stubs at the bottom keep callers compiling.
 */

#define CONSENSUS_NUM_VOTED_CLASSES 2u

typedef enum
{
    CONSENSUS_CLASS_MOBILITY = 0,
    CONSENSUS_CLASS_COMMUNICATION = 1
} ConsensusClass_t;

typedef enum
{
    CONSENSUS_TIER_NONE = 0,
    CONSENSUS_TIER_LOCAL = 1,       /* diagnostic only (decision D6) */
    CONSENSUS_TIER_ELEVATED = 2
} ConsensusTier_t;

typedef enum
{
    PEER_VERDICT_ABSTAIN = 0,
    PEER_VERDICT_CLEAR = 1,
    PEER_VERDICT_FAULT = 2
    /* wire value 3 is reserved and makes a beacon invalid */
} PeerVerdict_t;

/* Tier value the CSV and the live frame carry when consensus is not built in. */
#define CONSENSUS_TIER_NOT_BUILT (-1)

/* Consensus is always exposed through these, whether or not it is built in. */
typedef struct
{
    ConsensusTier_t tier;
    uint8_t x;              /* distinct agreeing failure domains */
    float y;                /* FTI severity lower bound, 0 if no FAULT votes */
    uint8_t n_fault, n_clear, n_abstain;
    uint8_t n_live, n_dark;
    uint8_t fast_path;      /* communication fast path (design 2.6) active */
    uint8_t n_reporters;    /* |R_k| of the last completed tick */
    uint8_t n_heard_target; /* how many of them heard this team */
    uint32_t round;
} ConsensusResult_t;

typedef struct
{
    uint32_t vbus_link_loss_ppm;
    uint8_t vbus_loss_model;
    uint8_t enforce;
} ConsensusRunInfo_t;

#if VIRTUAL_BUS_ENABLED

/* ---- Constants (design 1-3). All provisional, all overridable. ---- */

#define CONSENSUS_FRAME_BEACON 0x40u
#define CONSENSUS_BEACON_VERSION 1u
#define CONSENSUS_BEACON_HEADER_BYTES 12u
#define CONSENSUS_BEACON_SIZE \
    (CONSENSUS_BEACON_HEADER_BYTES + 2u * CONSENSUS_NUM_VOTED_CLASSES * NUM_TEAMS)

#ifndef CONSENSUS_COMM_WINDOW_TICKS
#define CONSENSUS_COMM_WINDOW_TICKS 6u
#endif
#ifndef CONSENSUS_COMM_FAULT_MISSES
#define CONSENSUS_COMM_FAULT_MISSES 4u
#endif
#ifndef CONSENSUS_COMM_CLEAR_MAX_MISSES
#define CONSENSUS_COMM_CLEAR_MAX_MISSES 1u
#endif
#ifndef CONSENSUS_COMM_MAX_ACCUSED
#define CONSENSUS_COMM_MAX_ACCUSED 2u
#endif
#define CONSENSUS_COMM_MISS_FULL_SCALE 0.667f

/* A mobility window this old (ticks since the last accepted beacon) is
   stale: the verdict abstains instead of repeating an old FAULT or CLEAR. */
#define CONSENSUS_MOB_MAX_AGE_TICKS 4u

/* Reputation: trust = (alpha+1)/(alpha+beta+2). Below 0.5 (integer test
   2(alpha+1) < alpha+beta+2) the observer stops voting on mobility. */
#define CONSENSUS_REPORT_TRUST_NUM 1u
#define CONSENSUS_REPORT_TRUST_DEN 2u

#ifndef CONSENSUS_ROUND_TICKS
#define CONSENSUS_ROUND_TICKS 2u
#endif

/* Fault budget from the CONFIGURED peer count, never from turnout. */
#define CONSENSUS_F ((NUM_TEAMS - 2u) / 3u)
#define CONSENSUS_X_MIN (CONSENSUS_F + 1u)
#define CONSENSUS_MIN_LIVE_VOTERS 3u
#define CONSENSUS_TAU_LOCAL 0.75f
/* The retired single-domain disjunct, kept so "a single domain never
   elevates" stays greppable. y <= 1, so this never holds. */
#define CONSENSUS_TAU_SINGLE_BOARD 3.0e38f

#define CONSENSUS_REPORTER_MIN_HEARD 3u
#define CONSENSUS_FAST_MIN_REPORTERS (2u * CONSENSUS_F + 1u)
#define CONSENSUS_FAST_PATH_CONFIDENCE 0.75f

#define CONSENSUS_DEFAULT_LINK_LOSS 0.05f   /* decision D4 */

_Static_assert(NUM_TEAMS <= 16u, "domain_mask and heard_bits are uint16_t");

/* ---- Wire format (design 1.2) ---- */

typedef struct
{
    uint8_t verdict;    /* PeerVerdict_t */
    uint8_t n_samples;  /* 0..63, window size behind the verdict */
    uint8_t lo_q;       /* 0..15, lo = lo_q / 15, rounded down */
    uint8_t hi_q;       /* 0..15, hi = hi_q / 15, rounded up */
} ConsensusVote_t;

typedef struct
{
    uint8_t version;
    uint8_t team_id;
    uint16_t seq;
    uint8_t status;
    uint8_t loc_x, loc_y;
    uint8_t task_pool, task_slot;
    uint8_t leg_flags;      /* bit0 heading_to_rest, bits1-7 resting_zone_id */
    uint16_t heard_bits;
    ConsensusVote_t votes[CONSENSUS_NUM_VOTED_CLASSES][NUM_TEAMS];
} ConsensusBeacon_t;

/* Unpack results. */
#define CONSENSUS_UNPACK_OK 0
#define CONSENSUS_UNPACK_BAD_LENGTH 1
#define CONSENSUS_UNPACK_BAD_VERSION 2
#define CONSENSUS_UNPACK_BAD_SOURCE 3    /* team_id != vbus src */
#define CONSENSUS_UNPACK_BAD_VERDICT 4   /* reserved verdict value */

/* Packs byte by byte, little-endian. Returns CONSENSUS_BEACON_SIZE. */
uint8_t consensus_beacon_pack(const ConsensusBeacon_t *beacon, uint8_t *out);
int consensus_beacon_unpack(const uint8_t *in, uint8_t len, uint8_t vbus_src,
    ConsensusBeacon_t *out);

/* ---- Pure helpers (unit tested) ---- */

/* Wilson score interval (z = 1.645) on k events in n <= 6 samples, from the
   precomputed table. Bounds are rates in [0, 1]. */
void consensus_wilson(uint8_t n, uint8_t k, float *lo, float *hi);

/* Severity interval: Wilson rate scaled by full_scale, clamped to [0, 1]. */
void consensus_severity_interval(uint8_t n, uint8_t k, float full_scale,
    float *lo, float *hi);

/* Outward-only quantization to 4 bits (lo rounds down, hi rounds up). */
uint8_t consensus_quantize_lo(float lo);
uint8_t consensus_quantize_hi(float hi);

/* Fault-tolerant-interval lower bound: the (f+1)-th largest lo, 0 if fewer
   than f+1 intervals. */
float consensus_fti_lower_bound(const float *lo, uint8_t count, uint8_t f);

/* The x/y gate for one class. verdict[i], lo[i] and domain[i] describe live
   voter i (count of them; n_live == count is checked by the caller's
   bookkeeping, dark nodes are simply absent). is_fast is the communication
   fast path already decided by the caller. */
ConsensusResult_t consensus_gate_compute(uint8_t n_live, const uint8_t *verdict,
    const float *lo, const uint8_t *domain, uint8_t count, uint8_t is_fast);

/* ---- Lifecycle ---- */

typedef struct
{
    uint8_t loss_model;     /* VbusLossModel_t */
    float link_loss;
    float p_gb, p_bg, loss_bad;
} ConsensusBusConfig_t;

/* Set before consensus_init(); consensus_init() keeps the last one set.
   Default: IID at CONSENSUS_DEFAULT_LINK_LOSS. */
void consensus_configure_bus(const ConsensusBusConfig_t *cfg);
void consensus_set_link_loss(float link_loss);
void consensus_init(uint32_t world_seed);

/* Called once per sim tick, from simulation_step(), after movement and
   incident work and before the twin and the watchdog. Design 3.1. */
void consensus_step(uint32_t now);

ConsensusResult_t consensus_get_result(uint8_t team, ConsensusClass_t cls);
ConsensusRunInfo_t consensus_get_run_info(void);

typedef struct
{
    uint32_t beacons_sent, beacons_rx, rx_bad, rx_stale, rx_late;
    uint32_t fast_path_fires;
} ConsensusStats_t;
ConsensusStats_t consensus_get_stats(void);

/* Per-team diagnostics the recovery manager and the CSV share. */
extern uint32_t g_diag_consensus_capped[NUM_TEAMS];
extern uint8_t g_consensus_capped_now[NUM_TEAMS];

/* ---- Log view of one observer about one target (design 5.2) ---- */

typedef struct
{
    uint8_t rx_on_time, rx_late, rx_stale, rx_age_ticks, observer_self_deaf;
    uint8_t comm_window_n, comm_misses;
    uint8_t rep_status, rep_loc_x, rep_loc_y, rep_task_pool, rep_task_slot;
    uint8_t rep_heading_to_rest, rep_resting_zone_id;
    uint8_t leg_changed, mob_sampled, mob_stalled, mob_window_n, mob_stalls;
    uint8_t mob_history;
    ConsensusVote_t vote[CONSENSUS_NUM_VOTED_CLASSES];
    float lo[CONSENSUS_NUM_VOTED_CLASSES], hi[CONSENSUS_NUM_VOTED_CLASSES];
    uint8_t observer_graded_abstain, observer_heard_count, reported_heard_target;
    uint8_t plaus_fail, rep_alpha, rep_beta;
    uint8_t gt_link_dropped;    /* label only: beacon was sent but not heard on time */
    uint8_t gt_loss_model;      /* label only */
} PeerObservation_t;

void consensus_get_peer_observation(uint8_t observer, uint8_t target,
    PeerObservation_t *out);

#if CONSENSUS_TEST_HOOKS
#define CONSENSUS_ANY 0xFFu
/* Voter always votes `verdict` (FAULT with interval [1,1], CLEAR [0,0]) on
   target/class (CONSENSUS_ANY = all). */
void consensus_test_force_verdict(uint8_t voter, uint8_t target, uint8_t cls,
    PeerVerdict_t verdict);
/* Reporter always claims it heard (claim 1) or missed (claim 0) target's
   beacon (CONSENSUS_ANY target = every target). */
void consensus_test_force_heard_bit(uint8_t reporter, uint8_t target, uint8_t claim);
void consensus_test_clear_forced(void);
#endif

#else /* !VIRTUAL_BUS_ENABLED */

static inline void consensus_init(uint32_t world_seed) { (void)world_seed; }
static inline void consensus_step(uint32_t now) { (void)now; }

#endif /* VIRTUAL_BUS_ENABLED */

#endif /* CONSENSUS_H */

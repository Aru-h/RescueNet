# Peer consensus on fault state: design

Status: **design only, nothing implemented.** Branch `feat/consensus`, based on
`origin/submission/single-board-dk` @ `29d855a`.

Goal: each team judges the other teams' fault state from what its own radio
**receives**. A fault is escalated only when independent nodes agree. The
design targets 10 virtual nodes (one per team) on one STM32N6570-DK, talking
over a simulated LoRa-like bus (`vbus_*`), so that the same code carries over
to one board per team later.

**Revision 2 (2026-09-26)** applies the prior-art review in
[docs/research/peer_fault_detection_prior_art.md](docs/research/peer_fault_detection_prior_art.md).
The per-peer detectors stay; the fusion changes:

| # | Change | Where |
|---|---|---|
| 1 | Fault budget `f` fixed from the configured peer count, so `x_min` = 3 always. Silence no longer lowers the bar | §3.3, §3.4, D5 |
| 2 | Supermajority `|F| ≥ 2|C| + 1` replaces bare majority | §3.3 |
| 3 | 16-bit heard-from bitmap in every beacon (50 → 52 bytes) and a communication fast path on the per-tick column of misses | §1.2, §1.3, §2.6, §3.3 |
| 4 | Graded self-deaf rule (an observer about to accuse ≥ 3 targets abstains) and CLEAR at ≤ 1 miss | §2.1 |
| 5 | `y` is the fault-tolerant-interval lower bound (Schmid–Schossmaier), not Brooks–Iyengar | §2.3, §3.3 |
| 6 | Position plausibility checks plus a first-hand beta reputation per target | §2.2 |
| 7 | Correlated-loss bus modes, a randomized-emission fault variant, and liar-plus-dark tests | §3.5, §7 |

Hard rule, restated because every section below depends on it:
`team->fault_state` is the injector's ground truth. Only physics code
(`move_team`, `update_team`, `team_effective_work_rate`) reads it. Nothing in
this design reads it, except the test harness, which uses it as a label.

---

## 0. What exists today and what this design keeps or drops

| Piece | Where | Status | Used here? |
|---|---|---|---|
| Two-axis gate (x = distinct agreeing failure domains, y = Brooks–Iyengar overlap) | retired `core/consensus_weighting.c` (`b8c025c`), rationale in `bba265e:consensus_weighting_design.md` | deleted | **Partly.** The `x` axis (monotonic domain mask per round) and the tier structure of `consensus_gate_tier()` are ported. The threshold is fixed at `f+1` with a supermajority guard, and `y` becomes the fault-tolerant-interval lower bound (§3.3). |
| `fault_buffer_fuse()` quorum fusion, board liveness | `core/fault_buffer.c` | in HEAD, never called | **No.** It sums vote weights, which is the flat scalar scheme the retired doc rejected. Its liveness is per board with a 16-tick window, and here a node is the unit. Left untouched (see §6). |
| `FAULT_BROADCAST` / `FAULT_VOTE` | `core/fault_comms.c` | in HEAD, never called | **No.** Every voter runs `fault_predictor_run()` on the evidence vector the **target** broadcasts. That copies one opinion 9 times, the problem this design exists to fix. |
| `evaluate_consensus_fault()` | `core/recovery_manager.c:1055`, behind `RECOVERY_MANAGER_ENABLE_CONSENSUS_FAULT=0` | parked | **Replaced** by `evaluate_peer_consensus()` (§4). |
| `evaluate_fault_predictor()` | `core/recovery_manager.c:1291` | live, but the model is inert (all scores 0) | Unchanged in v1. |
| Stall rule | `core/watchdog.c:199 update_movement_stalls()` | live | Mirrored exactly on the peer side (§2.2). |
| Heartbeat gap | `core/team.c:473` sets `last_heartbeat_tick`; `core/mission_execution_twin.c:94-123` turns gaps into `communication_health` | live | The heartbeat becomes the **physical transmit gate** of the beacon (§1.4). |

---

## 1. Broadcast: the peer beacon

### 1.1 One frame type, one frame per node per tick

Frame type `CONSENSUS_FRAME_BEACON` (vbus `frame_type` value `0x40`, which is
outside the `LORA_FRAME_TYPE_*` range already in use). Each node sends it to
`VBUS_BROADCAST` once per sim tick, **only if its radio transmitted this tick**
(§1.4). Votes ride inside the beacon, so there is no separate vote frame.

The beacon carries **raw state only**: position, status and travel-leg
identity. It carries nothing that the target's watchdog, twin, predictor or
recovery manager derived. In particular it does **not** carry the
`FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES` evidence vector. That vector is
`accumulated_evidence[]`, which is the target's own detection output, and
peers must not vote from it (§2.4).

### 1.2 Byte layout (little-endian, packed by hand, no struct memcpy)

`fault_comms.c` memcpy's structs that hold floats and padding onto the wire.
The beacon is packed byte by byte in `consensus_beacon_pack()` and
`consensus_beacon_unpack()`, so the layout does not depend on the compiler.

| Offset | Size | Field | Source (sender side) | Notes |
|---|---|---|---|---|
| 0 | 1 | `version` | const `1` | Receiver drops any other value |
| 1 | 1 | `team_id` | node id | Receiver drops the frame if this differs from the vbus `src` (spoof and misroute guard) |
| 2 | 2 | `seq` | `(uint16_t)g_current_tick` at emission | One beacon per tick, so a `seq` gap is a missed beacon. Wraps after 65 536 ticks (27 sim-days) and is compared modulo 2^16 |
| 4 | 1 | `status` | `team->status` (0..3) | |
| 5 | 1 | `loc_x` | `team->loc_x` (0..35) | |
| 6 | 1 | `loc_y` | `team->loc_y` | |
| 7 | 1 | `task_pool` | `team->assigned_task_pool` | These three bytes are the leg identity used by the stall reset |
| 8 | 1 | `task_slot` | `team->assigned_task_slot` (`0xFF` = none) | |
| 9 | 1 | `leg_flags` | bit0 `heading_to_rest`, bits1-7 `resting_zone_id` | |
| 10 | 2 | `heard_bits` | bit *j* set iff this node received *j*'s beacon for tick `seq`−1 on time. Own bit 0 | Feeds the communication fast path and the heard-matrix (§2.6). `uint16_t`, so `NUM_TEAMS ≤ 16` |
| 12 | 2·C·N | `votes[c][j]` | sender's current verdicts (§2) | C = `CONSENSUS_NUM_VOTED_CLASSES` = 2 (mobility, communication), N = `NUM_TEAMS` |

Each vote entry is 2 bytes, indexed by target team id `j`. The sender's own
slot is `0x00 0x00` (abstain):

```
byte a: bits7-6 verdict (0 ABSTAIN, 1 CLEAR, 2 FAULT, 3 reserved)
        bits5-0 n_samples (0..63)   -- window size behind the verdict, for logs
byte b: bits7-4 lo_q, bits3-0 hi_q  -- interval, lo = lo_q/15, hi = hi_q/15
        lo_q = floor(lo*15), hi_q = ceil(hi*15)   (outward rounding: the
        interval only ever widens, it never claims more certainty than the voter had)
```

```c
#define CONSENSUS_BEACON_HEADER_BYTES 12u   /* includes the 2-byte heard_bits */
#define CONSENSUS_BEACON_SIZE \
    (CONSENSUS_BEACON_HEADER_BYTES + 2u * CONSENSUS_NUM_VOTED_CLASSES * NUM_TEAMS)   /* = 52 */
_Static_assert(CONSENSUS_BEACON_SIZE <= LORA_FRAME_MAX_PAYLOAD, "beacon must fit one LoRa frame");
_Static_assert(NUM_TEAMS <= 16u, "domain_mask and heard_bits are uint16_t");
```

**Size: 52 bytes of payload.** A real LoRa frame adds
`LORA_FRAME_HEADER_SIZE` (7) + `LORA_FRAME_TRAILER_SIZE` (21, AEAD tag,
counter and checksum), so the frame is **80 bytes on the wire**. The vbus
charges airtime for the 80, not the 52, so the virtual numbers match what the
real encrypted link will cost. At the `NUM_TEAMS ≤ 16` limit the payload is
76 bytes, well inside 144.

### 1.3 Airtime per round

Same Semtech AN1200.13 formula and radio settings that `comms_transport.c:83-109`
derives (SF7, BW 125 kHz, CR 4/5, explicit header, CRC on, 8-symbol preamble,
LDRO off):

```
Tsym          = 2^7 / 125000                         = 1.024 ms
Tpreamble     = (8 + 4.25) * Tsym                    = 12.544 ms
payloadSymbNb = 8 + ceil((8*80 - 28 + 28 + 16) / 28) * 5
              = 8 + ceil(656 / 28) * 5 = 8 + 24*5    = 128 symbols
Tpayload      = 128 * 1.024                          = 131.072 ms
ToA(beacon)   =                                        143.616 ms
```

| Quantity | Value |
|---|---|
| One beacon | **143.6 ms** (138.5 ms before the heard bitmap) |
| One tick, all 10 nodes transmitting | **1 436 ms** |
| Per node, per hour at `TICK_TIME_SEC = 36` (100 ticks/h) | 14.36 s/h = **0.40 % duty cycle** |
| Whole channel, per hour | 143.6 s/h = 3.99 % occupancy |

The vbus must compute ToA per frame in milliseconds with this formula
(`lora_time_on_air_ms(wire_len)`) and report the sum in
`VbusStats_t.airtime_ms_total`. It must **not** reuse
`comms_transport_advance_channel_busy()`. That model charges
`1 + wire_len/22376` **ticks** per frame, so ten 80-byte beacons would mark the
channel busy for 10 ticks out of every 1.

For comparison, the retired `FAULT_BROADCAST` was `sizeof` = 48 bytes
(52 once the parallel branch widens it to 12 features), about 131 to 144 ms,
sent per team, with `FAULT_VOTE` frames on top. The beacon costs about the same
airtime as the old evidence frame alone, and it also carries every vote.

### 1.4 Cadence and the transmit gate

- **Cadence:** one beacon per node per tick, sent in the transmit phase of
  `consensus_step()` (§3.1).
- **Transmit gate:** a node transmits at tick *k* only if
  `team_radio_emitted(team)`, a new physics-owned predicate in `core/team.c`,
  returns true:

  ```c
  /* Physics output, not ground truth: true iff update_team() produced a
     heartbeat this tick. update_team() is the only code that consults
     fault_state[FAULT_CLASS_COMMUNICATION] to decide this. */
  uint8_t team_radio_emitted(const TeamState_t *team)
  { return team->last_heartbeat_tick == g_current_tick; }
  ```

  A communication fault of severity *s* makes `update_team()` emit on 1 tick
  in *s*+1 (`team.c:473`), so the target's beacons actually disappear from the
  air. Peers observe the silence. They are never told about the fault.
- **Real-radio slotting (for when the vbus becomes LoRa):** all nodes tick
  together, so unslotted beacons would all collide. Transmit at
  `tick_start + team_id * 150 ms`: 10 slots of 143.6 ms airtime plus 6.4 ms
  guard fit in 1.5 s. The vbus does not model collisions in v1. It must model
  per-link loss (§3.5).

### 1.5 Does it fit real LoRa limits?

| Region (repo supports IN865 default, JP920 via `LORA_REGION_JP920`) | Limit | Beacon load | Fits? |
|---|---|---|---|
| Strict 1 % duty-cycle reference (EU868 g-band style; `lora_radio_params.h` describes IN865 as "duty-cycle/power-limited") | 36 s/h per node | 14.36 s/h | **Yes, 2.5× margin** |
| IN865 (LoRaWAN RP002 lists no duty-cycle cap for IN865-867) | power only | n/a | Yes |
| JP920 (ARIB STD-T108: LBT required, ≤ 4 s per transmission, per-hour transmit-time cap, commonly 360 s/h in the short-carrier-sense mode; **verify against the channel plan in use**) | ≤ 4 s/frame, cap per hour | 0.14 s/frame, 14.36 s/h | Yes. The radio must do CAD/LBT, and the vbus does not model that |

**Only in real time.** The table assumes one tick is 36 real seconds. The
virtual bus runs in sim time, and board and laptop runs are unpaced
(`BOARD_TICK_DELAY_MS = 0`), so on one board the airtime is **accounted, not
enforced**. On real per-team radios the minimum real tick period is:

- **14.36 s** to keep each node under 1 % duty cycle, and
- **1.5 s** for the 10-slot TDMA frame to fit on the channel at all.

Any demo paced faster than 14.36 s per tick breaks the 1 % reference. The
airtime report (§7.6) prints both numbers.

---

## 2. Peer voting per class

Every node *p* keeps a `PeerObserver_t` for every other node *t* (9 × 10 = 90
observers, about 16 B each, 1.5 KB total). Verdicts use **integer counts**, so
there is no float threshold and no quantization edge. The interval `[lo, hi]`
feeds only `y` (§3.3).

### 2.1 Communication: missed and late beacons on p's own link

**Inputs** (at observer *p*, about target *t*): arrival of beacon
`seq = k` from *t*.

- **On time:** the beacon for tick *k* is delivered by the receive phase of
  tick *k*+1 (`CONSENSUS_BEACON_DEADLINE_TICKS = 1`). The vbus must deliver
  within 1 tick nominally.
- **Missed:** not on time. Late frames count as missed for the communication
  vote. They are still logged (`rx_late`) and still used for mobility (§2.2).
- **Self-deaf rule, hard part:** if *p* received **no** on-time beacon from
  **any** peer for tick *k*, then tick *k* is dropped from all of *p*'s
  communication windows. When everyone goes silent at once, the problem is
  *p*'s own radio, for example after `vbus_set_node_dark(p, 1)`. Without this
  rule, a node returning from dark would vote FAULT on all 9 teams.
- **Self-deaf rule, graded part** (Lifeguard's finding: false positives are
  dominated by the unhealthy *observer*): if, at one tick, *p* would cast
  communication FAULT on more than `CONSENSUS_COMM_MAX_ACCUSED (2)` targets,
  it abstains on **all** of them that tick and sets `observer_graded_abstain`
  in the log. A degraded receiver that still hears someone every tick is
  caught by this, and the hard rule alone would miss it. Three or more
  genuinely concurrent comm faults are still caught by the fast path (§2.6),
  which uses every reporter's bits, not *p*'s verdicts.

**Window:** a sliding window of the last `CONSENSUS_COMM_WINDOW_TICKS = 6`
counted ticks, stored as a `uint8_t` bitmap (`comm_miss_bits`) plus
`comm_window_n` (0..6).

**Verdict:**

| Verdict | Rule |
|---|---|
| FAULT | `comm_window_n == 6` **and** `misses >= CONSENSUS_COMM_FAULT_MISSES (4)` |
| CLEAR | `comm_window_n == 6` **and** `misses <= CONSENSUS_COMM_CLEAR_MAX_MISSES (1)` |
| ABSTAIN | otherwise (window not full, or 2 to 3 misses), or the graded self-deaf rule fired |

**Why these numbers:**

- Severity 2, used by the combined profile, emits every 3rd tick, which is 4
  misses in 6 → FAULT. Severity 3, used by the communication profile, emits
  every 4th tick, 4 to 5 misses → FAULT. Every profile uses severity ≥ 2
  (`fault_config.c`).
- Healthy target with per-link loss *q*:
  P(≥4 of 6 lost) = 8.6·10⁻⁵ at *q* = 0.05, and 1.7·10⁻² at *q* = 0.20.
  Fusion has to absorb the second figure (§3.3).
- CLEAR at ≤ 1 miss: at *q* = 0.2 a healthy link can vote CLEAR 65.5 % of
  the time, against 26 % for a spotless window. The supermajority guard
  (§3.3) depends on those CLEAR votes existing under loss.
- The 4-of-6 rule is the textbook M-of-N choice (radar rule of thumb
  M = 0.955·N^0.8 = 4 at N = 6). A CUSUM matched to the same false-alarm rate
  alarms at about the same fault tick (4.86 against 4.82 at *q* = 0.05), and a
  phi-accrual detector would never flag these intermittent faults, because
  every beacon that gets through resets it. The rule stays.
- Latency: severity-2 misses fall on fault ticks 1, 2, 4 and 5, so the window
  first holds 4 misses at fault tick 5. Fault durations are 5 to 16 ticks
  (`fault_config.c`), so the shortest faults can end before consensus forms.
  About 5 ticks is close to the information limit for any single-link rule
  at these false-alarm levels; the fast path (§2.6) is what attacks R3.

### 2.2 Mobility: stall fraction from received positions

This is the exact rule from `update_movement_stalls()` (`watchdog.c:199-248`),
run on received beacons instead of on `g_teams[]`. The constants are the
watchdog's, re-exported from `watchdog.h`, not copied:
`WATCHDOG_STALL_WINDOW_TICKS = 4`, `_MIN_SAMPLES = 2`, `_CONFIRM_COUNT = 2`.

Per observer: `last_seq`, `last_status`, `last_loc_x/y`, `last_leg`
(pool, slot, flags), `stall_history` (4 bits), `stall_samples`.

When *p* accepts a beacon *b* from *t* with `b.seq > last_seq` (older or
duplicate frames are dropped and counted as `rx_stale`):

```
leg_changed = b.leg != last_leg                     (pool, slot, heading_to_rest,
                                                     and resting_zone_id if heading_to_rest)
consecutive = (b.seq == last_seq + 1)

if b.status == EN_ROUTE && last_status == EN_ROUTE && consecutive && !leg_changed:
    stalled = (b.loc == last_loc)
    stall_history = ((stall_history << 1) | stalled) & 0xF; stall_samples = min(+1, 4)
elif b.status != EN_ROUTE || leg_changed:
    stall_history = 0; stall_samples = 0            # watchdog's reset, including the leg-change reset
# else: gap in seq, still EN_ROUTE on the same leg -> no sample, no reset
last_* = b.*
```

The only difference from the watchdog is the gap branch. The watchdog sees
every tick. A peer that missed tick *k*−1 cannot tell whether *t* moved between
*k*−1 and *k*, so it takes no sample. It cannot reset either, because it did
not see a leg change. A missed beacon therefore **never creates a false stall**.
It only delays samples. That is also why a communication fault on *t* makes
the mobility voters abstain rather than vote wrongly.

`move_team()` moves a healthy EN_ROUTE team exactly one cell per tick. That
stays true with the 8-direction movement on `feat/core-data-path`, where one
diagonal step is still one cell per tick. So the healthy stall rate is exactly
0.

**Verdict** (the watchdog's own confirm and clear rule):

| Verdict | Rule |
|---|---|
| FAULT | `stall_samples >= 2` **and** `popcount(stall_history) >= 2` |
| CLEAR | `stall_samples == 4` **and** `stall_history == 0` |
| ABSTAIN | otherwise, including every time *t* is not EN_ROUTE |

**Equivalence property (tested in §7):** at zero link loss, with no
communication fault active on *t*, every peer's `stall_history` and
`stall_samples` for *t* equal `g_watchdog_state[t]`'s at every tick.

**Plausibility checks on received positions** (applied to every accepted
beacon, before the stall update; *g* = `b.seq − last_seq`):

| Check | Rule |
|---|---|
| In grid | `loc_x < GRID_WIDTH` and `loc_y < GRID_HEIGHT` |
| Speed | Chebyshev distance `max(|Δx|, |Δy|) ≤ g`: a team moves at most one cell per tick |
| Leg step | consecutive, both EN_ROUTE, same leg: the step is 0 or 1 cell |

A failed check resets the mobility observer for *t* (no sample from an
implausible pair) and counts against *t*'s reputation. The checks catch
buggy or random position streams. They do **not** catch a target that lies
consistently (VeReMi's result for the equivalent "distance moved" check), so
R1 stays open.

**First-hand reputation** (RFSN-style beta counter, per observer–target
pair, never broadcast, so it cannot be bad-mouthed): every consecutive pair
that passes adds 1 to `rep_alpha`, every failure adds 1 to `rep_beta`; both
are `uint8_t` and are halved together when either reaches 255. Trust is
`(α+1)/(α+β+2)`. While trust < `CONSENSUS_REPORT_TRUST_MIN (0.5)`, *p*'s
mobility verdicts about *t* are ABSTAIN. Both counters go in the per-peer log
(§5.2).

**Healthy stalls must be zero.** The zero-baseline analysis (one stall is
conclusive) only holds if a healthy team never stalls. The scenario tests
fail on any stall seen while the harness label says no mobility fault is
active (§7.5), so a legitimate stall case is found rather than tuned around.
2-of-4 is kept for watchdog equivalence; it is a robustness rule, and costs
2 to 3 ticks of mean delay against a 1-stall rule.

### 2.3 Confidence interval [lo, hi]

The interval measures **fault severity** in [0, 1], not the raw rate. That
keeps the interval in the same units that `CONSENSUS_TAU_LOCAL` and the
recovery floors use. Treat it as a **monotone score**, not a calibrated
probability: each peer's interval estimates its own link's statistic, and
Wilson's minimum coverage at *n* ≤ 6 is only 75 to 79 %. Wilson is still the
recommended small-*n* interval (Brown, Cai & DasGupta 2001), and
Clopper–Pearson's guaranteed coverage would cost 25 to 30 % more width for no
behavioural gain.

1. Wilson score interval on (*k* events out of *n* samples), with z = 1.645
   (90 %). *n* ≤ 6, so the bounds come from a **precomputed
   `CONSENSUS_WILSON[n][k]` table** (at most 28 entries): no `sqrtf` on the
   board, and the result is bit-identical on laptop and board.
2. Map through the class's full-scale severity and clamp to [0, 1]:
   - mobility: `/ RECOVERY_MANAGER_MOBILITY_STALL_FULL_SCALE` (0.5, the stall
     rate of a severity-3 fault)
   - communication: `/ CONSENSUS_COMM_MISS_FULL_SCALE` (0.667, the miss rate
     of a severity-2 fault)

| Class | k / n | Wilson rate | Severity interval | Midpoint |
|---|---|---|---|---|
| Mobility | 2/2 | [0.425, 1.0] | [0.85, 1.0] | 0.93 |
| Mobility | 2/3 | [0.254, 0.922] | [0.51, 1.0] | 0.75 |
| Mobility | 2/4 (severity-3 steady state) | [0.182, 0.818] | [0.36, 1.0] | 0.68 |
| Mobility | 3/4 | [0.356, 0.942] | [0.71, 1.0] | 0.86 |
| Communication | 4/6 (severity-2 steady state) | [0.347, 0.883] | [0.52, 1.0] | 0.76 |
| Communication | 5/6 | [0.498, 0.962] | [0.75, 1.0] | 0.87 |
| Communication | 6/6 | [0.689, 1.0] | [1.0, 1.0] | 1.00 |

ABSTAIN entries still carry their interval on the wire, for logs and the
future peer-judge model. FAULT **and** CLEAR intervals enter `y` (§3.3), so a
peer's evidence of low severity is not thrown away.

### 2.4 Why each vote is independent of the target's own conclusions

- **Communication:** the input is the *absence* of frames on *p*'s own link.
  The target cannot report its way out of that. It can only transmit or fail
  to. Per-link loss also differs for every *p*, so each peer's sample is its
  own.
- **Mobility:** the input is a sequence of raw facts (cell, status, leg). The
  peer applies the rule itself, over its own received subsequence. Nothing the
  target concluded is on the wire: no `accumulated_evidence`, no
  `confirmed_mobility_impairment`, no `anomaly_confidence`, no recommendation.
- **What this is not:** independent *sensing*. The target reports its own
  position. A target whose position reporting is buggy or dishonest would
  fool every mobility voter in the same way. The plausibility checks in
  §2.2 catch buggy streams, not consistent lies. On one board, the peers also
  differ only through link loss. Both limits are stated as risks R1 and R2
  in §8. In the literature's terms, mobility consensus is redundancy, not
  corroboration, until a peer can measure something about the target that
  the target did not say (RSSI trend on real per-team radios).

### 2.5 Other fault classes: no peer vote in v1

- **RESOURCE:** changes stamina and equipment drain and on-scene work rate
  (`team.c:454`, `incident.c:866`). Only the target knows its equipment level.
  A peer could only repeat what the target reported, and that is not
  independent.
- **ENVIRONMENT, MISSION:** no physics code reads them outside the injector
  (grep of `core/*.c`). There is nothing physical to observe, so a vote would
  be noise.
- **Candidate for later:** on-scene progress as seen by the co-located support
  team. That is at most one observer, so `x ≤ 1` by construction and it could
  never elevate. Not worth adding now.

`CONSENSUS_NUM_VOTED_CLASSES = 2`, mapped by
`consensus_class_to_fault_class[] = { FAULT_CLASS_MOBILITY, FAULT_CLASS_COMMUNICATION }`.

### 2.6 Communication fast path: the heard-matrix

A communication fault silences the target on **the same ticks for every
peer**, while healthy losses are independent per link. Binary 4-of-6 votes
throw that away. Decentralized change-detection theory (Tartakovsky &
Veeravalli 2008) predicts that fusing quantized observations beats fusing
local decisions at practical false-alarm rates, and the review's calculation
confirms it here: a per-tick column rule alarms at fault tick 1 or 2, against
about tick 5 plus a round for the vote path.

- **Report:** each beacon from *p* sent at tick *k* carries `heard_bits`:
  which peers' tick-(*k*−1) beacons reached *p* on time (§1.2).
- **Heard-matrix:** fuser *t* assembles, per tick *k*, a row per reporter *p*
  from the bitmaps that arrive in beacons for tick *k*+1. `R_k` = reporters
  whose bitmap about tick *k* reached *t*.
- **Unhealthy reporters are dropped:** a reporter whose row shows it heard
  fewer than `CONSENSUS_REPORTER_MIN_HEARD (3)` of the other peers at tick *k*
  is excluded from `R_k` (a row of misses is the observer's radio, a column
  of misses is the target).
- **Column rule:** `silent_k(t)` = `|R_k| ≥ 2f+1 (5)` **and** every reporter
  in `R_k` has *t*'s bit clear.
- **Fast ELEVATED (communication only):** `silent_k(t)` and `silent_{k−1}(t)`.
  Two consecutive silent ticks cover fault ticks 1 and 2 of every
  severity ≥ 2 fault. The evidence reaches *t* in beacons delivered at the
  receive phase of fault tick 4, so the fast path elevates at fault tick 4,
  against about fault tick 7 for the vote path.
- **Evaluated every tick**, not only at round boundaries (step 5b in §3.1),
  so the round length adds no latency to it.
- **Hold:** a fast ELEVATED holds for the current and the next round, OR-ed
  with the vote-path tier (§3.3). `ConsensusResult_t.fast_path` records
  which path fired.
- **Liars:** a reporter that falsely claims "missed" cannot fire the rule
  alone, because every reporter in a set of ≥ 5 must agree. With 7 honest
  reporters, a false column needs all 7 to lose the same beacon, q⁷ ≈ 1.3·10⁻⁵
  per tick at *q* = 0.2, squared for two consecutive ticks under i.i.d. loss.
  A reporter that falsely claims "heard" **blocks** the fast path; the vote
  path then still detects the fault on its normal schedule. That is why the
  vote path stays.
- **Correlated loss** is the open risk: a transmitter-side glitch or shared
  burst that is not a fault looks exactly like a silent column. §7.2 measures
  this before the fast path is trusted.
- **Cost:** 2 bytes per beacon (§1.3: +5.1 ms airtime) and a 10×10 bit
  matrix per fuser.

---

## 3. Fusion

### 3.1 Per-tick order (inside `simulation_step()`, `runs_teams` only)

```
update_all_teams()        physics: heartbeat (the transmit gate), stamina, ...
move_all_teams()          physics: positions
...
consensus_step(now)       NEW, #if VIRTUAL_BUS_ENABLED
   1. vbus_step(now)                       deliver frames sent at now-1
   2. for each node n: drain vbus_poll(n)  -> update observers, store received votes
   3. for each node n: recompute verdicts about every t != n
   4. for each node n: if team_radio_emitted(n): vbus_send(n, VBUS_BROADCAST, BEACON, ...)
   5. if now % CONSENSUS_ROUND_TICKS == CONSENSUS_ROUND_TICKS-1: fuse round, publish results
   5b. every tick: evaluate the communication fast path (§2.6); if it fires, publish ELEVATED now
mission_execution_twin_step(); watchdog_step(); recovery_manager_step();
```

### 3.2 Rounds, and who fuses

- `CONSENSUS_ROUND_TICKS = 2`. Round *r* = ticks [2*r*, 2*r*+1]. Votes ride
  on every beacon, so a short round costs no airtime. It keeps the added
  latency to at most 2 ticks, against faults that last 5 to 16 ticks.
- **Fusion about target *t* runs at node *t*.** It uses the votes that node
  *t* received in the round. That is where team *t*'s recovery manager will
  run once each team has its own board, so the single-board build keeps the
  same data flow. The result published for team *t* is node *t*'s result.
- **Per-round vote set:** for each (voter *p*, class *c*), keep only the
  **latest** vote about *t* received during round *r* (highest `seq`). The set
  only grows during a round and is cleared only at the round boundary. This is
  the retired `board_mask` monotonic-OR property: a voter that goes dark after
  voting cannot be un-counted mid-round.
- The result for round *r* is published at the end of the round's last tick
  and held unchanged through round *r*+1.

### 3.3 x/y rules (one team = one failure domain)

For target *t*, class *c*, round *r*:

- `V` = **live voters**: nodes *p* ≠ *t* from which *t* received at least one
  beacon in round *r*. `n_live = |V|`.
  `n_dark = (NUM_TEAMS − 1) − n_live`.
- `F`, `C`, `A` = voters in `V` whose latest verdict is FAULT, CLEAR or
  ABSTAIN.
- `domain_of(p)`: `p` when `VIRTUAL_BUS_ENABLED` (one team = one node = one
  failure domain), otherwise `comms_transport_board_id_for_team(p)` (unused in
  v1). `domain_mask |= 1 << domain_of(p)` for `p ∈ F`, and
  `x = popcount(domain_mask)`.
- `CONSENSUS_F = (NUM_TEAMS − 2) / 3` = 2: the fault budget, fixed from the
  **configured** peer count (9), the strict f < N/3 bound. It is never
  recomputed from `n_live`. `x_min = CONSENSUS_F + 1` = 3, always.
- `y` = the **fault-tolerant interval lower bound** (Schmid & Schossmaier
  2001): the (`CONSENSUS_F`+1)-th largest `lo` over the intervals of all live
  voters that have an opinion (F ∪ C). If fewer than `CONSENSUS_F`+1 such
  intervals exist, `y` = 0. Reading: with at most *f* faulty peers, at least
  one correct peer is confident the severity is at least `y`. It is
  Lipschitz, so it does not jump when one interval crosses another. This
  replaces the Brooks–Iyengar max-overlap midpoint, whose preconditions (every
  correct interval contains one true value; τ fixed over all N inputs) the
  design does not meet.

```
ELEVATED  iff  n_live >= CONSENSUS_MIN_LIVE_VOTERS (3)    (implied by x >= 3; kept explicit)
          and  x >= x_min (3, fixed)
          and  |F| >= 2|C| + 1                (supermajority of the voters that have an opinion)
          [and the literal retired disjunct: or (x==1 and y >= CONSENSUS_TAU_SINGLE_BOARD=+inf),
           kept in the code so the "a single domain never elevates" decision stays greppable]
          or   (class == COMMUNICATION and the fast path fired, §2.6)
LOCAL     iff  not ELEVATED and x >= 1 and y >= CONSENSUS_TAU_LOCAL
NONE      otherwise
```

When several nodes share one failure domain (not in v1, where one team is
one domain), `|F|` and `|C|` are counted per domain too: a domain is FAULT if
its live FAULT members outnumber its CLEAR members, CLEAR if the reverse.

What changed from the retired gate, and why:

1. **`x_min` = 3, fixed against membership.** The retired rule was a flat
   `x ≥ 2`, and revision 1 recomputed `f` from `n_live`. Byzantine quorum
   constructions (Malkhi & Reiter) fix the fault budget in advance over the
   whole membership, because faulty nodes going silent is itself inside the
   budget. Under the revision-1 rule, two liars with three honest peers dark
   elevated a healthy target in 72 % of rounds at *q* = 0.2 and 6 % at
   *q* = 0.05. With `x_min` fixed at 3, two liars can never elevate, whatever
   the turnout. A real fault is seen by 7 to 9 peers (§2.4), so this costs no
   detection.
2. **Supermajority, `|F| ≥ 2|C| + 1`.** Per-link loss produces false FAULT
   votes (1.7 % per link at *q* = 0.2), and abstention lowers `|C|`.
   Correlated-voter theory (Kaniovski & Zaigraev 2011) shows a quota above the
   correlation-robust level gains reliability as voters become correlated,
   and a bare majority sits below it. Under i.i.d. loss the false ELEVATED
   rate is at most about 4·10⁻⁴ per target-round at *q* = 0.2 (the review's
   figure for `x ≥ 3` with a bare majority; the supermajority only lowers
   it). That figure is the **optimistic** case: at perfect correlation a
   group is only as accurate as one voter, so §7.2 measures correlated loss
   before any threshold is final. CLEAR votes come from live peers with a
   near-clean full window, so they are real evidence. ABSTAIN votes and dark
   nodes never count toward C.
3. **`CONSENSUS_TAU_LOCAL` keeps its name and its 0.75 value**, but `y` is now
   the FTI lower bound on a severity score, not a classifier probability. It
   stays provisional and is re-tuned by the sweep in §7. It only separates
   LOCAL from NONE, and LOCAL has no behavioural effect (§4.3).

### 3.4 Liveness and dark nodes

- A dark node, whether set by `vbus_set_node_dark(n, 1)` or with all its frames
  lost in the round, is **not in V**. It is excluded from `n_live`, F, C and A.
  It is **never** counted as "no fault". It lowers `n_live` and can remove
  a CLEAR, but it **never changes `x_min`** (fixed at 3) and never adds a
  vote.
- `n_live < 3`: at most LOCAL. Already implied by `x ≥ 3`; kept explicit
  (the same constant as `FAULT_BUFFER_MIN_LIVE_VOTERS`).
- Dark reporters also drop out of `R_k` for the fast path, which then needs
  ≥ 5 live reporters to fire at all.
- A dark node also receives nothing. As a fuser it holds its last published
  result for one round, then publishes NONE with `n_live = 0`.
- Coming back from dark: the self-deaf rule (§2.1) removes the dark ticks from
  its communication windows, and mobility observers restart through the
  `seq`-gap branch.
- There is no separate liveness timer. "Heard this round" is the liveness
  signal, and with 2-tick rounds and one beacon per tick it is equivalent to a
  2-tick timeout. This does not solve FLP: a slow node and a dead node look the
  same, and here both are treated as "not heard".

### 3.5 Message loss

| What is lost | Effect |
|---|---|
| One beacon from *t*, on one link | +1 miss in that peer's communication window (this is the signal). No mobility sample. No false stall |
| One beacon from voter *p* | Only loses that beacon's copy of *p*'s votes. Votes repeat every tick, so *p* stays in V if any of its round's beacons arrive |
| All of *p*'s beacons in the round | *p* is dark for the round (above) |
| Late beacon | Missed for the communication vote. Used for mobility if still newest. Counted `rx_late` |

Requirements for the vbus (for the other agent):

- Loss models selected by `VbusConfig_t.loss_model`:
  - `VBUS_LOSS_IID`: independent per-(src, dst) Bernoulli loss `link_loss`
    (the default, and the model every revision-1 number assumed).
  - `VBUS_LOSS_PER_SOURCE`: one draw per sender per tick, applied to all its
    receivers (a transmitter-side glitch that is not a fault).
  - `VBUS_LOSS_GILBERT_ELLIOTT`: per link, good/bad states with `p_gb`,
    `p_bg`, `loss_bad` (bursty links; Hayashibara's WAN trace saw bursts of
    up to 1093 heartbeats).
  - `VBUS_LOSS_SHARED_BURST`: the whole channel drops everything for a burst
    (all links correlated).
- `vbus_set_node_rx_loss(node, p)`: extra receive loss on one node, to model
  a degraded observer for the graded self-deaf test.
- Its **own RNG stream** seeded from `world_seed`. It must never draw from
  `core/random.c`'s stream, or turning the bus on would change the world and
  break the shadow-mode byte-identity test (§7.7).
- Deterministic per-tick delivery order (by src id).
- `vbus_set_node_dark()` blocks both TX and RX.

---

## 4. Recovery manager integration

### 4.1 `CONSENSUS_ENFORCE = 0` (shadow mode)

- `consensus_step()` runs, results go to the CSV and the dashboard.
- `recovery_manager.c` is **byte-for-byte unchanged in behaviour**:
  `evaluators[]` stays at 6 and nothing is capped.
- Every pre-existing CSV column is identical to a build without consensus
  (§7.7). This needs the separate vbus RNG, and no new `snapshot_record()`
  events, because extra event rows would shift the row stream.

### 4.2 `CONSENSUS_ENFORCE = 1`

Requires `VIRTUAL_BUS_ENABLED = 1` (enforced with `#error` otherwise). Two
changes, both inside `recovery_manager_step()`:

**(a) Cap.** After the evaluators run and before `recovery_build_status()`:

```c
#if CONSENSUS_ENFORCE
signals[RM_EVAL_IDX_MOBILITY]      = consensus_cap_signal(signals[RM_EVAL_IDX_MOBILITY],
                                        consensus_get_result(team_id, CONSENSUS_CLASS_MOBILITY).tier);
signals[RM_EVAL_IDX_COMMUNICATION] = consensus_cap_signal(signals[RM_EVAL_IDX_COMMUNICATION],
                                        consensus_get_result(team_id, CONSENSUS_CLASS_COMMUNICATION).tier);
#endif
/* consensus_cap_signal: if tier != ELEVATED and action > LOCAL_RECOVERY
   -> action = LOCAL_RECOVERY (reason and confidence kept), g_diag_consensus_capped[team]++ */
```

| Evaluator | Can be gated | Never gated |
|---|---|---|
| `evaluate_mobility` (including its `TEAM_DEGRADED` branches) | RETURN_TO_SAFE_ZONE, REQUEST_TASK_REALLOCATION, ABORT_MISSION | CONTINUE_MONITORING, LOCAL_RECOVERY |
| `evaluate_communication` | RETURN_TO_SAFE_ZONE, REQUEST_TASK_REALLOCATION, ABORT_MISSION | same |
| `evaluate_progress`, `_stamina`, `_equipment`, `_fault_predictor` | nothing (no peer-voted class behind them) | all |

The cap is by evaluator index, not by reason string, so a new reason cannot
slip past it. `recovery_apply_hysteresis()` needs the capped candidate to
persist 2 to 4 ticks (`*_PERSISTENCE` in `recovery_manager_config.h`), so in
effect ELEVATED must hold across consecutive rounds before anything above
LOCAL_RECOVERY is dispatched.

**(b) New evaluator `evaluate_peer_consensus()`** (7th slot,
`RECOVERY_MANAGER_EVALUATOR_COUNT` becomes `7u` under `CONSENSUS_ENFORCE`).
This replaces the parked `evaluate_consensus_fault()`.

| Tier (highest over the 2 classes) | Signal |
|---|---|
| NONE or LOCAL | CONTINUE_MONITORING, confidence = y (visible in logs, no action) |
| ELEVATED, mobility | the same guards as `evaluate_mobility`: RESTING or `heading_to_rest` → CONTINUE_MONITORING. Stage WORK → REQUEST_TASK_REALLOCATION, otherwise RETURN_TO_SAFE_ZONE. Passed through `recovery_downgrade_to_achievable_action(…, y)` |
| ELEVATED, communication (vote path or fast path) | the same RESTING and `heading_to_rest` guard. EN_ROUTE or ON_SCENE → REQUEST_TASK_REALLOCATION, otherwise RETURN_TO_SAFE_ZONE. Downgraded by y; a fast-path ELEVATED with `y` = 0 (no full windows yet) uses `CONSENSUS_FAST_PATH_CONFIDENCE (0.75)` |

- Reason: `RECOVERY_REASON_CONSENSUS_FAULT` (already in the enum).
- **Never ABORT_MISSION.** Aborting still needs a local evaluator's severe
  branch **and** ELEVATED.

Why (b) is needed, not just (a): on one board, the local evaluators read the
same facts the peers see. Once each team has its own board, a team cannot see
that its own transmissions are not being heard. Only the peers can. (b) is the
path that still works after that move.

### 4.3 How a single node is capped

- A single voter gives x ≤ 1, so the tier is at most LOCAL. LOCAL has **no
  behavioural effect**: the peer evaluator emits CONTINUE_MONITORING, and the
  cap treats LOCAL exactly like NONE.
- The target's own view counts as zero domains. It is never in V, and its
  local evaluators are capped at LOCAL_RECOVERY without ELEVATED. That matches
  the tiered-trust rule already in `evaluate_fault_predictor()`: "own word
  alone" is enough for LOCAL_RECOVERY and no more.
- A voter counts once per round (latest vote), however many beacons it sends.
- Two voters can never elevate, however many peers are dark (`x_min` = 3,
  fixed).
- A single reporter can block the fast path but never fire it (§2.6).

---

## 5. Logging

### 5.1 Main snapshot CSV (`tron_snapshot_dataset.csv`, one row per team per event, appended columns)

The values describe the team as a **target**, taken from the latest published
round result computed at node *t*.

| Column | Type | Meaning |
|---|---|---|
| `consensus_enforce` | 0/1 | Build flag. Constant per run. **Dataset provenance: never mix 0 and 1 runs in one training set** |
| `vbus_link_loss_ppm` | int | Configured per-link loss. Constant per run |
| `consensus_round` | int | Round index of the published result |
| `consensus_tier_mobility`, `consensus_tier_communication` | −1/0/1/2 | −1 = consensus not built in, 0 NONE, 1 LOCAL, 2 ELEVATED |
| `consensus_x_mobility`, `consensus_x_communication` | int | distinct agreeing domains |
| `consensus_y_mobility`, `consensus_y_communication` | %.3f | fused severity (0 if no FAULT votes) |
| `peer_fault_votes_{mobility,communication}` | int | \|F\| |
| `peer_clear_votes_{mobility,communication}` | int | \|C\| |
| `peer_abstain_votes_{mobility,communication}` | int | \|A\| |
| `peer_live_voters`, `peer_dark_voters` | int | `n_live`, `n_dark` |
| `consensus_fast_path_communication` | 0/1 | the published communication ELEVATED came from the fast path (§2.6) |
| `peer_reporters`, `peer_heard_target` | int | `|R_k|` and how many of them heard this team, for the last completed tick |
| `beacon_emitted` | 0/1 | `team_radio_emitted()` this tick |
| `consensus_capped` | 0/1 | ENFORCE cap changed one of this team's signals this tick |

The CSV header is identical in every build. Disabled builds write −1 or 0, so
`data_prep` needs no per-build branches. Board `snapshot.c` is unchanged in v1.

### 5.2 Per-peer observation CSV (`tron_peer_obs.csv`, laptop only, `--peer-log`)

This file is the training set for a future **peer-judge** model, which would
replace the hand rules in §2.1 and §2.2 on the voter side. There is one row per
(tick, observer, target) with observer ≠ target: 90 rows per tick, about 270 k
rows and about 25 MB per 3000-tick seed.

```
run_id, world_seed, cycle, observer_id, target_id,
rx_on_time, rx_late, rx_stale, rx_age_ticks, observer_self_deaf,
comm_window_n, comm_misses,
rep_status, rep_loc_x, rep_loc_y, rep_task_pool, rep_task_slot, rep_heading_to_rest, rep_resting_zone_id,
leg_changed, mob_sampled, mob_stalled, mob_window_n, mob_stalls,
verdict_mobility, lo_mobility, hi_mobility,
verdict_communication, lo_communication, hi_communication,
observer_graded_abstain, observer_heard_count, reported_heard_target,
plaus_fail, rep_alpha, rep_beta,
gt_link_dropped, gt_loss_model
```

- Labels come from joining the main CSV on `(run_id, cycle, target_id)` →
  `fault_active_*`, `fault_severity_*`.
- `gt_link_dropped` is the vbus channel's own ground truth. The `gt_` prefix,
  like `fault_*`, marks **label-only** columns. `data_prep` must drop both
  prefixes from the features.
- Every other column is something observer *p* actually has.

### 5.3 Dashboard live frame (`laptop/debug.c` `LIVEFRAME`)

Per-team object, new fields:

```json
"consensus_tier": [2, 0],                       // [mobility, communication], same codes as the CSV
"peer_votes": { "mob": [7, 1, 1], "comm": [0, 6, 3],   // [fault, clear, abstain]
                "live": 9, "dark": 0, "fast": 0 }  // fast = comm ELEVATED via §2.6
```

Top level: `"vbus": {"sent": …, "delivered": …, "dropped": …, "airtime_ms": …}`
from `vbus_get_stats()`.

`app.py` passes these through. `templates/index.html` adds:

- a tier badge per team (grey NONE, amber LOCAL, red ELEVATED, one per class)
- a vote string such as `M 7F/1C/1A · C 0F/6C/3A`
- a bus line: airtime per tick and drop %

**Not in v1:** board mission mode (the DK over USART1, via mission-link status
v3) does not carry these fields yet. See R8.

---

## 6. Files and build flags

### 6.1 Flags

| Flag | Default (`#ifndef`) | Set by | Effect |
|---|---|---|---|
| `VIRTUAL_BUS_ENABLED` | 0 | laptop `CFLAGS` and the single-board DK submission target | Compiles `vbus.c` and `consensus.c` bodies and the calls in `simulation.c`, `sim_runner.c`, `snapshot.c`, `debug.c` |
| `CONSENSUS_ENFORCE` | 0 | set explicitly (decision D1) | §4.2. `#error` if set without `VIRTUAL_BUS_ENABLED` |
| `CONSENSUS_TEST_HOOKS` | 0 | selftest targets only | `consensus_test_force_verdict()` and `consensus_test_force_heard_bit()`, for the faulty-voter and faulty-reporter tests |
| `CONSENSUS_FAST_PATH` | 1 | `consensus.h` | §2.6 on or off, so its effect can be measured and disabled if correlated loss makes it unsafe |
| `TEAM_TEST_RANDOM_EMISSION` | 0 | scenario tests only | in `update_team()` (physics, so it may read `fault_state`): a comm fault emits with probability 1/(*s*+1) from a dedicated RNG instead of on every (*s*+1)-th tick |

Multi-board builds (`check-board` with `THIS_BOARD_ID=0` and 5 teams per board,
`check-pico`, the two-board harnesses) do not set `VIRTUAL_BUS_ENABLED`. They
compile none of the new code, and the calls into it are `#if`'d out. The
`nm`/grep check in §7.8 enforces this.

The Makefile's ARM DK target today keeps the default of 5 teams per board.
The submission's "one DK hosts all 10 teams" configuration must also set
`COMMS_TRANSPORT_TEAMS_PER_BOARD=NUM_TEAMS`. I could not find where the
submission build does that (not in the `Makefile` or `tron_board/`), so it
needs confirming before wiring.

### 6.2 New files

| File | Contents |
|---|---|
| `core/vbus.h`, `core/vbus.c` | **other agent.** Interface as given, plus `vbus_set_node_rx_loss()`. Requirements in §1.3 and §3.5 (ms ToA, own RNG, the four loss models, dark = no TX/RX, deliver ≤ 1 tick) |
| `core/consensus.h` | `ConsensusTier_t`, `PeerVerdict_t`, `ConsensusClass_t`, `ConsensusInterval_t`, `ConsensusResult_t {tier, x, y, n_fault, n_clear, n_abstain, n_live, n_dark, fast_path, n_reporters, round}`, `PeerObservation_t` (the log view of §5.2). API: `consensus_init(world_seed, link_loss)`, `consensus_step(now)`, `consensus_get_result(team, cls)`, `consensus_cap_signal()`, `consensus_get_peer_observation(obs, tgt)`, the constants in §1 to §3 |
| `core/consensus.c` | observers with plausibility and reputation (§2.1, §2.2), `consensus_beacon_pack()`/`unpack()`, the verdict step with the graded self-deaf rule, the round vote tables, the heard-matrix and fast path (§2.6), `consensus_gate_compute()` (domain mask from `b8c025c`, FTI `y`) and `consensus_gate_tier()` (§3.3), `CONSENSUS_WILSON[7][7]` |
| `laptop/tools/consensus_selftest.c` | unit tests: pack/unpack round-trip, Wilson table against a double-precision reference, gate truth table, the monotonic round set |
| `laptop/tools/consensus_scenario_test.c` | whole-sim scenario tests (§7.1 to §7.5), linked with `CONSENSUS_TEST_HOOKS=1` |

### 6.3 Changed files

| File | Change |
|---|---|
| `core/team.c`, `core/team.h` | + `team_radio_emitted()` (§1.4); `TEAM_TEST_RANDOM_EMISSION` branch in `update_team()` (test builds only) |
| `core/watchdog.h` | export the three `WATCHDOG_STALL_*` constants. `watchdog.c` itself is unchanged |
| `core/simulation.c` | `#if VIRTUAL_BUS_ENABLED consensus_step(g_current_tick);` placed before `mission_execution_twin_step()` |
| `core/sim_runner.c` | `consensus_init()` in `sim_start_seed()` and `sim_start_seed_board()` |
| `core/recovery_manager.c`, `recovery_manager_config.h` | §4.2 cap and evaluator, named `RM_EVAL_IDX_*` indices. **Delete** `evaluate_consensus_fault()` and `RECOVERY_MANAGER_ENABLE_CONSENSUS_FAULT` (decision D3) |
| `laptop/snapshot.c` | §5.1 columns, §5.2 writer |
| `laptop/main.c` | `--vbus-loss=<p>` (default: decision D4), `--peer-log` |
| `laptop/debug.c`, `laptop/tools/app.py`, `laptop/tools/templates/index.html` | §5.3 |
| `Makefile` | `-DVIRTUAL_BUS_ENABLED=1` in laptop `CFLAGS` and the single-board DK target. Add `check-consensus-selftest`, `check-consensus-scenarios`, `check-consensus-airtime`, `check-consensus-shadow-regression`, `check-consensus-multiboard-untouched` |

### 6.4 Deliberately not touched

- `core/fault_buffer.{c,h}` and `core/fault_comms.{c,h}`. They are unused by
  this design. `feat/core-data-path` edits `fault_buffer.h` (widening
  `FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES` to 12), and leaving them alone
  avoids a merge conflict.
- Consensus code never names that macro or any fixed evidence width, because
  the beacon does not carry the evidence vector.
- If a v2 beacon ever adds target-reported features, its size becomes
  `CONSENSUS_BEACON_HEADER_BYTES + 2·C·N + 4·FAULT_BUFFER_REMOTE_EVIDENCE_FEATURES`
  (100 bytes at 12, 128 on the wire, 215 ms ToA), with the same `_Static_assert`
  against `LORA_FRAME_MAX_PAYLOAD`.
- `watchdog.c`, `mission_execution_twin.c`, `comms_transport.c`, `cbba.c`, and
  all board and Pico sources.

---

## 7. Test plan

All tests run with `CONSENSUS_TEST_HOOKS=1` builds of the laptop sim, 5 seeds
× 3000 ticks unless noted. Ground truth (`fault_state`) is read **only** by the
harness, to label and score.

1. **Dropout mid-round.** Profile `mobility`, loss 0. Pick a round *r* where
   target *t* is ELEVATED on mobility. At tick 2*r* (first tick of the round),
   after voter *p* ∈ F has sent its beacon, call `vbus_set_node_dark(p, 1)`.
   Assert:
   - round *r*: *p* still counted, `x` and `n_live` unchanged
   - round *r*+1: *p* ∉ V, `n_dark` +1, `|C|` unchanged (dark ≠ CLEAR)
   - tier(*r*+1) is ELEVATED iff the remaining `x ≥ x_min` and the majority
     still hold, recomputed independently in the test
   - after `vbus_set_node_dark(p, 0)`: *p* casts **zero** communication FAULT
     votes in the next 6 ticks (self-deaf rule)
2. **Message loss.** Profiles `none` and `communication`, loss *q* ∈
   {0, 0.05, 0.10, 0.20, 0.40}, under **every** loss model in §3.5 (i.i.d.,
   per-source, Gilbert–Elliott with mean burst 3 and 10 ticks, shared burst),
   with the fast path on and off, and with the periodic and the
   `TEAM_TEST_RANDOM_EMISSION` fault variants. Report, per cell:
   - false ELEVATED rounds per 1000 target-rounds (profile `none`), split by
     vote path and fast path
   - detection rate: the share of injected communication faults ≥ 6 ticks long
     that reach ELEVATED during the fault or within 3 ticks after
   - latency distribution (fault tick of first ELEVATED)

   **Pass:** 0 false ELEVATED for i.i.d. *q* ≤ 0.05, and ≥ 90 % detection at
   *q* ≤ 0.05 with the periodic fault. Everything else is reported, not
   asserted. If a correlated model produces fast-path false ELEVATED at
   *q* ≤ 0.05, `CONSENSUS_FAST_PATH` is turned off until the rule is fixed
   (R12). The random-emission variant is expected to lower vote-path
   detection (the review computed 71 to 90 % of 6-tick windows); the number
   is recorded, not hidden.
3. **One faulty voter.** `consensus_test_force_verdict(4, *, *, FAULT)`.
   Node 4 always votes FAULT on everyone. Profile `none`, loss 0.05. Assert:
   - no target is ever ELEVATED
   - with `CONSENSUS_ENFORCE=1`, every `recovery_*` column is **byte-identical**
     to the same seed without the liar

   Repeat with two liars (4, 7) at `n_live = 9`: still never ELEVATED
   (`x_min = 3`). Repeat again with the two liars **and three honest peers
   dark** (`vbus_set_node_dark` on 1, 2, 3), at *q* = 0.05 and 0.2: still
   never ELEVATED. This is the case revision 1 failed 72 % of the time.

   **Faulty reporters.** `consensus_test_force_heard_bit()`:
   - two reporters always claim "missed" about a healthy target: the fast
     path never fires
   - one reporter always claims "heard" about a comm-faulted target: the fast
     path never fires, and the vote path still reaches ELEVATED on its normal
     schedule
4. **Communication-fault target voted by ≥ 2 peers.** Profile
   `communication`, loss 0.05. For each injected communication fault (label
   from the harness) lasting ≥ 6 ticks, assert that ≥ 2 distinct peers voted
   communication FAULT about the target within the fault window + 3 ticks, and
   that the target reached ELEVATED at least once. Also assert that no
   *mobility* FAULT is cast about that target during the fault, because gaps
   must not create stalls. **Fast path:** with i.i.d. *q* = 0.05, the fast
   path elevates by fault tick 4 in ≥ 95 % of severity ≥ 2 faults.
5. **Zero votes on `--fault-profile=none`.** Loss 0: **zero FAULT verdicts**,
   any class, any observer, any tick. Loss 0.05: zero ELEVATED, and the FAULT
   verdict count is reported (expected about 0.9 per 10⁴ pair-ticks from
   §2.1).

   **Mobility mirror equivalence** (profiles `none` and `mobility`, loss 0):
   every observer's `stall_history`/`stall_samples` for *t* equals
   `g_watchdog_state[t]`'s at every tick. This needs a test-only watchdog
   accessor under `CONSENSUS_TEST_HOOKS`.

   **Healthy stalls:** any stall observed by any peer (or by the watchdog)
   while the harness label says no mobility fault is active fails the test,
   and the tick, team and leg are printed. Plausibility failures on a healthy
   run are also counted and must be zero.
6. **Airtime report** (`make check-consensus-airtime`, 1000 ticks, profile
   `combined`). Print `vbus_get_stats()`, airtime per tick, per-node duty cycle
   at 36 s per tick, channel occupancy, and the minimum real tick period for
   1 % duty and for 10-slot TDMA. Assert:
   - `lora_time_on_air_ms(80) == 143.616 ms` (±0.001)
   - per-node duty < 1 %
   - `airtime_ms_total == Σ ToA(sent frames)`
7. **Shadow-mode regression** (`CONSENSUS_ENFORCE=0`). 3 seeds × profiles
   `none`, `combined`, `communication`: every pre-existing CSV column is
   byte-identical to `origin/submission/single-board-dk` (via
   `laptop/tools/compare_dataset.py`).
8. **Multi-board untouched.** `make check-board check-pico
   check-two-board-harness check-two-board-cbba-harness
   check-two-board-mission-harness` pass unchanged. `nm` on the board and Pico
   objects shows no `consensus_*` or `vbus_*` symbols.
9. **Unit tests** (`check-consensus-selftest`):
   - beacon pack/unpack round-trip, including a `team_id` ≠ `src` drop and a
     `version` mismatch drop
   - interval quantization is outward-only
   - Wilson table within 1e-4 of a double reference
   - gate truth table over `n_live` 0..9 × |F| × |C|, including that
     `x_min` stays 3 at every `n_live`
   - FTI `y`: equals the 3rd largest `lo`; moves by at most the change in one
     input interval (Lipschitz); 0 with fewer than 3 opinionated voters
   - fast-path column rule: reporter exclusion by row count, ≥ 5 reporters,
     two consecutive silent ticks, hold for one extra round
   - graded self-deaf: a would-be accusation of 3 targets turns into 3
     abstentions
   - plausibility: out-of-grid, 2-cell jump and leg-step failures; beta
     counter halving at 255
   - retired selftests ported where they still apply (uncorrelated
     disagreement and dark mid-vote for `x`); the retired worked example's
     Brooks–Iyengar `y` assertions are dropped
11. **Degraded observer.** `vbus_set_node_rx_loss(3, 0.5)`, profile `none`,
    *q* = 0.05: node 3 never accuses 3 or more targets in one tick, no target
    reaches ELEVATED, and node 3 is excluded from `R_k` on the ticks where it
    heard fewer than 3 peers.
10. **`CONSENSUS_TAU_LOCAL` sweep.** Report only. Tabulate the FTI `y` for true
    and false FAULT sets at *q* ∈ {0.05, 0.2} and pick τ by F1. The same method
    set 0.75 in `b8c025c`. This affects logs only, since LOCAL has no
    behavioural effect.

---

## 8. Risks

| # | Risk | Mitigation or stance |
|---|---|---|
| R1 | **The target reports its own position.** A buggy or dishonest position stream fools every mobility voter the same way. The votes are independent of the target's *conclusions*, not of its *reports* | Plausibility checks and first-hand reputation (§2.2) catch buggy streams, not a consistent liar (VeReMi found the same for the equivalent check). The known fix is physical-layer evidence: RSSI plausibility reached 95.9 % precision and 83.7 % recall on modified VeReMi data. LoRa reports RSSI per packet at no airtime cost, so this is planned for per-team boards; it cannot be tested honestly on one board |
| R2 | **On one board, peers differ only by link loss.** At loss 0, all 9 voters see an identical stream, so x = 9 is "the stream says so", not 9 independent sensors. One-board common-mode faults (CPU, power) are not modelled | Accepted for the simulation. Real diversity arrives with one board per team |
| R3 | **Latency.** The vote path needs about 5 fault ticks, plus a ≤ 2-tick round, plus 2 to 4 ticks of hysteresis. About 5 ticks is the information limit for any single-link rule, so per-peer tuning cannot fix it | Communication: the fast path (§2.6) elevates at fault tick 4. Mobility: no fast path; delay stays. Measured in §7.2 and §7.4 |
| R4 | **A fully dark target (RX dead too) cannot receive the votes about itself**, so its tier is NONE exactly when it is most broken | Future: group-side reallocation by peers (CBBA). Out of v1 |
| R5 | **Training-data drift.** ENFORCE=1 changes trajectories, so data from 0 and 1 runs are different distributions | `consensus_enforce` column plus the never-mix rule (D1) |
| R6 | **Airtime is accounted, not enforced.** Real-time pacing faster than 14.36 s per tick breaks the 1 % reference. Collisions and LBT are not modelled | Report in §7.6. Slot plan in §1.4 |
| R7 | **Correlated loss.** Every false-ELEVATED estimate assumes independent per-link loss, which correlated-voter theory says is the optimistic case. Real LoRa loss is bursty, and nothing in the literature calibrates how correlated it is across receivers | The §3.5 loss models and the §7.2 sweep. If needed, lengthen `CONSENSUS_COMM_WINDOW_TICKS` or raise the quota, at the cost of latency |
| R8 | The DK-over-USART dashboard does not show tiers in v1 | Follow-up: 1 byte per team in mission-link status (2 tier bits × 2 classes, plus a 4-bit fault-vote count) |
| R9 | Unused `fault_buffer`/`fault_comms` code next to the live consensus is confusing | D3 |
| R10 | Constants are provisional: comm window 6 / threshold 4 / CLEAR ≤ 1, round 2, τ 0.75, `CONSENSUS_COMM_MAX_ACCUSED` 2, `CONSENSUS_REPORTER_MIN_HEARD` 3, trust 0.5 | All `#define`s, tuned by §7.2 and §7.10 |
| R11 | **A lying reporter can block the fast path** by claiming it heard a silent target | The vote path stays as the fallback (§7.3 checks this). Raw observations are more exposed to liars than votes (Bayraktar & Lai), which is why the fast path requires every one of ≥ 5 reporters |
| R12 | **A transmitter-side glitch that is not a fault looks like a silent column** and can fire the fast path | `VBUS_LOSS_PER_SOURCE` and the burst models in §7.2; `CONSENSUS_FAST_PATH` can be switched off |
| R13 | The simulator's comm faults are periodic, which flatters every detector | `TEAM_TEST_RANDOM_EMISSION` variant in §7.2 |

---

## 9. Decisions I need from you

**D1. Should the submission build run with `CONSENSUS_ENFORCE=1`?**
Recommendation: **yes**, with one condition.

- *Why:* shadow mode shows nothing in behaviour, and "escalate only when
  independent nodes agree" is the feature. On one board the peers see the
  same facts as the local evaluators, so the behaviour change is small: a
  1 to 3 tick delay on mobility escalations above LOCAL_RECOVERY, a similar
  or smaller one on communication once the fast path fires, and no triggers
  the local evaluators would not also produce on one board.
- *Condition:* an ENFORCE=1 run must pass the existing behaviour gates
  (`compare_behaviour.py`, the stress tests, incidents resolved within your
  tolerance) before the switch.
- *Data:* regenerate the whole training dataset with ENFORCE=1 before the next
  training run, and never mix it with ENFORCE=0 data. The `consensus_enforce`
  column enforces this.
- If there is no time to regenerate and retrain before the deadline, ship
  ENFORCE=0 (shadow) instead of a model trained on a behaviour it will not see.

**D2. Should votes ever become inputs to the fault-predictor model?**
Recommendation: **no, not as inputs to the target's own predictor.**

1. With ENFORCE=1, votes drive the actions that shape future data, so feeding
   them back creates a feedback loop between labels and features.
2. On one board, votes are a function of the same stall and heartbeat facts
   the predictor already sees. They add no information and add a leakage path.
3. The tiered-trust rule wants the AI and the peers as **two separate
   opinions**. Once the model reads votes, "AI + consensus" becomes one opinion
   counted twice.

Instead, use `tron_peer_obs.csv` to train a separate **peer-judge** model that
replaces the voter's hand rules and runs on received data only. Revisit once
per-peer observables carry information the target lacks (real radios: RSSI,
per-link loss).

**D3.** Delete the parked `evaluate_consensus_fault()` and
`RECOVERY_MANAGER_ENABLE_CONSENSUS_FAULT` in this branch? Recommendation:
**yes.** Leave `fault_buffer` and `fault_comms` untouched until
`feat/core-data-path` merges, then decide whether to delete them.

**D4.** Default `--vbus-loss` for the submission and for training runs?
Recommendation: **0.05.** It makes peers actually differ and keeps false
escalations at zero (§7.2). Loss 0 makes consensus a pure delay line.

**D5.** Accept the revised gate: `x_min` fixed at 3 from the configured
membership, and the supermajority `|F| ≥ 2|C| + 1`? Recommendation: **yes**
(§3.3). Revision 1's turnout-dependent `x_min` let two liars with three honest
peers dark elevate a healthy target in 72 % of rounds at *q* = 0.2.

**D6.** Make LOCAL tier diagnostic only (no action)? Recommendation: **yes**
(§4.3). Otherwise one lying voter can push every team into LOCAL_RECOVERY.

**D7.** Ship the communication fast path (§2.6) on by default? Recommendation:
**yes, conditionally.** It is the only change that shortens communication
detection (fault tick 4 against about 7), for 2 bytes per beacon. It stays on
only if the §7.2 correlated-loss sweep shows no fast-path false ELEVATED at
*q* ≤ 0.05; otherwise `CONSENSUS_FAST_PATH=0` until the rule is fixed.

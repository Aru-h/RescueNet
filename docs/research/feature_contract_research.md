# Feature contract research (agent G, research phase)

Scope: how the fault-detection AI gets its inputs on the STM32N6570-DK and over the (virtual) LoRa link.
No tracked file was modified. Experiments live in
[feature_contract_experiments/](feature_contract_experiments/) (throwaway; logs in `results/`).
Companion design: [../ml/FEATURE_CONTRACT_DESIGN.md](../ml/FEATURE_CONTRACT_DESIGN.md).

Base: `origin/submission/single-board-dk` @ 45298e2.

## Changelog

**feat/feature-contract, this pass**: headline finding 1 below (and the
design doc's D1) was wrong about *why* `team_zone_id`/`team_zone_trust`/
`env_raw_travel_cost` aren't board-computable. They said the board has
no way to compute a district id or zone trust for a location without
world storage - true only because no wire frame carried that data yet.
Two new mission-link frame types (`core/mission_link.h` sections 5e/5f,
`MissionLinkDistrictRow_t`/`MissionLinkZoneTrust_t`, USART1 only - single-
board submission, no LoRa/relay wiring) now carry it, the same way the
existing route map already lets a board answer `env_raw_travel_cost`
without world storage. All three features are kept as inputs, not
dropped; a real `check-zone-parity`/`check-zone-refresh` test confirms
a board fed only these frames computes the exact same answers the
laptop (world authority) does, including catching up within one refresh
period if it misses the initial burst. Finding 6 (evidence mirror vs C
watchdog) was acted on: `laptop/snapshot.c` now writes the C watchdog's
7-accumulator evidence vector straight to the training CSV, removing
the train/serve skew this finding flagged - `evidence_channels.py` is
now an optional cross-check only. The final input count is 74, not the
72 this doc's Q1 inventory counted - `feature_spec.json`/`core/
feature_spec.h`/`feature_spec.py` (generated, single source of truth)
have the exact current list; this doc's Q1 table is not updated to
match and should be read as a historical snapshot of the research
phase, not the current contract.

## 0. Headline findings

1. The trained model's 72 inputs are **not all computable on the board**. Board-computable core (46 features) loses only the `environment` class and `mission`; details in Q1/ablation.
2. A model fed the **7 watchdog evidence channels + confirmed_mobility_impairment** already carries most of the signal: a *scores-only* wire format (5 x u8 = 5 B) matches the full-input F1 (macro 0.681 vs 0.681).
3. The lossless packed format is 22.6 B/team/tick absolute (about 7-10 B with keyframe+delta); the quantiser costs <= 0.002 macro-F1.
4. Learned split bottleneck (int8 QAT) is near-lossless at k>=8; PCA/autoencoder are not (0.47-0.56 macro).
5. A peer-side head that trusts a self-report can be fooled by a **common-mode** corrupted report; asymmetric fusion (C*) contains it only for `communication`, not for other classes. Self-reports must be treated as untrusted hints, not ground truth.
6. Python evidence mirror vs C watchdog agreement is 99.977 % on `anomaly_detected`; emitting C evidence directly from snapshot.c would remove the residual and the train/serve skew.

## Q1. Feature inventory (72 columns after data_prep-2.py)

Cadence = share of ticks where the *quantised* value changes (`feature_change_rates.csv`): static (0), slow (<~2 % — classified by script threshold), per-tick.
"Board" = computable in `core/` at run time from observables. "Peer" = a peer can recompute it from another team's received report.

| Feature(s) | Source (file:function) | Type / range | Cadence | Board | Peer recompute |
|---|---|---|---|---|---|
| specialization_0..4 | team config, static | one-hot 5 | static | yes | only if sent once |
| fitness_tier_0..2 | team config | one-hot 3 | static | yes | send once |
| equipment_rope..generator (8) | team config | binary | static | yes | send once |
| equipment_level | core/incident.h `team_base_work_rate` input; snapshot | 0..100 | per-tick (22 % change) | yes | send |
| stamina, twin_actual_stamina | TeamState | 0..100 float | per-tick | yes | send |
| twin_expected_stamina, twin_stamina_error | core/mission_execution_twin.c (twin update) | float; error -64..63.75 | per-tick | yes | derivable from twin fields |
| twin_expected/actual_progress, twin_progress_error | mission_execution_twin.c | 0..1 | per-tick | yes | send |
| twin_remaining_path_cost | mission_execution_twin.c (sentinel 49.04 when not travelling, data_prep fill) | 0..1022 | per-tick | yes | send |
| twin_expected_work_remaining | mission_execution_twin.c:379 | int | per-tick | yes | send |
| twin_mission_stage | mission_execution_twin.c | 0..N | slow/per-tick | yes | send |
| twin_communication_health | mission_execution_twin.c:94-109 (`update_/resolve_communication_health`, windowed) | 0..2 observed (max 2) | slow | yes | derivable from peer comm misses |
| twin_expected_operating_state_0..3, status_0..3 | twin / TeamState | one-hot 4 | slow | yes | send |
| incident_priority, victim_count, incident_req_capability_*, hazard_gas/flood/structural/cbrn | core/incident.h (assigned incident) | small ints / flags | slow | yes | incident is shared world state |
| support_team_-1/4/9 | TeamState.support_team | **data-dependent one-hot (values seen in training)** | slow | yes | send |
| hours_worked | TeamState | 0..~20 | slow (0.4 %) | yes | send |
| evidence_progress..timeout (7) | core/watchdog.c (`accumulate_evidence`, `*_evidence`, `watchdog_get_evidence_vector`) — **mirrored in Python evidence_channels.py from CSV** | 0..1 float | per-tick (56-99 %) | yes (C) | needs sender's observables; else send |
| confirmed_mobility_impairment | recovery_manager.c:1273 `recovery_manager_build_predictor_evidence` path | 0/1 | slow | yes | send |
| team_zone_id_0..9, team_zone_trust | core/search_zones.c `search_zone_trust()`; snapshot.c:267 | one-hot 10; 0..? | slow / per-tick | **NO** (no world storage, `ENVIRONMENT_HAS_WORLD_STORAGE=0`, core/environment.h:213) | laptop only |
| env_raw_travel_cost | environment_cell_travel_cost() at team cell (fault_buffer.h:43) | float | per-tick | **NO** (world storage) | laptop only |
| path_is_unreachable | data_prep derived flag | 0/1 | slow | yes | send |

Flags (not computable on the board): zone one-hot (10), team_zone_trust, env_raw_travel_cost = 12 columns; incident context columns depend on the board mirroring the laptop world (board-side mission redesign) — check per build.

Note: `fault_predictor.h/.c` is inert (`FAULT_MODEL_LOADED 0`); its header comment still describes a stale 9-float layout. `core/fault_comms.c` is dead code (no callers in the build).

### Ablation (06_ablation.py, 2 seeds x 12 epochs, macro F1 on group-split test)

| inputs | n | mob | comm | res | env | mis | macro |
|---|---|---|---|---|---|---|---|
| all 72 | 72 | 0.647 | 0.978 | 0.848 | 0.298 | 0.643 | 0.683 |
| -zone | 61 | 0.664 | 0.978 | 0.853 | 0.185 | 0.649 | 0.666 |
| -zone -env_raw_travel_cost | 60 | 0.665 | 0.978 | 0.856 | 0.175 | 0.652 | 0.665 |
| -incident context | 58 | 0.647 | 0.978 | 0.847 | 0.286 | 0.532 | 0.658 |
| -evidence (raw telemetry only) | 64 | 0.505 | 0.970 | 0.824 | 0.289 | 0.615 | 0.641 |
| -zone -env -incident | 46 | 0.671 | 0.978 | 0.850 | 0.177 | 0.539 | 0.643 |

Reading: zone/env features matter only for `environment` (F1 0.30 -> 0.18); everything else is neutral or slightly better without them. Incident context matters for `mission`. Evidence channels matter for `mobility`.

## Q2. Emit the C watchdog evidence from snapshot.c?

Measured parity (02_evidence_parity.py, 8 seeds, 240 000 rows): `anomaly_detected` agreement 99.977 %; per-flag 99.92-100 %; confidence |mirror - C| mean 1.3e-4, p99 1.1e-3, max 5.7e-2; 0.033 % of ticks forward-filled (no logged row).

Recommendation: **yes, emit** the 7 accumulators + `confirmed_mobility_impairment` (`watchdog_get_evidence_vector`) as extra CSV columns.
- Cost: 8 columns; no new computation (already called for the recovery path).
- Risk: dataset schema change (data_prep must read them; the Python mirror can stay as a cross-check test).
- Benefit: removes mirror drift (tested, small) and the row-gap forward-fill; guarantees train == serve for the most important features.
Not urgent for accuracy; important for the single-source-of-truth contract (Q7).

## Q3. Wire format

Per-feature quantiser (03_wire_format.py -> `wire_spec.json`): stamina 0..100 step 0.25 (error 0.25); stamina_error -64..63.75 step 0.25 (9 bits; max err 0.12); expected_stamina err 0.38; expected/actual progress 1/255 (0.002); progress_err 1/128 (0.008); remaining path cost clip 0..1022 (err 0.0035 after scaling); evidence channels 8-bit (err 0.002) or 6-bit (0.008); small categoricals radix-packed. Static fields (17 columns) sent once; slow (35) per change; per-tick (20).

Bits per record (evidence 8/6/5 bit): 181 / 167 / 160 bits = 23 / 21 / 20 B. Per-tick fields only: 120 bits = 15 B; static+slow 61 bits.

Bytes per team per tick, payload only:

| scheme | mean | p50 | p95 |
|---|---|---|---|
| absolute | 22.6 | | |
| keyframe+delta K=5 | 9.7 | 7.2 | 22.6 |
| K=10 | 8.1 | 6.0 | 22.6 |
| K=20 | 7.3 | 5.9 | 22.6 |
| K=50 | 6.9 | 5.2 | 14.2 |

Airtime (AN1200.13; SF7/BW125/CR4/5, explicit header, CRC on, 8-symbol preamble, LDRO off; frame overhead 28 B = 7 hdr + 21 trailer; script asserts 80 B wire = 143.616 ms; tick 36 s):

| frame | payload | wire | ToA | duty (1 frame/tick) |
|---|---|---|---|---|
| ref: consensus beacon 52 B | 52 | 80 | 143.6 ms | 0.40 % |
| absolute record (1 team) | 23 | 51 | 102.7 ms | 0.29 % |
| delta mean (K=10) | 9 | 37 | 82.2 ms | 0.23 % |
| scores only 5 x u8 | 5 | 33 | 71.9 ms | 0.20 % |
| split k=8 int8 | 8 | 36 | 77.1 ms | 0.21 % |
| beacon + absolute record | 75 | 103 | 174.3 ms | 0.48 % |
| beacon + delta mean | 61 | 89 | 153.9 ms | 0.43 % |
| board hosting 5 teams, 5 abs | 116 | 144 | 235.8 ms | 0.65 % |
| max payload 144 B | 144 | 172 | 276.7 ms | 0.77 % |

All comfortably inside a 1 % duty cycle at 100 ticks/h even for 10 nodes (each node transmits once per tick; the 10-node column of the log is total air per tick, 0.8-2.8 s of a 36 s tick — this is a **shared-channel occupancy** number: 10 nodes x 143 ms = 1.4 s, about 4 %, so collisions are the constraint, not per-node duty).

## Q4. Learned compression vs packed (04_bottleneck.py)

Setup: `00_gen_dataset.sh` generates 4 shards `tron <s*10+1> 10 3000 --fault-profile=combined` (seeds 1-40, 3000 ticks, 10 teams; 1.2 M rows); `01_prepare.py` runs a patched copy of data_prep-2.py (same features, group split by run_id = world_seed; 840k/180k/180k rows; 72 features). Model: MLP (encoder + head), 8 epochs, seeds 0,1,2, per-class thresholds tuned on val, F1 on test. Positive rates 0.235 / 0.235 / 0.235 / 0.056 / 0.118 (mobility, comm, resource, env, mission).

Mean over 3 seeds:

| option | payload B | mobility | communication | resource | environment | mission | macro |
|---|---|---|---|---|---|---|---|
| full-input | n/a (72 floats) | 0.649 | 0.977 | 0.846 | 0.292 | 0.641 | 0.681 |
| (d) scores-only u8 | 5.0 | 0.649 | 0.977 | 0.846 | 0.292 | 0.641 | 0.681 |
| (d) scores-only u4 | 2.5 | 0.648 | 0.977 | 0.844 | 0.293 | 0.641 | 0.681 |
| (e) packed evid=8b | 22.6 | 0.648 | 0.977 | 0.846 | 0.292 | 0.641 | 0.681 |
| (e) packed evid=6b | 20.9 | 0.644 | 0.977 | 0.843 | 0.292 | 0.639 | 0.679 |
| (a) split k=4 int8 QAT | 4.0 | 0.646 | 0.977 | 0.847 | 0.261 | 0.642 | 0.674 |
| (a) split k=4 int4 QAT | 2.0 | 0.640 | 0.976 | 0.842 | 0.258 | 0.636 | 0.671 |
| (b) PCA k=4 int8 | 4 | 0.484 | 0.762 | 0.425 | 0.174 | 0.505 | 0.470 |
| (c) autoencoder k=4 int8 | 4 | 0.526 | 0.901 | 0.431 | 0.110 | 0.470 | 0.487 |
| (a) split k=8 int8 QAT | 8.0 | 0.648 | 0.977 | 0.844 | 0.285 | 0.640 | 0.679 |
| (a) split k=8 int4 QAT | 4.0 | 0.645 | 0.977 | 0.840 | 0.279 | 0.636 | 0.675 |
| (b) PCA k=8 int8 | 8 | 0.534 | 0.914 | 0.439 | 0.168 | 0.508 | 0.513 |
| (c) autoencoder k=8 int8 | 8 | 0.564 | 0.939 | 0.447 | 0.154 | 0.495 | 0.520 |
| (a) split k=16 int8 QAT | 16.0 | 0.645 | 0.977 | 0.846 | 0.290 | 0.642 | 0.680 |
| (a) split k=16 int4 QAT | 8.0 | 0.648 | 0.977 | 0.845 | 0.294 | 0.637 | 0.680 |
| (b) PCA k=16 int8 | 16 | 0.620 | 0.964 | 0.497 | 0.212 | 0.531 | 0.565 |
| (c) autoencoder k=16 int8 | 16 | 0.609 | 0.957 | 0.492 | 0.207 | 0.526 | 0.558 |

Per-class precision/recall are in `results/04_bottleneck.txt`'s JSON (`bottleneck_results.json` in the work dir). Seed-to-seed spread of environment F1 is about +/-0.02 (rare class), so differences below ~0.02 there are noise.

Interpretation: the "(d) scores-only" row is suspiciously equal to full-input; it is a **stand-in** (the five sender-side model scores fed to a thresholding head), so it shows that the sender's own score carries the decision, not that a peer can reconstruct it. Unsupervised compressors (PCA, AE) discard fault-relevant variance; supervised bottleneck (a) does not. The environment class is the hardest (F1 ~0.29 everywhere) and is the only one that suffers at k=4.

Caveat: full-input F1 (0.68 macro) is low for `environment` and `mission`; this is a property of the dataset/labels, not of compression.

## Q5. Trust: peer-side head (05_peer_head.py)

`tron_peer_obs.csv` does not exist on origin/feat/consensus at time of writing. Required columns per (observer, target, tick), for D2 to emit:

`run_id, cycle, observer_id, target_id, heard_this_tick (0/1), rx_age_ticks, rx_on_time_6 (count in last 6 ticks), comm_miss_6, mobility_stall_count_6, target_loc_x, target_loc_y (from received position), plausibility_fail (0/1), heard_count_window, link_rssi/snr (if available)`, plus the target's last-received report bytes (embedding or evidence scores).

Heads (link loss q=0.10; report is the sender's k=8 int8 embedding; sender-side F1 mob 0.644 comm 0.977 res 0.847 env 0.281 mis 0.643):
- A: uses the last received report only.
- B: peer observations only.
- C: fuses report + observations.
- C*: C trained with corrupted-report augmentation (containment).

Macro F1, embedding report (`results/05_peer_head_embedding.txt`):

| condition | A | B | C | C* |
|---|---|---|---|---|
| clean (loss only) | 0.668 | 0.462 | 0.674 | 0.672 |
| frozen 12 ticks | 0.495 | 0.462 | 0.537 | 0.537 |
| all reports "healthy" | 0.140 | 0.462 | 0.233 | 0.338 |
| 30 % reports "healthy" | 0.550 | 0.462 | 0.573 | 0.590 |
| random noise | 0.272 | 0.462 | 0.347 | 0.371 |
| zeroed | 0.161 | 0.462 | 0.228 | 0.308 |

Scores-only report (`results/05_peer_head_scores.txt`): clean C* 0.672, all-healthy C* 0.294, 30 % healthy C* 0.584.

Common-mode test (9 observers, independent link loss, all reports spoofed "healthy"): share of truly faulty target-ticks where >= 7/9 peers output "no fault":

| head | mobility | comm | resource | env | mission |
|---|---|---|---|---|---|
| A | 0.940 | 0.806 | 0.951 | 0.905 | 0.924 |
| C | 0.932 | 0.316 | 0.946 | 0.904 | 0.928 |
| C* | 0.606 | 0.022 | 0.882 | 0.933 | 0.902 |

Answer: **yes**, a corrupted self-report fools every peer identically (every peer sees the same bytes), and quorum voting does not help against common-mode error. Containment: (1) peer-observation-only head B is immune to report corruption but weak on resource/environment/mission (0.42/0.11/0.28) — those classes need the self-report; (2) C* augmentation restores communication and part of mobility; (3) for resource/environment/mission there is no observable peer-side substitute, so a lying node stays undetected by design; mitigate with plausibility checks (report vs peer-observed position/stall/heard-count), freshness limits (frozen reports), and by treating "all healthy while peers observe stalls" as a disagreement alarm. Do not claim more.

## Q6. On-device (STM32N6570-DK)

Not measured on hardware. From ST's public material (unverified in this session — no web access to re-check): STM32N6 has a Cortex-M55 at up to 800 MHz, ~4.2 MB contiguous SRAM and the Neural-ART accelerator (~600 GOPS), which runs **int8 quantised** networks compiled with **ST Edge AI Core** (generates network.c/.h; STAI runtime). Small MLP (72 -> 128 -> 128 -> 5): ~28 k parameters (≈28 kB int8, ≈112 kB float32); a split head/encoder is a few kB.

Estimate for 10 teams/tick: 10 x ~30 k MACs = 0.3 M MACs/tick; even float32 on the M55 is far below a millisecond-to-few-ms scale, and a 36 s tick has enormous slack. Therefore the NPU is **not needed** for latency; a flat C weights header (existing `FAULT_MODEL_LOADED` path in core/fault_predictor.c) with plain C matmul is sufficient, portable to the laptop build, and simpler for parity. Use ST Edge AI only if the model grows (>1 M MACs) or for demonstration. Split models are practical (the encoder is a prefix of the same MLP), but require QAT and shipping two weight sets (encoder on sender, head on peer) sharing one spec.

## Q7. Single source of truth

`data_prep` (or a new `feature_spec` step) emits `feature_spec.json`, from which a generator writes `feature_spec.h` (C: feature order, radix/steps, scaler mean/std as float arrays, one-hot vocabularies, pack/unpack offsets) and `feature_spec.py`. Findings from this research that the spec must fix:
- **Export the pickled scaler**, do not recompute: recomputing from float32 differs by 0.029 sigma; loading the pickle gives 0.0 diff.
- One-hot vocabularies are currently **data-dependent** (support_team -> {-1,4,9}); make them fixed.
- run_id collides across shards unless re-keyed (used world_seed).
- `twin_remaining_path_cost` sentinel fill (49.04 = train mean of travelling rows) must be in the spec.
- Incident features use -1/0 differently between snapshot and board (skew to check).

Tests and thresholds:
1. Round trip pack->unpack: max |err| <= half step per field (table in Q3); exact for categoricals.
2. C-vs-Python feature parity: run the sim, compare board-computed float features to `feature_spec.py` on the CSV: max abs diff <= 1e-5 scaled units (evidence <= 2e-3 if quantised).
3. Model parity: C forward vs torch on 10 000 rows: max |logit diff| <= 1e-3 (float) / <= 1 LSB (int8).
4. Ground truth guard: GROUND_TRUTH_COLUMNS and the value-based leak check stay; add a test that no spec feature name is in GROUND_TRUTH_COLUMNS.
5. Schema hash embedded in every packet header; mismatch => drop.

## Q8. Prior art (real, checkable citations; status marked)

Verified in this session by fetch: none re-verified after the session limit was hit; entries below are reused from the repo's `docs/research/peer_fault_detection_prior_art.md` (origin/feat/consensus) or from memory and are **marked**.

- Kang et al., "Neurosurgeon: Collaborative Intelligence Between the Cloud and Mobile Edge", ASPLOS 2017 — split computing. [unverified this session]
- Tishby, Pereira, Bialek, "The Information Bottleneck Method", 1999 (arXiv physics/0004057) — information bottleneck. [from memory, unverified]
- Alemi et al., "Deep Variational Information Bottleneck", ICLR 2017 (arXiv 1612.00410). [from memory, unverified]
- Semtech AN1200.13, "LoRa Modem Designer's Guide" — time-on-air formula. Used and asserted numerically (80 B wire -> 143.616 ms, matches repo's airtime function).
- van der Heijden et al., "VeReMi: A Dataset for Comparable Evaluation of Misbehavior Detection in VANETs" (arXiv 1804.06701) — peer-based misbehavior detection. [unverified this session]
- O'Keeffe et al., PMC7805982 — LoRa telemetry compression. [reused from repo doc, unverified]
- Jacob et al., "Quantization and Training of Neural Networks for Efficient Integer-Arithmetic-Only Inference", CVPR 2018 (arXiv 1712.05877) — QAT / straight-through fake-quant. [from memory, unverified]
- ST documentation for STM32N6 / Neural-ART / ST Edge AI Core (st.com). [unverified this session; numbers in Q6 must be re-checked before relying on them]

## Things in the brief that turned out different

- `core/fault_comms.c` is dead code (not a live path).
- data_prep-2.py reads `simulation_output_final_v4.csv`, not the snapshot CSV; the experiments patch a copy.
- `fault_predictor.h` comment about a 9-float layout is stale.
- `twin_communication_health` observed max is 2.
- run_id collides across shards; one-hot vocabulary data-dependent; float32 scaler noise.
- The board has no world storage, so zone/trust/travel-cost cannot be computed there.

## Reproduction

```
cd docs/research/feature_contract_experiments
export TRON_G_WORK=/some/scratch/dir REPO=<repo root>
python -m venv $TRON_G_WORK/venv && $TRON_G_WORK/venv/bin/pip install torch numpy pandas scikit-learn
bash 00_gen_dataset.sh                 # 4 shards, seeds 1-40, 3000 ticks, --fault-profile=combined
PY=$TRON_G_WORK/venv/bin/python
$PY 01_prepare.py                      # 840k/180k/180k rows, 72 features
$PY 02_evidence_parity.py              # N_SEEDS=8
$PY 03_wire_format.py
SEEDS=0,1,2 EPOCHS=8 $PY 04_bottleneck.py
LOSS=0.10 REPORT=embed  $PY 05_peer_head.py
LOSS=0.10 REPORT=scores $PY 05_peer_head.py
SEEDS=0,1 EPOCHS=12 $PY 06_ablation.py
```
Datasets are deterministic per seed. Logs from the runs behind this document are in `results/`.

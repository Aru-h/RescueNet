# FEATURE_CONTRACT_DESIGN.md — draft rev 1

Written for: the team deciding how fault-detection inputs are defined, packed and shared. Evidence: [docs/research/feature_contract_research.md](docs/research/feature_contract_research.md). Design only; no code changed.

## 1. Recommendation

**One generated feature spec, a packed per-team record over LoRa, and a peer head that treats self-reports as untrusted hints.**

1. Restrict the model to **board-computable features** (drop zone one-hot, team_zone_trust, env_raw_travel_cost: 12 columns) unless the board mirror provides them. Cost: `environment` F1 0.30 -> ~0.18; other classes neutral or better.
2. Emit the C watchdog evidence (7 accumulators + confirmed_mobility_impairment) from snapshot.c so training uses exactly what the board computes.
3. Wire format = **lossless-ish packed record** (Q3): 23 B absolute, ~8 B mean with keyframe+delta (K=10). Quantiser cost <= 0.002 macro-F1.
4. Learned split embedding (k=8 int8 QAT, 8 B) is a **later option**: near-lossless (0.679 vs 0.681) but needs QAT, two weight sets and a frozen encoder; PCA/AE are rejected (0.47-0.56).
5. Peer trust: head C* (report + peer observations, corruption-augmented). A common-mode spoof still fools resource/environment/mission; document this as a residual risk.
6. Inference: flat C weights header via the existing FAULT_MODEL_LOADED path; NPU not required (about 0.3 M MACs per tick for 10 teams).
7. Single source of truth: data_prep emits `feature_spec.json` -> `feature_spec.h` + `feature_spec.py`; parity tests in section 5.

## 2. Feature spec

Order and vocabularies as in `wire_spec.json` (72 columns, listed in the research doc Q1). Board-computable core = 60 columns (72 minus zone one-hot 10, zone_trust, env_raw_travel_cost -> 60; also drop `path_is_unreachable` if not board-derived). Fixed vocabularies: support_team -> `has_support` (0/1); incident_req_capability {-1,0..3}; specialization 5; fitness 3; status 4; operating state 4. Scaler: export the pickled StandardScaler mean/std as float arrays (do not recompute).

## 3. Wire format (per team record)

| group | fields | bits |
|---|---|---|
| header | schema_hash (8), team_id (4), tick_lo (8), flags: keyframe (1) | 21 |
| static (keyframe only) | specialization (3), fitness (2), equipment flags (8) | 13 |
| slow | status (2), op-state (2), incident_req_cap (3), priority (3), victims (6), hazards (4), mission_stage (3), comm_health (2), has_support (1), hours (8), confirmed_mobility (1) | 35 |
| per-tick | stamina (8, step 0.25), stamina_err (9), exp_stamina (9), progress exp/actual (8+8), progress_err (8), path_cost (10), work_remaining (8), equipment_level (7) | ~75 |
| evidence | 7 x 8-bit (6-bit optional) | 56 |

Absolute record 181 bits = 23 B; delta: bitmask of changed groups + changed fields; keyframe every K=10 ticks. Mean 8.1 B, p95 22.6 B. Beacon (52 B) plus record = 75 B payload, 103 B wire, 174 ms ToA, 0.48 % duty per node at 36 s ticks. Recommend a **separate frame** for records when the beacon is not sent that tick; combine only when both fit under 144 B.

Time on air (SF7/BW125/CR4/5): 23 B -> 102.7 ms; 9 B -> 82.2 ms; 5 x u8 scores -> 71.9 ms. With 10 nodes each sending once per tick, channel occupancy is about 1-1.7 s of 36 s (3-5 %): collisions, not duty cycle, are the limit.

## 4. Files a later implementation would touch (none touched now)

- `data_prep-2.py` (spec generation, fixed vocabularies, scaler export)
- `laptop/snapshot.c` (evidence columns; incident -1/0 alignment)
- new `core/feature_spec.h` / `feature_spec.py` (generated), new `core/feature_pack.c`
- `core/fault_predictor.c/.h` (real input assembly; fix stale 9-float comment)
- `core/lora_frame.h` consumers / D2 beacon code (record frame type)
- `core/Makefile`/tests (parity tests)

## 5. Tests and thresholds

- Pack/unpack: error <= half step per field; categoricals exact.
- C vs Python feature parity: <= 1e-5 (scaled), evidence <= 2e-3 if quantised.
- Model parity C vs torch: <= 1e-3 logit (float), <= 1 LSB (int8).
- GROUND_TRUTH_COLUMNS and the value-based leak check unchanged; test that no spec feature is a ground-truth column.
- Schema hash in packet header; mismatch drops packet.

## 6. Dependencies

- **D2** (feat/consensus): emit `tron_peer_obs.csv` with columns run_id, cycle, observer_id, target_id, heard_this_tick, rx_age_ticks, rx_on_time_6, comm_miss_6, mobility_stall_count_6, target_loc_x/y, plausibility_fail, heard_count_window, (rssi/snr if any); frame type for the record.
- **FIX**: any change to the snapshot columns or twin fields invalidates the spec; freeze before data generation.
- **E** (dataset generation): must use unique run_id (world_seed), fixed vocabularies, and the evidence columns.

Before data generation: decisions D1-D4 below. After training: D8-D9 (weights, thresholds).

## 7. Decisions for the user

| # | Decision | Recommendation |
|---|---|---|
| D1 | Zone/trust/env-cost features: drop or mirror from laptop | Drop (env F1 loss accepted), revisit if `environment` matters |
| D2 | Emit C evidence from snapshot.c | Yes |
| D3 | Fix incident -1/0 train/serve skew | Yes, before data generation |
| D4 | Fixed one-hot vocabularies (support_team -> has_support) | Yes |
| D5 | Absolute vs delta records | Delta with K=10 keyframes; absolute fallback |
| D6 | Beacon + record combined or separate frame | Separate; combine when < 144 B |
| D7 | Split embedding | Later option, not v1 |
| D8 | Flat C header vs NPU | Flat C header |
| D9 | Peer trust fusion | Head C*; document residual common-mode risk |
| D10 | run_id uniqueness | run_id = world_seed, enforced in data_prep |

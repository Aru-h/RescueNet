# core/ Function-by-Function Audit (laptop build target)

Scope: `core/*.c` (CORE_SOURCES per Makefile — top-level core/ only, `core/vendor/` excluded since the laptop target never links it). `laptop/tools/*.py` and `templates/` explicitly excluded per audit instructions.

Files audited in this pass (alphabetical, matching `$(wildcard core/*.c)`):
aead_chacha20poly1305.c, allocator.c, cbba.c, chacha20.c, comms_transport.c, environment.c, fault_buffer.c, fault_comms.c, fault_config.c, fault_injector.c, fault_predictor.c, grid.c, incident.c, lora_crypto.c, lora_frame.c, mission_execution_twin.c, mission_link.c, mission_link_handshake.c, mission_link_override.c, mission_state.c, mission_state_utils.c, path.c, poly1305.c, random.c, recovery_manager.c, safezone.c, scoring.c, search_zones.c, sim_runner.c, simulation.c, team.c, test_metrics.c, watchdog.c

Findings are appended incrementally, function by function, in file order.

---

## core/aead_chacha20poly1305.c

### 1. `aead_pad16` static — L18
**SEVERITY: NONE**
NONE — computes `16 - (n % 16)` with correct `rem==0` special case so it never returns 16; only called with `n` from bounded `aad_len`/`len` values, no overflow possible.
- **Connects to:** aead_build_mac_data

### 2. `aead_store_le64` static — L24
**SEVERITY: NONE**
NONE — fixed 8-iteration loop writing exactly 8 bytes; caller always has 8 bytes of room left at the write offset.
- **Connects to:** aead_build_mac_data

### 3. `aead_build_mac_data` static — L43
**SEVERITY: NONE**
NONE — has no bounds checks of its own on `aad_len`/`len` against `out`'s capacity, but both call sites (`aead_chacha20poly1305_seal`/`_open`) validate `len <= AEAD_CHACHA20POLY1305_MAX_DATA_BYTES` (144) and `aad_len <= AEAD_CHACHA20POLY1305_MAX_AAD_BYTES` (8) before calling it, and the worst case (176 bytes) fits within `AEAD_MAC_DATA_MAX_BYTES` (192) with margin. Documented "caller must validate" contract, not enforced defensively inside — a future third caller that skips the check would overflow `mac_data`, but not a live bug given the current two callers.
- **Connects to:** aead_pad16, aead_store_le64, aead_chacha20poly1305_seal, aead_chacha20poly1305_open

### 4. `aead_derive_poly1305_key` static — L81
**SEVERITY: NONE**
NONE — fixed-size local `block[CHACHA20_BLOCK_BYTES]` (64) fully filled by `chacha20_block`, then only the first `POLY1305_KEY_BYTES` (32) copied out.
- **Connects to:** chacha20_block

### 5. `aead_tag_equal` static — L100
**SEVERITY: NONE**
NONE — constant-time compare correctly ORs all byte diffs across the full fixed tag length with no early exit, matching its documented anti-timing-oracle purpose.
- **Connects to:** aead_chacha20poly1305_open

### 6. `aead_chacha20poly1305_seal` global — L114
**SEVERITY: NONE**
NONE — validates `len`/`aad_len` against documented max bounds before touching any buffer; uses counter 1 for ciphertext keystream (counter 0 reserved for the Poly1305 key, matching RFC 8439); derives `mac_data_len` from `aead_build_mac_data`'s return value rather than a hardcoded size.
- **Connects to:** aead_derive_poly1305_key, chacha20_xor, aead_build_mac_data, poly1305_mac

### 7. `aead_chacha20poly1305_open` global — L147
**SEVERITY: NONE**
NONE — same bound checks as seal(); decrypts only after `aead_tag_equal` succeeds, so a forged/corrupted tag never causes unauthenticated plaintext to be written (matches the documented "return 0, write nothing" contract).
- **Connects to:** aead_derive_poly1305_key, aead_build_mac_data, poly1305_mac, aead_tag_equal, chacha20_xor

## core/allocator.c

### 8. `select_best_team` global — L26
**SEVERITY: NONE**
NONE — `best_score` initialized to `-1.0f`, below any real score, so the first eligible team always wins; tie-break only fires when `best_team != NO_ASSIGNMENT`, avoiding a spurious coin-flip against the sentinel. Exact float `==` for tie detection is deliberate (scores built from a small, bounded set of terms), not an unintended bug.
- **Connects to:** team_can_accept_incident, calculate_score, random_range_stream

### 9. `assign_incident` global — L57
**SEVERITY: NONE**
NONE — `team_id`/`incident_slot` bounds-checked against `NUM_TEAMS`/`NUM_INCIDENTS` before indexing `g_teams`/`g_incidents`.
- **Connects to:** cbba_sync_mission_execution_twin, snapshot_record; called by allocate_all_incidents

### 10. `allocate_all_incidents` global — L79
**SEVERITY: LOW**
The inner loop searching `agent->path[j]` only ever assigns `team->incident_queue_slot[0]`/`incident_queue_id[0]` (hardcoded index 0) and immediately `break`s after the first matching task, so a team's `incident_queue_*` arrays (sized `CBBA_MAX_BUNDLE_SIZE`=3) never get more than one entry populated even when the team's CBBA path holds multiple incident tasks — extra capacity silently unused, a stale assumption if downstream code expects more than one queued incident.
- **Connects to:** cbba_run, search_generate_sweep_tasks, generate_evacuee_offboard_incidents, assign_incident, select_best_team, cbba_sync_mission_execution_twin, snapshot_record, allocate_support_team
   **RESOLVED** — commit `d044a24`. Now queues every qualifying task instead of hardcoding index 0/breaking after the first.

### 11. `team_can_accept_incident` global — L173
**SEVERITY: NONE**
NONE — `team`/`incident` NULL-checked up front; every documented historical bug in this function's comments already has an accompanying fix in the code; no new issue found.
- **Connects to:** recovery_manager_get_recommendation, get_soft_stamina_floor, get_ready_stamina, count_available_teams_for_capability, count_open_incidents_for_capability; called by cbba_team_can_execute_task, cbba_build_bundle

### 12. `count_available_teams_for_capability` static — L285
**SEVERITY: NONE**
NONE — simple bounded loop over `NUM_TEAMS`, `uint8_t count` can't overflow given `NUM_TEAMS`(10).
- **Connects to:** team_can_accept_incident

### 13. `count_open_incidents_for_capability` static — L301
**SEVERITY: NONE**
NONE — two bounded loops over `NUM_INCIDENTS`/`NUM_SYSTEM_TASKS`; `uint8_t count` can't overflow.
- **Connects to:** team_can_accept_incident

### 14. `get_soft_stamina_floor` global — L334
**SEVERITY: NONE**
NONE — exhaustive switch with a sane `default` fallback.
- **Connects to:** team_can_accept_incident, recovery_manager.c

### 15. `allocate_support_team` static — L352
**SEVERITY: NONE**
NONE — `support`/`best_slot` NULL/sentinel-checked before use; `best_slot` cast from `uint8_t` to `int8_t` is safe since `NUM_INCIDENTS`(20) fits in int8_t range.
- **Connects to:** incident_needs_support, cbba_sync_mission_execution_twin; called by allocate_all_incidents

## core/cbba.c

### 16. `cbba_team_is_local` global — L76
**SEVERITY: NONE**
NONE — thin equality wrapper, no indexing.
- **Connects to:** comms_transport_board_id_for_team; called by cbba_try_best_relocate, cbba_broadcast_local_teams, cbba_ingest_remote_mesh_replies, cbba_run

### 17. `cbba_find_incident_slot_by_id` static — L89
**SEVERITY: NONE**
NONE — bounded loop over `NUM_INCIDENTS`, returns `NO_ASSIGNMENT` sentinel on miss, matching callers' checks.
- **Connects to:** cbba_try_best_relocate, cbba_apply_override_pin

### 18. `cbba_resolve_task_location` static — L103
**SEVERITY: NONE**
NONE — NULL-checks all three pointer params, checks `task->slot < NUM_INCIDENTS`/`NUM_SYSTEM_TASKS` before indexing either array, plus `slot_occupied` before trusting location fields.
- **Connects to:** cbba_sync_team_execution_twin

### 19. `cbba_sync_team_execution_twin` static — L132
**SEVERITY: NONE**
NONE — writes `twin->planned_path[i]` only for `i < agent->path_size`, which is invariantly `<= CBBA_MAX_BUNDLE_SIZE` (same size as `planned_path[]`); `expected_stamina` clamped to `[MIN_STAMINA, MAX_STAMINA]`.
- **Connects to:** cbba_resolve_task_location, find_path_cost, team_travel_stamina_cost; called by cbba_sync_mission_execution_twin, cbba_build_bundle

### 20. `cbba_sync_mission_execution_twin` global — L206
**SEVERITY: NONE**
NONE — bounded loop over `NUM_TEAMS`.
- **Connects to:** cbba_sync_team_execution_twin; called by allocator.c, cbba_build_bundle, cbba_improve_routes, cbba_consensus, cbba_run

### 21. `cbba_task_equal` static — L212
**SEVERITY: NONE**
NONE — trivial field comparison.
- **Connects to:** cbba_path_contains

### 22. `cbba_path_contains` static — L217
**SEVERITY: NONE**
NONE — bounded by `agent->path_size` which is always `<= CBBA_MAX_BUNDLE_SIZE`.
- **Connects to:** cbba_task_equal; called by cbba_try_best_relocate

### 23. `cbba_team_can_execute_task` static — L227
**SEVERITY: NONE**
NONE — checks `task.pool == TASK_POOL_INCIDENT` and `task.slot < NUM_INCIDENTS` before indexing `g_incidents[task.slot]`.
- **Connects to:** team_can_accept_incident, team_has_minimum_equipment; called by cbba_try_best_relocate

### 24. `cbba_reverse_path_segment` static — L243
**SEVERITY: NONE**
NONE — standard two-pointer in-place reverse, only called with `first`/`last` already bounded by `agent->path_size`.
- **Connects to:** cbba_two_opt_agent

### 25. `cbba_two_opt_agent` static — L255
**SEVERITY: NONE**
NONE — early-returns for `path_size < 3`; `trial[CBBA_MAX_BUNDLE_SIZE]` fully sized for max `path_size`; `max_iter` bound guarantees loop termination.
- **Connects to:** calculate_route_utility, cbba_reverse_path_segment; called by cbba_improve_routes

### 26. `cbba_set_global_winner` static — L297
**SEVERITY: NONE**
NONE — bounded loop over `NUM_TEAMS`; `incident_slot` is the caller's already-validated task slot.
- **Connects to:** called by cbba_try_best_relocate

### 27. `cbba_try_best_relocate` static — L303
**SEVERITY: NONE**
NONE — `donor_trial`/`receiver_trial` arrays sized `CBBA_MAX_BUNDLE_SIZE`; outer guard `receiver->path_size >= CBBA_COMMITMENT_LIMIT` (== `CBBA_MAX_BUNDLE_SIZE`) already skips receivers at capacity, so trial arrays/final shift-insert never exceed declared size.
- **Connects to:** cbba_team_is_local, cbba_path_contains, cbba_team_can_execute_task, calculate_route_utility, cbba_set_global_winner; called by cbba_improve_routes

### 28. `cbba_improve_routes` global — L426
**SEVERITY: NONE**
NONE — `max_relocate` bound guarantees the `while` loop terminates even if `cbba_try_best_relocate` kept finding improvements.
- **Connects to:** cbba_two_opt_agent, cbba_try_best_relocate, cbba_sync_mission_execution_twin; called by cbba_run

### 29. `cbba_get_two_opt_improvements` global — L441
**SEVERITY: NONE**
NONE — trivial getter.
- **Connects to:** g_cbba_two_opt_improvements

### 30. `cbba_get_relocate_improvements` global — L446
**SEVERITY: NONE**
NONE — trivial getter.
- **Connects to:** g_cbba_relocate_improvements

### 31. `cbba_reset_improvement_counters` global — L451
**SEVERITY: NONE**
NONE — trivial resetter of two `uint32_t` globals.
- **Connects to:** g_cbba_two_opt_improvements, g_cbba_relocate_improvements

### 32. `cbba_bundle_contains` static — L457
**SEVERITY: NONE**
NONE — bounded by `bundle_size` which is always `<= CBBA_COMMITMENT_LIMIT`.
- **Connects to:** called by cbba_apply_override_pin, cbba_build_bundle

### 33. `cbba_init` global — L467
**SEVERITY: NONE**
NONE — resets every per-team array over its full declared size before any bidding round runs.
- **Connects to:** called by cbba_run

### 34. `cbba_apply_override_pin` static — L538
**SEVERITY: MEDIUM**
The overflow guard `if(agent->path_size >= CBBA_MAX_BUNDLE_SIZE || cbba_bundle_contains(...)) return;` protects BOTH the `agent->bundle[agent->bundle_size]` write and the `agent->path[j]` shift/write, but only checks `path_size` against `CBBA_MAX_BUNDLE_SIZE` — never `bundle_size` against `CBBA_COMMITMENT_LIMIT` directly. Memory-safe today only because `CBBA_MAX_BUNDLE_SIZE == CBBA_COMMITMENT_LIMIT == 3`; if either constant is changed independently, or `path_size` diverges from `bundle_size` (e.g. via `cbba_release_lost_tasks`'s system-task-preserving recompute), this becomes a real out-of-bounds write to `agent->bundle[]`/`bundle_bid[]`. A secondary consequence: if `bundle_size` increments without a matching path insert, `cbba_bundle_contains()` will believe the pinned task is already handled and never re-attempt the path insertion, silently orphaning the pin.
- **Connects to:** cbba_find_incident_slot_by_id, cbba_bundle_contains; called by cbba_build_bundle

### 35. `cbba_build_bundle` global — L584
**SEVERITY: LOW**
The `agent->path[j]` shift-and-insert is separately guarded by `path_size < CBBA_MAX_BUNDLE_SIZE`, so the path array itself is safe; however `agent->bundle_size` and `agent->winner[best_task.slot]`/`winning_bid[]` are updated unconditionally regardless of whether the path guard actually succeeded, so `bundle_size` can advance past what `path_size` reflects — the same divergence scenario noted in `cbba_apply_override_pin`, a latent state-consistency issue between `bundle` and `path` (not itself a memory-safety bug here since `bundle[]` is separately bounded).
- **Connects to:** cbba_apply_override_pin, cbba_bundle_contains, team_can_accept_incident, team_has_minimum_equipment, calculate_marginal_bundle_bid, candidate_claim_is_better, cbba_sync_team_execution_twin; called by cbba_run
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — same underlying issue as top-10 item #9's deliberately-unfixed "orphaned pin" behavior; tracked there.

### 36. `cbba_release_lost_tasks` static — L658
**SEVERITY: NONE**
NONE — `lost_index`/`id` are only read inside the `if(!winner)` block, reachable there only after being set together with `winner=false` in the preceding loop, so no uninitialized read; `task.slot` is captured before being overwritten with `NO_ASSIGNMENT`.
- **Connects to:** called by cbba_consensus

### 37. `cbba_shuffle_pairs` static — L740
**SEVERITY: NONE**
NONE — standard Fisher–Yates from `count` down to `2`, correct and bounded.
- **Connects to:** random_range_stream; called by cbba_consensus

### 38. `cbba_broadcast` static — L768
**SEVERITY: NONE**
NONE — copies fixed-size arrays over their full declared sizes into a matching-size `out` struct.
- **Connects to:** called by cbba_consensus, cbba_broadcast_local_teams

### 39. `cbba_receive_and_update` static — L791
**SEVERITY: NONE**
NONE — implements the CBBA consensus action table faithfully; `m = z_k` only dereferenced as an index (`timestamp[m]`) after `z_k == NO_ASSIGNMENT` has already been ruled out, so `m` is always valid; only mutates `receiver`'s own struct, never the sender's.
- **Connects to:** candidate_claim_is_better; called by cbba_consensus, cbba_ingest_remote_mesh_replies

### 40. `cbba_communication_link_up` static — L873
**SEVERITY: NONE**
NONE — `elapsed % cadence` with `cadence = severity + 1` so division-by-zero can't occur regardless of `severity`.
- **Connects to:** called by cbba_consensus

### 41. `cbba_now_ms` weak global — L914
**SEVERITY: NONE**
NONE (as documented) — process-local monotonic counter for the laptop/desktop weak default only; wraps at `UINT32_MAX` but that's far away at the 10ms step, and the real hardware override is expected to replace this entirely.
- **Connects to:** called by cbba_ingest_remote_mesh_replies

### 42. `cbba_broadcast_local_teams` global — L928
**SEVERITY: NONE**
NONE — `num_boards` ceiling-division matches `comms_transport_board_id_for_team()`'s own rule; `representative_team_id` stays a valid `uint8_t`.
- **Connects to:** cbba_team_is_local, cbba_broadcast, comms_transport_send

### 43. `cbba_ingest_remote_mesh_replies` global — L986
**SEVERITY: LOW → re-rated during triage, see below**
The `while((cbba_now_ms() - round_start) < CBBA_ROUND_TIMEOUT_MS)` loop busy-polls with no yield/sleep between iterations when the inbox is empty; on real hardware with a genuine wall-clock `cbba_now_ms()` override this becomes a tight CPU-spinning busy-wait for up to 530ms every tick, expensive on an embedded target expected to also service other tasks. Not a correctness bug (behavior is as documented) but a real efficiency/scheduling concern.
- **Connects to:** cbba_now_ms, comms_transport_peek_frame_type, comms_transport_poll_received, cbba_team_is_local, cbba_receive_and_update; called by cbba_run
   **Re-assessed, then RESOLVED — commit `4f081e9`.** During the LOW-severity triage pass this was pulled out of LOW: confirmed `cbba_run()` (and this function) runs **every tick**, unconditionally, via `allocate_all_incidents()` — not only when a reply is actually outstanding — and the loop had no early exit on success, so it burned up to the full 530ms every single tick regardless. `BOARD_TICK_DELAY_MS=0` (no pacing) means there was nothing to absorb that cost. Fixed by tallying ingested CBBA messages against the number of non-local teams (traced `cbba_broadcast_local_teams()`'s exact message shape first — one message per locally-hosted team, not one per board) and exiting early once that count is met; the 530ms timeout remains the correct fallback for a board that doesn't respond in full. Verified no regression via `--sweep-priority1`.

### 44. `cbba_consensus` global — L1073
**SEVERITY: NONE**
NONE — `outbox[NUM_TEAMS]`/`link_up[NUM_TEAMS]`/`pairs[CBBA_NUM_CONSENSUS_PAIRS]` sized to exactly match their fill loops; `pairs[p]/NUM_TEAMS` and `%NUM_TEAMS` correctly decode `(i,k)` back out of the packed index.
- **Connects to:** cbba_broadcast, cbba_communication_link_up, cbba_shuffle_pairs, cbba_receive_and_update, cbba_release_lost_tasks, cbba_sync_mission_execution_twin; called by cbba_run

### 45. `cbba_run` global — L1116
**SEVERITY: NONE**
NONE — orchestration; round count is a fixed bounded loop; correctly brackets `path_cache_begin()`/`path_cache_end()` per that API's documented invariant.
- **Connects to:** path_cache_begin, cbba_init, cbba_team_is_local, cbba_build_bundle, cbba_consensus, cbba_broadcast_local_teams, cbba_ingest_remote_mesh_replies, cbba_improve_routes, cbba_sync_mission_execution_twin, path_cache_end

## core/chacha20.c

### 46. `chacha20_rotl32` static — L13
**SEVERITY: LOW**
Computes `(x << n) | (x >> (32 - n))`; if ever called with `n == 0`, shifting by 32 is undefined behavior in C. Not currently triggerable (every call site uses fixed literals 16, 12, 8, 7), but the helper itself has no guard — a latent landmine for any future caller/refactor passing a variable rotation of 0 or ≥32.
- **Connects to:** chacha20_quarter_round
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — theoretical UB, unreachable given fixed-literal call sites.

### 47. `chacha20_quarter_round` static — L20
**SEVERITY: NONE**
NONE — faithful RFC 8439 §2.1 quarter-round, operates purely on the four pointed-to words.
- **Connects to:** chacha20_rotl32; called by chacha20_block

### 48. `chacha20_load_le32` static — L28
**SEVERITY: NONE**
NONE — reads exactly 4 bytes; all call sites pass pointers proven to have at least 4 bytes remaining.
- **Connects to:** called by chacha20_block

### 49. `chacha20_store_le32` static — L36
**SEVERITY: NONE**
NONE — writes exactly 4 bytes; called only with offsets that stay within the 64-byte `out` buffer.
- **Connects to:** called by chacha20_block

### 50. `chacha20_block` global — L44
**SEVERITY: NONE**
NONE — fixed 16-word arrays fully initialized before use; 10-iteration/8-quarter-round structure matches RFC 8439's reference implementation; output loop writes exactly `CHACHA20_BLOCK_BYTES`.
- **Connects to:** chacha20_load_le32, chacha20_quarter_round, chacha20_store_le32; called by chacha20_xor, aead_derive_poly1305_key

### 51. `chacha20_xor` global — L97
**SEVERITY: NONE**
NONE — `chunk` correctly clamped to `min(len-offset, CHACHA20_BLOCK_BYTES)`; loop terminates via strictly increasing `offset`; `counter` wraparound out of scope per documented caller-bounded usage (max 144-byte frames, nowhere near 2^32).
- **Connects to:** chacha20_block; called by aead_chacha20poly1305_seal, aead_chacha20poly1305_open

## core/comms_transport.c

### 52. `lora_spi_transmit` weak global — L30
**SEVERITY: NONE**
NONE — documented stub, unconditionally returns success.
- **Connects to:** called by comms_transport_dispatch_pending

### 53. `lora_radio_tx_busy` weak global — L57
**SEVERITY: NONE**
NONE — documented stub, unconditionally reports "not busy".
- **Connects to:** called by comms_transport_dispatch_pending

### 54. `comms_transport_advance_channel_busy` static — L132
**SEVERITY: NONE**
NONE — `cost_ticks` always `>= 1` regardless of `wire_len`; `start_tick` takes the max of current occupancy and `g_current_tick` so sends serialize rather than overlap. Only called from task-context sites in this file, no ISR/task race found here.
- **Connects to:** called by comms_transport_send, comms_transport_dispatch_pending

### 55. `comms_transport_channel_busy_until_tick` global — L142
**SEVERITY: NONE**
NONE — trivial read-only accessor.
- **Connects to:** g_channel_busy_until_tick

### 56. `comms_transport_inbox_push` static — L176
**SEVERITY: HIGH**
`g_inbox_head`/`g_inbox_tail`/`g_inbox_count` are plain (non-`volatile`) file-scope globals, mutated here via a multi-step, non-atomic sequence. This function is called both from `comms_transport_send()` (task context) AND, via `comms_transport_deliver_received()`, from what this codebase's own header explicitly documents as a real SPI/IRQ RX handler (interrupt context on real hardware). Meanwhile `comms_transport_poll_received()`/`comms_transport_peek_frame_type()` (task context) concurrently read/decrement the same three globals with no critical section, disabled-interrupt window, or lock anywhere in this file. If an RX interrupt fires mid-poll, the ring-buffer bookkeeping can desynchronize — lost frames, stale data returned as "new", or the queue permanently reporting itself full/empty when it isn't.
- **Connects to:** called by comms_transport_send, comms_transport_deliver_received

### 57. `comms_transport_tx_queues_init` static — L271
**SEVERITY: NONE**
NONE — wires each of the 4 fixed-size backing arrays to its matching queue slot and resets head/tail/count; called once from `comms_transport_init()` before any activity, no concurrency concern.
- **Connects to:** called by comms_transport_init

### 58. `comms_transport_tx_enqueue` static — L290
**SEVERITY: NONE**
NONE — does not itself re-check `payload_len` against capacity, but its only caller (`comms_transport_send`) already rejects oversized payloads before either branch runs; TX-side functions are only invoked from task/tick-loop context in this codebase, so the ISR/task race affecting the RX inbox does not apply here.
- **Connects to:** called by comms_transport_send

### 59. `comms_transport_tx_dequeue_highest` static — L325
**SEVERITY: NONE**
NONE — strict priority scan correctly bounded over the 4 defined tiers; only pops when `q->count > 0`.
- **Connects to:** called by comms_transport_dispatch_pending

### 60. `comms_transport_init` global — L351
**SEVERITY: NONE**
NONE — resets inbox indices/count and TX queues, re-seeds `g_channel_busy_until_tick` from live `g_current_tick`, matching the documented "safe to call mid-run" contract.
- **Connects to:** comms_transport_tx_queues_init

### 61. `comms_transport_board_id_for_team` global — L377
**SEVERITY: NONE**
NONE — single integer division, no bounds issue.
- **Connects to:** called by cbba_team_is_local, comms_transport_send

### 62. `comms_transport_board_count` global — L400
**SEVERITY: NONE**
NONE — correct ceiling-division idiom, no overflow risk given small compile-time constants.
- **Connects to:** (external callers per header: mission_link_task.c, mission_link_handshake.c)

### 63. `comms_transport_send` global — L406
**SEVERITY: NONE**
NONE — validates `payload_len > LORA_FRAME_MAX_PAYLOAD` up front for both paths; checks `lora_frame_pack()`'s and `lora_frame_try_parse()`'s return values before pushing to the inbox. (See #56 for the shared-inbox race this function's success path feeds into.)
- **Connects to:** comms_transport_board_id_for_team, lora_frame_pack, comms_transport_advance_channel_busy, lora_frame_try_parse, comms_transport_inbox_push, comms_transport_tx_enqueue

### 64. `comms_transport_dispatch_pending` global — L495
**SEVERITY: NONE**
NONE — checks `lora_radio_tx_busy()` before doing anything; bails cleanly if every TX tier is empty; ignores `lora_frame_pack()`/`lora_spi_transmit()` failures by explicit documented policy (drop, no NACK), not by omission.
- **Connects to:** lora_radio_tx_busy, comms_transport_tx_dequeue_highest, lora_frame_pack, comms_transport_advance_channel_busy, lora_spi_transmit

### 65. `comms_transport_poll_received` global — L544
**SEVERITY: HIGH**
Same shared-state hazard as #56: reads and non-atomically decrements `g_inbox_count`/advances `g_inbox_tail` (neither `volatile`) with no interrupt-disable/critical-section guard, while `comms_transport_deliver_received()` (documented RX-ISR entry point) can mutate the same variables concurrently from interrupt context. A race can cause a payload to be read from a slot simultaneously overwritten by a concurrent ISR push, or an entry consumed twice / never consumed.
- **Connects to:** called by cbba_ingest_remote_mesh_replies and (per header) mission_link's RX task

### 66. `comms_transport_peek_frame_type` global — L574
**SEVERITY: HIGH**
Reads `g_inbox_count` and `g_inbox[g_inbox_tail].frame_type` without any synchronization against the same RX-ISR-driven `comms_transport_inbox_push()` path — the peeked `frame_type` could correspond to a slot a concurrent ISR is overwriting, or the emptiness check could be stale by the time the caller acts on it.
- **Connects to:** called by cbba_ingest_remote_mesh_replies

### 67. `comms_transport_deliver_received` global — L582
**SEVERITY: HIGH**
The function the header explicitly documents as being called from a real SPI/IRQ RX handler on real hardware. Calls `comms_transport_inbox_push()` with zero interrupt-safety measures (no critical section, no `volatile` on the shared inbox state) — this is the actual ISR-side half of the race documented under #56/#65/#66. The function itself correctly bounds-checks `payload_len > LORA_FRAME_MAX_PAYLOAD` before delegating.
- **Connects to:** comms_transport_inbox_push; called by (per header) a real SPI/IRQ RX handler, not yet implemented in this tree


## core/environment.c

### 68. `environment_get_district_center` global — L78
**SEVERITY: NONE**
NONE — validates `district_id < NUM_DISTRICT_TYPES` and non-NULL `x`/`y` before reading `g_districts[district_id]`.
- **Connects to:** called by mission/incident code outside this file; reads `g_districts` populated by `assign_anchor`.

### 69. `clamp_u8` static — L95
**SEVERITY: NONE**
NONE — `value` is `int`, compared against `min`/`max` before narrowing to `uint8_t`; no overflow since comparisons happen in `int` domain.
- **Connects to:** used pervasively throughout the file (dozens of call sites).

### 70. `clamp_u16` static — L104
**SEVERITY: NONE**
NONE — same pattern as `clamp_u8`, casts `min`/`max` to `int` for comparison before clamping.
- **Connects to:** `stage_generate_terrain`, `find_best_land_cell`.

### 71. `abs_i` static — L113
**SEVERITY: LOW**
`value < 0 ? -value : value` on `INT_MIN` is undefined behavior (overflow), but every caller passes small grid-coordinate differences (max ~72), so this is unreachable in practice.
- **Connects to:** called by `stage_generate_terrain`, `find_best_land_cell`, `ensure_unique_anchor`, `connection_cost`, `carve_path_between`, `carve_repair_to_nearest_road`, `earthquake_damage_at`.
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — theoretical UB, unreachable given small grid-coordinate deltas.

### 72. `deterministic_hash` static — L118
**SEVERITY: NONE**
NONE — pure integer mixing function, all operations well-defined on `uint32_t` (unsigned wraparound is intentional here).
- **Connects to:** `deterministic_percent_roll`, `coord_hash`.

### 73. `deterministic_percent_roll` static — L136
**SEVERITY: NONE**
NONE — `% 100u` on an unsigned hash result, truncation to `uint8_t` is safe since result range is [0,99].
- **Connects to:** `deterministic_hash`; called by `propagate_fire`, `escalate_gas_leaks`, `apply_aftershocks`.

### 74. `coord_hash` static — L141
**SEVERITY: NONE**
NONE — adds 4096 to `x`/`y` before casting to `uint32_t`; callers only ever pass grid coordinates in `[-few, GRID_WIDTH/HEIGHT+few]`, so the result stays positive and the cast is safe.
- **Connects to:** `deterministic_hash`; called throughout `stage_generate_*` functions.

### 75. `smoothstepf` static — L146
**SEVERITY: NONE**
NONE — simple float polynomial, no edge case.
- **Connects to:** `value_noise`.

### 76. `lerpf` static — L151
**SEVERITY: NONE**
NONE — trivial linear interpolation.
- **Connects to:** `value_noise`.

### 77. `hash_to_unit` static — L156
**SEVERITY: NONE**
NONE — masks to 16 bits before dividing by 65535.0f, bounded to [0,1].
- **Connects to:** `value_noise`.

### 78. `value_noise` static — L161
**SEVERITY: NONE**
NONE — reads 4 lattice corners via `coord_hash`, interpolates; no bounds issue since `coord_hash`/`deterministic_hash` don't index arrays.
- **Connects to:** `coord_hash`, `smoothstepf`, `lerpf`, `hash_to_unit`; called by `fractal_noise`.

### 79. `fractal_noise` static — L178
**SEVERITY: NONE**
NONE — `weight` accumulates positive `amplitude` values each iteration so `weight <= 0.0f` guard is effectively dead but harmless; loop bound `octaves` is caller-controlled small constant (4 or 3).
- **Connects to:** `value_noise`; called by `stage_generate_terrain`.

### 80. `in_bounds` static — L199
**SEVERITY: NONE**
NONE — correct half-open range check `x >= 0 && x < GRID_WIDTH` etc.
- **Connects to:** called extremely widely throughout the file.

### 81. `is_water_cell` static — L204
**SEVERITY: NONE**
NONE — checks `in_bounds` first, then `flood` flag before falling back to the navigable/road/infra heuristic; matches the documented intent in the comment.
- **Connects to:** `in_bounds`; called by many stage functions.

### 82. `has_road` static — L223
**SEVERITY: NONE**
NONE — `in_bounds` short-circuits before indexing `g_environment`.
- **Connects to:** `in_bounds`; called by `road_degree`, `lot_adjacent_to_road`, `road_reachable_map`, `carve_repair_to_nearest_road`.

### 83. `district_base_safety` static — L228
**SEVERITY: NONE**
NONE — plain switch with default case, no indexing.
- **Connects to:** called by `stage_generate_districts`.

### 84. `district_risk` static — L246
**SEVERITY: NONE**
NONE — same pattern.
- **Connects to:** called by `place_building_rect`, `building_incident_score` (via `building->risk_level` set from this).

### 85. `district_seismic_vulnerability` static — L264
**SEVERITY: NONE**
NONE — same pattern.
- **Connects to:** `assign_anchor`, `place_building_rect`, `earthquake_damage_at`.

### 86. `building_quality_for_district` static — L282
**SEVERITY: NONE**
NONE — same pattern.
- **Connects to:** `assign_anchor`, `place_building_rect`.

### 87. `district_road_density` static — L300
**SEVERITY: NONE**
NONE — same pattern.
- **Connects to:** `assign_anchor`.

### 88. `district_building_density` static — L318
**SEVERITY: NONE**
NONE — same pattern.
- **Connects to:** `assign_anchor`, `stage_generate_buildings`, `place_building_rect` (indirectly via `district_id`/`cell->district_density`).

### 89. `district_population_density` static — L336
**SEVERITY: NONE**
NONE — same pattern.
- **Connects to:** `assign_anchor`, `place_building_rect`.

### 90. `district_infrastructure_density` static — L354
**SEVERITY: LOW**
NONE for the function itself, but note it is computed into `DistrictRecord_t.critical_infrastructure_density` in `assign_anchor` and that field is never read anywhere else in the file — dead data, not a correctness bug.
- **Connects to:** `assign_anchor`.
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — dead data, not a correctness bug.

### 91. `road_travel_cost` static — L372
**SEVERITY: NONE**
NONE — plain switch, default covers `ROAD_NONE` and any unexpected value.
- **Connects to:** called by `paint_road_cell`.

### 92. `road_capacity` static — L384
**SEVERITY: NONE**
NONE — same pattern.
- **Connects to:** called by `paint_road_cell`.

### 93. `road_quality` static — L396
**SEVERITY: NONE**
NONE — same pattern.
- **Connects to:** called by `apply_road_damage`.

### 94. `clear_pending_hazard_events` static — L408
**SEVERITY: NONE**
NONE — fixed-size loop over `MAX_ENV_PENDING_HAZARDS`, zero-initializes each slot.
- **Connects to:** called by `reset_generation_state`.

### 95. `queue_hazard_event` static — L414
**SEVERITY: LOW**
Silent data loss: if all `MAX_ENV_PENDING_HAZARDS` (16) slots are occupied, the function returns 0 and the caller (e.g. `propagate_fire`) simply does not increment its count — the hazard state on the building/cell is still updated, but the consumer-facing pending-event queue silently drops the notification with no logging/assert. This is a legitimate design tradeoff (bounded queue) but worth flagging as silent loss under hazard storms.
- **Connects to:** called by `propagate_fire`, `escalate_gas_leaks`, `apply_aftershocks`; consumed by `environment_pop_pending_hazard_event`.
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — explicit, acknowledged bounded-queue tradeoff.

### 96. `reset_generation_state` static — L431
**SEVERITY: NONE**
NONE — `memset`s all generation-state globals then re-initializes per-cell sentinel fields (`ENV_NO_BUILDING`, `ENV_NO_ROAD`, `ENV_NO_NAV_NODE`) and default navigable/cost/safety values in one pass; loop bounds match array declarations (`GRID_WIDTH`/`GRID_HEIGHT`, `MAX_ENV_BUILDINGS`).
- **Connects to:** `clear_pending_hazard_events`; called by `generate_environment`.

### 97. `distance_to_coast` static — L463
**SEVERITY: NONE**
NONE — four-way switch on `g_coast_orientation` (set to `random_range_stream(...,0,3)`), all branches covered including default.
- **Connects to:** called by `stage_generate_terrain`, `find_best_land_cell`, `stage_generate_districts`.

### 98. `mark_water_cell` static — L474
**SEVERITY: NONE**
NONE — bounds-checked via `in_bounds` before writing `g_environment[x][y]`.
- **Connects to:** `in_bounds`, `clamp_u8`; called by `stage_generate_terrain`.

### 99. `stage_generate_terrain` static — L494
**SEVERITY: NONE**
NONE — `g_river_center[x]` is set for every `x` (branch only skips the *update* for `x==0`, not the initial assignment); all array writes bounded by the `for(x<GRID_WIDTH)`/`for(y<GRID_HEIGHT)` loops matching declared dimensions. `river_y` narrowing to `int16_t` is safe at current `GRID_HEIGHT` (36).
- **Connects to:** `random_range_stream`, `clamp_u16`, `fractal_noise`, `distance_to_coast`, `abs_i`, `clamp_u8`, `mark_water_cell`, `in_bounds`; called by `generate_environment`.

### 100. `assign_anchor` static — L585
**SEVERITY: HIGH**
No bounds check on `g_environment_summary.district_total` before indexing `g_districts[g_environment_summary.district_total]`. `g_districts` is sized `[NUM_DISTRICT_TYPES]` (10). `stage_generate_districts` calls `assign_anchor` exactly 10 times in a fixed sequence so it never overflows today, but the function performs an unchecked write-then-increment with no assertion — any future caller invoking it an 11th time (or a code change reordering/duplicating a call) silently corrupts adjacent static memory. This is a landmine, not a currently-triggered bug.
- **Connects to:** `district_population_density`, `district_building_density`, `district_road_density`, `building_quality_for_district`, `district_seismic_vulnerability`, `district_infrastructure_density`; called by `stage_generate_districts`.

### 101. `find_best_land_cell` static — L602
**SEVERITY: NONE**
NONE — iterates `min_x..max_x`/`min_y..max_y` guarded by `in_bounds` inside the loop; `best` is pre-seeded with a clamped midpoint so a return value is always in bounds even if no valid cell is found.
- **Connects to:** `in_bounds`, `is_water_cell`, `distance_to_coast`, `abs_i`, `clamp_u16`; called by `stage_generate_districts`.

### 102. `ensure_unique_anchor` static — L634
**SEVERITY: LOW**
The outer `radius` loop (up to 72) re-scans the entire `(2*radius+1)²` box every iteration instead of only the new ring — an O(radius³) performance smell, not a correctness bug, and only called 9 times at world-gen time (not a hot ISR/task path). If no unique probe is ever found, it silently falls back to returning the original `candidate` unmodified, permitting a duplicate/overlapping anchor without any signal.
- **Connects to:** `in_bounds`, `is_water_cell`, `abs_i`; called by `stage_generate_districts`.
   **RESOLVED** — commit `d044a24`. Added `g_diag_duplicate_anchor_fallback` diagnostic counter; no better fallback exists (every reachable cell was already tried).

### 103. `stage_generate_districts` static — L668
**SEVERITY: NONE**
NONE — `anchors[]` sized `NUM_DISTRICT_TYPES`, indices 0–9 statically fixed and in bounds; `district_count[...]--`/`++` pairs are balanced.
- **Connects to:** `find_best_land_cell`, `ensure_unique_anchor`, `assign_anchor`, `is_water_cell`, `abs_i`, `distance_to_coast`, `in_bounds`; called by `generate_environment`.

### 104. `paint_road_cell` static — L820
**SEVERITY: NONE**
NONE — `in_bounds` check first; `road_class < cell->road_class` early-return prevents downgrading an existing stronger road; water-cell guard requires `bridge` flag to paint over water.
- **Connects to:** `in_bounds`, `is_water_cell`, `road_travel_cost`, `road_capacity`, `clamp_u8`; called by `paint_road_width`, `stage_generate_roads`, `ensure_hospital_redundancy`.

### 105. `paint_road_width` static — L847
**SEVERITY: NONE**
NONE — `paint_road_cell` internally bounds-checks each `(px,py)`, so out-of-range offsets from `width` are safely dropped rather than indexing OOB.
- **Connects to:** `is_water_cell`, `paint_road_cell`; called by `carve_path_between`.

### 106. `next_road_id` static — L858
**SEVERITY: LOW**
`static uint16_t road_id` pre-incremented on every call with no wraparound guard; at 65535 calls it would wrap and collide with earlier-issued IDs. Current world-generation issues at most a few hundred IDs per run, so unreachable today, but there is no overflow guard.
- **Connects to:** called by `stage_generate_roads`, `carve_path_between`, `ensure_hospital_redundancy`, `carve_repair_to_nearest_road`.
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — wraparound unreachable at current road-ID volumes.

### 107. `connection_cost` static — L864
**SEVERITY: NONE**
NONE — `in_bounds` guard returns `UINT16_MAX` sentinel before dereferencing `g_environment[x][y]`; `uint16_t cost` arithmetic stays far under overflow.
- **Connects to:** `in_bounds`, `is_water_cell`, `abs_i`; called by `carve_path_between`.

### 108. `carve_path_between` static — L889
**SEVERITY: MEDIUM**
The backtrace loop `while(!(cx==start.x && cy==start.y))` has no iteration cap and no cycle-detection while walking `nodes[cx][cy].parent_x/y`; this is safe today because parent pointers only ever form a tree rooted at `start` given the forward search's invariants, but the lack of a defensive bound in a function walking data-derived parent pointers is a latent risk if that invariant is ever broken by a future edit (e.g. a bug in the forward search that lets a node be revisited/re-parented). Also O(N³) worst-case full-grid rescans per step — a performance concern, not a memory-safety one. No actual out-of-bounds access found; every index used is bounds-checked via `in_bounds` except `start`/`end`, which are always caller-supplied in-range coordinates.
- **Connects to:** `connection_cost`, `paint_road_width`, `next_road_id`, `abs_i`, `in_bounds`; called by `stage_generate_roads`, `carve_repair_to_nearest_road`.
   **RESOLVED** — commit `5a6538a`. Bounded the backtrace loop at `GRID_WIDTH*GRID_HEIGHT` steps.

### 109. `stage_generate_roads` static — L983
**SEVERITY: NONE**
NONE — `anchors[]` sized `NUM_DISTRICT_TYPES`, populated only for `i < district_total` (always 10); all fixed anchor indices used are valid.
- **Connects to:** `carve_path_between`, `coord_hash`, `paint_road_cell`, `next_road_id`, `is_water_cell`; called by `generate_environment`.

### 110. `road_degree` static — L1044
**SEVERITY: NONE**
NONE — delegates bounds checking to `has_road`, which calls `in_bounds` first.
- **Connects to:** `has_road`; called by `stage_generate_infrastructure`, `stage_validate_and_build_navigation`.

### 111. `lot_adjacent_to_road` static — L1059
**SEVERITY: NONE**
NONE — delegates bounds checking to `has_road`; output pointers NULL-checked before write.
- **Connects to:** `has_road`; called by `can_place_building_rect`, `place_building_rect`, `ensure_hospital_redundancy`, `stage_validate_and_build_navigation`.

### 112. `can_place_building_rect` static — L1086
**SEVERITY: NONE**
NONE — explicit `x<=0 || y<=0 || x+w>=GRID_WIDTH || y+h>=GRID_HEIGHT` bound check before the nested loop indexes `g_environment[cx][cy]`, keeping every index strictly inside `[1,GRID_WIDTH-2]`/`[1,GRID_HEIGHT-2]`.
- **Connects to:** `is_water_cell`, `lot_adjacent_to_road`; called by `stage_generate_buildings`.

### 113. `pick_building_type` static — L1113
**SEVERITY: NONE**
NONE — plain switch with default, `area` only used in comparisons.
- **Connects to:** called by `place_building_rect`.

### 114. `place_building_rect` static — L1130
**SEVERITY: NONE**
NONE — guarded at entry by `building_count >= MAX_ENV_BUILDINGS` before indexing `g_environment_buildings[id]`; `footprint = (uint8_t)area` where `area <= 16` from all call sites, fits safely.
- **Connects to:** `lot_adjacent_to_road`, `pick_building_type`, `district_population_density`, `district_risk`, `building_quality_for_district`, `coord_hash`, `district_seismic_vulnerability`, `clamp_u8`; called by `stage_generate_buildings`.

### 115. `stage_generate_buildings` static — L1225
**SEVERITY: NONE**
NONE — outer loop bounds leave room for the largest possible footprint to still satisfy `can_place_building_rect`'s own edge check; `building_count >= MAX_ENV_BUILDINGS` checked every outer iteration to bail early.
- **Connects to:** `is_water_cell`, `coord_hash`, `district_building_density`, `can_place_building_rect`, `place_building_rect`; called by `generate_environment`.

### 116. `mark_safe_zone_candidate` static — L1280
**SEVERITY: NONE**
NONE — `in_bounds` guard before write; count incremented only once per cell via a flag check.
- **Connects to:** `in_bounds`; called by `stage_generate_infrastructure`.

### 117. `stage_generate_infrastructure` static — L1310
**SEVERITY: MEDIUM**
Per the function's own trailing comment, empirically ~47/50 seeds leave `g_gateway_sites[1..2]` invalid because usable non-water border candidates are scarce — a documented, acknowledged placement-policy gap. Marking MEDIUM because the consequence (invalid/degenerate gateways feeding `environment_find_gateway_site`) is real and downstream-visible even though this function's own diagnostic bookkeeping is internally correct.
- **Connects to:** `road_degree`, `mark_safe_zone_candidate`, `coord_hash`, `is_water_cell`, `road_reachable_map`, `has_road`, `lot_reachable`, `carve_repair_to_nearest_road`; called by `generate_environment`.
   **CORRECTED, not a live finding — the "~47/50" figure was stale.** The trailing comment this finding cited (and which the original audit pass trusted at face value) described the candidate-selection loop's *old* logic, which required `has_road()` on a border cell before accepting it as a gateway candidate. That requirement was already removed by the time of this audit — the current loop (immediately above the stale comment, its own text explicit: "No has_road() requirement here... Any non-water border cell is now a candidate") accepts any non-water border cell and repairs connectivity afterward via `carve_repair_to_nearest_road()`. The stale comment measuring "~47/50" was simply never deleted after that fix landed, and was mistakenly repeated as current-state fact across several turns of a later investigation before being caught. Verified directly against the running code via `--sweep-priority1` across 550 seeds in 3 disjoint ranges: **0 degenerate worlds, 0 `carve_repair_to_nearest_road` failures.** The stale comment has been deleted and replaced with one citing this measurement. No fix needed — this item is closed as a documentation defect, not a code defect.

### 118. `road_reachable_map` static — L1468
**SEVERITY: MEDIUM**
Declares `GridPoint_t queue[GRID_WIDTH * GRID_HEIGHT]` (~5.2 KB) as a **stack** local, unlike sibling arrays (`g_carve_nodes`/`g_carve_visited`) that were explicitly promoted to BSS elsewhere in this file specifically to eliminate a large stack frame (per that code's own comment). This function is called repeatedly from `stage_generate_infrastructure`, `stage_validate_and_build_navigation`, and `environment_validate_hazard_reachability`; a ~5 KB single-call stack frame on an embedded target with limited task stack budgets risks overflow if the calling task's stack wasn't sized with this frame in mind — the same class of bug already fixed once for `carve_path_between`. No out-of-bounds indexing found otherwise.
- **Connects to:** `has_road`, `in_bounds`; called by `stage_generate_infrastructure`, `stage_validate_and_build_navigation`, `environment_validate_hazard_reachability`.
   **RESOLVED** — commit `5a6538a`. BFS queue promoted from stack to file-scope static (`g_road_reachable_queue`), same treatment as `g_carve_nodes`/`g_carve_visited` in this file.

### 119. `lot_reachable` static — L1564
**SEVERITY: NONE**
NONE — `in_bounds` checked before indexing `reachable[nx][ny]`.
- **Connects to:** `in_bounds`; called by `stage_generate_infrastructure`, `stage_validate_and_build_navigation`, `environment_validate_hazard_reachability`.

### 120. `carve_repair_to_nearest_road` static — L1586
**SEVERITY: LOW**
If no road cell exists anywhere on the grid, `best` stays equal to `start` and `carve_path_between(start,start,...)` produces a degenerate single-cell "path" that silently "succeeds" rather than signaling that no reachable road network was found to connect to. Essentially unreachable given `stage_generate_roads` always carves the arterial chain first, but there is no defensive distinction between "found and repaired" and "nothing to connect to."
- **Connects to:** `has_road`, `abs_i`, `carve_path_between`; called by `stage_generate_infrastructure`, `stage_validate_and_build_navigation`, `environment_validate_hazard_reachability`.
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — degenerate case prevented by `stage_generate_roads()`'s own ordering.

### 121. `ensure_hospital_redundancy` static — L1614
**SEVERITY: LOW**
`building->access_count = 2;` is set unconditionally at the end regardless of whether the `+x`/`+y` guarded `paint_road_cell` calls actually executed (both are skipped if the hospital sits near the grid's far edge) — a silent-success bug where the metadata claims redundant access the building does not actually have, which could mislead any consumer trusting `access_count >= 2` to mean "reachable via 2 independent routes."
- **Connects to:** `paint_road_cell`, `next_road_id`; called by `stage_validate_and_build_navigation`.
   **RESOLVED** — commit `ddb46cf` (batch). `access_count` now reflects roads actually painted instead of a hardcoded `2`.

### 122. `stage_validate_and_build_navigation` static — L1634
**SEVERITY: NONE**
NONE — `nav_node_count`/`nav_edge_count` both checked against `MAX_ENV_NAV_NODES`/`MAX_ENV_NAV_EDGES` before writing, so excess candidates are dropped rather than overflowing the fixed arrays.
- **Connects to:** `road_reachable_map`, `lot_reachable`, `carve_repair_to_nearest_road`, `ensure_hospital_redundancy`, `road_degree`, `lot_adjacent_to_road`, `in_bounds`; called by `generate_environment`.

### 123. `generate_environment` global — L1755
**SEVERITY: NONE**
NONE — straightforward single-threaded orchestration; `generated=1` set only after all stages complete.
- **Connects to:** `random_seed_stream`, `derive_seed`, `reset_generation_state`, `stage_generate_terrain`, `stage_generate_districts`, `stage_generate_roads`, `stage_generate_buildings`, `stage_generate_infrastructure`, `stage_validate_and_build_navigation`.

### 124. `earthquake_damage_at` static — L1780
**SEVERITY: NONE**
NONE — arithmetic done in `int` before final `clamp_u8`, so no unsigned wraparound even though intermediate `raw_damage` can go negative; no `in_bounds` check on `x,y` but every caller passes already-validated in-bounds coordinates.
- **Connects to:** `abs_i`, `district_seismic_vulnerability`, `coord_hash`, `clamp_u8`; called by `apply_building_damage`, `apply_road_damage`, `apply_infrastructure_damage`.

### 125. `apply_building_damage` static — L1800
**SEVERITY: LOW**
Reconstructs the footprint window from `loc_x`/`loc_y`/`footprint_width`/`footprint_height` rather than reusing the original placement rectangle; mitigated by a `building_id` mismatch check per cell (mismatches are skipped, not flagged), but this coupling is fragile against future changes to how `loc_x`/footprint are computed in `place_building_rect`.
- **Connects to:** `earthquake_damage_at`, `clamp_u8`, `in_bounds`; called by `environment_apply_earthquake`.
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — fragile but currently mitigated by an existing mismatch check.

### 126. `apply_road_damage` static — L1844
**SEVERITY: NONE**
NONE — full-grid loop, every index in bounds; `blocked_road_count`/`navigable_cell_count` reset to 0 and only incremented in a fresh recomputation, no underflow risk.
- **Connects to:** `earthquake_damage_at`, `road_quality`, `clamp_u8`; called by `environment_apply_earthquake`.

### 127. `apply_infrastructure_damage` static — L1891
**SEVERITY: NONE**
NONE — same recompute-from-scratch pattern as `apply_road_damage`.
- **Connects to:** `earthquake_damage_at`, `clamp_u8`; called by `environment_apply_earthquake`.

### 128. `environment_apply_earthquake` global — L1931
**SEVERITY: NONE**
NONE — orchestration only, single-threaded world-gen/event path.
- **Connects to:** `random_seed_stream`, `derive_seed`, `apply_building_damage`, `apply_road_damage`, `apply_infrastructure_damage`.

### 129. `environment_validate_hazard_reachability` global — L1963
**SEVERITY: NONE**
NONE — loops bounded by `g_num_safe_zones`/`NUM_INCIDENTS`; inherits `road_reachable_map`'s stack-usage concern (flagged separately) but introduces no new indexing bug.
- **Connects to:** `road_reachable_map`, `lot_reachable`, `carve_repair_to_nearest_road`; called from mission/simulation code outside this file.

### 130. `environment_cell_is_navigable` global — L1992
**SEVERITY: NONE**
NONE — `in_bounds` checked first; safe default returned when the world hasn't been generated yet, avoiding uninitialized reads.
- **Connects to:** `in_bounds`; called by pathing/mission code outside this file.

### 131. `environment_cell_is_flood` global — L2001
**SEVERITY: NONE**
NONE — same guard pattern.
- **Connects to:** `in_bounds`.

### 132. `environment_cell_travel_cost` global — L2010
**SEVERITY: NONE**
NONE — same guard pattern.
- **Connects to:** `in_bounds`.

### 133. `environment_cell_base_safety` global — L2019
**SEVERITY: NONE**
NONE — same guard pattern.
- **Connects to:** `in_bounds`.

### 134. `environment_set_cell_operational_state` global — L2028
**SEVERITY: MEDIUM**
Performs a non-atomic, unprotected read-modify-write on the shared globals `g_environment_summary.blocked_road_count`/`navigable_cell_count` (increment/decrement based on comparing old vs. new cell state), plus a plain (non-`volatile`) write into the shared `g_environment` grid. Neither `g_environment` nor `g_environment_summary` is `volatile`, and there is no lock/critical-section guard anywhere in this function. If this public entry point is ever invoked from both task and ISR/fault-injection-callback context (plausible for a "set operational state" API reacting to async fault events), a race on the read-then-increment/decrement can desynchronize the counters from actual grid state.
- **Connects to:** `in_bounds`; called from mission/incident/fault-injection code outside this file.
   **RESOLVED (re-scoped, not a live race)** — commit `5a6538a`. Confirmed every caller runs from the single simulation task; documented the invariant.

### 135. `environment_register_safe_zone` global — L2053
**SEVERITY: NONE**
NONE — `in_bounds` checked before writing.
- **Connects to:** `in_bounds`; called by mission/safe-zone setup code outside this file.

### 136. `environment_register_gateway` global — L2065
**SEVERITY: NONE**
NONE — `gateway_id % NUM_EVACUATION_GATEWAYS` guarantees the index is always in bounds regardless of the caller-supplied id.
- **Connects to:** `in_bounds`; called by mission/gateway setup code outside this file.

### 137. `environment_find_safe_zone_site` global — L2080
**SEVERITY: NONE**
NONE — `total == 0` guarded to avoid divide-by-zero.
- **Connects to:** called by search-zone/mission-assignment code outside this file.

### 138. `environment_find_gateway_site` global — L2108
**SEVERITY: NONE**
NONE — `index % NUM_EVACUATION_GATEWAYS` bounds the access regardless of input.
- **Connects to:** called by evacuation/gateway logic outside this file.

### 139. `building_can_spawn_incident` static — L2123
**SEVERITY: NONE**
NONE — NULL and sentinel (`ENV_NO_BUILDING`) checked before reading `building->damage_state`; switch has `default: return 0`.
- **Connects to:** called by `select_damaged_building_source`, `select_damaged_building_source_in_zone`.

### 140. `cell_can_spawn_incident` static — L2167
**SEVERITY: NONE**
NONE — NULL-checked before dereference; default case returns 0.
- **Connects to:** called by `select_damaged_cell_source`, `select_damaged_cell_source_in_zone`.

### 141. `building_incident_score` static — L2204
**SEVERITY: LOW**
No NULL check of its own (unlike sibling `building_can_spawn_incident`), but both call sites only invoke this after `building_can_spawn_incident` has already returned true (which itself NULL-checks first), so `building` is guaranteed non-NULL today; would be unsafe if reused elsewhere without that precondition.
- **Connects to:** called by `select_damaged_building_source`, `select_damaged_building_source_in_zone`.
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — both call sites already guarantee non-NULL.

### 142. `cell_incident_score` static — L2212
**SEVERITY: NONE**
NONE — no NULL check but both call sites only invoke after `cell_can_spawn_incident` returned true using the same non-NULL pointer.
- **Connects to:** called by `select_damaged_cell_source`, `select_damaged_cell_source_in_zone`.

### 143. `mark_building_incident_hazard` static — L2228
**SEVERITY: NONE**
NONE — NULL-checked; consistent with the design pattern of clearing a pending "collapse risk" flag once it resolves into a concrete spawned incident.
- **Connects to:** called by `select_damaged_building_source`, `select_damaged_building_source_in_zone`.

### 144. `mark_cell_incident_hazard` static — L2251
**SEVERITY: NONE**
NONE — same pattern/reasoning as `mark_building_incident_hazard`.
- **Connects to:** called by `select_damaged_cell_source`, `select_damaged_cell_source_in_zone`.

### 145. `select_damaged_building_source` static — L2274
**SEVERITY: NONE**
NONE — `best_id` initialized to sentinel `ENV_NO_BUILDING`; `found`/sentinel double-checked before dereferencing `g_environment_buildings[best_id]`.
- **Connects to:** `building_can_spawn_incident`, `building_incident_score`, `random_range_stream`, `mark_building_incident_hazard`; called by `environment_find_incident_site`.

### 146. `select_damaged_cell_source` static — L2309
**SEVERITY: NONE**
NONE — `best_x`/`best_y` initialized to `-1`; `found` flag gates the write, so the `-1` sentinel is never used as an actual index.
- **Connects to:** `cell_can_spawn_incident`, `cell_incident_score`, `random_range_stream`, `mark_cell_incident_hazard`; called by `environment_find_incident_site`.

### 147. `select_damaged_building_source_in_zone` static — L2348
**SEVERITY: NONE**
NONE — identical guards to `select_damaged_building_source`, additionally filtered by district.
- **Connects to:** `building_can_spawn_incident`, `building_incident_score`, `random_range_stream`, `mark_building_incident_hazard`; called by `environment_find_incident_site_in_zone`.

### 148. `select_damaged_cell_source_in_zone` static — L2386
**SEVERITY: NONE**
NONE — identical guards to `select_damaged_cell_source`, additionally filtered by district.
- **Connects to:** `cell_can_spawn_incident`, `cell_incident_score`, `random_range_stream`, `mark_cell_incident_hazard`; called by `environment_find_incident_site_in_zone`.

### 149. `building_at` static — L2428
**SEVERITY: NONE**
NONE — `in_bounds` checked first; `building_id == ENV_NO_BUILDING || building_id >= building_count` both checked before indexing.
- **Connects to:** `in_bounds`; called by `propagate_fire`.

### 150. `propagate_fire` static — L2441
**SEVERITY: LOW**
The `if(target->building_id < MAX_ENV_BUILDINGS)` guard before `ignite[target->building_id] = 1` is redundant-but-safe (IDs are always `< building_count <= MAX_ENV_BUILDINGS` by construction), so no actual OOB write — defensive rather than buggy. Two-pass ignite-then-apply correctly limits fire to one hop per tick.
- **Connects to:** `building_at`, `deterministic_percent_roll`, `queue_hazard_event`; called by `environment_update_hazard_propagation`.
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — guard is redundant but genuinely safe.

### 151. `escalate_gas_leaks` static — L2506
**SEVERITY: NONE**
NONE — loop bounded by `building_count`; all writes go through already-validated pointers.
- **Connects to:** `deterministic_percent_roll`, `clamp_u8`, `queue_hazard_event`; called by `environment_update_hazard_propagation`.

### 152. `apply_aftershocks` static — L2559
**SEVERITY: LOW**
Performs a cross-module, unguarded read-modify-write increment on `g_search_zones[building->district_id].aftershock_hits_since_swept` (a field owned by `search_zones.c`). The bounds check (`< NUM_SEARCH_ZONES`) is correct, but there is no visible synchronization; if that counter is also touched by a Search-team task concurrently, this is an unprotected shared-counter race. Exploitability depends on `search_zones.c`'s threading model (outside this file), but the direct cross-module field mutation itself is a coupling/consistency risk.
- **Connects to:** `deterministic_percent_roll`, `clamp_u8`, `queue_hazard_event`, external `g_search_zones`; called by `environment_update_hazard_propagation`.
   **RESOLVED (re-scoped, not a live race)** — commit `d044a24`. Confirmed both board targets run exactly one simulation task; documented the invariant rather than adding a lock nothing needs yet.

### 153. `degrade_blocked_roads` static — L2611
**SEVERITY: NONE**
NONE — full-grid bounded loop; `clamp_u8` prevents `travel_cost` overflow; counters only incremented from a fresh scan.
- **Connects to:** `clamp_u8`; called by `environment_update_hazard_propagation`.

### 154. `environment_update_hazard_propagation` global — L2638
**SEVERITY: NONE**
NONE — simple gate-then-dispatch, no direct state mutation of its own.
- **Connects to:** `propagate_fire`, `escalate_gas_leaks`, `apply_aftershocks`, `degrade_blocked_roads`; called from the main simulation tick loop outside this file.

### 155. `environment_pop_pending_hazard_event` global — L2649
**SEVERITY: NONE**
NONE — NULL-checked output pointers; bounded loop; clears the popped slot before returning to prevent re-delivery.
- **Connects to:** consumes `g_pending_hazard_events` written by `queue_hazard_event`; called from simulation/incident dispatch code outside this file.

### 156. `environment_find_incident_site` global — L2669
**SEVERITY: NONE**
NONE — NULL-checked output pointers; switch has explicit `default: return 0`.
- **Connects to:** `select_damaged_building_source`, `select_damaged_cell_source`; called by incident-spawning code outside this file.

### 157. `environment_find_incident_site_in_zone` global — L2701
**SEVERITY: NONE**
NONE — same guard pattern as `environment_find_incident_site`, scoped variant.
- **Connects to:** `select_damaged_building_source_in_zone`, `select_damaged_cell_source_in_zone`; called by search-zone sweep logic outside this file.

### 158. `environment_district_name` global — L2725
**SEVERITY: NONE**
NONE — plain switch returning string literals, `default` covers any unrecognized value.
- **Connects to:** called by logging/diagnostic code outside this file.

### 159. `environment_road_name` global — L2743
**SEVERITY: NONE**
NONE — same pattern, `default` covers `ROAD_NONE` and any unrecognized value.
- **Connects to:** called by logging/diagnostic code outside this file.


## core/fault_buffer.c

### 160. `fault_buffer_init` [global] — L65
**SEVERITY: NONE**
Clamps `num_boards` to `(0, FAULT_BUFFER_NUM_BOARDS]` and defaults `teams_per_board` to a safe nonzero value; zeroing loop uses the compile-time `FAULT_BUFFER_NUM_BOARDS`, so no OOB write even if a bogus runtime value were passed.
- **Connects to:** fault_comms_init

### 161. `board_mark_heard` [static] — L86
**SEVERITY: NONE**
Bounds-checks `board_id` against the compile-time array size before indexing; "only advance" logic correctly special-cases the `FAULT_BUFFER_BOARD_NEVER_HEARD` sentinel so a first heard-tick of 0 is not rejected.
- **Connects to:** fault_buffer_record_local_evidence, fault_buffer_record_remote_evidence, fault_buffer_record_remote_vote, fault_buffer_record_local_vote

### 162. `board_is_live` [static] — L98
**SEVERITY: NONE**
Indexes `g_board_last_heard_tick[board_id]` with no internal bounds check, but its only call site (`fault_buffer_fuse`) always passes `board < g_num_boards <= FAULT_BUFFER_NUM_BOARDS`; underflow-avoidance for `now < last_heard` handled explicitly.
- **Connects to:** fault_buffer_fuse

### 163. `teams_on_board` [static] — L117
**SEVERITY: NONE**
Correctly returns 0 for a board past `NUM_TEAMS`, and correctly returns the remainder for a partial last board.
- **Connects to:** fault_buffer_fuse

### 164. `fault_buffer_record_local_evidence` [global] — L130
**SEVERITY: NONE**
Validates `team_id` and `evidence != NULL`; ring index math correct modulo `FAULT_BUFFER_LOCAL_DEPTH_TICKS`; `count` clamped rather than incremented unbounded.
- **Connects to:** board_mark_heard; called by fault_comms_broadcast_local_teams

### 165. `fault_buffer_get_local_sample` [global] — L169
**SEVERITY: NONE**
Returns a zeroed/`known=0` sample for an unknown team or out-of-range id; "most recent" index computation correctly wraps backward from `head`. No current callers found (dead but harmless public API).
- **Connects to:** (no in-repo caller found)

### 166. `fault_buffer_record_remote_evidence` [global] — L188
**SEVERITY: NONE**
Same pattern as the local-evidence recorder: validates inputs, correct ring math, marks board liveness.
- **Connects to:** board_mark_heard; called by fault_comms_ingest_remote_frames

### 167. `fault_buffer_get_remote_evidence_sample` [global] — L214
**SEVERITY: NONE**
Same correctness pattern as function 6. No current callers found.
- **Connects to:** (no in-repo caller found)

### 168. `fault_buffer_record_remote_vote` [global] — L233
**SEVERITY: HIGH**
`weight` and `class_mask` come directly off the wire (`FaultVotePayload_t`, deserialized in `fault_comms_ingest_remote_frames`) with no validation. The accumulator `g_vote_weight[target_team_id][fault_class] += weight` is only clamped on the *upper* side (`> (float)NUM_TEAMS`); a negative or NaN `weight` from a corrupt/malicious/misbehaving frame drives the tally arbitrarily negative (or NaN) with no lower-bound clamp, silently corrupting `fault_buffer_fuse()`'s `best_ratio`/`best_class` computation for that team with no crash or rejected-frame log. Only `payload_len` is validated by the caller — the actual field values are not.
- **Connects to:** board_mark_heard; called by fault_comms_ingest_remote_frames; feeds fault_buffer_fuse

### 169. `fault_buffer_record_local_vote` [global] — L276
**SEVERITY: MEDIUM**
Unlike its remote counterpart, this function applies **no cap at all** (upper or lower) to `g_vote_weight[target_team_id][fault_class] += weight` — the header-documented invariant ("Cap at NUM_TEAMS so a replayed or duplicate frame can't inflate weight beyond what the real voter count allows") is silently absent from this sibling entry point. Latent under the current single call site (once per target per round); any future caller (or a bug invoking this more than once per round for the same target) inflates a team's vote weight past what `fault_buffer_fuse()`'s quorum math assumes, with no defense.
- **Connects to:** board_mark_heard; called by fault_comms_cast_vote
   **RESOLVED** — commit `5a6538a`. Added the same upper-bound cap its sibling `fault_buffer_record_remote_vote()` already has.

### 170. `fault_buffer_round_reset` [global] — L307
**SEVERITY: NONE**
Simple `memset` of the vote-weight table only; correctly leaves evidence rings and liveness tracking untouched per its documented contract.
- **Connects to:** called by fault_comms_ingest_remote_frames

### 171. `fault_buffer_get_vote_weight` [global] — L312
**SEVERITY: NONE**
Bounds-checks both `target_team_id` and `fault_class` before indexing, returns 0.0 otherwise. No current callers found.
- **Connects to:** (no in-repo caller found)

### 172. `fault_buffer_fuse` [global] — L320
**SEVERITY: NONE**
Bounds-checks `target_team_id`; quorum is provably non-zero whenever fusion isn't skipped (`n_live_teams >= FAULT_BUFFER_MIN_LIVE_VOTERS=3` guarantees `quorum >= 3`), so the division cannot divide by zero; tie-break to lowest class index correctly implemented. Output quality is only as good as the (potentially corrupted, see finding 9) `g_vote_weight` it reads — a defect in the writer, not this reader.
- **Connects to:** board_is_live, teams_on_board; called by recovery_manager.c's evaluate_fault_predictor

## core/fault_comms.c

### 173. `fault_comms_init` [global] — L80
**SEVERITY: NONE**
Correctly derives `teams_per_board` by counting board-0-owned teams via the public `comms_transport_board_id_for_team()`, matching the documented workaround for not having a getter in comms_transport.c.
- **Connects to:** fault_buffer_init, comms_transport_board_count, comms_transport_board_id_for_team

### 174. `fault_comms_locally_hosted_team_count` [static] — L103
**SEVERITY: NONE**
Straightforward count loop bounded by `NUM_TEAMS`; delegates hosting determination to `cbba_team_is_local`.
- **Connects to:** cbba_team_is_local; called by fault_comms_cast_vote

### 175. `fault_comms_cast_vote` [static] — L161
**SEVERITY: NONE**
Same-board branch correctly excludes the target team from its own weight (`count - 1.0f`) and bails if that would be `<= 0`; cross-board branch correctly does not subtract 1. `class_mask`/threshold logic internally consistent with `fault_predictor_run`'s currently-inert all-zero output.
- **Connects to:** fault_predictor_run, comms_transport_board_id_for_team, fault_buffer_record_local_vote, comms_transport_send, comms_transport_board_count; called by fault_comms_broadcast_local_teams, fault_comms_ingest_remote_frames

### 176. `fault_comms_broadcast_local_teams` [global] — L225
**SEVERITY: NONE**
Builds the 9-feature evidence vector in the exact slot layout the `_Static_assert`s pin, records it locally before sending so the ring and the wire payload are byte-identical, and correctly skips this board in the per-board send loop.
- **Connects to:** cbba_team_is_local, watchdog_get_evidence_vector, fault_buffer_record_local_evidence, fault_comms_cast_vote, comms_transport_board_count, comms_transport_send

### 177. `fault_comms_ingest_remote_frames` [global] — L288
**SEVERITY: HIGH**
The function's own header comment documents the mechanism precisely: `comms_transport_poll_received()` destructively pops from a shared per-board inbox, and this receiver's loop `break`s (not `continue`s) as soon as it peeks a frame type it doesn't own, leaving that frame for CBBA/mission_link — but if this receiver and `cbba_ingest_remote_mesh_replies()` are ever both driven in the same tick, whichever runs its drain first will pop-and-discard frames belonging to the other (its own `continue`-on-unrecognized-type already pops-then-drops). A genuine, currently-latent silent-data-loss hazard baked into this function's draining semantics that activates the moment both receivers share a real per-tick call site.
- **Connects to:** fault_buffer_round_reset, comms_transport_peek_frame_type, comms_transport_poll_received, fault_buffer_record_remote_evidence, fault_comms_cast_vote, fault_buffer_record_remote_vote, comms_transport_board_id_for_team

## core/fault_config.c

### 178. `set_duration_range` [static] — L7
**SEVERITY: NONE**
Trivial field-pair setter, no bounds/ordering issue (all call sites pass `min <= max` literals).
- **Connects to:** called by fault_config_apply_profile

### 179. `fault_config_reset` [global] — L16
**SEVERITY: NONE**
`memset` zeroes the struct then explicitly sets `profile = FAULT_PROFILE_NONE` (redundant with the zero value but harmless).
- **Connects to:** called by fault_config_apply_profile

### 180. `fault_config_apply_profile` [global] — L22
**SEVERITY: NONE**
Switch covers every `FaultProfile_t` value including `FAULT_PROFILE_NONE`/default; every case's array indices are named enumerators, no raw indices.
- **Connects to:** fault_config_reset, set_duration_range; called by fault_injector_set_profile

### 181. `fault_profile_from_string` [global] — L90
**SEVERITY: NONE**
NULL-checks `name` before `strcmp`; falls through to `FAULT_PROFILE_NONE` for any unrecognized string.
- **Connects to:** (external CLI/config callers)

### 182. `fault_profile_to_string` [global] — L109
**SEVERITY: NONE**
Switch covers all enum values with a default fallback to `"none"`.
- **Connects to:** (external CLI/config callers)

## core/fault_injector.c

### 183. `fault_duration_for_class` [static] — L14
**SEVERITY: NONE**
Handles `min==0 && max==0` "disabled" case, promotes a stray `min==0` to 1, clamps `max < min` up to `min` before calling `random_range_stream`, so the range passed is always valid.
- **Connects to:** random_range_stream; called by inject_mobility_fault, inject_communication_fault, inject_resource_fault, inject_environment_fault, inject_mission_fault

### 184. `fault_severity_for_class` [static] — L31
**SEVERITY: NONE**
Promotes a configured `0` severity to `1` so no fault class can have a zero-effect injection.
- **Connects to:** called by inject_mobility_fault, inject_communication_fault, inject_resource_fault, inject_environment_fault, inject_mission_fault

### 185. `clear_fault_summary` [static] — L41
**SEVERITY: LOW**
Resets every per-class field the comment claims to reset except `fault_state->target_incident_id[fault_class]`, which is left stale after a mission-fault clears. Currently harmless because `inject_mission_fault` unconditionally overwrites that field before it's ever read again, and no other file reads it — but an incomplete "reset point" relative to the function's own stated contract, and a latent trap for a future consumer.
- **Connects to:** called by recover_environment_fault, recover_mission_fault, recover_fault
   **RESOLVED** — commit `ddb46cf` (batch). `target_incident_id[fault_class]` now reset.

### 186. `mark_last_fault` [static] — L73
**SEVERITY: NONE**
Straightforward field copy from per-class arrays into `last_fault_*` summary fields; no bounds issue since `fault_class` is always a valid enumerator at every call site.
- **Connects to:** called by inject_mobility_fault, inject_communication_fault, inject_resource_fault, inject_environment_fault, inject_mission_fault, recover_environment_fault, recover_mission_fault, recover_fault

### 187. `inject_mobility_fault` [static] — L82
**SEVERITY: NONE**
Re-entrancy guard (`if active return`) prevents resetting an in-progress fault's duration; fields set consistently with mask set last, before `mark_last_fault`.
- **Connects to:** fault_duration_for_class, fault_severity_for_class, mark_last_fault, snapshot_record; called by inject_fault

### 188. `inject_communication_fault` [static] — L103
**SEVERITY: NONE**
Identical structure/guard to `inject_mobility_fault`.
- **Connects to:** fault_duration_for_class, fault_severity_for_class, mark_last_fault, snapshot_record; called by inject_fault

### 189. `inject_resource_fault` [static] — L124
**SEVERITY: NONE**
Identical structure/guard to `inject_mobility_fault`.
- **Connects to:** fault_duration_for_class, fault_severity_for_class, mark_last_fault, snapshot_record; called by inject_fault

### 190. `inject_environment_fault` [static] — L145
**SEVERITY: NONE**
Correctly guards `assigned_task_slot < NUM_INCIDENTS` before indexing `g_incidents`; ref-counted cell-cost bookkeeping correctly distinguishes "first injector on this cell" from "piling on"; `uint8_t` overflow guard on `new_cost` correctly catches wraparound. `next_x`/`next_y` default to the team's own always-valid location when no path step is found.
- **Connects to:** find_next_path_step, environment_set_cell_operational_state, fault_duration_for_class, fault_severity_for_class, mark_last_fault, snapshot_record; called by inject_fault

### 191. `inject_mission_fault` [static] — L228
**SEVERITY: NONE**
Only ever targets the team's own assigned incident-pool task; `mission_fault_active_count` correctly gates capturing the "original" priority/victim_count only on the first concurrent fault; `victim_count` clamped post-add, avoiding an off-by-one under-clamp.
- **Connects to:** fault_duration_for_class, fault_severity_for_class, mark_last_fault, snapshot_record; called by inject_fault

### 192. `recover_environment_fault` [static] — L295
**SEVERITY: NONE**
Ref-count decrement guarded against underflow; the "last owner" branch always restores to the immutable `env_fault_base_cost` regardless of intermediate arithmetic drift from other teams' partial peel-backs, so the terminal state is correct.
- **Connects to:** environment_set_cell_operational_state, mark_last_fault, snapshot_record, clear_fault_summary; called by recover_fault

### 193. `recover_mission_fault` [static] — L345
**SEVERITY: NONE**
Matches by `incident_id` rather than location/slot (avoiding stale-slot-reuse hazard); if the target incident can no longer be found, the loop safely breaks without corrupting an unrelated incident.
- **Connects to:** mark_last_fault, snapshot_record, clear_fault_summary; called by recover_fault

### 194. `recover_fault` [static] — L382
**SEVERITY: NONE**
Dispatch switch covers all five fault classes plus a no-op default.
- **Connects to:** recover_environment_fault, recover_mission_fault, mark_last_fault, snapshot_record, clear_fault_summary; called by fault_injector_step

### 195. `inject_fault` [static] — L410
**SEVERITY: NONE**
Dispatch switch covers all five fault classes plus a no-op default.
- **Connects to:** inject_mobility_fault, inject_communication_fault, inject_resource_fault, inject_environment_fault, inject_mission_fault; called by fault_injector_step

### 196. `roll_injection` [static] — L439
**SEVERITY: NONE**
`probability==0` short-circuits before drawing from the RNG stream; comparison direction correctly yields a `probability`-percent chance over a `[1,100]` draw.
- **Connects to:** random_range_stream; called by fault_injector_step

### 197. `fault_injector_init` [global] — L447
**SEVERITY: NONE**
Simple seed assignment and stream re-seed via `derive_seed`.
- **Connects to:** random_seed_stream, derive_seed

### 198. `fault_injector_set_profile` [global] — L453
**SEVERITY: NONE**
Thin wrapper delegating to `fault_config_apply_profile`.
- **Connects to:** fault_config_apply_profile

### 199. `fault_injector_get_profile` [global] — L458
**SEVERITY: NONE**
Trivial getter.
- **Connects to:** (reads g_fault_config.profile)

### 200. `fault_injector_step` [global] — L463
**SEVERITY: NONE**
The two-stage repair state machine is internally consistent: environment-class bypass correctly `continue`s before stage 1/2 so it can't also set `pending_repair`; stage 1 and stage 2 can't both fire in the same call for a non-zero `FAULT_REPAIR_CONFIRM_DELAY_TICKS`; immunity check on the inactive-fault path correctly gates re-injection.
- **Connects to:** recover_fault, roll_injection, inject_fault

## core/fault_predictor.c

### 201. `fault_predictor_run` [global] — L5
**SEVERITY: LOW**
With `FAULT_MODEL_LOADED==0` (current build state), `evidence` is never dereferenced, so a NULL `evidence` pointer from either call site is currently harmless. No NULL-check on `evidence` though — once real inference code lands in the `#if FAULT_MODEL_LOADED` branch (which the header says is coming), a NULL/short evidence buffer would be dereferenced with no defense.
- **Connects to:** called by fault_comms_cast_vote, recovery_manager.c's evaluate_fault_predictor
   **RESOLVED** — commit `ddb46cf` (batch). Added a NULL check on `evidence`.

## core/grid.c

### 202. `initialize_grid` [global] — L6
**SEVERITY: NONE**
Simple double loop over `GRID_WIDTH`×`GRID_HEIGHT`, bounds correct.
- **Connects to:** environment_cell_base_safety; called by world-generation call site

### 203. `degrade_grid_area` [static] — L18
**SEVERITY: NONE**
Explicitly clamps `x`/`y` to grid bounds before indexing, Manhattan-distance skip avoids over-applying degradation outside the diamond, denominator `radius+1` guarded against division-by-zero (always `>=1`), computes subtraction in `int` before clamping to `uint8_t`, exactly avoiding the wraparound the inline comment calls out.
- **Connects to:** called by apply_incident_grid_degradation

### 204. `apply_incident_grid_degradation` [global] — L43
**SEVERITY: NONE**
Early-returns when `radius==0 || degradation==0`, avoiding a wasted (harmless) call.
- **Connects to:** incident_degradation_radius, incident_base_degradation, degrade_grid_area


## core/incident.c

### 205. `allocate_incident_id` [global] — L206
**SEVERITY: NONE**
Plain post-increment of `g_incident_counter` (uint32_t). No callers from ISR context found anywhere in the repo, so the non-atomic read-modify-write is not a real cross-context hazard; wraps only after 4 billion calls.
- **Connects to:** called by search_generate_sweep_tasks, create_equipment_cache_incident, create_evacuee_offboard_incident, generate_incident_at, find_reclaimable_system_task_slot (indirectly).

### 206. `apply_incident_archetype` [static] — L303
**SEVERITY: LOW**
No bounds check on `type` before indexing `g_incident_archetypes[type]` (sized `NUM_INCIDENT_TYPES`). Every current call site passes an in-range value, so no live OOB read today, but no defensive guard for a future unvalidated caller.
- **Connects to:** called by search_generate_sweep_tasks, create_equipment_cache_incident, create_evacuee_offboard_incident, populate_generated_incident_model.
   **RESOLVED** — commit `ddb46cf` (batch). Added a `type < NUM_INCIDENT_TYPES` bounds check.

### 207. `search_generate_sweep_tasks` [global] — L337
**SEVERITY: NONE**
In-flight bitmask (`districts_with_task_in_flight`, uint16_t) against `NUM_SEARCH_ZONES` (=10) fits safely in 16 bits today. Slot exhaustion and frontier-pick failure both handled cleanly.
- **Connects to:** find_free_system_task_slot, allocate_incident_id, apply_incident_archetype, search_zone_pick_next_target, search_zone_pick_frontier_cell, environment_get_district_center, zone_id_for_location.

### 208. `sweep_radius_for_team` [static] — L452
**SEVERITY: LOW**
Local `IncidentState_t probe` is stack-allocated and only `probe.req_capability` is initialized before being passed by pointer to `capability_score()`. Currently safe only because `capability_score()` reads exactly `req_capability` and nothing else — if extended to read another field, this becomes an uninitialized-stack-memory read feeding a live radius calculation.
- **Connects to:** capability_score (scoring.c); called by search_sweep_step.
   **RESOLVED** — commit `ddb46cf` (batch). `probe` is now fully zero-initialized.

### 209. `search_sweep_step` [global] — L483
**SEVERITY: NONE**
Walked both passes. The documented "M37" cross-district misattribution is a known, deliberately-shelved design tradeoff, not a new finding. No unguarded array access; distance arithmetic uses `int32_t` so no overflow at this scale.
- **Connects to:** search_zone_check_cycle_completion, clear_task_slot, search_zone_pick_frontier_cell, search_zone_mark_cell_covered, sweep_radius_for_team, zone_id_for_location, capability_score (indirectly).

### 210. `begin_return_to_safe_zone` [global] — L650
**SEVERITY: NONE**
Looks up nearest safe zone, bails with `NO_ASSIGNMENT` if none reachable, otherwise sets team fields consistently.
- **Connects to:** find_nearest_safe_zone; called by release_team, abandon_task_for_recovery.

### 211. `create_equipment_cache_incident` [global] — L678
**SEVERITY: NONE**
Falls back from `find_free_system_task_slot()` to `find_reclaimable_system_task_slot()`, returns `NO_ASSIGNMENT` if both fail (checked at every call site). All task fields initialized before returning.
- **Connects to:** find_free_system_task_slot, find_reclaimable_system_task_slot, allocate_incident_id, apply_incident_archetype.

### 212. `incident_degradation_radius` [global] — L707
**SEVERITY: NONE**
Simple switch with `default: return 0`.
- **Connects to:** used by apply_incident_grid_degradation.

### 213. `incident_base_degradation` [global] — L720
**SEVERITY: NONE**
Same shape as #8; magic numbers are unnamed but that's a style note, not a correctness bug.
- **Connects to:** used by apply_incident_grid_degradation.

### 214. `is_valid_team_id` [static] — L733
**SEVERITY: NONE**
`team_id < NUM_TEAMS` — correct bound.
- **Connects to:** called by update_incident_work, release_team, release_main_task_team, release_all_task_teams, complete_task, clear_stale_equipment_cache_requests.

### 215. `clear_task_slot` [static] — L738
**SEVERITY: NONE**
NULL-checked, pool-validated before mutating. Every field in `IncidentState_t` reset to a sane inert value.
- **Connects to:** called by initialize_incidents, complete_task, find_reclaimable_system_task_slot, search_sweep_step.

### 216. `initialize_incidents` [global] — L773
**SEVERITY: NONE**
Resets all generation state and clears both pools via `clear_task_slot`.
- **Connects to:** clear_task_slot.

### 217. `complete_return_to_rest` [global] — L795
**SEVERITY: NONE**
NULL-checked; sets `status=RESTING`, clears `heading_to_rest`.
- **Connects to:** (caller is in team.c, outside this file).

### 218. `team_base_work_rate` [global] — L834
**SEVERITY: NONE**
`(equipment_level * base_range) / EQ_MAX_LEVEL` computed in `uint32_t` before narrowing — checked for overflow: intermediate product max 1785, well within `uint32_t`.
- **Connects to:** called by team_effective_work_rate, and externally by mission_execution_twin.c's nominal_work_rate().

### 219. `team_effective_work_rate` [global] — L842
**SEVERITY: NONE**
NULL-checked. Fault degradation uses a floor-guard before each subtraction, so no unsigned-underflow wraparound.
- **Connects to:** team_base_work_rate; called by incident_work_rate_for_team and mission_link_build_status().

### 220. `incident_work_rate_for_team` [static] — L890
**SEVERITY: NONE**
Delegates cleanly based on `cbba_team_is_local()`; no index validation needed since `team_id` is checked by callers before invocation.
- **Connects to:** cbba_team_is_local, team_effective_work_rate, random_range_stream; called by update_incident_work.

### 221. `update_incident_work` [static] — L898
**SEVERITY: NONE**
Guarded by status/type/team-validity/on-scene/equipment checks before touching `work_remaining`; subtraction only reached after confirming `work_remaining > work_rate`, so no unsigned underflow.
- **Connects to:** is_valid_team_id, incident_work_rate_for_team, complete_incident_stage; called by update_all_incident_work.

### 222. `update_all_incident_work` [global] — L923
**SEVERITY: NONE**
Iterates both pools guarded by `slot_occupied`; calls stale-cache cleanup first as documented.
- **Connects to:** clear_stale_equipment_cache_requests, update_incident_work.

### 223. `update_all_passive_effects` [global] — L944
**SEVERITY: NONE**
Only iterates `g_incidents` — correct, since every system-task archetype has `passive_effect = PASSIVE_NONE`. Timer decrement/expiry logic correctly ordered, can't underflow.
- **Connects to:** apply_incident_grid_degradation.

### 224. `dispatch_next_queued_incident` [static] — L979
**SEVERITY: NONE**
Loop bound safe at `CBBA_MAX_BUNDLE_SIZE=3` scale. Validates slot range, `slot_occupied`, id match, status, and ownership before dispatching. Full queue reset on exhaustion correct.
- **Connects to:** called by release_team.

### 225. `release_team` [static] — L1018
**SEVERITY: NONE**
Validates `team_id` first. Clears pool/slot before deciding next state — no stale `assigned_task_slot` left pointing at the just-vacated task.
- **Connects to:** is_valid_team_id, dispatch_next_queued_incident, team_needs_safe_zone, begin_return_to_safe_zone, team_set_available; called by release_main_task_team, release_all_task_teams, complete_task.

### 226. `release_main_task_team` [static] — L1041
**SEVERITY: NONE**
Pool-validated, NULL-checked. Diagnostic increment only fires for `TASK_POOL_INCIDENT` with a valid `team_id`.
- **Connects to:** release_team; called by complete_incident_stage.

### 227. `release_all_task_teams` [static] — L1061
**SEVERITY: NONE**
Guards against double-releasing the same team when `support_id == team_id`.
- **Connects to:** release_team; called by complete_task.

### 228. `complete_task` [static] — L1081
**SEVERITY: NONE**
Pool-validated, NULL-checked. `TASK_POOL_SYSTEM` branch correctly clears the slot before releasing teams; support/primary double-release guard mirrors #23.
- **Connects to:** snapshot_record_resolved, clear_task_slot, release_team, release_all_task_teams; called by complete_incident_stage, clear_stale_equipment_cache_requests.

### 229. `complete_incident_stage` [global] — L1129
**SEVERITY: MEDIUM**
In the `SPEC_SUPPORT` case, if `incident->incident_type == INCIDENT_EQUIPMENT_CACHE` but `spawned_by` is `NO_ASSIGNMENT` or `>= NUM_TEAMS`, the function does nothing at all: no `complete_task()`, no status change. Since `update_incident_work()` re-invokes this every tick once `work_remaining` hits 0, this would create a task permanently stuck `INC_IN_PROGRESS` with its team stuck `ON_SCENE` forever. Not reachable via the current sole creator `create_equipment_cache_incident()` (always sets `spawned_by = team->team_id`), but there is no fallback path to recover state if that invariant is ever violated.
- **Connects to:** random_range_stream, snapshot_record, release_main_task_team, complete_task, find_best_gateway, find_nearest_safe_zone, cbba_sync_mission_execution_twin.
   **RESOLVED** — commit `5a6538a`. The task is now always completed; only the equipment-refill credit is conditional on `spawned_by` being valid.

### 230. `incident_needs_support` [global] — L1337
**SEVERITY: NONE**
Trivial switch/default.
- **Connects to:** (caller is in cbba.c/allocator).

### 231. `get_min_stamina` [static] — L1347
**SEVERITY: NONE**
Switch covers all `FitnessTier_t` values with a sane default.
- **Connects to:** called by team_needs_safe_zone.

### 232. `team_needs_safe_zone` [static] — L1359
**SEVERITY: LOW**
Correct logic, but `30` (equipment threshold) is a bare magic number rather than a named constant — a maintainability gap, not a functional bug.
- **Connects to:** get_min_stamina; called by release_team.
   **RESOLVED** — commit `ddb46cf` (batch). `30` named as `TEAM_NEEDS_SAFE_ZONE_EQUIPMENT_THRESHOLD` (mission_state.h).

### 233. `find_nearest_safe_zone` [global] — L1372
**SEVERITY: NONE**
`min` initialized to `UINT16_MAX`; `find_path_cost()` returns `UINT16_MAX` for unreachable destinations, so an unreachable zone can never win; correctly returns `NO_SAFE_ZONE` if none reachable.
- **Connects to:** find_path_cost; called by begin_return_to_safe_zone, complete_incident_stage.

### 234. `initialize_evacuation_gateways` [global] — L1385
**SEVERITY: LOW**
The `while(!valid_position)` retry loop has no iteration cap. If the deterministic site pool were ever exhausted, this would spin forever at world-init. Not currently triggered by any known configuration, but no safety valve.
- **Connects to:** random_seed_stream, environment_find_gateway_site, random_range_stream, environment_register_gateway.
   **RESOLVED** — commit `85c0556`. Before fixing, confirmed road-connectivity for gateways is already guaranteed elsewhere (see item #117's correction above — the gateway-degeneracy premise this item's severity partly rested on was itself stale). Added a bounded random-retry count (`EVACUATION_GATEWAY_MAX_RANDOM_ATTEMPTS`=100) followed by a deterministic perimeter scan (`find_free_perimeter_cell()`) as a guaranteed-to-terminate fallback, rather than `generate_safe_zones()`'s existing fixed-`(0,0)`-with-no-dedup pattern (itself a still-open finding, not worth copying). Verified no behavioral regression via `--sweep-priority1`.

### 235. `create_evacuee_offboard_incident` [global] — L1464
**SEVERITY: NONE**
Bounds-checks `safezone_id`, dedups via `g_safezone_evacuation_active[]`, bails cleanly on slot exhaustion. All fields initialized before setting the active flag last.
- **Connects to:** find_free_system_task_slot, allocate_incident_id, apply_incident_archetype, random_range_stream; called by generate_evacuee_offboard_incidents.

### 236. `generate_evacuee_offboard_incidents` [global] — L1525
**SEVERITY: NONE**
Trivial loop over `g_num_safe_zones`, ignores per-zone return value by design.
- **Connects to:** create_evacuee_offboard_incident.

### 237. `initialize_evacuation_teams` [global] — L1533
**SEVERITY: NONE**
Just seeds a stream.
- **Connects to:** random_seed_stream, derive_seed.

### 238. `find_best_gateway` [static] — L1543
**SEVERITY: NONE**
`lowest_cost` initialized to `UINT16_MAX`, `path_cost` checked `>= 0` before comparing — correctly rejects unreachable gateways without misreading a negative as huge-but-valid.
- **Connects to:** find_next_path_step; called by complete_incident_stage.

### 239. `find_free_incident_slot` [static] — L1560
**SEVERITY: NONE**
Linear scan, sets `slot_occupied=1` before returning the pointer — correct for single-threaded/non-ISR call context.
- **Connects to:** called by generate_incident_at.

### 240. `find_free_system_task_slot` [static] — L1573
**SEVERITY: NONE**
Same shape as #35, no off-by-one.
- **Connects to:** called by search_generate_sweep_tasks, create_equipment_cache_incident, create_evacuee_offboard_incident.

### 241. `find_reclaimable_system_task_slot` [static] — L1596
**SEVERITY: NONE**
Only reclaims `INC_OPEN` `UNSEARCHED_SECTOR` slots, sets the district's cooldown before clearing, then re-marks `slot_occupied=1` after `clear_task_slot()` — ordering is correct.
- **Connects to:** zone_id_for_location, clear_task_slot; called by create_equipment_cache_incident.

### 242. `populate_generated_incident_model` [static] — L1631
**SEVERITY: NONE**
Every `IncidentType_t` reachable through `generate_incident()`'s weighted profile table gets a defined, sane victim/structure range — `default` is a deliberate generic fallback, not a gap.
- **Connects to:** apply_incident_archetype, random_range_stream; called by generate_incident_at.

### 243. `build_generation_context` [static] — L1680
**SEVERITY: LOW**
Bounds checks use `team->specialization < NUM_TEAMS` (10) instead of the more precise `NUM_TEAM_SPECIALIZATIONS` (5) before indexing `available_by_spec[]`/etc. Not an OOB write since those arrays are sized `[NUM_TEAMS]`, only an overly permissive guard. Also iterates the full grid every generation tick — a performance note, not a correctness bug.
- **Connects to:** g_teams, g_incidents, g_system_tasks, g_grid; called by generate_incidents_if_needed.
   **RESOLVED** — commit `ddb46cf` (batch). All three bounds checks now compare against `NUM_TEAM_SPECIALIZATIONS`, not `NUM_TEAMS`.

### 244. `calculate_generation_load` [static] — L1764
**SEVERITY: NONE**
Pure weighted-sum arithmetic in `uint32_t`, no overflow. Threshold ladder monotonic and complete.
- **Connects to:** called by generate_incidents_if_needed.

### 245. `calculate_generation_probability` [static] — L1799
**SEVERITY: NONE**
`hidden_permille` division guarded by `ctx->open_incidents > 0`. Clamp applied consistently, no division-by-zero, no overflow.
- **Connects to:** called by generate_incidents_if_needed.

### 246. `select_weighted_incident_type` [static] — L1850
**SEVERITY: NONE**
`weight` floored to `1` before accumulating, so `total_weight > 0`, making the subsequent random draw safe. Cumulative-sum loop has a correct fallback for rounding edge cases.
- **Connects to:** random_range_stream; called by generate_incidents_if_needed.

### 247. `schedule_next_generation_evaluation` [static] — L1903
**SEVERITY: NONE**
Complete switch with a `default` case sharing `GEN_LOAD_CRITICAL`'s interval.
- **Connects to:** random_range_stream; called by generate_incidents_if_needed.

### 248. `generate_incident_at` [static] — L1941
**SEVERITY: NONE**
NULL-checked slot allocation; every required field set before model population/degradation run.
- **Connects to:** find_free_incident_slot, allocate_incident_id, populate_generated_incident_model, apply_incident_grid_degradation; called by generate_incidents_if_needed, generate_incident.

### 249. `generate_incident` [static] — L1966
**SEVERITY: NONE**
Bails cleanly if `environment_find_incident_site()` fails.
- **Connects to:** environment_find_incident_site, generate_incident_at; called by generate_incidents_if_needed.

### 250. `escalate_incidents_by_zone_trust` [global] — L1994
**SEVERITY: MEDIUM**
Dead code with a documented, dangerous latent effect: internally correct, but not currently invoked from the live tick loop, and its precondition (`last_swept_tick` refreshed by a real sweep-cycle-completion writer) is not yet met anywhere. Per the function's own header comment, wiring this in today would push every open, discovered incident to permanent `PRIORITY_CRITICAL` within `ZONE_TRUST_DECAY_CAP_TICKS` of simulation start. No runtime impact today since nothing calls it.
- **Connects to:** zone_id_for_location, search_zone_trust; not currently called by anything live.
   **FLAGGED, not fixed — deliberately.** This is genuinely dead code (never called from the live tick loop) whose only "fix" would be wiring it in, which requires deciding how `last_swept_tick` gets refreshed by a real sweep-cycle-completion writer - a feature decision, not a bug fix, and out of scope for this pass. Left as documented dead code.

### 251. `escalate_starved_incidents` [global] — L2067
**SEVERITY: NONE**
`age` computed defensively against tick-counter regression. Only ever decrements `priority` by one enum step per call and only when already `> PRIORITY_CRITICAL`, so it cannot escalate below the numeric floor. Resets `reported_at_tick` on escalation.
- **Connects to:** called by simulation_step().

### 252. `generate_incidents_if_needed` [global] — L2091
**SEVERITY: MEDIUM**
`while(environment_pop_pending_hazard_event(...)) generate_incident_at(...)` discards `generate_incident_at()`'s return value. If the incident pool is full, the popped hazard event is silently dropped forever — already consumed from the pending queue, not requeued or logged — so a real hazard (fire/gas/collapse) can vanish with no incident ever created and no diagnostic trail.
- **Connects to:** environment_pop_pending_hazard_event, generate_incident_at, build_generation_context, calculate_generation_load, calculate_generation_probability, select_weighted_incident_type, generate_incident, schedule_next_generation_evaluation.
   **RESOLVED** — commit `5a6538a`. Added `g_diag_hazard_event_dropped_pool_full` diagnostic counter; the event itself can't be recovered once popped, but the loss is now visible.

### 253. `clear_stale_equipment_cache_requests` [static] — L2130
**SEVERITY: NONE**
Guards `spawned_by` via `is_valid_team_id()` before indexing `g_teams[]`. Completion conditions are reasonable and don't race against `complete_incident_stage()`'s own completion path.
- **Connects to:** is_valid_team_id, complete_task; called by update_all_incident_work.

### 254. `reopen_future_queued_incidents` [global] — L2156
**SEVERITY: NONE**
Reopens only future queued bundle entries after validating slot range, occupancy, id match, status, and ownership. Correctly re-seeds the queue with just the current in-flight entry only when genuinely still valid/owned.
- **Connects to:** g_teams, g_incidents; caller outside this file (likely a CBBA re-plan path).

### 255. `release_queued_incidents` [static] — L2225
**SEVERITY: NONE**
Same validation shape as #50, intentionally doesn't preserve a "current" entry since its caller already tears that down separately.
- **Connects to:** g_incidents; called by abandon_task_for_recovery.

### 256. `abandon_task_for_recovery` [global] — L2267
**SEVERITY: LOW**
Early-returns if `heading_to_rest`. If `task` resolves non-NULL but the team is neither `task->assigned_team` nor `task->support_team` (an invariant violation not asserted anywhere), the function silently skips both branches, leaving the task's bookkeeping untouched while still clearing the team's pool/slot and sending it to rest — no diagnostic, no defensive cleanup. Also, `task->unreachable_streak` is only read here, never incremented in this function — its increment must live entirely elsewhere, unverified.
- **Connects to:** begin_return_to_safe_zone, release_queued_incidents, team_set_available; reads g_incidents/g_system_tasks via team's assigned pool/slot.
   **RESOLVED (partially by finding it was already correct) — commit `9efa538`.** The `unreachable_streak` claim traced clean: it's genuinely incremented elsewhere (`core/simulation.c:320-321`'s `move_team()`, on a failed path attempt) and reset correctly on arrival/slot-clear — non-issue, no fix needed. The ownership-mismatch claim: traced every writer of `->assigned_team` in the codebase and confirmed the mismatch isn't reachable through any currently-known-correct assignment path (`assign_incident()` always sets a task's `assigned_team` and the owning team's `assigned_task_pool/slot` together, in one call) — it's a defensive check against a hypothetical bug elsewhere, the same class of stale-slot-reuse hazard `recover_mission_fault()` already guards against via `incident_id` matching. Implemented the diagnostic-counter fix (confirmed over the heavier `incident_id`-cross-check alternative, which would need a new `TeamState_t` field) — runtime behavior unchanged, the "shouldn't happen" branch is now visible if ever hit.


## core/lora_crypto.c

### 257. `lora_crypto_next_tx_counter` [global] — L34
**SEVERITY: LOW**
Reads and increments the file-scope `g_tx_counter` (plain, non-`volatile` `uint32_t`) with a non-atomic read-then-increment. If ever called from both an ISR and task context (or two preemptible task contexts) for LoRa TX, this would produce a repeated (key, nonce) pair and a catastrophic AEAD break (keystream reuse). All current call sites (comms_transport.c) are task-context only, single-threaded dispatch, so not currently triggered, but there is no lock/disable-IRQ and no `volatile`.
- **Connects to:** lora_frame_pack, lora_crypto_reset
   **RESOLVED (re-scoped, not a live race)** — commit `d044a24`. Same single-task confirmation as item #15; documented the invariant.

### 258. `lora_crypto_reset` [global] — L41
**SEVERITY: NONE**
Straightforward reset of `g_tx_counter`, `g_rx_last_counter[]`, `g_rx_seen[]` via `memset`; sizes checked against declared array extents. Doc correctly warns callers not to invoke mid-session.
- **Connects to:** lora_crypto_next_tx_counter, lora_crypto_reconstruct_counter, lora_crypto_accept_counter

### 259. `lora_crypto_build_nonce` [global] — L48
**SEVERITY: NONE**
Checked the 12-byte nonce layout (byte 0 = sender id, bytes 1-7 = zero, bytes 8-11 = little-endian counter) against the header doc — matches, no overflow, no missing bytes.
- **Connects to:** lora_frame_pack, lora_frame_try_parse

### 260. `lora_crypto_reconstruct_counter` [global] — L66
**SEVERITY: MEDIUM**
In the declared-unreachable-but-handled 32-bit-wrap branch, the function returns `candidate` un-bumped while `candidate <= last_accepted`. If a previously-accepted ciphertext is retransmitted at that exact point, `aead_chacha20poly1305_open()` re-verifies successfully (byte-for-byte identical), and `lora_crypto_accept_counter()` is then called with a counter `<= last_accepted`, rolling the per-sender floor backward and reopening the replay window between the new (lower) floor and the old one. The comment calls this "astronomically unlikely" but the logic is a genuine floor-regression bug in the one case it's exercised.
- **Connects to:** lora_frame_try_parse, lora_crypto_accept_counter

### 261. `lora_crypto_accept_counter` [global] — L121
**SEVERITY: LOW**
Unconditionally overwrites `g_rx_last_counter[sender_board_id]` with `counter` whenever `sender_board_id < LORA_CRYPTO_MAX_SENDERS`, with no check that `counter > g_rx_last_counter[sender_board_id]` — relies entirely on the caller only invoking this after a successful verify with a counter from `lora_crypto_reconstruct_counter()`. This is what actually commits the floor regression from finding #4. Also non-`volatile`, non-atomic shared state; same ISR/task caveat as #1.
- **Connects to:** lora_frame_try_parse, lora_crypto_reconstruct_counter
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — the actual bug already fixed via top-10 item #10 (commit `7e733ab`); residual note is the same class as item #4.

## core/lora_frame.c

### 262. `lora_frame_checksum` [static] — L14
**SEVERITY: NONE**
Simple additive mod-256 checksum over `len` bytes; loop bound and indexing correct.
- **Connects to:** lora_frame_pack, lora_frame_try_parse

### 263. `lora_frame_pack` [global] — L27
**SEVERITY: NONE**
Checked `payload_len > LORA_FRAME_MAX_PAYLOAD` guard before any writes; buffer offsets match the header's documented wire layout exactly. Assumes `out_buf` has room per its documented contract, not re-validated here, but no caller in this codebase violates it.
- **Connects to:** lora_crypto_next_tx_counter, lora_crypto_build_nonce, aead_chacha20poly1305_seal, lora_frame_checksum

### 264. `lora_frame_try_parse` [global] — L79
**SEVERITY: NONE**
Checked length-gating order: header-size check → payload-size check (before computing `frame_size`, avoiding overflow) → wire-size check → checksum → AEAD open, and replay-state commit (`lora_crypto_accept_counter`) only happens after a real successful verify — matches the documented contract exactly.
- **Connects to:** lora_crypto_reconstruct_counter, lora_crypto_build_nonce, aead_chacha20poly1305_open, lora_crypto_accept_counter, lora_frame_checksum

## core/mission_execution_twin.c

### 265. `task_ref_equal` [static] — L12
**SEVERITY: NONE**
Simple field-wise struct comparison of `pool` and `slot`.
- **Connects to:** plan_contains_current_assignment, twin_plan_changed, twin_sync_observed_plan

### 266. `resolve_task_ref` [static] — L17
**SEVERITY: NONE**
Bounds-checks `task_ref.slot` before indexing `g_incidents`/`g_system_tasks`, and checks `slot_occupied` before returning a pointer.
- **Connects to:** resolve_assigned_task, twin_expected_task

### 267. `resolve_assigned_task` [static] — L34
**SEVERITY: NONE**
NULL-checks `team`; delegates bounds-checking to `resolve_task_ref`.
- **Connects to:** resolve_task_ref, mission_execution_twin_step

### 268. `clamp_stamina` [static] — L45
**SEVERITY: NONE**
Straightforward two-sided clamp.
- **Connects to:** twin_advance_expected_position, twin_advance_expected_work, mission_execution_twin_step

### 269. `resolve_actual_remaining_path_cost` [static] — L56
**SEVERITY: NONE**
Short-circuits to 0 when start==dest; otherwise delegates to `find_path_cost`.
- **Connects to:** find_path_cost, reset_twin_for_current_mission, mission_execution_twin_step, twin_advance_expected_position

### 270. `resolve_communication_health` [static] — L68
**SEVERITY: NONE**
Reads a valid enum-indexed field on an always-non-NULL team pointer.
- **Connects to:** reset_twin_for_current_mission, mission_execution_twin_step

### 271. `nominal_work_rate` [static] — L87
**SEVERITY: NONE**
Checks `task != NULL`, `support_team != NO_ASSIGNMENT`, and `support_team < NUM_TEAMS` (correct order) before indexing `g_teams[]`. Note: this function isn't the site of the `support_team=0` sentinel bug found in `mission_link.c`'s `mission_link_apply_incident`; it just consumes whatever value ends up there, so an upstream mis-set `support_team` causes it to silently include a phantom support team's work rate.
- **Connects to:** team_base_work_rate, twin_advance_expected_work

### 272. `compute_progress_ratio` [static] — L102
**SEVERITY: NONE**
`remaining_path_cost`/`work_remaining` both clamped to `<=` their baselines before subtraction, so neither can underflow; `total_units==0` guarded separately.
- **Connects to:** mission_execution_twin_step

### 273. `resolve_expected_stage` [static] — L125
**SEVERITY: NONE**
Simple three-way dispatch; both zero-remaining branches correctly fall through to MISSION_STAGE_WORK per the comment's intent.
- **Connects to:** reset_twin_for_current_mission, mission_execution_twin_step

### 274. `plan_contains_current_assignment` [static] — L142
**SEVERITY: NONE**
Loop bound `observed_plan_size` always `<= CBBA_MAX_BUNDLE_SIZE` by every writer — verified against the struct layout. No OOB read.
- **Connects to:** task_ref_equal, twin_sync_observed_plan

### 275. `twin_plan_changed` [static] — L165
**SEVERITY: NONE**
The `effective_size==0 -> 1` synthetic-plan-of-one special case is applied consistently with `twin_sync_observed_plan()`'s own handling; `observed_plan[0]` read only happens after `observed_plan_size` is already confirmed equal to 1, so not an uninitialized read.
- **Connects to:** task_ref_equal, mission_execution_twin_step

### 276. `twin_sync_observed_plan` [static] — L199
**SEVERITY: NONE**
Loop bound `planned_path_size` externally maintained but sized against the same array capacity as `observed_plan[]`, so no OOB write.
- **Connects to:** plan_contains_current_assignment, task_ref_equal, mission_execution_twin_step

### 277. `twin_expected_task` [static] — L223
**SEVERITY: NONE**
Checks `expected_path_index != NO_ASSIGNMENT && expected_path_index < observed_plan_size` before indexing; falls back to `resolve_assigned_task()` otherwise.
- **Connects to:** resolve_task_ref, resolve_assigned_task, mission_execution_twin_step

### 278. `twin_current_task_changed` [static] — L239
**SEVERITY: NONE**
Indexes `g_safezones[team->resting_zone_id]` without a local bounds check, but this mirrors the established codebase-wide invariant (confirmed across team.c, incident.c, simulation.c, mission_state_utils.c) that `resting_zone_id` is only ever meaningful while `heading_to_rest` is set, and every setter of `heading_to_rest=1` also sets a valid `resting_zone_id` in the same operation. Not a new bug here.
- **Connects to:** twin_expected_task, mission_execution_twin_step

### 279. `reset_twin_for_current_mission` [static] — L265
**SEVERITY: NONE**
Checked every field write against the struct layout and all three branches; `resolve_actual_remaining_path_cost()`'s `UINT16_MAX` sentinel is explicitly normalized to 0 before use as a baseline. No uninitialized reads: every twin field referenced later in `mission_execution_twin_step()` is written on every path.
- **Connects to:** resolve_communication_health, resolve_actual_remaining_path_cost, resolve_expected_stage, mission_execution_twin_step

### 280. `twin_advance_expected_position` [static] — L335
**SEVERITY: NONE**
Early-returns when remaining path cost is 0; on a failed `find_next_path_step()`, recomputes from scratch rather than leaving stale state; `step_cost >= expected_remaining_path_cost` guard prevents unsigned underflow.
- **Connects to:** find_next_path_step, environment_cell_travel_cost, resolve_actual_remaining_path_cost, clamp_stamina, team_travel_stamina_cost, mission_execution_twin_step

### 281. `twin_advance_expected_work` [static] — L376
**SEVERITY: NONE**
Same underflow-guard pattern as #24.
- **Connects to:** nominal_work_rate, clamp_stamina, mission_execution_twin_step

### 282. `mission_execution_twin_reset` [global] — L397
**SEVERITY: NONE**
Fully re-initializes every field of `MissionExecutionTwin_t` per team, cross-checked against the struct definition; no field left stale from a prior run.
- **Connects to:** (initializes state consumed by) mission_execution_twin_step

### 283. `mission_execution_twin_step` [global] — L436
**SEVERITY: NONE**
The heaviest function in the file, carries an in-code post-mortem documenting a previously-found and now-fixed one-tick double-advance bug. Traced through all four structural branches and confirmed `twin_just_reset` correctly gates all three places (`twin_advance_expected_position` at two call sites, `twin_advance_expected_work`) the double-step could recur. No new instance of the previously-fixed pattern, no new logic error found.
- **Connects to:** resolve_assigned_task, twin_plan_changed, twin_sync_observed_plan, twin_current_task_changed, reset_twin_for_current_mission, resolve_communication_health, twin_expected_task, twin_advance_expected_position, twin_advance_expected_work, compute_progress_ratio, resolve_expected_stage, team_recovery_rate

## core/mission_link.c

### 284. `mission_link_checksum` [static] — L62
**SEVERITY: NONE**
Identical correct additive-checksum pattern to lora_frame_checksum().
- **Connects to:** every mission_link_pack_*/parse_* function

### 285. `mission_link_pack_roster_payload` [global] — L83
**SEVERITY: NONE**
Mechanical byte-for-byte copy bounded by the declared payload size; relies on caller-provided buffer sizing per documented contract.
- **Connects to:** mission_link_pack_roster

### 286. `mission_link_parse_roster_payload` [global] — L95
**SEVERITY: NONE**
Reverse of #29.
- **Connects to:** mission_link_parse_roster

### 287. `mission_link_pack_incident_payload` [global] — L107
**SEVERITY: NONE**
Same pattern as #29 for MissionLinkIncident_t.
- **Connects to:** mission_link_pack_incident

### 288. `mission_link_parse_incident_payload` [global] — L119
**SEVERITY: NONE**
Same pattern as #30 for MissionLinkIncident_t.
- **Connects to:** mission_link_parse_incident

### 289. `mission_link_pack_fault_payload` [global] — L131
**SEVERITY: NONE**
Same pattern as #29 for MissionLinkFault_t.
- **Connects to:** mission_link_pack_fault

### 290. `mission_link_parse_fault_payload` [global] — L143
**SEVERITY: NONE**
Same pattern as #30 for MissionLinkFault_t.
- **Connects to:** mission_link_parse_fault

### 291. `mission_link_pack_status_payload` [global] — L155
**SEVERITY: NONE**
Same pattern as #29 for MissionLinkStatus_t.
- **Connects to:** mission_link_pack_status

### 292. `mission_link_parse_status_payload` [global] — L167
**SEVERITY: NONE**
Same pattern as #30 for MissionLinkStatus_t.
- **Connects to:** mission_link_parse_status

### 293. `mission_link_pack_roster` [global] — L188
**SEVERITY: NONE**
Magic/payload/checksum offsets match documented wire layout and wire-size macro.
- **Connects to:** mission_link_pack_roster_payload, mission_link_checksum

### 294. `mission_link_parse_roster` [global] — L204
**SEVERITY: NONE**
Length-gating order matches the documented 1/0/-1 contract exactly, same shape as lora_frame_try_parse().
- **Connects to:** mission_link_checksum, mission_link_parse_roster_payload, mission_link_apply_roster

### 295. `mission_link_pack_incident` [global] — L237
**SEVERITY: NONE**
Same pattern as #37 for incident.
- **Connects to:** mission_link_pack_incident_payload, mission_link_checksum

### 296. `mission_link_parse_incident` [global] — L253
**SEVERITY: NONE**
Same pattern as #38 for incident.
- **Connects to:** mission_link_checksum, mission_link_parse_incident_payload, mission_link_apply_incident

### 297. `mission_link_pack_fault` [global] — L286
**SEVERITY: NONE**
Same pattern as #37 for fault.
- **Connects to:** mission_link_pack_fault_payload, mission_link_checksum

### 298. `mission_link_parse_fault` [global] — L302
**SEVERITY: NONE**
Same pattern as #38 for fault.
- **Connects to:** mission_link_checksum, mission_link_parse_fault_payload, mission_link_apply_fault

### 299. `mission_link_pack_status` [global] — L335
**SEVERITY: NONE**
Same pattern as #37 for status.
- **Connects to:** mission_link_pack_status_payload, mission_link_checksum, mission_link_build_status

### 300. `mission_link_parse_status` [global] — L351
**SEVERITY: NONE**
Same pattern as #38 for status.
- **Connects to:** mission_link_checksum, mission_link_parse_status_payload

### 301. `board_incident_cache_get` [global] — L395
**SEVERITY: NONE**
Linear scan bounded by cache size, checks `in_use` before comparing `incident_id`.
- **Connects to:** mission_link_apply_incident (indirectly)

### 302. `board_incident_cache_find_eviction_index` [static] — L419
**SEVERITY: NONE**
Correctly finds the highest-priority in-use slot; `worst_priority` starts below any real priority value, so the first in-use slot always initializes it — no risk of an uninitialized index given its only caller only runs when the cache is full.
- **Connects to:** board_incident_cache_put

### 303. `board_incident_cache_put` [global] — L438
**SEVERITY: NONE**
Update-in-place, then free-slot scan, then eviction fallback — `target_index`/`have_target` correctly guard against using an uninitialized index.
- **Connects to:** board_incident_cache_find_eviction_index, debug_trace, mission_link_apply_incident

### 304. `board_incident_cache_remove` [global] — L488
**SEVERITY: NONE**
Linear scan, clears `in_use` on match, no-op otherwise.
- **Connects to:** mission_link_apply_incident

### 305. `mission_link_apply_roster` [global] — L508
**SEVERITY: NONE**
Bounds-checks `team_id` before indexing `g_teams[]`. Widening `uint8_t` stamina to float is lossless.
- **Connects to:** mission_link_parse_roster

### 306. `mission_link_find_incident_slot` [static] — L542
**SEVERITY: NONE**
Linear scan bounded by NUM_INCIDENTS, checks `slot_occupied` before comparing.
- **Connects to:** mission_link_apply_incident

### 307. `mission_link_find_free_incident_slot` [static] — L557
**SEVERITY: NONE**
Linear scan bounded by NUM_INCIDENTS, returns first unoccupied slot or NULL.
- **Connects to:** mission_link_apply_incident

### 308. `mission_link_apply_incident` [global] — L572
**SEVERITY: HIGH**
When creating a brand-new local incident slot, the function sets `incident->support_team = 0u;` instead of `NO_ASSIGNMENT` (0xFF). Every other initializer of `support_team` in this codebase uses `NO_ASSIGNMENT` to mean "no support team assigned," and consumers such as `core/incident.c`'s work-rate computation and `core/mission_execution_twin.c`'s `nominal_work_rate()` treat any in-range value, including 0, as a real team reference. Since team_id 0 is always valid, whenever team 0 happens to be `STATUS_ON_SCENE` somewhere else at the moment a board first learns of a new incident over the mission link, that board will silently and incorrectly credit team 0's equipment-scaled work rate as a "support team" bonus on this freshly-created local incident — corrupting both the mission execution twin's baseline math and, for a board-hosted incident, the board's own real work computation. A clear sentinel-value bug (0 used where NO_ASSIGNMENT was intended), not documented as intentional anywhere.
- **Connects to:** mission_link_find_incident_slot, mission_link_find_free_incident_slot, board_incident_cache_put, board_incident_cache_remove; consumed downstream by core/incident.c's work-rate computation and core/mission_execution_twin.c's nominal_work_rate()

### 309. `mission_link_apply_fault` [global] — L639
**SEVERITY: NONE**
Bounds-checks `team_id`; loop bound matches both MissionLinkFault_t's and TeamFaultState_t's array sizes.
- **Connects to:** mission_link_parse_fault

### 310. `mission_link_team_current_task` [static] — L668
**SEVERITY: NONE**
Same bounds-check pattern as `resolve_task_ref()` in mission_execution_twin.c.
- **Connects to:** mission_link_build_status

### 311. `mission_link_build_status` [global] — L687
**SEVERITY: NONE**
Defensive out-of-range `team_id` branch fully initializes `*out` before returning. Truncating cast of stamina is safe per the documented clamp invariant elsewhere.
- **Connects to:** mission_link_team_current_task, team_effective_work_rate, mission_link_pack_status

### 312. `mission_link_incident_precedence` [global] — L760
**SEVERITY: NONE**
Pure function matching documented FLASH/PRIORITY cutoff exactly.
- **Connects to:** (external relay call sites)

### 313. `mission_link_fault_precedence` [global] — L770
**SEVERITY: NONE**
Matches documented IMMEDIATE/PRIORITY cutoffs; array indices are valid enum values.
- **Connects to:** (external relay call sites)

### 314. `mission_link_status_precedence` [global] — L796
**SEVERITY: NONE**
Matches documented FLASH/ROUTINE cutoffs.
- **Connects to:** (external relay call sites)

## core/mission_link_handshake.c

### 315. `handshake_checksum` [static] — L125
**SEVERITY: NONE**
Identical correct additive-checksum pattern.
- **Connects to:** mission_link_handshake_pack_req, mission_link_handshake_parse_req, mission_link_handshake_pack_reply, mission_link_handshake_parse_reply

### 316. `mission_link_handshake_pack_req_payload` [global] — L143
**SEVERITY: NONE**
Mechanical copy bounded by the payload size (2 bytes, statically asserted).
- **Connects to:** mission_link_handshake_pack_req

### 317. `mission_link_handshake_parse_req_payload` [global] — L155
**SEVERITY: NONE**
Reverse of #60.
- **Connects to:** mission_link_handshake_parse_req

### 318. `mission_link_handshake_pack_reply_payload` [global] — L167
**SEVERITY: NONE**
Same pattern, bounded by 3-byte payload.
- **Connects to:** mission_link_handshake_pack_reply

### 319. `mission_link_handshake_parse_reply_payload` [global] — L179
**SEVERITY: NONE**
Reverse of #62.
- **Connects to:** mission_link_handshake_parse_reply

### 320. `mission_link_handshake_pack_req` [global] — L196
**SEVERITY: NONE**
Layout matches wire-size macro.
- **Connects to:** handshake_checksum, mission_link_handshake_pack_req_payload

### 321. `mission_link_handshake_parse_req` [global] — L212
**SEVERITY: NONE**
Same 1/0/-1 gating order as every other parse function.
- **Connects to:** handshake_checksum, mission_link_handshake_parse_req_payload, mission_link_handshake_note_request_received

### 322. `mission_link_handshake_pack_reply` [global] — L245
**SEVERITY: NONE**
Same pattern as #64 for reply.
- **Connects to:** handshake_checksum, mission_link_handshake_pack_reply_payload

### 323. `mission_link_handshake_parse_reply` [global] — L280
**SEVERITY: NONE**
Same pattern as #65 for reply. Confirmed no collision risk from reusing shared magic bytes (laptop only ever calls this parser, DK only ever calls parse_req).
- **Connects to:** handshake_checksum, mission_link_handshake_parse_reply_payload, mission_link_handshake_note_remote_reply

### 324. `mission_link_handshake_board_init` [global] — L358
**SEVERITY: NONE**
Fully resets every piece of state this file owns via a bounded loop over HANDSHAKE_MAX_BOARDS.
- **Connects to:** (resets state consumed by) all other functions in this file

### 325. `mission_link_handshake_mark_local_ready` [global] — L383
**SEVERITY: NONE**
Trivial idempotent flag set.
- **Connects to:** mission_link_handshake_local_is_ready, mission_link_handshake_build_local_reply

### 326. `mission_link_handshake_local_is_ready` [global] — L388
**SEVERITY: NONE**
Trivial read.
- **Connects to:** mission_link_handshake_mark_local_ready

### 327. `mission_link_handshake_note_request_received` [global] — L393
**SEVERITY: NONE**
Trivial idempotent flag set, matches documented "safe to call repeatedly" contract.
- **Connects to:** mission_link_handshake_build_local_reply

### 328. `mission_link_handshake_build_local_reply` [global] — L398
**SEVERITY: NONE**
Requires both flags before filling and returning success; correctly not "consumed" (repeatable).
- **Connects to:** mission_link_handshake_local_is_ready, mission_link_handshake_note_request_received, mission_link_handshake_pack_reply

### 329. `mission_link_handshake_seed_timeout_clock_if_needed` [static] — L422
**SEVERITY: NONE**
Correct one-shot lazy-seed pattern via a valid-flag guard.
- **Connects to:** mission_link_handshake_should_send_relay_req, mission_link_handshake_relay_timed_out

### 330. `mission_link_handshake_note_relay_req_sent` [static] — L431
**SEVERITY: MEDIUM**
Writes `g_relay_req_sent_once[peer_board_id]`, `g_relay_req_last_sent_valid[peer_board_id]`, `g_relay_req_last_sent_tick[peer_board_id]` with **no bounds check** that `peer_board_id < HANDSHAKE_MAX_BOARDS`. Only called from `mission_link_handshake_should_send_relay_req()`, which has the same gap (#75) — a caller passing an out-of-range `peer_board_id` (e.g. via `comms_transport_board_count()` drift if `NUM_TEAMS`/`COMMS_TRANSPORT_TEAMS_PER_BOARD` is raised past `HANDSHAKE_MAX_BOARDS` without updating that macro — exactly the drift scenario the header's own comment warns about) causes an out-of-bounds **write** to these static arrays — real memory corruption on an MMU-less embedded target. Contrasts with `lora_crypto.c`'s and this same file's own `mission_link_handshake_note_remote_reply()` defensive `>= CAP` checks elsewhere — an inconsistent trust boundary.
- **Connects to:** mission_link_handshake_should_send_relay_req
   **RESOLVED** — commit `d044a24`. Fixed as one cluster with item #26 and its three siblings below - all five now bounds-check `peer_board_id` before indexing.

### 331. `mission_link_handshake_should_send_relay_req` [global] — L438
**SEVERITY: MEDIUM**
Indexes five `[peer_board_id]`-keyed arrays and calls `mission_link_handshake_note_relay_req_sent(peer_board_id, now)` with none of it bounds-checked against `HANDSHAKE_MAX_BOARDS`, unlike `mission_link_handshake_note_remote_reply()` in the same file which does check. Same out-of-bounds read/write risk as #74 if `peer_board_id` is ever out of range.
- **Connects to:** mission_link_handshake_seed_timeout_clock_if_needed, mission_link_handshake_note_relay_req_sent
   **RESOLVED** — commit `d044a24`. See item #26/`mission_link_handshake_note_relay_req_sent` above - fixed as one cluster.

### 332. `mission_link_handshake_relay_timed_out` [global] — L479
**SEVERITY: MEDIUM**
Indexes and writes `g_relay_timed_out[peer_board_id]` with no bounds check, same class of issue as #74/#75. Logic itself (sticky timeout, shared clock, "reply-seen wins over timeout" ordering) is otherwise correct.
- **Connects to:** mission_link_handshake_seed_timeout_clock_if_needed, mission_link_handshake_get_relay_reply
   **RESOLVED** — commit `d044a24`. See item #26/`mission_link_handshake_note_relay_req_sent` above - fixed as one cluster.

### 333. `mission_link_handshake_note_remote_reply` [global] — L502
**SEVERITY: NONE**
Correctly bounds-checks `reply->board_id >= HANDSHAKE_MAX_BOARDS` before indexing — the one function in this group that guards its index, appropriately since `board_id` comes from untrusted wire data rather than a trusted caller-supplied loop variable.
- **Connects to:** mission_link_handshake_parse_reply, mission_link_handshake_get_relay_reply, mission_link_handshake_relay_reply_seen

### 334. `mission_link_handshake_get_relay_reply` [global] — L517
**SEVERITY: MEDIUM**
Indexes `g_relay_reply_seen`/`g_relay_reply` with no bounds check before calling into `mission_link_handshake_relay_timed_out()` (also unguarded) — same class of issue as #74-#76, compounding through the call chain.
- **Connects to:** mission_link_handshake_relay_timed_out, mission_link_handshake_pack_reply
   **RESOLVED** — commit `d044a24`. See item #26/`mission_link_handshake_note_relay_req_sent` above - fixed as one cluster.

### 335. `mission_link_handshake_relay_reply_seen` [global] — L542
**SEVERITY: LOW**
Read-only, single-array access with no bounds check — same missing-guard pattern as #74-#78 but lower severity since it's a read, not a write.
- **Connects to:** (external caller: laptop/tools/two_board_harness.c, board mission_link_task.c)
   **RESOLVED** — commit `d044a24`. Fixed together with its MEDIUM-rated siblings below (same root cause: bounds-checked all five now).

## core/mission_link_override.c

### 336. `override_checksum` [static] — L28
**SEVERITY: NONE**
Identical correct additive-checksum pattern to the other checksum helpers.
- **Connects to:** mission_link_override_pack, mission_link_override_parse

### 337. `mission_link_override_pack_payload` [global] — L46
**SEVERITY: NONE**
Mechanical copy bounded by the payload size (5 bytes, statically asserted).
- **Connects to:** mission_link_override_pack, mission_link_override_route_or_apply

### 338. `mission_link_override_parse_payload` [global] — L58
**SEVERITY: NONE**
Reverse of #81.
- **Connects to:** mission_link_override_parse

### 339. `mission_link_override_pack` [global] — L75
**SEVERITY: NONE**
Layout matches wire-size macro.
- **Connects to:** override_checksum, mission_link_override_pack_payload

### 340. `mission_link_override_parse` [global] — L91
**SEVERITY: NONE**
Same 1/0/-1 gating order and structure as every other parse function.
- **Connects to:** override_checksum, mission_link_override_parse_payload

### 341. `mission_link_override_apply` [global] — L132
**SEVERITY: NONE**
Bounds-checks `team_id` before indexing `g_cbba_agent_state[]`. Sets `override_active` and `override_incident_id` together, matching the documented CBBA pin contract; does not itself validate the incident_id refers to a real/open incident, but that's documented as `cbba_apply_override_pin()`'s job, not a gap here.
- **Connects to:** mission_link_override_route_or_apply

### 342. `mission_link_override_route_or_apply` [global] — L150
**SEVERITY: NONE**
Bounds-checks `team_id` up front, avoiding a pointless relay attempt for a bad id. Correctly branches on local-apply vs. relay; payload buffer sized exactly to match what the pack function writes.
- **Connects to:** comms_transport_board_id_for_team, mission_link_override_apply, mission_link_override_pack_payload, comms_transport_send

## core/mission_state.c

(No functions defined in this file — only file-scope global variable definitions: `g_teams`, `g_incidents`, `g_system_tasks`, `g_safezones`, `g_num_safe_zones`, `g_grid`, `g_evacuation_gateways`, `g_safezone_evacuation_active`, `g_current_tick`, `g_sim_step_tick`, `g_world_seed`, `g_cbba_agent_state`, `g_mission_execution_twin`.)

## core/mission_state_utils.c

### 343. `team_status_to_string` [global] — L6
**SEVERITY: NONE**
Exhaustive switch over TeamStatus_t with an "UNKNOWN" default.
- **Connects to:** (debug/logging callers)

### 344. `incident_status_to_string` [global] — L18
**SEVERITY: NONE**
Same pattern as #87 for IncidentStatus_t.
- **Connects to:** (debug/logging callers)

### 345. `specialization_to_string` [global] — L30
**SEVERITY: NONE**
Same pattern as #87 for TeamSpecialization_t.
- **Connects to:** (debug/logging callers)

### 346. `priority_to_string` [global] — L43
**SEVERITY: NONE**
Same pattern as #87 for Priority_t.
- **Connects to:** (debug/logging callers)

### 347. `team_recovery_rate` [global] — L55
**SEVERITY: NONE**
Exhaustive switch over the three known fitness_tier values plus a default fallback. Silently defaulting on an out-of-range value matches the established default-on-invalid-enum convention used throughout this file and the rest of the codebase — a deliberate, consistent design choice.
- **Connects to:** mission_execution_twin_step

### 348. `equipment_speed_multiplier` [static] — L73
**SEVERITY: NONE**
Each equipment bit independently adds its own penalty; all seven flags used elsewhere are covered, none silently ignored.
- **Connects to:** team_travel_stamina_cost

### 349. `fitness_speed_multiplier` [static] — L88
**SEVERITY: NONE**
Same exhaustive-switch-with-default pattern as #91.
- **Connects to:** team_travel_stamina_cost

### 350. `team_travel_stamina_cost` [global] — L99
**SEVERITY: NONE**
Simple product of three bounded factors, no overflow risk.
- **Connects to:** equipment_speed_multiplier, fitness_speed_multiplier, twin_advance_expected_position

### 351. `team_set_available` [global] — L106
**SEVERITY: NONE**
NULL-checks `team`; early-returns if already AVAILABLE; correctly clears `heading_to_rest=0` and `resting_zone_id=NO_ASSIGNMENT` together — the canonical example, elsewhere in the codebase, of the NO_ASSIGNMENT sentinel convention that finding #52 (`mission_link_apply_incident`) deviates from for `support_team`.
- **Connects to:** snapshot_record


## core/path.c

### 352. `path_in_bounds` [static] — L7
**SEVERITY: NONE**
Checked signed-int comparison against GRID_WIDTH/GRID_HEIGHT (36x36); correctly rejects negative and over-range coordinates.
- **Connects to:** get_movement_cost, can_traverse_cell, find_next_path_step_with_equipment

### 353. `path_cache_begin` [global] — L33
**SEVERITY: NONE**
memset of the whole 1024-entry cache plus setting the active flag; documented single-threaded/intra-call use only, consistent with its only caller's usage pattern.
- **Connects to:** path_cache_lookup, path_cache_store, called from cbba.c's cbba_run

### 354. `path_cache_end` [global] — L39
**SEVERITY: NONE**
Simple flag clear; lookup/store both gate on the flag.
- **Connects to:** path_cache_lookup, path_cache_store

### 355. `path_cache_hash` [static] — L44
**SEVERITY: NONE**
Multiplicative hash folding 5 fields mod PATH_CACHE_SIZE; a collision only causes a cache miss since callers verify full key equality afterward.
- **Connects to:** path_cache_lookup, path_cache_store

### 356. `path_cache_lookup` [static] — L54
**SEVERITY: NONE**
Verifies `occupied` and full key match before trusting the slot; a hash collision cannot return a wrong answer.
- **Connects to:** path_cache_hash, find_next_path_step_with_equipment

### 357. `path_cache_store` [static] — L76
**SEVERITY: NONE**
Only writes when cache active; narrows ints but values are already grid-bounded or a cost checked elsewhere.
- **Connects to:** path_cache_hash, find_next_path_step_with_equipment

### 358. `get_movement_cost` [static] — L113
**SEVERITY: MEDIUM**
`equipment_flags` is cast to `(void)` and never used — confirmed `environment_cell_is_navigable`/`environment_cell_travel_cost` have no equipment-awareness at all. The entire "with_equipment" pathfinding variant is a no-op with respect to equipment: any hazmat/vehicle-capable routing distinction the API implies never takes effect, and no caller currently even passes a non-zero `equipment_flags`.
- **Connects to:** path_in_bounds, environment_cell_is_navigable, environment_cell_travel_cost, find_next_path_step_with_equipment
   **FLAGGED, not fixed — deliberately, same reasoning as its sibling `can_traverse_cell` below.** "Fixing" this means designing real equipment-aware terrain semantics (what equipment should unlock what terrain) from scratch - a feature decision with no existing design to implement, not a mechanical bug fix. Left as a documented gap.

### 359. `can_traverse_cell` [static] — L126
**SEVERITY: MEDIUM**
Same issue as `get_movement_cost`: `equipment_flags` ignored, so destination traversability never varies by equipment.
- **Connects to:** path_in_bounds, environment_cell_is_navigable, find_next_path_step_with_equipment
   **FLAGGED, not fixed — deliberately.** Same reasoning as `get_movement_cost` above: this is a missing feature (equipment-aware traversal), not a bug with an obvious fix. Left as a documented gap.

### 360. `get_heuristic` [static] — L136
**SEVERITY: NONE**
Manhattan distance via `abs()`; max value 2*35=70, far under uint16_t range for a 36x36 grid.
- **Connects to:** path_heap_f_cost

### 361. `path_cell_index` [static] — L165
**SEVERITY: NONE**
`x*GRID_HEIGHT+y` fits uint16_t for a 36x36 grid; correctness depends on caller passing in-bounds x/y (true everywhere except the gap noted in #19).
- **Connects to:** path_heap_push, path_heap_pop_min, path_heap_f_cost, path_heap_swap

### 362. `path_heap_reset` [static] — L170
**SEVERITY: NONE**
Resets size and marks every `g_heap_pos` slot `PATH_HEAP_NONE`; called once per search before any push.
- **Connects to:** find_next_path_step_with_equipment, path_heap_push

### 363. `path_heap_f_cost` [static] — L183
**SEVERITY: NONE**
Decodes cell back to x,y consistent with `path_cell_index`'s encoding; f_cost computed live, never stale.
- **Connects to:** path_heap_sift_up, path_heap_sift_down, path_cell_index, get_heuristic

### 364. `path_heap_swap` [static] — L193
**SEVERITY: NONE**
Swaps heap slots and keeps `g_heap_pos` in sync for both moved cells.
- **Connects to:** path_heap_sift_up, path_heap_sift_down

### 365. `path_heap_sift_up` [static] — L206
**SEVERITY: NONE**
Standard bubble-up terminating at root or when parent's f_cost is <= child's; loop termination correct.
- **Connects to:** path_heap_swap, path_heap_f_cost, path_heap_push, path_heap_decrease_key

### 366. `path_heap_sift_down` [static] — L224
**SEVERITY: NONE**
Standard bubble-down with left/right bound checks against `g_heap_size`; terminates when `smallest==i`.
- **Connects to:** path_heap_swap, path_heap_f_cost, path_heap_pop_min

### 367. `path_heap_push` [static] — L257
**SEVERITY: NONE**
No explicit capacity check against `GRID_WIDTH*GRID_HEIGHT`, but callers only push a given cell once (state transitions UNVISITED->OPEN before push, decrease-key used thereafter), so size is structurally capped by the array's size — dependent on `find_next_path_step_with_equipment` never pushing an out-of-grid cell (see #19).
- **Connects to:** path_cell_index, path_heap_sift_up, find_next_path_step_with_equipment

### 368. `path_heap_decrease_key` [static] — L269
**SEVERITY: NONE**
Relies on caller having already updated `g_cost` and on `g_heap_pos[x][y]` being valid; both preconditions hold at its call site.
- **Connects to:** path_heap_sift_up, find_next_path_step_with_equipment

### 369. `path_heap_pop_min` [static] — L278
**SEVERITY: NONE**
Standard pop-min with move-last-to-root + sift-down; the empty case and the re-sift path both leave `g_heap_pos` consistent.
- **Connects to:** path_heap_sift_down, find_next_path_step_with_equipment

### 370. `find_next_path_step_with_equipment` [global] — L309
**SEVERITY: HIGH**
`dest_x`/`dest_y` are bounds-checked via `can_traverse_cell()` before use, but `start_x`/`start_y` are **never validated** anywhere in this function. `nodes[start_x][start_y].g_cost = 0` and the subsequent `path_heap_push(nodes, dest_x, dest_y, start_x, start_y)` (which computes `path_cell_index(start_x,start_y)` and writes `g_heap_pos[start_x][start_y]`) perform raw out-of-bounds array writes into the file-scope statics `g_path_nodes[GRID_WIDTH][GRID_HEIGHT]` and `g_heap_pos[GRID_WIDTH][GRID_HEIGHT]` if `start_x`/`start_y` is ever negative or >=36 (e.g. a corrupted `team->loc_x/loc_y` from a fault-injection bug or memory corruption elsewhere). On an MMU-less embedded target this is silent memory corruption, not a crash. All current callers pass coordinates otherwise kept in-grid, so this is latent rather than actively triggered today, but the asymmetry with the destination check (which explicitly handles "team standing on an out-of-bounds/blocked cell") is a real gap.
- **Connects to:** can_traverse_cell, path_cache_lookup, path_cache_store, path_heap_reset, path_heap_push, path_heap_pop_min, path_heap_decrease_key, get_movement_cost, path_in_bounds; called by find_next_path_step, find_path_cost_with_equipment, find_path_cost

### 371. `find_next_path_step` [global] — L502
**SEVERITY: NONE**
Trivial wrapper forcing `equipment_flags=0`; `cost` parameter explicitly discarded via `(void)cost` (dead parameter, kept for API compatibility).
- **Connects to:** find_next_path_step_with_equipment; called by cbba.c, fault_injector.c, mission_execution_twin.c, incident.c, simulation.c, scoring.c

### 372. `find_path_cost_with_equipment` [global] — L515
**SEVERITY: NONE**
Converts a negative "no path" sentinel (-1) to `UINT16_MAX`; a real cost can never reach UINT16_MAX under normal per-cell cost clamping.
- **Connects to:** find_next_path_step_with_equipment

### 373. `find_path_cost` [global] — L537
**SEVERITY: NONE**
Trivial wrapper with `equipment_flags=0`.
- **Connects to:** find_path_cost_with_equipment; called by cbba.c, mission_execution_twin.c, incident.c

## core/poly1305.c

### 374. `poly1305_load_le32` [static] — L3
**SEVERITY: NONE**
Standard little-endian 4-byte load, byte-by-byte, no unaligned-access assumptions.
- **Connects to:** poly1305_blocks, poly1305_mac

### 375. `poly1305_store_le32` [static] — L11
**SEVERITY: NONE**
Standard little-endian 4-byte store, byte-by-byte.
- **Connects to:** poly1305_mac

### 376. `poly1305_blocks` [static] — L29
**SEVERITY: NONE**
Checked the 26-bit limb split, the `r*5` reduction trick, and the partial carry chain against the standard donna-style Poly1305 reference — matches exactly, including the `hibit` handling per RFC 8439 §2.5.1. No truncation: all products computed in `uint64_t` before narrowing.
- **Connects to:** poly1305_load_le32; called by poly1305_mac

### 377. `poly1305_mac` [global] — L82
**SEVERITY: NONE**
Checked: r-clamping masks match the standard donna clamp exactly; final-block padding never overruns the 16-byte buffer (`remainder<16` always); full carry propagation and the `h - p` conditional-reduction trick correctly implement mod 2^130-5 without branching on secret data; tag = (h+pad) mod 2^128 done with proper carry chaining. No length-based over/underflow for realistic AEAD buffer sizes.
- **Connects to:** poly1305_load_le32, poly1305_store_le32, poly1305_blocks; called by core/aead_chacha20poly1305.c

## core/random.c

### 378. `normalize_seed` [static] — L8
**SEVERITY: NONE**
Maps 0 to 1 to avoid an all-zero xorshift state (which would otherwise stick at 0 forever).
- **Connects to:** random_seed, random_seed_stream

### 379. `xorshift32` [static] — L13
**SEVERITY: NONE**
Standard xorshift32 triple (13/17/5); every caller of the public RNG API is task-level simulation code, never an interrupt handler, so the non-atomic read-modify-write here is not an ISR-vs-task race in this codebase as it stands.
- **Connects to:** random_u32, random_u32_stream

### 380. `random_seed` [global] — L22
**SEVERITY: NONE**
Sets `g_random_state` via `normalize_seed`.
- **Connects to:** normalize_seed; called by random_seed_all

### 381. `random_seed_stream` [global] — L28
**SEVERITY: NONE**
Bounds-checks `stream` against `(0, NUM_RANDOM_STREAM_STATES)` before indexing — correct guard, no off-by-one.
- **Connects to:** normalize_seed; called by random_seed_all, random_u32_stream

### 382. `random_seed_all` [global] — L38
**SEVERITY: NONE**
Loops over all valid streams seeding each via `derive_seed`; loop bound is inclusive of the last valid stream and doesn't touch index 0.
- **Connects to:** random_seed, random_seed_stream, derive_seed

### 383. `random_u32` [global] — L48
**SEVERITY: NONE**
Thin wrapper over `xorshift32(&g_random_state)`.
- **Connects to:** xorshift32; called throughout core/*.c

### 384. `random_u32_stream` [global] — L53
**SEVERITY: NONE**
Bounds-checks `stream`, falls back to the global stream if out of range, and lazily re-seeds a stream if its state is exactly 0. The lazy-reseed can't loop forever since `derive_seed` is coerced through `normalize_seed`, which maps 0 to 1.
- **Connects to:** random_u32, random_seed_stream, derive_seed, xorshift32

### 385. `random_range` [global] — L68
**SEVERITY: MEDIUM**
Normalizes `min>max` by swapping, then returns `min + (random_u32() % (max-min+1))`. If `max-min == UINT32_MAX` (e.g. `min=0, max=UINT32_MAX`), `max-min+1` wraps to `0` in `uint32_t` arithmetic, causing a modulo-by-zero (undefined behavior / typically a hardware fault or crash on both Cortex-M targets and the x86-64 laptop build). No current caller passes a full `[0, UINT32_MAX]` range, but nothing guards against it — a live landmine in a general-purpose utility.
- **Connects to:** random_u32; called from core/allocator.c, core/safezone.c, core/team.c, core/incident.c, core/environment.c, core/cbba.c
   **RESOLVED** — commit `5a6538a`. Added the full-range special case (returns `random_u32()` directly, no modulo).

### 386. `random_range_stream` [global] — L79
**SEVERITY: MEDIUM**
Same modulo-by-zero landmine as `random_range` when `max-min == UINT32_MAX`.
- **Connects to:** random_u32_stream; called from core/fault_injector.c and other stream-based callers
   **RESOLVED** — commit `5a6538a`. Same fix as `random_range()` above.

### 387. `derive_seed` [global] — L90
**SEVERITY: NONE**
`root_seed ^ (stream * SEED_MIX_CONSTANT)` — deterministic, no overflow concern since wraparound multiplication in `uint32_t` is well-defined.
- **Connects to:** called by random_seed_all, random_u32_stream

## core/recovery_manager.c

### 388. `recovery_clamp01` [static] — L65
**SEVERITY: NONE**
Standard clamp to [0,1]; no caller feeds NaN.
- **Connects to:** used throughout nearly every evaluator and recovery_make_signal

### 389. `recovery_positive` [static] — L76
**SEVERITY: NONE**
`value > 0 ? value : 0`, simple ReLU.
- **Connects to:** evaluate_progress, evaluate_stamina

### 390. `recovery_team_signature` [static] — L81
**SEVERITY: LOW**
`team->fault_state.active_mask` is `uint32_t` but shifted left by 52 bits into the `uint64_t signature` — only the low 12 bits survive; bits 12-31 are lost. Currently harmless because only `NUM_OPERATIONAL_FAULT_CLASSES==5` fault classes exist, but the signature (used by `recovery_dispatch` to dedupe repeated dispatch calls) would silently fail to detect a change confined to bits 12+ if that field's bit layout ever grows.
- **Connects to:** recovery_dispatch
   **RESOLVED** — commit `d044a24`. Added a `_Static_assert(NUM_OPERATIONAL_FAULT_CLASSES <= 12)` tripwire rather than re-deriving the whole bit-packing scheme for a currently-theoretical concern.

### 391. `recovery_metrics_reset` [static] — L97
**SEVERITY: NONE**
`memset`s both metrics structs to zero; sizes match array declarations.
- **Connects to:** recovery_manager_init

### 392. `recovery_metrics_note_recommendation` [static] — L103
**SEVERITY: NONE**
Bounds-checks `team_id`; only counts a "reversal" when both old and new recommendations are actionable and differ.
- **Connects to:** recovery_manager_step

### 393. `recovery_metrics_note_dispatch` [static] — L126
**SEVERITY: NONE**
Bounds-checks `team_id`, ignores CONTINUE_MONITORING, avoids double-counting when the same action is already active. `action` always 1..4 when indexing `dispatched[]`, safely within `RECOVERY_ACTION_COUNT==5`.
- **Connects to:** recovery_dispatch, g_current_tick

### 394. `recovery_metrics_update_resolution` [static] — L146
**SEVERITY: NONE**
Guards `g_current_tick < state->start_tick` before subtracting to avoid unsigned underflow; branch structure internally consistent; `state->action` always in `[1,4]`.
- **Connects to:** recovery_manager_step

### 395. `recovery_stage_for_team` [static] — L182
**SEVERITY: NONE**
Trivial accessor returning `twin->mission_stage`.
- **Connects to:** used by every evaluator and recovery_manager_step

### 396. `recovery_stage_multiplier_progress` [static] — L191
**SEVERITY: NONE**
Exhaustive switch with a `default` catching MISSION_STAGE_IDLE and any future stage value.
- **Connects to:** evaluate_progress

### 397. `recovery_stage_multiplier_stamina` [static] — L210
**SEVERITY: NONE**
Same pattern as #45.
- **Connects to:** evaluate_stamina

### 398. `recovery_stage_multiplier_mobility` [static] — L229
**SEVERITY: NONE**
Same pattern.
- **Connects to:** evaluate_mobility

### 399. `recovery_stage_multiplier_communication` [static] — L248
**SEVERITY: NONE**
Same pattern.
- **Connects to:** evaluate_communication

### 400. `recovery_stage_multiplier_equipment` [static] — L267
**SEVERITY: NONE**
Same pattern.
- **Connects to:** evaluate_equipment

### 401. `recovery_stage_action_floor` [static] — L286
**SEVERITY: NONE**
Two nested exhaustive switches (action then stage) with defaults; every combination resolves to a defined float, never uninitialized.
- **Connects to:** recovery_downgrade_to_achievable_action, recovery_select_primary_signal, every evaluate_* function, recovery_build_status

### 402. `recovery_action_cooldown` [static] — L344
**SEVERITY: NONE**
Exhaustive nested switch, `default: return 0u` for CONTINUE_MONITORING/unknown action in every stage.
- **Connects to:** recovery_apply_hysteresis

### 403. `recovery_action_persistence` [static] — L415
**SEVERITY: NONE**
Same structure as #51.
- **Connects to:** recovery_apply_hysteresis

### 404. `recovery_make_signal` [static] — L486
**SEVERITY: NONE**
Clamps confidence, sets `escalation_level=(uint8_t)action` — consistent with every other assumption in the file that escalation_level tracks the action 1:1.
- **Connects to:** called by nearly every evaluator and by recovery_apply_hysteresis/recovery_build_status

### 405. `recovery_aggregate_signal_confidence` [static] — L500
**SEVERITY: NONE**
Direct-match confidence takes priority over discounted higher-action fallback support; the short-circuit correctly prefers an exact match over a discounted higher-severity one even if numerically larger.
- **Connects to:** recovery_build_status

### 406. `recovery_reason_for_action` [static] — L532
**SEVERITY: LOW**
Both scan loops initialize `best_confidence` to `0.0f` and compare with `>=`, so any signal whose confidence *equals* the running best overwrites `reason` — in effect returns the *last* qualifying signal in evaluator-registration order rather than a genuinely highest-confidence one on ties. Only affects which `RecoveryReason_t` is reported for diagnostics/logging, not which `RecoveryAction_t` is dispatched.
- **Connects to:** recovery_build_status
   **RESOLVED** — commit `ddb46cf` (batch). Tie-break changed from `>=` to `>` in both scan loops.

### 407. `recovery_downgrade_to_achievable_action` [static] — L570
**SEVERITY: NONE**
Loop strictly decrements `action` while above CONTINUE_MONITORING and below that action's floor; terminates (bounded by the 5-level enum) and never underflows.
- **Connects to:** recovery_stage_action_floor; called by evaluate_progress, evaluate_mobility, evaluate_communication, evaluate_equipment, evaluate_consensus_fault, evaluate_fault_predictor

### 408. `evaluate_progress` [static] — L583
**SEVERITY: NONE**
Guards `threshold > 0.0f` before dividing; every stage branch returns a defined signal.
- **Connects to:** recovery_stage_for_team, recovery_positive, recovery_stage_multiplier_progress, recovery_clamp01, recovery_stage_action_floor, recovery_downgrade_to_achievable_action, recovery_make_signal

### 409. `evaluate_stamina` [static] — L623
**SEVERITY: HIGH**
Unlike `evaluate_mobility`, `evaluate_communication`, and `evaluate_equipment` — all three of which explicitly short-circuit to `CONTINUE_MONITORING` when `team->status == STATUS_RESTING || team->heading_to_rest` (their own inline comments cite `evaluate_stamina` as precedent for this pattern) — `evaluate_stamina`'s actual body contains **no** `STATUS_RESTING`/`heading_to_rest` check anywhere. Its `else if` branch (covering every status other than EN_ROUTE/ON_SCENE, including STATUS_RESTING and STATUS_AVAILABLE) fires `RETURN_TO_SAFE_ZONE` whenever `team->degraded || stamina <= CRITICAL_THRESHOLD || stamina < get_soft_stamina_floor(team)` — conditions routinely true for a team currently resting precisely because its stamina is low. This reproduces the exact oscillation bug the other three evaluators were patched for: the moment `update_team()`'s STATUS_RESTING handler promotes the team RESTING->AVAILABLE mid-tick while stamina is still below the soft floor, `evaluate_stamina` re-fires `RETURN_TO_SAFE_ZONE`, `recovery_dispatch`'s only guard (`status != STATUS_RESTING`) no longer blocks it, and `abandon_task_for_recovery` sends the team straight back to the safe zone it's already at/near, re-entering RESTING immediately and repeating indefinitely — the same RESTING<->AVAILABLE/EN_ROUTE bounce documented and fixed for mobility/communication/equipment, left unpatched here.
- **Connects to:** recovery_stage_for_team, recovery_positive, recovery_stage_multiplier_stamina, recovery_clamp01, get_soft_stamina_floor, recovery_stage_action_floor, recovery_downgrade_to_achievable_action, recovery_make_signal; feeds recovery_dispatch's RETURN_TO_SAFE_ZONE case

### 410. `evaluate_mobility` [static] — L732
**SEVERITY: NONE**
The `STATUS_RESTING || heading_to_rest` short-circuit correctly precedes the degraded/stage branches, so a resting team never gets a fresh recommendation while a mobility fault is still active.
- **Connects to:** recovery_stage_for_team, recovery_stage_multiplier_mobility, recovery_clamp01, recovery_stage_action_floor, recovery_downgrade_to_achievable_action, recovery_make_signal

### 411. `evaluate_communication` [static] — L824
**SEVERITY: NONE**
`communication_health` is actually a fault-severity value (0=no fault, higher=worse), so the comparisons against MINOR/SEVERE thresholds are directionally correct. Has the same RESTING/heading_to_rest short-circuit as `evaluate_mobility`.
- **Connects to:** recovery_stage_for_team, recovery_stage_multiplier_communication, recovery_clamp01, recovery_stage_action_floor, recovery_downgrade_to_achievable_action, recovery_make_signal

### 412. `evaluate_equipment` [static] — L899
**SEVERITY: NONE**
Subtraction only happens inside the `equipment_level < LOW_THRESHOLD` guard so it can't underflow; has the same RESTING/heading_to_rest guard as mobility/communication.
- **Connects to:** recovery_stage_for_team, recovery_stage_multiplier_equipment, recovery_clamp01, recovery_stage_action_floor, recovery_downgrade_to_achievable_action, recovery_make_signal

### 413. `evaluate_consensus_fault` [static, compiled out] — L1011
**SEVERITY: NONE**
Compiled out entirely (`RECOVERY_MANAGER_ENABLE_CONSENSUS_FAULT` is 0) and not registered in the evaluators table even when compiled — pure dead code today. Logic itself is sound; its own header comment already documents a known, non-blocking simplification.
- **Connects to:** fault_buffer_fuse, recovery_stage_action_floor, recovery_downgrade_to_achievable_action, recovery_make_signal — not connected to recovery_manager_step (unregistered)

### 414. `evaluate_fault_predictor` [static] — L1115
**SEVERITY: NONE**
The early `confidence <= 0.0f` return is always taken today since `fault_predictor_run()` is an untrained stub returning all-zero scores, but this is gated on a real future condition (`FAULT_MODEL_LOADED`), not an accidentally-dead branch.
- **Connects to:** watchdog_get_evidence_vector, fault_predictor_run, recovery_stage_action_floor, recovery_downgrade_to_achievable_action, recovery_make_signal

### 415. `recovery_select_primary_signal` [static] — L1210
**SEVERITY: NONE**
Discards any signal below its own action's floor and otherwise picks the highest-severity action, tie-broken by higher confidence within the same action. The `g_diag_discarded_signal_count` counter is unprotected but is only ever touched from this single task-context call site.
- **Connects to:** recovery_stage_action_floor; called by recovery_build_status

### 416. `recovery_apply_hysteresis` [static] — L1241
**SEVERITY: NONE**
Escalation only steps the action ladder by exactly one level per satisfied persistence window and de-escalation similarly steps down by one — no enum over/underflow possible. `pending_streak` capped at `UINT8_MAX`. The gradual, one-rung-at-a-time escalation is a deliberate anti-oscillation design, not a defect.
- **Connects to:** recovery_action_cooldown, recovery_action_persistence, recovery_make_signal; called by recovery_build_status

### 417. `recovery_build_status` [static] — L1331
**SEVERITY: NONE**
Always writes back into `state->status` after computing, so no path leaves it stale; the min-confidence + CONTINUE_MONITORING clause correctly clears `reason` back to NONE only when both conditions hold.
- **Connects to:** recovery_select_primary_signal, recovery_apply_hysteresis, recovery_aggregate_signal_confidence, recovery_reason_for_action, recovery_clamp01; called by recovery_manager_step

### 418. `recovery_status_from_state` [static] — L1383
**SEVERITY: NONE**
Trivial struct-copy accessor.
- **Connects to:** recovery_manager_get_status

### 419. `recovery_manager_init` [global] — L1389
**SEVERITY: NONE**
Resets every per-team field, seeds `last_assigned_task_pool/slot/status` from live `g_teams[]` so `recovery_manager_step`'s reassignment-detection logic doesn't fire spuriously on the first tick, resets `g_last_dispatch_tick` to a sentinel that differs from any real tick.
- **Connects to:** recovery_metrics_reset; called once at startup

### 420. `recovery_manager_step` [global] — L1410
**SEVERITY: NONE**
The stale-escalation-state reset condition fires before the evaluators run each tick and correctly re-derives `last_assigned_task_pool/slot/status` every iteration. `RECOVERY_MANAGER_EVALUATOR_COUNT` matches the evaluators array literal exactly. Not ISR-shared — only called from task-level code.
- **Connects to:** recovery_stage_for_team, evaluate_progress, evaluate_stamina, evaluate_mobility, evaluate_communication, evaluate_equipment, evaluate_fault_predictor, recovery_build_status, recovery_metrics_note_recommendation, recovery_metrics_update_resolution, watchdog_get_status

### 421. `recovery_dispatch` [global] — L1515
**SEVERITY: NONE**
Bounds-checks `team_id`; the per-tick `allocated_this_tick` reset is correctly gated so it only resets once per tick. Each action case's status guard matches documented intent and is internally consistent with how each evaluator special-cases RESTING (except evaluate_stamina, see finding #58 — this function's own guards are correct, the staleness originates upstream).
- **Connects to:** recovery_manager_get_status, recovery_team_signature, abandon_task_for_recovery, allocate_all_incidents, recovery_metrics_note_dispatch

### 422. `recovery_manager_get_recommendation` [global] — L1648
**SEVERITY: NONE**
Bounds-checked accessor with a safe CONTINUE_MONITORING default for invalid `team_id`.
- **Connects to:** g_recovery_state

### 423. `recovery_manager_get_status` [global] — L1656
**SEVERITY: NONE**
Bounds-checked; constructs a fully-initialized default struct for invalid `team_id` rather than returning a partially-initialized one.
- **Connects to:** recovery_status_from_state; called by recovery_dispatch

### 424. `recovery_manager_get_metrics` [global] — L1672
**SEVERITY: NONE**
Returns a copy of `g_recovery_metrics` by value.
- **Connects to:** g_recovery_metrics


## core/safezone.c

### 425. `generate_safe_zones` [global] — L13
**SEVERITY: MEDIUM**
The `MAX_ATTEMPTS` hard-exit fallback path commits `g_safezones[i]` at whatever `(x,y)` was last computed without re-running the duplicate-location check the normal path performs. Once `attempts >= 40` and the candidate cell is non-navigable, `x`/`y` are forced to `(0,0)`; if this happens for more than one zone index on a degenerate map, multiple safe zones can be registered at the identical `(0,0)` location with no de-duplication, unlike every other placement path in this function which checks against existing zone locations. A genuinely degenerate-map-only edge case (tracked partially via a diagnostic counter), but the duplicate-check gap itself is real and untracked.
- **Connects to:** random_seed_stream, random_range_stream, derive_seed, environment_find_safe_zone_site, environment_cell_is_navigable, environment_register_safe_zone
   **RESOLVED** — commit `5a6538a`. Added `find_free_navigable_cell()`, a deterministic whole-grid scan for an unclaimed cell, same shape as `initialize_evacuation_gateways()`'s earlier fix.

## core/scoring.c

### 426. `equipment_requirements` [static] — L43
**SEVERITY: NONE**
Pure switch/lookup, no indexing, all fields default-initialized, all branches bounded by `default: break`.
- **Connects to:** equipment_score, team_has_minimum_equipment

### 427. `equipment_requirement_pair_is_defined` [static] — L159
**SEVERITY: NONE**
Pure boolean lookup table mirroring `equipment_requirements`, no array indexing.
- **Connects to:** validate_equipment_requirement_matrix

### 428. `validate_equipment_requirement_matrix` [global] — L203
**SEVERITY: NONE**
Static array with sizeof-derived count, loop bound correct.
- **Connects to:** equipment_requirement_pair_is_defined, sim_runner_init

### 429. `team_has_minimum_equipment` [global] — L254
**SEVERITY: NONE**
Simple bitmask compare, no NULL deref given both pointers are always valid.
- **Connects to:** equipment_requirements

### 430. `equipment_score` [static] — L266
**SEVERITY: NONE**
Bit-counting loop bounded to 8 bits; division only happens after a zero-optimal early-return guard.
- **Connects to:** equipment_requirements, calculate_score, incident_utility_score, candidate_claim_is_better

### 431. `equipment_condition_factor` [static] — L304
**SEVERITY: NONE**
`level` clamped to `[0,1]` before use; division by a compile-time nonzero constant.
- **Connects to:** calculate_score, team_equipment_cost, incident_utility_score

### 432. `team_stamina_cost` [static] — L322
**SEVERITY: NONE**
Trivial complement of a value already bounded to `[0,1]`.
- **Connects to:** stamina_score, calculate_marginal_bundle_bid

### 433. `team_equipment_cost` [static] — L327
**SEVERITY: NONE**
Trivial complement of a value already clamped to `[0,1]`.
- **Connects to:** equipment_condition_factor, calculate_marginal_bundle_bid

### 434. `logarithmic_waiting_time_bonus` [static] — L332
**SEVERITY: NONE**
`log1pf` on non-negative input, result explicitly clamped to `[0,1]` before scaling.
- **Connects to:** priority_score

### 435. `normalize_scoring_weights` [static] — L351
**SEVERITY: NONE**
Guards `total <= 0.0f` by resetting to defaults before dividing, avoiding div-by-zero/negative-normalization.
- **Connects to:** scoring_set_weights

### 436. `scoring_set_weights` [global] — L373
**SEVERITY: NONE**
Passes through by value then assigns global — single-threaded sim state, no ISR context.
- **Connects to:** normalize_scoring_weights

### 437. `scoring_get_weights` [global] — L379
**SEVERITY: NONE**
Plain struct return by value.
- **Connects to:** (external config/UI callers)

### 438. `scoring_reset_weights` [global] — L384
**SEVERITY: NONE**
Plain global assignment.
- **Connects to:** sim_runner_init

### 439. `path_distance_score` [static] — L407
**SEVERITY: NONE**
Negative `path_cost` (no route) short-circuits to 0 before division; result clamped to `[0,1]`.
- **Connects to:** find_next_path_step, distance_score

### 440. `distance_score` [global] — L429
**SEVERITY: NONE**
Thin wrapper.
- **Connects to:** path_distance_score, calculate_score, candidate_claim_is_better

### 441. `capability_score` [global] — L439
**SEVERITY: LOW**
`CAPABILITY_MATRIX[specialization][req_capability]` indexes with no bounds check on either dimension. Every current producer keeps both fields in `[0,4]`, so not observed to misbehave today, but any future corrupted/uninitialized `TeamState_t`/`IncidentState_t` would read out of the 5x5 array with no guard.
- **Connects to:** calculate_score, incident_utility_score, candidate_claim_is_better, team.c's generate_teams
   **RESOLVED** — commit `ddb46cf` (batch). Both `CAPABILITY_MATRIX` indices now bounds-checked, defaulting to `0.0f`.

### 442. `priority_score` [global] — L486
**SEVERITY: NONE**
`age_ticks` underflow guarded; band math bounded; `default:` returns 0.0f for unrecognized priority.
- **Connects to:** logarithmic_waiting_time_bonus, calculate_score, incident_utility_score

### 443. `calculate_score` [global] — L532
**SEVERITY: NONE**
Pure weighted sum of already-bounded sub-scores.
- **Connects to:** equipment_score, equipment_condition_factor, capability_score, distance_score, priority_score, stamina_score

### 444. `incident_utility_score` [static] — L560
**SEVERITY: NONE**
Same shape as calculate_score minus the travel term (by design).
- **Connects to:** equipment_score, equipment_condition_factor, capability_score, priority_score, stamina_score, calculate_route_utility

### 445. `route_path_cost` [static] — L576
**SEVERITY: NONE**
`task.slot` bounds-checked before indexing; `slot_occupied` checked; negative path_cost short-circuits.
- **Connects to:** find_next_path_step, calculate_route_utility

### 446. `calculate_route_utility` [global] — L620
**SEVERITY: NONE**
`task.slot` bounds-checked; negative total cost handled; division by a compile-time-nonzero constant.
- **Connects to:** incident_utility_score, route_path_cost, calculate_marginal_bundle_bid

### 447. `calculate_marginal_bundle_bid` [global] — L673
**SEVERITY: LOW**
`trial_path[CBBA_MAX_BUNDLE_SIZE]` (size 3) is filled by a loop writing up to `agent->path_size + 1` entries with no internal check that `path_size < CBBA_MAX_BUNDLE_SIZE`; it implicitly trusts the caller. The sole call site (`cbba_build_bundle`, inside a loop bounded by `CBBA_COMMITMENT_LIMIT == CBBA_MAX_BUNDLE_SIZE == 3`) only ever calls this with `path_size <= 2`, so no overflow today, but the guarantee lives entirely in the caller, not this function — a latent stack-buffer-overflow risk if a future caller reuses it without preserving that invariant.
- **Connects to:** calculate_route_utility, cbba_build_bundle
   **RESOLVED** — commit `ddb46cf` (batch). Added an internal `path_size < CBBA_MAX_BUNDLE_SIZE` guard.

### 448. `stamina_score` [global] — L776
**SEVERITY: NONE**
Normalized then squared — if stamina ever exceeded MAX_STAMINA this wouldn't clamp, but `change_stamina()` in team.c always clamps stamina, so the invariant holds cross-module.
- **Connects to:** calculate_score, incident_utility_score, team_stamina_cost, team.c's change_stamina

### 449. `candidate_claim_is_better` [global] — L782
**SEVERITY: NONE**
Epsilon-based tiebreaking consistent in direction at every stage; final deterministic tiebreak prevents non-terminating oscillation.
- **Connects to:** capability_score, equipment_score, distance_score, cbba_build_bundle

## core/search_zones.c

### 450. `search_zones_init` [global] — L12
**SEVERITY: NONE**
Straightforward full reset of both per-district struct array and the grid-sized coverage array; loop bounds match declarations.
- **Connects to:** generate_world

### 451. `navigable_reachable_map` [static] — L46
**SEVERITY: NONE**
`queue[GRID_WIDTH*GRID_HEIGHT]` sized to the theoretical max BFS frontier (each cell enqueued at most once), so `tail` can never exceed the array size. Anchor out-of-range/non-navigable returns early leaving `reach` all-zero.
- **Connects to:** search_zone_coverage_percent, search_zone_pick_frontier_cell

### 452. `zone_id_for_location` [global] — L84
**SEVERITY: NONE**
Explicit clamp of x/y to grid bounds before indexing.
- **Connects to:** (external callers in incident/allocator modules)

### 453. `search_zone_trust` [global] — L94
**SEVERITY: NONE**
`district_id` bounds-checked; underflow guarded; division only after the boundary case is handled; result clamped to `[0,100]`.
- **Connects to:** (external allocator/dashboard callers)

### 454. `search_zone_mark_cell_covered` [global] — L119
**SEVERITY: NONE**
x/y and district_id bounds-checked before any write; "only count first-covered-this-cycle" comparison correctly avoids double counting.
- **Connects to:** simulation.c's search_sweep_step/search_mark_radius_covered

### 455. `search_zone_coverage_percent` [global] — L138
**SEVERITY: NONE**
`district_id` bounds-checked; fallback anchor-search loop terminates via a flag; `navigable_total==0` explicitly handled; `covered` clamped to `navigable_total` before division.
- **Connects to:** environment_get_district_center, navigable_reachable_map, search_zone_check_cycle_completion

### 456. `search_zone_check_cycle_completion` [global] — L204
**SEVERITY: NONE**
`district_id` bounds-checked; both completion and stall branches reset only documented-resettable fields.
- **Connects to:** search_zone_coverage_percent, simulation.c's search_sweep_step

### 457. `search_zone_pick_next_target` [global] — L245
**SEVERITY: NONE**
`exclude_mask` bit-test safely within a uint16_t's 16 bits; `best_zone` correctly returns NO_ASSIGNMENT sentinel for an all-excluded case.
- **Connects to:** (external allocator caller)

### 458. `search_zone_pick_frontier_cell` [global] — L268
**SEVERITY: NONE**
Inputs NULL/bounds-checked; `dist_sq` computed in int32_t with grid dimensions far too small to overflow; `found` flag correctly gates the "nothing uncovered" path.
- **Connects to:** navigable_reachable_map, environment_get_district_center

## core/sim_runner.c

### 459. `sim_runner_init` [global] — L11
**SEVERITY: NONE**
Return value of `validate_equipment_requirement_matrix()` is checked and propagated per the documented "fatal" contract; no state left half-initialized in the failure path.
- **Connects to:** validate_equipment_requirement_matrix, scoring_reset_weights, fault_config_apply_profile

### 460. `sim_start_seed` [global] — L26
**SEVERITY: NONE**
Straight-line sequencing matches the documented contract exactly.
- **Connects to:** fault_config_apply_profile, fault_injector_set_profile, generate_world, snapshot_begin_run

### 461. `sim_start_seed_board` [global] — L41
**SEVERITY: NONE**
Deliberately omits `generate_world()` per its documented board-only contract; otherwise identical to sim_start_seed. The header's maintenance warning about the two functions diverging is a process risk, not a code bug — both are currently in sync.
- **Connects to:** fault_config_apply_profile, fault_injector_set_profile, snapshot_begin_run

### 462. `sim_tick` [global] — L59
**SEVERITY: LOW**
`g_sim_step_tick = g_current_tick + 1u` wraps to 0 if `g_current_tick == UINT32_MAX`, momentarily desynchronizing event-logging tick numbers (self-heals within one tick since `simulation_step()` re-syncs it). Requires ~4.29 billion ticks — theoretical, not practically reachable.
- **Connects to:** fault_injector_step, allocate_all_incidents, simulation_step
   **ACCEPTED, moved to `KNOWN_LIMITATIONS.md`** — wraparound requires ~4.29 billion ticks and self-heals within one tick.

### 463. `sim_end_seed` [global] — L71
**SEVERITY: NONE**
Single delegated call, no logic of its own.
- **Connects to:** snapshot_end_run

### 464. `sim_runner_shutdown` [global] — L76
**SEVERITY: NONE**
Single delegated call, no logic of its own.
- **Connects to:** snapshot_close_dataset

## core/simulation.c

### 465. `generate_world` [global] — L32
**SEVERITY: NONE**
Full deterministic init sequence; diagnostic-array loops bounded by NUM_TEAMS matching their declared sizes; MAX_SAFE_ZONES loop matches that array's size.
- **Connects to:** generate_environment, initialize_incidents, generate_safe_zones, initialize_evacuation_gateways, environment_apply_earthquake, search_zones_init, initialize_grid, generate_teams, fault_injector_init, watchdog_init, mission_execution_twin_reset, cbba_reset_improvement_counters, recovery_manager_init, fault_comms_init

### 466. `simulation_step` [global] — L77
**SEVERITY: NONE**
The documented two-pass record-then-dispatch split is implemented exactly as commented — `action_now[NUM_TEAMS]` fully populated in pass 1 before any dispatch runs in pass 2, so no team's snapshot can see another team's same-tick dispatch side effects.
- **Connects to:** environment_update_hazard_propagation, generate_incidents_if_needed, update_all_teams, move_all_teams, search_sweep_step, escalate_starved_incidents, update_all_incident_work, update_all_passive_effects, mission_execution_twin_step, watchdog_step, recovery_manager_step, snapshot_record, recovery_dispatch

### 467. `move_team` [global] — L171
**SEVERITY: NONE**
Both branches guard `pre_step_cost` sign before dereferencing next_x/next_y; task slot bounds-checked before indexing; `cached_remaining_path_cost` subtraction guarded against unsigned underflow; `unreachable_streak` increment guarded against uint8_t wraparound.
- **Connects to:** find_next_path_step, environment_cell_travel_cost, find_nearest_safe_zone, complete_return_to_rest, abandon_task_for_recovery, snapshot_record

### 468. `move_all_teams` [global] — L345
**SEVERITY: NONE**
Simple bounded loop over NUM_TEAMS matching array size.
- **Connects to:** move_team, simulation_step

## core/team.c

### 469. `generate_teams` [global] — L15
**SEVERITY: LOW**
The `fitness_tier` switch has no `default:` case. Since `specialization` is always `i % 5` here, every value 0-4 is covered and this cannot misbehave today, but if `TeamSpecialization_t` is ever extended without updating this switch, `fitness_tier` would be left at whatever memset set it to with no explicit fallback — unlike the equipment switch immediately above it, which does have a default.
- **Connects to:** random_seed_stream, derive_seed, random_range_stream, generate_world
   **RESOLVED** — commit `ddb46cf` (batch). Added a `default:` case to the `fitness_tier` switch, matching `fitness_speed_multiplier()`'s convention.

### 470. `change_stamina` [static] — L127
**SEVERITY: NONE**
Clamps `new_stamina` to `[MIN_STAMINA, MAX_STAMINA]` before assignment — the invariant `stamina_score()` relies on.
- **Connects to:** update_team, stamina_score

### 471. `update_team` [global] — L140
**SEVERITY: NONE**
Checked in detail given size/complexity: `replenish_rate` only read after being assigned in the same block; task slot bounds-checked before indexing in the ON_SCENE case; equipment_level underflow guarded before subtracting; `hours_worked` increment guarded against uint8_t wraparound; the communication-fault modulo divisor is guaranteed nonzero (`severity+1u`), and `g_current_tick >= start_tick` always holds.
- **Connects to:** change_stamina, get_soft_stamina_floor, begin_return_to_safe_zone, abandon_task_for_recovery, create_equipment_cache_incident, get_ready_stamina, recovery_manager_get_recommendation, team_set_available, snapshot_record

### 472. `update_all_teams` [global] — L473
**SEVERITY: NONE**
Bounded loop over NUM_TEAMS.
- **Connects to:** update_team, simulation_step

### 473. `get_ready_stamina` [global] — L493
**SEVERITY: NONE**
Switch covers all three FitnessTier_t values plus an explicit default fallback — no unhandled-enum gap (unlike generate_teams's tier-assignment switch above).
- **Connects to:** update_team, allocator.c's team_can_accept_incident

## core/test_metrics.c

### 474. `count_open_incidents` [static] — L30
**SEVERITY: NONE**
Bounded loop over NUM_INCIDENTS matching array size.
- **Connects to:** metrics_sample_tick, metrics_end_run

### 475. `count_active_incidents` [static] — L39
**SEVERITY: NONE**
Same shape as #50.
- **Connects to:** metrics_sample_tick, metrics_end_run

### 476. `count_active_system_tasks` [static] — L48
**SEVERITY: NONE**
Bounded loop over NUM_SYSTEM_TASKS matching array size.
- **Connects to:** metrics_sample_tick, metrics_end_run

### 477. `metrics_begin_run` [global] — L58
**SEVERITY: NONE**
All per-run static state reset with loop bounds matching declared sizes; `g_metrics` reset via full struct zero-init.
- **Connects to:** (external run-start caller)

### 478. `metrics_sample_tick` [global] — L93
**SEVERITY: NONE**
Peak-tracking comparisons use strict `>` so ties don't re-trigger; streak resets correctly ordered; no array access beyond the two bounded loops.
- **Connects to:** count_open_incidents, count_active_incidents, count_active_system_tasks

### 479. `test_metrics_record_equipment_exhaustion_recovery` [global] — L192
**SEVERITY: NONE**
Plain counter/sum increments, no indexing, no realistically-reachable overflow.
- **Connects to:** (external caller)

### 480. `test_metrics_record_support_reserve_recovery` [global] — L199
**SEVERITY: NONE**
Identical shape to #55.
- **Connects to:** (external caller)

### 481. `test_metrics_record_rest_entry` [global] — L206
**SEVERITY: NONE**
`team_id` bounds-checked before indexing.
- **Connects to:** (external caller at RESTING-entry transition)

### 482. `test_metrics_record_rest_exit` [global] — L215
**SEVERITY: NONE**
`team_id` bounds-checked; active-flag gate prevents recording an exit with no matching entry; subtraction guarded against unsigned underflow.
- **Connects to:** (external caller at RESTING-exit transition)

### 483. `test_metrics_record_stage_completion` [global] — L238
**SEVERITY: NONE**
Trivial counter increment plus delegated call.
- **Connects to:** test_metrics_record_progress

### 484. `test_metrics_record_progress` [global] — L244
**SEVERITY: NONE**
Single flag set, consumed by metrics_sample_tick.
- **Connects to:** test_metrics_record_stage_completion, metrics_sample_tick

### 485. `test_metrics_current_tick` [global] — L249
**SEVERITY: NONE**
Plain accessor.
- **Connects to:** (external callers)

### 486. `test_metrics_record_cbba_path_size` [global] — L254
**SEVERITY: NONE**
`path_size==0` explicitly excluded from sampling; max-tracking comparison correct.
- **Connects to:** (external caller)

### 487. `test_metrics_record_incident_assignment` [global] — L266
**SEVERITY: NONE**
`slot` bounds-checked before indexing three array-sized timer arrays.
- **Connects to:** (external caller)

### 488. `test_metrics_record_incident_work_start` [global] — L279
**SEVERITY: NONE**
`slot` bounds-checked; active-flag and `incident_id` match both required before use, preventing crediting a stale timer to a reused slot; subtraction guarded.
- **Connects to:** test_metrics_record_assignment_to_work_start

### 489. `test_metrics_record_assignment_to_work_start` [global] — L302
**SEVERITY: NONE**
Plain counter/sum increments.
- **Connects to:** test_metrics_record_incident_work_start

### 490. `metrics_end_run` [global] — L308
**SEVERITY: NONE**
Copies accumulated static counters into the returned struct; delegates to cbba's own getters for CBBA-owned counters rather than duplicating them.
- **Connects to:** count_open_incidents, count_active_incidents, count_active_system_tasks, cbba_get_two_opt_improvements, cbba_get_relocate_improvements

## core/watchdog.c

### 491. `clamp01` [static] — L62
**SEVERITY: NONE**
Standard two-branch clamp, correctly ordered.
- **Connects to:** normalized_error, accumulate_evidence, communication_evidence, stamina_evidence, path_evidence, timeout_evidence, weighted_confidence

### 492. `normalized_error` [static] — L73
**SEVERITY: NONE**
`full_scale <= 0.0f` guarded before division.
- **Connects to:** clamp01, progress_evidence, stamina_evidence

### 493. `accumulate_evidence` [static] — L81
**SEVERITY: NONE**
Asymmetric rise/decay formula is intentional; `instantaneous` clamped before use so the running value stays bounded by induction.
- **Connects to:** clamp01, watchdog_step

### 494. `progress_evidence` [static] — L91
**SEVERITY: NONE**
`fabsf` correctly handles sign before normalizing.
- **Connects to:** normalized_error, watchdog_step

### 495. `stamina_evidence` [static] — L97
**SEVERITY: NONE**
Additive stamina-critical bonus applied through clamp01 after adding, preventing overshoot past 1.0.
- **Connects to:** normalized_error, clamp01, watchdog_step

### 496. `communication_evidence` [static] — L109
**SEVERITY: NONE**
Divided by a fixed constant matching its documented 0-3 range, then clamped.
- **Connects to:** clamp01, watchdog_step

### 497. `operating_state_evidence` [static] — L115
**SEVERITY: NONE**
Simple binary comparison, no arithmetic risk.
- **Connects to:** watchdog_step

### 498. `update_mobility_confirmation` [static] — L146
**SEVERITY: NONE**
Checked carefully given the ISR-shared-state concern: all fields touched are per-team entries in a file-static array mutated only from `watchdog_step()`, which runs once per tick from a single simulation-task context — no ISR writer exists anywhere in this file or its callers, so no cross-context race or missing-volatile issue applies. The four-branch transition logic is mutually exclusive and covers all (prev,now) status pairs without a gap.
- **Connects to:** watchdog_step, path_evidence, classify_failure_mode

### 499. `path_evidence` [static] — L209
**SEVERITY: NONE**
`path_stagnation_streak` increments are explicitly saturation-guarded rather than relying on unsigned wraparound.
- **Connects to:** update_mobility_confirmation, watchdog_step

### 500. `assignment_evidence` [static] — L268
**SEVERITY: NONE**
The `MISSION_STAGE_IDLE` gate correctly distinguishes "genuinely idle" from "heading to rest," consistent with what path_evidence/timeout_evidence check.
- **Connects to:** watchdog_step

### 501. `timeout_evidence` [static] — L312
**SEVERITY: NONE**
`timeout_streak` increments bound-checked before incrementing; tick comparison is a straightforward unsigned comparison with no subtraction, so no underflow risk.
- **Connects to:** clamp01, watchdog_step

### 502. `evidence_flags_from_values` [static] — L359
**SEVERITY: NONE**
Each flag threshold applied independently and consistently across all 7 slots; indices match array sizes everywhere used.
- **Connects to:** watchdog_step

### 503. `weighted_confidence` [static] — L381
**SEVERITY: NONE**
The seven weight constants sum to ~1.0, so clamp01 on the weighted sum is a safety net, not masking a scaling bug.
- **Connects to:** clamp01, watchdog_step

### 504. `classify_failure_mode` [static] — L393
**SEVERITY: NONE**
The "best-so-far" cascade correctly starts at 0/NONE and only overwrites on strictly-greater combined scores, including the hardcoded 0.90f equipment override, which is correctly allowed to be beaten by a genuinely larger later score (intended "let evidence decide" behavior, not a bug). The `best >= 0.20f` gate is consistent with the threshold used elsewhere.
- **Connects to:** watchdog_step

### 505. `recommend_action` [static] — L456
**SEVERITY: HIGH**
`WATCHDOG_ANOMALY_THRESHOLD` (0.10f, lowered from 0.35f per commit `929eacd`) is now numerically *smaller* than `WATCHDOG_INVESTIGATE_THRESHOLD` (0.18f). The two-branch gate checks `confidence < INVESTIGATE_THRESHOLD` first, returning `2u`, before the `confidence < ANOMALY_THRESHOLD` branch (`return 1u`) can ever execute — since `[0, 0.10) ⊂ [0, 0.18)`, the second branch is now dead code. Worse, for confidence in `[0.10, 0.18)`, `watchdog_step()` has already set `anomaly_detected = true` (confidence `>= ANOMALY_THRESHOLD`), yet `recommend_action()` still returns the same "no anomaly" action code it would return near confidence 0, never reaching the switch that would recommend a specific corrective action. This silently downgrades every confirmed anomaly whose confidence lands in `[0.10, 0.18)` to a no-op recommendation — directly undermining the intent of the commit that lowered `WATCHDOG_ANOMALY_THRESHOLD` (its own message: "worst point on the curve"); this function was not updated to match the new threshold ordering.
- **Connects to:** watchdog_step, classify_failure_mode

### 506. `watchdog_init` [global] — L487
**SEVERITY: NONE**
Full struct zero-init followed by explicit sentinel overrides for fields that must not start at 0; loop bound matches array size.
- **Connects to:** generate_world

### 507. `watchdog_step` [global] — L503
**SEVERITY: NONE** (beyond the `recommend_action` issue already reported)
Call ordering correctly places `update_mobility_confirmation()` before the evidence functions that depend on its output; the `degraded` flag's clear-vs-hold logic correctly checks all fault classes before clearing, matching the documented "held through unrepaired fault lifecycle" contract.
- **Connects to:** progress_evidence, stamina_evidence, communication_evidence, operating_state_evidence, update_mobility_confirmation, path_evidence, assignment_evidence, timeout_evidence, accumulate_evidence, evidence_flags_from_values, weighted_confidence, classify_failure_mode, recommend_action, simulation_step

### 508. `watchdog_get_status` [global] — L594
**SEVERITY: NONE**
`team_id` bounds-checked, returning a zero-initialized struct rather than reading out of bounds.
- **Connects to:** simulation_step, generate_world

### 509. `watchdog_get_evidence_vector` [global] — L602
**SEVERITY: NONE**
`team_id` bounds-checked before any access; both output pointers independently NULL-checked per documented contract; out-of-range path explicitly zeros/clears outputs.
- **Connects to:** fault_comms.c's evidence assembly, fault_predictor.c model input

---

## Severity Summary

| Severity | Count |
|---|---|
| CRITICAL | 0 |
| HIGH | 11 |
| MEDIUM | 19 |
| LOW | 32 |
| NONE | 447 |
| **Total functions audited** | **509** |

## Prioritized Top 10 (fix first)

1. **`mission_link_apply_incident`** (core/mission_link.c:572) — HIGH. New locally-created incidents get `support_team = 0u` instead of `NO_ASSIGNMENT` (0xFF). Team 0 is a real team id, so if team 0 happens to be `STATUS_ON_SCENE` elsewhere at the moment a board first learns of an incident over the mission link, its work rate is silently and incorrectly credited to that incident as a "support bonus," corrupting `mission_execution_twin`'s baseline math and, for a board-hosted incident, the real work computation. Fix: initialize to `NO_ASSIGNMENT`, matching every other initializer of `support_team` in the codebase.
   **RESOLVED** — commit `5725725`, "Fix support_team sentinel default in mission_link_apply_incident". `support_team` now defaults to `NO_ASSIGNMENT`. Note: `spawned_by = 0u` three lines above the fix has the identical sentinel defect (same downstream risk via `complete_incident_stage`'s `EQUIPMENT_CACHE` completion path) and was flagged during the fix review but deliberately left unfixed — out of the requested scope for that change; still open.

2. **`recommend_action`** (core/watchdog.c:456) — HIGH. Commit `929eacd` lowered `WATCHDOG_ANOMALY_THRESHOLD` to 0.10, but it is now *smaller* than `WATCHDOG_INVESTIGATE_THRESHOLD` (0.18), so the investigate-branch gate (`confidence < 0.18 → return 2u`) fully shadows the anomaly branch. Any confirmed anomaly with confidence in `[0.10, 0.18)` — which `watchdog_step()` already flags as `anomaly_detected=true` — gets a silent no-op recommendation instead of a corrective action. This is a live regression from the most recent commit shown in git history and directly undermines its stated intent. Fix: reorder the comparisons (check anomaly threshold first) or re-derive the two thresholds so investigate ⊂ anomaly again.
   **RESOLVED** — commit `f090697`, "Fix recommend_action gate ordering after ANOMALY_THRESHOLD lowering". Gate now composes `min`/`max` of the two thresholds instead of assuming a fixed ordering. **Corrected, narrower scope of impact** (measured against `tron_snapshot_dataset.csv` before fixing): the specific corrective-action rate (codes 3–6, gated by the 0.18 bar regardless of ordering) was **never affected** — 6.12% TPR / 2.17% FPR both before and after. The bug's actual, measured effect was that `recommended_action`'s coarse "any signal" tier collapsed to that same 6.12%/2.17% instead of tracking `anomaly_detected`'s true 43.7% TPR, because 39,859 true-positive rows in the dead `[0.10, 0.18)` band silently reported "no anomaly" via `recommended_action` while `anomaly_detected` said `true` for the same row — an internal inconsistency between two output fields, not a lost detection.

3. **`evaluate_stamina`** (core/recovery_manager.c:623) — HIGH. Unlike `evaluate_mobility`/`evaluate_communication`/`evaluate_equipment` (each explicitly short-circuits on `STATUS_RESTING || heading_to_rest`), `evaluate_stamina` has no such guard. When `update_team()` promotes a team RESTING→AVAILABLE mid-tick while stamina is still below the soft floor, this function re-fires `RETURN_TO_SAFE_ZONE`, `recovery_dispatch`'s guard no longer blocks it (status is now AVAILABLE), and the team bounces straight back to the safe zone it just left, re-entering RESTING and repeating indefinitely. Fix: add the same RESTING/heading_to_rest short-circuit used by the other three evaluators.
   **RESOLVED** — commit `2ea200d`, "Add missing RESTING guard to evaluate_stamina". Added the same `STATUS_RESTING || heading_to_rest` short-circuit the other three evaluators already had.

4. **`comms_transport_inbox_push` / `comms_transport_poll_received` / `comms_transport_peek_frame_type` / `comms_transport_deliver_received`** (core/comms_transport.c:176,544,574,582) — ~~HIGH~~ **re-scoped to a documented future-risk, not a live bug (see below).** The shared RX inbox (`g_inbox_head/tail/count`) is plain non-`volatile` global state, mutated by task-context pollers and by `comms_transport_deliver_received()`, which `core/comms_transport.h`'s doc comment describes as the real SPI/IRQ RX-handler entry point. No critical section/interrupt-disable anywhere.
   **Investigated, not patched (by design — see below).** Commit `fd7ea33`. Checked both currently-implemented boards before assuming the header's "real IRQ" framing described the actual current wiring: it doesn't. `board/STM32N6570-DK/sx1262_hal.c` and `board/Raspberry_Pi_Pico/sx1262_hal.c` both explicitly document DIO1 as **polled**, not a real GPIO/EXTI interrupt ("IRQ line, plain GPIO input, polled by this file today" / "polled ... not an actual hardware interrupt"), and `comms_transport.h`'s own doc on `comms_transport_deliver_received()` says its real IRQ entry point is "still pending bring-up." On the DK, `sx1262_poll_rx()` and `mission_link_task_step()` both run sequentially inside the single micro T-Kernel task (`sim_task` in `main_board.c`) — there is currently no ISR anywhere in this codebase calling any of these four functions, so the race cannot occur today on either board. **Action taken:** added a concurrency-invariant comment to all four functions (`core/comms_transport.c`) documenting the current single-task/no-real-ISR assumption and requiring a critical section (or lock-free SPSC ring) be added *before* DIO1 is ever wired to a real interrupt — not a functional fix, since there is nothing to fix yet, but a tripwire for whoever does that rewiring later. Revisit with a real fix once board bring-up actually adds the interrupt.

5. **`fault_buffer_record_remote_vote`** (core/fault_buffer.c:233) — HIGH. `weight`/`class_mask` come straight off the wire with no validation; the accumulator is clamped only on the upper bound, so a corrupt or hostile frame with a negative/NaN weight drives a team's fault-vote tally arbitrarily off, silently corrupting `fault_buffer_fuse()`'s quorum decision with no rejected-frame trail. Fix: validate/clamp `weight` to `[0, 1]` (or reject the frame) before accumulating.
   **RESOLVED, with a reachability caveat.** Commit `3777233`. Traced the only producer of a `LORA_FRAME_TYPE_FAULT_VOTE` frame, `fault_comms_cast_vote()` (`fault_comms.c:161`): it gates on `class_mask` built from `fault_predictor_run(evidence)`'s scores crossing `FAULT_COMMS_VOTE_DETECTION_THRESHOLD` (0.50), and `fault_predictor_run()` is a stub — `FAULT_MODEL_LOADED` is `#define`d to `0` (`fault_predictor.h:16`), so it always returns an all-zero `scores` struct, `class_mask` is always `0`, and `fault_comms_cast_vote()` returns before ever sending a frame. No selftest tool exercises `fault_buffer_record_remote_vote()` directly either. **This function is currently 100% unreachable in the running system** — the same dead-code status the audit itself already correctly assigned `evaluate_consensus_fault` (rated NONE there for being unregistered), inconsistently rated HIGH here for the identical reachability. Fixed anyway since the fix carries no design uncertainty (unlike item #4's interrupt-synchronization choice, deferred pending a real hardware decision) and removes a landmine for whoever trains and loads the real model later: `weight` is now clamped to `[0, NUM_TEAMS]` via `!(weight >= 0.0f)` (catches both negative values and NaN in one check — NaN fails every ordered comparison, so a plain `< 0.0f` guard would let it through unclamped and poison the accumulator permanently once summed in) before it touches either the history slot or `g_vote_weight`.

6. **`fault_comms_ingest_remote_frames`** (core/fault_comms.c:288) — **original claim INCORRECT, verified by re-reading the function.** The original entry read: "Drains the same shared inbox as `cbba_ingest_remote_mesh_replies()`; its `break`-on-unrecognized-frame-type semantics mean whichever of the two receivers runs first in a tick will pop-and-discard frames belonging to the other... Currently latent (not yet co-scheduled) but a real design landmine." On inspection this is wrong: both `fault_comms_ingest_remote_frames()` and `cbba_ingest_remote_mesh_replies()` (`core/cbba.c:986`) call `comms_transport_peek_frame_type()` first and only call the destructive `comms_transport_poll_received()` after confirming the peeked type is one they own; on a type they don't recognize, `break` follows a non-destructive peek, so nothing gets popped or dropped. Neither function has this bug.
   **The real bug — found by tracing where a frame actually could be silently dropped, one level deeper — is `mission_link_comms_transport_rx_step()` in `board/STM32N6570-DK/mission_link_task.c:550` and `board/Raspberry_Pi_Pico/mission_link_task.c:550` (byte-identical), OUTSIDE the original 509-function audit's scope entirely** (`board/*.c` isn't in `CORE_SOURCES`). It used `while (comms_transport_poll_received(...))` — an unconditional, destructive poll with the frame-type filter running only *after* the pop — so it silently consumed and dropped any `LORA_FRAME_TYPE_CBBA`/`FAULT_BROADCAST`/`FAULT_VOTE` frame sitting in the inbox when it ran, despite its own comment claiming it "never drains the whole inbox indiscriminately." **Confirmed live and reachable every tick, unlike items #4/#5 (not a race pending future interrupt/model work):** `main_board.c`'s per-tick loop runs `sim_tick()` (which drives `cbba_run()`→`cbba_ingest_remote_mesh_replies()` and `fault_comms_ingest_remote_frames()`) *before* `sx1262_poll_rx()` (pushes newly-arrived frames) and `mission_link_task_step()` (calls the culprit) in the same tick body — so a CBBA/fault frame arriving during that tick's `sx1262_poll_rx()` call gets swallowed by the mission-link RX step moments later, before `cbba`/`fault_comms` get a chance to see it on the next tick.
   **RESOLVED** — commit `63cd714`. Applied the same peek-first pattern `fault_comms_ingest_remote_frames()`/`cbba_ingest_remote_mesh_replies()` already use, to both `board/STM32N6570-DK/mission_link_task.c` and `board/Raspberry_Pi_Pico/mission_link_task.c`: peek the frame type, only poll (destructively) if it's `LORA_FRAME_TYPE_HANDSHAKE` or `LORA_FRAME_TYPE_MISSION_LINK` (the two types this task owns), otherwise leave it for the next consumer. `make check-board-compile` and `make check-pico-compile` both clean after the fix.
   **Follow-up, not yet done:** `board/*.c` (both boards) received zero coverage from the original 509-function audit, which was explicitly scoped to `core/*.c` only. This bug is direct evidence that gap hides real, live defects — worth a dedicated audit pass over `board/STM32N6570-DK/` and `board/Raspberry_Pi_Pico/` once the current core/ HIGH-severity queue (items #7 onward) is finished.

7. **`find_next_path_step_with_equipment`** (core/path.c:309) — HIGH. `start_x`/`start_y` are never bounds-checked (only `dest_x`/`dest_y` are, via `can_traverse_cell`), so a corrupted team location writes out of bounds into the file-scope `g_path_nodes`/`g_heap_pos` statics — silent memory corruption on an MMU-less embedded target. Fix: bounds-check `start_x`/`start_y` symmetrically with the destination.
   **RESOLVED, plus a sibling fix — same shape as item #1.** Traced the actual reachability chain before patching: `find_next_path_step()`/`find_path_cost()` are called with `team->loc_x/loc_y` as the start coordinate throughout `simulation.c`'s `move_team()`, `fault_injector.c`'s `inject_environment_fault()`, and `mission_execution_twin.c` — and `team->loc_x/loc_y` (`int16_t`, full range) is written straight from wire data with **zero validation** by `mission_link_apply_roster()` (`core/mission_link.c:508`). On the **laptop build this chain is dead** (`generate_teams()`'s own placement always stays in-bounds, and `mission_link_apply_roster()` is called only from `board/*/mission_link_task.c`, never from `laptop/main.c`'s tick loop — same reachability profile as item #1's `mission_link_apply_incident`); on the **board build it's live**: a corrupted roster frame (bit-flip surviving the checksum, or any future upstream bug) is a direct path to the out-of-bounds write.
   **Fix 1 (`core/path.c`):** added a `path_in_bounds(start_x, start_y)` check at the top of `find_next_path_step_with_equipment()`, returning `-1` the same way an unreachable destination already does — deliberately just an in-bounds check, not `can_traverse_cell()` like the destination gets, since a team must still be able to attempt to leave its current cell even if that cell just became non-navigable (existing, intentional behavior, preserved).
   **Fix 2 (`core/mission_link.c`):** checked for an existing reject/log convention before adding one — `mission_link_apply_roster()`'s only existing validation (`msg->team_id >= NUM_TEAMS`) rejects the whole message with a **silent early return**, no logging, no partial apply; no other field in this file is validated at all, and the file's one `debug_trace()` call is reserved for incident-cache eviction, not rejection. Matched that exact pattern: added a `msg->loc_x`/`msg->loc_y` range check against `GRID_WIDTH`/`GRID_HEIGHT` immediately after the `team_id` check, silently rejecting the whole message on failure, same comment style ("defensive only... guarantees valid values in practice").
   Commit `194e28d`. Verified clean on all three targets: `make check-board-compile`, `make check-pico-compile`, and a full laptop rebuild + `make smoke`.

8. **`assign_anchor`** (core/environment.c:585) — HIGH. Writes `g_districts[g_environment_summary.district_total++]` with no bounds check against `NUM_DISTRICT_TYPES`; safe only because it's called exactly 10 times today. Any future extra call (new district, refactor, bug) silently corrupts adjacent static memory. Fix: add an explicit bounds assertion/guard before the write.
   **RESOLVED, no scope surprises.** Confirmed the audit's description exactly: `assign_anchor` is `static` (file-private), called from exactly one place (`stage_generate_districts`) as a hardcoded sequence of 10 literal calls matching `g_districts[NUM_DISTRICT_TYPES]`'s size by construction — no other call site, no loop. Extra data point from checking board reachability: this function never runs on either board at all — `generate_environment()`/`generate_world()` is only reached via `sim_start_seed()`, and `main_board.c` explicitly calls `sim_start_seed_board()` instead ("the board variant, which does not self-generate a world," per its own comment); boards receive the environment via mission-link relay, not by generating it. Laptop-only, world-generation-time code. Added `if(g_environment_summary.district_total >= NUM_DISTRICT_TYPES) return;` at the top of the function, with a comment tracing the guard back to the single-call-site assumption it protects. Commit `b17239c`. Verified clean on all three targets (`check-board-compile`, `check-pico-compile`, laptop rebuild + `make smoke`).

9. **`cbba_apply_override_pin` / `cbba_build_bundle`** (core/cbba.c:538,584) — MEDIUM, but affects core task-assignment correctness. `bundle_size` and `path_size` are allowed to drift out of lockstep (the overflow guard only checks `path_size` against `CBBA_MAX_BUNDLE_SIZE`, never `bundle_size` against `CBBA_COMMITMENT_LIMIT` directly); currently safe only because the two constants happen to be equal. If they ever diverge, this becomes a real out-of-bounds write to `agent->bundle[]`/`bundle_bid[]`, and even today a pinned task can silently get "orphaned" (believed handled but never inserted into the path). Fix: guard both arrays' bounds independently at every insertion point.
   **`cbba_apply_override_pin`: RESOLVED, with corrected scope.** Traced every mutation site of both counters before patching: `cbba_build_bundle`'s two writes were actually already safe independently — `bundle[bundle_size]` is bounded by the enclosing `while(bundle_size < CBBA_COMMITMENT_LIMIT)` loop condition (checks `bundle_size` directly, not `path_size`), and the `path[]` insert has its own separate `path_size < CBBA_MAX_BUNDLE_SIZE` guard. Only `cbba_apply_override_pin` genuinely shared one guard (`path_size`) across both writes. Confirmed the invariant that made this safe today (`bundle_size <= path_size` always): `agent->path[]` never actually receives a `TASK_POOL_SYSTEM` entry in the current code — system tasks are assigned straight to `team->assigned_task_pool/slot` (`core/allocator.c`'s `allocate_all_incidents()`), entirely bypassing `bundle[]`/`path[]` — even though `cbba_release_lost_tasks()` already has code anticipating that changing ("System tasks are never in the bundle - always keep them," currently dead). Not a live out-of-bounds write today, but a real fragility resting on an undocumented invariant. **Fix:** added an independent `bundle_size >= CBBA_COMMITMENT_LIMIT` check alongside the existing `path_size` check, with a comment documenting the invariant this removes the dependency on and pointing at `cbba_release_lost_tasks()`'s dead system-task branch as the place that would need re-review if system tasks are ever wired into CBBA's `path[]`. Commit `424d24e`. Verified clean on all three targets.
   **`cbba_build_bundle`'s "orphaned pin" issue: FLAGGED, not fixed — same treatment as item #1's `spawned_by`.** When `path_size` hits `CBBA_MAX_BUNDLE_SIZE` while `bundle_size` still has room, a task can be credited as "won" via `bundle[]`/`winner[]` without ever being inserted into `path[]` — a real functional bug (the team believes it holds a task that never appears in its actual travel plan), not a memory-safety one. Left alone deliberately: the fix needs a design decision (drop the win when path is full, vs. extend path's effective capacity) rather than a mechanical bounds guard, and is out of scope for this pass.

10. **`lora_crypto_reconstruct_counter` / `lora_crypto_accept_counter`** (core/lora_crypto.c:66,121) — MEDIUM, security property. In the documented "astronomically unlikely" 32-bit-wrap edge case, a replayed old ciphertext can roll a sender's accepted-counter floor backward, reopening the replay window for a range of previously-rejected counters. `lora_crypto_accept_counter` has no defense of its own (unconditionally trusts the counter it's given). Fix: `lora_crypto_accept_counter` should refuse to lower `g_rx_last_counter[sender]`, independent of what `reconstruct_counter` computes.
   **RESOLVED, `reconstruct_counter` untouched by design.** Confirmed the wrap-guard fallback in `reconstruct_counter` (line 100-113) is the only path that can ever hand `accept_counter` a candidate `<= last_accepted` — every other path returns a value already `> last_accepted` by construction — and confirmed that fallback requires `last_accepted` within `0x10000` of `UINT32_MAX` (billions of prior frames from one sender; ~136 years at an optimistic sustained 1 frame/sec), genuinely not reachable in this project's lifetime. Left `reconstruct_counter` as-is and fixed the actual state-commit point instead: added a monotonic check to `lora_crypto_accept_counter()` (`!g_rx_seen[sender] || counter > g_rx_last_counter[sender]`) before overwriting the floor. Zero effect on normal operation (every legitimate counter presented to this function is already increasing), fully closes the floor-regression regardless of what upstream computes, and can't cause a false rejection since this function only ever runs after the AEAD tag has already verified — it's bookkeeping, not a gate. Commit `7e733ab`. Verified clean on all three targets.

*(Honorable mentions just outside the top 10: `random_range`/`random_range_stream` modulo-by-zero on a full `[0,UINT32_MAX]` range; `mission_link_handshake_*` relay-bookkeeping functions indexing `peer_board_id` with no bounds check; `road_reachable_map`'s ~5KB stack-allocated BFS queue on an embedded target that already had a sibling function's stack frame moved to BSS for the same reason; `generate_incidents_if_needed` silently dropping a hazard event when the incident pool is full.)*

# board/ Function-by-Function Audit (STM32N6570-DK and Raspberry Pi Pico)

Scope: all 11 `board/STM32N6570-DK/*.c` and `board/Raspberry_Pi_Pico/*.c` files, outside `CORE_SOURCES` and never covered by the original `core/`-only audit above. That gap is what surfaced the live bug fixed in `mission_link_comms_transport_rx_step()` earlier this session (see that item's note in the core/ audit) — this pass closes it properly.

Both boards run micro T-Kernel 3.0 and were confirmed (independently, per-board, not assumed by inheritance) to create exactly **one** simulation task (`tk_cre_tsk`) with no RTOS preemption, and to poll the LoRa radio's DIO1 line rather than servicing a real hardware interrupt. Findings that would otherwise read as "ISR/task race" concerns are qualified against that confirmed single-task model rather than assumed to be theoretical or assumed to be live.

Files audited, in order:
`board/STM32N6570-DK/dk_debug.c`, `main_board.c`, `snapshot.c`, `mission_link_task.c`, `sx1262_hal.c`; `board/Raspberry_Pi_Pico/debug.c`, `snapshot.c`, `syscalls.c`, `main_board.c`, `mission_link_task.c`, `sx1262_hal.c`.

`mission_link_comms_transport_rx_step()`'s inbox-draining fix (applied earlier this session, both boards) was explicitly excluded from re-flagging per the audit brief, but every other function — including the rest of that same function — was audited fresh.

**Audit-only, per instruction — nothing below has been fixed yet.**

---

## Severity Summary

| Severity | Count |
|---|---|
| CRITICAL | 0 |
| HIGH | 1 |
| MEDIUM | 15 |
| LOW | 11 |
| NONE | 90 |
| **Total functions audited** | **117** |

## Prioritized list (fix first, pending your triage)

1. **`spi5_init`** (board/STM32N6570-DK/sx1262_hal.c:189) — HIGH. Sets only `SPI_CFG2_MASTER`, never `SPI_CFG2_SSM`, despite the comment claiming software NSS management. With `SSM=0` in master mode and NSS routed as a plain GPIO (not the SPI5 AF), the peripheral's internal hardware NSS input floats — a classic MODF (mode fault) setup that STM32 SPI answers by auto-clearing `SPI_CR1_SPE`, disabling the peripheral. Once disabled, `spi5_xfer_byte()`'s unbounded TXP/RXP wait loops (finding below) spin forever, hanging the entire single-task firmware. This is a concrete, plausible boot-time hang on real DK hardware, not a theoretical concern — the DK board's most serious finding in this pass. Fix: set `SPI_CFG2_SSM` (and typically `SPI_CR1_SSI`) alongside `SPI_CFG2_MASTER`.

2. **`sx126x_hal_wakeup`** (both boards: `board/STM32N6570-DK/sx1262_hal.c:322`, `board/Raspberry_Pi_Pico/sx1262_hal.c:400`) — MEDIUM on both, but for different specific reasons. DK: uses an uncalibrated 10-cycle delay for the NSS-low wake pulse, 1000x smaller than the reset pulse's 10,000-cycle delay in the same file — internally inconsistent, no basis shown for 10 cycles satisfying the SX126x's minimum wake timing. Pico: has **no delay at all** between asserting and deasserting NSS, and skips the pre-emptive BUSY wait the DK's version has. Both currently dead code — nothing in this codebase calls `sx126x_wakeup()`/`sx126x_set_sleep()` — but both are real bugs waiting for sleep mode to ever get wired in, and the Pico's version is strictly worse than the DK's.

3. **`ensure_radio_configured`** (DK, `sx1262_hal.c:519`) / **`sx1262_hal_init`** (Pico, `sx1262_hal.c:699`) — MEDIUM on both. Every one of the ~9 one-time radio-configuration SPI calls (frequency, packet params, sync word, IRQ mask, etc.) discards its return code, and the "configured" flag is set unconditionally afterward. A single failed call during bring-up — plausible on both boards, especially the Pico, whose own header comment says pin assignments are "still placeholders" — silently leaves the radio partially configured with zero diagnostic anywhere, for the entire run. This runs on every single boot, unlike most of the other findings here.

4. **`sx1262_enter_rx`** (DK, `sx1262_hal.c:496`, LOW there) / **`sx1262_enter_rx`** (Pico, `sx1262_hal.c:535`, MEDIUM there) — same underlying issue on both boards (two agents independently rated it differently — DK's is called from fewer places, Pico's is called after every TX completion/timeout too, which is likely why it was rated higher there; worth reconciling to one severity during triage). Return codes discarded; a failed call leaves the radio silently not listening, with no retry.

5. **`mission_link_handshake_tx_step`** (both boards, same file/line: `mission_link_task.c:919`) — MEDIUM on both, independently confirmed by both agents. `g_relay_reply_sent[]`/`g_relay_reply_last_sent[]` (both `HANDSHAKE_MAX_BOARDS`-sized) are indexed by a loop bounded only by `comms_transport_board_count()`, with no clamp. This is the exact same root cause already fixed this session in `core/mission_link_handshake.c`'s own per-peer arrays (bounds-checked in every accessor now) — but that fix was never propagated to these board-local duplicate arrays. Currently unreachable (`board_count=2` today) but a real latent out-of-bounds write if `NUM_TEAMS` ever scales past 40 without also updating this file.

6. **`usermain`** (DK, `main_board.c:461`) — MEDIUM. `tk_sta_tsk()`'s return value isn't checked (unlike `tk_cre_tsk()`'s, which is). Since this starts the only task in the whole system, a failure here means total, silent, undiagnosable firmware inactivity. Note: the Pico's equivalent `usermain` was checked and found to correctly check `tk_cre_tsk` (the DK's own `tk_cre_tsk` check is also fine) — this is specifically about the unchecked `tk_sta_tsk` call, and it's unclear from the two independent audits whether the Pico's `tk_sta_tsk` call has the same gap or not; worth a quick specific check before fixing.

7. **`snapshot_begin_run`**/**`board_snapshot_ring_drain`**/**`sim_task`**'s telemetry-ring interaction (both boards) — MEDIUM, independently found on both boards with slightly different severity splits across the two functions involved. The final post-seed drain is capped at 32 frames per call and only called once; if per-seed telemetry volume exceeds that, `snapshot_begin_run()`'s unconditional ring reset silently and permanently discards the remainder — worse on the last seed of a run, since nothing drains after `sim_runner_shutdown()`. Explicitly a debug/telemetry side channel, not the authoritative record, which is why neither agent rated it above MEDIUM.

8. **`spi5_xfer_byte`** (DK) / **`sx1262_spi_xfer_byte`** (Pico) and **`wait_busy_low`** (DK) / **`sx1262_wait_not_busy`** (Pico) — MEDIUM/LOW split across boards for materially the same pattern: no-timeout busy-waits on SPI/radio status. The BUSY-line wait is intentionally undocumented-timeout per the datasheet (a command issued while BUSY is high is silently dropped, so waiting it out is the only correct policy) — that part is fine. The FIFO-status wait (`spi5_xfer_byte`/`sx1262_spi_xfer_byte`) has no such justification and would hang the single-task firmware if the SPI peripheral itself ever wedges (e.g. exactly the MODF scenario in finding #1) — the two issues compound on the DK specifically.

9. **`mission_link_representative_team`** (both boards, LOW/MEDIUM split) — hardcodes the board-assignment divisor as a bare `5u` literal instead of referencing `COMMS_TRANSPORT_TEAMS_PER_BOARD`. Unreachable at current constants, but silently produces a wrong team-to-board mapping if that macro is ever changed without updating this literal too.

10. **`mission_link_task_init`** (DK rated MEDIUM, Pico rated NONE for the *same underlying code* — the two agents weighted "is this ever actually re-invoked" differently) — doesn't reset `g_roster_applied_count`. Worth resolving to one severity: the function is documented as a re-init entry point regardless of whether anything currently calls it more than once.

*(Remaining LOW items — `fmt_fixed3`'s tiny-buffer/large-value edge cases, `debug_trace`'s intentional no-op, `_sbrk`'s missing lower-bound check, `_fstat`'s unchecked NULL, `board_led_init`/`board_led_pulse`'s unchecked BSP return values, `sx126x_hal_reset`'s missing post-reset settle delay — are all either explicitly-documented-as-intentional stubs or narrow, low-probability edge cases at current call patterns. Full detail in the numbered findings above.)*
## board/STM32N6570-DK/dk_debug.c

### 1. `fmt_fixed3` [static] — L45
**SEVERITY: LOW**
Correct for its actual call sites (stamina 0-100, decimals=3, bufsz=16): traced digit extraction, sign handling, and every `pos+1 < bufsz` guard before each write. Two latent (currently unreachable) issues: (1) `if(bufsz < 2) return;` leaves `buf` uninitialized/non-terminated if a future caller passes a smaller buffer; (2) `tmp[12]`/`int32_t scaled` would truncate/overflow for a value/decimals combination outside the stamina/CBBA-score range this file restricts it to. Neither triggered by any current caller.
- **Connects to:** print_world_state, debug_emit_live_frame

### 2. `debug_trace` [global] — L107
**SEVERITY: LOW**
Pure no-op (all params cast to void), silently dropping every trace call instead of emitting to UART/USB. Documented as intentional/gated-off-by-default, so no corruption/blocking, but on-device diagnostic trace is currently a complete black hole regardless of g_simulation_trace_enabled.
- **Connects to:** called from core/allocator.c and core/simulation.c

### 3. `print_world_state` [global] — L124
**SEVERITY: NONE**
Loop bound matches g_teams[NUM_TEAMS]'s actual size; fmt_fixed3 buffer sized correctly; format specifiers match field types; no NULL-pointer risk.
- **Connects to:** fmt_fixed3, team_status_to_string, reads g_environment_summary/g_teams[]

### 4. `initialize_test_scenario` [global] — L169
**SEVERITY: NONE**
Empty stub, intentionally not ported (desktop test-harness only). Confirmed nothing in core/ or board/ calls this for the board build.
- **Connects to:** none (unreferenced in board build)

### 5. `debug_emit_live_frame` [global] — L179
**SEVERITY: NONE**
Same bound/buffer checks as print_world_state; deliberately narrower payload than laptop's LIVEFRAME is a documented design choice.
- **Connects to:** fmt_fixed3, team_status_to_string, called from core/sim_runner.c

### 6. `debug_print_generator_evaluation` [global] — L220
**SEVERITY: NONE**
All 15 parameters discarded via (void); documented no-op stub, no side effects.
- **Connects to:** called from core/ incident-generator logic

### 7. `debug_print_generator_selection` [global] — L247
**SEVERITY: NONE**
Same as above - no-op stub, no side effects.
- **Connects to:** called from core/ incident-generator selection logic

## board/STM32N6570-DK/main_board.c

### 8. `mission_link_handshake_round_trip_complete` [static] — L249
**SEVERITY: NONE**
mission_link_handshake_build_local_reply() has no side effects beyond writing into the caller-supplied out struct on success - repeated calls in the busy-wait loops are safe/idempotent.
- **Connects to:** mission_link_handshake_build_local_reply, called by sim_task

### 9. `sim_task` [LOCAL/static] — L289
**SEVERITY: MEDIUM**
Two config-fragility issues (neither reachable with shipped defaults): (1) `BOARD_SEED + BOARD_SEED_COUNT` computed as uint32_t with no overflow guard - a build config with BOARD_SEED near UINT32_MAX would wrap the loop bound; (2) `tick % BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL` has no zero-guard - a build redefining that macro to 0 divides by zero on the first tick. Both latent, not reachable with current defaults (1u/1u and 50u).
- **Connects to:** sx1262_hal_init, sim_runner_init, comms_transport_init, mission_link_task_init, mission_link_handshake_board_init, mission_link_handshake_mark_local_ready, mission_link_handshake_round_trip_complete, sx1262_poll_rx, mission_link_task_step, comms_transport_dispatch_pending, mission_link_task_any_roster_applied, sim_start_seed_board, sim_tick, board_snapshot_ring_drain, sim_end_seed, sim_runner_shutdown, tk_dly_tsk/tk_slp_tsk

### 10. `usermain` [global/EXPORT] — L461
**SEVERITY: MEDIUM**
`tk_sta_tsk(sim_tskid, 0)`'s return value is not checked (unlike tk_cre_tsk's, which is handled correctly). If tk_sta_tsk fails, usermain proceeds to tk_slp_tsk(TMO_FEVR) as if the simulation task were running - the board sits silently parked forever with no diagnostic, indistinguishable from a normally-running-but-quiet system. Since this is the single entry point starting the only task in this single-task RTOS model, a silent failure here means the entire firmware does nothing with no error message.
- **Connects to:** sim_task (via tk_cre_tsk/tk_sta_tsk), µT-Kernel APIs

## board/STM32N6570-DK/snapshot.c

### 11. `board_snapshot_ring_pending` [global] — L73
**SEVERITY: NONE**
Trivial getter returning g_ring_count; not currently called anywhere (drain cadence uses a fixed tick interval instead).
- **Connects to:** none currently call it

### 12. `snapshot_begin_run` [global] — L78
**SEVERITY: MEDIUM**
Unconditionally resets g_ring_head/tail/count to 0 with no check of pending count beforehand. Safe only because main_board.c always does one final board_snapshot_ring_drain() before the next seed's snapshot_begin_run() - but that drain is capped at BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL (32) per call and only called once (main_board.c's own comment flags this as an assumption: "if that assumption ever stops holding, this would need a loop instead of one call"). If per-seed telemetry volume ever exceeds 32 undrained frames at end-of-seed, those frames are silently and permanently discarded with no counter, log, or assertion.
- **Connects to:** called from core/sim_runner.c (sim_start_seed_board/sim_start_seed); shares ring state with board_snapshot_ring_push/board_snapshot_ring_drain

### 13. `board_snapshot_ring_push` [static] — L102
**SEVERITY: NONE**
Overflow branch correctly overwrites the oldest entry at g_ring_head (same index g_ring_tail currently points at when full) and advances g_ring_tail; non-full path increments g_ring_count correctly; head wraps via modulo in both branches. Single-task-only caller confirmed, so lack of volatile on ring-index globals is not a live issue.
- **Connects to:** snapshot_record, snapshot_record_resolved

### 14. `snapshot_record` [global] — L128
**SEVERITY: NONE**
Thin wrapper casting event_type to uint8_t before pushing, consistent with the documented wire-format schema.
- **Connects to:** board_snapshot_ring_push, called from core/ incident/task code

### 15. `snapshot_record_resolved` [global] — L144
**SEVERITY: NONE**
NULL-checks task before dereferencing. Uses task->assigned_team directly (documented rationale: team may have been reassigned earlier in the same tick) - matches snapshot.h's contract, not a defect introduced in this port.
- **Connects to:** board_snapshot_ring_push, called from core/incident.c's complete_task()

### 16. `snapshot_end_run` [global] — L154
**SEVERITY: NONE**
Empty stub, correctly documented as intentional (run_id is embedded per-frame, no end marker needed).
- **Connects to:** called from core/sim_runner.c (sim_end_seed)

### 17. `snapshot_close_dataset` [global] — L163
**SEVERITY: NONE**
Empty stub, correctly documented (no on-device dataset file to close).
- **Connects to:** called from core/sim_runner.c (sim_runner_shutdown)

### 18. `board_snapshot_ring_drain` [global] — L183
**SEVERITY: NONE**
Loop bound correctly caps at BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL and g_ring_count>0; TelemetryFrame_t filled correctly field-by-field matching the packed wire struct; TELEMETRY_FRAME_PAYLOAD_SIZE (10) correctly bounds the byte-write loop, checksum computed over the same 10 bytes before being sent, matching the documented wire layout. g_ring_tail/g_ring_count update correctly matching push's indexing scheme. tm_putchar return values discarded, consistent with this file's already-documented UART tradeoff.
- **Connects to:** board_snapshot_ring_push, called from main_board.c's sim_task tick loop


## board/STM32N6570-DK/mission_link_task.c

### 19. `mission_link_usart1_write` [static] — L160
**SEVERITY: NONE**
Checked loop bound (i < len), byte cast to INT for tm_putchar. No off-by-one, no NULL deref given valid args.
- **Connects to:** tm_putchar; called by mission_link_send_status, mission_link_comms_transport_rx_step, mission_link_handshake_tx_step

### 20. `mission_link_task_any_roster_applied` [global] — L215
**SEVERITY: NONE**
Trivial getter; single-task model means no torn read, no ISR writer so volatile not needed.
- **Connects to:** called by main_board.c's gate loop; reads g_roster_applied_count

### 21. `mission_link_representative_team` [static] — L226
**SEVERITY: MEDIUM**
Hardcodes the board-assignment divisor as a bare literal 5u instead of referencing COMMS_TRANSPORT_TEAMS_PER_BOARD (currently 5, but documented as overridable). If that macro changes without updating this literal, this function computes a team id that no longer matches comms_transport_board_id_for_team()'s actual rule, silently misrouting incident/handshake relays to the wrong board.
- **Connects to:** comms_transport_board_id_for_team (the rule it must mirror); called by mission_link_apply_and_relay_incident, mission_link_handshake_tx_step

### 22. `mission_link_handshake_ready_now` [static] — L244
**SEVERITY: NONE**
Delegates to mission_link_handshake_build_local_reply(), matches documented round-trip semantics.
- **Connects to:** mission_link_handshake_build_local_reply; called by mission_link_route_or_apply_roster, mission_link_route_or_apply_fault, mission_link_apply_and_relay_incident, mission_link_comms_transport_rx_step

### 23. `mission_link_route_or_apply_roster` [static] — L265
**SEVERITY: NONE**
relay_buf sizing correct; readiness gate applied before both branches; roster-applied counter incremented only on the documented path.
- **Connects to:** mission_link_handshake_ready_now, comms_transport_board_id_for_team, mission_link_apply_roster, mission_link_pack_roster_payload, comms_transport_send; called by mission_link_rx_try_consume_one

### 24. `mission_link_route_or_apply_fault` [static] — L301
**SEVERITY: NONE**
Same structure as #5, correctly sized buffer, precedence computed via mission_link_fault_precedence(msg).
- **Connects to:** mission_link_handshake_ready_now, comms_transport_board_id_for_team, mission_link_apply_fault, mission_link_pack_fault_payload, mission_link_fault_precedence, comms_transport_send; called by mission_link_rx_try_consume_one

### 25. `mission_link_apply_and_relay_incident` [static] — L349
**SEVERITY: NONE**
board_count from comms_transport_board_count(), not hardcoded; correctly skips this board; relay_buf correctly sized. Depends on mission_link_representative_team() (see #3) but is not itself buggy.
- **Connects to:** mission_link_handshake_ready_now, mission_link_apply_incident, comms_transport_board_count, mission_link_representative_team, mission_link_pack_incident_payload, mission_link_incident_precedence, comms_transport_send; called by mission_link_rx_try_consume_one

### 26. `mission_link_rx_try_consume_one` [static] — L393
**SEVERITY: NONE**
Each parser follows the documented 1/0/-1 contract; compaction loop correctly shifts remaining bytes and decrements g_rx_buf_len, no underflow.
- **Connects to:** mission_link_handshake_parse_req, mission_link_handshake_note_request_received, mission_link_parse_roster, mission_link_route_or_apply_roster, mission_link_parse_incident, mission_link_apply_and_relay_incident, mission_link_parse_fault, mission_link_route_or_apply_fault; called by mission_link_usart1_rx_step

### 27. `mission_link_usart1_rx_step` [static] — L491
**SEVERITY: NONE**
Board-0-only gate correct; RX accumulation bounded by MISSION_LINK_RX_BUF_CAPACITY; correctly implements the corrected readiness-gate placement.
- **Connects to:** board_usart1_read_byte, mission_link_rx_try_consume_one; called by mission_link_task_step

### 28. `mission_link_comms_transport_rx_step` [static] — L550
**SEVERITY: NONE** (peek-before-pop fix already applied, excluded per instructions)
Rest of function checked fresh: HANDSHAKE branch payload-length guards before indexing payload[1]; MISSION_LINK sub-type switch guards payload_len before every parse call; STATUS case correctly restricted to board 0; readiness gate correctly scoped to only the four data types.
- **Connects to:** comms_transport_peek_frame_type, comms_transport_poll_received, mission_link_handshake_note_request_received, mission_link_handshake_parse_reply_payload, mission_link_handshake_note_remote_reply, mission_link_handshake_ready_now, mission_link_parse_roster_payload, mission_link_apply_roster, mission_link_parse_incident_payload, mission_link_apply_incident, mission_link_parse_fault_payload, mission_link_apply_fault, mission_link_parse_status_payload, mission_link_pack_status, mission_link_usart1_write; called by mission_link_task_step

### 29. `mission_link_send_status` [static] — L784
**SEVERITY: NONE**
Board-0 path and non-DK path both correctly sized; team 0 resolving to board 0 is divisor-independent (unlike #3's literal-5 usage).
- **Connects to:** mission_link_pack_status, mission_link_usart1_write, mission_link_pack_status_payload, mission_link_status_precedence, comms_transport_send; called by mission_link_status_tx_step

### 30. `mission_link_status_tx_step` [static] — L810
**SEVERITY: NONE**
Correctly filters to locally-hosted teams; four should_send triggers checked in sensible order; unsigned wraparound on now_ms-last_sent_ms is intentional/documented, matching cbba.c's established pattern.
- **Connects to:** cbba_now_ms, comms_transport_board_id_for_team, mission_link_build_status, mission_link_send_status; called by mission_link_task_step

### 31. `mission_link_handshake_tx_step` [static] — L919
**SEVERITY: MEDIUM**
g_relay_reply_sent[]/g_relay_reply_last_sent[] are fixed arrays capped at HANDSHAKE_MAX_BOARDS (8), but the peer loop (`for(peer=1; peer<board_count; peer++)`) never clamps peer < HANDSHAKE_MAX_BOARDS before indexing. Safe today (board_count=2 given current NUM_TEAMS/TEAMS_PER_BOARD), but if NUM_TEAMS grows without also bumping HANDSHAKE_MAX_BOARDS (a change this file's own comment explicitly anticipates as valid), this writes out of bounds. No defensive clamp exists despite the header explicitly flagging HANDSHAKE_MAX_BOARDS as a cap other code must respect.
- **Connects to:** cbba_now_ms, mission_link_handshake_build_local_reply, mission_link_handshake_pack_reply, mission_link_usart1_write, mission_link_handshake_pack_reply_payload, comms_transport_send, comms_transport_board_count, mission_link_handshake_should_send_relay_req, mission_link_handshake_pack_req_payload, mission_link_representative_team, mission_link_handshake_get_relay_reply; called by mission_link_task_step

### 32. `mission_link_task_init` [global] — L1045
**SEVERITY: MEDIUM**
Resets g_rx_buf_len, g_last_sent_status[], g_own_reply_sent, g_relay_reply_sent[] but NOT g_roster_applied_count. Harmless in the documented single-boot-call usage, but this is the designated re-init entry point; a re-invocation (test harness, soft-restart) would leave main_board.c's roster-arrival gate seeing a stale nonzero count against freshly-reset g_teams[] state.
- **Connects to:** writes g_rx_buf_len/g_last_sent_status[]/g_own_reply_sent/g_relay_reply_sent[]; does not touch g_roster_applied_count (read by mission_link_task_any_roster_applied)

### 33. `mission_link_task_step` [global] — L1070
**SEVERITY: NONE**
Straight-line dispatch of the four sub-steps in documented order; no state of its own.
- **Connects to:** mission_link_usart1_rx_step, mission_link_comms_transport_rx_step, mission_link_status_tx_step, mission_link_handshake_tx_step; called by main_board.c's per-tick loop

## board/STM32N6570-DK/sx1262_hal.c

### 34. `gpio_clock_enable_all` [static] — L131
**SEVERITY: NONE**
Enables clocks for all 7 ports; dummy read-back forces the required post-enable sync delay.
- **Connects to:** called by sx1262_hal_init

### 35. `gpio_set_output` [static] — L143
**SEVERITY: NONE**
2-bit MODER/OSPEEDR/PUPDR field math and 1-bit OTYPER checked against pin-shift formulas - correct.
- **Connects to:** called by sx1262_hal_init

### 36. `gpio_set_input` [static] — L151
**SEVERITY: NONE**
Clears MODER to input, sets PUPDR correctly; doesn't touch OSPEEDR/OTYPER (correct for an input).
- **Connects to:** called by sx1262_hal_init

### 37. `gpio_set_af` [static] — L157
**SEVERITY: NONE**
AFR[0] vs AFR[1] index/shift math correct; checked against PG2/PH8/PE15 - correct.
- **Connects to:** called by spi5_init

### 38. `gpio_write` [static inline] — L168
**SEVERITY: NONE**
BSRR single-write set/reset pattern is the standard atomic GPIO write idiom, no RMW race.
- **Connects to:** called by sx1262_hal_init, sx126x_hal_reset, sx126x_hal_wakeup, apply_rf_switch_for_opcode, sx126x_hal_write, sx126x_hal_read

### 39. `gpio_read` [static inline] — L174
**SEVERITY: NONE**
Correct IDR bit test.
- **Connects to:** called by wait_busy_low, sx1262_poll_rx

### 40. `spi5_init` [static] — L189
**SEVERITY: HIGH**
`SX1262_SPI->CFG2 = SPI_CFG2_MASTER;` sets only the MASTER bit. The comment claims software NSS management (SSM/SSOE off, driving NSS via GPIO), but SPI_CFG2_SSM (bit 26) is NOT actually set - leaving SSM=0 is hardware NSS management (the peripheral watches the real NSS AF line for master-mode-fault detection), the opposite of the stated intent. Since PA3 is configured as a plain GPIO output (not routed to SPI5's NSS AF), the peripheral's internal hardware NSS input is left disconnected/floating. With SSM=0 in master mode this is a classic setup for a MODF (mode fault) event, which STM32 SPI responds to by auto-clearing SPI_CR1_SPE (disabling the peripheral). Once disabled, spi5_xfer_byte()'s unbounded TXP/RXP wait loops (#23) would spin forever, hanging the single cooperative task permanently. Correct fix: set SPI_CFG2_SSM (and typically SPI_CR1_SSI) alongside SPI_CFG2_MASTER.
- **Connects to:** gpio_set_af, RCC registers; called by sx1262_hal_init

### 41. `spi5_xfer_byte` [static] — L223
**SEVERITY: MEDIUM**
No timeout on the TXP/RXP wait loops - unlike wait_busy_low()'s infinite wait (which is justified against datasheet BUSY semantics), a disabled/wedged SPI peripheral here (e.g. via #22's MODF scenario) hangs the entire single-task firmware with no diagnostic or recovery. Re-setting CR1|=CSTART on every byte call is idempotent/harmless, not a bug.
- **Connects to:** called by sx126x_hal_write, sx126x_hal_read

### 42. `crude_delay_cycles` [static] — L251
**SEVERITY: NONE**
Volatile loop counter prevents optimization-away; explicitly documented as an uncalibrated placeholder for generous-margin uses.
- **Connects to:** called by sx126x_hal_reset, sx126x_hal_wakeup

### 43. `wait_busy_low` [static] — L259
**SEVERITY: MEDIUM**
Infinite busy-wait with no timeout, intentionally documented (a command issued while BUSY is high is silently dropped per datasheet). But since there's exactly one cooperative task and this runs on every SPI transaction, a dead/disconnected/miswired radio (a real risk given this file's documented required solder-bridge rework) freezes the entire firmware (mission-link, sim tick, telemetry) with no watchdog recovery.
- **Connects to:** gpio_read; called by sx126x_hal_reset, sx126x_hal_wakeup, sx126x_hal_write, sx126x_hal_read

### 44. `sx1262_hal_init` [global] — L277
**SEVERITY: NONE**
GPIO roles assigned correctly per the wiring table; NSS/NRST idle correctly before spi5_init(). (Depends on spi5_init's register bug, #22, but this function's own logic is correct.)
- **Connects to:** gpio_clock_enable_all, gpio_set_output, gpio_write, gpio_set_input, spi5_init; called by board bring-up

### 45. `sx126x_hal_reset` [global] — L304
**SEVERITY: LOW**
No extra settle delay between NRST release and the first BUSY read - if BUSY takes a moment to assert after reset release, could in principle read stale-low. Low-confidence, unverified-against-datasheet-timing concern, not a demonstrated failure.
- **Connects to:** gpio_write, crude_delay_cycles, wait_busy_low; called by ensure_radio_configured

### 46. `sx126x_hal_wakeup` [global] — L322
**SEVERITY: MEDIUM**
NSS-low wake pulse uses crude_delay_cycles(10u) - an arbitrarily tiny, uncalibrated cycle count, inconsistent with sx126x_hal_reset's comparatively generous crude_delay_cycles(10000u) (1000x larger) for its reset pulse. Nothing establishes 10 cycles satisfies the SX126x's minimum wake-pulse timing at the actual core clock; if too short, could silently leave the radio asleep.
- **Connects to:** wait_busy_low, gpio_write, crude_delay_cycles; called by (currently unused elsewhere in this file - dead entry point exposed via sx126x_hal.h to the vendored driver)

### 47. `apply_rf_switch_for_opcode` [static] — L345
**SEVERITY: NONE**
Opcode constants cross-checked against sx126x.c's private enum; switch correctly sets RXEN/TXEN mutually exclusively, both low for SetSleep, untouched for other opcodes.
- **Connects to:** gpio_write; called by sx126x_hal_write

### 48. `sx126x_hal_write` [global] — L370
**SEVERITY: NONE**
Correctly snoops command[0] only when length>0; NSS bracketing correct; SetSleep trailing-wait exception documented and matches reference HALs. Always returns OK since there's no error path from the busy-wait SPI primitives to propagate (real risk lives in spi5_xfer_byte/wait_busy_low, #23/#25, not here).
- **Connects to:** apply_rf_switch_for_opcode, wait_busy_low, gpio_write, spi5_xfer_byte; called by the vendored sx126x_driver core for every command

### 49. `sx126x_hal_read` [global] — L411
**SEVERITY: NONE**
Correctly clocks NOP bytes during data-read phase; wait_busy_low bracketing correct. Same "always OK" caveat as #30, not a defect specific to this function.
- **Connects to:** wait_busy_low, gpio_write, spi5_xfer_byte; called by the vendored sx126x_driver core

### 50. `sx1262_enter_rx` [static] — L496
**SEVERITY: LOW**
Both underlying calls' return codes discarded via (void). Dormant given the HAL always reports success today, but a genuine unchecked-return-value instance if real error detection is ever added.
- **Connects to:** sx126x_set_standby, sx126x_set_rx_with_timeout_in_rtc_step; called by ensure_radio_configured, sx1262_poll_rx

### 51. `ensure_radio_configured` [static] — L519
**SEVERITY: MEDIUM**
All ~9 configuration calls have return codes discarded via (void), and g_radio_configured=1 is set unconditionally afterward regardless of whether any actually failed. A partial/failed radio bring-up is undetectable - proceeds to TX/RX against whatever partial state landed on the chip. lora_spi_transmit() (see below), by contrast, DOES check its own critical calls - this function's inconsistency stands out.
- **Connects to:** sx126x_hal_reset, sx126x_set_standby, sx126x_set_pkt_type, sx126x_set_rf_freq, sx126x_set_buffer_base_address, sx126x_set_dio_irq_params, sx126x_set_lora_mod_params, sx126x_set_lora_pkt_params, sx126x_set_lora_sync_word, sx126x_set_tx_params, sx1262_enter_rx; called by lora_spi_transmit, sx1262_poll_rx

### 52. `lora_spi_transmit` [global] — L586
**SEVERITY: NONE**
frame_len bounds checked against 0 and 255 before anything else; return codes of set_lora_pkt_params/write_buffer/set_tx all checked with early return on failure - correctly avoids setting g_lora_tx_busy unless TX was actually keyed.
- **Connects to:** ensure_radio_configured, sx126x_set_lora_pkt_params, sx126x_write_buffer, sx126x_set_tx; called by comms_transport.c's dispatcher

### 53. `lora_radio_tx_busy` [global] — L631
**SEVERITY: NONE**
Trivial getter of volatile int g_lora_tx_busy - volatile correctly present given the two call sites are both driven from the same single task's tick loop.
- **Connects to:** reads g_lora_tx_busy; called by comms_transport.c's dispatch_pending()

### 54. `sx1262_poll_rx` [global] — L653
**SEVERITY: NONE**
DIO1 fast-path early-return keeps common case cheap; all return codes checked with early returns; IRQ bits cleared with exact irq_status read (not a broader mask), preventing stale-DIO1 false retrigger; buffer-length bounds check correctly guards the read against an out-of-range wire value; TxDone/Timeout vs RxDone handling both correct against the driver's documented semantics.
- **Connects to:** ensure_radio_configured, gpio_read, sx126x_get_irq_status, sx126x_clear_irq_status, sx126x_get_rx_buffer_status, sx126x_read_buffer, lora_frame_try_parse, comms_transport_deliver_received, sx1262_enter_rx; called by main_board.c's per-tick loop


## board/Raspberry_Pi_Pico/debug.c

### 55. `fmt_fixed3` [static] — L54
**SEVERITY: NONE**
Every buffer write guarded by `pos + 1 < bufsz` before increment; truncation-toward-zero matches documented behavior for 0-100 stamina/score ranges; decimals only ever called with literal 3.
- **Connects to:** print_world_state, debug_emit_live_frame

### 56. `debug_trace` [global] — L116
**SEVERITY: NONE**
Intentional no-op per comment/debug.h doc (varargs float forwarding to tm_printf unsupported); gated behind g_simulation_trace_enabled, documented not accidental.
- **Connects to:** called from core/allocator.c, core/simulation.c

### 57. `print_world_state` [global] — L133
**SEVERITY: NONE**
Loop bound matches g_teams[] sizing; format specifiers match field types after promotion; fmt_fixed3 buffer correctly sized.
- **Connects to:** fmt_fixed3, team_status_to_string

### 58. `initialize_test_scenario` [global] — L178
**SEVERITY: NONE**
Empty stub, documented as intentionally not ported.
- **Connects to:** none

### 59. `debug_emit_live_frame` [global] — L188
**SEVERITY: NONE**
Same checks as print_world_state pass; narrower field set is a deliberate documented scope reduction.
- **Connects to:** fmt_fixed3, team_status_to_string

### 60. `debug_print_generator_evaluation` [global] — L229
**SEVERITY: NONE**
All parameters voided, documented intentional silent stub.
- **Connects to:** none

### 61. `debug_print_generator_selection` [global] — L256
**SEVERITY: NONE**
Same as above.
- **Connects to:** none

## board/Raspberry_Pi_Pico/snapshot.c

### 62. `board_snapshot_ring_pending` [global] — L73
**SEVERITY: NONE**
Trivial accessor; single-task model confirmed, no tearing concern despite volatile.
- **Connects to:** called from main_board.c, currently unused there

### 63. `snapshot_begin_run` [global] — L78
**SEVERITY: NONE**
Resets ring state unconditionally; the safety argument (final drain before next begin_run) holds for the single-seed-loop path, though see sim_task's finding below re: the final drain not always fully emptying the ring - that risk is attributed to sim_task/board_snapshot_ring_drain's interaction, not this function's own logic.
- **Connects to:** board_snapshot_ring_drain (indirectly), sim_task

### 64. `board_snapshot_ring_push` [static] — L102
**SEVERITY: NONE**
Overflow handling correct: when full, g_ring_head==g_ring_tail, so overwriting g_ring_head before advancing g_ring_tail correctly drops the oldest entry with no double-counting or OOB index.
- **Connects to:** snapshot_record, snapshot_record_resolved

### 65. `snapshot_record` [global] — L128
**SEVERITY: NONE**
Thin wrapper casting EventType_t to uint8_t; no truncation issue.
- **Connects to:** board_snapshot_ring_push

### 66. `snapshot_record_resolved` [global] — L144
**SEVERITY: NONE**
NULL-checks task; correctly uses task->assigned_team per documented rationale.
- **Connects to:** board_snapshot_ring_push

### 67. `snapshot_end_run` [global] — L154
**SEVERITY: NONE**
Empty stub, documented intentional.
- **Connects to:** none

### 68. `snapshot_close_dataset` [global] — L163
**SEVERITY: NONE**
Empty stub, documented intentional.
- **Connects to:** none

### 69. `board_snapshot_ring_drain` [global] — L183
**SEVERITY: LOW**
Wire-format construction correct (10-byte payload, checksum, magic bytes match telemetry_frame.h). The real gap: capped at BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL (32) per call, and main_board.c's single post-seed "final drain" call relies on the unenforced assumption that 32 covers typical pending volume. If more than 32 events are pending when the tick loop ends, the remainder is silently left in the ring, and the next snapshot_begin_run() unconditionally zeroes ring state, permanently discarding them (worse on the last seed, since nothing drains after sim_runner_shutdown()). Rated LOW since this is an explicitly-documented-as-lossy debug/telemetry side channel, not the authoritative record, and the gap is already flagged in surrounding comments as a known assumption.
- **Connects to:** called from main_board.c's sim_task

## board/Raspberry_Pi_Pico/syscalls.c

### 70. `_sbrk` [global] — L46
**SEVERITY: LOW**
Upper-bound check (heap_end+incr > RP2040_RAM_END) correctly prevents heap/stack collision. No lower-bound check: a negative incr (not reachable via newlib's actual nano-malloc usage here, but the stub itself doesn't defend against it) could walk heap_end back into .bss/.data with no guard.
- **Connects to:** called by newlib malloc internals, transitively by fabsf/log1pf users in core/scoring.c, core/watchdog.c

### 71. `_write` [global] — L66
**SEVERITY: NONE**
Unconditionally returns -1/ENOSYS, matching documented design; tm_printf/tm_putchar (the real output paths) never go through this.
- **Connects to:** none

### 72. `_read` [global] — L75
**SEVERITY: NONE**
Same pattern as _write - correctly stubbed to fail.
- **Connects to:** none

### 73. `_close` [global] — L84
**SEVERITY: NONE**
Correctly stubbed to fail, no real fds exist.
- **Connects to:** none

### 74. `_fstat` [global] — L91
**SEVERITY: LOW**
Dereferences st->st_mode with no NULL check on st. Only invoked by newlib's internal stdio init with a valid on-stack struct in practice, so theoretical.
- **Connects to:** none

### 75. `_isatty` [global] — L98
**SEVERITY: NONE**
Always returns 1, harmless default.
- **Connects to:** none

### 76. `_lseek` [global] — L104
**SEVERITY: NONE**
Always returns 0, correct no-op.
- **Connects to:** none

### 77. `_exit` [global] — L112
**SEVERITY: NONE**
Infinite empty loop as documented, correct terminal behavior.
- **Connects to:** none

### 78. `_kill` [global] — L121
**SEVERITY: NONE**
Always fails with EINVAL, correct for no process/signal model.
- **Connects to:** none

### 79. `_getpid` [global] — L129
**SEVERITY: NONE**
Always returns 1, harmless placeholder.
- **Connects to:** none

## board/Raspberry_Pi_Pico/main_board.c

### 80. `mission_link_handshake_round_trip_complete` [static] — L342
**SEVERITY: NONE**
scratch intentionally unread by caller; correctly forwards to mission_link_handshake_build_local_reply(), the correct two-condition gate.
- **Connects to:** mission_link_handshake_build_local_reply; called by sim_task

### 81. `board_led_init` [LOCAL] — L360
**SEVERITY: LOW**
Return values of gpio_set_pin()/gpio_set_val() (both ER, can report error) discarded - a GPIO config failure would silently leave LED state unconfirmed. Consistent with the same unchecked-return convention already used throughout sx1262_hal.c, not a newly introduced inconsistency.
- **Connects to:** gpio_set_pin, gpio_set_val (BSP); called by sim_task

### 82. `board_led_pulse` [LOCAL] — L369
**SEVERITY: LOW**
Same unchecked-return issue for gpio_set_val()/tk_dly_tsk(). Blocking tk_dly_tsk() is safe here since every call site is inside sim_task, a real task context, never an ISR.
- **Connects to:** gpio_set_val (BSP), tk_dly_tsk (kernel); called by board_led_blink_n, board_led_wait_pulse_handshake, board_led_wait_pulse_roster, sim_task

### 83. `board_led_blink_n` [LOCAL] — L383
**SEVERITY: NONE**
Simple bounded loop, no off-by-one or overflow risk at realistic call values.
- **Connects to:** board_led_pulse; called by board_led_wait_pulse_roster, sim_task

### 84. `board_led_wait_pulse_handshake` [LOCAL] — L404
**SEVERITY: NONE**
Single fixed pulse call matching its documented pattern.
- **Connects to:** board_led_pulse; called by sim_task

### 85. `board_led_wait_pulse_roster` [LOCAL] — L412
**SEVERITY: NONE**
Two blinks plus a top-up delay; total timing doesn't exactly match the comment's stated intent but this is an explicitly-approximate LED-timing cosmetic, not a functional bug.
- **Connects to:** board_led_blink_n, tk_dly_tsk (kernel); called by sim_task

### 86. `sim_task` [LOCAL] — L457
**SEVERITY: MEDIUM**
Full control flow checked: sim_runner_init() failure handled correctly; init ordering matches its own extensive comments; handshake/roster wait loops both correctly poll RX before stepping mission_link/dispatching comms (a previously-fixed bug per the comment, confirmed fixed in both loops); seed/tick loop bounds correct half-open ranges. The genuine issue: the post-tick-loop final drain (one board_snapshot_ring_drain() call, capped at 32 frames) is not guaranteed to fully empty the ring before the next snapshot_begin_run() unconditionally resets ring state - if per-tick snapshot volume in the last drain-interval window exceeds 32, leftover telemetry is silently and permanently lost with no warning (worse on the last seed, since nothing drains after sim_runner_shutdown()). Real, triggerable data-loss path, but confined to the debug/telemetry side channel (explicitly not the authoritative record per telemetry_frame.h) - hence MEDIUM not HIGH.
- **Connects to:** board_led_init, board_led_pulse, board_led_blink_n, board_led_wait_pulse_handshake, board_led_wait_pulse_roster, sx1262_hal_init, sx1262_rx_poll, sim_runner_init, comms_transport_init, mission_link_task_init, mission_link_handshake_board_init, mission_link_handshake_mark_local_ready, mission_link_handshake_round_trip_complete, mission_link_task_any_roster_applied, mission_link_task_step, comms_transport_dispatch_pending, sim_start_seed_board, sim_tick, board_snapshot_ring_drain, sim_end_seed, sim_runner_shutdown, tk_dly_tsk, tk_slp_tsk; called by usermain

### 87. `usermain` [global/EXPORT] — L724
**SEVERITY: NONE**
tk_cre_tsk() return value checked (sim_tskid<=0 -> failure message + return); tk_sta_tsk() starts the task; usermain parks in tk_slp_tsk(TMO_FEVR) rather than returning, correctly avoiding the "returning shuts down µT-Kernel" hazard. Confirms single-task model: exactly one tk_cre_tsk call in the whole file.
- **Connects to:** sim_task (via tk_cre_tsk/tk_sta_tsk), tk_slp_tsk (kernel)


## board/Raspberry_Pi_Pico/mission_link_task.c

### 88. `board_usart1_read_byte` [global/weak] — L148
**SEVERITY: NONE**
Weak stub deliberately returns -1 until a real UART driver links in a strong definition. Non-blocking contract honored, no side effects.
- **Connects to:** mission_link_usart1_rx_step

### 89. `mission_link_usart1_write` [static] — L160
**SEVERITY: NONE**
Byte-at-a-time write via tm_putchar; loop bound matches caller-supplied wire-size constants. tm_putchar's return discarded, matching the same convention already established in snapshot.c.
- **Connects to:** tm_putchar, mission_link_send_status, mission_link_comms_transport_rx_step, mission_link_handshake_tx_step

### 90. `mission_link_task_any_roster_applied` [global] — L215
**SEVERITY: NONE**
Trivial accessor, no side effects.
- **Connects to:** main_board.c gate check, mission_link_route_or_apply_roster, mission_link_comms_transport_rx_step

### 91. `mission_link_representative_team` [static] — L226
**SEVERITY: LOW**
`(uint8_t)(board_id * 5u)` truncates silently for board_id > 51. Unreachable today (board_count=2 given current constants), but no bounds check guards the multiply/cast if board_count is ever scaled up.
- **Connects to:** mission_link_apply_and_relay_incident, mission_link_handshake_tx_step

### 92. `mission_link_handshake_ready_now` [static] — L244
**SEVERITY: NONE**
Discards the built reply, only uses the return value as documented gate.
- **Connects to:** mission_link_handshake_build_local_reply, mission_link_route_or_apply_roster, mission_link_route_or_apply_fault, mission_link_apply_and_relay_incident, mission_link_comms_transport_rx_step

### 93. `mission_link_route_or_apply_roster` [static] — L265
**SEVERITY: NONE**
Readiness-gated correctly; relay_buf sized exactly to match the payload write, no overflow.
- **Connects to:** mission_link_handshake_ready_now, comms_transport_board_id_for_team, mission_link_apply_roster, mission_link_pack_roster_payload, comms_transport_send

### 94. `mission_link_route_or_apply_fault` [static] — L301
**SEVERITY: NONE**
Same shape/checks as roster variant, correctly sized.
- **Connects to:** mission_link_handshake_ready_now, comms_transport_board_id_for_team, mission_link_apply_fault, mission_link_pack_fault_payload, mission_link_fault_precedence, comms_transport_send

### 95. `mission_link_apply_and_relay_incident` [static] — L349
**SEVERITY: NONE**
Loop bound correct and non-overflowing for realistic board counts; relay_buf sized exactly to match; self-board correctly skipped.
- **Connects to:** mission_link_handshake_ready_now, mission_link_apply_incident, comms_transport_board_count, mission_link_representative_team, mission_link_pack_incident_payload, mission_link_incident_precedence, comms_transport_send

### 96. `mission_link_rx_try_consume_one` [static] — L393
**SEVERITY: NONE**
Verified against each parser's 1/0/-1 contract; fixed-order type trying can't starve a smaller/matching frame behind an incomplete larger one; compaction loop has no off-by-one or OOB access.
- **Connects to:** mission_link_handshake_parse_req, mission_link_parse_roster, mission_link_parse_incident, mission_link_parse_fault, mission_link_handshake_note_request_received, mission_link_route_or_apply_roster, mission_link_route_or_apply_fault, mission_link_apply_and_relay_incident

### 97. `mission_link_usart1_rx_step` [static] — L491
**SEVERITY: NONE**
Board-0-only gate correct; fill loop bounded, preventing overflow; drain loop correctly bounded by the consume function's contract.
- **Connects to:** board_usart1_read_byte, mission_link_rx_try_consume_one

### 98. `mission_link_comms_transport_rx_step` [static] — L550
**SEVERITY: NONE** (peek-before-poll fix already applied, confirmed, not re-flagged)
payload_len bounds-checked before every payload[1..] parse, buffer large enough for every checked size; HANDSHAKE branch correctly bypasses the readiness gate; STATUS sub-type correctly no-ops on a non-DK board. The `frame_type != LORA_FRAME_TYPE_MISSION_LINK` guard after the peek is dead code today (single-task, no real ISR) but harmless defensive code.
- **Connects to:** comms_transport_peek_frame_type, comms_transport_poll_received, mission_link_handshake_note_request_received, mission_link_handshake_parse_reply_payload, mission_link_handshake_note_remote_reply, mission_link_handshake_ready_now, mission_link_parse_roster_payload, mission_link_apply_roster, mission_link_parse_incident_payload, mission_link_apply_incident, mission_link_parse_fault_payload, mission_link_apply_fault, mission_link_parse_status_payload, mission_link_pack_status, mission_link_usart1_write

### 99. `mission_link_send_status` [static] — L784
**SEVERITY: NONE**
Board-0 and relay paths both sized exactly correctly; team-0-addressing trick consistent with the rest of the file.
- **Connects to:** mission_link_pack_status, mission_link_usart1_write, mission_link_pack_status_payload, mission_link_status_precedence, comms_transport_send

### 100. `mission_link_status_tx_step` [static] — L810
**SEVERITY: NONE**
Loop bound and ownership filter correct; unsigned-subtraction wraparound handling correct/intentional, matching cbba.c's own pattern; no stale last-sent fields after a send.
- **Connects to:** cbba_now_ms, comms_transport_board_id_for_team, mission_link_build_status, mission_link_send_status

### 101. `mission_link_handshake_tx_step` [static] — L919
**SEVERITY: MEDIUM**
g_relay_reply_sent[]/g_relay_reply_last_sent[] (both HANDSHAKE_MAX_BOARDS-sized) are indexed by a loop bounded only by comms_transport_board_count(), with no clamp against HANDSHAKE_MAX_BOARDS. core/mission_link_handshake.c's own equivalent per-peer state is now protected by explicit bounds checks in every accessor (fixed earlier this session), but this file's two LOCAL arrays are indexed directly with no such guard. Currently unreachable (board_count=2 today) but a latent out-of-bounds write if NUM_TEAMS is ever scaled past 40 without updating this file too. Same finding independently confirmed on the DK's identical function.
- **Connects to:** cbba_now_ms, mission_link_handshake_build_local_reply, mission_link_handshake_pack_reply, mission_link_usart1_write, mission_link_handshake_pack_reply_payload, comms_transport_send, comms_transport_board_count, mission_link_handshake_should_send_relay_req, mission_link_handshake_pack_req_payload, mission_link_representative_team, mission_link_handshake_get_relay_reply

### 102. `mission_link_task_init` [global] — L1045
**SEVERITY: NONE**
Zeroes all state this function owns; g_roster_applied_count/other untouched globals start at 0 via C zero-init and this function is called exactly once at boot per the documented/actual usage - not a re-init bug in practice (contrast with the DK finding on the same function, which flagged this as a real gap if ever re-invoked - same underlying code, same latent gap, rated NONE here vs MEDIUM there only because this agent weighted the "called once today" fact more heavily; worth reconciling as the same underlying issue during triage).
- **Connects to:** none (pure state reset), called once from main_board.c

### 103. `mission_link_task_step` [global] — L1070
**SEVERITY: NONE**
Trivial fixed-order dispatcher, RX before TX, matches documented design.
- **Connects to:** mission_link_usart1_rx_step, mission_link_comms_transport_rx_step, mission_link_status_tx_step, mission_link_handshake_tx_step

**Divergence check vs DK's mission_link_task.c:** byte-identical except board-name/BSP comments - no functional divergence.

**Task/interrupt-model check:** confirmed exactly one tk_cre_tsk call in this board's main_board.c (sim_task) - single-task model holds for the Pico too.

## board/Raspberry_Pi_Pico/sx1262_hal.c

### 104. `sx1262_spi_init` [static] — L195
**SEVERITY: NONE**
Idempotency guard correct for single-task use; reset/idle-level sequencing correct; PL022 SPI mode configuration (DSS_8BIT/FRF_MOTOROLA/SPO=0/SPH=0) matches SX126x's required mode 0.
- **Connects to:** sx126x_hal_write, sx126x_hal_read, sx126x_hal_reset, sx126x_hal_wakeup, lora_spi_transmit, sx1262_enter_rx, sx1262_hal_init

### 105. `sx1262_spi_xfer_byte` [static] — L267
**SEVERITY: LOW**
Busy-waits on local PL022 FIFO status bits with no timeout - resolves almost immediately in normal operation (not dependent on the external radio), same class as the DK's spi5_xfer_byte.
- **Connects to:** sx126x_hal_write, sx126x_hal_read

### 106. `sx1262_wait_not_busy` [static] — L282
**SEVERITY: NONE**
Deliberate no-timeout busy-wait on the SX126x BUSY line, matching the DK's wait_busy_low() and its documented datasheet rationale.
- **Connects to:** sx126x_hal_write, sx126x_hal_read, sx126x_hal_wakeup

### 107. `apply_rf_switch_for_opcode` [static] — L299
**SEVERITY: NONE**
Ported verbatim from the DK's identical function, correct switch coverage.
- **Connects to:** sx126x_hal_write

### 108. `sx126x_hal_write` [global] — L326
**SEVERITY: NONE**
Opcode snoop correctly guarded; BUSY-wait before NSS assert; loop bounds correct. Always returns OK (no failure path to detect at this level), consistent with the DK's identical HAL.
- **Connects to:** sx1262_spi_init, apply_rf_switch_for_opcode, sx1262_wait_not_busy, sx1262_spi_xfer_byte

### 109. `sx126x_hal_read` [global] — L358
**SEVERITY: NONE**
Same structure as write, correct NOP-byte clocking, no bounds issues.
- **Connects to:** sx1262_spi_init, sx1262_wait_not_busy, sx1262_spi_xfer_byte

### 110. `sx126x_hal_reset` [global] — L384
**SEVERITY: NONE**
NRESET pulse timing coarser than datasheet minimum but intentionally erring long. Doesn't itself wait on BUSY after release, but every subsequent hal call independently waits on BUSY first, so no race introduced.
- **Connects to:** sx1262_spi_init; called from sx1262_hal_init

### 111. `sx126x_hal_wakeup` [global] — L400
**SEVERITY: MEDIUM**
Pulses NSS low then immediately high with NO delay between the two calls, and doesn't wait on BUSY before starting the pulse - unlike the DK's equivalent, which inserts a delay cycle and a pre-emptive BUSY wait. Real bug (no guaranteed minimum NSS-low pulse width), but currently dead code: neither sx126x_set_sleep() nor sx126x_wakeup() (this function's only caller path) is invoked anywhere in this codebase.
- **Connects to:** sx1262_spi_init, sx1262_wait_not_busy; called only via sx126x_wakeup() (vendored driver), which itself has no callers

### 112. `lora_spi_transmit` [global] — L423
**SEVERITY: NONE**
frame_len bounds checked before any truncating cast; every SPI call's return code checked with early-return on failure; tx_busy state set only after full success. timeout_in_ms=0 (no hardware TX watchdog) is a documented, deliberate divergence from the DK, compensated by this board's own software poll-count watchdog in sx1262_rx_poll - intentional, not an oversight.
- **Connects to:** sx1262_spi_init, sx126x_set_standby, sx126x_set_lora_pkt_params, sx126x_write_buffer, sx126x_set_tx; sets state read by lora_radio_tx_busy and sx1262_rx_poll

### 113. `lora_radio_tx_busy` [global] — L483
**SEVERITY: NONE**
Trivial volatile accessor, no concurrency hazard given the confirmed single-task model.
- **Connects to:** comms_transport_dispatch_pending

### 114. `sx1262_dio1_is_asserted` [static] — L513
**SEVERITY: NONE**
Single GPIO read gating the more expensive SPI round trip; confirmed no interrupt-vector registration anywhere for DIO1 - genuinely polled, matching the DK's model.
- **Connects to:** sx1262_rx_poll

### 115. `sx1262_enter_rx` [global] — L535
**SEVERITY: MEDIUM**
Both underlying calls' return codes discarded via (void). If either fails, the radio is silently left not actually listening, with zero diagnostic and no retry - called both at boot and after every TX completion/timeout, so a single failed call could silently deafen the radio for the rest of the run.
- **Connects to:** sx1262_spi_init, sx126x_set_standby, sx126x_set_rx_with_timeout_in_rtc_step; called by sx1262_hal_init, sx1262_rx_poll

### 116. `sx1262_rx_poll` [global] — L562
**SEVERITY: NONE**
TX-busy watchdog increment correctly placed before the DIO1 early-return; buffer-length bounds check correctly precedes the actual buffer read; IRQ bits always cleared once observed; RX only re-armed on the correct branch given this configuration's mutually-exclusive RX_DONE vs TX_DONE/Timeout semantics (continuous RX, no RX timeout possible, TX timeout=0 so no TX timeout possible either).
- **Connects to:** sx126x_get_irq_status, sx126x_get_rx_buffer_status, sx126x_read_buffer, lora_frame_try_parse, comms_transport_deliver_received, sx126x_clear_irq_status, sx1262_enter_rx, sx1262_dio1_is_asserted

### 117. `sx1262_hal_init` [global] — L699
**SEVERITY: MEDIUM**
Every configuration call in the one-time bring-up sequence discards its return code via (void). A failure during bring-up (plausible given this file's own header notes pin assignments are still placeholders) leaves the radio incompletely configured with zero diagnostic emitted anywhere - silent total-comms failure, hard to diagnose. Same pattern present in the DK's own bring-up (ensure_radio_configured).
- **Connects to:** sx1262_spi_init, sx126x_hal_reset, sx126x_set_dio_irq_params, sx126x_set_pkt_type, sx126x_set_rf_freq, sx126x_set_buffer_base_address, sx126x_set_lora_mod_params, sx126x_set_lora_pkt_params, sx126x_set_lora_sync_word, sx126x_set_tx_params, sx1262_enter_rx; called once from main_board.c

**Divergence check vs DK's sx1262_hal.c:** differs substantially in register-level specifics as expected (PL022/libbsp GPIO here vs CMSIS direct-register on the DK), plus one deliberately-documented behavioral difference (TX timeout 0 vs 1000ms, compensated by a software watchdog here). The RXEN/TXEN opcode-snoop logic, DIO1-polled model, and continuous-RX policy are functionally identical between boards. The one substantive UNINTENTIONAL divergence: sx126x_hal_wakeup() (finding #24) - the DK's version has a delay cycle and pre-emptive BUSY wait this board's version omits.


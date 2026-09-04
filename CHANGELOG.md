# Changelog

## Real SX1262 RX wired up on both boards (DK + Pico) - integration pass

One logical milestone landed as two separate, independently-written
board prompts (DK: `board/STM32N6570-DK/sx1262_hal.c` +
`main_board.c`; Pico: `board/Raspberry_Pi_Pico/sx1262_hal.c` +
`main_board.c`), each owning only its own board's files. This entry
covers both together, plus the integration/documentation pass that
followed once both had landed.

- Both boards now poll DIO1 (`sx1262_poll_rx()` on the DK,
  `sx1262_rx_poll()` on the Pico - same contract, different name per
  board) for `IrqMask`/`Dio1Mask` `0x0203` (TxDone | RxDone |
  Timeout), clear exactly the observed IRQ status (not a hardcoded
  mask), and on RxDone parse and hand the frame to
  `comms_transport_deliver_received()` with the same argument shape
  and rc handling on both boards (rc==1 → deliver, rc==0/-1 → drop).
  Both also now define a strong (non-weak) `lora_spi_transmit()`,
  overriding `core/comms_transport.c`'s weak stub, so TX is wired to
  real hardware on both boards as well, not just RX.
- **Confirmed side by side, not assumed identical**: the two boards'
  independently-written RX-poll code matches on the IRQ mask, the
  IRQ-clear method, and the `comms_transport_deliver_received()`
  call shape, per the checks above.
- **Real, non-cosmetic inconsistency found and flagged back, not
  silently fixed here**: the two boards re-enter RX mode differently
  after handling a packet. The DK uses continuous RX mode
  (`SX126X_RX_CONTINUOUS`) and only re-arms on the TxDone/Timeout
  branch, relying on the radio to keep listening on its own after
  RxDone. The Pico uses single-shot RX mode and unconditionally
  re-arms after every DIO1 event, including RxDone. Both boards end
  up listening again afterward, but via different mechanisms and
  different justifying comments - this crosses the DK/Pico
  file-ownership boundary, so it's reported here for whoever owns
  those board prompts to reconcile (or explicitly confirm as an
  acceptable divergence), not resolved unilaterally in this pass.
- **Pico's RXEN/TXEN RF-switch wiring is unconfirmed against real
  hardware** - this was previously recorded only as a code comment in
  `board/Raspberry_Pi_Pico/sx1262_hal.c` ("RXEN/TXEN note"). Promoted
  to `ARCHITECTURE.md`'s Open Architectural Questions so it isn't
  lost/missed.
- Each board prompt shipped its own standalone selftest under
  `laptop/tools/`: `sx1262_rx_selftest_dk.c` and
  `sx126x_rx_selftest_pico.c`. Both built and run per their own
  build-comment recipe for this integration pass - `sx1262_rx_selftest:
  all tests passed` and `sx126x_rx_selftest: all tests passed`
  respectively.
- Doc/comment accuracy pass, now that real HALs exist on both boards:
  - `laptop/tools/two_board_harness.c` and
    `laptop/tools/two_board_cbba_harness.c`'s own "Does NOT prove"
    header comments previously said "no SX1262 HAL exists yet" - now
    stale, since both exist. Fixed to say what's actually still
    unproven by those harnesses: they talk over a Unix socket, not
    SPI/DIO1/a real radio, so they still exercise neither board's
    HAL - the harnesses' actual scope was already correct, only the
    stated justification had gone stale.
  - `core/vendor/sx126x_driver/README.md`'s project-specific section
    (below the vendored Semtech README) previously said "neither
    [HAL] exists yet anywhere in this repo" and left RXEN/TXEN
    driving "NOT YET DECIDED" - updated to reflect that both HALs now
    exist and that RXEN/TXEN driving is decided at the code level
    (opcode-snooped, MCU-driven) even though the Pico's physical
    wiring is still unconfirmed (see above).
  - Both boards' `main_board.c` had an identical "known gap (nothing
    sends/receives real handshake frames yet, so this currently
    blocks forever on real hardware)" comment predating this RX work.
    Comment-only fix on both: the gap is closed at the code level (RX
    now feeds `comms_transport`'s inbox, TX has a strong
    `lora_spi_transmit()`), but a full handshake round trip has not
    been exercised on real silicon, since neither board has ever been
    flashed.
- Verified together: `make check-board`, `make check-pico` (both real
  BSP mode, full compile + partial-link, clean on both), `make smoke`,
  and `make check-two-board-harness` (real cross-process handshake +
  LoRa-relayed roster delivery) all pass with both boards' RX work
  landed together - the first point where a real inconsistency
  between the two independently-written implementations could have
  surfaced at build/link time. None did.
- **`make check-two-board-cbba-harness` showed intermittent failures**
  (1-3 out of 40 runs, reproduced across multiple independent runs) -
  investigated and determined to be unrelated to this RX work: that
  harness communicates over a Unix socket and never links either
  board's `sx1262_hal.c` or `main_board.c`, so this pass cannot be
  its cause. The failing runs show a genuine consensus-convergence
  race in `core/cbba.c` (a board's local view disagreeing on the
  winning bid) - a pre-existing issue in the CBBA harness itself,
  flagged here rather than silently retried past, but out of scope
  for this integration pass to fix.
- Makefile: no changes needed - both boards' `sx1262_hal.c` and
  `core/vendor/sx126x_driver/src/sx126x.c` were already correctly
  wired into `BOARD_SOURCES`/`PICO_BOARD_SOURCES`/`ARM_CFLAGS` by the
  two board prompts themselves, confirmed by the clean `check-board`/
  `check-pico` results above. The one actual staleness found was a
  doc comment, not a missing target:
  `laptop/tools/check-two-board-cbba-harness.sh` said "No Makefile
  target for this exists yet," but the Makefile already has a
  `check-two-board-cbba-harness` target wired to it - comment fixed
  to match.

## LoRa frame encryption implemented (AEAD_CHACHA20_POLY1305, RFC 8439)

- `core/lora_frame.c`'s envelope (the single choke point every
  `frame_type` — CBBA, mission-link, handshake — already passed
  through) now AEAD-encrypts every frame under a static pre-shared
  key, per the LoRa-encryption reference doc's "pragmatic fallback"
  option (one PSK for the whole contest build, not a per-boot
  session key). New files, all in `core/` so the laptop build's
  `core/*.c` wildcard and both board Makefile targets pick them up
  with no Makefile changes:
  - `core/chacha20.c`/`.h` — RFC 8439 IETF ChaCha20 (96-bit nonce,
    32-bit counter).
  - `core/poly1305.c`/`.h` — RFC 8439 Poly1305, 26-bit-limb ("donna"
    style) algorithm ported to fixed-width `uint32_t`/`uint64_t`
    rather than `unsigned long`/`unsigned long long` (the original's
    types are 32-bit on the Cortex-M targets but 64-bit on the
    x86-64 laptop host — this algorithm's carry logic assumes a
    specific limb width, so pinning it removes that platform
    ambiguity rather than relying on it happening to still work).
  - `core/aead_chacha20poly1305.c`/`.h` — the RFC 8439 §2.8 AEAD
    construction combining the two.
  - `core/lora_crypto.c`/`.h` — LoRa-specific policy: the static PSK,
    per-sender TX counter, 12-byte nonce construction, and RX-side
    16-bit-wire-fragment counter reconstruction (mirrors LoRaWAN's
    own frame-counter scheme, per the reference doc).
- Written in-repo rather than vendoring a library (the reference doc
  flagged `cifra`/HACL*/ST's X-CUBE-CRYPTOLIB as candidates) —
  avoids validating an external dependency's freestanding/license fit
  across two different Cortex-M targets for the time available.
- **Bug caught by cross-verification, not caught by inspection**: the
  first `poly1305.c` pass had an inverted mask in the final `h`-vs-`p`
  reduction (`mask = 0u - bit` instead of `mask = bit - 1u`), which
  silently produced wrong tags. Found by cross-checking against
  Python's `cryptography` library and an independent reference
  implementation — not by re-reading the code. All RFC 8439 test
  vectors, 50 randomized AEAD-seal vectors against `cryptography`'s
  `ChaCha20Poly1305`, and open()/tamper-rejection tests now pass
  exactly. Recorded here per this project's own "empirical
  verification over assertion" norm — a bug this easy to introduce
  and this easy to miss by inspection alone is exactly the case that
  norm exists for.
- **Deliberate, documented deviation from the reference doc's 165-byte
  wire sketch**: added one `sender_board_id` AAD byte (166 bytes
  total, not 165). The doc's counter/nonce sketch didn't work through
  what happens when multiple boards share one static PSK — without a
  per-sender nonce partition, two boards could reach the same 32-bit
  counter value and produce an identical `(key, nonce)` pair, a
  catastrophic AEAD break (plaintext recovery + tag forgery), not a
  theoretical one. `sender_board_id` is folded into both the AAD
  (so a receiver knows whose counter-state slot to check) **and**
  the actual nonce fed to ChaCha20 (`core/lora_crypto.c`'s
  `lora_crypto_build_nonce()` — byte 0 of the 12-byte nonce), which
  is the part that actually separates the keystreams; AAD alone would
  only have authenticated the claim without preventing the collision.
  Verified directly: `lora_frame_selftest.c` packs the same payload
  at the same counter value under two different `sender_board_id`s
  and asserts the ciphertext differs, not just the header byte.
- This resolves the reference doc's "bootstrap paradox" concern for
  free: with a static PSK (not one derived from the handshake),
  `MissionLinkHandshakeReq_t`/`Reply_t` need no special-casing — they
  ride the same encrypted envelope as every other `frame_type`.
- Also recomputed and updated `core/comms_transport.c`'s Semtech
  AN1200.13 LoRa time-on-air calibration
  (`COMMS_TRANSPORT_CHANNEL_BYTES_PER_TICK`), which was derived
  against the old 147-byte max frame size and had gone stale: now
  166 bytes → 248 symbols → 266.496 ms → ~22424 bytes/tick (was
  21968). Re-derived with the same formula the original comment
  used, not hand-waved.
- **New: `make check-board-ids-distinct`** (wired into both
  `check-board` and `check-pico`, so it runs by default) — closes a
  gap found during review: the per-board nonce partition above only
  holds if every board's `THIS_BOARD_ID` is actually unique, and
  nothing previously checked that across the two boards' separate
  compile recipes (`-DTHIS_BOARD_ID=0` for DK, `=1` for Pico, in two
  different Makefile rules — no `_Static_assert` can span two
  separate translation units built for two separate targets). The
  new target asks each real compiler (`arm-none-eabi-gcc -dM -E`)
  what `THIS_BOARD_ID` actually resolves to and fails loudly on a
  collision; verified it both passes today (DK=0, Pico=1) and
  actually fails when a real collision is forced through it (not
  just asserted to fail — an earlier draft of this check hard-coded
  the comparison values and would have silently always passed,
  caught and fixed before landing).
- Also documented, next to `lora_crypto_build_nonce()`'s own layout
  comment: the 256-sender ceiling this scheme's one-byte
  `sender_board_id` field sets (nowhere near a real constraint at
  this project's 2-board scale, but a real limit worth having
  written down rather than implicit), and that board-id uniqueness
  is an assumption this crypto layer depends on but cannot itself
  enforce — `check-board-ids-distinct` above is the actual guarantee.
- **Known, deliberate, documented tradeoff** (not hidden): because the
  PSK is static but each board's TX counter resets to 0 on reboot, a
  frame captured before a reboot is technically replayable
  immediately after. Accepted for this contest build per the
  reference doc's own "pragmatic fallback" guidance; a real
  deployment would need the doc's "full solution" (root-secret +
  per-boot-derived session key) to close this gap.
- Verified together, not just individually: `make check-board` and
  `make check-pico` (real BSP mode, full partial-link, both clean),
  `make smoke` (200-tick full sim through the encrypted envelope
  every send), `make check-two-board-harness` (real cross-process
  handshake + encrypted LoRa relay), `make check-two-board-cbba-harness`
  (40/40 runs, this project's own flakiness bar), and the
  `cbba_board_selftest`/`override_selftest`/`mission_link_selftest`
  standalone selftests — all pass clean against the new envelope.
- `laptop/tools/lora_frame_selftest.c` rewritten for the new
  signatures/envelope: adds AAD-tampering and replay-rejection cases
  alongside the pre-existing corruption/oversized/incomplete-buffer
  coverage.

## Two-board communication harness finished (Track 3 complete)

- `laptop/tools/two_board_harness.c` now passes reliably (100+
  consecutive runs, zero failures) after fixing three real bugs found
  by actually running it, not by inspection:
  1. Injecting the test roster at iteration 0 let it race ahead of
     board 0's own first Req to board 1 (real `mission_link_task_
     step()` call-order behavior). Fixed by having the harness wait
     for board 1's confirmed readiness before injecting, mirroring
     what the real laptop's `HandshakeCoordinator` already does.
  2. A fixed-iteration-count version of that wait was flaky (7/8 runs
     failed) — two `fork()`ed processes' loop iterations have no
     real-time relationship to each other.
  3. Polling `mission_link_handshake_get_relay_reply()` directly
     failed 30/30 — that function consults the shared timeout clock
     (`cbba_now_ms()`, no "peek" mode, every call advances it), and
     polling it from a second call site inflated board 0's own clock
     ~3x relative to board 1's, causing a spurious sticky timeout
     every run.
- New `core/mission_link_handshake.h`/`.c` function:
  `mission_link_handshake_relay_reply_seen(uint8_t peer_board_id)` —
  a plain array read, no `now` parameter, no clock side effects at
  all. Added specifically to fix bug 3 above; purely additive, no
  existing function's signature or behavior changed.
- `HARNESS_MAX_ITERATIONS` raised from 2000 to 30000 — a real passing
  run lands around iteration 10000-12000 (confirmed empirically
  across many runs), and 2000 left too little margin for two
  asynchronous processes' actual progress.
- The file's own top comment now has a "Debugging history" section
  recording all three root causes, so the reasoning isn't lost.
- New `check-two-board-harness` Makefile target — builds and runs it
  for real, non-zero exit on failure. Re-verified alongside every
  other check (`check-board`, `check-pico`, `check-pico-bsp`,
  `check-sx126x-driver`, laptop build, both handshake selftests) —
  all clean together.

## SX1262 driver core vendored (first piece of the actual radio driver)

- Real hardware correction, confirmed with the user before vendoring
  anything: the actual module is a **SmartElex SX1262 868M LoRa HAT**
  (E22-900M22S), not the Waveshare Core1262-HF from the original
  hardware handoff doc. This matters beyond naming — the real module
  has RXEN/TXEN RF-switch control pins the original plan never
  mentioned, which the eventual per-board HAL will need to account
  for (driven by the MCU, or tied to DIO2 — not yet decided).
- Vendored `core/vendor/sx126x_driver/` — Semtech's official SX126X
  driver (https://github.com/Lora-net/sx126x_driver), Clear BSD
  licensed (redistribution-friendly, unlike either BSP — see
  `core/vendor/README.md` for why this changes where/how it's
  vendored vs. the two BSP folders). Lives under `core/`, not either
  board's own `vendor/`, because it's genuinely **shared** — the
  driver core is meant to be identical on both boards; only the HAL
  underneath differs. `core/vendor/sx126x_driver/src/*.c` doesn't
  match `core/*.c`'s wildcard, so the laptop build never links it.
- Confirmed, not assumed: genuinely freestanding (no `stdio.h`/
  `malloc`/`printf` anywhere in the vendored source), and every one of
  `sx126x.c`'s 74 public functions delegates entirely to four HAL
  functions (`sx126x_hal_write`/`_read`/`_reset`/`_wakeup`) declared
  in `sx126x_hal.h` — the driver core never touches SPI/GPIO directly,
  confirmed by reading `sx126x.c` itself, not by trusting the header's
  own doc comment alone.
- New `check-sx126x-driver` Makefile target: compiles `sx126x.c`
  freestanding for BOTH real target CPUs (`-mcpu=cortex-m55` and
  `-mcpu=cortex-m0plus`), clean. Deliberately NOT added to
  `check-board`/`check-pico`'s own `BOARD_SOURCES`/`PICO_BOARD_
  SOURCES` yet — doing that before a real per-board HAL exists would
  make those targets' link checks correctly fail on "undefined
  reference to sx126x_hal_write" (nothing defines those symbols
  anywhere yet), which would look like a regression rather than the
  honest, expected state of an unfinished driver. This target exists
  to keep proving the driver core itself is sound in the meantime.
- Re-verified: laptop build, `check-board`, `check-pico`,
  `check-pico-bsp`, and the new `check-sx126x-driver` all re-run clean
  together.
- What's genuinely still blocking either board's actual HAL
  implementation, documented in `core/vendor/sx126x_driver/README.md`'s
  "What's still genuinely undecided" section rather than guessed at:
  GPIO pin assignments for both boards (the datasheet's own wiring
  table is for a Raspberry Pi single-board computer running Linux,
  which doesn't transfer to either of this project's actual bare-metal
  targets) and the RXEN/TXEN handling decision.

## Handshake generalized from a hardcoded 2-board pair to N boards

- No 3rd board is actually planned for this project (confirmed with
  the user before doing this) — this is a design-smell fix, not a
  feature needed for a concrete future board. Driven by a single
  configurable knob per language, matching the request: `core/
  mission_link_handshake.h`'s `HANDSHAKE_MAX_BOARDS` (compile-time
  `-D`, default 8, a static-array sizing cap, not the real board
  count) and `laptop/tools/telemetry_bridge.py`'s
  `HandshakeCoordinator.NUM_BOARDS` (runtime constant, default 2 — the
  real board count for this project today). Neither number appears
  anywhere else — comms_transport.c's existing `COMMS_TRANSPORT_TEAMS_
  PER_BOARD` (already configurable) combined with `NUM_TEAMS` is still
  the actual source of "how many boards", now exposed as a proper
  function (`comms_transport_board_count()`, new) instead of being
  re-derived ad-hoc in more than one place — `mission_link_task.c`'s
  incident-relay loop (pre-existing, both boards) switched to call it
  too, closing a related duplication this generalization surfaced.
- `core/mission_link_handshake.h`/`.c`: `should_send_nucleo_req`/
  `nucleo_timed_out`/`get_relay_reply` — all singular, hardcoded to
  peer board_id=1 — renamed to `should_send_relay_req`/`relay_timed_
  out`/`get_relay_reply`, all now taking a `peer_board_id` parameter,
  with per-peer state converted from single variables to arrays
  indexed by board_id (capped at `HANDSHAKE_MAX_BOARDS`).
  `note_remote_reply`'s signature is unchanged — it already
  self-identifies the peer from the reply's own `board_id` field, so
  no parameter was needed there.
- Both boards' `mission_link_task.c` (`board/STM32N6570-DK/` and
  `board/Raspberry_Pi_Pico/`, kept identical): the handshake TX step's
  DK-only section now loops `peer` over `comms_transport_board_
  count()` instead of hardcoding board 1 — sending each peer's Req,
  and forwarding each peer's reply to the laptop as its own distinct
  frame (the wire format already self-identifies which board each
  reply is from, so no format change was needed to carry N peers'
  worth of status). Dedup-send state (`g_relay_reply_sent`/`_last_
  sent`) converted from single variables to per-peer arrays.
- `laptop/tools/telemetry_bridge.py`'s `HandshakeCoordinator`:
  `board1_ready`/`board1_timed_out` (named, singular) replaced with
  `board_ready(board_id)`/`board_timed_out(board_id)` plus
  `peer_board_ids` (derived from `NUM_BOARDS`). `all_ready`,
  `request_dk_only_override()` (now refuses unless EVERY currently-
  not-ready peer has genuinely timed out, not just one), and
  `status_dict()` (now `peer_ready`/`peer_timed_out` dicts keyed by
  board_id, replacing the old single-peer-only fields) generalized to
  match. Checked first that nothing in `app.py` or `templates/
  index.html` depended on the old field names before changing the
  JSON shape — confirmed `app.py` only ever passes `status_dict()`
  through opaquely and the frontend template doesn't consume any of
  these fields yet, so this was a clean rename, not a breaking change
  to something already wired up.
- New tests proving the generalization itself, not just that a single
  peer still works through the new signatures: `handshake_selftest.c`'s
  `test_relay_state_is_independent_per_peer()` (two C-side peers'
  retry/reply/timeout state genuinely independent) and
  `handshake_selftest.py`'s `test_multi_peer_independence()` (same
  proof on the Python side, with `NUM_BOARDS` bumped to 3 for the
  test). Both existing single-peer tests kept and renamed to match the
  new API, still passing — proving today's actual 2-board behavior is
  unchanged, not just that the generalized code compiles.
- Re-verified for real, not assumed: laptop build, `check-board`,
  `check-pico`, `check-pico-bsp`, the C `handshake_selftest`, the
  Python `handshake_selftest.py` (9/9 tests), and
  `telemetry_bridge_selftest.py` (unaffected, confirmed) all re-run
  clean after every change in this entry.

## Board folder restructure: laptop folder (step 5 — the last piece)

- Moved `platform/laptop/{main.c, debug.c, snapshot.c}` to a new
  top-level `laptop/`, and `tools/` to `laptop/tools/` (broader scope
  than the board moves, by request — `tools/` is unambiguously
  laptop-only per `ARCHITECTURE.md`'s own existing section header, so
  it folds in cleanly; top-level docs stay where they are since they
  cover the whole project, board targets included, not just the
  laptop). `platform/` no longer exists at all — this was the last
  thing in it. `core/` untouched, same reasoning as both board moves:
  genuinely shared, moving it would break the "each target owns what's
  specific to it" principle kept throughout this whole restructure.
- Every literal `platform/laptop` and `tools/` path reference
  repo-wide updated — including inside `laptop/tools/`'s own files
  (their self-referential build-command comments, e.g. "gcc ...
  tools/override_selftest.c"), which a first pass missed by only
  fixing references pointing AT the moved folders, not references
  FROM inside them — caught by a follow-up sweep, not by assuming the
  first pass was complete.
- `Makefile`'s `TOOLS_DIR` also fixed by hand (`TOOLS_DIR = tools`,
  no trailing slash, so it wasn't touched by the literal `tools/`
  substring replacement — different case from every other reference,
  worth noting since it's the kind of miss that's easy to repeat).
  Top-of-file layout comment block rewritten for accuracy while in
  there — also removed stale pre-existing text on the `board/
  STM32N6570-DK/` line ("Stubs pending BSP (contest-provided, not the
  public mtk3_bsp repo)") that predated this session's actual DK BSP
  work and no longer matched reality.
- Re-verified for real, not assumed: laptop build (`tron` +
  `label_snapshots`), `check-board`, `check-pico`, and `check-pico-bsp`
  all re-run clean after the move; `handshake_selftest` (now under
  `laptop/tools/`) rebuilt and re-run, still passes.

## Board folder restructure: per-device `board/<name>/` layout (step 4 — Pico project sources)

- Added `board/Raspberry_Pi_Pico/{main_board.c, mission_link_task.c,
  debug.c, snapshot.c}`, ported from `board/STM32N6570-DK/`'s
  equivalents. Genuinely mechanical for most of it: confirmed neither
  BSP's actual STM32 HAL calls appear in any of the four DK files
  (only portable `tk_*`/`tm_*` µT-Kernel API + `core/` headers), and
  confirmed `tk_cre_tsk`/`tk_sta_tsk`/`tk_slp_tsk`/`tk_dly_tsk` and
  `T_CTSK` are byte-for-byte identical between the v1 (Pico) and v2
  (DK) BSPs by diffing both headers directly — closing an open
  question flagged since this session's very first message.
- Added `THIS_BOARD_ID` — a compile-time `-D`, resolved once in each
  board's `main_board.c` at startup, setting `g_comms_transport_this_
  board_id` and calling `comms_transport_init()` before anything that
  depends on either. This closes a real gap that existed for **both**
  boards, not just Pico: `g_comms_transport_this_board_id` has had a
  single hardcoded `0u` initializer in `core/comms_transport.c` since
  it was introduced, and nothing anywhere ever actually set it
  per-board or called `comms_transport_init()` — confirmed by
  grepping for callers before writing this. DK now builds with
  `-DTHIS_BOARD_ID=0`, Pico with `-DTHIS_BOARD_ID=1`.
- Three things rewritten by hand rather than mechanically ported,
  because they don't carry over as-is:
  - `main_board.c`'s top-of-file bring-up comment: rewritten around
    the real Eclipse CDT project + boot chain (`Reset_Handler` ->
    `knl_startup_hw()` -> `usermain()`, no CubeMX-generated `main.c`
    to inject a start call into, no TrustZone split) instead of the
    STM32CubeIDE/TrustZone/`knl_start_mtkernel()` steps that don't
    apply to this BSP at all.
  - The `TA_FPU` task-attribute comment: RP2040 is dual Cortex-M0+,
    confirmed no FPU at all (`USE_FPU (0)` in this BSP's own
    `config/config.h`) — rewritten rather than carrying over the DK's
    `ALWAYS_FPU_ATR` reasoning, which doesn't apply.
  - `debug.c`'s "`tm_printf` doesn't support floats" claim:
    re-verified directly against `lib/libtm/libtm_printf.c`'s own
    source (states "Unsupported specifiers: floating point..."
    outright, no `'f'` case in its format-specifier switch) rather
    than trusting the DK's citation of its own BSP's manual to also
    hold for a different BSP.
- New `check-pico`/`check-pico-compile`/`check-pico-link` Makefile
  targets, mirroring `check-board-compile`/`-link` exactly
  (`-mcpu=cortex-m0plus` in place of `-mcpu=cortex-m55`). Run for
  real: full `core/` + all four `board/Raspberry_Pi_Pico/*.c` files
  compile clean, 30-object partial link clean, zero cross-file
  interface breaks. `check-pico-bsp` (the smaller, BSP-only smoke
  check from step 3) kept alongside it, useful for isolating "the
  vendored BSP is broken" from "our port broke something."
- Re-verified `check-board`, `check-pico`, `check-pico-bsp`, and the
  laptop build all together, all clean.
- Scope NOT covered by this step, same honest caveat as the DK's own
  file states about itself: **not yet wired into a real, flashable
  firmware project.** Compiling/link-checking cleanly is not the same
  as building a flashable image — that still needs the BSP's own
  Eclipse CDT project (`board/Raspberry_Pi_Pico/vendor/mtk3_bsp/
  .project`) to actually include `core/` and `board/Raspberry_Pi_
  Pico/`'s sources in its build settings, which is IDE work, not
  something done from this Makefile. Concretely simpler than the DK's
  equivalent step, though (no TrustZone split, no manual entry-point
  injection needed) — see `main_board.c`'s own rewritten header
  comment for specifics.

## Board folder restructure: per-device `board/<name>/` layout (step 3 — Pico BSP)

- Added `board/Raspberry_Pi_Pico/vendor/mtk3_bsp/` — the real
  `pico_rp2040` branch of `github.com/tron-forum/mtk3_bsp` (v1 BSP,
  NOT `mtk3_bsp2` which the DK uses — confirmed genuinely different
  repos, not branches of one repo, by actually cloning both), `.git`
  stripped, matching `board/STM32N6570-DK/vendor/mtk3_bsp2/`'s
  existing precedent of vendoring the real tree rather than only
  documenting fetch steps.
- Verified for real, not assumed from docs: cloned the branch
  cleanly, then actually compiled the BSP's own bundled sample
  (`app_program/app_main.c`) against its own headers with
  `arm-none-eabi-gcc -mcpu=cortex-m0plus -D_PICO_RP2040_` — clean,
  only two harmless pre-existing unused-parameter warnings in the
  vendor's own sample code. New `make check-pico-bsp` target runs
  this same check going forward; ran clean.
- This corrects two assumptions the original handoff doc made
  without being able to verify them at the time: (1) the Pico's real
  build path is an Eclipse CDT managed-build project (`.project`/
  `.cproject`, confirmed present on the real branch), not the
  community `mtk3-pico-gmake` Makefile+`elf2uf2` fork that was the
  best available reference before this; (2) this BSP has no
  `pico-sdk` dependency at all (self-contained, embeds its own RP2040
  stage-2 boot bytes) — see `board/Raspberry_Pi_Pico/vendor/
  README.md` for the full corrected picture, including that this
  BSP (like the DK's `mtk3_bsp2`) has no SPI driver at all, so the
  SX1262 HAL's SPI half will need raw register access on both boards,
  not a vendor SDK call.
- Scope of THIS step: BSP only, matching step 1's scope for the DK.
  No `board/Raspberry_Pi_Pico/*.c` project sources exist yet (no
  main-loop file, no SX1262 HAL port) — `check-pico-bsp` deliberately
  only checks the vendored BSP itself, not a `check-board`-equivalent
  compile+link of project code, since there is no project code for
  this target yet.

## Board folder restructure: per-device `board/<name>/` layout (step 2)

- Moved `platform/board/{main_board.c, mission_link_task.c, debug.c,
  snapshot.c}` into `board/STM32N6570-DK/` directly (not nested under
  another `platform/board/` inside it) — the last piece flagged as
  "not moved yet" at the end of step 1 below. `platform/board/` is
  now gone entirely; `platform/` holds only `laptop/`.
- Every literal `platform/board` path reference repo-wide (comments,
  `Makefile`'s `BOARD_DIR`, `ARCHITECTURE.md`, `CHANGELOG.md`,
  `README.md`, several `core/*.h`/`.c` doc comments, `laptop/tools/*_selftest.c`
  comments, and `tron_board/Appli/.project`/`.cproject`'s linked-
  resource entries) updated to `board/STM32N6570-DK` via a single
  literal-substring replacement — safe here because "platform/board"
  never appears as a partial match inside an unrelated word or path
  anywhere in this repo (checked, not assumed).
- `board/STM32N6570-DK/` is now genuinely self-contained: its own
  source files (`main_board.c`, `mission_link_task.c`, `debug.c`,
  `snapshot.c`) alongside its own `vendor/mtk3_bsp2/` — matches the
  "choose that board's folder to flash into the board" goal step 1
  only partially achieved.
- Re-verified with real tool runs, not assumed: `make check-board`
  (compile + link, Cortex-M55, real BSP2 headers) and `make` (laptop
  build) both pass clean against the new paths with zero `BSP_DIR`/
  `BOARD_DIR` override needed; `laptop/tools/handshake_selftest.c` re-run and
  still passes (unaffected — desktop-only, doesn't touch this path).
- `tron_board/Appli/.cproject`'s include-path entries were included in
  the same sed pass as `.project`'s linked-resource entry — same
  "unverified in actual STM32CubeIDE, re-index/rebuild before
  trusting" caveat as step 1's `vendor/mtk3_bsp2` path fix below.

## Board folder restructure: per-device `board/<name>/` layout (step 1)

- Moved the top-level `vendor/mtk3_bsp2/` (and its `README.md`) to
  `board/STM32N6570-DK/vendor/mtk3_bsp2/`. `Makefile`'s `BSP_DIR`
  default, `ARCHITECTURE.md`, and every in-repo comment referencing
  the old path updated to match; `make check-board`/`make
  check-board-compile`/`make check-board-link` re-verified clean
  against the new location (real Cortex-M55 cross-compile + link, not
  just a path-rename sanity check).
- Purpose: each physical board target is moving toward owning a
  self-contained `board/<name>/` folder (BSP, and eventually its own
  build files) rather than sharing one top-level `vendor/`, so a
  board's folder alone has what's needed to build/flash it. A
  Raspberry Pi Pico target will get `board/Raspberry_Pi_Pico/vendor/
  mtk3_bsp` (v1, NOT the v2 BSP STM32N6570-DK uses) alongside this one
  once that port exists.
- Scope of THIS step: the BSP move only. `board/STM32N6570-DK/*.c`
  (`main_board.c`, `mission_link_task.c`, `debug.c`, `snapshot.c`) has
  NOT moved and still targets STM32N6570-DK only — whether/how those
  also move under `board/STM32N6570-DK/` is a separate follow-up.
- `tron_board/Appli/.project`'s linked-resource entry AND
  `.cproject`'s 12 include-path entries for `vendor/mtk3_bsp2` were
  both updated (plain literal-substring replacement, same old path
  string everywhere in both files) to point at the new
  `board/STM32N6570-DK/vendor/mtk3_bsp2` path — this was NOT verified
  by actually opening the project in STM32CubeIDE (no CubeIDE
  available in this environment). Open the project and let the IDE
  re-index/rebuild before trusting it. The generated `Debug/` build
  tree (`subdir.mk`, `.o`/`.d`/`.su`/`.cyclo` files) was deliberately
  left untouched — those are IDE-regenerated build artifacts from a
  previous compile against the OLD path, not something to hand-patch;
  a clean rebuild inside CubeIDE replaces them.
  hand-patch.

## Merge: tron_merged_step11 (Dashboard: Incident Journal, PAR Timer, Map Visualization)

Reconciled a sibling branch (11 separate merge/feature steps in another
session) that forked before the fog-of-war/search-zone merge and the
gateway-hang, CSV-sentinel, stale-read, fault-compounding, and
active-mask fixes documented below - confirmed via a file-by-file
`diff -u` against the true original, not assumed. Kept every one of
those fixes; layered in what was genuinely new on the other side.

### Merged (real, independent work - not present in this branch before)

- `incident.c` - two related `RETURN_TO_SAFE_ZONE` duplicate-task
  fixes: `create_return_to_safe_zone_task()` now checks for an
  existing pending task before creating a new one, and
  `abandon_task_for_recovery()` short-circuits if the team's current
  task is already a `RETURN_TO_SAFE_ZONE` task (previously it would
  clear the assignment and immediately recreate an equivalent task,
  destroying the evidence the dedup check needed).
- `g_diag_stage_completions[NUM_TEAMS]` - new counter tracking every
  stage handoff (not just final incident resolution), incremented in
  `release_main_task_team()`. Threaded through `main.c`'s DIAG
  THROUGHPUT summary and `debug_emit_live_frame()`'s LIVEFRAME output.
- `debug_emit_static_environment()` - new one-shot STATICENV JSON
  frame (buildings, epicenter) sent once per `--live` run rather than
  every tick, since that layout doesn't change after world generation.
- `hazard_cells` array in LIVEFRAME - streams only cells that are
  actually blocked/hazardous/damaged (not all 1296 grid cells) for
  live hazard-map rendering.
- `app.py` - `STATICENV` SSE line handling (dispatches to a
  `static_env` message type alongside the existing `frame` type).
  Also fixed a stale comment describing team-name indexing that no
  longer matched the actual `% len(TEAM_NAMES)` code beneath it.
- `templates/index.html` - taken as the new file wholesale after
  confirming (via the full diff) that all removed lines were
  superseded restructuring, not lost functionality unique to this
  branch. Adds: Incident Journal tab, PAR (Personnel Accountability
  Report) timer per team, static building/epicenter map rendering,
  hazard-cell density-tinted overlay, incident type icons, a
  preview-only drag-to-reassign UI (explicitly not wired to a live
  backend channel - documented as future work in the source), and a
  Chart.js update-in-place optimization replacing destroy/recreate
  every frame.

### Fixed as part of reconciling the port

- `debug_trace()` changed from writing to stdout to stderr. Necessary,
  not cosmetic: `app.py`'s batch (non-`--live`) endpoint captures
  stdout/stderr *separately* and only parses `result.stderr` for
  `[DIAG ...]` lines. The new rest-cycle tracing this merge adds uses
  `debug_trace()`, and a naive stdout implementation would have
  silently dropped that data for every non-live run while appearing to
  work fine in `--live` mode (which merges the streams). Verified
  against both code paths, not just live mode.

### Explicitly rejected (confirmed real regressions in the other
### branch, superseded by fixes already in this one)

- `recovery_manager_config.h` / `test_metrics.c` - reverted time
  constants (`RECOVERY_MANAGER_DEESCALATION_TICKS`,
  `STALL_THRESHOLD_TICKS`) to their pre-fix bare-integer values.
- `fault_injector.c` - reverted `active_mask` clearing, the
  wrong-incident-targeting fallback fix, and the fault
  reference-counting fix.
- `incident.c` / `snapshot.c` - lacked the gateway-hang fix and the
  CSV sentinel / stale-read fixes entirely.
- Fog-of-war/search-zone system absent entirely (`search_zones.c/h`,
  `discovered` flag, related `environment.c`/`incident.c` support) -
  confirmed via the merged session's own file timestamps that this
  branch forked before that work existed, not that it was removed.

### Verified

- Multi-seed sweep (1-15) clean, no hangs.
- CSV sentinel and stale-read fixes both still hold post-merge.
- `make check-board` passes against real BSP2 headers.
- Live SSE stream exercised end-to-end through the actual Flask
  server (not just the raw binary): confirmed `STATICENV` emits real
  building/epicenter data, `LIVEFRAME` carries both
  `stage_completions` and populated `hazard_cells`, and `[DIAG]`
  lines route correctly to `diag_line` SSE events.
- Merged `templates/index.html`'s script block is brace/paren-balanced
  (337/337, 515/515).

## Merge: Search-Zone / Fog-of-War System + Stale-Read CSV Fix

### Merged

- Merged a separately-developed session's work: `search_zones.c/h`
  (district-based fog-of-war and Search-team sweep dispatch), a
  fault-tracking refactor (`mission_fault_active_count` on
  `IncidentState_t` replacing per-fault-class `original_priority[]`/
  `original_victim_count[]` on `TeamFaultState_t`, fixing a
  compounding bug where a second mission fault landing on an
  already-faulted incident would capture the wrong "original" value to
  restore), and 3 new CSV telemetry columns (`incident_discovered`,
  `team_zone_id`, `team_zone_trust`).
- Kept `NUM_TEAMS=10` (the merged session's own baseline used 5); this
  is a real, verified difference in assumptions between the two
  branches, not a leftover default.
- Kept this branch's existing `--live` dashboard implementation as-is
  where the two branches independently built overlapping
  functionality; dashboard-adjacent discrepancies are expected and
  out of scope for this merge (a separately improved dashboard exists
  elsewhere).
- Verified the specific scenario the merged session's own notes
  flagged as never tested - two teams sharing the same specialization
  (Search) under `NUM_TEAMS=10` - actually works: both swept
  different, overlapping-but-not-identical zone sets over a 2000-tick
  run with no deadlock.

### Fixed

- **Stale team-state read in `EVENT_INCIDENT_RESOLVED` snapshot
  logging** (`snapshot.c`, `complete_task()` in `incident.c`): by the
  time a task's completion is detected and logged, the team it was
  assigned to can already have been dispatched to a new task earlier
  in the same tick (`allocate_all_incidents()` runs before
  `simulation_step()`'s completion detection). `snapshot_record()`
  derived incident-detail columns from the team's *live* state, so
  the logged row described the team's *next* assignment instead of
  the one that just resolved. Rare before this merge (2/68 resolved-task
  log rows affected in a 2000-tick run) but became the near-universal
  case afterward (88/88), almost certainly because the new search-zone
  system makes many more system tasks available for immediate
  same-tick redispatch. Fixed by adding `snapshot_record_resolved()`,
  which takes the resolving task's own state directly rather than
  re-deriving it from the team - `complete_task()` now uses this
  instead of the general `snapshot_record()`. Verified: post-fix, 78/88
  resolved-task rows show real incident IDs and the remaining 10
  correctly correspond to genuine system-task completions (which are
  supposed to show no incident), matching the diag counters exactly.

### Verified

- Multi-seed sweep (1-15) clean, no hangs.
- CSV sentinel fix still holds (zero `4294967295` occurrences).
- `make check-board` passes against real BSP2 headers with the merged
  and fixed sources.

## Bug Fixes: Evacuation Gateway Hang, CSV Sentinel Corruption

### Fixed

- **Multi-seed run hang** (`incident.c`, `initialize_evacuation_gateways()`):
  the deterministic gateway-site lookup (`environment_find_gateway_site()`)
  could return the same colliding cell forever on retry, since it has no
  randomness. Seed 2 (gateway index 1) hung indefinitely. Fixed by
  tracking whether the deterministic candidate was already rejected once
  and falling through to the randomized placement branch on retry.
  Verified: seed 2 now completes instantly; swept seeds 1-10 clean.
- **`cbba_selected_team` showing `4294967295` in the snapshot CSV**
  (`snapshot.c`, `snapshot_record()`): `selected_team` was a signed `int`
  defaulting to sentinel `-1`, but printed with `%u`, reinterpreting the
  bit pattern as `0xFFFFFFFF`. Fixed by using the codebase's existing
  `NO_ASSIGNMENT` (`0xFF`) sentinel convention instead of `-1`, matching
  how `assigned_team`/`assigned_task_slot` are already handled elsewhere.
  Verified: 261 corrupted rows in a 500-tick run before the fix, zero
  after.

## Laptop / Board Platform Split

### Added

- Split the codebase into `core/` (portable simulator, verified
  freestanding-clean for Cortex-M55), `laptop/`, and
  `board/STM32N6570-DK/` (STM32N6570-DK).
- Added `core/sim_runner.h/.c` as the single source of truth for the
  per-tick sequence (`fault_injector_step -> allocate_all_incidents ->
  simulation_step`), shared by both platforms.
- Added `board/STM32N6570-DK/main_board.c`, `debug.c`, `snapshot.c` -
  compiled and verified against the real µT-Kernel 3.0 BSP2 headers
  (`github.com/tron-forum/mtk3_bsp2`, official STM32N6570-DK support).
- Added `make check-board`, which compiles `core/` + `board/STM32N6570-DK/`
  for Cortex-M55 against real BSP headers when `vendor/mtk3_bsp2` is
  present (see `vendor/README.md`), catching libc/OS dependency
  regressions before they ever reach hardware.

### Removed

- Removed the `sweep`/weight-sensitivity-analysis CLI mode and
  `analysis.c`/`analysis.h` (unused dev tooling, not part of the
  production simulator).
- Removed committed run artifacts (logs, CSVs, stale binaries) and
  added `.gitignore` to keep them out going forward.

### Verified

- Rebuilt from a fresh extraction of the packaged tree; 200-tick run
  byte-identical to the pre-restructure original on both stdout and
  stderr.

## Operational Fault Injection and Watchdog Refinement

### Added

- Added a generic operational fault injection subsystem with deterministic fault profiles.
- Added fault-state tracking to teams so the simulator can model mobility, communication, resource, environment, and mission effects.
- Added optional `--fault-profile` CLI support.

### Changed

- The watchdog now emits recommendations from evidence instead of directly mutating CBBA state.
- Simulation applies watchdog recommendations and fault effects while preserving the existing snapshot schema.

### Verified

- Built the simulator and ran a short smoke test.

## Offline Snapshot Labeling Pipeline

### Added

- Added a standalone offline label generator for `tron_snapshot_dataset.csv` that writes a separate labeled CSV.
- Added a horizon-based recovery label that marks whether a team returns to `STATUS_AVAILABLE` within `N` ticks after a snapshot.
- Added recovery metadata columns to the labeled dataset without changing the original snapshot export schema.

### Documented

- Documented the snapshot-labeling methodology, including the run/team grouping rule and the tier-based ready-stamina threshold used as recovery metadata.

### Verified

- Built the simulator and labeler with `make`.

## Procedural Environment Milestone

### Added

- Added persistent damage state to generated buildings, road cells, and infrastructure cells.
- Added construction quality to generated building records.
- Added infrastructure registration for safe zones and evacuation gateways.
- Added a deterministic earthquake event with epicenter, intensity, district vulnerability, construction quality, and distance attenuation.
- Added road blockage state and high-cost blocked-road routing behavior.
- Added object-level spawned-incident masks so damaged objects do not emit the same incident type repeatedly.
- Added environment summary output for earthquake epicenter, intensity, damaged buildings, blocked roads, and damaged infrastructure.

### Changed

- Incident generation now originates from damaged environment objects instead of random coordinates.
- Collapse, confined-space, gas leak, chemical exposure, fire, casualty, survivor, and search incidents now select eligible damaged buildings, roads, or infrastructure sources.
- World setup now registers safe zones and gateways as infrastructure before applying earthquake damage, then initializes the safety grid from the damaged environment.
- Blocked roads now influence routing through high travel cost while remaining reachable for incident response.

### Preserved

- Existing simulator CLI remains `./tron [first_seed] [seed_count] [ticks_per_seed]`.
- Existing `TeamState_t`, `IncidentState_t`, `SafeZone_t`, CBBA bundle/path structures, and snapshot CSV schema remain compatible.
- Existing metrics and snapshot flows continue to observe the same state transitions.
- Teams still move one grid step per simulator tick, preserving current movement semantics.

### Verified

- Built the simulator with `make`.
- Ran the smoke simulation with `make smoke`.

## Initial Procedural Environment Milestone

### Added

- Added a procedural environment subsystem in `environment.c` and `environment.h`.
- Added deterministic district generation for rescue base, residential, industrial, medical, civic, and waterfront districts.
- Added a generated road network with arterial, collector, and local road classes.
- Added typed building records with district and risk metadata.
- Added per-cell environment metadata for base safety, travel cost, safe-zone suitability, and navigability.
- Added environment-aware safe-zone candidate selection.
- Added environment-aware evacuation gateway placement.
- Added environment-aware incident placement with type-specific district preferences.
- Added `find_path_cost(...)` wrapper for route-cost consumers that do not need the next step.
- Added a `Makefile` with `make`, `make smoke`, and `make clean`.
- Added `ARCHITECTURE.md` describing the new subsystem and compatibility boundaries.

### Changed

- `generate_world(...)` now creates the procedural environment before initializing the safety grid.
- `initialize_grid(...)` now initializes each cell from environment base safety instead of a flat value of `100`.
- `find_next_path_step(...)` now uses environment navigability and travel cost while preserving its existing signature.
- CBBA distance scoring now uses weighted route cost through the same path API.
- Return-to-safe-zone selection now uses environment-aware path cost instead of Manhattan distance.
- Generated incidents now land on environment-selected sites instead of uniformly random grid cells.
- Final debug output now includes an environment summary with district, road, navigable-cell, and building counts.

### Preserved

- Existing simulator CLI remains `./tron [first_seed] [seed_count] [ticks_per_seed]`.
- Existing `TeamState_t`, `IncidentState_t`, `SafeZone_t`, CBBA bundle/path structures, and snapshot CSV schema remain compatible.
- Existing metrics and snapshot flows continue to observe the same state transitions.
- Teams still move one grid step per simulator tick, preserving current movement semantics.

### Verified

- Built the simulator with `gcc -std=c11 -Wall -Wextra -Wpedantic`.
- Ran a seeded smoke simulation with `./tron 1 1 200`.


20th August 2026
Merge summary
=============
You uploaded files-2.zip containing 9 files from a separate branch of work
(diagnostics/tracing flags, a fog-of-war "god/operator view" toggle, a
sweep-task anti-thrash cooldown, and a real capability-matrix bug fix).
I diffed every one of them against what I'd already built and merged as
follows:

TRUE MERGES (both sides had changes to reconcile)
--------------------------------------------------
tools__app.py
  - Kept: my handshake gating (/api/status, /api/board/launch,
    /api/board/continue-without-nucleo) and the override route
    (/api/board/override) from this chat.
  - Added: the "--trace" "--diag" CLI flags to both subprocess launch
    points (/api/run and /api/run-live), which is the only real content
    their branch's app.py had (everything else in their version was just
    a pre-handshake baseline, not an intentional removal).

tools_templates__index.html
  - Kept: my real confirmPendingOverride() fetch() implementation.
  - Added: their fog-of-war "god mode / operator view" toggle (button,
    toggleFogView(), and the two incident-filtering call sites) - this
    only works together with the "discovered" field debug.c now emits
    (see below), so both had to come over together.

ADOPTED AS-IS (files I never touched - no merge needed, just verified
they don't conflict with anything I built: no shared-struct changes to
CBBAAgentState_t/TeamState_t/IncidentState_t, no mission_link/comms_
transport touches)
-----------------------------------------------------------------------
platform_laptop__debug.c   -> laptop/debug.c
  Adds "discovered" to the JSON incident/system-task output - required
  by index.html's fog-of-war toggle.

platform_laptop__main.c    -> laptop/main.c
  New --trace / --diag CLI flags (gates g_simulation_trace_enabled and
  the end-of-run [DIAG ...] stderr summary, both off by default).

core__scoring.c            -> core/scoring.c
  Fixes a real bug: CAPABILITY_MATRIX's row/column order didn't match
  the actual Specialization_t enum (SEARCH=0, MEDICAL=1, ...) - Search
  and Medical had swapped data. Re-derived from the original intended
  semantics, not new numbers.

core__simulation.c         -> core/simulation.c
core__search_zones.h       -> core/search_zones.h
core__search_zones.c       -> core/search_zones.c
core__incident.c           -> core/incident.c
  Diagnostic counters (g_diag_discovered_by_type etc.) and a new
  ZONE_SWEEP_RECLAIM_COOLDOWN_TICKS / reclaimed_at_tick field on
  SearchZoneState_t to stop a create/evict thrash loop.

Verification performed after merging
-------------------------------------
- Full laptop build: clean, no errors.
- All core/board files that touch mission-link work re-checked with
  host gcc against the real BSP headers (-fsyntax-only): clean.
- All 5 standalone test suites re-run against the merged sources:
  handshake_selftest.c, mission_link_selftest.c, lora_frame_selftest.c,
  override_selftest.c (rebuilt against the merged core/scoring.c,
  core/incident.c etc.), handshake_selftest.py, telemetry_bridge_
  selftest.py - all pass.
- index.html's merged JS re-parsed for syntax errors: clean, both
  features (override fetch + fog toggle) confirmed present.
- A real ./tron run with --trace --diag confirmed the new flags actually
  produce output.

Filenames below use "__" in place of "/" - map back to the real path
using the arrow.

NEW FILES (from this chat, unaffected by the merge)
-----------------------------------------------------
core__mission_link_codec.h          -> core/mission_link_codec.h
core__mission_link.c                -> core/mission_link.c
platform_board__mission_link_task.c -> board/STM32N6570-DK/mission_link_task.c
tools__mission_link_selftest.c      -> laptop/tools/mission_link_selftest.c
core__mission_link_handshake.h      -> core/mission_link_handshake.h
core__mission_link_handshake.c      -> core/mission_link_handshake.c
tools__handshake_selftest.c         -> laptop/tools/handshake_selftest.c
tools__handshake_selftest.py        -> laptop/tools/handshake_selftest.py
tools__override_selftest.c          -> laptop/tools/override_selftest.c
tools__telemetry_bridge.py          -> laptop/tools/telemetry_bridge.py

MERGED FILES (see above)
---------------------------
tools__app.py                       -> laptop/tools/app.py
tools_templates__index.html         -> laptop/tools/templates/index.html

EDITED (Makefile only, unrelated to this merge)
--------------------------------------------------
Makefile                            -> Makefile

ADOPTED AS-IS FROM files-2.zip (see above)
---------------------------------------------
platform_laptop__debug.c            -> laptop/debug.c
platform_laptop__main.c             -> laptop/main.c
core__scoring.c                     -> core/scoring.c
core__simulation.c                  -> core/simulation.c
core__search_zones.h                -> core/search_zones.h
core__search_zones.c                -> core/search_zones.c
core__incident.c                    -> core/incident.c

NOT included (already correct, never touched by me or files-2.zip):
core/mission_link.h, core/mission_link_override.h, core/mission_link_
override.c, core/mission_state.h, core/cbba.c,
core/WIRE_PROTOCOL_MAGIC_BYTES.md.


Second upload (files.zip) - genuine merge conflict
=====================================================
This one was the real thing: files.zip contained core/mission_state.h and
core/incident.c from a branch that ALSO started before the override work
landed on CBBAAgentState_t - exactly the collision the original override
prompt's own coordination note warned about ("if another change also adds
a field here around the same time, reconcile field order explicitly
rather than letting one silently clobber the other").

Their mission_state.h had two genuine, unrelated additions:
  - WORK_RATE_MIN/WORK_RATE_MAX (1-8, replacing a hardcoded 1-5) - a
    tuned constant with a detailed sensitivity-sweep justification
    comment (cuts total task volume 39%, safe-zone fallback failures 91%)
  - NUM_SYSTEM_TASKS bumped 10 -> 20

...but their version of the file had DROPPED the override_active /
override_incident_id fields and the _Static_assert entirely - not an
intentional removal, just an artifact of branching before those existed.
Taking their file as-is would have silently broken core/cbba.c's
cbba_apply_override_pin() and core/mission_link_override.c's apply
function (both reference those fields directly) - compile failure, not
a subtle bug, but still exactly the kind of clobber the coordination
note was trying to prevent.

Merge performed: took their file as the base (their tuning work is real
and correct), re-inserted the override_active/override_incident_id
fields and _Static_assert in the same place as before, added a "MERGE
NOTE" explaining why, and re-ran the sizeof() probe rather than assuming
the old 216-byte number still held (it does - neither of their changes
affects this struct's layout: WORK_RATE_MIN/MAX are plain #defines,
NUM_SYSTEM_TASKS only sizes an unrelated array).

incident.c's one change (hardcoded 1,5 -> WORK_RATE_MIN/WORK_RATE_MAX)
depends on mission_state.h's new constants and had no other conflicts -
adopted as-is.

Verified NUM_SYSTEM_TASKS=20 doesn't break anything: grepped every call
site across the codebase (debug.c, allocator.c, cbba.c, team.c,
incident.c, recovery_manager.c, mission_execution_twin.c,
test_metrics.c) - every one already uses the macro, not a hardcoded 10,
so the array simply got bigger with no code changes needed.

Re-ran the full verification pass after this merge, override_selftest.c
in particular (the test most directly dependent on the re-merged
fields): full build clean, board syntax checks clean, all 5 standalone
test suites pass, and a live smoke run confirms NUM_SYSTEM_TASKS=20 is
in effect (system task slot IDs now go past 10).

core__mission_state.h               -> core/mission_state.h  (merged)
core__incident.c                    -> core/incident.c        (updated,
                                        replaces the earlier core__incident.c)



# Changelog

## Team-5 (SEARCH specialization) permanent idle deadlock fixed

- Reported as "second search team not working on any task after it
  reaches around 1k ticks". Confirmed and reproduced exactly via
  `./tron --diag <seed> 1 2000`: team 5 (the second `SPEC_SEARCH`
  team, since `generate_teams()` assigns `specialization = i % 5`)
  showed `resting=29 available=1136` against every other team's
  `resting=700-1100+ available=49-130` range, consistently across
  seeds 1-5.
- Root cause, confirmed via a temporary status-transition trace (not
  guessed from inspection): a team in `STATUS_AVAILABLE` is only
  eligible for new work if `stamina >= get_soft_stamina_floor(team)`
  and `equipment_level > RECOVERY_MANAGER_EQUIPMENT_CRITICAL_
  THRESHOLD` (see `team_can_accept_incident()`, `core/allocator.c`).
  But `update_team()`'s `STATUS_AVAILABLE` case was a no-op, and only
  `STATUS_RESTING` actually recovers stamina/equipment. Multiple call
  sites across the codebase set a team directly to `STATUS_AVAILABLE`
  with no eligibility check whenever its current task disappears out
  from under it — `team_set_available()` itself is unconditional by
  design, and `core/incident.c`'s `search_sweep_step()` (a SEARCH
  team's sweep cycle completing) and `core/simulation.c`'s
  `move_team()` (task vanished mid-travel) both bypass even that and
  assign `team->status = STATUS_AVAILABLE` directly. Team 5 hit this
  via `search_sweep_step()`: landed on `AVAILABLE` at stamina 24.50
  against a 26.0 floor, and simply never changed status again for the
  rest of a 2000-tick run — too weak to be picked up, never resting to
  recover.
- Fixed in `core/team.c`:
  1. `update_team()`'s `STATUS_AVAILABLE` case now checks the same
     eligibility bar `team_can_accept_incident()` uses, and routes an
     ineligible team to `create_return_to_safe_zone_task()` (falling
     back to forcing `STATUS_RESTING` directly — which recovers
     stamina/equipment unconditionally regardless of location in this
     simulator's model — if even that fails). This is the one choke
     point that closes the gap for all current and future call sites
     that can land a team on `AVAILABLE`, rather than chasing down
     each one individually.
  2. The `STATUS_ON_SCENE` "task vanished" branch now routes through
     `abandon_task_for_recovery()` instead of calling
     `team_set_available()` directly, matching every other
     task-loss path in the same function.
- Re-verified with `--diag` across seeds 1-5 (2000 ticks) and a 5000-
  tick run on seed 1: team 5's `available`/`resting` numbers land in
  the same range as every other team in every run, including no
  late-onset stall at the longer tick count.

## Two-board communication harness finished (Track 3 complete)

- `laptop/tools/two_board_harness.c` now passes reliably (100+
  consecutive runs, zero failures) after fixing three real bugs found
  by actually running it, not by inspection:
  1. Injecting the test roster at iteration 0 let it race ahead of
     board 0's own first Req to board 1 (real `mission_link_task_
     step()` call-order behavior). Fixed by having the harness wait
     for board 1's confirmed readiness before injecting, mirroring
     what the real laptop's `HandshakeCoordinator` already does.
  2. A fixed-iteration-count version of that wait was flaky (7/8 runs
     failed) — two `fork()`ed processes' loop iterations have no
     real-time relationship to each other.
  3. Polling `mission_link_handshake_get_relay_reply()` directly
     failed 30/30 — that function consults the shared timeout clock
     (`cbba_now_ms()`, no "peek" mode, every call advances it), and
     polling it from a second call site inflated board 0's own clock
     ~3x relative to board 1's, causing a spurious sticky timeout
     every run.
- New `core/mission_link_handshake.h`/`.c` function:
  `mission_link_handshake_relay_reply_seen(uint8_t peer_board_id)` —
  a plain array read, no `now` parameter, no clock side effects at
  all. Added specifically to fix bug 3 above; purely additive, no
  existing function's signature or behavior changed.
- `HARNESS_MAX_ITERATIONS` raised from 2000 to 30000 — a real passing
  run lands around iteration 10000-12000 (confirmed empirically
  across many runs), and 2000 left too little margin for two
  asynchronous processes' actual progress.
- The file's own top comment now has a "Debugging history" section
  recording all three root causes, so the reasoning isn't lost.
- New `check-two-board-harness` Makefile target — builds and runs it
  for real, non-zero exit on failure. Re-verified alongside every
  other check (`check-board`, `check-pico`, `check-pico-bsp`,
  `check-sx126x-driver`, laptop build, both handshake selftests) —
  all clean together.

## SX1262 driver core vendored (first piece of the actual radio driver)

- Real hardware correction, confirmed with the user before vendoring
  anything: the actual module is a **SmartElex SX1262 868M LoRa HAT**
  (E22-900M22S), not the Waveshare Core1262-HF from the original
  hardware handoff doc. This matters beyond naming — the real module
  has RXEN/TXEN RF-switch control pins the original plan never
  mentioned, which the eventual per-board HAL will need to account
  for (driven by the MCU, or tied to DIO2 — not yet decided).
- Vendored `core/vendor/sx126x_driver/` — Semtech's official SX126X
  driver (https://github.com/Lora-net/sx126x_driver), Clear BSD
  licensed (redistribution-friendly, unlike either BSP — see
  `core/vendor/README.md` for why this changes where/how it's
  vendored vs. the two BSP folders). Lives under `core/`, not either
  board's own `vendor/`, because it's genuinely **shared** — the
  driver core is meant to be identical on both boards; only the HAL
  underneath differs. `core/vendor/sx126x_driver/src/*.c` doesn't
  match `core/*.c`'s wildcard, so the laptop build never links it.
- Confirmed, not assumed: genuinely freestanding (no `stdio.h`/
  `malloc`/`printf` anywhere in the vendored source), and every one of
  `sx126x.c`'s 74 public functions delegates entirely to four HAL
  functions (`sx126x_hal_write`/`_read`/`_reset`/`_wakeup`) declared
  in `sx126x_hal.h` — the driver core never touches SPI/GPIO directly,
  confirmed by reading `sx126x.c` itself, not by trusting the header's
  own doc comment alone.
- New `check-sx126x-driver` Makefile target: compiles `sx126x.c`
  freestanding for BOTH real target CPUs (`-mcpu=cortex-m55` and
  `-mcpu=cortex-m0plus`), clean. Deliberately NOT added to
  `check-board`/`check-pico`'s own `BOARD_SOURCES`/`PICO_BOARD_
  SOURCES` yet — doing that before a real per-board HAL exists would
  make those targets' link checks correctly fail on "undefined
  reference to sx126x_hal_write" (nothing defines those symbols
  anywhere yet), which would look like a regression rather than the
  honest, expected state of an unfinished driver. This target exists
  to keep proving the driver core itself is sound in the meantime.
- Re-verified: laptop build, `check-board`, `check-pico`,
  `check-pico-bsp`, and the new `check-sx126x-driver` all re-run clean
  together.
- What's genuinely still blocking either board's actual HAL
  implementation, documented in `core/vendor/sx126x_driver/README.md`'s
  "What's still genuinely undecided" section rather than guessed at:
  GPIO pin assignments for both boards (the datasheet's own wiring
  table is for a Raspberry Pi single-board computer running Linux,
  which doesn't transfer to either of this project's actual bare-metal
  targets) and the RXEN/TXEN handling decision.

## Handshake generalized from a hardcoded 2-board pair to N boards

- No 3rd board is actually planned for this project (confirmed with
  the user before doing this) — this is a design-smell fix, not a
  feature needed for a concrete future board. Driven by a single
  configurable knob per language, matching the request: `core/
  mission_link_handshake.h`'s `HANDSHAKE_MAX_BOARDS` (compile-time
  `-D`, default 8, a static-array sizing cap, not the real board
  count) and `laptop/tools/telemetry_bridge.py`'s
  `HandshakeCoordinator.NUM_BOARDS` (runtime constant, default 2 — the
  real board count for this project today). Neither number appears
  anywhere else — comms_transport.c's existing `COMMS_TRANSPORT_TEAMS_
  PER_BOARD` (already configurable) combined with `NUM_TEAMS` is still
  the actual source of "how many boards", now exposed as a proper
  function (`comms_transport_board_count()`, new) instead of being
  re-derived ad-hoc in more than one place — `mission_link_task.c`'s
  incident-relay loop (pre-existing, both boards) switched to call it
  too, closing a related duplication this generalization surfaced.
- `core/mission_link_handshake.h`/`.c`: `should_send_nucleo_req`/
  `nucleo_timed_out`/`get_relay_reply` — all singular, hardcoded to
  peer board_id=1 — renamed to `should_send_relay_req`/`relay_timed_
  out`/`get_relay_reply`, all now taking a `peer_board_id` parameter,
  with per-peer state converted from single variables to arrays
  indexed by board_id (capped at `HANDSHAKE_MAX_BOARDS`).
  `note_remote_reply`'s signature is unchanged — it already
  self-identifies the peer from the reply's own `board_id` field, so
  no parameter was needed there.
- Both boards' `mission_link_task.c` (`board/STM32N6570-DK/` and
  `board/Raspberry_Pi_Pico/`, kept identical): the handshake TX step's
  DK-only section now loops `peer` over `comms_transport_board_
  count()` instead of hardcoding board 1 — sending each peer's Req,
  and forwarding each peer's reply to the laptop as its own distinct
  frame (the wire format already self-identifies which board each
  reply is from, so no format change was needed to carry N peers'
  worth of status). Dedup-send state (`g_relay_reply_sent`/`_last_
  sent`) converted from single variables to per-peer arrays.
- `laptop/tools/telemetry_bridge.py`'s `HandshakeCoordinator`:
  `board1_ready`/`board1_timed_out` (named, singular) replaced with
  `board_ready(board_id)`/`board_timed_out(board_id)` plus
  `peer_board_ids` (derived from `NUM_BOARDS`). `all_ready`,
  `request_dk_only_override()` (now refuses unless EVERY currently-
  not-ready peer has genuinely timed out, not just one), and
  `status_dict()` (now `peer_ready`/`peer_timed_out` dicts keyed by
  board_id, replacing the old single-peer-only fields) generalized to
  match. Checked first that nothing in `app.py` or `templates/
  index.html` depended on the old field names before changing the
  JSON shape — confirmed `app.py` only ever passes `status_dict()`
  through opaquely and the frontend template doesn't consume any of
  these fields yet, so this was a clean rename, not a breaking change
  to something already wired up.
- New tests proving the generalization itself, not just that a single
  peer still works through the new signatures: `handshake_selftest.c`'s
  `test_relay_state_is_independent_per_peer()` (two C-side peers'
  retry/reply/timeout state genuinely independent) and
  `handshake_selftest.py`'s `test_multi_peer_independence()` (same
  proof on the Python side, with `NUM_BOARDS` bumped to 3 for the
  test). Both existing single-peer tests kept and renamed to match the
  new API, still passing — proving today's actual 2-board behavior is
  unchanged, not just that the generalized code compiles.
- Re-verified for real, not assumed: laptop build, `check-board`,
  `check-pico`, `check-pico-bsp`, the C `handshake_selftest`, the
  Python `handshake_selftest.py` (9/9 tests), and
  `telemetry_bridge_selftest.py` (unaffected, confirmed) all re-run
  clean after every change in this entry.

## Board folder restructure: laptop folder (step 5 — the last piece)

- Moved `platform/laptop/{main.c, debug.c, snapshot.c}` to a new
  top-level `laptop/`, and `tools/` to `laptop/tools/` (broader scope
  than the board moves, by request — `tools/` is unambiguously
  laptop-only per `ARCHITECTURE.md`'s own existing section header, so
  it folds in cleanly; top-level docs stay where they are since they
  cover the whole project, board targets included, not just the
  laptop). `platform/` no longer exists at all — this was the last
  thing in it. `core/` untouched, same reasoning as both board moves:
  genuinely shared, moving it would break the "each target owns what's
  specific to it" principle kept throughout this whole restructure.
- Every literal `platform/laptop` and `tools/` path reference
  repo-wide updated — including inside `laptop/tools/`'s own files
  (their self-referential build-command comments, e.g. "gcc ...
  tools/override_selftest.c"), which a first pass missed by only
  fixing references pointing AT the moved folders, not references
  FROM inside them — caught by a follow-up sweep, not by assuming the
  first pass was complete.
- `Makefile`'s `TOOLS_DIR` also fixed by hand (`TOOLS_DIR = tools`,
  no trailing slash, so it wasn't touched by the literal `tools/`
  substring replacement — different case from every other reference,
  worth noting since it's the kind of miss that's easy to repeat).
  Top-of-file layout comment block rewritten for accuracy while in
  there — also removed stale pre-existing text on the `board/
  STM32N6570-DK/` line ("Stubs pending BSP (contest-provided, not the
  public mtk3_bsp repo)") that predated this session's actual DK BSP
  work and no longer matched reality.
- Re-verified for real, not assumed: laptop build (`tron` +
  `label_snapshots`), `check-board`, `check-pico`, and `check-pico-bsp`
  all re-run clean after the move; `handshake_selftest` (now under
  `laptop/tools/`) rebuilt and re-run, still passes.

## Board folder restructure: per-device `board/<name>/` layout (step 4 — Pico project sources)

- Added `board/Raspberry_Pi_Pico/{main_board.c, mission_link_task.c,
  debug.c, snapshot.c}`, ported from `board/STM32N6570-DK/`'s
  equivalents. Genuinely mechanical for most of it: confirmed neither
  BSP's actual STM32 HAL calls appear in any of the four DK files
  (only portable `tk_*`/`tm_*` µT-Kernel API + `core/` headers), and
  confirmed `tk_cre_tsk`/`tk_sta_tsk`/`tk_slp_tsk`/`tk_dly_tsk` and
  `T_CTSK` are byte-for-byte identical between the v1 (Pico) and v2
  (DK) BSPs by diffing both headers directly — closing an open
  question flagged since this session's very first message.
- Added `THIS_BOARD_ID` — a compile-time `-D`, resolved once in each
  board's `main_board.c` at startup, setting `g_comms_transport_this_
  board_id` and calling `comms_transport_init()` before anything that
  depends on either. This closes a real gap that existed for **both**
  boards, not just Pico: `g_comms_transport_this_board_id` has had a
  single hardcoded `0u` initializer in `core/comms_transport.c` since
  it was introduced, and nothing anywhere ever actually set it
  per-board or called `comms_transport_init()` — confirmed by
  grepping for callers before writing this. DK now builds with
  `-DTHIS_BOARD_ID=0`, Pico with `-DTHIS_BOARD_ID=1`.
- Three things rewritten by hand rather than mechanically ported,
  because they don't carry over as-is:
  - `main_board.c`'s top-of-file bring-up comment: rewritten around
    the real Eclipse CDT project + boot chain (`Reset_Handler` ->
    `knl_startup_hw()` -> `usermain()`, no CubeMX-generated `main.c`
    to inject a start call into, no TrustZone split) instead of the
    STM32CubeIDE/TrustZone/`knl_start_mtkernel()` steps that don't
    apply to this BSP at all.
  - The `TA_FPU` task-attribute comment: RP2040 is dual Cortex-M0+,
    confirmed no FPU at all (`USE_FPU (0)` in this BSP's own
    `config/config.h`) — rewritten rather than carrying over the DK's
    `ALWAYS_FPU_ATR` reasoning, which doesn't apply.
  - `debug.c`'s "`tm_printf` doesn't support floats" claim:
    re-verified directly against `lib/libtm/libtm_printf.c`'s own
    source (states "Unsupported specifiers: floating point..."
    outright, no `'f'` case in its format-specifier switch) rather
    than trusting the DK's citation of its own BSP's manual to also
    hold for a different BSP.
- New `check-pico`/`check-pico-compile`/`check-pico-link` Makefile
  targets, mirroring `check-board-compile`/`-link` exactly
  (`-mcpu=cortex-m0plus` in place of `-mcpu=cortex-m55`). Run for
  real: full `core/` + all four `board/Raspberry_Pi_Pico/*.c` files
  compile clean, 30-object partial link clean, zero cross-file
  interface breaks. `check-pico-bsp` (the smaller, BSP-only smoke
  check from step 3) kept alongside it, useful for isolating "the
  vendored BSP is broken" from "our port broke something."
- Re-verified `check-board`, `check-pico`, `check-pico-bsp`, and the
  laptop build all together, all clean.
- Scope NOT covered by this step, same honest caveat as the DK's own
  file states about itself: **not yet wired into a real, flashable
  firmware project.** Compiling/link-checking cleanly is not the same
  as building a flashable image — that still needs the BSP's own
  Eclipse CDT project (`board/Raspberry_Pi_Pico/vendor/mtk3_bsp/
  .project`) to actually include `core/` and `board/Raspberry_Pi_
  Pico/`'s sources in its build settings, which is IDE work, not
  something done from this Makefile. Concretely simpler than the DK's
  equivalent step, though (no TrustZone split, no manual entry-point
  injection needed) — see `main_board.c`'s own rewritten header
  comment for specifics.

## Board folder restructure: per-device `board/<name>/` layout (step 3 — Pico BSP)

- Added `board/Raspberry_Pi_Pico/vendor/mtk3_bsp/` — the real
  `pico_rp2040` branch of `github.com/tron-forum/mtk3_bsp` (v1 BSP,
  NOT `mtk3_bsp2` which the DK uses — confirmed genuinely different
  repos, not branches of one repo, by actually cloning both), `.git`
  stripped, matching `board/STM32N6570-DK/vendor/mtk3_bsp2/`'s
  existing precedent of vendoring the real tree rather than only
  documenting fetch steps.
- Verified for real, not assumed from docs: cloned the branch
  cleanly, then actually compiled the BSP's own bundled sample
  (`app_program/app_main.c`) against its own headers with
  `arm-none-eabi-gcc -mcpu=cortex-m0plus -D_PICO_RP2040_` — clean,
  only two harmless pre-existing unused-parameter warnings in the
  vendor's own sample code. New `make check-pico-bsp` target runs
  this same check going forward; ran clean.
- This corrects two assumptions the original handoff doc made
  without being able to verify them at the time: (1) the Pico's real
  build path is an Eclipse CDT managed-build project (`.project`/
  `.cproject`, confirmed present on the real branch), not the
  community `mtk3-pico-gmake` Makefile+`elf2uf2` fork that was the
  best available reference before this; (2) this BSP has no
  `pico-sdk` dependency at all (self-contained, embeds its own RP2040
  stage-2 boot bytes) — see `board/Raspberry_Pi_Pico/vendor/
  README.md` for the full corrected picture, including that this
  BSP (like the DK's `mtk3_bsp2`) has no SPI driver at all, so the
  SX1262 HAL's SPI half will need raw register access on both boards,
  not a vendor SDK call.
- Scope of THIS step: BSP only, matching step 1's scope for the DK.
  No `board/Raspberry_Pi_Pico/*.c` project sources exist yet (no
  main-loop file, no SX1262 HAL port) — `check-pico-bsp` deliberately
  only checks the vendored BSP itself, not a `check-board`-equivalent
  compile+link of project code, since there is no project code for
  this target yet.

## Board folder restructure: per-device `board/<name>/` layout (step 2)

- Moved `platform/board/{main_board.c, mission_link_task.c, debug.c,
  snapshot.c}` into `board/STM32N6570-DK/` directly (not nested under
  another `platform/board/` inside it) — the last piece flagged as
  "not moved yet" at the end of step 1 below. `platform/board/` is
  now gone entirely; `platform/` holds only `laptop/`.
- Every literal `platform/board` path reference repo-wide (comments,
  `Makefile`'s `BOARD_DIR`, `ARCHITECTURE.md`, `CHANGELOG.md`,
  `README.md`, several `core/*.h`/`.c` doc comments, `laptop/tools/*_selftest.c`
  comments, and `tron_board/Appli/.project`/`.cproject`'s linked-
  resource entries) updated to `board/STM32N6570-DK` via a single
  literal-substring replacement — safe here because "platform/board"
  never appears as a partial match inside an unrelated word or path
  anywhere in this repo (checked, not assumed).
- `board/STM32N6570-DK/` is now genuinely self-contained: its own
  source files (`main_board.c`, `mission_link_task.c`, `debug.c`,
  `snapshot.c`) alongside its own `vendor/mtk3_bsp2/` — matches the
  "choose that board's folder to flash into the board" goal step 1
  only partially achieved.
- Re-verified with real tool runs, not assumed: `make check-board`
  (compile + link, Cortex-M55, real BSP2 headers) and `make` (laptop
  build) both pass clean against the new paths with zero `BSP_DIR`/
  `BOARD_DIR` override needed; `laptop/tools/handshake_selftest.c` re-run and
  still passes (unaffected — desktop-only, doesn't touch this path).
- `tron_board/Appli/.cproject`'s include-path entries were included in
  the same sed pass as `.project`'s linked-resource entry — same
  "unverified in actual STM32CubeIDE, re-index/rebuild before
  trusting" caveat as step 1's `vendor/mtk3_bsp2` path fix below.

## Board folder restructure: per-device `board/<name>/` layout (step 1)

- Moved the top-level `vendor/mtk3_bsp2/` (and its `README.md`) to
  `board/STM32N6570-DK/vendor/mtk3_bsp2/`. `Makefile`'s `BSP_DIR`
  default, `ARCHITECTURE.md`, and every in-repo comment referencing
  the old path updated to match; `make check-board`/`make
  check-board-compile`/`make check-board-link` re-verified clean
  against the new location (real Cortex-M55 cross-compile + link, not
  just a path-rename sanity check).
- Purpose: each physical board target is moving toward owning a
  self-contained `board/<name>/` folder (BSP, and eventually its own
  build files) rather than sharing one top-level `vendor/`, so a
  board's folder alone has what's needed to build/flash it. A
  Raspberry Pi Pico target will get `board/Raspberry_Pi_Pico/vendor/
  mtk3_bsp` (v1, NOT the v2 BSP STM32N6570-DK uses) alongside this one
  once that port exists.
- Scope of THIS step: the BSP move only. `board/STM32N6570-DK/*.c`
  (`main_board.c`, `mission_link_task.c`, `debug.c`, `snapshot.c`) has
  NOT moved and still targets STM32N6570-DK only — whether/how those
  also move under `board/STM32N6570-DK/` is a separate follow-up.
- `tron_board/Appli/.project`'s linked-resource entry AND
  `.cproject`'s 12 include-path entries for `vendor/mtk3_bsp2` were
  both updated (plain literal-substring replacement, same old path
  string everywhere in both files) to point at the new
  `board/STM32N6570-DK/vendor/mtk3_bsp2` path — this was NOT verified
  by actually opening the project in STM32CubeIDE (no CubeIDE
  available in this environment). Open the project and let the IDE
  re-index/rebuild before trusting it. The generated `Debug/` build
  tree (`subdir.mk`, `.o`/`.d`/`.su`/`.cyclo` files) was deliberately
  left untouched — those are IDE-regenerated build artifacts from a
  previous compile against the OLD path, not something to hand-patch;
  a clean rebuild inside CubeIDE replaces them.
  hand-patch.

## Merge: tron_merged_step11 (Dashboard: Incident Journal, PAR Timer, Map Visualization)

Reconciled a sibling branch (11 separate merge/feature steps in another
session) that forked before the fog-of-war/search-zone merge and the
gateway-hang, CSV-sentinel, stale-read, fault-compounding, and
active-mask fixes documented below - confirmed via a file-by-file
`diff -u` against the true original, not assumed. Kept every one of
those fixes; layered in what was genuinely new on the other side.

### Merged (real, independent work - not present in this branch before)

- `incident.c` - two related `RETURN_TO_SAFE_ZONE` duplicate-task
  fixes: `create_return_to_safe_zone_task()` now checks for an
  existing pending task before creating a new one, and
  `abandon_task_for_recovery()` short-circuits if the team's current
  task is already a `RETURN_TO_SAFE_ZONE` task (previously it would
  clear the assignment and immediately recreate an equivalent task,
  destroying the evidence the dedup check needed).
- `g_diag_stage_completions[NUM_TEAMS]` - new counter tracking every
  stage handoff (not just final incident resolution), incremented in
  `release_main_task_team()`. Threaded through `main.c`'s DIAG
  THROUGHPUT summary and `debug_emit_live_frame()`'s LIVEFRAME output.
- `debug_emit_static_environment()` - new one-shot STATICENV JSON
  frame (buildings, epicenter) sent once per `--live` run rather than
  every tick, since that layout doesn't change after world generation.
- `hazard_cells` array in LIVEFRAME - streams only cells that are
  actually blocked/hazardous/damaged (not all 1296 grid cells) for
  live hazard-map rendering.
- `app.py` - `STATICENV` SSE line handling (dispatches to a
  `static_env` message type alongside the existing `frame` type).
  Also fixed a stale comment describing team-name indexing that no
  longer matched the actual `% len(TEAM_NAMES)` code beneath it.
- `templates/index.html` - taken as the new file wholesale after
  confirming (via the full diff) that all removed lines were
  superseded restructuring, not lost functionality unique to this
  branch. Adds: Incident Journal tab, PAR (Personnel Accountability
  Report) timer per team, static building/epicenter map rendering,
  hazard-cell density-tinted overlay, incident type icons, a
  preview-only drag-to-reassign UI (explicitly not wired to a live
  backend channel - documented as future work in the source), and a
  Chart.js update-in-place optimization replacing destroy/recreate
  every frame.

### Fixed as part of reconciling the port

- `debug_trace()` changed from writing to stdout to stderr. Necessary,
  not cosmetic: `app.py`'s batch (non-`--live`) endpoint captures
  stdout/stderr *separately* and only parses `result.stderr` for
  `[DIAG ...]` lines. The new rest-cycle tracing this merge adds uses
  `debug_trace()`, and a naive stdout implementation would have
  silently dropped that data for every non-live run while appearing to
  work fine in `--live` mode (which merges the streams). Verified
  against both code paths, not just live mode.

### Explicitly rejected (confirmed real regressions in the other
### branch, superseded by fixes already in this one)

- `recovery_manager_config.h` / `test_metrics.c` - reverted time
  constants (`RECOVERY_MANAGER_DEESCALATION_TICKS`,
  `STALL_THRESHOLD_TICKS`) to their pre-fix bare-integer values.
- `fault_injector.c` - reverted `active_mask` clearing, the
  wrong-incident-targeting fallback fix, and the fault
  reference-counting fix.
- `incident.c` / `snapshot.c` - lacked the gateway-hang fix and the
  CSV sentinel / stale-read fixes entirely.
- Fog-of-war/search-zone system absent entirely (`search_zones.c/h`,
  `discovered` flag, related `environment.c`/`incident.c` support) -
  confirmed via the merged session's own file timestamps that this
  branch forked before that work existed, not that it was removed.

### Verified

- Multi-seed sweep (1-15) clean, no hangs.
- CSV sentinel and stale-read fixes both still hold post-merge.
- `make check-board` passes against real BSP2 headers.
- Live SSE stream exercised end-to-end through the actual Flask
  server (not just the raw binary): confirmed `STATICENV` emits real
  building/epicenter data, `LIVEFRAME` carries both
  `stage_completions` and populated `hazard_cells`, and `[DIAG]`
  lines route correctly to `diag_line` SSE events.
- Merged `templates/index.html`'s script block is brace/paren-balanced
  (337/337, 515/515).

## Merge: Search-Zone / Fog-of-War System + Stale-Read CSV Fix

### Merged

- Merged a separately-developed session's work: `search_zones.c/h`
  (district-based fog-of-war and Search-team sweep dispatch), a
  fault-tracking refactor (`mission_fault_active_count` on
  `IncidentState_t` replacing per-fault-class `original_priority[]`/
  `original_victim_count[]` on `TeamFaultState_t`, fixing a
  compounding bug where a second mission fault landing on an
  already-faulted incident would capture the wrong "original" value to
  restore), and 3 new CSV telemetry columns (`incident_discovered`,
  `team_zone_id`, `team_zone_trust`).
- Kept `NUM_TEAMS=10` (the merged session's own baseline used 5); this
  is a real, verified difference in assumptions between the two
  branches, not a leftover default.
- Kept this branch's existing `--live` dashboard implementation as-is
  where the two branches independently built overlapping
  functionality; dashboard-adjacent discrepancies are expected and
  out of scope for this merge (a separately improved dashboard exists
  elsewhere).
- Verified the specific scenario the merged session's own notes
  flagged as never tested - two teams sharing the same specialization
  (Search) under `NUM_TEAMS=10` - actually works: both swept
  different, overlapping-but-not-identical zone sets over a 2000-tick
  run with no deadlock.

### Fixed

- **Stale team-state read in `EVENT_INCIDENT_RESOLVED` snapshot
  logging** (`snapshot.c`, `complete_task()` in `incident.c`): by the
  time a task's completion is detected and logged, the team it was
  assigned to can already have been dispatched to a new task earlier
  in the same tick (`allocate_all_incidents()` runs before
  `simulation_step()`'s completion detection). `snapshot_record()`
  derived incident-detail columns from the team's *live* state, so
  the logged row described the team's *next* assignment instead of
  the one that just resolved. Rare before this merge (2/68 resolved-task
  log rows affected in a 2000-tick run) but became the near-universal
  case afterward (88/88), almost certainly because the new search-zone
  system makes many more system tasks available for immediate
  same-tick redispatch. Fixed by adding `snapshot_record_resolved()`,
  which takes the resolving task's own state directly rather than
  re-deriving it from the team - `complete_task()` now uses this
  instead of the general `snapshot_record()`. Verified: post-fix, 78/88
  resolved-task rows show real incident IDs and the remaining 10
  correctly correspond to genuine system-task completions (which are
  supposed to show no incident), matching the diag counters exactly.

### Verified

- Multi-seed sweep (1-15) clean, no hangs.
- CSV sentinel fix still holds (zero `4294967295` occurrences).
- `make check-board` passes against real BSP2 headers with the merged
  and fixed sources.

## Bug Fixes: Evacuation Gateway Hang, CSV Sentinel Corruption

### Fixed

- **Multi-seed run hang** (`incident.c`, `initialize_evacuation_gateways()`):
  the deterministic gateway-site lookup (`environment_find_gateway_site()`)
  could return the same colliding cell forever on retry, since it has no
  randomness. Seed 2 (gateway index 1) hung indefinitely. Fixed by
  tracking whether the deterministic candidate was already rejected once
  and falling through to the randomized placement branch on retry.
  Verified: seed 2 now completes instantly; swept seeds 1-10 clean.
- **`cbba_selected_team` showing `4294967295` in the snapshot CSV**
  (`snapshot.c`, `snapshot_record()`): `selected_team` was a signed `int`
  defaulting to sentinel `-1`, but printed with `%u`, reinterpreting the
  bit pattern as `0xFFFFFFFF`. Fixed by using the codebase's existing
  `NO_ASSIGNMENT` (`0xFF`) sentinel convention instead of `-1`, matching
  how `assigned_team`/`assigned_task_slot` are already handled elsewhere.
  Verified: 261 corrupted rows in a 500-tick run before the fix, zero
  after.

## Laptop / Board Platform Split

### Added

- Split the codebase into `core/` (portable simulator, verified
  freestanding-clean for Cortex-M55), `laptop/`, and
  `board/STM32N6570-DK/` (STM32N6570-DK).
- Added `core/sim_runner.h/.c` as the single source of truth for the
  per-tick sequence (`fault_injector_step -> allocate_all_incidents ->
  simulation_step`), shared by both platforms.
- Added `board/STM32N6570-DK/main_board.c`, `debug.c`, `snapshot.c` -
  compiled and verified against the real µT-Kernel 3.0 BSP2 headers
  (`github.com/tron-forum/mtk3_bsp2`, official STM32N6570-DK support).
- Added `make check-board`, which compiles `core/` + `board/STM32N6570-DK/`
  for Cortex-M55 against real BSP headers when `vendor/mtk3_bsp2` is
  present (see `vendor/README.md`), catching libc/OS dependency
  regressions before they ever reach hardware.

### Removed

- Removed the `sweep`/weight-sensitivity-analysis CLI mode and
  `analysis.c`/`analysis.h` (unused dev tooling, not part of the
  production simulator).
- Removed committed run artifacts (logs, CSVs, stale binaries) and
  added `.gitignore` to keep them out going forward.

### Verified

- Rebuilt from a fresh extraction of the packaged tree; 200-tick run
  byte-identical to the pre-restructure original on both stdout and
  stderr.

## Operational Fault Injection and Watchdog Refinement

### Added

- Added a generic operational fault injection subsystem with deterministic fault profiles.
- Added fault-state tracking to teams so the simulator can model mobility, communication, resource, environment, and mission effects.
- Added optional `--fault-profile` CLI support.

### Changed

- The watchdog now emits recommendations from evidence instead of directly mutating CBBA state.
- Simulation applies watchdog recommendations and fault effects while preserving the existing snapshot schema.

### Verified

- Built the simulator and ran a short smoke test.

## Offline Snapshot Labeling Pipeline

### Added

- Added a standalone offline label generator for `tron_snapshot_dataset.csv` that writes a separate labeled CSV.
- Added a horizon-based recovery label that marks whether a team returns to `STATUS_AVAILABLE` within `N` ticks after a snapshot.
- Added recovery metadata columns to the labeled dataset without changing the original snapshot export schema.

### Documented

- Documented the snapshot-labeling methodology, including the run/team grouping rule and the tier-based ready-stamina threshold used as recovery metadata.

### Verified

- Built the simulator and labeler with `make`.

## Procedural Environment Milestone

### Added

- Added persistent damage state to generated buildings, road cells, and infrastructure cells.
- Added construction quality to generated building records.
- Added infrastructure registration for safe zones and evacuation gateways.
- Added a deterministic earthquake event with epicenter, intensity, district vulnerability, construction quality, and distance attenuation.
- Added road blockage state and high-cost blocked-road routing behavior.
- Added object-level spawned-incident masks so damaged objects do not emit the same incident type repeatedly.
- Added environment summary output for earthquake epicenter, intensity, damaged buildings, blocked roads, and damaged infrastructure.

### Changed

- Incident generation now originates from damaged environment objects instead of random coordinates.
- Collapse, confined-space, gas leak, chemical exposure, fire, casualty, survivor, and search incidents now select eligible damaged buildings, roads, or infrastructure sources.
- World setup now registers safe zones and gateways as infrastructure before applying earthquake damage, then initializes the safety grid from the damaged environment.
- Blocked roads now influence routing through high travel cost while remaining reachable for incident response.

### Preserved

- Existing simulator CLI remains `./tron [first_seed] [seed_count] [ticks_per_seed]`.
- Existing `TeamState_t`, `IncidentState_t`, `SafeZone_t`, CBBA bundle/path structures, and snapshot CSV schema remain compatible.
- Existing metrics and snapshot flows continue to observe the same state transitions.
- Teams still move one grid step per simulator tick, preserving current movement semantics.

### Verified

- Built the simulator with `make`.
- Ran the smoke simulation with `make smoke`.

## Initial Procedural Environment Milestone

### Added

- Added a procedural environment subsystem in `environment.c` and `environment.h`.
- Added deterministic district generation for rescue base, residential, industrial, medical, civic, and waterfront districts.
- Added a generated road network with arterial, collector, and local road classes.
- Added typed building records with district and risk metadata.
- Added per-cell environment metadata for base safety, travel cost, safe-zone suitability, and navigability.
- Added environment-aware safe-zone candidate selection.
- Added environment-aware evacuation gateway placement.
- Added environment-aware incident placement with type-specific district preferences.
- Added `find_path_cost(...)` wrapper for route-cost consumers that do not need the next step.
- Added a `Makefile` with `make`, `make smoke`, and `make clean`.
- Added `ARCHITECTURE.md` describing the new subsystem and compatibility boundaries.

### Changed

- `generate_world(...)` now creates the procedural environment before initializing the safety grid.
- `initialize_grid(...)` now initializes each cell from environment base safety instead of a flat value of `100`.
- `find_next_path_step(...)` now uses environment navigability and travel cost while preserving its existing signature.
- CBBA distance scoring now uses weighted route cost through the same path API.
- Return-to-safe-zone selection now uses environment-aware path cost instead of Manhattan distance.
- Generated incidents now land on environment-selected sites instead of uniformly random grid cells.
- Final debug output now includes an environment summary with district, road, navigable-cell, and building counts.

### Preserved

- Existing simulator CLI remains `./tron [first_seed] [seed_count] [ticks_per_seed]`.
- Existing `TeamState_t`, `IncidentState_t`, `SafeZone_t`, CBBA bundle/path structures, and snapshot CSV schema remain compatible.
- Existing metrics and snapshot flows continue to observe the same state transitions.
- Teams still move one grid step per simulator tick, preserving current movement semantics.

### Verified

- Built the simulator with `gcc -std=c11 -Wall -Wextra -Wpedantic`.
- Ran a seeded smoke simulation with `./tron 1 1 200`.


20th August 2026
Merge summary
=============
You uploaded files-2.zip containing 9 files from a separate branch of work
(diagnostics/tracing flags, a fog-of-war "god/operator view" toggle, a
sweep-task anti-thrash cooldown, and a real capability-matrix bug fix).
I diffed every one of them against what I'd already built and merged as
follows:

TRUE MERGES (both sides had changes to reconcile)
--------------------------------------------------
tools__app.py
  - Kept: my handshake gating (/api/status, /api/board/launch,
    /api/board/continue-without-nucleo) and the override route
    (/api/board/override) from this chat.
  - Added: the "--trace" "--diag" CLI flags to both subprocess launch
    points (/api/run and /api/run-live), which is the only real content
    their branch's app.py had (everything else in their version was just
    a pre-handshake baseline, not an intentional removal).

tools_templates__index.html
  - Kept: my real confirmPendingOverride() fetch() implementation.
  - Added: their fog-of-war "god mode / operator view" toggle (button,
    toggleFogView(), and the two incident-filtering call sites) - this
    only works together with the "discovered" field debug.c now emits
    (see below), so both had to come over together.

ADOPTED AS-IS (files I never touched - no merge needed, just verified
they don't conflict with anything I built: no shared-struct changes to
CBBAAgentState_t/TeamState_t/IncidentState_t, no mission_link/comms_
transport touches)
-----------------------------------------------------------------------
platform_laptop__debug.c   -> laptop/debug.c
  Adds "discovered" to the JSON incident/system-task output - required
  by index.html's fog-of-war toggle.

platform_laptop__main.c    -> laptop/main.c
  New --trace / --diag CLI flags (gates g_simulation_trace_enabled and
  the end-of-run [DIAG ...] stderr summary, both off by default).

core__scoring.c            -> core/scoring.c
  Fixes a real bug: CAPABILITY_MATRIX's row/column order didn't match
  the actual Specialization_t enum (SEARCH=0, MEDICAL=1, ...) - Search
  and Medical had swapped data. Re-derived from the original intended
  semantics, not new numbers.

core__simulation.c         -> core/simulation.c
core__search_zones.h       -> core/search_zones.h
core__search_zones.c       -> core/search_zones.c
core__incident.c           -> core/incident.c
  Diagnostic counters (g_diag_discovered_by_type etc.) and a new
  ZONE_SWEEP_RECLAIM_COOLDOWN_TICKS / reclaimed_at_tick field on
  SearchZoneState_t to stop a create/evict thrash loop.

Verification performed after merging
-------------------------------------
- Full laptop build: clean, no errors.
- All core/board files that touch mission-link work re-checked with
  host gcc against the real BSP headers (-fsyntax-only): clean.
- All 5 standalone test suites re-run against the merged sources:
  handshake_selftest.c, mission_link_selftest.c, lora_frame_selftest.c,
  override_selftest.c (rebuilt against the merged core/scoring.c,
  core/incident.c etc.), handshake_selftest.py, telemetry_bridge_
  selftest.py - all pass.
- index.html's merged JS re-parsed for syntax errors: clean, both
  features (override fetch + fog toggle) confirmed present.
- A real ./tron run with --trace --diag confirmed the new flags actually
  produce output.

Filenames below use "__" in place of "/" - map back to the real path
using the arrow.

NEW FILES (from this chat, unaffected by the merge)
-----------------------------------------------------
core__mission_link_codec.h          -> core/mission_link_codec.h
core__mission_link.c                -> core/mission_link.c
platform_board__mission_link_task.c -> board/STM32N6570-DK/mission_link_task.c
tools__mission_link_selftest.c      -> laptop/tools/mission_link_selftest.c
core__mission_link_handshake.h      -> core/mission_link_handshake.h
core__mission_link_handshake.c      -> core/mission_link_handshake.c
tools__handshake_selftest.c         -> laptop/tools/handshake_selftest.c
tools__handshake_selftest.py        -> laptop/tools/handshake_selftest.py
tools__override_selftest.c          -> laptop/tools/override_selftest.c
tools__telemetry_bridge.py          -> laptop/tools/telemetry_bridge.py

MERGED FILES (see above)
---------------------------
tools__app.py                       -> laptop/tools/app.py
tools_templates__index.html         -> laptop/tools/templates/index.html

EDITED (Makefile only, unrelated to this merge)
--------------------------------------------------
Makefile                            -> Makefile

ADOPTED AS-IS FROM files-2.zip (see above)
---------------------------------------------
platform_laptop__debug.c            -> laptop/debug.c
platform_laptop__main.c             -> laptop/main.c
core__scoring.c                     -> core/scoring.c
core__simulation.c                  -> core/simulation.c
core__search_zones.h                -> core/search_zones.h
core__search_zones.c                -> core/search_zones.c
core__incident.c                    -> core/incident.c

NOT included (already correct, never touched by me or files-2.zip):
core/mission_link.h, core/mission_link_override.h, core/mission_link_
override.c, core/mission_state.h, core/cbba.c,
core/WIRE_PROTOCOL_MAGIC_BYTES.md.


Second upload (files.zip) - genuine merge conflict
=====================================================
This one was the real thing: files.zip contained core/mission_state.h and
core/incident.c from a branch that ALSO started before the override work
landed on CBBAAgentState_t - exactly the collision the original override
prompt's own coordination note warned about ("if another change also adds
a field here around the same time, reconcile field order explicitly
rather than letting one silently clobber the other").

Their mission_state.h had two genuine, unrelated additions:
  - WORK_RATE_MIN/WORK_RATE_MAX (1-8, replacing a hardcoded 1-5) - a
    tuned constant with a detailed sensitivity-sweep justification
    comment (cuts total task volume 39%, safe-zone fallback failures 91%)
  - NUM_SYSTEM_TASKS bumped 10 -> 20

...but their version of the file had DROPPED the override_active /
override_incident_id fields and the _Static_assert entirely - not an
intentional removal, just an artifact of branching before those existed.
Taking their file as-is would have silently broken core/cbba.c's
cbba_apply_override_pin() and core/mission_link_override.c's apply
function (both reference those fields directly) - compile failure, not
a subtle bug, but still exactly the kind of clobber the coordination
note was trying to prevent.

Merge performed: took their file as the base (their tuning work is real
and correct), re-inserted the override_active/override_incident_id
fields and _Static_assert in the same place as before, added a "MERGE
NOTE" explaining why, and re-ran the sizeof() probe rather than assuming
the old 216-byte number still held (it does - neither of their changes
affects this struct's layout: WORK_RATE_MIN/MAX are plain #defines,
NUM_SYSTEM_TASKS only sizes an unrelated array).

incident.c's one change (hardcoded 1,5 -> WORK_RATE_MIN/WORK_RATE_MAX)
depends on mission_state.h's new constants and had no other conflicts -
adopted as-is.

Verified NUM_SYSTEM_TASKS=20 doesn't break anything: grepped every call
site across the codebase (debug.c, allocator.c, cbba.c, team.c,
incident.c, recovery_manager.c, mission_execution_twin.c,
test_metrics.c) - every one already uses the macro, not a hardcoded 10,
so the array simply got bigger with no code changes needed.

Re-ran the full verification pass after this merge, override_selftest.c
in particular (the test most directly dependent on the re-merged
fields): full build clean, board syntax checks clean, all 5 standalone
test suites pass, and a live smoke run confirms NUM_SYSTEM_TASKS=20 is
in effect (system task slot IDs now go past 10).

core__mission_state.h               -> core/mission_state.h  (merged)
core__incident.c                    -> core/incident.c        (updated,
                                        replaces the earlier core__incident.c)



SEP 2nd:
## check-two-board-cbba-harness intermittent-failure investigation

- Instrumented `core/cbba.c`'s `cbba_ingest_remote_mesh_replies()` with a
  `[cbba] round timeout expired, no reply from peer` log at the
  CBBA_ROUND_TIMEOUT_MS (530ms) expiry point, gated behind
  `CBBA_DEBUG_TIMING` (off by default, so real board builds are
  unaffected; enabled only by the laptop harness build).
- Stress-tested: 400 runs with no induced load + 400 runs with
  background CPU load pinning all cores (`yes > /dev/null` x nproc).
  Result: 800/800 passed. Did NOT reproduce the historical 1-3/40
  intermittent failure. Not declaring it fixed on that basis alone -
  logging this for the record per the "can't reproduce" branch.
- The debug log line did surface a real, separate bug while
  instrumenting: `two_board_cbba_harness.c`'s `cbba_now_ms()` computed
  elapsed time as an unsigned subtraction that didn't borrow across a
  tv_sec/tv_nsec boundary, so it periodically returned a garbage
  multi-billion-ms value (confirmed via the new "waited=" field:
  16.3% of timeout events in the no-load runs, 29.9% under CPU load).
  That garbage value made the 530ms wait bail out almost instantly.
  This never caused a visible test failure in the 800-run stress test
  because the harness always runs a fixed 12 simulation ticks per
  side regardless, so losing one round to a spurious early bailout
  gets absorbed by later ticks' gossip - but it's a real bug and was
  fixed (proper sec/nsec borrow) since it directly undermines the
  530ms budget it's supposed to enforce, independent of whether it
  happens to be masked today.
- CBBA_ROUND_TIMEOUT_MS itself was left untouched - no evidence it's
  the culprit, and it's a shared constant with real hardware code.
- core/cbba.c's tie-break logic in cbba_receive_and_update() was not
  touched - no run showed a reply arriving in time with boards still
  disagreeing, so there's no evidence pointing at the consensus
  algorithm itself.
- The CBBA_DEBUG_TIMING log line stays in place for next time this
  flakes on a real CI run - it's cheap, off by default, and now
  reports trustworthy numbers.
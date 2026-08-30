# LoRa frame encryption (AEAD) — reference doc

Status: **IMPLEMENTED** (was: deferred until after base case). See
`CHANGELOG.md`'s "LoRa frame encryption implemented" entry for the
full change record and verification results; this file is kept as
the design record — what was decided, what was resolved, and what
was deliberately deviated from and why — not duplicated there.

Implementation: `core/chacha20.c`/`.h`, `core/poly1305.c`/`.h`,
`core/aead_chacha20poly1305.c`/`.h`, `core/lora_crypto.c`/`.h`,
wired into the envelope in `core/lora_frame.c`/`.h`.

---

## What actually shipped, vs. what this doc originally sketched

The sections below are the **original** design doc, left intact as
the record of the reasoning that led here — but two things changed
between the sketch and the implementation, both because they were
real gaps the sketch hadn't worked through, not stylistic choices:

1. **Wire size is 166 bytes, not 165.** The extra byte is an
   unencrypted-but-authenticated `sender_board_id` field (AAD). The
   "What's already settled" section below sized the counter/tag
   overhead correctly, but didn't account for what happens when
   *multiple boards* share *one* static PSK: two boards could reach
   the same 32-bit counter value and produce an identical `(key,
   nonce)` pair — a real AEAD break (plaintext recovery and tag
   forgery both become possible), not a theoretical one. Fixed by
   folding `sender_board_id` into the actual ChaCha20 nonce
   (`core/lora_crypto.c`'s `lora_crypto_build_nonce()`, byte 0 of 12),
   not just carrying it as authenticated metadata alongside the tag —
   AAD alone would only authenticate *who claims to have sent it*,
   it does nothing to the keystream itself. Verified directly:
   `lora_frame_selftest.c` packs identical payload+counter under two
   different `sender_board_id`s and asserts the ciphertext itself
   differs, not just the header byte.

   This does mean the sender-id space is capped at 256 boards sharing
   one PSK (one byte) — nowhere near a real constraint at this
   project's 2-board scale, but written down (`core/lora_crypto.h`,
   next to the nonce layout) rather than left implicit.

   It also means the whole scheme depends on every board's
   `THIS_BOARD_ID` actually being unique — a precondition this
   crypto layer can rely on but cannot itself verify (two boards
   flashed with the same id are two separate compiled binaries; no
   `_Static_assert` can span them). `make check-board-ids-distinct`
   (new, wired into both `check-board` and `check-pico`) is where
   that actually gets checked — it asks each real compiler what
   `THIS_BOARD_ID` resolves to and fails loudly on a collision.

2. **The "bootstrap paradox" section below is resolved by the choice
   of key management, not worked around.** The doc's "pragmatic
   fallback" option (one static PSK for the whole contest build) was
   the one implemented — and choosing it makes the circularity
   described below a non-issue rather than something to route
   around: since the key isn't derived from the handshake, the
   handshake needs no special-casing at all.
   `MissionLinkHandshakeReq_t`/`Reply_t` ride the exact same
   encrypted envelope as every other `frame_type`.

The known reboot-replay weakness the "pragmatic fallback" option
itself flagged below is real and was **not** solved — it's the
accepted tradeoff, documented in `core/lora_crypto.h`'s own top
comment, not discovered after the fact.

Crypto primitives were written in-repo (RFC 8439 ChaCha20/Poly1305,
not `cifra`/HACL*/X-CUBE-CRYPTOLIB — see "Candidate crypto libraries"
below for why those were considered) and cross-verified against
Python's `cryptography` library and an independent reference
implementation, not just checked against the RFC's own hand-copied
test vectors — that cross-verification is what caught a real bug (an
inverted mask in `poly1305.c`'s final reduction) that inspection
alone had missed. See `CHANGELOG.md` for the verification list
(both real targets, `check-board`/`check-pico`/`check-two-board-harness`/
`check-two-board-cbba-harness`, and the standalone selftests).

---

## Original design doc (as written before implementation)

Priority (at time of writing): low — invisible to contest judges,
not required for demo, real design risk remained open (see below).
Superseded by "IMPLEMENTED" status above; kept for the reasoning
trail.

### What's already settled

**New wire size:** `LORA_FRAME_MAX_WIRE_SIZE` grows from 147 to
**165 bytes** (+18). *(Shipped as 166 — see correction #1 above.)*

| Component | Bytes | Status |
|---|---|---|
| `frame_type` + `payload_len` (AAD, unencrypted but authenticated) | 2 | unchanged |
| ciphertext payload (ChaCha20 — stream cipher, same length as plaintext) | 144 | unchanged |
| Poly1305 tag | 16 | new |
| truncated nonce/counter fragment | 2 | new |
| additive checksum (kept as a cheap pre-filter before a Poly1305 verify) | 1 | unchanged |
| **Total** | **165** | was 147 |

165 fits comfortably under the SX1262's 256-byte FIFO and
`lora_spi_transmit()`'s 255-byte runtime cap (the cap is enforced by
a runtime check on a `uint32_t frame_len` param, not a `uint8_t`
type — minor correction from an earlier pass).

**Counter design:** mirror LoRaWAN's frame-counter scheme — only
transmit the low **16 bits** of a 32-bit counter; the receiver
reconstructs the full value from context (closest value greater than
the last accepted one, accounting for wraparound), and rejects
anything not strictly increasing. No replay-window bookkeeping
needed; tolerates dropped LoRa frames naturally.

### The unresolved part — read this before starting implementation

**1. No crypto primitive exists in this repo at all.** Grepped the
whole tree — no ChaCha20, no Poly1305, no mbedTLS/tinycrypt/
monocypher, nothing. This means "implement encryption" starts with
sourcing or writing an AEAD primitive that runs correctly bare-metal
on *both* a Cortex-M55 (DK) and an RP2040/2350 (Pico), with no crypto
library currently linked on either build. That's a real chunk of
work on its own, separate from the framing/counter design above.
*(Resolved: written in-repo, `core/chacha20.c`/`core/poly1305.c`;
zero-warning freestanding builds confirmed on both real targets.)*

**2. The "derive session key from the existing handshake" idea
doesn't actually work as stated — real bootstrap paradox.**
- `mission_link_handshake.h`/`.c`: `MissionLinkHandshakeReq_t` is
  `{board_id, protocol_version}` (2 bytes),
  `MissionLinkHandshakeReply_t` is `{board_id, protocol_version,
  ready}` (3 bytes) — plain structs, zero key material, no DH
  exchange, nothing a session key could come from. Calling this
  "reuse the existing infrastructure" undersells it — there's no
  key-exchange infrastructure there, it's a liveness ping/pong.
- Bigger issue: `comms_transport_send()` and both boards'
  `mission_link_task.c` route `LORA_FRAME_TYPE_HANDSHAKE` through the
  *same* `lora_frame_pack()` envelope as CBBA/mission-link frames. If
  everything under that envelope gets AEAD-encrypted under a session
  key, and the session key is supposed to come *from* the handshake,
  you can't decrypt the handshake reply that was supposed to give you
  the key. Circular.
- LoRaWAN avoids this because its join-request/accept messages are a
  structurally separate message class, encrypted (if at all) under
  fixed pre-provisioned root keys — not under the session key being
  established.
  *(Resolved by choice, not by routing around it: the implemented
  scheme uses a static PSK, not a handshake-derived key, so there is
  no ordering problem at all — see correction #2 above.)*

### Candidate crypto libraries (the piece that was missing before)

Checked what's actually available rather than assuming this has to
be written from scratch:

- **ST's own legacy STM32 cryptographic library (X-CUBE-CRYPTOLIB /
  UM1924)** ships a working **ChaCha20-Poly1305 example** already,
  per ST's own user manual. Worth checking first since it's from the
  same vendor ecosystem the DK side already builds against — may be
  the path of least friction there, if its license and portability
  (it's meant for CubeMX projects, not µT-Kernel) work out.
- **`cifra`** (`github.com/ctz/cifra`) — a small, portable,
  well-tested embedded crypto library with ChaCha20 and Poly1305
  built in. Notably, its own test matrix explicitly includes
  **Cortex-M0** targets (on real STM32F0 hardware via OpenOCD), which
  is a good sign for the Pico's Cortex-M0+ — this is the strongest
  candidate for the Pico side specifically. 100% line coverage,
  valgrind-clean per its own docs.
- **HACL\*** (`Hacl_Chacha20Poly1305_32.h`) — a formally-verified,
  portable 32-bit C implementation with no platform-specific
  dependencies. Good fallback if `cifra`'s license or structure
  doesn't fit.

Given the platform split (Cortex-M55 DK vs. Cortex-M0+ Pico), a
single portable-C library that both targets can build (like `cifra`
or HACL\*'s 32-bit variant) is probably the right call over anything
with per-platform assembly — one implementation, two targets, less
to validate.

*(Decision made: none of the above were vendored. Written in-repo
instead — see "What actually shipped" above for why: avoids
validating an external dependency's freestanding/license fit across
two different Cortex-M targets for the time available, and keeps the
implementation small enough to cross-verify exhaustively against
RFC 8439 vectors and an independent reference, which is what actually
caught the one real bug found during implementation.)*

### Hardware crypto acceleration — checked, and it doesn't actually matter here

Worth knowing so nobody goes down this path unnecessarily: the
STM32N657 (DK) datasheet does list real hardware crypto peripherals
(SAES, CRYP, HASH, PKA) — but they're **AES-based**, not
ChaCha20/Poly1305-based, so they wouldn't accelerate the scheme
already designed above. The Pico's chip, confirmed from the repo's
own comments, is the original **RP2040** (not RP2350) — which has
**no crypto acceleration at all**. So there's no hardware shortcut
available on either side for this specific cipher choice, and
switching ciphers just to chase DK-only AES hardware would make the
two boards asymmetric for no real benefit — LoRa's low data rate
means even slow software crypto (a few ms for a ~165-byte frame, per
published Cortex-M4 benchmarks of this exact cipher) is nowhere near
a bottleneck. Software ChaCha20-Poly1305 on both boards, no hardware
acceleration needed, is the right call — don't spend time chasing
this further.

Two real options, in order of how much time you have:

- **Full solution:** design an actual pre-shared-root-secret scheme —
  bake a root secret into both firmwares at build time, send only a
  cleartext-but-authenticated nonce per boot, derive `session_key =
  KDF(root_secret, nonce)`. This avoids the circularity above but is
  a real design task, not wiring.
- **Pragmatic fallback if time is short:** skip per-boot session keys
  entirely. Use one static pre-shared key for the whole contest
  build, with the 16-bit truncated/reconstructed counter as above,
  and explicitly document the reboot-replay weakness (a captured
  pre-reboot frame is replayable after a reboot since the counter
  resets but the key doesn't) as a known, deliberate tradeoff. This
  is honest and defensible to judges ("identified X, chose Y given
  constraints") and takes a fraction of the time of the full
  solution.
  *(This is the option implemented — see `core/lora_crypto.h`'s own
  top comment for the tradeoff, documented there rather than
  discovered later.)*

Either way, budget separate time for sourcing/validating the
ChaCha20-Poly1305 implementation itself — that's independent of which
key-management option you pick.

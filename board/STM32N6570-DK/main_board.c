/*
 * Board entry point (STM32N6570-DK, µT-Kernel 3.0 BSP2).
 *
 * This is the on-device analogue of laptop/main.c: it drives
 * the same core/sim_runner.h lifecycle (sim_runner_init ->
 * sim_start_seed_board -> sim_tick* -> sim_end_seed -> sim_runner_shutdown),
 * so the tick sequence can never silently diverge between platforms.
 * What's platform-specific here is only how the loop gets scheduled
 * (a µT-Kernel task instead of a plain C for-loop) and how progress
 * gets reported (tm_printf over USART1 instead of stdout).
 *
 * NOT YET WIRED INTO A REAL FIRMWARE PROJECT. This file compiles
 * cleanly against the real BSP2 headers (verified with arm-none-eabi-gcc,
 * see vendor/README.md), but turning it into a flashable image still
 * needs an STM32CubeIDE / STM32CubeMX project for the STM32N6570-DK
 * that:
 *   1. Generates HAL init + the secure-world project structure
 *      (TrustZone is enabled on STM32N657 by default; BSP2 currently
 *      only runs in the secure sub-project).
 *   2. Adds `void knl_start_mtkernel(void); knl_start_mtkernel();`
 *      inside the generated Core/Src/main.c's USER CODE BEGIN/END 2
 *      block, per the BSP2 STM32Cube manual section 4.2.3.
 *   3. Adds this project's core/ and board/STM32N6570-DK/ sources, plus
 *      board/STM32N6570-DK/vendor/mtk3_bsp2's include/config/mtkernel
 *      paths, to the project's build settings.
 * That project-scaffolding step is IDE work, not something any C
 * source file can do — tracked as the next concrete step once you're
 * ready to actually flash the board, not part of this pass.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include "board_log.h"       /* console output through the logger task */
#include "board_rtos.h"      /* kernel objects, helper tasks, priorities */
#include "mtkernel/lib/libtm/libtm.h"   /* tm_com_init() */
#include "sim_runner.h"
#include "state_hash.h"      /* per-tick determinism fingerprint */
#include "cbba.h"            /* CBBA_REPLY_WAIT_STEP_MS - cbba_wait_poll() */
#include "fault_config.h"
#include "mission_state.h"       /* g_current_tick - clock-driven tick loop */
#include "mission_link_codec.h" /* mission_link_roster_reject_count() -
                                    roster-wait loop's reject diagnostic */
#include "comms_transport.h" /* g_comms_transport_this_board_id,
                                 comms_transport_init(), comms_transport_
                                 board_count() - see THIS_BOARD_ID below
                                 and the boot CONFIG line in usermain() */
#include "vbus.h"             /* VIRTUAL_BUS_ENABLED - boot CONFIG line */
#include "consensus_config.h" /* CONSENSUS_ENFORCE - boot CONFIG line */
#include "fault_predictor.h"  /* FAULT_MODEL_LOADED - boot CONFIG line */
#include "stm32n657xx.h"     /* USART1 registers - board_usart1_read_byte();
                                same CMSIS header sx1262_hal.c uses */

/* Mirrors sx1262_hal.c's own #ifndef/#define default (see that file's
   top-of-file comment) so this translation unit can print the boot
   CONFIG line below without needing sx1262_hal.c's internals - both
   pick up the same value from a single -DBOARD_RADIO_ENABLED=1 on the
   command line when it's set, and agree on the same default (0,
   single-board submission: no radio dependency) when it isn't. */
#ifndef BOARD_RADIO_ENABLED
#define BOARD_RADIO_ENABLED 0
#endif

/*
 * THIS_BOARD_ID: closes a gap comms_transport.h's own "Handoff notes"
 * (and this file's mission_link_task_init()/mission_link_handshake_
 * board_init() ordering comments, a few lines down) have flagged since
 * they were written but nothing ever actually implemented - platform
 * bring-up code (this file) is supposed to set g_comms_transport_
 * this_board_id to this board's real id and call comms_transport_
 * init() before anything that depends on either. Neither call existed
 * anywhere until this line.
 *
 * A compile-time -D, not a runtime probe/strap-pin read: matches how
 * _STM32CUBE_DISCOVERY_N657_ (this board) and _PICO_RP2040_ (the other
 * board target) already work as per-target -D flags in this project -
 * which physical board a given firmware image is FOR is already a
 * build-time fact everywhere else in this project, this is the same
 * kind of fact, not a new class of one. STM32N6570-DK is board id 0 -
 * see comms_transport.h's own doc: "team_id / 5" is the board-
 * assignment rule, so board 0 owns teams 0-4, and this board is the
 * one with a laptop-facing USART1 (see mission_link_task.c's own
 * role-based, not chip-based, gate on this same id).
 */
#ifndef THIS_BOARD_ID
#define THIS_BOARD_ID 0u
#endif

#ifdef LORA_RAW_RX_CHECK
/* ~15s window at ~15ms/poll (tk_dly_tsk(15) per iteration below,
   see sim_task()'s own LORA_RAW_RX_CHECK block) - 1000 * 15ms. */
#ifndef LORA_RAW_RX_CHECK_TICKS
#define LORA_RAW_RX_CHECK_TICKS 1000u
#endif
#endif

/* Bring-up defaults. No CLI on-device, so these are compile-time
 * constants rather than argv-parsed like the laptop's main.c. Revisit
 * once there's a real reason to change them at runtime (e.g. a USB
 * command channel) — premature to build that before USB telemetry
 * exists at all. */
#ifndef BOARD_SEED
#define BOARD_SEED 1u
#endif
#ifndef BOARD_SEED_COUNT
#define BOARD_SEED_COUNT 1u
#endif
/* Exactly one seed per boot. g_current_tick follows the laptop's mission
 * clock and nothing resets it between seeds (sim_start_seed_board() must
 * not - see core/sim_runner.h), so a second seed would find its tick loop
 * (g_current_tick < BOARD_TICKS_PER_SEED) already finished and never run.
 * Running several seeds would need the laptop to restart its clock and
 * resend the world for each one first. */
_Static_assert(BOARD_SEED_COUNT == 1u,
               "BOARD_SEED_COUNT must be 1: g_current_tick is never reset between board seeds");
/* Upper bound only: ticks follow the laptop's mission clock (see the
 * tick loop in sim_task()), so the laptop decides how long a mission
 * runs. */
#ifndef BOARD_TICKS_PER_SEED
#define BOARD_TICKS_PER_SEED 0xFFFFFFFFu
#endif
#ifndef BOARD_FAULT_PROFILE
#define BOARD_FAULT_PROFILE FAULT_PROFILE_NONE
#endif

/* Tick pacing: none of the board's own. The laptop is the clock
 * master (core/mission_link.h section 5b) - a board runs a sim tick only
 * once the laptop's MissionLinkClock_t allows it, so a 36 s real-time
 * demo and a faster rehearsal are the same firmware. Between allowed
 * ticks the board keeps the link serviced and sleeps this long per
 * pass, which bounds how late it notices a new clock frame. */
/* Determinism fingerprint (core/state_hash.h): every this-many ticks the
   board prints a STATEHASH line, same fields and format as the PC build's
   `--state-hash=N`, so the lines can be diffed between runs and between
   firmware versions. 0 turns it off. Printing one line holds interrupts
   masked for ~10 ms (tm_putstring, see KNOWN_LIMITATIONS), which is why it
   is periodic and not per tick. */
#ifndef BOARD_STATE_HASH_INTERVAL
#define BOARD_STATE_HASH_INTERVAL 50u
#endif

#ifndef BOARD_CLOCK_IDLE_POLL_MS
#define BOARD_CLOCK_IDLE_POLL_MS 5u
#endif

/* Cadence for draining the snapshot ring (board/STM32N6570-DK/snapshot.c)
 * out over USART1. Every tick is too eager: in a fast (non-real-time)
 * mission ticks come quickly, and that would call into
 * tm_putchar-based UART writes far more often than the ring can
 * possibly need it, for no benefit -- there's no live consumer this
 * needs to be near-real-time for yet, and it would just add UART
 * contention with tm_printf debug lines on every single tick. Every-
 * N-ticks is simpler to reason about than a fill-threshold trigger
 * (no need to call board_snapshot_ring_pending() every tick just to
 * decide whether to drain) and, combined with the per-call cap in
 * board_snapshot_ring_drain() (see BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_
 * PER_CALL in snapshot.c), keeps the ring from ever holding much more
 * than one interval's worth of undrained entries in steady state.
 * 50 ticks is a starting point, not a measured value -- revisit once
 * there's a real receiver to check actual undrained-backlog / gap
 * behavior against. */
#ifndef BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL
#define BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL 50u
#endif

/*
 * Not part of snapshot.h's public interface (core/ never calls
 * this) -- same status as board_snapshot_ring_pending(), which is
 * also forward-declared at its one call site rather than added to
 * snapshot.h, since both are board-platform-internal telemetry
 * plumbing, not part of the portable core/snapshot.h contract other
 * platforms implement. Defined in board/STM32N6570-DK/snapshot.c.
 */
extern uint32_t board_snapshot_ring_drain(void);

/*
 * Mission-link heartbeat trigger hook (this prompt's addition -
 * additive only, coordinate with Prompt 4's roster-wait diagnostic
 * hook in this same file, different concern). mission_link_task_step()
 * (board/STM32N6570-DK/mission_link_task.c) is what actually decides
 * whether to send: it compares each local team's current status
 * against MISSION_LINK_HEARTBEAT_INTERVAL_MS of wall-clock elapsed
 * time via cbba_now_ms() (see that file's mission_link_status_tx_step()
 * and mission_link.h section 5a) and re-anchors on a periodic
 * heartbeat even when nothing changed. This hook's only job is to make
 * sure that check actually runs often enough for the timer to matter -
 * called every tick, same as the header's own handoff note 2 specifies
 * ("unlike the snapshot ring, there's no large backlog to bound; each
 * call does a handful of non-blocking reads/polls and returns"). Both
 * functions are forward-declared here rather than added to a portable
 * core/ header, same convention board_snapshot_ring_drain() above
 * already follows for board-platform-internal plumbing.
 */
extern void mission_link_task_init(void);
extern void mission_link_task_step(void);

/*
 * sim_tick()-vs-roster-arrival gap (flagged, not designed - see the
 * task note this change closes). mission_link_handshake_round_trip_
 * complete() below only proves liveness: this board and its handshake
 * peer are both up and able to exchange a frame. It says nothing
 * about whether a real MissionLinkRoster_t for any team THIS board
 * hosts has actually arrived and been applied - before that happens,
 * g_teams[] for this board's own teams is still whatever
 * sim_runner_init()/generate_teams() seeded it as, not the
 * laptop-authoritative roster data the mission is actually supposed
 * to run against. Defined in mission_link_task.c (this board's own
 * file, same as the other forward-declared hooks above) as a simple
 * "has mission_link_apply_roster() ever been called for one of this
 * board's own teams" counter - see that file's own comment on
 * g_roster_applied_count for why it counts local applies only, not
 * relays. Used below exactly like the handshake gate just above it:
 * block the seed loop, pumping mission_link_task_step() so the wait
 * can actually resolve, until at least one team's real data has
 * landed. */
extern uint32_t mission_link_task_any_roster_applied(void);

extern void sx1262_hal_init(void);

/* board/STM32N6570-DK/usart1_driver.c - enables the USART1 RXNE
   interrupt and NVIC line; see that file's own top comment for why
   this has to be interrupt-driven rather than polled. */
extern void board_usart1_rx_init(void);
extern void board_usart1_set_rx_notify(ID flgid, UINT bit);

/*
 * RX counterpart to sx1262_hal_init() above (board/STM32N6570-DK/
 * sx1262_hal.c). Non-blocking, cheap-when-idle poll of the SX1262's
 * DIO1 line - see that function's own doc for the full contract.
 * Called at every one of this file's mission_link_task_step() call
 * sites below, not just the per-tick seed loop: comms_transport's
 * inbox (which mission_link_task_step() drains) only ever gets real
 * physical-RX data in it if this function is what's pumping DIO1/the
 * radio in the first place, and the handshake-round-trip and
 * roster-arrival wait loops below need exactly that to ever unblock
 * on real hardware - the same "known gap" both loops' own comments
 * already flag, now closed for the RX half specifically (TX still
 * depends on lora_spi_transmit() being reachable, unrelated to this
 * function).
 */
extern void sx1262_poll_rx(void);

/*
 * Tiered TX dispatcher (core/comms_transport.c) - the piece that
 * actually moves a cross-board comms_transport_send() from "enqueued"
 * to "on air". Must be called every place this file already pumps
 * sx1262_poll_rx()/mission_link_task_step(), including this file's
 * two wait-loops (handshake/roster) - a handshake or roster relay
 * message enqueued during those phases would otherwise never flush,
 * since nothing else calls this. See core/comms_transport.h's own
 * doc on this function for the full dispatch contract.
 */
extern void comms_transport_dispatch_pending(void);

/*
 * Handshake readiness gate (this prompt's addition — read alongside
 * the matching gate in board/STM32N6570-DK/mission_link_task.c's RX
 * paths, which reject inbound frames on the same condition this
 * blocks the seed loop on).
 *
 * Design is deliberately ROLE-based, not chip-based: it depends only
 * on mission_link_handshake_round_trip_complete() below (itself
 * built on mission_link_handshake_build_local_reply(), core/mission_
 * link_handshake.c — portable, no board-name branching inside it at
 * all) and on g_comms_transport_this_board_id's numeric role (0 =
 * USB-facing, others = LoRa-relay-only), the same role check
 * mission_link_task.c's USART1 gate already uses. Nothing here
 * checks for "the DK" or "the Pico" by name, so if the USB-facing
 * and LoRa-relay-only roles ever swap which physical board carries
 * them, NOTHING in this gate needs to change — only whichever
 * build's compile-time g_comms_transport_this_board_id assignment
 * does (that assignment is itself still pending wiring elsewhere —
 * see the existing "must follow whatever sets
 * g_comms_transport_this_board_id" note a few lines below).
 *
 * KNOWN GAP, not introduced by this change but load-bearing for it:
 * mission_link_handshake_round_trip_complete() (and the build_local_
 * reply() it wraps) only ever returns true after mission_link_
 * handshake_mark_local_ready() AND mission_link_handshake_note_
 * request_received() have both been called — and as of this change,
 * NOTHING in this file or
 * mission_link_task.c actually sends or receives a real
 * MissionLinkHandshakeReq_t/Reply_t frame yet (no LORA_FRAME_TYPE_
 * HANDSHAKE wire type even exists in core/lora_frame.h yet — see
 * core/mission_link_handshake.c's own top comment, step 1-4, which
 * describes the intended sequence but isn't wired to any actual RX/TX
 * path). Until that separate wiring lands, this gate will block
 * forever on real hardware — that's expected, not a bug in the gate
 * itself, and is the next piece of work this unblocks rather than
 * something this change was scoped to also finish.
 */
#include "mission_link_handshake.h" /* MissionLinkHandshakeReply_t,
                                        mission_link_handshake_* below */
extern uint8_t g_comms_transport_this_board_id;

/*
 * CORRECTION vs. the first pass of this comment: mission_link_
 * handshake_local_is_ready() alone is NOT the right condition to
 * wait/gate on — it only reflects THIS board's own init being done
 * (g_local_ready), which mark_local_ready() sets unconditionally a
 * few lines below, before any request has arrived. Checked against
 * mission_link_handshake.c: the actual "has this board's handshake
 * completed" condition (both local-ready AND the incoming request
 * having been seen) is exactly what mission_link_handshake_build_
 * local_reply() tests — it returns 1 only once both are true. Use
 * THAT as the gate, everywhere (here and in mission_link_task.c's RX
 * paths), not local_is_ready() alone.
 */
static int mission_link_handshake_round_trip_complete(void)
{
    MissionLinkHandshakeReply_t scratch; /* discarded — only the
                                             return value (0/1) is
                                             used as the gate here;
                                             the actual reply bytes
                                             are built/sent by
                                             mission_link_task.c's own
                                             TX path, not here. */
    return mission_link_handshake_build_local_reply(&scratch) != 0;
}

#include "lora_frame.h" /* lora_frame_auth_fail_count(),
                           lora_frame_accept_count() */

/*
 * Link diagnostics. A board rebooting while its peer stays up used to
 * wedge boot silently in two ways (both reproduced at runtime): the
 * peer's replay floor locked out the rebooted board's restarted frame
 * counter, and board 0 never re-sent a rebooted peer its handshake Req.
 * Both are fixed now - per-boot LoRa session epochs with a challenge
 * (core/lora_crypto.h) and board 0 re-opening a rebooted peer's
 * handshake leg (mission_link_task.c) - and these lines show that
 * recovery happening, or show where it stalls if it doesn't. Most of
 * it is visible on the board that STAYED UP, which is why this is also
 * called from the tick loop, not only from the boot wait loops.
 *
 * Each line prints once per change, since the wait loops spin
 * unthrottled. Report only - nothing here aborts a wait, retries, or
 * relaxes replay rejection.
 */
extern uint32_t mission_link_task_peer_reboot_count(void);
extern int sx1262_hal_get_random_u32(uint32_t *out);

/* USART1 receive errors: hardware ORE/FE/NE plus RX-ring-full drops,
   both counted in usart1_driver.c's board_usart1_isr(). */
extern uint32_t board_usart1_rx_error_count(void);
extern uint32_t board_usart1_rx_drop_count(void);
static uint32_t g_reported_usart1_rx_errors;
static uint32_t g_reported_auth_fails;
static uint32_t g_reported_session_holds;
static uint32_t g_reported_peer_reboots;
static uint8_t g_reported_first_frame;
static uint8_t g_reported_peer_timed_out[HANDSHAKE_MAX_BOARDS];

static void board_report_link_diagnostics(void)
{
    uint32_t auth_fails = lora_frame_auth_fail_count();

    if(auth_fails != g_reported_auth_fails)
    {
        BOARD_LOG((UB*)"tron: LoRa AUTH FAIL (%lu so far) - frames arriving but failing authentication (RF corruption, a peer built with a different key, or a replay)\n",
                  (unsigned long)auth_fails);
        g_reported_auth_fails = auth_fails;
    }

    {
        uint32_t holds = lora_frame_session_hold_count();

        if(holds != g_reported_session_holds)
        {
            BOARD_LOG((UB*)"tron: LoRa session HOLD (%lu so far) - a peer started a new session (reboot?); its frames wait until it answers this board's challenge\n",
                      (unsigned long)holds);
            g_reported_session_holds = holds;
        }
    }

    {
        uint32_t reboots = mission_link_task_peer_reboot_count();

        if(reboots != g_reported_peer_reboots)
        {
            BOARD_LOG((UB*)"tron: peer reboot detected (%lu so far) - re-running that board's handshake\n",
                      (unsigned long)reboots);
            g_reported_peer_reboots = reboots;
        }
    }

    {
        uint32_t rx_errors = board_usart1_rx_error_count();

        if(rx_errors != g_reported_usart1_rx_errors)
        {
            BOARD_LOG((UB*)"tron: USART1 RX errors (%lu so far, %lu of them RX ring full) - laptop bytes lost or damaged (overrun/framing/noise, or the sim task not reading fast enough)\n",
                      (unsigned long)rx_errors,
                      (unsigned long)board_usart1_rx_drop_count());
            g_reported_usart1_rx_errors = rx_errors;
        }
    }

    if(!g_reported_first_frame && lora_frame_accept_count() > 0u)
    {
        board_log_puts((const char*)"tron: LoRa link alive - first authenticated frame received\n");
        g_reported_first_frame = 1u;
    }

    if(g_comms_transport_this_board_id == 0u)
    {
        uint8_t board_count = comms_transport_board_count();
        uint8_t peer;

        if(board_count > HANDSHAKE_MAX_BOARDS)
        {
            board_count = HANDSHAKE_MAX_BOARDS;
        }

        for(peer = 1u; peer < board_count; peer++)
        {
            if(!g_reported_peer_timed_out[peer] &&
               !mission_link_handshake_relay_reply_seen(peer) &&
               mission_link_handshake_relay_timed_out_seen(peer))
            {
                BOARD_LOG((UB*)"tron: peer board %lu never answered the handshake (timed out) - the laptop will not send mission data until it does; check that board is powered, flashed with this build, and on the same radio params\n",
                          (unsigned long)peer);
                g_reported_peer_timed_out[peer] = 1u;
            }
        }
    }
}

/*
 * Boot wait loops only: nothing reads CBBA or fault traffic until the
 * simulation starts, so a peer that is already mid-mission would fill the
 * 16-slot inbox with it and the Req/roster these loops are waiting for
 * would be refused on arrival. Those frames describe rounds this board is
 * not part of yet, so dropping them loses nothing. Override frames and
 * their acks are kept: mission_link_task_step() answers an override that
 * arrives before this board is ready with a NOT_READY ack, and board 0
 * forwards acks to the laptop, so the operator isn't left guessing.
 */
static void board_boot_wait_discard_stale_frames(void)
{
    (void)comms_transport_discard_received_except(
        COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_HANDSHAKE) |
        COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_MISSION_LINK) |
        COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_OVERRIDE) |
        COMMS_FRAME_TYPE_BIT(LORA_FRAME_TYPE_OVERRIDE_ACK));
}

/*
 * Per-boot LoRa session epoch (core/lora_crypto.h lora_crypto_set_tx_
 * epoch()): MUST differ from every previous boot's, or a reboot brings
 * back AEAD nonce reuse and the peer-side lockout. Primary source is the
 * SX1262's own RNG. Every draw is mixed with µT-Kernel operating time as
 * well, and a failure or a zero result is reported loudly - operating
 * time alone at this point of boot is close to constant across reboots,
 * so a board that prints the warning below should not be trusted to
 * recover from a reboot until its radio RNG works.
 */
static void board_seed_lora_session_epoch(void)
{
    uint32_t epoch = 0u;
    uint32_t extra = 0u;
    SYSTIM otm;
    int rng_ok = 0;
    UINT attempt;

    /* Retried: a single SPI glitch at bring-up shouldn't cost reboot
       safety. The RNG and the LoRa link are the same chip on the same
       bus, so if every attempt fails the radio can't carry traffic
       either - the weak fallback below only matters for a radio that
       comes good later, and the warning says so. */
    for(attempt = 0u; attempt < 5u && !rng_ok; attempt++)
    {
        rng_ok = (sx1262_hal_get_random_u32(&epoch) == 0) &&
                 (sx1262_hal_get_random_u32(&extra) == 0);
        if(!rng_ok)
        {
            tk_dly_tsk(20u);
        }
    }

    if(tk_get_otm(&otm) == E_OK)
    {
        extra ^= otm.lo;
    }
    epoch ^= (extra << 13) | (extra >> 19);

    if(!rng_ok || epoch == 0u)
    {
        board_log_puts((const char*)"tron: WARNING - SX1262 RNG unavailable; LoRa session epoch is weak, reboot recovery may fail\n");
        if(epoch == 0u)
        {
            epoch = 0x5EED0001u ^ (uint32_t)THIS_BOARD_ID;
        }
    }

    lora_crypto_set_tx_epoch(epoch);
    BOARD_LOG((UB*)"tron: LoRa session epoch %08lx\n", (unsigned long)epoch);
}

/* ---------------------------------------------------------------
 * USART1 (laptop link) byte I/O and CBBA timing hooks
 * ---------------------------------------------------------------
 * The BSP's console driver only offers a blocking read (tm_getchar()
 * "polling not supported") and a tm_putchar() that sends CR before every
 * LF. Nothing provided board_usart1_read_byte(), so its weak -1 stub was
 * all mission_link_task.c ever saw: board 0 could not receive a single
 * laptop byte - not the handshake Req, not an override, not a roster
 * (found by review). Both are done here on the same USART1 the BSP's
 * tm_com.c drives. */
extern void mission_link_task_usart1_pump(void);

/* Raw write via the BSP's own tm_snd_dat() (declared in libtm.h), with
   interrupts masked per byte the way tm_putchar() does it. */
void board_usart1_write(const uint8_t *bytes, uint32_t len)
{
    uint32_t i;
    UINT imask;

    /* A whole frame under the UART lock: the logger task's text and other
       tasks' frames cannot land in the middle of it. */
    board_uart_tx_lock();
    for (i = 0u; i < len; i++)
    {
        DI(imask);
        tm_snd_dat((const UB *)&bytes[i], 1);
        EI(imask);
    }
    board_uart_tx_unlock();
}

/* Real board_usart1_read_byte() lives in usart1_driver.c (interrupt-
   driven ring buffer) - see this file's own sim_task() comment at
   board_usart1_rx_init()'s call site for why the polling version this
   used to be here was replaced. Its ISR counts and clears ORE/FE/NE
   and counts RX-ring-full drops; board_report_link_diagnostics() above
   reads the total through board_usart1_rx_error_count(). */

/* tm_com_init() leaves USART1 with a one-byte receive register, so any
   byte not read before the next one arrives is lost - and this board is
   often busy for longer than one byte time (86 us at 115200). The
   16-byte hardware FIFO holds a whole laptop frame; it can only be
   switched on while the peripheral is disabled, so wait for any
   transmission in progress to finish first. */
/* USART1_S, never the plain USART1 macro: stm32n657xx.h maps USART1 to the
   SECURE alias (0x52001000) only when CPU_IN_SECURE_STATE is defined, and
   that comes from -mcmse (both the CubeIDE .cproject and the Makefile pass
   it today). Without it the macro is the NON-secure alias (0x42001000), a
   different view of the same UART that does not see the secure side's
   state, and these writes would land on the wrong register file with no
   fault. tm_com.c and usart1_driver.c address the secure alias explicitly;
   this does the same so FIFO enable cannot depend on a compiler flag.
   Returns the CR1 value read back, so boot can print whether FIFOEN stuck. */
static uint32_t board_usart1_enable_rx_fifo(void)
{
    UINT imask;
    uint32_t spins = 1000000u;
    uint32_t cr1;

    DI(imask);
    while ((USART1_S->ISR & USART_ISR_TC) == 0u && spins > 0u)
    {
        spins--;
    }
    USART1_S->CR1 &= ~USART_CR1_UE;
    USART1_S->CR1 |= USART_CR1_FIFOEN;
    USART1_S->CR1 |= USART_CR1_UE;
    cr1 = USART1_S->CR1;
    EI(imask);

    return cr1;
}

/*
 * Per-tick CPU cost, task 7: read via the Cortex-M55's own DWT cycle
 * counter (CYCCNT), not a µT-Kernel timer - a sim_tick() (CBBA auction
 * for up to 10 teams, the mission-execution twin, watchdog, and now
 * consensus_step() over 90 observer pairs when VIRTUAL_BUS_ENABLED=1)
 * is expected to run well under one tick period, but that has only
 * ever been estimated host-side (see the single-board DK report's CPU
 * section) - nothing on this board actually measured it before. DWT is
 * standard CMSIS (core_cm55.h, pulled in transitively via
 * stm32n657xx.h, already included above for USART1's registers), not
 * board-specific, so this needs no extra header.
 *
 * UNVERIFIED against real hardware: this project has no vendored BSP2
 * in this environment (see Makefile's BSP_MODE fallback), so this has
 * only been reviewed against the standard CMSIS DWT/CoreDebug register
 * layout, never actually compiled or run on a real STM32N657. Treat
 * board_dwt_init()'s effect (whether CYCCNT actually starts counting)
 * as unconfirmed until someone checks it on hardware.
 */
#define BOARD_DWT_DEMCR_TRCENA   (1u << 24)
#define BOARD_DWT_CTRL_CYCCNTENA (1u << 0)

typedef struct
{
    volatile uint32_t CTRL;
    volatile uint32_t CYCCNT;
} BoardDwtRegs_t;

#define BOARD_DWT       ((BoardDwtRegs_t *)0xE0001000u)
#define BOARD_DEMCR_REG (*(volatile uint32_t *)0xE000EDFCu)

static void board_dwt_init(void)
{
    BOARD_DEMCR_REG |= BOARD_DWT_DEMCR_TRCENA;
    BOARD_DWT->CYCCNT = 0u;
    BOARD_DWT->CTRL |= BOARD_DWT_CTRL_CYCCNTENA;
}

static uint32_t board_dwt_cycles(void)
{
    return BOARD_DWT->CYCCNT;
}

/* tron_board.ioc: RCC.CpuClockFreq_Value=600000000 - the actual
   STM32CubeIDE project's configured CPU clock for this board, not an
   assumed value. Used only to print an approximate microsecond
   figure alongside the raw cycle count below; the raw count is the
   authoritative number if this ever drifts from the real .ioc. */
#define BOARD_CPU_CLOCK_HZ 600000000u

static uint32_t g_tick_cycles_last;
static uint32_t g_tick_cycles_max;

/* Real milliseconds for the status heartbeat and handshake pacing (link
   side only - CBBA's reply wait is bounded by a step count, see
   cbba_wait_poll() below, and the simulation reads no clock). Without this
   override the link would run on cbba.c's simulated clock - 10 ms per call
   - and a heartbeat "interval" would pass in microseconds (found by
   review). If the kernel call ever fails, time still advances so no wait
   can spin forever. */
uint32_t cbba_now_ms(void)
{
    SYSTIM now;
    static uint32_t s_fallback_ms;

    if (tk_get_otm(&now) == E_OK)
    {
        return (uint32_t)now.lo;
    }

    s_fallback_ms += 10u;
    return s_fallback_ms;
}

/* One idle wait step of CBBA's reply wait (see cbba.h). Runs inside
   sim_tick(), i.e. in the sim compute task WHILE IT HOLDS the comms mutex
   (the radio, comms_transport and the mission-link state belong to whoever
   holds it). Releasing it for the length of the step lets the link task, which
   has the higher priority, run its in-tick service pass - poll the radio,
   dispatch queued TX, drain USART1 - so this board's queued bids go out and the
   peer's replies come in, exactly what the old inline calls did, but now by the
   task that owns the radio. The link task applies nothing to simulation state
   in that pass (g_sim_in_tick is set), so nothing changes under CBBA's feet.

   The wait is bounded in core by a COUNT of these calls, so the sleep is
   what maps the budget onto real time. The kernel adds one TIMER_PERIOD to
   every timeout (timer.c: event time = now + tmout + TIMER_PERIOD), so a
   call made right after a wake-up sleeps about two ticks:
   BOARD_CBBA_WAIT_STEP_MS = 2 * CNF_TIMER_PERIOD = 20 ms. sim_compute_task
   sizes the budget from that (CBBA_ROUND_TIMEOUT_MS / BOARD_CBBA_WAIT_STEP_MS
   = 62 steps, ~1.24 s), so a dead-peer round stays ~1.25 s. A live peer's
   replies end the wait early either way. NOT yet measured on a board. */
#define BOARD_CBBA_WAIT_STEP_MS (2u * (uint32_t)CNF_TIMER_PERIOD)

void cbba_wait_poll(void)
{
    (void)tk_set_flg(g_flg_link, FLG_LINK_POLL);
    (void)tk_unl_mtx(g_mtx_comms);
    (void)tk_dly_tsk(CNF_TIMER_PERIOD);
    (void)tk_loc_mtx(g_mtx_comms, TMO_FEVR);
}

/*
 * Two tasks run the board application (priorities in board_rtos_cfg.h):
 *
 *   link_task         (4)  owns the radio and the laptop link. Between ticks
 *                          it runs the whole mission-link step (parse and
 *                          apply the laptop's frames, relay, heartbeat); during
 *                          a tick only the service pass (radio, TX dispatch,
 *                          USART1 drain). Tells the sim when the laptop has
 *                          authorised the next tick.
 *   sim_compute_task  (10) runs sim_tick() unchanged and is the ONLY writer
 *                          of simulation state.
 *
 * The comms mutex (g_mtx_comms) is held by the sim for a whole tick (it is
 * released only inside cbba_wait_poll), and by the link task for each pass, so
 * the link never applies a laptop frame in the middle of a tick: laptop input
 * reaches the simulation only between ticks, as it did when one task did both.
 * g_sim_in_tick (written under the mutex) tells the link which kind of pass
 * it may run.
 */
static ID sim_tskid;
static ID link_tskid;
static volatile uint8_t g_sim_in_tick;
static uint8_t g_world_ready; /* link task only */
static uint8_t g_link_handshake_reported; /* link task only */
static uint32_t g_link_reported_rejects; /* link task only */

static uint8_t g_sim_stack[BOARD_SIM_STACK_BYTES] __attribute__((aligned(8)));
static uint8_t g_link_stack[BOARD_LINK_STACK_BYTES] __attribute__((aligned(8)));

/* Watchdog for one sim_tick(): it covers compute plus CBBA's bounded reply
   waits, not the wait for the laptop's next tick. Fires FLG_MON_WDOG. */
#ifndef BOARD_SIM_WDOG_MS
#define BOARD_SIM_WDOG_MS 10000u
#endif

LOCAL void sim_compute_task(INT stacd, void *exinf);
LOCAL void link_task(INT stacd, void *exinf);

/* Handshake and world-data waits, run by the link task before the seed loop
   (these used to be spin loops in the single task). */
static void board_link_pass_prerun(void)
{
    sx1262_poll_rx();
    mission_link_task_step();
    comms_transport_dispatch_pending();
    board_report_link_diagnostics();
    board_boot_wait_discard_stale_frames();

    if(!g_link_handshake_reported)
    {
        if(!mission_link_handshake_round_trip_complete())
        {
            return;
        }
        g_link_handshake_reported = 1u;
        board_log_puts((const char*)"tron: handshake ready\n");
        board_log_puts((const char*)"tron: waiting on mission roster, map and sites\n");
    }

    {
        /* mission_link_apply_roster() rejects a bad team_id or an
           out-of-grid location silently, so without this the wait below
           looks exactly like "no roster ever arrived" - two very different
           faults (bad DATA vs a dead LINK) with no way to tell them apart.
           Reported once per new reject. Keeps waiting deliberately: a later
           valid roster still resolves it. */
        uint32_t rejects = mission_link_roster_reject_count();

        if(rejects != g_link_reported_rejects)
        {
            BOARD_LOG((UB*)"tron: roster REJECTED (%lu so far) - arriving but failing validation\n",
                      (unsigned long)rejects);
            g_link_reported_rejects = rejects;
        }
    }

    /* Also waits for the whole route map and the safe zone/gateway sites
       (core/mission_link.h sections 5c/5d): the laptop sends them right
       after the rosters, and a tick run without them routes teams through
       blocked cells and leaves nowhere to rest. */
    if(mission_link_task_any_roster_applied() != 0u &&
       mission_link_map_rows_received() >= MISSION_LINK_MAP_HEIGHT &&
       mission_link_sites_received())
    {
        g_world_ready = 1u;
        (void)tk_set_flg(g_flg_sim, FLG_SIM_WORLD_READY);
    }
}

/* One pass of the link task with the comms mutex held. */
static void board_link_pass(void)
{
    uint32_t start_tick;

    if(g_sim_in_tick)
    {
        /* The sim is inside a tick (and is in cbba_wait_poll()): service the
           radio and the UART only. Applying laptop frames here would change
           simulation state mid-tick. */
        sx1262_poll_rx();
        comms_transport_dispatch_pending();
        mission_link_task_usart1_pump();
        return;
    }

    /* Between ticks: the full step. */
    sx1262_poll_rx();
    mission_link_task_step();
    comms_transport_dispatch_pending();

    if(mission_link_clock_step_plan(g_current_tick, mission_link_authorized_tick(), &start_tick))
    {
        (void)tk_set_flg(g_flg_sim, FLG_SIM_TICK_AUTH);
    }
}

LOCAL void link_task(INT stacd, void *exinf)
{
    UINT ptn;

    (void)stacd;
    (void)exinf;

    /* Nothing to do until the sim task has finished the boot init (radio,
       comms_transport, handshake state) that this task's work depends on. */
    (void)tk_wai_flg(g_flg_link, FLG_LINK_GO, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);
    board_log_puts((const char*)"tron: link task running\n");

    for(;;)
    {
        /* Wakes on a USART1 byte (ISR), the 10 ms poll handler (radio, heartbeat
           check), or the sim finishing a tick; the timeout is only a safety net. */
        ER er = tk_wai_flg(g_flg_link, FLG_LINK_RX | FLG_LINK_POLL | FLG_LINK_POST_TICK,
                           TWF_ORW | TWF_BITCLR, &ptn, 100);

        if(er != E_OK)
        {
            ptn = 0u; /* timeout: still run a pass */
        }

        (void)tk_loc_mtx(g_mtx_comms, TMO_FEVR);
        if(g_world_ready)
        {
            board_link_pass();
        }
        else
        {
            board_link_pass_prerun();
        }
        (void)tk_unl_mtx(g_mtx_comms);

        if(ptn & FLG_LINK_POST_TICK)
        {
            (void)tk_set_flg(g_flg_sim, FLG_SIM_LINK_STEPPED);
        }
    }
}

LOCAL void sim_compute_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    /* SX1262 hardware bring-up (SPI5 + GPIO config) - must happen
       before anything below tries to send/receive over LoRa, and
       doesn't depend on sim_runner_init()/mission_link state, so it
       goes first. See sx1262_hal.c's own top-of-file comment for the
       wiring this assumes. */
    board_log_puts((const char*)"tron: sim compute task entered\n");
    sx1262_hal_init();

    /* Size CBBA's reply-wait budget from the kernel tick (see
       cbba_wait_poll()). */
    cbba_set_reply_wait_steps(CBBA_ROUND_TIMEOUT_MS / BOARD_CBBA_WAIT_STEP_MS);

    /* Real (interrupt-driven) USART1 RX - see usart1_driver.c's own
       top-of-file note for why this has to be a real NVIC interrupt
       rather than a polled read: a polled board_usart1_read_byte()
       demonstrably loses bytes to overrun whenever comms_transport's
       relay-to-peer SPI attempts (below, in the wait loop) block the
       main loop for a while, which they do on every retry when no
       SX1262 module is actually attached. Must run before the wait
       loop below, same as sx1262_hal_init() above - both are one-time
       hardware bring-up, not per-tick work. */
    board_usart1_set_rx_notify(g_flg_link, FLG_LINK_RX);
    board_usart1_rx_init();

    board_log_puts((const char*)"tron: sim_runner_init\n");

    if(sim_runner_init() != 0)
    {
        board_log_puts((const char*)"tron: equipment requirement matrix validation FAILED\n");
        tk_slp_tsk(TMO_FEVR);
        return;
    }

    /*
     * Closes the gap both comments below have been flagging since
     * they were written: platform bring-up code (this function) sets
     * this board's real id and initializes comms_transport BEFORE
     * anything that depends on either. THIS_BOARD_ID (see its own doc
     * above this function) is a compile-time -D, resolved once here.
     */
    g_comms_transport_this_board_id = THIS_BOARD_ID;
    comms_transport_init();
    board_seed_lora_session_epoch();
    {
        uint32_t cr1 = board_usart1_enable_rx_fifo();

        /* Proof, on the console, that the FIFO enable reached the real
           (secure) USART1: expect usart1=0x52001000 fifoen=1 ue=1 re=1
           rxneie=1. fifoen=0 would mean the write missed the peripheral
           the BSP's console runs on. */
        BOARD_LOG((UB*)"tron: USART1 addr=0x%08lx fifoen=%u ue=%u re=%u rxneie=%u cr1=0x%08lx\n",
                  (unsigned long)(uintptr_t)USART1_S,
                  (unsigned)((cr1 & USART_CR1_FIFOEN) != 0u),
                  (unsigned)((cr1 & USART_CR1_UE) != 0u),
                  (unsigned)((cr1 & USART_CR1_RE) != 0u),
                  (unsigned)((cr1 & USART_CR1_RXNEIE_RXFNEIE) != 0u),
                  (unsigned long)cr1);
    }

#ifdef LORA_RAW_RX_CHECK
    /*
     * TEMPORARY bring-up diagnostic, not part of the real application
     * flow - see board/Raspberry_Pi_Pico/main_board.c's own
     * LORA_PING_TEST_MODE block for the peer side (sends a fixed
     * "PONG" payload every ~2s over the real comms_transport/
     * lora_frame/AEAD path).
     *
     * Why this can't be noise: comms_transport_deliver_received() is
     * only ever called from sx1262_poll_rx() after
     * lora_frame_try_parse() returns success, and that function's own
     * doc (core/lora_frame.c) is explicit that the additive checksum
     * is just a cheap pre-filter ahead of the real authentication -
     * core/aead_chacha20poly1305.c's Poly1305 MAC verification, a
     * 128-bit tag. Random RF noise or another device's unrelated
     * transmission has a ~1-in-2^128 chance of producing a valid tag
     * by chance - anything printed below decrypted successfully
     * against this project's pre-shared key, which only this
     * codebase's boards have. It is cryptographic proof of a genuine
     * peer transmission, not a coincidence.
     *
     * Bounded window (not a permanent hook): runs for
     * LORA_RAW_RX_CHECK_TICKS polls, printing anything received, then
     * falls through to the real application below regardless of
     * outcome - never steals frames from the real handshake/mission-
     * link path once this window ends. Remove the
     * -DLORA_RAW_RX_CHECK build define (or delete this block) once
     * you're done link-checking; nothing else in this file changes.
     */
    {
        uint32_t i;
        uint32_t rx_count = 0u;

        board_log_puts((const char*)"tron: LORA_RAW_RX_CHECK - listening for authenticated frames (~15s)\n");

        for (i = 0u; i < LORA_RAW_RX_CHECK_TICKS; i++)
        {
            uint8_t sender_team_id;
            uint8_t frame_type;
            uint8_t payload[LORA_FRAME_MAX_PAYLOAD];
            uint8_t payload_len;

            sx1262_poll_rx();
            /* Needed for session control too (core/lora_crypto.h): a peer
               that stayed up from an earlier boot of this board holds our
               frames until our challenge RESPONSE goes out through here. */
            comms_transport_dispatch_pending();

            while (comms_transport_poll_received(&sender_team_id, &frame_type,
                                                   payload, &payload_len) == 1)
            {
                char line[LORA_FRAME_MAX_PAYLOAD + 1];
                uint8_t k;
                uint8_t safe_len = payload_len;

                if (safe_len >= sizeof(line) - 1u)
                {
                    safe_len = (uint8_t)(sizeof(line) - 2u);
                }
                for (k = 0u; k < safe_len; k++)
                {
                    /* Printable-only guard - a non-ASCII byte becomes
                       '.' rather than corrupting the UART line; this
                       is a display concern only, not a re-check of
                       authenticity (already proven above). */
                    uint8_t c = payload[k];
                    line[k] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '.';
                }
                line[safe_len] = '\0';

                rx_count++;
                BOARD_LOG((UB*)"tron: AUTHENTICATED RX #%lu from team %u, type %u, %u bytes: %s\n",
                          (unsigned long)rx_count, (unsigned)sender_team_id,
                          (unsigned)frame_type, (unsigned)payload_len, line);
            }

            tk_dly_tsk(15);   /* ~15ms/poll * LORA_RAW_RX_CHECK_TICKS - see
                                  that macro's own doc for the resulting
                                  window length. */
        }

        BOARD_LOG((UB*)"tron: LORA_RAW_RX_CHECK done - %lu authenticated frame(s) received\n",
                   (unsigned long)rx_count);
    }
#endif /* LORA_RAW_RX_CHECK */

    /* Once, before the seed loop - see mission_link_task.c's own
       handoff note 1. Must follow whatever sets
       g_comms_transport_this_board_id / calls comms_transport_init()
       per that same note (now satisfied, two lines up); harmless to
       call ahead of that ordering landing elsewhere too, since this
       only zeroes this file's own per-team "last sent" bookkeeping
       (mission_link_task.c's g_last_sent_status[]) and doesn't touch
       comms_transport state itself. */
    mission_link_task_init();

    /*
     * Boot-side half of the handshake readiness gate — see the extern
     * block above this function for the full design note (role-based,
     * not chip-based) and the known gap (nothing sends/receives real
     * handshake frames yet, so this currently blocks forever on real
     * hardware). Ordering: must follow whatever sets
     * g_comms_transport_this_board_id, same requirement
     * mission_link_task_init() already has a few lines up (now
     * satisfied — see above) — this call uses that same variable's
     * current value.
     */
    mission_link_handshake_board_init(g_comms_transport_this_board_id);

    /*
     * This board's own local init (sim_runner_init, mission_link_task_
     * init, the handshake state machine itself, everything above this
     * line) is done — declare readiness. NOT the same as the seed loop
     * being allowed to start: mission_link_handshake_local_is_ready()
     * also requires the incoming request to have been seen (see
     * mission_link_handshake_build_local_reply()'s own two-condition
     * check) — that half arrives over whichever transport this board
     * has (USART1 direct, or LoRa relay), driven by mission_link_task_
     * step() in the wait loop just below.
     */
    mission_link_handshake_mark_local_ready();

    board_log_puts((const char*)"tron: waiting on mission-link handshake\n");

    /* Boot init is done: hand the radio and the laptop link to the link task,
       which runs the handshake and the roster/map/sites waits (it used to be
       the two spin loops here) and sets FLG_SIM_WORLD_READY when they are
       satisfied. */
    (void)tk_set_flg(g_flg_link, FLG_LINK_GO);

    {
        UINT ptn;

        (void)tk_wai_flg(g_flg_sim, FLG_SIM_WORLD_READY, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);
    }

    board_log_puts((const char*)"tron: roster, map and sites received, starting seed loop\n");

    board_dwt_init();

    for(uint32_t seed = BOARD_SEED; seed < BOARD_SEED + BOARD_SEED_COUNT; ++seed)
    {
        BOARD_LOG((UB*)"tron: seed %lu start\n", (unsigned long)seed);

        /* sim_start_seed_board(), NOT sim_start_seed() — the board
         * variant, which does not self-generate a world. See
         * core/sim_runner.h's doc on both functions if this ever
         * looks like a typo; it is not one. */
        (void)tk_loc_mtx(g_mtx_comms, TMO_FEVR); /* it writes simulation state */
        sim_start_seed_board(seed, BOARD_FAULT_PROFILE);
        (void)tk_unl_mtx(g_mtx_comms);

        while(g_current_tick < BOARD_TICKS_PER_SEED)
        {
            uint32_t start_tick;
            uint32_t tick;
            UINT ptn;

            /* Blocks until the link task has seen the laptop authorise a tick
               (it keeps receiving, relaying and heartbeating meanwhile). */
            (void)tk_wai_flg(g_flg_sim, FLG_SIM_TICK_AUTH, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);

            /* The whole tick, hash included, runs with the comms mutex held
               (cbba_wait_poll() releases it only for the length of a wait
               step), so the link task cannot apply a laptop frame mid-tick. */
            (void)tk_loc_mtx(g_mtx_comms, TMO_FEVR);

            if(!mission_link_clock_step_plan(g_current_tick, mission_link_authorized_tick(),
                                             &start_tick))
            {
                (void)tk_unl_mtx(g_mtx_comms);
                continue;
            }

            if(start_tick != g_current_tick)
            {
                BOARD_LOG((UB*)"tron: tick %lu is too far behind the laptop's %lu, joining at %lu\n",
                          (unsigned long)g_current_tick,
                          (unsigned long)mission_link_authorized_tick(),
                          (unsigned long)(start_tick + 1u));
                g_current_tick = start_tick;
            }

            {
                uint32_t cycles_before = board_dwt_cycles();

                g_sim_in_tick = 1u;
                board_wdog_arm(BOARD_SIM_WDOG_MS);
                sim_tick();
                board_wdog_disarm();
                g_sim_in_tick = 0u;

                /* Wraps cleanly even if CYCCNT itself wraps (32-bit
                   unsigned subtraction), same as every other free-
                   running-counter delta in this codebase (cbba_now_ms()
                   above relies on the same property for SYSTIM). */
                g_tick_cycles_last = board_dwt_cycles() - cycles_before;
                if(g_tick_cycles_last > g_tick_cycles_max)
                {
                    g_tick_cycles_max = g_tick_cycles_last;
                }
            }
            tick = g_current_tick;

#if BOARD_STATE_HASH_INTERVAL > 0u
            if((tick % BOARD_STATE_HASH_INTERVAL) == 0u)
            {
                StateHash_t sh;

                state_hash_compute(&sh);
                /* tm_printf has no 64-bit conversions: each hash goes out
                   as two zero-padded 32-bit halves, which concatenate to
                   the same 16 hex digits the PC prints. */
                BOARD_LOG((UB*)"STATEHASH tick=%lu all=%08lx%08lx teams=%08lx%08lx inc=%08lx%08lx flt=%08lx%08lx rec=%08lx%08lx\n",
                          (unsigned long)tick,
                          (unsigned long)(sh.all >> 32), (unsigned long)(uint32_t)sh.all,
                          (unsigned long)(sh.teams >> 32), (unsigned long)(uint32_t)sh.teams,
                          (unsigned long)(sh.incidents >> 32), (unsigned long)(uint32_t)sh.incidents,
                          (unsigned long)(sh.faults >> 32), (unsigned long)(uint32_t)sh.faults,
                          (unsigned long)(sh.recovery >> 32), (unsigned long)(uint32_t)sh.recovery);
            }
#endif

            (void)tk_unl_mtx(g_mtx_comms);

            /* The between-ticks pass (radio, step, heartbeat) now belongs to
               the link task: ask for it and wait until it is done, which keeps
               the old order - tick, one step, next tick - so laptop frames that
               arrived during this tick are applied before the next one starts. */
            (void)tk_set_flg(g_flg_link, FLG_LINK_POST_TICK);
            (void)tk_wai_flg(g_flg_sim, FLG_SIM_LINK_STEPPED, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);

            if((tick % BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL) == 0u)
            {
                (void)board_snapshot_ring_drain();
                /* Throttled to the drain interval: during a post-reboot
                   lockout the auth-fail count can climb every tick. */
                (void)tk_loc_mtx(g_mtx_comms, TMO_FEVR);
                board_report_link_diagnostics();
                (void)tk_unl_mtx(g_mtx_comms);

                /* Task 7: per-tick CPU cost, on hardware. Same cadence
                   as the diagnostics just above rather than every tick -
                   this is a text status line over the same USART1 debug
                   channel (tm_printf), not a wire-format telemetry field
                   (see this build's report on why the binary MissionLink
                   Status_t schema stays untouched), so there is no
                   reason to print it more often than a human watching
                   the console would want a fresh number. */
                BOARD_LOG((UB*)"tron: tick cost last=%lu cyc (%lu.%02lu us) max=%lu cyc (%lu.%02lu us) @%lu MHz\n",
                          (unsigned long)g_tick_cycles_last,
                          (unsigned long)(g_tick_cycles_last / (BOARD_CPU_CLOCK_HZ / 1000000u)),
                          (unsigned long)(((uint64_t)g_tick_cycles_last * 100u / (BOARD_CPU_CLOCK_HZ / 1000000u)) % 100u),
                          (unsigned long)g_tick_cycles_max,
                          (unsigned long)(g_tick_cycles_max / (BOARD_CPU_CLOCK_HZ / 1000000u)),
                          (unsigned long)(((uint64_t)g_tick_cycles_max * 100u / (BOARD_CPU_CLOCK_HZ / 1000000u)) % 100u),
                          (unsigned long)(BOARD_CPU_CLOCK_HZ / 1000000u));
            }

        }

        /* Final drain: flush whatever the last partial interval left
         * pending so a run's last few events aren't stuck in the
         * ring until the next seed's ticks happen to drain them (or
         * lost if this was the last seed).
         * Bug fix: this used to be a single call, only correct as
         * long as BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL (32) covers
         * whatever accumulated since the last periodic drain - the
         * comment itself flagged this as an assumption, not a
         * guarantee. If exceeded, snapshot_begin_run()'s unconditional
         * ring reset at the start of the next seed would have silently
         * and permanently discarded the remainder (worse on the very
         * last seed, since nothing drains after sim_runner_shutdown()
         * at all). Loop until the ring is actually empty instead of
         * assuming one call is enough - board_snapshot_ring_drain()
         * returns 0 once there's nothing left to drain. */
        while(board_snapshot_ring_drain() > 0u)
        {
        }

        sim_end_seed();
        BOARD_LOG((UB*)"tron: seed %lu done\n", (unsigned long)seed);
    }

    sim_runner_shutdown();
    board_log_puts((const char*)"tron: run complete\n");

    /* Mirrors the laptop's process exit: nothing left to do. Sleep
     * forever rather than returning, per BSP2 manual section 4.3.2 --
     * returning from usermain() shuts down µT-Kernel entirely, which
     * is very unlikely to be what you want immediately after a run
     * completes on real hardware (vs. e.g. waiting for a USB command
     * to start another run once that exists). */
    tk_slp_tsk(TMO_FEVR);
}

EXPORT INT usermain(void)
{
    tm_com_init(); /* initializes USART1 - must run before any console output */

    /* The logger task and the UART lock exist before anything prints, so all
       later console output goes through the message buffer. If it cannot be
       created, output stays direct (board_log_emit falls back). */
    {
        int log_rc = board_log_start();

        if (log_rc != 0)
        {
            BOARD_LOG_N(96, (UB*)"tron: board_log_start FAILED rc=%d (console stays direct)\n", log_rc);
        }
    }

    /* One line, once, before anything else runs: makes the actually-
       running build config visible on the wire instead of assumed.
       Exists because the CubeIDE project and the Makefile's
       DK_SINGLE_BOARD_FLAGS are two separate places that both have to
       agree for a single-board flash to actually behave as single-
       board (see Makefile's check-cubeide-single-flags, which catches
       the same drift at build time - this catches it on a board
       someone forgot to reflash, or flashed from a stale project).
       teams_per_board is derived from the two public quantities every
       build already exposes (NUM_TEAMS, comms_transport_board_count())
       rather than printing COMMS_TRANSPORT_TEAMS_PER_BOARD directly -
       that macro's default lives in comms_transport.c, private to
       that file (see its own comment), not something this file should
       reach into. */
    /* Static buffer, not the stack: this task has only 1 KB. */
    {
        static UB cfg_line[BOARD_LOG_LINE_MAX];
        INT cfg_len = tm_sprintf(cfg_line, (const UB*)"tron: CONFIG teams_per_board=%u vbus=%u consensus_enforce=%u radio=%u model_loaded=%u cpu_hz=%lu icache=%u dcache=%u\n",
               (unsigned)(NUM_TEAMS / comms_transport_board_count()),
               (unsigned)VIRTUAL_BUS_ENABLED,
               (unsigned)CONSENSUS_ENFORCE,
               (unsigned)BOARD_RADIO_ENABLED,
               (unsigned)FAULT_MODEL_LOADED,
               (unsigned long)SystemCoreClock,
               (unsigned)((SCB->CCR & SCB_CCR_IC_Msk) != 0u),
               (unsigned)((SCB->CCR & SCB_CCR_DC_Msk) != 0u));
        board_log_emit(cfg_line, cfg_len);
    }

    {
        int rtos_rc = board_rtos_create();

        if (rtos_rc != 0)
        {
            BOARD_LOG_N(96, (UB*)"tron: board_rtos_create FAILED rc=%d\n", rtos_rc);
            return 1;
        }
    }

    board_log_puts((const char*)"tron: starting simulation task\n");

    /* Link task first (it parks on FLG_LINK_GO), then the sim compute task,
       which does the boot init and releases it. Both on static stacks. */
    link_tskid = board_rtos_spawn((FP)link_task, BOARD_PRI_LINK, g_link_stack,
                                  sizeof(g_link_stack), 0);
    if(link_tskid <= 0)
    {
        BOARD_LOG_N(96, (UB*)"tron: link task start FAILED (%d)\n", (int)link_tskid);
        return 1;
    }
    BOARD_LOG_N(96, (UB*)"tron: link task created (pri %d)\n", BOARD_PRI_LINK);

    /* Bug fix (kept from the single-task version): a failed start used to go
       unnoticed and leave the board silently parked below. */
    sim_tskid = board_rtos_spawn((FP)sim_compute_task, BOARD_PRI_SIM, g_sim_stack,
                                 sizeof(g_sim_stack), 0);
    if(sim_tskid <= 0)
    {
        BOARD_LOG_N(96, (UB*)"tron: sim compute task start FAILED (%d)\n", (int)sim_tskid);
        return 1;
    }
    BOARD_LOG_N(96, (UB*)"tron: sim compute task created (pri %d)\n", BOARD_PRI_SIM);

    if(board_rtos_start_cyclics() != 0)
    {
        board_log_puts((const char*)"tron: cyclic handler start FAILED\n");
        return 1;
    }

    /* usermain's own task stays parked forever; the link and sim compute
     * tasks do the actual work. Per BSP2 manual: if usermain returns,
     * µT-Kernel shuts down, which would tear down every task with it. */
    tk_slp_tsk(TMO_FEVR);

    return 0;
}
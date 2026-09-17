/*
 * Board entry point (Raspberry Pi Pico / RP2040, µT-Kernel 3.0 BSP,
 * v1 — see board/Raspberry_Pi_Pico/vendor/README.md for why this is
 * NOT the same BSP as board/STM32N6570-DK/'s, and for what's
 * genuinely different about this target vs. that one).
 *
 * This is the on-device analogue of laptop/main.c: it drives
 * the same core/sim_runner.h lifecycle (sim_runner_init ->
 * sim_start_seed_board -> sim_tick* -> sim_end_seed ->
 * sim_runner_shutdown), so the tick sequence can never silently
 * diverge between platforms. What's platform-specific here is only
 * how the loop gets scheduled (a µT-Kernel task instead of a plain C
 * for-loop) and how progress gets reported (tm_printf over USART1
 * instead of stdout) — same as board/STM32N6570-DK/main_board.c, this
 * file was ported from.
 *
 * NOT YET WIRED INTO A REAL FIRMWARE PROJECT. This file compiles
 * cleanly against the real BSP (v1) headers (verified with
 * arm-none-eabi-gcc -mcpu=cortex-m0plus, see vendor/README.md), but
 * turning it into a flashable image still needs the BSP's own Eclipse
 * CDT project (board/Raspberry_Pi_Pico/vendor/mtk3_bsp/.project —
 * confirmed real, `org.eclipse.cdt.managedbuilder.core.genmakebuilder`
 * — NOT STM32CubeIDE/STM32CubeMX; see this board's own vendor/
 * README.md for why that assumption from the original handoff doc was
 * wrong) to actually include this file and build it.
 *
 * Genuinely SIMPLER than the DK's equivalent step, confirmed by
 * reading the real boot chain, not assumed: this BSP owns its own
 * `Reset_Handler` (kernel/sysdepend/cpu/core/armv6m/reset_hdl.c),
 * which calls `knl_startup_hw()` directly — there is no CubeMX-
 * generated `Core/Src/main.c` skeleton here to manually inject a
 * `knl_start_mtkernel()` call into (that whole step, and the
 * TrustZone secure/non-secure split alongside it, is a STM32Cube-
 * specific thing that doesn't exist for this BSP at all). `usermain()`
 * below is called automatically once the boot chain reaches it — the
 * remaining IDE work is "add this project's core/ and
 * board/Raspberry_Pi_Pico/ sources to the BSP's own Eclipse CDT
 * project's build settings/include paths", not project-generation +
 * manual entry-point wiring the way the DK needs. Still IDE work, not
 * something any C source file can do — same "tracked as the next
 * concrete step once you're ready to actually flash the board, not
 * part of this pass" caveat as the DK's own file.
 * source file can do — tracked as the next concrete step once you're
 * ready to actually flash the board, not part of this pass.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <bsp/libbsp.h>        /* gpio_set_pin()/gpio_set_val() - onboard
                                   LED heartbeat below (BOARD_LED_PIN),
                                   same header board/Raspberry_Pi_Pico/
                                   sx1262_hal.c already uses for its own
                                   GPIO work. */

#include "sim_runner.h"
#include "fault_config.h"
#include "mission_state.h"       /* g_current_tick - clock-driven tick loop */
#include "mission_link_codec.h" /* mission_link_roster_reject_count() -
                                    roster-wait loop's reject diagnostic */
#include "comms_transport.h" /* g_comms_transport_this_board_id,
                                 comms_transport_init() - see
                                 THIS_BOARD_ID below */

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
 * _PICO_RP2040_ (this board) and _STM32CUBE_DISCOVERY_N657_ (the
 * other board target) already work as per-target -D flags in this
 * project - which physical board a given firmware image is FOR is
 * already a build-time fact everywhere else in this project, this is
 * the same kind of fact, not a new class of one. Raspberry Pi Pico is
 * board id 1 - see comms_transport.h's own doc: "team_id / 5" is the
 * board-assignment rule, so board 1 owns teams 5-9, and this board is
 * the LoRa-relay-only one (no direct laptop-facing USART1 - see
 * mission_link_task.c's own role-based, not chip-based, gate on this
 * same id; that gate is what actually decides who has a laptop link,
 * this default is just this board's own identity, not a claim about
 * having one).
 */
#ifndef THIS_BOARD_ID
#define THIS_BOARD_ID 1u
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
 * pass (board_sleep_ms(), which polls the radio). */
#ifndef BOARD_CLOCK_IDLE_POLL_MS
#define BOARD_CLOCK_IDLE_POLL_MS 5u
#endif

/* Cadence for draining the snapshot ring (board/Raspberry_Pi_Pico/snapshot.c)
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
 * Onboard LED heartbeat - added purely as a bring-up aid. This board
 * currently has NO way to observe program state without extra
 * hardware (no UART adapter, no SWD debugger on hand as of this
 * writing - see this file's own commit history/chat record for that
 * discussion) other than the RP2040's own native-USB port, which
 * only serves the boot ROM's BOOTSEL mass-storage bootloader and has
 * nothing listening on it once this firmware is running (no USB CDC
 * stack anywhere in this codebase - confirmed by search, not
 * assumed). The one thing available with truly zero extra hardware
 * is the Pico board's own onboard LED, wired to GP25 on every
 * standard (non-W, non-2) Raspberry Pi Pico. This is NOT a
 * substitute for real UART/SWD bring-up once either is available -
 * it only answers "did the board get this far", never "why did it
 * stop" - but it's strictly better than flying fully blind on a
 * first flash. See board_led_init()/board_led_pulse()/
 * board_led_blink_n() below and their call sites in sim_task() for
 * what each blink pattern means. Safe to delete this whole block
 * once real UART/SWD bring-up is in place and this stops being
 * useful. */
#ifndef BOARD_LED_PIN
#define BOARD_LED_PIN   25u   /* Onboard LED on every standard Pico
                                 (RP2040) board - NOT one of this
                                 file's own SX1262_PIN_* radio pins,
                                 no conflict with sx1262_hal.c. If
                                 you're on a Pico W or Pico 2 W, this
                                 pin is wired to the wireless chip
                                 instead of a plain GPIO on real
                                 hardware and this default will need
                                 overriding (or the LED code skipping
                                 entirely) - override at build time
                                 (-D) if so. */
#endif
#ifndef BOARD_LED_HEARTBEAT_MS
#define BOARD_LED_HEARTBEAT_MS 2000u  /* wall-clock period of the mission
                                         loop's short "still alive" pulse.
                                         Milliseconds, not ticks: a tick
                                         can take 36 s in a real-time
                                         mission. */
#endif

/*
 * Wait-loop heartbeat cadence/pattern - separate from
 * BOARD_LED_HEARTBEAT_MS above on purpose. The handshake
 * and roster wait loops (see sim_task() below) have no pacing
 * at all - they spin as fast as
 * sx1262_rx_poll()/mission_link_task_step()/comms_transport_
 * dispatch_pending() themselves take, unthrottled - so a much larger
 * iteration count is needed here than in the tick loop just to get a
 * human-readable blink rate, and even then it's still just "roughly
 * this often", not a real timer. Each wait loop also gets its own
 * DISTINCT blink pattern (single vs double) so "still waiting on the
 * handshake" and "still waiting on the roster" don't look identical
 * on the LED - see board_led_wait_pulse_handshake()/
 * board_led_wait_pulse_roster() below. */
#ifndef BOARD_LED_WAIT_PULSE_TICK_INTERVAL
#define BOARD_LED_WAIT_PULSE_TICK_INTERVAL 4000u
#endif

/*
 * Not part of snapshot.h's public interface (core/ never calls
 * this) -- same status as board_snapshot_ring_pending(), which is
 * also forward-declared at its one call site rather than added to
 * snapshot.h, since both are board-platform-internal telemetry
 * plumbing, not part of the portable core/snapshot.h contract other
 * platforms implement. Defined in board/Raspberry_Pi_Pico/snapshot.c.
 */
extern uint32_t board_snapshot_ring_drain(void);

/*
 * Mission-link heartbeat trigger hook (this prompt's addition -
 * additive only, coordinate with Prompt 4's roster-wait diagnostic
 * hook in this same file, different concern). mission_link_task_step()
 * (board/Raspberry_Pi_Pico/mission_link_task.c) is what actually decides
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
 * SX1262 radio bring-up + RX ingestion (board/Raspberry_Pi_Pico/
 * sx1262_hal.c) - mirrors board/STM32N6570-DK/main_board.c's own
 * sx1262_hal_init() extern/call just above this file's DK
 * counterpart. sx1262_hal_init() does one-time bring-up (SPI/GPIO
 * config, DIO1 IRQ routing, first RX arm) and must run before
 * anything below tries to send/receive over LoRa - same reasoning as
 * the DK's own call ordering. sx1262_rx_poll() is the per-tick RX
 * pump: cheap (a single GPIO read) when nothing has arrived, called
 * at the same cadence as mission_link_task_step() above so an
 * incoming frame doesn't sit unread for a whole seed's worth of
 * ticks.
 */
extern int sx1262_hal_init(void);
extern void sx1262_rx_poll(void);

/*
 * Tiered TX dispatcher (core/comms_transport.c) - the piece that
 * actually moves a cross-board comms_transport_send() from "enqueued"
 * to "on air". Must be called every place this file already pumps
 * sx1262_rx_poll()/mission_link_task_step(), including the two wait-
 * loops below (handshake/roster) - a handshake or roster relay
 * message enqueued during those phases would otherwise never flush,
 * since nothing else calls this. See core/comms_transport.h's own
 * doc on this function for the full dispatch contract.
 */
extern void comms_transport_dispatch_pending(void);

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

/*
 * Handshake readiness gate (this prompt's addition — read alongside
 * the matching gate in board/Raspberry_Pi_Pico/mission_link_task.c's RX
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
        tm_printf((UB*)"tron: LoRa AUTH FAIL (%lu so far) - frames arriving but failing authentication (RF corruption, a peer built with a different key, or a replay)\n",
                  (unsigned long)auth_fails);
        g_reported_auth_fails = auth_fails;
    }

    {
        uint32_t holds = lora_frame_session_hold_count();

        if(holds != g_reported_session_holds)
        {
            tm_printf((UB*)"tron: LoRa session HOLD (%lu so far) - a peer started a new session (reboot?); its frames wait until it answers this board's challenge\n",
                      (unsigned long)holds);
            g_reported_session_holds = holds;
        }
    }

    {
        uint32_t reboots = mission_link_task_peer_reboot_count();

        if(reboots != g_reported_peer_reboots)
        {
            tm_printf((UB*)"tron: peer reboot detected (%lu so far) - re-running that board's handshake\n",
                      (unsigned long)reboots);
            g_reported_peer_reboots = reboots;
        }
    }

    if(!g_reported_first_frame && lora_frame_accept_count() > 0u)
    {
        tm_putstring((UB*)"tron: LoRa link alive - first authenticated frame received\n");
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
                tm_printf((UB*)"tron: peer board %lu never answered the handshake (timed out) - the laptop will not send mission data until it does; check that board is powered, flashed with this build, and on the same radio params\n",
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
        tm_putstring((UB*)"tron: WARNING - SX1262 RNG unavailable; LoRa session epoch is weak, reboot recovery may fail\n");
        if(epoch == 0u)
        {
            epoch = 0x5EED0001u ^ (uint32_t)THIS_BOARD_ID;
        }
    }

    lora_crypto_set_tx_epoch(epoch);
    tm_printf((UB*)"tron: LoRa session epoch %08lx\n", (unsigned long)epoch);
}

/* ---------------------------------------------------------------
 * USART1 byte I/O, CBBA timing hooks, radio-servicing sleep
 * --------------------------------------------------------------- */
extern void mission_link_task_usart1_pump(void);

/* The BSP's raw console send (lib/libtm/libtm.h). */
IMPORT void tm_snd_dat(const UB *buf, INT size);

/* Raw write via tm_snd_dat(): the BSP's tm_putchar() sends a CR before
   every LF byte, which corrupted binary frames containing 0x0A (found by
   review). Interrupts masked per byte, as tm_putchar() does. */
void board_usart1_write(const uint8_t *bytes, uint32_t len)
{
    uint32_t i;
    UINT imask;

    for (i = 0u; i < len; i++)
    {
        DI(imask);
        tm_snd_dat((const UB *)&bytes[i], 1);
        EI(imask);
    }
}

/* Non-blocking read of the console UART (UART0, the same registers the
   BSP's pico_rp2040 tm_com.c drives; its 32-byte FIFO is enabled there).
   The BSP only offers a blocking tm_getchar(), and nothing defined this
   function, so a Pico built as board 0 could not receive from the laptop.
   Error bits in the data register are ignored: a damaged byte fails the
   frame checksum and mission_link_task.c's parser resyncs. */
#define BOARD_UART0_DR   (*(_UW *)0x40034000u)
#define BOARD_UART0_FR   (*(_UW *)0x40034018u)
#define BOARD_UART0_FR_RXFE (1u << 4)

int board_usart1_read_byte(void)
{
    if ((BOARD_UART0_FR & BOARD_UART0_FR_RXFE) != 0u)
    {
        return -1;
    }
    return (int)(BOARD_UART0_DR & 0xFFu);
}

/* Real milliseconds for CBBA's reply timeout and the status heartbeat
   (cbba.h requires this on hardware). Neither board used to define it, so
   both ran on cbba.c's simulated clock - 10 ms per call - and a "530 ms"
   CBBA wait took microseconds (found by review). If the kernel call ever
   fails, time still advances so no wait can spin forever. */
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

/* Services the radio and USART1 through CBBA's reply wait (see cbba.h):
   this board's queued bids go out and the peer's replies come in. */
void cbba_wait_poll(void)
{
    sx1262_rx_poll();
    comms_transport_dispatch_pending();
    mission_link_task_usart1_pump();
}

/* Set once the radio and comms_transport are up. */
static UB g_board_radio_serviced;

/* tk_dly_tsk() that keeps the radio serviced once it is up, in 5 ms
   slices. The LED patterns in both boot wait loops used to block for
   480-680 ms with nothing polling the radio; the SX1262 keeps only the
   last received packet, so frames arriving in that window were lost - and
   a lost roster is only recovered by the laptop's next resend (found by
   review). Before the radio is up this is a plain delay. */
LOCAL void board_sleep_ms(UINT ms)
{
    while (ms > 0u)
    {
        UINT slice = (ms > 5u) ? 5u : ms;

        tk_dly_tsk(slice);
        ms -= slice;

        if (g_board_radio_serviced)
        {
            sx1262_rx_poll();
            comms_transport_dispatch_pending();
            mission_link_task_usart1_pump();
        }
    }
}

/*
 * Onboard LED heartbeat helpers - see BOARD_LED_PIN's own doc above
 * for why this exists. gpio_set_pin()/gpio_set_val() are the exact
 * same <bsp/libbsp.h> calls board/Raspberry_Pi_Pico/sx1262_hal.c
 * already uses for its own GPIO setup, just against a different pin.
 */
LOCAL void board_led_init(void)
{
    gpio_set_pin(BOARD_LED_PIN, GPIO_MODE_OUT);
    gpio_set_val(BOARD_LED_PIN, 0);   /* start off, not left floating/undefined */
}

/* One on/off pulse, blocking for on_ms+off_ms via board_sleep_ms() (safe
   here - always called from within sim_task(), a real µT-Kernel
   task, never from an interrupt context). */
LOCAL void board_led_pulse(UINT on_ms, UINT off_ms)
{
    gpio_set_val(BOARD_LED_PIN, 1);
    board_sleep_ms(on_ms);
    gpio_set_val(BOARD_LED_PIN, 0);
    board_sleep_ms(off_ms);
}

LOCAL void board_led_blink_n(UINT count, UINT on_ms, UINT off_ms)
{
    UINT i;
    for(i = 0u; i < count; i++)
    {
        board_led_pulse(on_ms, off_ms);
    }
}

/* Handshake-wait pattern: ONE long, slow blink, then a long dark pause
   - visually distinct from board_led_wait_pulse_roster()'s three
   short blinks just below, and from the tick loop's own much-more-
   frequent single pulse (BOARD_LED_HEARTBEAT_TICK_INTERVAL). Widened
   from the original 80ms-on/400ms-off (too fast to read as "one
   blink" vs. "flickering") to a slow, unambiguous 600ms-on/1800ms-off
   - long enough on and off that the two states ("lit" vs. "dark") are
   never in doubt, and the on/off shapes of this pattern vs. the
   roster-wait pattern below read as different at a glance rather
   than needing a stopwatch. "Long pause" here is a real blocking
   delay (via board_led_pulse()'s off_ms, which keeps polling the
   radio - see board_sleep_ms()), not just a larger iteration-count
   gate - unlike the tick loop's heartbeat, this is fine to let
   block: these wait loops have no other pacing at all today (no
   BOARD_TICK_DELAY_MS equivalent), and even the widened ~2.4s full
   cycle here is still far faster than any realistic LoRa RX
   turnaround, so it doesn't meaningfully delay noticing an incoming
   frame either. */
LOCAL void board_led_wait_pulse_handshake(void)
{
    board_led_pulse(600u, 1800u);
}

/* Roster-wait pattern: THREE short, sharp blinks back to back, then a
   long dark pause - see board_led_wait_pulse_handshake()'s own doc
   just above for the widened-timing reasoning; the shape (staccato
   triple vs. one slow pulse) is what's meant to be unmistakable here,
   not just the count. Total per-cycle time deliberately similar to
   the handshake pattern's ~2.4s (3*(150+150) + 1500 = 2400ms) so
   both patterns repeat at roughly the same rate - only the on/off
   shape differs. */
LOCAL void board_led_wait_pulse_roster(void)
{
    board_led_blink_n(3u, 150u, 150u);
    board_sleep_ms(1500u);   /* radio-polling delay, not a plain blocking
                           tk_dly_tsk() - see board_led_wait_pulse_handshake()'s
                           own doc just above for why this must not
                           starve sx1262_rx_poll() for 1.5s at a time. */
}

/* Roster-REJECTED pattern: FOUR quick blinks. Distinct from the plain
   roster wait's two, because the two states need very different
   responses and look identical without this: two blinks means nothing
   has arrived (peer not flashed yet, radio params mismatched), four
   means roster frames ARE arriving and being thrown away by
   mission_link_apply_roster()'s validation, so the wire is fine and the
   DATA is wrong. Without the split, a malformed roster field looks
   exactly like a dead link forever - and this board has no UART adapter
   or SWD debugger attached in the bring-up case this LED exists for
   (see BOARD_LED_PIN's own doc), so the LED is the only channel that
   difference can arrive on. */
LOCAL void board_led_wait_pulse_roster_rejected(void)
{
    board_led_blink_n(4u, 80u, 120u);
    board_sleep_ms(120u);
}

/* Auth-FAILED pattern: THREE quick blinks, used by both wait loops once
   lora_frame_auth_fail_count() is nonzero. Means LoRa frames ARE
   arriving but failing authentication - e.g. a peer that rebooted while
   this board stayed up, locked out by this board's replay floor (see
   board_report_link_diagnostics()) - as opposed to one blink / two
   blinks (nothing usable arrived) or four (roster authenticated but
   invalid). */
LOCAL void board_led_wait_pulse_auth_failed(void)
{
    board_led_blink_n(3u, 80u, 120u);
    board_sleep_ms(200u);
}

LOCAL void sim_task(INT stacd, void *exinf);
LOCAL ID sim_tskid;
LOCAL T_CTSK sim_ctsk = {
    .itskpri = 10,
    .stksz   = 24576,          /* core/ uses static globals, not stack,
                                * for all sim state (grid, teams,
                                * incidents) -- this only needs to
                                * cover call-depth locals (A* search
                                * frames, CBBA bundle evaluation),
                                * not sim data. 8KB(now added to 24kb after a stack overflow) is a starting
                                * point; check high-water mark once
                                * you can actually run this and tune
                                * from there rather than guessing
                                * further. */
    .task    = sim_task,
    .tskatr  = TA_HLNG | TA_RNG3,   /* TA_FPU deliberately not set, and
                                      * NOT applicable here the way it
                                      * is for the DK's copy of this
                                      * file: RP2040 is dual Cortex-M0+,
                                      * which has no FPU at all (this
                                      * BSP's own config/config.h
                                      * confirms `USE_FPU (0)` for this
                                      * target) - there's no "ALWAYS_
                                      * FPU_ATR forces it anyway"
                                      * consideration to depend on
                                      * because there's no FPU context
                                      * to force. Left unset for the
                                      * same reason the DK's copy of
                                      * this struct doesn't set it
                                      * either (nothing here needs it),
                                      * not because of any equivalent
                                      * automatic-force behavior on
                                      * this target. */
};

LOCAL void sim_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;

    /* Heartbeat stage 1/3: one long pulse as soon as sim_task() is
       actually running - proves the kernel booted and scheduled this
       task at all, before anything below (including the radio init
       just below, the riskiest untested step) gets a chance to hang.
       See BOARD_LED_PIN's own doc above for full context. */
    board_led_init();
    board_led_pulse(300u, 200u);

    /* SX1262 hardware bring-up (SPI0 + GPIO config, DIO1 IRQ routing,
       first RX arm) - must happen before anything below tries to
       send/receive over LoRa, and doesn't depend on sim_runner_init()/
       mission_link state, so it goes first. Mirrors board/
       STM32N6570-DK/main_board.c's own sx1262_hal_init() call
       ordering - see sx1262_hal.c's own top-of-file comment for the
       wiring this assumes. */
    if(sx1262_hal_init() != 0)
    {
        tm_putstring((UB*)"tron: sx1262_hal_init FAILED\n");
        tk_slp_tsk(TMO_FEVR);
        return;
    }

    /* Heartbeat stage 2/3: three quick pulses once sx1262_hal_init()
       has RETURNED - if this never appears (only stage 1's single
       long pulse ever shows), radio bring-up hung, most likely on a
       BUSY-pin wait if the SX1262 module isn't wired up yet (or
       isn't wired up correctly - DIO1 in particular is still an
       unconfirmed placeholder pin as of this writing, see
       SX1262_PIN_DIO1's own doc in sx1262_hal.c). */
    board_led_blink_n(3u, 120u, 120u);

    tm_putstring((UB*)"tron: sim_runner_init\n");

    if(sim_runner_init() != 0)
    {
        tm_putstring((UB*)"tron: equipment requirement matrix validation FAILED\n");
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
    g_board_radio_serviced = 1u;

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

    tm_putstring((UB*)"tron: waiting on mission-link handshake\n");

    {
    /* Local, not file-scope: this counter only needs to survive
       across iterations of ONE wait loop at a time (reused, reset to
       0, for the roster-wait loop just below) - see BOARD_LED_
       WAIT_PULSE_TICK_INTERVAL's own doc above for why this is an
       approximate period, not a precise timer, same caveat as the
       per-tick loop's own copy of this pattern. Without this, a
       real second-board mismatch (e.g. SF/BW/CR/sync word not
       actually matching yet - a known open item) would look
       IDENTICAL on the LED to this board correctly waiting alone:
       both are "3 blinks then dark forever". This at least
       distinguishes "still spinning, still trying" from "actually
       hung", the same reason stage 3 exists in the tick loop below. */
    UINT wait_pulse_counter = 0u;

    while (!mission_link_handshake_round_trip_complete())
    {
        /* Pumps RX so the handshake request that completes this
           board's own readiness actually has a chance to arrive -
           not gated behind the readiness check itself (that would
           deadlock - this call is what MAKES the check eventually
           true).
           
           sx1262_rx_poll() FIRST, then mission_link_task_step():
           sx1262_rx_poll() is what actually reads the radio and
           pushes any received frame into comms_transport's inbox
           (via comms_transport_deliver_received() - see sx1262_hal.c);
           mission_link_task_step() only reads FROM that inbox (via
           comms_transport_poll_received() - see mission_link_task.c),
           it does not touch the radio itself. This split was missed
           here originally - this loop used to call only mission_
           link_task_step()/comms_transport_dispatch_pending(), same
           as the per-tick loop below, but sx1262_rx_poll() (added
           later, once real RX ingestion existed) never got backfilled
           into this loop or the roster-wait loop just below, so on
           real hardware neither wait loop could ever see an incoming
           LoRa frame regardless of what a peer board sent - confirmed
           by tracing the call chain, not assumed; fixed here and in
           the roster-wait loop below in the same pass. */
        sx1262_rx_poll();
        mission_link_task_step();
        comms_transport_dispatch_pending();
        board_report_link_diagnostics();
        board_boot_wait_discard_stale_frames();

        if((wait_pulse_counter % BOARD_LED_WAIT_PULSE_TICK_INTERVAL) == 0u)
        {
            if(lora_frame_auth_fail_count() > 0u)
            {
                board_led_wait_pulse_auth_failed();
            }
            else
            {
                board_led_wait_pulse_handshake();
            }
        }
        wait_pulse_counter++;
    }
    }

    tm_putstring((UB*)"tron: handshake ready\n");

    /*
     * Roster-arrival gate - see the mission_link_task_any_roster_
     * applied() extern's own doc above for why this is a distinct
     * condition from the handshake completing. Same shape as the
     * handshake wait loop just above, including the same sx1262_rx_
     * poll() requirement and the same reasoning for why it has to run
     * here too - see that loop's own comment for the full story. On
     * real hardware today this still blocks forever with only one
     * board flashed (nothing to receive a roster FROM), but as of
     * this fix it no longer blocks forever ONCE a second board
     * exists, the way it would have with sx1262_rx_poll() missing
     * from this loop.
     */
    /* Also waits for the whole route map and the safe zone/gateway sites
       (core/mission_link.h sections 5c/5d): the laptop sends them right
       after the rosters, and a tick run without them routes teams through
       blocked cells and leaves nowhere to rest. */
    tm_putstring((UB*)"tron: waiting on mission roster, map and sites\n");

    {
    UINT wait_pulse_counter = 0u;   /* fresh counter for this loop - see
                                        the handshake wait loop's own
                                        copy of this variable above for
                                        why it exists. */
    uint32_t reported_rejects = 0u; /* last reject count already printed,
                                        so a steady stream of rejects
                                        doesn't reprint on every pulse. */

    while (mission_link_task_any_roster_applied() == 0u ||
           mission_link_map_rows_received() < MISSION_LINK_MAP_HEIGHT ||
           !mission_link_sites_received())
    {
        uint32_t rejects;

        sx1262_rx_poll();
        mission_link_task_step();
        comms_transport_dispatch_pending();
        board_report_link_diagnostics();
        board_boot_wait_discard_stale_frames();

        rejects = mission_link_roster_reject_count();

        if((wait_pulse_counter % BOARD_LED_WAIT_PULSE_TICK_INTERVAL) == 0u)
        {
            if(rejects > 0u)
            {
                /* Deliberately keeps waiting rather than aborting: one
                   corrupted frame shouldn't end the mission, and a later
                   good roster still resolves this loop normally. The
                   point is only to stop the failure being invisible. */
                if(rejects != reported_rejects)
                {
                    tm_printf((UB*)"tron: roster REJECTED (%lu so far) - arriving but failing validation\n",
                              (unsigned long)rejects);
                    reported_rejects = rejects;
                }
                board_led_wait_pulse_roster_rejected();
            }
            else if(lora_frame_auth_fail_count() > 0u)
            {
                board_led_wait_pulse_auth_failed();
            }
            else
            {
                board_led_wait_pulse_roster();
            }
        }
        wait_pulse_counter++;
    }
    }

    tm_putstring((UB*)"tron: roster, map and sites received, starting seed loop\n");

    for(uint32_t seed = BOARD_SEED; seed < BOARD_SEED + BOARD_SEED_COUNT; ++seed)
    {
        tm_printf((UB*)"tron: seed %lu start\n", (unsigned long)seed);

        /* sim_start_seed_board(), NOT sim_start_seed() — the board
         * variant, which does not self-generate a world. See
         * core/sim_runner.h's doc on both functions if this ever
         * looks like a typo; it is not one. */
        sim_start_seed_board(seed, BOARD_FAULT_PROFILE);

        uint32_t last_heartbeat_ms = cbba_now_ms();

        while(g_current_tick < BOARD_TICKS_PER_SEED)
        {
            uint32_t start_tick;
            uint32_t tick;

            /* Heartbeat stage 3/3: one short pulse every
               BOARD_LED_HEARTBEAT_MS, for as long as the mission loop
               keeps running, whether or not a tick is due - proves the
               board is still alive and looping, not just that it got
               here once. If the board hangs somewhere later in this
               loop (radio stuck waiting on BUSY, a fault path spinning,
               etc.), this simply stops - the LED going dark and staying
               dark after stage 1/2 already showed is itself the
               signal. */
            if((cbba_now_ms() - last_heartbeat_ms) >= BOARD_LED_HEARTBEAT_MS)
            {
                board_led_pulse(30u, 0u);
                last_heartbeat_ms = cbba_now_ms();
            }

            if(!mission_link_clock_step_plan(g_current_tick, mission_link_authorized_tick(),
                                             &start_tick))
            {
                /* Waiting for the laptop's next tick: keep receiving,
                   relaying and heartbeating meanwhile. */
                mission_link_task_step();
                comms_transport_dispatch_pending();
                board_sleep_ms(BOARD_CLOCK_IDLE_POLL_MS);
                continue;
            }

            if(start_tick != g_current_tick)
            {
                tm_printf((UB*)"tron: tick %lu is too far behind the laptop's %lu, joining at %lu\n",
                          (unsigned long)g_current_tick,
                          (unsigned long)mission_link_authorized_tick(),
                          (unsigned long)(start_tick + 1u));
                g_current_tick = start_tick;
            }

            sim_tick();
            tick = g_current_tick;

            /* Heartbeat trigger hook (this prompt's addition - see the
               extern declarations above for scope). Every tick, not
               gated behind an interval like the snapshot-ring drain
               below: mission_link_status_tx_step() does its own
               cbba_now_ms()-based rate limiting internally, so calling
               this more often than the heartbeat interval just means
               the wall-clock check runs (and finds nothing due yet)
               a few extra times for free - cheap, non-blocking, no
               reason to add a second interval here to throttle it. */
            mission_link_task_step();

            /* RX pump, same cadence as mission_link_task_step() just
               above (every tick, not gated behind an interval) - see
               the sx1262_rx_poll() extern's own doc a few hundred
               lines up for why. */
            sx1262_rx_poll();

            /* TX dispatch - see the comms_transport_dispatch_pending()
               extern's own doc above. Same cadence as the RX pump
               just above; order relative to it doesn't matter (this
               only acts on the radio's current busy state and
               whatever's already queued, not on anything sx1262_rx_
               poll() itself produces). */
            comms_transport_dispatch_pending();

            if((tick % BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL) == 0u)
            {
                (void)board_snapshot_ring_drain();
                /* Throttled to the drain interval: during a post-reboot
                   lockout the auth-fail count can climb every tick. */
                board_report_link_diagnostics();
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
        tm_printf((UB*)"tron: seed %lu done\n", (unsigned long)seed);
    }

    sim_runner_shutdown();
    tm_putstring((UB*)"tron: run complete\n");

    /* Mirrors the laptop's process exit: nothing left to do. Sleep
     * forever rather than returning, per BSP (v1) manual section 4.3.2 --
     * returning from usermain() shuts down µT-Kernel entirely, which
     * is very unlikely to be what you want immediately after a run
     * completes on real hardware (vs. e.g. waiting for a USB command
     * to start another run once that exists). */
    tk_slp_tsk(TMO_FEVR);
}

EXPORT INT usermain(void)
{
    tm_putstring((UB*)"tron: starting simulation task\n");

    sim_tskid = tk_cre_tsk(&sim_ctsk);
    if(sim_tskid <= 0)
    {
        tm_putstring((UB*)"tron: tk_cre_tsk FAILED\n");
        return 1;
    }

    /* Bug fix: this return value used to be discarded, unlike
       tk_cre_tsk()'s just above. Since this starts the only task in
       the whole system, a failed tk_sta_tsk() with no check would have
       left the board silently parked forever at tk_slp_tsk() below,
       indistinguishable from a normally-running-but-quiet system - no
       diagnostic, no simulation activity, ever. */
    ER sta_ercd = tk_sta_tsk(sim_tskid, 0);
    if(sta_ercd < E_OK)
    {
        tm_putstring((UB*)"tron: tk_sta_tsk FAILED\n");
        return 1;
    }

    /* usermain's own task stays parked forever; sim_task does the
     * actual work. Per BSP (v1) manual: if usermain returns, µT-Kernel
     * shuts down, which would tear down sim_task along with it. */
    tk_slp_tsk(TMO_FEVR);

    return 0;
}
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

#include "sim_runner.h"
#include "fault_config.h"
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
#ifndef BOARD_TICKS_PER_SEED
#define BOARD_TICKS_PER_SEED 2000u
#endif
#ifndef BOARD_FAULT_PROFILE
#define BOARD_FAULT_PROFILE FAULT_PROFILE_NONE
#endif

/* Tick pacing via tk_dly_tsk, the µT-Kernel equivalent of the
 * laptop's nanosleep-based --tick-delay-ms. 0 = run flat out, which
 * is almost certainly what you want for real bring-up (the sim
 * itself is not real-time-paced; only the laptop's --live dashboard
 * mode paces ticks, for human-watchable playback). */
#ifndef BOARD_TICK_DELAY_MS
#define BOARD_TICK_DELAY_MS 0u
#endif

/* Cadence for draining the snapshot ring (board/Raspberry_Pi_Pico/snapshot.c)
 * out over USART1. Every tick is too eager: at BOARD_TICK_DELAY_MS=0
 * (flat-out, the expected bring-up setting) that would call into
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

    while (!mission_link_handshake_round_trip_complete())
    {
        /* Pumps RX on whichever transport this board has, the same
           function the per-tick loop below also calls — needed here
           too, since the request that completes this board's own
           readiness has to actually be received to unblock this
           wait. Not gated behind the readiness check itself (that
           would deadlock — this call is what MAKES the check
           eventually true). */
        mission_link_task_step();
    }

    tm_putstring((UB*)"tron: handshake ready, starting seed loop\n");

    for(uint32_t seed = BOARD_SEED; seed < BOARD_SEED + BOARD_SEED_COUNT; ++seed)
    {
        tm_printf((UB*)"tron: seed %lu start\n", (unsigned long)seed);

        /* sim_start_seed_board(), NOT sim_start_seed() — the board
         * variant, which does not self-generate a world. See
         * core/sim_runner.h's doc on both functions if this ever
         * looks like a typo; it is not one. */
        sim_start_seed_board(seed, BOARD_FAULT_PROFILE);

        for(uint32_t tick = 0; tick < BOARD_TICKS_PER_SEED; ++tick)
        {
            sim_tick();

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

            if((tick % BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL) == 0u)
            {
                (void)board_snapshot_ring_drain();
            }

#if BOARD_TICK_DELAY_MS > 0
            tk_dly_tsk(BOARD_TICK_DELAY_MS);
#endif
        }

        /* Final drain: flush whatever the last partial interval left
         * pending so a run's last few events aren't stuck in the
         * ring until the next seed's ticks happen to drain them (or
         * lost if this was the last seed). One call is enough as
         * long as BOARD_SNAPSHOT_DRAIN_MAX_FRAMES_PER_CALL >=
         * BOARD_SNAPSHOT_DRAIN_TICK_INTERVAL's worth of typical
         * per-tick snapshot_record() volume; if that assumption ever
         * stops holding, this would need a loop instead of one call. */
        (void)board_snapshot_ring_drain();

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

    tk_sta_tsk(sim_tskid, 0);

    /* usermain's own task stays parked forever; sim_task does the
     * actual work. Per BSP (v1) manual: if usermain returns, µT-Kernel
     * shuts down, which would tear down sim_task along with it. */
    tk_slp_tsk(TMO_FEVR);

    return 0;
}
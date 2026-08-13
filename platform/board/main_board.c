/*
 * Board entry point (STM32N6570-DK, µT-Kernel 3.0 BSP2).
 *
 * This is the on-device analogue of platform/laptop/main.c: it drives
 * the same core/sim_runner.h lifecycle (sim_runner_init ->
 * sim_start_seed -> sim_tick* -> sim_end_seed -> sim_runner_shutdown),
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
 *   3. Adds this project's core/ and platform/board/ sources, plus
 *      vendor/mtk3_bsp2's include/config/mtkernel paths, to the
 *      project's build settings.
 * That project-scaffolding step is IDE work, not something any C
 * source file can do — tracked as the next concrete step once you're
 * ready to actually flash the board, not part of this pass.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "sim_runner.h"
#include "fault_config.h"

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

LOCAL void sim_task(INT stacd, void *exinf);
LOCAL ID sim_tskid;
LOCAL T_CTSK sim_ctsk = {
    .itskpri = 10,
    .stksz   = 8192,          /* core/ uses static globals, not stack,
                                * for all sim state (grid, teams,
                                * incidents) -- this only needs to
                                * cover call-depth locals (A* search
                                * frames, CBBA bundle evaluation),
                                * not sim data. 8KB is a starting
                                * point; check high-water mark once
                                * you can actually run this and tune
                                * from there rather than guessing
                                * further. */
    .task    = sim_task,
    .tskatr  = TA_HLNG | TA_RNG3,   /* TA_FPU not set explicitly:
                                      * ALWAYS_FPU_ATR=1 is the BSP2
                                      * default (config/config.h),
                                      * which forces FPU context on
                                      * every task regardless of this
                                      * flag. Left unset rather than
                                      * OR'd in so this doesn't
                                      * silently break if someone
                                      * later flips ALWAYS_FPU_ATR
                                      * off without noticing this
                                      * file depends on it. */
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

    for(uint32_t seed = BOARD_SEED; seed < BOARD_SEED + BOARD_SEED_COUNT; ++seed)
    {
        tm_printf((UB*)"tron: seed %lu start\n", (unsigned long)seed);

        sim_start_seed(seed, BOARD_FAULT_PROFILE);

        for(uint32_t tick = 0; tick < BOARD_TICKS_PER_SEED; ++tick)
        {
            sim_tick();

#if BOARD_TICK_DELAY_MS > 0
            tk_dly_tsk(BOARD_TICK_DELAY_MS);
#endif
        }

        sim_end_seed();
        tm_printf((UB*)"tron: seed %lu done\n", (unsigned long)seed);
    }

    sim_runner_shutdown();
    tm_putstring((UB*)"tron: run complete\n");

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
    tm_putstring((UB*)"tron: starting simulation task\n");

    sim_tskid = tk_cre_tsk(&sim_ctsk);
    if(sim_tskid <= 0)
    {
        tm_putstring((UB*)"tron: tk_cre_tsk FAILED\n");
        return 1;
    }

    tk_sta_tsk(sim_tskid, 0);

    /* usermain's own task stays parked forever; sim_task does the
     * actual work. Per BSP2 manual: if usermain returns, µT-Kernel
     * shuts down, which would tear down sim_task along with it. */
    tk_slp_tsk(TMO_FEVR);

    return 0;
}

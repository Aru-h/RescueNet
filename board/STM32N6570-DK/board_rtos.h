/*
 * Kernel objects and helper tasks of the board firmware (see
 * docs/RTOS_DESIGN.md once it exists, and board_rtos_cfg.h for priorities).
 *
 * Everything here is created once from usermain() by board_rtos_create(),
 * before the link and sim tasks start, so no task ever sees a missing object.
 */
#ifndef BOARD_RTOS_H
#define BOARD_RTOS_H

#include <tk/tkernel.h>
#include <stdint.h>

#include "board_rtos_cfg.h"

/* ---- event flag bits ------------------------------------------------- */

/* g_flg_link: what wakes the link task. */
#define FLG_LINK_RX        (1u << 0) /* USART1 RX ISR put bytes in the ring */
#define FLG_LINK_POLL      (1u << 1) /* cyclic handler: poll the radio, check the heartbeat */
#define FLG_LINK_POST_TICK (1u << 2) /* sim finished a tick: run the between-ticks pass */
#define FLG_LINK_GO        (1u << 3) /* sim finished boot init: comms are ready */

/* g_flg_sim: what the sim compute task waits for. */
#define FLG_SIM_TICK_AUTH    (1u << 0) /* link: the laptop authorised a tick */
#define FLG_SIM_WORLD_READY  (1u << 1) /* link: handshake done, roster/map/sites received */
#define FLG_SIM_LINK_STEPPED (1u << 2) /* link: the between-ticks pass is done */

/* g_flg_ai (Phase 2). */
#define FLG_AI_GO   (1u << 0)
#define FLG_AI_DONE (1u << 8) /* worker w sets bit 8+w when its team range is done */

/* g_flg_mon: what the monitor task waits for. */
#define FLG_MON_TICK (1u << 0) /* 1 s cyclic handler */
#define FLG_MON_WDOG (1u << 1) /* sim watchdog alarm fired: one tick ran too long */

extern ID g_flg_link;
extern ID g_flg_sim;
extern ID g_flg_ai;
extern ID g_flg_mon;
extern ID g_mtx_comms;  /* radio, comms_transport, mission-link and simulation state */
extern ID g_mbf_telemetry;
extern ID g_cyc_poll;   /* 10 ms: sets FLG_LINK_POLL */
extern ID g_cyc_mon;    /* 1 s: sets FLG_MON_TICK */
extern ID g_alm_sim_wdog;

/* Number of AI worker tasks (Phase 2 fills them with work; the skeleton just
   creates them). */
#ifndef BOARD_AI_WORKERS
#define BOARD_AI_WORKERS 2
#endif

/* Creates every kernel object and starts the helper tasks (AI workers,
   telemetry, monitor). Returns 0, or a negative step number on failure. */
int board_rtos_create(void);

/* Starts the 10 ms link poll cyclic handler and the 1 s monitor handler. */
int board_rtos_start_cyclics(void);

/* Creates and starts one task on a caller-supplied static stack. Returns the
   task id, or a negative error. */
ID board_rtos_spawn(FP entry, PRI pri, void *stack, SZ stack_bytes, INT stacd);

/* Sim watchdog: arm at the start of a tick, disarm at its end. */
void board_wdog_arm(uint32_t timeout_ms);
void board_wdog_disarm(void);

#endif

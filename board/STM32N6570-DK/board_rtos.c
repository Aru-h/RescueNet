#include "board_rtos.h"

#include <string.h>

#include "board_log.h"

/* ---- budget against the BSP's configured object limits -----------------
   (board/STM32N6570-DK/vendor/mtk3_bsp2/config/config.h: 32 tasks, 16 event
   flags, 8 message buffers, 4 mutexes, 4 cyclic handlers, 8 alarms.) The
   counts below are what this firmware creates; the asserts keep a later
   addition from silently exceeding what the kernel was built with. */
#define BOARD_NUM_TASKS  (1 /* logger */ + 1 /* link */ + 1 /* sim */ + BOARD_AI_WORKERS + 1 /* telemetry */ + 1 /* monitor */)
#define BOARD_NUM_FLAGS  4
#define BOARD_NUM_MBFS   2 /* logger, telemetry */
#define BOARD_NUM_MTXS   2 /* uart, comms */
#define BOARD_NUM_CYCS   2
#define BOARD_NUM_ALMS   1

#ifdef CNF_MAX_TSKID
_Static_assert(BOARD_NUM_TASKS + 4 <= CNF_MAX_TSKID, "task budget (leaves 4 for the kernel/usermain)");
_Static_assert(BOARD_NUM_FLAGS <= CNF_MAX_FLGID, "event flag budget");
_Static_assert(BOARD_NUM_MBFS <= CNF_MAX_MBFID, "message buffer budget");
_Static_assert(BOARD_NUM_MTXS <= CNF_MAX_MTXID, "mutex budget");
_Static_assert(BOARD_NUM_CYCS <= CNF_MAX_CYCID, "cyclic handler budget");
_Static_assert(BOARD_NUM_ALMS <= CNF_MAX_ALMID, "alarm handler budget");
#endif

_Static_assert(BOARD_PRI_LINK < BOARD_PRI_SIM && BOARD_PRI_SIM < BOARD_PRI_AI &&
               BOARD_PRI_AI < BOARD_PRI_TELEMETRY && BOARD_PRI_TELEMETRY < BOARD_PRI_MONITOR &&
               BOARD_PRI_MONITOR < BOARD_PRI_LOGGER,
               "priority order: link > sim > ai > telemetry > monitor > logger");
_Static_assert(BOARD_AI_WORKERS >= 1 && BOARD_AI_WORKERS <= 8, "FLG_AI_DONE uses bits 8..15");

ID g_flg_link;
ID g_flg_sim;
ID g_flg_ai;
ID g_flg_mon;
ID g_mtx_comms;
ID g_mbf_telemetry;
ID g_cyc_poll;
ID g_cyc_mon;
ID g_alm_sim_wdog;

#define BOARD_TELEMETRY_MBF_BYTES 1024
#define BOARD_TELEMETRY_MSG_MAX   64

static uint8_t g_ai_stack[BOARD_AI_WORKERS][BOARD_AI_STACK_BYTES] __attribute__((aligned(8)));
static uint8_t g_telemetry_stack[BOARD_TELEMETRY_STACK_BYTES] __attribute__((aligned(8)));
static uint8_t g_monitor_stack[BOARD_MONITOR_STACK_BYTES] __attribute__((aligned(8)));
static uint8_t g_telemetry_mbf_buf[BOARD_TELEMETRY_MBF_BYTES] __attribute__((aligned(8)));

ID board_rtos_spawn(FP entry, PRI pri, void *stack, SZ stack_bytes, INT stacd)
{
    T_CTSK ctsk;
    ID tid;
    ER er;

    memset(&ctsk, 0, sizeof(ctsk));
    ctsk.itskpri = pri;
    ctsk.stksz = stack_bytes;
    ctsk.task = entry;
    ctsk.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF; /* TA_FPU comes from ALWAYS_FPU_ATR=1 */
    ctsk.bufptr = stack;

    tid = tk_cre_tsk(&ctsk);
    if (tid <= 0)
    {
        return tid < 0 ? tid : -1;
    }
    er = tk_sta_tsk(tid, stacd);
    if (er < E_OK)
    {
        return er;
    }
    return tid;
}

/* ---- handlers (task-independent context: non-waiting calls only) ------- */

static void board_cyc_poll_handler(void *exinf)
{
    (void)exinf;
    (void)tk_set_flg(g_flg_link, FLG_LINK_POLL);
}

static void board_cyc_mon_handler(void *exinf)
{
    (void)exinf;
    (void)tk_set_flg(g_flg_mon, FLG_MON_TICK);
}

static void board_wdog_handler(void *exinf)
{
    (void)exinf;
    (void)tk_set_flg(g_flg_mon, FLG_MON_WDOG);
}

void board_wdog_arm(uint32_t timeout_ms)
{
    if (g_alm_sim_wdog > 0)
    {
        (void)tk_sta_alm(g_alm_sim_wdog, (RELTIM)timeout_ms);
    }
}

void board_wdog_disarm(void)
{
    if (g_alm_sim_wdog > 0)
    {
        (void)tk_stp_alm(g_alm_sim_wdog);
    }
}

/* ---- helper tasks (skeleton: they exist, announce themselves, and wait) -- */

static void board_ai_worker_task(INT stacd, void *exinf)
{
    UINT ptn;

    (void)exinf;
    BOARD_LOG((UB*)"tron: ai worker %d up (pri %d)\n", (int)stacd, BOARD_PRI_AI);

    for (;;)
    {
        /* Phase 2 puts the inference here; until then there is nothing to do. */
        (void)tk_wai_flg(g_flg_ai, FLG_AI_GO, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR);
    }
}

static void board_telemetry_task(INT stacd, void *exinf)
{
    uint8_t msg[BOARD_TELEMETRY_MSG_MAX];

    (void)stacd;
    (void)exinf;
    BOARD_LOG((UB*)"tron: telemetry task up (pri %d)\n", BOARD_PRI_TELEMETRY);

    for (;;)
    {
        /* Phase 3 formats and sends telemetry here; until then nothing is sent. */
        (void)tk_rcv_mbf(g_mbf_telemetry, msg, TMO_FEVR);
    }
}

static void board_monitor_task(INT stacd, void *exinf)
{
    UINT ptn;

    (void)stacd;
    (void)exinf;
    BOARD_LOG((UB*)"tron: monitor task up (pri %d)\n", BOARD_PRI_MONITOR);

    for (;;)
    {
        if (tk_wai_flg(g_flg_mon, FLG_MON_TICK | FLG_MON_WDOG, TWF_ORW | TWF_BITCLR, &ptn, TMO_FEVR) == E_OK)
        {
            if (ptn & FLG_MON_WDOG)
            {
                BOARD_LOG((UB*)"tron: WATCHDOG sim tick exceeded its time budget\n");
            }
            /* Phase 3: stack high-water marks, timings, RX ring depth. */
        }
    }
}

/* ---- creation --------------------------------------------------------- */

static ID board_create_flag(UINT initial)
{
    T_CFLG cflg;

    memset(&cflg, 0, sizeof(cflg));
    cflg.flgatr = TA_TFIFO | TA_WMUL;
    cflg.iflgptn = initial;
    return tk_cre_flg(&cflg);
}

int board_rtos_create(void)
{
    T_CMTX cmtx;
    T_CMBF cmbf;
    T_CCYC ccyc;
    T_CALM calm;
    int w;

    g_flg_link = board_create_flag(0u);
    g_flg_sim = board_create_flag(0u);
    g_flg_ai = board_create_flag(0u);
    g_flg_mon = board_create_flag(0u);
    if (g_flg_link <= 0 || g_flg_sim <= 0 || g_flg_ai <= 0 || g_flg_mon <= 0)
    {
        return -1;
    }

    memset(&cmtx, 0, sizeof(cmtx));
    cmtx.mtxatr = TA_INHERIT;
    g_mtx_comms = tk_cre_mtx(&cmtx);
    if (g_mtx_comms <= 0)
    {
        return -2;
    }

    memset(&cmbf, 0, sizeof(cmbf));
    cmbf.mbfatr = TA_TFIFO | TA_USERBUF;
    cmbf.bufsz = BOARD_TELEMETRY_MBF_BYTES;
    cmbf.maxmsz = BOARD_TELEMETRY_MSG_MAX;
    cmbf.bufptr = g_telemetry_mbf_buf;
    g_mbf_telemetry = tk_cre_mbf(&cmbf);
    if (g_mbf_telemetry <= 0)
    {
        return -3;
    }

    memset(&ccyc, 0, sizeof(ccyc));
    ccyc.cycatr = TA_HLNG; /* not started: board_rtos_start_cyclics() */
    ccyc.cychdr = (FP)board_cyc_poll_handler;
    ccyc.cyctim = 10u;
    g_cyc_poll = tk_cre_cyc(&ccyc);
    ccyc.cychdr = (FP)board_cyc_mon_handler;
    ccyc.cyctim = 1000u;
    g_cyc_mon = tk_cre_cyc(&ccyc);
    if (g_cyc_poll <= 0 || g_cyc_mon <= 0)
    {
        return -4;
    }

    memset(&calm, 0, sizeof(calm));
    calm.almatr = TA_HLNG;
    calm.almhdr = (FP)board_wdog_handler;
    g_alm_sim_wdog = tk_cre_alm(&calm);
    if (g_alm_sim_wdog <= 0)
    {
        return -5;
    }

    for (w = 0; w < BOARD_AI_WORKERS; w++)
    {
        if (board_rtos_spawn((FP)board_ai_worker_task, BOARD_PRI_AI, g_ai_stack[w],
                             sizeof(g_ai_stack[w]), w) <= 0)
        {
            return -6;
        }
    }
    if (board_rtos_spawn((FP)board_telemetry_task, BOARD_PRI_TELEMETRY, g_telemetry_stack,
                         sizeof(g_telemetry_stack), 0) <= 0)
    {
        return -7;
    }
    if (board_rtos_spawn((FP)board_monitor_task, BOARD_PRI_MONITOR, g_monitor_stack,
                         sizeof(g_monitor_stack), 0) <= 0)
    {
        return -8;
    }

    return 0;
}

int board_rtos_start_cyclics(void)
{
    if (tk_sta_cyc(g_cyc_poll) < E_OK || tk_sta_cyc(g_cyc_mon) < E_OK)
    {
        return -1;
    }
    return 0;
}

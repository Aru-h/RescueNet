#include "board_log.h"

#include <string.h>

#include "board_rtos_cfg.h"

#define BOARD_LOG_MBF_BYTES 4096

static ID g_log_mbf;
static ID g_uart_mtx;
static volatile uint32_t g_log_dropped;
static volatile uint8_t g_log_running; /* set by the logger task once it loops */

static uint8_t g_log_stack[BOARD_LOGGER_STACK_BYTES] __attribute__((aligned(8)));
static uint8_t g_log_mbf_buf[BOARD_LOG_MBF_BYTES] __attribute__((aligned(8)));

void board_uart_tx_lock(void)
{
    if (g_uart_mtx > 0)
    {
        (void)tk_loc_mtx(g_uart_mtx, TMO_FEVR);
    }
}

void board_uart_tx_unlock(void)
{
    if (g_uart_mtx > 0)
    {
        (void)tk_unl_mtx(g_uart_mtx);
    }
}

uint32_t board_log_dropped(void)
{
    return g_log_dropped;
}

/* tm_putchar() masks interrupts for ONE character (and adds the CR before LF
   that tm_putstring adds); the string is not wrapped in an outer mask. */
static void board_log_write(const UB *text, INT len)
{
    INT i;

    for (i = 0; i < len; i++)
    {
        (void)tm_putchar((INT)text[i]);
    }
}

void board_log_emit(const UB *text, INT len)
{
    if (len <= 0)
    {
        return;
    }
    if (len > BOARD_LOG_LINE_MAX)
    {
        len = BOARD_LOG_LINE_MAX;
    }

    if (!g_log_running)
    {
        /* Early boot (or the logger could not start): direct, as before. */
        board_log_write(text, len);
        return;
    }

    if (tk_snd_mbf(g_log_mbf, text, len, TMO_POL) != E_OK)
    {
        UINT imask;

        DI(imask);
        g_log_dropped++;
        EI(imask);
    }
}

void board_log_puts(const char *text)
{
    board_log_emit((const UB *)text, (INT)strlen(text));
}

static void board_logger_task(INT stacd, void *exinf)
{
    UB line[BOARD_LOG_LINE_MAX];
    uint32_t reported_drops = 0u;

    (void)stacd;
    (void)exinf;

    g_log_running = 1u;

    for (;;)
    {
        INT n = tk_rcv_mbf(g_log_mbf, line, TMO_FEVR);

        if (n > 0)
        {
            board_uart_tx_lock();
            board_log_write(line, n);
            board_uart_tx_unlock();
        }

        if (g_log_dropped != reported_drops)
        {
            UB note[48];
            INT len;

            reported_drops = g_log_dropped;
            len = tm_sprintf(note, (const UB *)"log: %lu lines dropped\n", (unsigned long)reported_drops);
            board_uart_tx_lock();
            board_log_write(note, len);
            board_uart_tx_unlock();
        }
    }
}

int board_log_start(void)
{
    T_CMTX cmtx;
    T_CMBF cmbf;
    T_CTSK ctsk;
    ID tid;

    memset(&cmtx, 0, sizeof(cmtx));
    cmtx.mtxatr = TA_INHERIT;
    g_uart_mtx = tk_cre_mtx(&cmtx);
    if (g_uart_mtx <= 0)
    {
        return -1;
    }

    memset(&cmbf, 0, sizeof(cmbf));
    cmbf.mbfatr = TA_TFIFO | TA_USERBUF;
    cmbf.bufsz = BOARD_LOG_MBF_BYTES;
    cmbf.maxmsz = BOARD_LOG_LINE_MAX;
    cmbf.bufptr = g_log_mbf_buf;
    g_log_mbf = tk_cre_mbf(&cmbf);
    if (g_log_mbf <= 0)
    {
        return -2;
    }

    memset(&ctsk, 0, sizeof(ctsk));
    ctsk.itskpri = BOARD_PRI_LOGGER;
    ctsk.stksz = sizeof(g_log_stack);
    ctsk.task = board_logger_task;
    ctsk.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
    ctsk.bufptr = g_log_stack;
    tid = tk_cre_tsk(&ctsk);
    if (tid <= 0)
    {
        return -3;
    }
    if (tk_sta_tsk(tid, 0) < E_OK)
    {
        return -4;
    }

    return 0;
}

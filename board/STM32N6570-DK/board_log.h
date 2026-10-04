/*
 * board_log - console output through one low-priority logger task.
 *
 * Why: the BSP's tm_putstring() keeps interrupts masked for the WHOLE string
 * (about 87 us per character at 115200 baud, ~5 ms for a 60-character line,
 * ~12 ms for a hash line), which is long enough to overflow USART1's 16-byte
 * RX FIFO and to delay every task. Here producers only format the line into a
 * small stack buffer and send it to a message buffer with TMO_POL (never
 * waits), and the logger task (lowest priority) writes it out with tm_putchar(),
 * which masks interrupts for one character at a time, so the longest mask is
 * one character time (~87 us) and no time-critical task ever waits on the UART.
 *
 * If the buffer is full the message is dropped and counted (a dropped line is
 * reported by the logger itself as "log: N lines dropped"). Before the logger
 * runs (early boot) output goes straight to the UART, as before.
 *
 * The same UART also carries binary frames to the laptop, so frame writers
 * take the UART lock (board_uart_tx_lock) for a whole frame: a frame is never
 * split by another task's text or by another frame.
 *
 * Formatting uses the BSP's tm_sprintf (same conversions as tm_printf: no
 * floating point, no 64-bit). A line must fit BOARD_LOG_LINE_MAX bytes: tm_sprintf
 * has no length limit, so keep %s arguments short.
 */
#ifndef BOARD_LOG_H
#define BOARD_LOG_H

#include <tk/tkernel.h>
#include <tm/tmonitor.h>
#include <stdint.h>

#define BOARD_LOG_LINE_MAX 256

/* Creates the UART mutex, the message buffer and the logger task and starts
   it. Call once from usermain, after tm_com_init(). Returns 0 on success. */
int board_log_start(void);

/* Queues one already-formatted line (not necessarily NUL-terminated) of len
   bytes. Never blocks. */
void board_log_emit(const UB *text, INT len);

/* Queues a NUL-terminated string. */
void board_log_puts(const char *text);

/* printf-style: the call sites keep the (UB*)"..." form tm_printf used. The
   line buffer lives on the caller's stack: BOARD_LOG() uses BOARD_LOG_LINE_MAX
   bytes, BOARD_LOG_N(n, ...) a smaller buffer for a task with a small stack.
   usermain() runs on the BSP's 1 KB init task stack (INITTASK_STKSZ), so it
   must use BOARD_LOG_N(96, ...) or board_log_puts(), never BOARD_LOG(). */
#define BOARD_LOG_N(n, ...)                                         \
    do {                                                            \
        UB board_log_line_[(n)];                                    \
        INT board_log_len_ = tm_sprintf(board_log_line_, __VA_ARGS__); \
        board_log_emit(board_log_line_, board_log_len_);            \
    } while (0)

#define BOARD_LOG(...) BOARD_LOG_N(BOARD_LOG_LINE_MAX, __VA_ARGS__)

/* Lines dropped because the buffer was full, since boot. */
uint32_t board_log_dropped(void);

/* Binary-frame writers: hold this across one whole frame. No-ops until the
   logger (which creates the mutex) has started. Task context only. */
void board_uart_tx_lock(void);
void board_uart_tx_unlock(void);

#endif

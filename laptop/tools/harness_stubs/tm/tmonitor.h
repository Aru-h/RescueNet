/*
 * Minimal stand-in for the real µT-Kernel tm/tmonitor.h, for
 * two_board_harness.c's desktop build ONLY (-Ilaptop/tools/harness_
 * stubs, ordered before either real BSP's own -I path so this is
 * picked up instead). board/STM32N6570-DK/mission_link_task.c has
 * exactly one real dependency on this header - tm_putchar(), inside
 * mission_link_usart1_write() - everything else in that file is
 * portable. Rather than pull in a full real BSP (which would also
 * need this file linked against a real target, defeating the point
 * of a desktop-only harness), this stub supplies just enough to link:
 * INT (the µT-Kernel integer typedef the real call casts through) and
 * a tm_putchar that writes to stderr, so USART1 TX bytes are at least
 * visible during a harness run instead of silently discarded.
 *
 * NOT a real BSP header - do not add this to check-board's or
 * check-pico's own include paths; it exists solely so
 * two_board_harness.c can link mission_link_task.c on a desktop gcc
 * build without needing either real vendored BSP.
 */
#ifndef HARNESS_STUB_TM_TMONITOR_H
#define HARNESS_STUB_TM_TMONITOR_H

#include <stdio.h>

typedef int INT;

/* Writes to a log file, not stderr - stderr is reserved for this
   harness's own human-readable [board0]/[board1]/[harness] status
   lines (see two_board_harness.c's own fprintf calls); mixing those
   with raw binary USART1 protocol bytes on the same stream produces
   garbled, invalid-UTF8 terminal output. The bytes themselves aren't
   needed to judge pass/fail (see two_board_harness.c's own assertion,
   which checks g_teams[] state directly, not USART1 traffic) - this
   file exists only so the write has somewhere real to go rather than
   being silently discarded, in case anyone wants to inspect it later. */
static inline INT tm_putchar(INT c)
{
    static FILE *log = NULL;
    if (log == NULL)
    {
        log = fopen("/tmp/two_board_harness_usart1.log", "ab");
    }
    if (log != NULL)
    {
        (void)fputc(c, log);
        (void)fflush(log);
    }
    return c;
}

#endif /* HARNESS_STUB_TM_TMONITOR_H */

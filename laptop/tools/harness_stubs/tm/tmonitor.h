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
 * a tm_putchar that hands each USART1 TX byte to the capture hook below.
 *
 * NOT a real BSP header - do not add this to check-board's or
 * check-pico's own include paths; it exists solely so
 * two_board_harness.c can link mission_link_task.c on a desktop gcc
 * build without needing either real vendored BSP.
 */
#ifndef HARNESS_STUB_TM_TMONITOR_H
#define HARNESS_STUB_TM_TMONITOR_H

typedef int INT;

/* USART1 TX bytes are not written anywhere by default: this used to append
   every byte to a log file in /tmp that no test read and nothing ever
   trimmed (it reached hundreds of MB across harness runs). Not stderr
   either - stderr carries the harness's own [board0]/[board1]/[harness]
   status lines, and raw binary frames there garble the terminal.
   A harness that needs to check what a board wrote to USART1
   (two_board_harness.c checks override acks) defines this hook strongly.
   A weak do-nothing definition rather than a weak declaration, because
   the macOS linker rejects an undefined weak reference. */
__attribute__((weak)) void harness_usart1_tx_byte(int c)
{
    (void)c;
}

static inline INT tm_putchar(INT c)
{
    harness_usart1_tx_byte(c);
    return c;
}

#endif /* HARNESS_STUB_TM_TMONITOR_H */

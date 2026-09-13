/*
 * Minimal stand-in for the real µT-Kernel tm/tmonitor.h, for laptop/
 * tools/mission_link_status_gate_test.c's desktop build ONLY - same
 * role as laptop/tools/harness_stubs/tm/tmonitor.h (two_board_
 * harness.c's own stub), but this one CAPTURES the raw USART1 TX
 * bytes into an inspectable in-memory buffer instead of writing them
 * to a log file, since this test needs to actually decode the real
 * wire frames mission_link_status_tx_step() (board/STM32N6570-DK/
 * mission_link_task.c) writes via tm_putchar(), to confirm which
 * team_ids' MissionLinkStatus_t frames were genuinely sent.
 *
 * Deliberately a SEPARATE stub directory from laptop/tools/harness_
 * stubs/ - that one is shared with two_board_harness.c (already
 * hardened/flakiness-tested per its own doc); this test does not
 * touch or depend on it.
 */
#ifndef MISSION_LINK_GATE_TEST_STUB_TM_TMONITOR_H
#define MISSION_LINK_GATE_TEST_STUB_TM_TMONITOR_H

typedef int INT;

#define MISSION_LINK_GATE_TEST_CAPTURE_MAX 4096
extern unsigned char g_mission_link_gate_test_captured_bytes[MISSION_LINK_GATE_TEST_CAPTURE_MAX];
extern int g_mission_link_gate_test_captured_len;

static inline INT tm_putchar(INT c)
{
    if (g_mission_link_gate_test_captured_len < MISSION_LINK_GATE_TEST_CAPTURE_MAX)
        g_mission_link_gate_test_captured_bytes[g_mission_link_gate_test_captured_len++] = (unsigned char)c;
    return c;
}

#endif /* MISSION_LINK_GATE_TEST_STUB_TM_TMONITOR_H */

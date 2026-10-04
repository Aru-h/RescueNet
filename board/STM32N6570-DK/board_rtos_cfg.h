/*
 * Task priorities, stack sizes and kernel-object budget of the board
 * firmware. One place, so the priority order can be read (and checked) at a
 * glance. μT-Kernel priorities: 1 is the highest, 32 (CNF_MAX_TSKPRI) the
 * lowest, and a LOWER number preempts a higher one.
 *
 *   link        4   serves the radio and the laptop link; never blocked by
 *                   anything slower than itself except the comms mutex
 *   sim compute 10  the simulation, the only writer of simulation state
 *   ai worker   11  (Phase 2) inference helpers, below the sim that waits
 *                   for them
 *   telemetry   12  (Phase 3)
 *   monitor     20  (Phase 3) stack/timing watchdog, runs only when idle
 *   logger      30  console output
 */
#ifndef BOARD_RTOS_CFG_H
#define BOARD_RTOS_CFG_H

#define BOARD_PRI_LINK       4
#define BOARD_PRI_SIM        10
#define BOARD_PRI_AI         11
#define BOARD_PRI_TELEMETRY  12
#define BOARD_PRI_MONITOR    20
#define BOARD_PRI_LOGGER     30

/* Static stacks (TA_USERBUF), so they show up in the linker map and can be
   painted and measured. sim keeps the 24 KB it already needed. */
#define BOARD_SIM_STACK_BYTES        24576
#define BOARD_LINK_STACK_BYTES       8192
#define BOARD_AI_STACK_BYTES         4096
#define BOARD_TELEMETRY_STACK_BYTES  4096
#define BOARD_MONITOR_STACK_BYTES    4096
#define BOARD_LOGGER_STACK_BYTES     3072

#endif

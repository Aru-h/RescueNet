/*
 * usart1_driver.c - real (non-weak) board_usart1_read_byte() for the
 * DK's laptop-facing USART1 link. See mission_link_task.c's own
 * weak-stub comment on board_usart1_read_byte() - this file is "the
 * UART driver's own .c file" that comment says would override it;
 * nothing in mission_link_task.c needs to change once this is linked
 * in, the linker resolves every call to this definition automatically.
 *
 * mtk3_bsp2's own USART1 path (vendor/mtk3_bsp2/sysdepend/stm32_cube/
 * lib/libtm/discovery_stm32n657/tm_com.c) already brings this UART up
 * automatically before usermain() ever runs - mtkernel's sysinit.c
 * calls libtm_init(), which calls tm_com_init(): 115200-8-N-1, both TX
 * and RX enabled at the register level (CR1 = UE|RE|TE). Its own RX
 * path (tm_getchar()/tm_rcv_dat()) is unusable here though - both
 * block forever waiting for a byte with interrupts globally disabled
 * (see libtm.c) - not something a per-tick poll that must never stall
 * the sim can call.
 *
 * Bug fix #1: originally used the plain `USART1` macro from the CMSIS
 * header. That macro resolves to the SECURE alias (USART1_S,
 * 0x52001000) only when CPU_IN_SECURE_STATE is defined at compile
 * time, and the header defines that only under -mcmse
 * (__ARM_FEATURE_CMSE == 3). When this bug was found the build did not
 * pass -mcmse (today both the Makefile and the CubeIDE .cproject do), so
 * it silently resolved to the NON-secure alias (USART1_NS, 0x42001000)
 * instead; the explicit _S below keeps it correct whatever the flags. tm_com_init() (and every other
 * read/write this board does of this peripheral) targets the SECURE
 * alias directly, unconditionally, because TRUSTZONE_SECURE=1 for
 * this target (see vendor/mtk3_bsp2's own sysdef.h) - it doesn't key
 * off CPU_IN_SECURE_STATE at all. Reading the non-secure alias meant
 * RXNE never appeared to be set (a different, inactive view of the
 * same physical UART), so this function silently returned -1 forever
 * no matter what arrived on the wire. Fixed by using USART1_S
 * explicitly, matching tm_com.c's own convention.
 *
 * Bug fix #2 (this revision): a first pass of this file polled RXNE
 * from board_usart1_read_byte() instead of using an interrupt - fine
 * in isolation, but demonstrated (with real hardware, real laptop
 * traffic) to lose bytes in practice: mission_link_handshake_tx_step()
 * (mission_link_task.c) retries a relay Send to the LoRa peer every
 * HANDSHAKE_NUCLEO_RETRY_INTERVAL_TICKS (500ms) via comms_transport_
 * send() -> sx1262_hal.c's SPI path, which - with no SX1262 module
 * actually wired up (as during this exact bring-up phase) - blocks on
 * wait_busy_low()'s BUSY-pin timeout for real, measured hundreds of
 * milliseconds per attempt. That's long enough that a multi-byte
 * frame arriving mid-block overruns: USART1 has a single-byte receive
 * buffer, no FIFO enabled, so every byte after the first in a frame
 * that arrives while nothing is polling gets silently discarded via
 * ORE before board_usart1_read_byte() next runs - confirmed via a
 * debug trace that showed exactly one magic byte received per
 * handshake attempt, forever, no matter how many times the laptop
 * retried.
 *
 * Deliberately NOT fixed by shrinking sx1262_hal.c's SPI/BUSY
 * timeouts - those are sized against real Tswmode datasheet timing
 * with a documented safety margin for when a real module IS attached
 * (Goal 2); shrinking them to paper over this is exactly the kind of
 * change that quietly breaks real hardware later. Fixed here instead,
 * at the actual point of loss: RXNE now drives a real NVIC interrupt
 * into a small software ring buffer, so a byte is captured within
 * interrupt latency of arriving regardless of how long anything else
 * (SPI busy-waits included) blocks the main loop.
 *
 * Bug fix #3 (RTOS work): the handler used to be a bare NVIC vector
 * (USART1_IRQHandler) because the BSP was thought not to expose tk_def_int()
 * for USART1. It does - tk_def_int() works for any interrupt number - so the
 * handler is now registered with the kernel (TA_HLNG, IRQ 159, level 15) and
 * may wake a task with tk_set_flg() (board_usart1_set_rx_notify()). It still
 * calls nothing but non-waiting kernel services and touches only this file's
 * own ring buffer and counters.
 */

#include <tk/tkernel.h>
#include "stm32n657xx.h"

/* Sized for the laptop's real traffic, not a single frame: the bridge
   sends up to 10 frames/s of <= 40 B at 115200 baud (~11.5 B/ms on the
   wire), and the sim task can legitimately go hundreds of ms between
   pumps (a CBBA round, an SX1262 BUSY-pin timeout - see Bug fix #2
   above). The old 64-byte ring overflowed after ~100-150 ms without a
   pump; 512 B covers ~45 ms of back-to-back line-rate bytes, or several
   seconds of the bridge's actual frame rate. Keep it a power of 2: the
   head/tail math below uses %, which then compiles to a mask. */
#define USART1_RX_RING_CAPACITY 512u
_Static_assert((USART1_RX_RING_CAPACITY & (USART1_RX_RING_CAPACITY - 1u)) == 0u,
               "USART1_RX_RING_CAPACITY must be a power of 2");

static volatile uint8_t g_rx_ring[USART1_RX_RING_CAPACITY];
static volatile uint32_t g_rx_ring_head; /* next slot the ISR writes */
static volatile uint32_t g_rx_ring_tail; /* next slot board_usart1_read_byte() reads */
/* Bytes the ISR received but had to drop because the ring was full.
   Written only by the ISR; a single aligned 32-bit word, so the
   getter's plain read is atomic on this core. */
static volatile uint32_t g_rx_ring_drop_count;
/* USART1 receive errors the ISR saw and cleared: one count per ORE
   (overrun - a byte was lost in hardware), FE (framing) or NE (noise)
   flag occurrence. Same single-writer/atomic-read reasoning as
   g_rx_ring_drop_count. */
static volatile uint32_t g_rx_hw_error_count;

/* Optional wake-up for a consumer task: when set (board_usart1_set_rx_notify),
   the ISR ORs `bit` into event flag `id` after it has put bytes in the ring.
   tk_set_flg() is one of the calls a kernel-managed interrupt handler may make
   (non-waiting, ISR-safe). The flag is created before the interrupt is enabled
   and the pair is written once at boot, so a plain read in the ISR is enough. */
/* Wake the consumer only once this many bytes are waiting: a flag set (and a
   priority-4 task switch) per received byte would cost thousands of switches a
   second at 115200 baud. The consumer also wakes every 10 ms on its own poll
   handler, so a short frame (a laptop frame is <= 40 bytes) is picked up
   within 10 ms, and a 512-byte ring never gets near full from this. */
#define BOARD_USART1_RX_WAKE_BYTES 32u

static volatile ID g_rx_notify_flg;
static volatile UINT g_rx_notify_bit;

void board_usart1_set_rx_notify(ID flgid, UINT bit)
{
    g_rx_notify_bit = bit;
    g_rx_notify_flg = flgid;
}

/* Kernel-managed handler: registered with tk_def_int() (TA_HLNG), so the
   BSP's knl_hll_inthdr() enters task-independent context around it and passes
   the interrupt number; no hand-patched vector. */
static void board_usart1_isr(UINT intno)
{
    uint32_t received = 0u;

    (void)intno;

    for (;;)
    {
        uint32_t isr = USART1_S->ISR;
        uint32_t clear = 0u;
        uint8_t byte;

        /* Count and clear every receive-error flag on each pass, BEFORE
           the RXNE check: RXNEIE also enables the ORE interrupt, so an
           ORE left set with RXNE already clear would re-enter this
           handler forever. FE/NE don't raise an interrupt here (EIE is
           off) but still latch, so they're picked up alongside the byte
           they belong to. That byte is still delivered to the ring -
           the mission-link frame checksum is what rejects a corrupted
           frame, as it already does for any other line noise. */
        if ((isr & USART_ISR_ORE) != 0u)
        {
            g_rx_hw_error_count++;
            clear |= USART_ICR_ORECF;
        }
        if ((isr & USART_ISR_FE) != 0u)
        {
            g_rx_hw_error_count++;
            clear |= USART_ICR_FECF;
        }
        if ((isr & USART_ISR_NE) != 0u)
        {
            g_rx_hw_error_count++;
            clear |= USART_ICR_NECF;
        }
        if (clear != 0u)
        {
            USART1_S->ICR = clear;
        }

        if ((isr & USART_ISR_RXNE) == 0u)
        {
            break;
        }

        byte = (uint8_t)(USART1_S->RDR & 0xFFu);

        /* Drop the byte if the ring is full rather than overwrite an
           unread one - same "never block, bounded loss under real
           overload" contract this file's read side already accepted
           for the hardware's own single-byte buffer, just moved one
           level up to a deeper (USART1_RX_RING_CAPACITY) buffer. The
           drop is counted, not silent: see
           board_usart1_rx_drop_count(). */
        if ((g_rx_ring_head - g_rx_ring_tail) < USART1_RX_RING_CAPACITY)
        {
            g_rx_ring[g_rx_ring_head % USART1_RX_RING_CAPACITY] = byte;
            g_rx_ring_head++;
            received++;
        }
        else
        {
            g_rx_ring_drop_count++;
        }
    }

    if (received != 0u && g_rx_notify_flg > 0 &&
        (g_rx_ring_head - g_rx_ring_tail) >= BOARD_USART1_RX_WAKE_BYTES)
    {
        (void)tk_set_flg(g_rx_notify_flg, g_rx_notify_bit);
    }
}

/* Interrupt number and priority. USART1_IRQn is 159 on the STM32N657; the
   BSP's level is 1 (highest allowed for an external interrupt, 0 is reserved
   for the kernel) to 15. 15 is the lowest and what this ISR ran at before it
   moved under the kernel: it never contends with kernel-managed interrupts
   for correctness, only for CPU time, and it is a handful of instructions per
   byte. */
#define BOARD_USART1_IRQ_NO    159u
#define BOARD_USART1_IRQ_LEVEL 15

_Static_assert((unsigned)USART1_IRQn == BOARD_USART1_IRQ_NO,
               "BOARD_USART1_IRQ_NO must match the CMSIS USART1_IRQn");

void board_usart1_rx_init(void)
{
    T_DINT dint;
    ER er;

    USART1_S->CR1 |= USART_CR1_RXNEIE;

    dint.intatr = TA_HLNG;
    dint.inthdr = (FP)board_usart1_isr;
    er = tk_def_int(BOARD_USART1_IRQ_NO, &dint);
    if (er < E_OK)
    {
        /* Nothing can be printed from here reliably before the logger exists;
           leave RX unarmed and let the missing-bytes symptom show it. */
        return;
    }

    EnableInt(BOARD_USART1_IRQ_NO, BOARD_USART1_IRQ_LEVEL);
}

int board_usart1_read_byte(void)
{
    uint8_t byte;

    if (g_rx_ring_head == g_rx_ring_tail)
    {
        return -1;
    }

    byte = g_rx_ring[g_rx_ring_tail % USART1_RX_RING_CAPACITY];
    g_rx_ring_tail++;

    return (int)byte;
}

/* Running total of received bytes dropped because the RX ring was full
   (the sim task went too long without calling board_usart1_read_byte()).
   Monotonic; wraps at 2^32. */
uint32_t board_usart1_rx_drop_count(void)
{
    return g_rx_ring_drop_count;
}

/* Running total of laptop bytes lost or damaged on USART1 receive:
   hardware errors (ORE/FE/NE, counted in board_usart1_isr()) plus
   ring-full drops (board_usart1_rx_drop_count()). Read by main_board.c's
   board_report_link_diagnostics(). Monotonic; wraps at 2^32. */
uint32_t board_usart1_rx_error_count(void)
{
    return g_rx_hw_error_count + g_rx_ring_drop_count;
}
